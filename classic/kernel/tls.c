/*
 * EmberOS HTTPS support: a thin layer over BearSSL.
 * The TCP connection comes from net.c (net_raw_read / net_raw_write).
 * Certificates are checked against the Mozilla root list in trust_anchors.h,
 * using the VM's real-time clock for the current date.
 */
#include "kernel.h"
#include "bearssl.h"
#include "trust_anchors.h"

struct tls_mem {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};
#define T ((struct tls_mem *)MEM_TLS)
_Static_assert(sizeof(struct tls_mem) < 0x40000, "TLS state must fit its memory slot");

static int tls_err;

/* ---- entropy: RDRAND when the CPU has it, plus timing jitter from the TSC ---- */
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }

static int has_rdrand(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (c >> 30) & 1;
}
static int rdrand32(uint32_t *v) {
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}

static void add_entropy(void) {
    static uint32_t pool[160];
    int n = 0;
    uint64_t prev = rdtsc();
    for (int i = 0; i < 128; i++) {                 /* jitter: time loops of data-dependent length */
        volatile uint32_t spin = (uint32_t)(prev & 0x3FF) + 64;
        while (spin--) {}
        uint64_t t = rdtsc();
        pool[n++] = (uint32_t)(t - prev) ^ (uint32_t)(t >> 7) ^ ticks;
        prev = t;
    }
    if (has_rdrand()) {
        for (int i = 0; i < 16; i++) { uint32_t v; for (int k = 0; k < 10 && !rdrand32(&v); k++) {} pool[n++] = v; }
    }
    pool[n++] = ticks;
    br_ssl_engine_inject_entropy(&T->sc.eng, pool, (size_t)n * 4);
}

/* ---- time: days since 1 Jan 0 AD and seconds into the day, from the RTC ---- */
static uint32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe + 60);       /* +60: 0000-03-01 -> 0000-01-01 (year 0 is leap) */
}

/* ---- transport callbacks ---- */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    int n = net_raw_read(buf, (uint32_t)len);
    return n > 0 ? n : -1;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    return net_raw_write(buf, (uint32_t)len);
}

/* ---- certificate recorder: wraps the X.509 checker and keeps a copy of what the site sent ---- */
#define REC_CERTS 6
#define REC_BYTES 2048                       /* enough to reach the issuer and subject names */
static struct { uint8_t data[REC_CERTS][REC_BYTES]; uint32_t len[REC_CERTS]; int n, cur; } rec;
static struct { const br_x509_class *vtable; } rec_ctx;
#define INNER (&T->xc.vtable)

static void rec_start_chain(const br_x509_class **ctx, const char *name) { (void)ctx; rec.n = 0; rec.cur = -1; (*INNER)->start_chain(INNER, name); }
static void rec_start_cert(const br_x509_class **ctx, uint32_t length) {
    (void)ctx;
    rec.cur = rec.n < REC_CERTS ? rec.n : -1;
    if (rec.cur >= 0) rec.len[rec.cur] = 0;
    (*INNER)->start_cert(INNER, length);
}
static void rec_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    if (rec.cur >= 0) {
        uint32_t room = REC_BYTES - rec.len[rec.cur];
        uint32_t n = len < room ? (uint32_t)len : room;
        memcpy(rec.data[rec.cur] + rec.len[rec.cur], buf, n);
        rec.len[rec.cur] += n;
    }
    (*INNER)->append(INNER, buf, len);
}
static void rec_end_cert(const br_x509_class **ctx) { (void)ctx; rec.n++; (*INNER)->end_cert(INNER); }
static unsigned rec_end_chain(const br_x509_class **ctx) { (void)ctx; return (*INNER)->end_chain(INNER); }
static const br_x509_pkey *rec_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    (void)ctx; return (*INNER)->get_pkey((const br_x509_class *const *)INNER, usages);
}
static const br_x509_class rec_vtable = {
    sizeof(rec_ctx), rec_start_chain, rec_start_cert, rec_append, rec_end_cert, rec_end_chain, rec_get_pkey
};

