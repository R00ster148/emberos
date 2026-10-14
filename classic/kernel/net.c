/*
 * EmberOS networking: PCI scan, Intel e1000 and AMD PCnet drivers,
 * Ethernet / ARP / IPv4 / UDP / DHCP / DNS / TCP, and an HTTP/1.0 client.
 * Everything is polled (no NIC interrupts) and single-connection.
 */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint32_t csum_add(uint32_t sum, const uint8_t *d, uint32_t n) {
    for (; n > 1; n -= 2, d += 2) sum += get16(d);
    if (n) sum += (uint32_t)d[0] << 8;
    return sum;
}
static uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void print_ip(uint32_t ip) { kprintf("%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }

static int parse_ip(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); if (v > 255) return 0; }
        ip = ip << 8 | v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = ip;
    return 1;
}

static uint32_t rng = 0x1234567;
static uint32_t rnd32(void) { rng ^= ticks; rng = rng * 1664525u + 1013904223u; return rng; }

static int user_abort(void) { return kb_esc_pressed(); }

/* ------------------------------------------------------------------ */
/* PCI                                                                 */
/* ------------------------------------------------------------------ */
static uint32_t pci_read(int bus, int dev, int fn, int off) {
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (uint32_t)(off & 0xFC));
    return inl(0xCFC);
}
static void pci_write(int bus, int dev, int fn, int off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (uint32_t)(off & 0xFC));
    outl(0xCFC, v);
}

/* ------------------------------------------------------------------ */
/* NIC abstraction                                                     */
/* ------------------------------------------------------------------ */
static struct {
    const char *name;
    uint8_t mac[6];
    int  (*send)(const uint8_t *frame, uint32_t len);
    int  (*recv)(uint8_t *frame);            /* returns length or 0 */
} nic;

#define RX_COUNT 32
#define TX_COUNT 8
#define BUF_SZ   2048

/* DMA memory layout inside MEM_NET */
#define NET_RXDESC (MEM_NET + 0x0000)
#define NET_TXDESC (MEM_NET + 0x1000)
#define NET_INIT   (MEM_NET + 0x1800)
#define NET_RXBUF  (MEM_NET + 0x2000)
#define NET_TXBUF  (NET_RXBUF + RX_COUNT * BUF_SZ)

/* ---- Intel 8254x (e1000) ---- */
static volatile uint8_t *e1k;
static uint32_t e1k_rx_cur, e1k_tx_cur;

struct e1k_rx { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct e1k_tx { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

static uint32_t e1k_rd(uint32_t r) { return *(volatile uint32_t *)(e1k + r); }
static void e1k_wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(e1k + r) = v; }

static int e1k_send(const uint8_t *frame, uint32_t len) {
    volatile struct e1k_tx *d = (volatile struct e1k_tx *)NET_TXDESC + e1k_tx_cur;
    uint8_t *buf = (uint8_t *)(NET_TXBUF + e1k_tx_cur * BUF_SZ);
    memcpy(buf, frame, len);
    d->addr = (uint32_t)buf;
    d->len = (uint16_t)len;
    d->cmd = 0x0B;                         /* EOP | IFCS | RS */
    d->status = 0;
    e1k_tx_cur = (e1k_tx_cur + 1) % TX_COUNT;
    e1k_wr(0x3818, e1k_tx_cur);            /* TDT */
    uint32_t t0 = ticks;
    while (!(d->status & 1)) if (ticks - t0 > 50) return 0;
    return 1;
}

static int e1k_recv(uint8_t *frame) {
    volatile struct e1k_rx *d = (volatile struct e1k_rx *)NET_RXDESC + e1k_rx_cur;
    if (!(d->status & 1)) return 0;
    uint32_t len = d->len;
    if (len > 1518) len = 0;
    if (len) memcpy(frame, (void *)(NET_RXBUF + e1k_rx_cur * BUF_SZ), len);
    d->status = 0;
    e1k_wr(0x2818, e1k_rx_cur);            /* RDT: give it back */
    e1k_rx_cur = (e1k_rx_cur + 1) % RX_COUNT;
    return (int)len;
}

static int e1k_init(uint32_t mmio) {
    e1k = (volatile uint8_t *)mmio;
    e1k_wr(0x00D8, 0xFFFFFFFF);            /* IMC: mask all interrupts */
    e1k_wr(0x0000, e1k_rd(0x0000) | (1u << 26));   /* reset */
    uint32_t t0 = ticks; while (ticks - t0 < 2) {}
    while (e1k_rd(0x0000) & (1u << 26)) if (ticks - t0 > 100) return 0;
    e1k_wr(0x00D8, 0xFFFFFFFF);
    (void)e1k_rd(0x00C0);

    uint32_t ctrl = e1k_rd(0x0000);
    ctrl |= (1u << 6) | (1u << 5);         /* SLU, ASDE */
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7));
    e1k_wr(0x0000, ctrl);

    uint32_t ral = e1k_rd(0x5400), rah = e1k_rd(0x5404);
    if (!ral && !(rah & 0xFFFF)) {         /* read MAC from EEPROM */
        for (uint32_t i = 0; i < 3; i++) {
            e1k_wr(0x0014, (i << 8) | 1);
            uint32_t v; t0 = ticks;
            while (!((v = e1k_rd(0x0014)) & 0x10)) if (ticks - t0 > 20) return 0;
            nic.mac[i * 2] = (v >> 16) & 0xFF; nic.mac[i * 2 + 1] = v >> 24;
        }
        ral = nic.mac[0] | nic.mac[1] << 8 | (uint32_t)nic.mac[2] << 16 | (uint32_t)nic.mac[3] << 24;
        rah = nic.mac[4] | nic.mac[5] << 8;
    } else {
        for (int i = 0; i < 4; i++) nic.mac[i] = (ral >> (8 * i)) & 0xFF;
        nic.mac[4] = rah & 0xFF; nic.mac[5] = (rah >> 8) & 0xFF;
    }
    e1k_wr(0x5400, ral);
    e1k_wr(0x5404, (rah & 0xFFFF) | (1u << 31));  /* address valid */
    for (int i = 0; i < 128; i++) e1k_wr(0x5200 + i * 4, 0);

    volatile struct e1k_rx *rx = (volatile struct e1k_rx *)NET_RXDESC;
    for (int i = 0; i < RX_COUNT; i++) { rx[i].addr = NET_RXBUF + (uint32_t)i * BUF_SZ; rx[i].status = 0; }
    e1k_wr(0x2800, NET_RXDESC); e1k_wr(0x2804, 0);
    e1k_wr(0x2808, RX_COUNT * 16);
    e1k_wr(0x2810, 0); e1k_wr(0x2818, RX_COUNT - 1);
    e1k_rx_cur = 0;
    e1k_wr(0x0100, (1u << 1) | (1u << 15) | (1u << 26));   /* RCTL: EN | BAM | SECRC, 2 KB buffers */

    volatile struct e1k_tx *tx = (volatile struct e1k_tx *)NET_TXDESC;
    for (int i = 0; i < TX_COUNT; i++) { tx[i].addr = 0; tx[i].cmd = 0; tx[i].status = 1; }
    e1k_wr(0x3800, NET_TXDESC); e1k_wr(0x3804, 0);
    e1k_wr(0x3808, TX_COUNT * 16);
    e1k_wr(0x3810, 0); e1k_wr(0x3818, 0);
    e1k_tx_cur = 0;
    e1k_wr(0x0400, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12)); /* TCTL: EN | PSP | CT | COLD */
    e1k_wr(0x0410, 0x0060200A);           /* TIPG */

    nic.send = e1k_send; nic.recv = e1k_recv;
    return 1;
}

/* ---- AMD PCnet (Am79C970A / Am79C973) ---- */
static uint16_t pcn_io;
static uint32_t pcn_rx_cur, pcn_tx_cur;

struct pcn_desc { uint32_t addr; uint32_t flags; uint32_t flags2; uint32_t user; } __attribute__((packed));

static uint32_t pcn_csr_rd(uint32_t r) { outl(pcn_io + 0x14, r); return inl(pcn_io + 0x10); }
static void pcn_csr_wr(uint32_t r, uint32_t v) { outl(pcn_io + 0x14, r); outl(pcn_io + 0x10, v); }
static uint32_t pcn_bcr_rd(uint32_t r) { outl(pcn_io + 0x14, r); return inl(pcn_io + 0x1C); }
static void pcn_bcr_wr(uint32_t r, uint32_t v) { outl(pcn_io + 0x14, r); outl(pcn_io + 0x1C, v); }

static int pcn_send(const uint8_t *frame, uint32_t len) {
    volatile struct pcn_desc *d = (volatile struct pcn_desc *)NET_TXDESC + pcn_tx_cur;
    uint32_t t0 = ticks;
    while (d->flags & 0x80000000u) if (ticks - t0 > 50) return 0;
    uint8_t *buf = (uint8_t *)(NET_TXBUF + pcn_tx_cur * BUF_SZ);
    memcpy(buf, frame, len);
    d->addr = (uint32_t)buf;
    d->flags2 = 0;
    d->flags = 0x80000000u | 0x03000000u | 0xF000u | ((0u - len) & 0xFFF);  /* OWN | STP | ENP */
    pcn_tx_cur = (pcn_tx_cur + 1) % TX_COUNT;
    pcn_csr_wr(0, 0x0008);                 /* TDMD: transmit now */
    return 1;
}

static int pcn_recv(uint8_t *frame) {
    volatile struct pcn_desc *d = (volatile struct pcn_desc *)NET_RXDESC + pcn_rx_cur;
    if (d->flags & 0x80000000u) return 0;
    /* The card may clear OWN a moment before it writes the length; 0 means "not finished yet"
       (we reset it to 0 when recycling, and a real frame is never shorter than 64 bytes). */
    if (!(d->flags & 0x40000000u) && (d->flags2 & 0xFFF) == 0) {
        uint32_t t0 = ticks;
        while ((d->flags2 & 0xFFF) == 0) if (ticks - t0 > 2) break;
    }
    uint32_t len = 0;
    if (!(d->flags & 0x40000000u)) {       /* no error */
        len = d->flags2 & 0xFFF;
        len = len > 4 ? len - 4 : 0;       /* drop FCS */
        if (len > 1518) len = 0;
        if (len) memcpy(frame, (void *)d->addr, len);
    }
    d->flags2 = 0;
    d->flags = 0x80000000u | 0xF000u | ((0u - BUF_SZ) & 0xFFF);
    pcn_rx_cur = (pcn_rx_cur + 1) % RX_COUNT;
    return (int)len;
}

static int pcn_init(uint16_t io) {
    pcn_io = io;
    (void)inl(io + 0x18); (void)inw(io + 0x14);      /* reset (DWIO, then WIO) */
    uint32_t t0 = ticks; while (ticks - t0 < 2) {}
    for (int i = 0; i < 6; i++) nic.mac[i] = inb(io + i);   /* MAC from APROM (byte reads work in 16-bit mode) */
    outl(io + 0x10, 0);                               /* switch to 32-bit I/O */
    uint32_t lo = inl(io), hi = inl(io + 4);          /* re-read in 32-bit mode in case byte reads failed */
    if (nic.mac[0] == 0xFF && nic.mac[1] == 0xFF) {
        for (int i = 0; i < 4; i++) nic.mac[i] = (lo >> (8 * i)) & 0xFF;
        nic.mac[4] = hi & 0xFF; nic.mac[5] = (hi >> 8) & 0xFF;
    }

    pcn_bcr_wr(20, (pcn_bcr_rd(20) & ~0xFFu) | 2);    /* SWSTYLE 2: 32-bit structures */

    volatile struct pcn_desc *rx = (volatile struct pcn_desc *)NET_RXDESC;
    volatile struct pcn_desc *tx = (volatile struct pcn_desc *)NET_TXDESC;
    for (int i = 0; i < RX_COUNT; i++) {
        rx[i].addr = NET_RXBUF + (uint32_t)i * BUF_SZ; rx[i].flags2 = 0; rx[i].user = 0;
        rx[i].flags = 0x80000000u | 0xF000u | ((0u - BUF_SZ) & 0xFFF);
    }
    for (int i = 0; i < TX_COUNT; i++) { tx[i].addr = 0; tx[i].flags = 0; tx[i].flags2 = 0; tx[i].user = 0; }
    pcn_rx_cur = pcn_tx_cur = 0;

    uint8_t *ib = (uint8_t *)NET_INIT;
    memset(ib, 0, 28);
    ib[0] = 0; ib[1] = 0;                             /* mode: normal */
    ib[2] = 5 << 4;                                   /* RLEN: 2^5 = 32 */
    ib[3] = 3 << 4;                                   /* TLEN: 2^3 = 8 */
    memcpy(ib + 4, nic.mac, 6);
    /* ib[12..19] logical address filter = 0 */
    uint32_t rdra = NET_RXDESC, tdra = NET_TXDESC;
    memcpy(ib + 20, &rdra, 4);
    memcpy(ib + 24, &tdra, 4);

    pcn_csr_wr(1, NET_INIT & 0xFFFF);
    pcn_csr_wr(2, NET_INIT >> 16);
    pcn_csr_wr(3, pcn_csr_rd(3) | (1u << 10) | (1u << 9) | (1u << 8)); /* mask IDON/RINT/TINT irqs */
    pcn_csr_wr(4, pcn_csr_rd(4) | (1u << 11));         /* auto-pad short frames */
    pcn_csr_wr(0, 0x0001);                             /* INIT */
    t0 = ticks;
    while (!(pcn_csr_rd(0) & 0x0100)) if (ticks - t0 > 100) return 0;
    pcn_csr_wr(0, 0x0102);                             /* clear IDON, STRT */

    nic.send = pcn_send; nic.recv = pcn_recv;
    return 1;
}