/* Minimal DER reading: header at b[i] -> tag, content start, content end. */
static int der(const uint8_t *b, uint32_t len, uint32_t i, uint8_t *tag, uint32_t *cs, uint32_t *ce) {
    if (i + 2 > len) return 0;
    *tag = b[i];
    uint32_t l = b[i + 1], h = 2;
    if (l & 0x80) {
        uint32_t nb = l & 0x7F;
        if (nb == 0 || nb > 3 || i + 2 + nb > len) return 0;
        l = 0;
        for (uint32_t k = 0; k < nb; k++) l = l << 8 | b[i + 2 + k];
        h = 2 + nb;
    }
    *cs = i + h; *ce = i + h + l;
    return 1;
}

/* Copies "CN" (or else "O") from a DER Name into out. */
static void name_of(const uint8_t *b, uint32_t len, uint32_t ns, uint32_t ne, char *out, int cap) {
    static const uint8_t CN[3] = { 0x55, 0x04, 0x03 }, O[3] = { 0x55, 0x04, 0x0A };
    char o[64] = "";
    out[0] = 0;
    uint8_t t; uint32_t s1, e1, s2, e2, s3, e3, s4, e4;
    for (uint32_t i = ns; i < ne && der(b, len, i, &t, &s1, &e1) && e1 <= len; i = e1) {          /* SET */
        for (uint32_t j = s1; j < e1 && der(b, len, j, &t, &s2, &e2) && e2 <= len; j = e2) {     /* SEQ */
            if (!der(b, len, s2, &t, &s3, &e3) || t != 0x06 || e3 > len) continue;              /* OID */
            if (!der(b, len, e3, &t, &s4, &e4) || e4 > len) continue;                           /* value */
            char *dst = 0; int dcap = 0;
            if (e3 - s3 == 3 && !memcmp(b + s3, CN, 3)) { dst = out; dcap = cap; }
            else if (e3 - s3 == 3 && !memcmp(b + s3, O, 3)) { dst = o; dcap = sizeof(o); }
            if (!dst) continue;
            int n = 0;
            for (uint32_t k = s4; k < e4 && n + 1 < dcap; k++) dst[n++] = (b[k] >= 32 && b[k] < 127) ? (char)b[k] : '?';
            dst[n] = 0;
        }
    }
    if (!out[0]) { int n = 0; for (; o[n] && n + 1 < cap; n++) out[n] = o[n]; out[n] = 0; }
    else if (o[0]) {                                   /* "CN (O)" when it fits */
        int n = (int)strlen(out), m = (int)strlen(o);
        if (n + m + 3 < cap) { out[n++] = ' '; out[n++] = '('; memcpy(out + n, o, (size_t)m); n += m; out[n++] = ')'; out[n] = 0; }
    }
}

/* Prints who each certificate the site sent is for, and who issued it. */
void tls_print_chain(void) {
    if (!rec.n) return;
    puts("Certificates the site sent:\n");
    for (int c = 0; c < rec.n && c < REC_CERTS; c++) {
        const uint8_t *b = rec.data[c]; uint32_t len = rec.len[c];
        uint8_t t; uint32_t cs, ce, ts, te, i;
        char subj[72], iss[72];
        subj[0] = iss[0] = 0;
        if (der(b, len, 0, &t, &cs, &ce) && der(b, len, cs, &t, &ts, &te)) {
            uint32_t f[8]; int nf = 0;
            for (i = ts; nf < 8 && der(b, len, i, &t, &cs, &ce) && ce <= len; i = ce) f[nf++] = i;
            int k = (nf && b[f[0]] == 0xA0) ? 1 : 0;          /* skip [0] version */
            if (nf >= k + 5) {                                 /* serial, sigalg, issuer, validity, subject */
                der(b, len, f[k + 2], &t, &cs, &ce); name_of(b, len, cs, ce, iss, sizeof(iss));
                der(b, len, f[k + 4], &t, &cs, &ce); name_of(b, len, cs, ce, subj, sizeof(subj));
            }
        }
        set_color(0x0F); kprintf("  %u. %s\n", (uint32_t)(c + 1), subj[0] ? subj : "(unreadable)"); reset_color();
        kprintf("     issued by %s\n", iss[0] ? iss : "(unreadable)");
    }
    if (rec.n >= REC_CERTS) puts("  ...\n");
}