static int nic_found;          /* 0 = not scanned, 1 = ok, -1 = none */
static uint16_t other_vendor, other_device;

static int nic_probe(void) {
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                uint16_t ven = id & 0xFFFF, did = id >> 16;
                uint32_t cls = pci_read(bus, dev, fn, 8) >> 16;
                uint32_t cmd = pci_read(bus, dev, fn, 4);
                uint32_t bar0 = pci_read(bus, dev, fn, 0x10);

                if (ven == 0x8086 && (did == 0x100E || did == 0x100F || did == 0x1004 || did == 0x10D3 || did == 0x1015 || did == 0x1016 || did == 0x1017)) {
                    pci_write(bus, dev, fn, 4, (cmd & 0xFFFF) | 0x6);     /* memory + bus master */
                    if (did == 0x10D3) continue;                          /* 82574 differs; skip */
                    if (e1k_init(bar0 & ~0xFu)) { nic.name = "Intel PRO/1000 (e1000)"; return 1; }
                }
                if (ven == 0x1022 && did == 0x2000) {
                    pci_write(bus, dev, fn, 4, (cmd & 0xFFFF) | 0x5);     /* I/O + bus master */
                    if (pcn_init((uint16_t)(bar0 & ~0x3u))) { nic.name = "AMD PCnet"; return 1; }
                }
                if (cls == 0x0200 && !other_vendor) { other_vendor = ven; other_device = did; }
                if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000)) break;  /* single function */
            }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ethernet, ARP, IPv4                                                 */
/* ------------------------------------------------------------------ */
static uint32_t my_ip, my_mask, gw_ip, dns_ip, dns_override;
static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct { uint32_t ip; uint8_t mac[6]; } arp_cache[8];
static int arp_next;

static uint8_t txf[1600], rxf[1600];

static int eth_send(const uint8_t *dst, uint16_t type, const uint8_t *payload, uint32_t len) {
    if (len > 1500) return 0;
    memcpy(txf, dst, 6);
    memcpy(txf + 6, nic.mac, 6);
    put16(txf + 12, type);
    memcpy(txf + 14, payload, len);
    uint32_t total = 14 + len;
    if (total < 60) { memset(txf + total, 0, 60 - total); total = 60; }
    return nic.send(txf, total);
}

static int arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < 8; i++) if (arp_cache[i].ip == ip && ip) { memcpy(mac, arp_cache[i].mac, 6); return 1; }
    return 0;
}
static void arp_store(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < 8; i++) if (arp_cache[i].ip == ip) { memcpy(arp_cache[i].mac, mac, 6); return; }
    arp_cache[arp_next].ip = ip; memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % 8;
}
static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip) {
    uint8_t p[28];
    put16(p, 1); put16(p + 2, 0x0800); p[4] = 6; p[5] = 4; put16(p + 6, op);
    memcpy(p + 8, nic.mac, 6); put32(p + 14, my_ip);
    memcpy(p + 18, tmac, 6); put32(p + 24, tip);
    eth_send(op == 1 ? bcast_mac : tmac, 0x0806, p, 28);
}

/* ---- UDP mailbox (one waiter at a time) ---- */
static uint16_t udp_port;
static uint8_t udp_buf[1500];
static uint32_t udp_len;

/* ---- TCP connection state ---- */
enum { T_CLOSED, T_SYN_SENT, T_EST };
static struct {
    int state, fin, rst;
    uint32_t rip; uint16_t rport, lport;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint8_t *buf; uint32_t len, cap, rd;   /* receive buffer: bytes [rd, len) not yet read */
    uint32_t total;                        /* total payload bytes received */
    uint32_t last_rx;
    uint32_t segs, ooo;              /* diagnostics: segments seen, out-of-order data segments */
} tcp;

static void tcp_out(uint8_t flags, const uint8_t *data, uint32_t len);
static int ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload, uint32_t len);

static void tcp_in(uint32_t src, const uint8_t *seg, uint32_t len) {
    if (len < 20 || tcp.state == T_CLOSED) return;
    if (src != tcp.rip || get16(seg) != tcp.rport || get16(seg + 2) != tcp.lport) return;
    uint32_t seq = get32(seg + 4), ack = get32(seg + 8);
    uint32_t hlen = (uint32_t)(seg[12] >> 4) * 4;
    uint8_t fl = seg[13];
    if (hlen < 20 || hlen > len) return;
    const uint8_t *data = seg + hlen;
    uint32_t dlen = len - hlen;

    if (fl & 0x04) { tcp.rst = 1; tcp.state = T_CLOSED; return; }          /* RST */

    if (tcp.state == T_SYN_SENT) {
        if ((fl & 0x12) == 0x12 && ack == tcp.snd_nxt) {                    /* SYN+ACK */
            tcp.rcv_nxt = seq + 1;
            tcp.snd_una = ack;
            tcp.state = T_EST;
            tcp_out(0x10, 0, 0);
        }
        return;
    }

    tcp.segs++;
    if ((fl & 0x10) && (int32_t)(ack - tcp.snd_una) > 0 && (int32_t)(ack - tcp.snd_nxt) <= 0) tcp.snd_una = ack;

    int need_ack = 0;
    if (dlen) {
        need_ack = 1;
        if (seq == tcp.rcv_nxt) {
            uint32_t room = tcp.cap - tcp.len;
            memcpy(tcp.buf + tcp.len, data, dlen < room ? dlen : room);
            tcp.len += dlen < room ? dlen : room;
            tcp.rcv_nxt += dlen;
            tcp.total += dlen;
            tcp.last_rx = ticks;
        } else tcp.ooo++;
    }
    if ((fl & 0x01) && seq + dlen == tcp.rcv_nxt && !tcp.fin) {             /* FIN in order */
        tcp.rcv_nxt++;
        tcp.fin = 1;
        need_ack = 1;
    }
    if (need_ack) tcp_out(0x10, 0, 0);
}

static void ip_in(const uint8_t *p, uint32_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    uint32_t hl = (uint32_t)(p[0] & 0xF) * 4;
    uint32_t tot = get16(p + 2);
    if (tot > len || hl < 20 || tot < hl) return;
    if (get16(p + 6) & 0x3FFF) return;                                       /* fragments: ignore */
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    if (my_ip && dst != my_ip && dst != 0xFFFFFFFF) return;
    const uint8_t *pl = p + hl; uint32_t plen = tot - hl;

    if (p[9] == 17 && plen >= 8) {                                           /* UDP */
        uint16_t dport = get16(pl + 2);
        uint32_t ulen = get16(pl + 4);
        if (dport == udp_port && ulen >= 8 && ulen <= plen && !udp_len) {
            udp_len = ulen - 8;
            memcpy(udp_buf, pl + 8, udp_len);
        }
    } else if (p[9] == 6) {
        tcp_in(src, pl, plen);
    } else if (p[9] == 1 && plen >= 8 && pl[0] == 8 && my_ip) {             /* ICMP echo -> reply */
        uint8_t r[1500];
        if (plen > sizeof(r)) return;
        memcpy(r, pl, plen);
        r[0] = 0; r[2] = r[3] = 0;
        put16(r + 2, csum_fold(csum_add(0, r, plen)));
        ip_send(src, 1, r, plen);
    }
}

static void net_poll(void) {
    int n;
    while ((n = nic.recv(rxf)) > 0) {
        if (n < 14) continue;
        uint16_t type = get16(rxf + 12);
        const uint8_t *p = rxf + 14; uint32_t len = (uint32_t)n - 14;
        if (type == 0x0806 && len >= 28) {
            uint16_t op = get16(p + 6);
            uint32_t sip = get32(p + 14), tip = get32(p + 24);
            if (sip) arp_store(sip, p + 8);
            if (op == 1 && my_ip && tip == my_ip) arp_send(2, p + 8, sip);
        } else if (type == 0x0800) {
            ip_in(p, len);
        }
    }
}

static int arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_lookup(ip, mac)) return 1;
    for (int tries = 0; tries < 4; tries++) {
        arp_send(1, (const uint8_t *)"\0\0\0\0\0\0", ip);
        uint32_t t0 = ticks;
        while (ticks - t0 < 50) { net_poll(); if (arp_lookup(ip, mac)) return 1; }
    }
    return 0;
}

static uint16_t ip_id = 1;
static int ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload, uint32_t len) {
    uint8_t pkt[1500];
    uint8_t mac[6];
    if (len > 1480) return 0;
    if (dst == 0xFFFFFFFF) memcpy(mac, bcast_mac, 6);
    else {
        uint32_t hop = ((dst & my_mask) == (my_ip & my_mask)) ? dst : gw_ip;
        if (!arp_resolve(hop, mac)) return 0;
    }
    memset(pkt, 0, 20);
    pkt[0] = 0x45; put16(pkt + 2, (uint16_t)(20 + len)); put16(pkt + 4, ip_id++);
    put16(pkt + 6, 0x4000); pkt[8] = 64; pkt[9] = proto;
    put32(pkt + 12, my_ip); put32(pkt + 16, dst);
    put16(pkt + 10, csum_fold(csum_add(0, pkt, 20)));
    memcpy(pkt + 20, payload, len);
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len) {
    uint8_t seg[1480];
    if (len + 8 > sizeof(seg)) return 0;
    put16(seg, sport); put16(seg + 2, dport); put16(seg + 4, (uint16_t)(8 + len)); put16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    return ip_send(dst, 17, seg, 8 + len);   /* UDP checksum 0 = none (allowed in IPv4) */
}