int tls_begin(const char *host) {
    tls_err = 0;
    br_ssl_client_init_full(&T->sc, &T->xc, TAs, TAs_NUM);
    rec_ctx.vtable = &rec_vtable;
    br_ssl_engine_set_x509(&T->sc.eng, &rec_ctx.vtable);
    rec.n = 0;
    br_ssl_engine_set_buffer(&T->sc.eng, T->iobuf, sizeof(T->iobuf), 1);
    struct rtc_time_s t; rtc_now(&t);
    br_x509_minimal_set_time(&T->xc, days_from_civil(t.year, t.mon, t.day),
                             (uint32_t)(t.hour * 3600 + t.min * 60 + t.sec));
    add_entropy();
    if (!br_ssl_client_reset(&T->sc, host, 0)) { tls_err = br_ssl_engine_last_error(&T->sc.eng); return 0; }
    br_sslio_init(&T->ioc, &T->sc.eng, sock_read, 0, sock_write, 0);
    return 1;
}

/* Sends data (the handshake happens on the first send). */
int tls_send(const void *data, uint32_t len) {
    if (br_sslio_write_all(&T->ioc, data, len) < 0 || br_sslio_flush(&T->ioc) < 0) {
        tls_err = br_ssl_engine_last_error(&T->sc.eng);
        return 0;
    }
    return 1;
}

/* Returns bytes read, 0 at end of stream, -1 on error (see tls_error). */
int tls_recv(void *buf, uint32_t len) {
    int n = br_sslio_read(&T->ioc, buf, len);
    if (n > 0) return n;
    tls_err = br_ssl_engine_last_error(&T->sc.eng);
    if (tls_err == BR_ERR_OK) return 0;                /* clean close_notify */
    return -1;
}

void tls_end(void) {
    if (br_ssl_engine_current_state(&T->sc.eng) != BR_SSL_CLOSED) br_ssl_engine_close(&T->sc.eng);
}

int tls_error(void) { return tls_err; }
int tls_error_is_transport(void) { return tls_err == BR_ERR_IO; }

const char *tls_error_text(int e) {
    switch (e) {
    case BR_ERR_X509_EXPIRED:
        return "The site's certificate has expired or isn't valid yet. If other sites work,\n"
               "the site is at fault; if none do, check the VM's clock (the 'time' command).";
    case BR_ERR_X509_BAD_SERVER_NAME:   return "The site's certificate is for a different name. Not safe to continue.";
    case BR_ERR_X509_NOT_TRUSTED:       return "The site's certificate isn't from an authority EmberOS trusts.";
    case BR_ERR_X509_BAD_SIGNATURE:     return "The site's certificate has a bad signature. Not safe to continue.";
    case BR_ERR_X509_UNSUPPORTED:
    case BR_ERR_X509_WEAK_PUBLIC_KEY:   return "The site's certificate uses a key type EmberOS doesn't support.";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:    return "The site doesn't support TLS 1.2 (EmberOS doesn't have TLS 1.3).";
    case BR_ERR_NO_RANDOM:              return "No random numbers available for encryption.";
    case BR_ERR_BAD_PARAM:              return "Bad site name for HTTPS.";
    }
    if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT) {
        int a = e - BR_ERR_RECV_FATAL_ALERT;
        if (a == 40 || a == 70 || a == 71)
            return "The site refused the secure connection - it probably needs TLS 1.3,\nwhich EmberOS doesn't have.";
        return "The site refused the secure connection.";
    }
    if (e >= BR_ERR_X509_OK && e < BR_ERR_X509_OK + 64) return "The site's certificate couldn't be verified.";
    return "The secure connection failed.";
}