/* Wait for a UDP datagram on udp_port. Returns length or -1 on timeout. */
static int udp_wait(uint32_t timeout_ticks) {
    uint32_t t0 = ticks;
    while (ticks - t0 < timeout_ticks) {
        net_poll();
        if (udp_len) return (int)udp_len;
        if (user_abort()) return -2;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* DHCP                                                                */
/* ------------------------------------------------------------------ */
static int dhcp_opt(const uint8_t *opts, uint32_t len, uint8_t code, const uint8_t **val) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t c = opts[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t l = opts[i + 1];
        if (c == code) { *val = opts + i + 2; return l; }
        i += 2u + l;
    }
    return -1;
}

static int dhcp(void) {
    uint8_t p[300];
    uint32_t xid = rnd32();
    uint32_t offered = 0, server = 0;

    for (int phase = 0; phase < 2; phase++) {
        int done = 0;
        for (int tries = 0; tries < 4 && !done; tries++) {
            memset(p, 0, sizeof(p));
            p[0] = 1; p[1] = 1; p[2] = 6;
            put32(p + 4, xid);
            put16(p + 10, 0x8000);                       /* broadcast flag */
            memcpy(p + 28, nic.mac, 6);
            put32(p + 236, 0x63825363);                  /* magic cookie */
            uint8_t *o = p + 240;
            *o++ = 53; *o++ = 1; *o++ = phase == 0 ? 1 : 3;   /* DISCOVER / REQUEST */
            if (phase == 1) {
                *o++ = 50; *o++ = 4; put32(o, offered); o += 4;
                *o++ = 54; *o++ = 4; put32(o, server); o += 4;
            }
            *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
            *o++ = 255;
            udp_port = 68; udp_len = 0;
            udp_send(0xFFFFFFFF, 68, 67, p, (uint32_t)(o - p));

            uint32_t t0 = ticks;
            while (ticks - t0 < 200 && !done) {
                int n = udp_wait(200 - (ticks - t0));
                if (n == -2) return 0;
                if (n < 0) break;
                udp_len = 0;
                if (n < 240 || udp_buf[0] != 2 || get32(udp_buf + 4) != xid) continue;
                const uint8_t *v; uint32_t olen = (uint32_t)n - 240;
                if (dhcp_opt(udp_buf + 240, olen, 53, &v) != 1) continue;
                if (phase == 0 && v[0] == 2) {
                    offered = get32(udp_buf + 16);
                    if (dhcp_opt(udp_buf + 240, olen, 54, &v) == 4) server = get32(v);
                    done = 1;
                } else if (phase == 1 && v[0] == 5) {
                    my_ip = get32(udp_buf + 16);
                    my_mask = 0xFFFFFF00; gw_ip = 0; dns_ip = 0;
                    if (dhcp_opt(udp_buf + 240, olen, 1, &v) == 4) my_mask = get32(v);
                    if (dhcp_opt(udp_buf + 240, olen, 3, &v) >= 4) gw_ip = get32(v);
                    if (dhcp_opt(udp_buf + 240, olen, 6, &v) >= 4) dns_ip = get32(v);
                    if (!gw_ip) gw_ip = server;
                    done = 1;
                } else if (phase == 1 && v[0] == 6) {
                    return 0;                            /* NAK */
                }
            }
        }
        if (!done) return 0;
    }
    udp_port = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */
/* ------------------------------------------------------------------ */
static int dns_skip_name(const uint8_t *m, uint32_t len, uint32_t i) {
    while (i < len) {
        uint8_t l = m[i];
        if (l == 0) return (int)i + 1;
        if ((l & 0xC0) == 0xC0) return (int)i + 2;
        i += 1u + l;
    }
    return -1;
}

static int dns_resolve(const char *host, uint32_t *out) {
    if (parse_ip(host, out)) return 1;
    uint32_t server = dns_override ? dns_override : dns_ip;
    if (!server) return 0;
    uint8_t q[300];
    uint16_t id = (uint16_t)rnd32();
    memset(q, 0, 12);
    put16(q, id); put16(q + 2, 0x0100); put16(q + 4, 1);
    uint32_t n = 12;
    const char *s = host;
    while (*s) {
        const char *dot = s; while (*dot && *dot != '.') dot++;
        uint32_t l = (uint32_t)(dot - s);
        if (l == 0 || l > 63 || n + l + 6 > sizeof(q)) return 0;
        q[n++] = (uint8_t)l; memcpy(q + n, s, l); n += l;
        s = *dot ? dot + 1 : dot;
    }
    q[n++] = 0; put16(q + n, 1); put16(q + n + 2, 1); n += 4;

    for (int tries = 0; tries < 3; tries++) {
        udp_port = (uint16_t)(40000 + rnd32() % 20000); udp_len = 0;
        udp_send(server, udp_port, 53, q, n);
        int r = udp_wait(300);
        if (r == -2) { udp_port = 0; return 0; }
        if (r < 12 || get16(udp_buf) != id) continue;
        udp_port = 0;
        uint32_t len = (uint32_t)r;
        if ((udp_buf[3] & 0x0F) != 0) return 0;                   /* NXDOMAIN etc */
        uint16_t qd = get16(udp_buf + 4), an = get16(udp_buf + 6);
        int i = 12;
        for (int k = 0; k < qd && i >= 0; k++) { i = dns_skip_name(udp_buf, len, (uint32_t)i); if (i >= 0) i += 4; }
        for (int k = 0; k < an && i >= 0; k++) {
            i = dns_skip_name(udp_buf, len, (uint32_t)i);
            if (i < 0 || (uint32_t)i + 10 > len) return 0;
            uint16_t type = get16(udp_buf + i), rdl = get16(udp_buf + i + 8);
            i += 10;
            if (type == 1 && rdl == 4 && (uint32_t)i + 4 <= len) { *out = get32(udp_buf + i); return 1; }
            i += rdl;
        }
        return 0;
    }
    udp_port = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* TCP (client, one connection)                                        */
/* ------------------------------------------------------------------ */
#define TCP_WINDOW 16384

static void tcp_out(uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t seg[1480];
    uint32_t hl = (flags & 0x02) ? 24 : 20;
    memset(seg, 0, hl);
    put16(seg, tcp.lport); put16(seg + 2, tcp.rport);
    put32(seg + 4, (flags & 0x02) ? tcp.snd_nxt - 1 : tcp.snd_nxt - ((flags & 0x01) ? 1 : 0) - len);
    put32(seg + 8, (flags & 0x10) ? tcp.rcv_nxt : 0);
    seg[12] = (uint8_t)((hl / 4) << 4); seg[13] = flags;
    put16(seg + 14, TCP_WINDOW);
    if (flags & 0x02) { seg[20] = 2; seg[21] = 4; put16(seg + 22, 1460); }   /* MSS */
    memcpy(seg + hl, data, len);
    uint8_t ph[12];
    put32(ph, my_ip); put32(ph + 4, tcp.rip); ph[8] = 0; ph[9] = 6; put16(ph + 10, (uint16_t)(hl + len));
    put16(seg + 16, csum_fold(csum_add(csum_add(0, ph, 12), seg, hl + len)));
    ip_send(tcp.rip, 6, seg, hl + len);
}

static int tcp_connect(uint32_t ip, uint16_t port) {
    memset(&tcp, 0, sizeof(tcp));
    tcp.rip = ip; tcp.rport = port;
    tcp.lport = (uint16_t)(49152 + rnd32() % 16000);
    tcp.snd_una = rnd32();
    tcp.snd_nxt = tcp.snd_una + 1;                 /* SYN uses one sequence number */
    tcp.state = T_SYN_SENT;
    tcp.buf = (uint8_t *)MEM_TCPRAW; tcp.cap = MEM_TCPRAW_SZ;
    for (int tries = 0; tries < 5; tries++) {
        tcp_out(0x02, 0, 0);
        uint32_t t0 = ticks;
        while (ticks - t0 < (uint32_t)(100 + tries * 50)) {
            net_poll();
            if (tcp.state == T_EST) return 1;
            if (tcp.rst) return 0;
            if (user_abort()) { tcp.state = T_CLOSED; return 0; }
        }
    }
    tcp.state = T_CLOSED;
    return 0;
}

/* Send data in segments, waiting for each to be acknowledged (simple and reliable). */
static int tcp_write(const uint8_t *data, uint32_t len) {
    while (len) {
        uint32_t n = len > 1400 ? 1400 : len;
        tcp.snd_nxt += n;
        int ok = 0;
        for (int tries = 0; tries < 6 && !ok; tries++) {
            tcp_out(0x18, data, n);                /* PSH | ACK */
            uint32_t t0 = ticks;
            while (ticks - t0 < (uint32_t)(100 + tries * 100)) {
                net_poll();
                if (tcp.snd_una == tcp.snd_nxt) { ok = 1; break; }
                if (tcp.rst || tcp.state == T_CLOSED) return 0;
                if (user_abort()) return 0;
            }
        }
        if (!ok) return 0;
        data += n; len -= n;
    }
    return 1;
}

static void tcp_close(void) {
    if (tcp.state == T_EST && !tcp.rst) { tcp.snd_nxt++; tcp_out(0x11, 0, 0); }
    uint32_t t0 = ticks;
    while (ticks - t0 < 5) net_poll();
    tcp.state = T_CLOSED;
}

/* ------------------------------------------------------------------ */
/* Bring-up and status                                                 */
/* ------------------------------------------------------------------ */
static int net_ready;

/* Web browsing keeps its buffers between 1 MB and 9 MB; make sure that RAM exists. */
static int ram_ok(void) {
    volatile uint32_t *p = (volatile uint32_t *)(MEM_MIN_MB * 0x100000u - 16);
    uint32_t old = *p;
    *p = 0x5A17C0DEu;
    int ok = *p == 0x5A17C0DEu;
    *p = old;
    return ok;
}

int net_up(int verbose) {
    if (net_ready) return 1;
    if (!ram_ok()) {
        set_color(0x0C); kprintf("The web needs at least %u MB of RAM. Give the VM more memory\n"
                                 "(Settings > System > Base Memory, 64 MB is plenty).\n", (uint32_t)MEM_MIN_MB); reset_color();
        return 0;
    }
    if (!nic_found) nic_found = nic_probe() ? 1 : -1;
    if (nic_found < 0) {
        set_color(0x0C); puts("No supported network card found.\n"); reset_color();
        if (other_vendor) kprintf("Found an unsupported card (PCI %04x:%04x).\n", (uint32_t)other_vendor, (uint32_t)other_device);
        puts("In VirtualBox: Settings > Network > Adapter 1: Enable, Attached to: NAT,\n"
             "Advanced > Adapter Type: Intel PRO/1000 MT Desktop (or PCnet-FAST III).\n");
        return 0;
    }
    if (verbose) kprintf("Network card: %s\nGetting an address (DHCP)... ", nic.name);
    if (!dhcp()) {
        if (verbose) puts("\n");
        set_color(0x0C); puts("DHCP failed - is the VM's network adapter attached to NAT?\n"); reset_color();
        return 0;
    }
    if (verbose) { print_ip(my_ip); putc('\n'); }
    net_ready = 1;
    return 1;
}

void net_status(void) {
    if (!net_up(1)) return;
    kprintf("  card     %s\n  mac      %02x:%02x:%02x:%02x:%02x:%02x\n", nic.name,
            (uint32_t)nic.mac[0], (uint32_t)nic.mac[1], (uint32_t)nic.mac[2], (uint32_t)nic.mac[3], (uint32_t)nic.mac[4], (uint32_t)nic.mac[5]);
    puts("  ip       "); print_ip(my_ip); puts("\n  netmask  "); print_ip(my_mask);
    puts("\n  gateway  "); print_ip(gw_ip); puts("\n  dns      "); print_ip(dns_override ? dns_override : dns_ip);
    puts(dns_override ? " (manual)\n" : "\n");
}

void net_set_dns(const char *s) {
    uint32_t ip;
    if (!parse_ip(s, &ip)) { puts("usage: net dns <a.b.c.d>\n"); return; }
    dns_override = ip;
    puts("DNS server set to "); print_ip(ip); putc('\n');
}

/* ------------------------------------------------------------------ */
/* HTTP/1.0 GET                                                        */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Stream I/O used by HTTP and by the TLS layer                        */
/* ------------------------------------------------------------------ */
enum { F_OK, F_DNS, F_CONNECT, F_SEND, F_RESET, F_CLOSED, F_TIMEOUT, F_CANCEL, F_BAD, F_TLS };

static uint32_t io_first_timeout = 4500;
static int io_fail;

/* Returns bytes read, 0 at end of stream, -1 on error (reason in io_fail). */
int net_raw_read(uint8_t *dst, uint32_t max) {
    for (;;) {
        if (tcp.len > tcp.rd) {
            uint32_t n = tcp.len - tcp.rd;
            if (n > max) n = max;
            memcpy(dst, tcp.buf + tcp.rd, n);
            tcp.rd += n;
            if (tcp.rd == tcp.len) tcp.rd = tcp.len = 0;
            return (int)n;
        }
        if (tcp.fin) return 0;
        if (tcp.rst) { io_fail = F_RESET; return -1; }
        if (ticks - tcp.last_rx > (tcp.total ? 1500u : io_first_timeout)) { io_fail = F_TIMEOUT; return -1; }
        if (user_abort()) { io_fail = F_CANCEL; tcp_out(0x14, 0, 0); tcp.state = T_CLOSED; tcp.rst = 1; return -1; }
        net_poll();
    }
}

int net_raw_write(const uint8_t *src, uint32_t len) {
    if (tcp.state != T_EST || tcp.rst) { io_fail = F_RESET; return -1; }
    if (!tcp_write(src, len)) { io_fail = tcp.rst ? F_RESET : F_SEND; return -1; }
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* HTTP/1.0 client (http:// and https://)                              */
/* ------------------------------------------------------------------ */
static int parse_url(const char *url, int *tls, char *host, int hcap, uint16_t *port, const char **path) {
    *tls = 0;
    if (!strncmp(url, "http://", 7)) url += 7;
    else if (!strncmp(url, "https://", 8)) { url += 8; *tls = 1; }
    int n = 0;
    while (*url && *url != '/' && *url != ':' && *url != '?') { if (n + 1 < hcap) host[n++] = *url; url++; }
    host[n] = 0;
    *port = *tls ? 443 : 80;
    if (*url == ':') { url++; uint32_t p = 0; while (*url >= '0' && *url <= '9') p = p * 10 + (uint32_t)(*url++ - '0'); *port = (uint16_t)p; }
    *path = *url ? url : "/";
    return n > 0;
}

static int hdr_find(const char *hdr, uint32_t hlen, const char *name, char *out, int cap) {
    uint32_t nl = (uint32_t)strlen(name);
    for (uint32_t i = 0; i + nl < hlen; i++) {
        if (i && hdr[i - 1] != '\n') continue;
        uint32_t k = 0;
        while (k < nl && ((hdr[i + k] | 0x20) == (name[k] | 0x20))) k++;
        if (k < nl || hdr[i + k] != ':') continue;
        i += k + 1;
        while (i < hlen && hdr[i] == ' ') i++;
        int n = 0;
        while (i < hlen && hdr[i] != '\r' && hdr[i] != '\n') { if (n + 1 < cap) out[n++] = hdr[i]; i++; }
        out[n] = 0;
        return 1;
    }
    return 0;
}

static uint32_t dechunk(uint8_t *b, uint32_t len) {
    uint32_t r = 0, w = 0;
    for (;;) {
        uint32_t sz = 0; int digits = 0;
        while (r < len) {
            uint8_t c = b[r];
            int d = (c >= '0' && c <= '9') ? c - '0' : ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') ? (c | 0x20) - 'a' + 10 : -1;
            if (d < 0) break;
            sz = sz * 16 + (uint32_t)d; r++; digits++;
        }
        while (r < len && b[r] != '\n') r++;
        r++;
        if (!digits || sz == 0 || r >= len) break;
        if (sz > len - r) sz = len - r;
        memmove(b + w, b + r, sz);
        w += sz; r += sz + 2;
    }
    return w;
}

static uint32_t page_len;

/* One HTTP/1.0 request. On F_OK the reply is in MEM_PAGE[0..page_len) and *hdr_end is set. */
static int fetch_once(const char *host, uint16_t port, int tls, const char *path, int verbose,
                      uint32_t first_byte_ticks, uint32_t *hdr_end, uint32_t *elapsed) {
    uint32_t ip;
    uint32_t start = ticks;
    *elapsed = 0;
    page_len = 0;
    io_fail = F_OK;
    io_first_timeout = first_byte_ticks;
    if (verbose) kprintf("Looking up %s... ", host);
    if (!dns_resolve(host, &ip)) { if (verbose) puts("not found.\n"); return F_DNS; }
    if (verbose) { print_ip(ip); puts("\nConnecting... "); }
    if (!tcp_connect(ip, port)) { if (verbose) puts("failed.\n"); return tcp.rst ? F_RESET : F_CONNECT; }
    tcp.last_rx = ticks;

    static char req[900];
    int n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nUser-Agent: Mozilla/5.0 (compatible; EmberOS/" OS_VERSION "; text browser)\r\n"
                            "Accept: text/html, text/plain, */*\r\nAccept-Language: en-GB,en;q=0.8\r\n"
                            "Connection: close\r\n\r\n" };
    for (unsigned k = 0; k < 5; k++) for (const char *c = parts[k]; *c && n < (int)sizeof(req) - 1; c++) req[n++] = *c;

    if (tls) {
        if (verbose) puts("securing... ");
        if (!tls_begin(host) || !tls_send(req, (uint32_t)n)) {
            int r = tls_error_is_transport() ? (io_fail ? io_fail : F_CLOSED) : F_TLS;
            *elapsed = ticks - start;
            if (verbose) putc('\n');
            tcp_close();
            return r;
        }
    } else if (net_raw_write((const uint8_t *)req, (uint32_t)n) < 0) {
        if (verbose) puts("send failed.\n");
        tcp_close();
        return io_fail;
    }

    if (verbose) puts("waiting for reply... ");
    int col0 = cur_col;
    start = ticks;
    tcp.last_rx = ticks;
    uint32_t shown = 0xFFFFFFFF;
    uint8_t *page = (uint8_t *)MEM_PAGE;
    int r = 0;
    for (;;) {
        uint32_t room = MEM_PAGE_SZ - page_len;
        if (!room) break;
        r = tls ? tls_recv(page + page_len, room) : net_raw_read(page + page_len, room);
        if (r <= 0) break;
        page_len += (uint32_t)r;
        if (verbose && page_len / 1024 != shown) {
            shown = page_len / 1024; cur_col = col0;
            kprintf("%u KB received      ", shown);
        }
    }
    int tls_bad = tls && r < 0 && !tls_error_is_transport();
    if (tls) tls_end();
    *elapsed = ticks - start;
    tcp_close();
    if (verbose) putc('\n');

    for (uint32_t i = 0; i + 3 < page_len; i++)
        if (page[i] == '\r' && page[i + 1] == '\n' && page[i + 2] == '\r' && page[i + 3] == '\n') { *hdr_end = i + 4; return F_OK; }
    if (io_fail == F_CANCEL) return F_CANCEL;
    if (page_len) return F_BAD;
    if (tls_bad) return F_TLS;
    if (io_fail == F_RESET || io_fail == F_TIMEOUT) return io_fail;
    return F_CLOSED;
}

static int http_status(const uint8_t *b, uint32_t he) {
    int status = 0;
    for (uint32_t i = 0; i < he; i++) if (b[i] == ' ') {
        for (uint32_t k = i + 1; k < i + 4 && k < he; k++) status = status * 10 + (b[k] - '0');
        break;
    }
    return status;
}

static void explain_failure(int r, uint32_t secs) {
    set_color(0x0C);
    switch (r) {
    case F_DNS:     puts("Couldn't find that site's address (DNS lookup failed).\n"); break;
    case F_CONNECT: puts("Couldn't connect to the server.\n"); break;
    case F_SEND:    puts("Couldn't send the request.\n"); break;
    case F_RESET:   kprintf("The connection was reset without a reply (after %us).\n", secs); break;
    case F_CLOSED:  kprintf("The server closed the connection without replying (after %us).\n", secs); break;
    case F_TIMEOUT: kprintf("No reply after %u seconds.\n", secs); break;
    case F_BAD:     kprintf("Got %u bytes, but it wasn't a valid web page reply.\n", page_len); break;
    case F_TLS:     puts(tls_error_text(tls_error())); set_color(0x08); kprintf("\n(TLS error %u)\n", (uint32_t)tls_error()); break;
    }
    reset_color();
    if (r == F_TLS && tls_error() >= 32 && tls_error() < 64) {      /* certificate problem: show the chain */
        putc('\n');
        tls_print_chain();
        if (tls_error() == 62) {
            set_color(0x0E);
            puts("\nIf an issuer above is your antivirus, firewall or school/work network rather\n"
                 "than a public authority (Google Trust Services, Let's Encrypt, DigiCert...),\n"
                 "then it is intercepting secure connections on this computer or network.\n");
            reset_color();
        }
    }
    if (r == F_RESET || r == F_CLOSED || r == F_TIMEOUT) {
        set_color(0x08); kprintf("(packets from server: %u, out of order: %u)\n", tcp.segs, tcp.ooo); reset_color();
    }
}

int http_cancelled;
int http_last_status;

/* Fetches url (follows redirects). Body points into MEM_PAGE. Returns HTTP status or 0. */
int http_get(const char *url_in, uint8_t **body, uint32_t *blen, char *final_url, int final_cap) {
    static char url[600];
    strncpy0(url, url_in, sizeof(url));
    http_cancelled = 0;
    http_last_status = 0;
    if (!net_up(0)) return 0;

    for (int redirects = 0; redirects < 8; redirects++) {
        char host[128]; uint16_t port; const char *path; int tls;
        if (!parse_url(url, &tls, host, sizeof(host), &port, &path)) {
            set_color(0x0C); puts("That doesn't look like a web address.\n"); reset_color();
            return 0;
        }
        uint32_t he = 0, el;
        int r = fetch_once(host, port, tls, path, 1, 4500, &he, &el);
        if (r != F_OK) {
            http_cancelled = r == F_CANCEL;
            if (r != F_CANCEL) explain_failure(r, el / HZ);
            if (r == F_RESET || r == F_CLOSED || r == F_TIMEOUT)
                puts("Type 'nettest' to check which sites your connection can reach.\n");
            return 0;
        }
        uint8_t *b = (uint8_t *)MEM_PAGE; uint32_t len = page_len;
        int status = http_status(b, he);
        http_last_status = status;
        char val[600];
        if (status >= 300 && status < 400 && hdr_find((const char *)b, he, "Location", val, sizeof(val))) {
            if (val[0] == '/' && val[1] == '/') {              /* scheme-relative */
                char tmp[600]; tmp[0] = 0;
                strncpy0(tmp, tls ? "https:" : "http:", sizeof(tmp));
                int k = (int)strlen(tmp);
                for (const char *v = val; *v && k < 599; v++) tmp[k++] = *v;
                tmp[k] = 0;
                strncpy0(url, tmp, sizeof(url));
            } else if (val[0] == '/') {                         /* same site */
                char tmp[600]; int k = 0;
                const char *pre = tls ? "https://" : "http://";
                while (*pre) tmp[k++] = *pre++;
                for (const char *h = host; *h && k < 580; h++) tmp[k++] = *h;
                if (port != (tls ? 443 : 80)) { tmp[k++] = ':'; char d[6]; int nd = 0; uint32_t p = port; do { d[nd++] = (char)('0' + p % 10); p /= 10; } while (p); while (nd) tmp[k++] = d[--nd]; }
                for (const char *v = val; *v && k < 599; v++) tmp[k++] = *v;
                tmp[k] = 0;
                strncpy0(url, tmp, sizeof(url));
            } else strncpy0(url, val, sizeof(url));
            set_color(0x08); kprintf("Redirected to %s\n", url); reset_color();
            continue;
        }
        *body = b + he;
        *blen = len - he;
        if (hdr_find((const char *)b, he, "Transfer-Encoding", val, sizeof(val)) && (val[0] | 0x20) == 'c')
            *blen = dechunk(*body, *blen);
        if (final_url) strncpy0(final_url, url, (size_t)final_cap);
        return status;
    }
    set_color(0x0C); puts("Too many redirects.\n"); reset_color();
    return 0;
}

/* nettest: try a few sites over HTTP and HTTPS and say what happened with each. */
void net_test(void) {
    static const char *const sites[][3] = {
        { "http",  "example.com", "/" },
        { "http",  "neverssl.com", "/" },
        { "https", "example.com", "/" },
        { "https", "html.duckduckgo.com", "/html/?q=test" },
        { "https", "www.mojeek.com", "/search?q=test" },
        { "https", "en.wikipedia.org", "/wiki/Main_Page" },
        { "http",  "wiby.me", "/" },
    };
    enum { N = sizeof(sites) / sizeof(sites[0]) };
    int res[N];
    if (!net_up(1)) return;
    puts("Testing sites (Esc skips one)...\n\n");
    for (int i = 0; i < N; i++) {
        int tls = sites[i][0][4] == 's';
        int n = 0;
        kprintf("  %s://%s", sites[i][0], sites[i][1]);
        n = (int)strlen(sites[i][0]) + 3 + (int)strlen(sites[i][1]);
        for (; n < 34; n++) putc(' ');
        uint32_t he = 0, el;
        int r = fetch_once(sites[i][1], tls ? 443 : 80, tls, sites[i][2], 0, 1500, &he, &el);
        res[i] = r;
        uint32_t secs = el / HZ;
        if (r == F_OK) { set_color(0x0A); kprintf("OK  HTTP %u, %u KB\n", (uint32_t)http_status((uint8_t *)MEM_PAGE, he), (page_len + 1023) / 1024); }
        else {
            set_color(0x0C);
            switch (r) {
            case F_DNS: puts("DNS lookup failed\n"); break;
            case F_CONNECT: puts("could not connect\n"); break;
            case F_SEND: puts("send failed\n"); break;
            case F_RESET: kprintf("reset after %us\n", secs); break;
            case F_CLOSED: kprintf("closed, no reply (%us)\n", secs); break;
            case F_TIMEOUT: kprintf("no reply in %us\n", secs); break;
            case F_CANCEL: puts("skipped\n"); break;
            case F_TLS: kprintf("TLS error %u\n", (uint32_t)tls_error()); break;
            default: kprintf("bad reply (%u bytes)\n", page_len); break;
            }
        }
        reset_color();
    }
    putc('\n');
    int http_ok = res[0] == F_OK || res[1] == F_OK;
    int https_ok = res[2] == F_OK || res[5] == F_OK;
    int any_dns = 0; for (int i = 0; i < N; i++) any_dns |= res[i] != F_DNS;
    if (!any_dns) puts("DNS isn't working. Check the VM's network adapter is attached to NAT.\n");
    else if (https_ok && (res[3] == F_OK || res[4] == F_OK)) puts("Everything looks good - search and HTTPS should work.\n");
    else if (https_ok) puts("HTTPS works, but the search engines didn't answer - 'search' will try\nthe others in turn.\n");
    else if (res[2] == F_TLS || res[5] == F_TLS) puts("Secure connections are failing. Check the VM's clock with 'time' - if the\ndate is wrong, certificates look invalid.\n");
    else if (http_ok) puts("Plain HTTP works but HTTPS doesn't - something on your network may be\ninterfering with secure connections.\n");
    else puts("Nothing worked. Check the VM's network settings (Adapter 1: NAT).\n");
}
