/*
 * EmberOS text web browser + search.
 * Pages are fetched directly over HTTP or HTTPS (TLS via BearSSL in tls.c).
 * Search uses DuckDuckGo's HTML version, with Mojeek and Wiby as backups.
 */
#include "kernel.h"

#define URL_MAX     600
#define LINK_MAX    512
#define MAX_LINKS   (0x80000 / LINK_MAX)

#define DOC   ((uint16_t *)MEM_DOC)
#define LINKS ((char (*)[LINK_MAX])MEM_LINKS)

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */
static void urlencode(const char *s, char *out, int cap, int space_plus) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s && n + 4 < cap; s++) {
        uint8_t c = (uint8_t)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[n++] = (char)c;
        else if (c == ' ' && space_plus) out[n++] = '+';
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
}

static void str_cat(char *d, int cap, const char *s) {
    int n = (int)strlen(d);
    while (*s && n + 1 < cap) d[n++] = *s++;
    d[n] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Decode %XX and '+' from s (up to '&' or end) into out. */
static void urldecode_param(const char *s, char *out, int cap) {
    int n = 0;
    while (*s && *s != '&' && n + 1 < cap) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) { out[n++] = (char)(hexval(s[1]) * 16 + hexval(s[2])); s += 3; }
        else if (*s == '+') { out[n++] = ' '; s++; }
        else out[n++] = *s++;
    }
    out[n] = 0;
}

static const char *find_str(const char *h, const char *n) {
    size_t nl = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, nl)) return h;
    return 0;
}

/* Resolve href against base. Returns 0 for links we can't follow. */
static int resolve(const char *base, const char *href, char *out, int cap) {
    while (*href == ' ') href++;
    if (!*href || *href == '#' || !strncmp(href, "javascript:", 11) || !strncmp(href, "mailto:", 7)) return 0;
    out[0] = 0;
    /* DuckDuckGo wraps result links in a redirect: //duckduckgo.com/l/?uddg=<real url>&rut=... */
    const char *u = find_str(href, "duckduckgo.com/l/?");
    if (u && (u = find_str(u, "uddg=")) != 0) {
        urldecode_param(u + 5, out, cap);
        if (!strncmp(out, "http://", 7) || !strncmp(out, "https://", 8)) return 1;
        out[0] = 0;
    }
    int base_tls = !strncmp(base, "https://", 8);
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) { str_cat(out, cap, href); return 1; }
    if (!strncmp(href, "//", 2)) { str_cat(out, cap, base_tls ? "https:" : "http:"); str_cat(out, cap, href); return 1; }

    /* origin = "scheme://host[:port]" of base */
    int i = base_tls ? 8 : 7;
    while (base[i] && base[i] != '/' && base[i] != '?') i++;
    if (href[0] == '/') {
        for (int k = 0; k < i && k + 1 < cap; k++) { out[k] = base[k]; out[k + 1] = 0; }
        str_cat(out, cap, href);
        return 1;
    }
    /* relative: base directory (path up to last '/', ignoring the query) */
    int end = i, last = -1;
    while (base[end] && base[end] != '?') { if (base[end] == '/') last = end; end++; }
    int keep = last >= 0 ? last + 1 : i;
    for (int k = 0; k < keep && k + 1 < cap; k++) { out[k] = base[k]; out[k + 1] = 0; }
    if (last < 0) str_cat(out, cap, "/");
    if (href[0] == '?') {                         /* query-only link keeps the path */
        out[0] = 0;
        for (int k = 0; k < end && k + 1 < cap; k++) { out[k] = base[k]; out[k + 1] = 0; }
        if (end == i) str_cat(out, cap, "/");
    }
    if (!strncmp(href, "./", 2)) href += 2;
    str_cat(out, cap, href);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Document buffer: wrapped lines of VGA cells                         */
/* ------------------------------------------------------------------ */
static int d_line, d_col, d_trail_blank, d_pend_space, d_full;
static int nlinks;
static char page_title[COLS];
static int title_len;

static void doc_clear_line(int l) { for (int c = 0; c < COLS; c++) DOC[l * COLS + c] = ' ' | 0x0700; }
static void doc_reset(void) { d_line = d_col = d_pend_space = d_full = 0; d_trail_blank = 1; nlinks = 0; doc_clear_line(0); title_len = 0; page_title[0] = 0; }

static void doc_newline(void) {
    if (d_line + 1 >= DOC_LINES - 3) { d_full = 1; return; }
    d_line++; d_col = 0;
    doc_clear_line(d_line);
}

static void doc_putc(char ch, uint8_t a) {
    if (d_full) return;
    if (d_col >= COLS) {
        uint16_t *ln = DOC + d_line * COLS;
        int s = COLS - 1;
        while (s > 20 && (ln[s] & 0xFF) != ' ') s--;
        if (s > 20) {
            int carry = COLS - 1 - s;
            uint16_t tmp[COLS];
            for (int k = 0; k < carry; k++) { tmp[k] = ln[s + 1 + k]; ln[s + 1 + k] = ' ' | 0x0700; }
            doc_newline(); if (d_full) return;
            for (int k = 0; k < carry; k++) DOC[d_line * COLS + k] = tmp[k];
            d_col = carry;
        } else { doc_newline(); if (d_full) return; }
    }
    DOC[d_line * COLS + d_col++] = (uint16_t)(uint8_t)ch | (uint16_t)a << 8;
    d_trail_blank = 0;
}

static void doc_block(int blank) {
    if (d_col > 0) doc_newline();
    if (blank && !d_trail_blank) { doc_newline(); d_trail_blank = 1; }
    d_pend_space = 0;
}

static void doc_str(const char *s, uint8_t a) { while (*s) doc_putc(*s++, a); }

/* ------------------------------------------------------------------ */
/* Character mapping (Unicode -> code page 437)                        */
/* ------------------------------------------------------------------ */
static const uint8_t latin1_cp437[96] = {
    ' ', 0xAD, 0x9B, 0x9C, '?', 0x9D, '|', 0x15, '"', 'c', 0xA6, 0xAE, 0xAA, '-', 'R', '-',
    0xF8, 0xF1, 0xFD, '3', '\'', 0xE6, 0x14, 0xFA, ',', '1', 0xA7, 0xAF, 0xAC, 0xAB, '?', 0xA8,
    'A', 'A', 'A', 'A', 0x8E, 0x8F, 0x92, 0x80, 'E', 0x90, 'E', 'E', 'I', 'I', 'I', 'I',
    'D', 0xA5, 'O', 'O', 'O', 'O', 0x99, 'x', 'O', 'U', 'U', 'U', 0x9A, 'Y', 'P', 0xE1,
    0x85, 0xA0, 0x83, 'a', 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
    'd', 0xA4, 0x95, 0xA2, 0x93, 'o', 0x94, 0xF6, 'o', 0x97, 0xA3, 0x96, 0x81, 'y', 'p', 0x98,
};

/* Returns a CP437 byte, or 0 to drop the character. Sets *two for "..." style expansions. */
static int map_cp(uint32_t cp, const char **multi) {
    *multi = 0;
    if (cp == '\t' || cp == '\n' || cp == '\r') return ' ';
    if (cp < 0x20 || cp == 0x7F) return 0;
    if (cp < 0x80) return (int)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return latin1_cp437[cp - 0xA0];
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2022: case 0x25CF: return 0x07;
    case 0x2026: *multi = "..."; return 1;
    case 0x2122: *multi = "TM"; return 1;
    case 0x20AC: *multi = "EUR"; return 1;
    case 0x2192: return 0x1A;
    case 0x2190: return 0x1B;
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: return ' ';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return 0;
    }
    if (cp >= 0x0300 && cp < 0x0370) return 0;          /* combining marks */
    if (cp >= 0x1F000 || (cp >= 0x2600 && cp < 0x2800) || (cp >= 0xFE00 && cp < 0xFE10)) return 0;  /* emoji */
    return '?';
}

/* ------------------------------------------------------------------ */
/* HTML renderer                                                       */
/* ------------------------------------------------------------------ */
static int r_bold, r_head, r_link, r_pre, r_title;
static char cur_url[URL_MAX];

static uint8_t r_attr(void) { return r_link ? 0x0B : r_head ? 0x0E : r_bold ? 0x0F : 0x07; }

static void emit_char(int ch) {
    if (r_title) {
        if (ch == ' ' && (title_len == 0 || page_title[title_len - 1] == ' ')) return;
        if (title_len < COLS - 1) { page_title[title_len++] = (char)ch; page_title[title_len] = 0; }
        return;
    }
    if (ch == ' ' && !r_pre) { if (d_col > 0) d_pend_space = 1; return; }
    if (d_pend_space && d_col > 0) doc_putc(' ', 0x07);
    d_pend_space = 0;
    doc_putc((char)ch, r_attr());
}

static void emit_cp(uint32_t cp) {
    if (r_pre && cp == '\n') { if (!r_title) doc_newline(); return; }
    if (r_pre && cp == '\r') return;
    const char *m; int c = map_cp(cp, &m);
    if (m) { while (*m) emit_char(*m++); return; }
    if (c) emit_char(c);
}

static uint32_t decode_entity(const uint8_t *b, uint32_t len, uint32_t *i) {
    /* b[*i] == '&' */
    static const struct { const char *n; uint16_t cp; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
        { "copy", 0xA9 }, { "reg", 0xAE }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
        { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C },
        { "rdquo", 0x201D }, { "middot", 0xB7 }, { "bull", 0x2022 }, { "trade", 0x2122 }, { "deg", 0xB0 },
        { "times", 0xD7 }, { "eacute", 0xE9 }, { "euro", 0x20AC }, { "pound", 0xA3 }, { "rarr", 0x2192 }, { "larr", 0x2190 },
    };
    uint32_t j = *i + 1;
    char name[12]; int n = 0;
    while (j < len && n < 11 && b[j] != ';' && b[j] != '&' && b[j] != '<' && b[j] > ' ') name[n++] = (char)b[j++];
    name[n] = 0;
    if (j >= len || b[j] != ';') { (*i)++; return '&'; }
    uint32_t cp = 0;
    if (name[0] == '#') {
        int hex = name[1] == 'x' || name[1] == 'X';
        for (const char *p = name + (hex ? 2 : 1); *p; p++) {
            char c = *p;
            int d = (c >= '0' && c <= '9') ? c - '0' : (hex && (c | 0x20) >= 'a' && (c | 0x20) <= 'f') ? (c | 0x20) - 'a' + 10 : -1;
            if (d < 0) break;
            cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
        }
    } else {
        for (unsigned k = 0; k < sizeof(ents) / sizeof(ents[0]); k++) if (!strcmp(ents[k].n, name)) cp = ents[k].cp;
        if (!cp) { (*i)++; return '&'; }
    }
    *i = j + 1;
    return cp;
}

static uint32_t decode_utf8(const uint8_t *b, uint32_t len, uint32_t *i) {
    uint8_t c = b[*i];
    if (c < 0x80) { (*i)++; return c; }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    uint32_t cp = extra == 1 ? c & 0x1F : extra == 2 ? c & 0x0F : c & 0x07;
    if (!extra || *i + (uint32_t)extra >= len + 0) { (*i)++; return c >= 0xA0 ? c : '?'; }   /* treat as Latin-1 */
    for (int k = 1; k <= extra; k++) {
        uint8_t cc = b[*i + (uint32_t)k];
        if ((cc & 0xC0) != 0x80) { (*i)++; return c >= 0xA0 ? c : '?'; }
        cp = cp << 6 | (cc & 0x3F);
    }
    *i += 1u + (uint32_t)extra;
    return cp;
}

/* Find attribute value inside a tag's attribute text; decodes entities. */
static int get_attr(const uint8_t *a, uint32_t alen, const char *name, char *out, int cap) {
    uint32_t nl = (uint32_t)strlen(name);
    for (uint32_t i = 0; i + nl < alen; i++) {
        if (i && a[i - 1] != ' ' && a[i - 1] != '\t' && a[i - 1] != '\n' && a[i - 1] != '\r' && a[i - 1] != '"' && a[i - 1] != '\'') continue;
        uint32_t k = 0;
        while (k < nl && (a[i + k] | 0x20) == name[k]) k++;
        if (k < nl) continue;
        uint32_t j = i + nl;
        while (j < alen && a[j] == ' ') j++;
        if (j >= alen || a[j] != '=') continue;
        j++;
        while (j < alen && a[j] == ' ') j++;
        uint8_t q = (j < alen && (a[j] == '"' || a[j] == '\'')) ? a[j++] : 0;
        int n = 0;
        while (j < alen && (q ? a[j] != q : (a[j] != ' ' && a[j] != '>'))) {
            uint32_t cp;
            if (a[j] == '&') cp = decode_entity(a, alen, &j); else cp = a[j++];
            if (cp < 0x80 && n + 1 < cap) out[n++] = (char)cp;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

static int tag_is(const char *t, const char *const *list) { for (; *list; list++) if (!strcmp(t, *list)) return 1; return 0; }

static void handle_tag(const char *t, int closing, const uint8_t *a, uint32_t alen) {
    static const char *const blank_blocks[] = { "p", "h1", "h2", "h3", "h4", "h5", "h6", "table", "ul", "ol", "blockquote", "pre", "dl", "form", 0 };
    static const char *const blocks[] = { "div", "tr", "center", "section", "article", "header", "footer", "nav", "main", "dt", "dd", "aside", "figure", "figcaption", "address", "fieldset", 0 };

    if (!strcmp(t, "title")) { r_title = !closing; return; }
    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) { doc_block(1); r_head = !closing; return; }
    if (!strcmp(t, "b") || !strcmp(t, "strong")) { r_bold = !closing; return; }
    if (!strcmp(t, "pre")) { doc_block(1); r_pre = !closing; return; }
    if (!strcmp(t, "br")) {
        if (d_col > 0) { doc_newline(); d_trail_blank = 0; }
        else if (!d_trail_blank) { doc_newline(); d_trail_blank = 1; }
        d_pend_space = 0;
        return;
    }
    if (!strcmp(t, "hr")) {
        doc_block(0);
        for (int c = 0; c < COLS - 2; c++) doc_putc((char)0xC4, 0x08);
        doc_block(0);
        return;
    }
    if (!strcmp(t, "li")) { if (!closing) { doc_block(0); doc_str("  ", 0x07); doc_putc((char)0x07, 0x0C); doc_putc(' ', 0x07); } return; }
    if (!strcmp(t, "td") || !strcmp(t, "th")) { if (!closing && d_col > 0) d_pend_space = 1; return; }
    if (!strcmp(t, "a")) {
        if (closing) { r_link = 0; return; }
        char href[LINK_MAX];
        if (get_attr(a, alen, "href", href, sizeof(href)) && nlinks < MAX_LINKS && !r_title &&
            resolve(cur_url, href, LINKS[nlinks], LINK_MAX)) {
            /* Same target as the previous link (e.g. a search result's title, address and
               snippet): colour it as a link but keep the one number. */
            if (d_col > 0) d_pend_space = 1;           /* keep adjacent links apart */
            if (nlinks > 0 && !strcmp(LINKS[nlinks], LINKS[nlinks - 1])) { r_link = 1; return; }
            nlinks++;
            char num[12]; int n = 0; int v = nlinks;
            do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v);
            if (d_pend_space && d_col > 0) doc_putc(' ', 0x07);
            d_pend_space = 0;
            doc_putc('[', 0x06);
            while (n) doc_putc(num[--n], 0x06);
            doc_putc(']', 0x06);
            r_link = 1;
        }
        return;
    }
    if (!strcmp(t, "img")) {
        char alt[60];
        if (get_attr(a, alen, "alt", alt, sizeof(alt)) && alt[0]) {
            if (d_pend_space && d_col > 0) doc_putc(' ', 0x07);
            d_pend_space = 0;
            doc_str("[", 0x08); doc_str(alt, 0x08); doc_str("]", 0x08);
            d_pend_space = 1;
        }
        return;
    }
    if (tag_is(t, blank_blocks)) { doc_block(1); return; }
    if (tag_is(t, blocks)) { doc_block(0); return; }
}

static void render(const uint8_t *b, uint32_t len) {
    static const char *const skip_tags[] = { "script", "style", "noscript", "svg", "iframe", "select", "textarea", "template", 0 };
    doc_reset();
    r_bold = r_head = r_link = r_pre = r_title = 0;

    uint32_t i = 0;
    while (i < len && (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t')) i++;
    if (i < len && b[i] != '<') r_pre = 1;          /* plain text */

    while (i < len && !d_full) {
        uint8_t c = b[i];
        if (c == '<' && !(r_pre && i + 1 < len && b[i + 1] == ' ')) {
            if (i + 3 < len && b[i + 1] == '!' && b[i + 2] == '-' && b[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(b[i] == '-' && b[i + 1] == '-' && b[i + 2] == '>')) i++;
                i += 3;
                continue;
            }
            uint32_t j = i + 1;
            int closing = 0;
            if (j < len && b[j] == '/') { closing = 1; j++; }
            char t[16]; int n = 0;
            while (j < len && ((b[j] | 0x20) >= 'a' && (b[j] | 0x20) <= 'z' ? 1 : (b[j] >= '0' && b[j] <= '9'))) {
                if (n < 15) t[n++] = (char)(b[j] | 0x20);
                j++;
            }
            t[n] = 0;
            if (!n && !(j < len && b[j] == '!')) { emit_cp('<'); i++; continue; }
            uint32_t as = j; uint8_t q = 0;
            while (j < len && (q || b[j] != '>')) {
                if (q) { if (b[j] == q) q = 0; }
                else if (b[j] == '"' || b[j] == '\'') q = b[j];
                j++;
            }
            handle_tag(t, closing, b + as, j - as);
            i = j + 1;
            if (!closing && tag_is(t, skip_tags)) {         /* skip to </tag> */
                while (i + (uint32_t)n + 2 < len) {
                    if (b[i] == '<' && b[i + 1] == '/') {
                        int k = 0;
                        while (k < n && (b[i + 2 + (uint32_t)k] | 0x20) == t[k]) k++;
                        if (k == n) break;
                    }
                    i++;
                }
            }
            continue;
        }
        uint32_t cp = (c == '&') ? decode_entity(b, len, &i) : decode_utf8(b, len, &i);
        emit_cp(cp);
    }
    if (d_full) {
        d_full = 0; d_line++; d_col = 0; doc_clear_line(d_line);
        d_line++; d_col = 0; doc_clear_line(d_line);
        doc_str("[page too long - truncated]", 0x08);
    }
    while (title_len > 0 && page_title[title_len - 1] == ' ') page_title[--title_len] = 0;
}

/* ------------------------------------------------------------------ */
/* Viewer                                                              */
/* ------------------------------------------------------------------ */
#define V_TOP   2
#define V_ROWS  (ROWS - 1 - V_TOP)
enum { ACT_QUIT, ACT_LINK, ACT_BACK, ACT_SEARCH, ACT_RELOAD };

static int v_link;
static char v_query[64];

static void bar(int row, const char *s, uint8_t a) {
    int c = 0;
    for (; *s && c < COLS; c++) put_at(row, c, *s++, a);
    for (; c < COLS; c++) put_at(row, c, ' ', a);
}

static void draw_page(int top, int total, int status) {
    char t[COLS + 1]; t[0] = 0;
    str_cat(t, sizeof(t), " ");
    str_cat(t, sizeof(t), page_title[0] ? page_title : cur_url);
    bar(1, t, 0x1F);
    char pos[24]; int n = 0;
    int pct = total <= V_ROWS ? 100 : (top + V_ROWS >= total ? 100 : (top + V_ROWS) * 100 / total);
    if (status != 200) { pos[n++] = 'H'; pos[n++] = 'T'; pos[n++] = 'T'; pos[n++] = 'P'; pos[n++] = ' ';
                         pos[n++] = (char)('0' + status / 100); pos[n++] = (char)('0' + status / 10 % 10); pos[n++] = (char)('0' + status % 10); pos[n++] = ' '; }
    if (pct == 100) { pos[n++] = '1'; pos[n++] = '0'; pos[n++] = '0'; } else { if (pct >= 10) pos[n++] = (char)('0' + pct / 10); pos[n++] = (char)('0' + pct % 10); }
    pos[n++] = '%'; pos[n++] = ' '; pos[n] = 0;
    for (int k = 0; k < n; k++) put_at(1, COLS - n + k, pos[k], 0x1E);

    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    for (int r = 0; r < V_ROWS; r++) {
        int l = top + r;
        for (int c = 0; c < COLS; c++) vga[(V_TOP + r) * COLS + c] = l < total ? DOC[l * COLS + c] : (uint16_t)(' ' | 0x0700);
    }
}

static int view(int status) {
    int total = d_line + (d_col > 0 ? 1 : 0);
    if (total < 1) total = 1;
    int top = 0;
    char num[6]; int nn = 0;
    cursor_show(0);
    for (;;) {
        if (top > total - V_ROWS) top = total - V_ROWS;
        if (top < 0) top = 0;
        draw_page(top, total, status);
        if (nn) {
            char s[60] = " Open link #"; str_cat(s, sizeof(s), num); str_cat(s, sizeof(s), "_   (Enter = go, Esc = cancel)");
            bar(ROWS - 1, s, 0x70);
        } else {
            char s[COLS + 1] = " \x18\x19 PgUp PgDn scroll   number+Enter open link   B back   S search   Q quit";
            bar(ROWS - 1, s, 0x70);
        }
        int k = getkey();
        if (nn) {
            if (k >= '0' && k <= '9' && nn < 4) { num[nn++] = (char)k; num[nn] = 0; }
            else if (k == '\b') { num[--nn] = 0; }
            else if (k == 27) { nn = 0; }
            else if (k == '\n') {
                int v = 0; for (int i = 0; i < nn; i++) v = v * 10 + num[i] - '0';
                nn = 0;
                if (v >= 1 && v <= nlinks) { v_link = v - 1; return ACT_LINK; }
            }
            continue;
        }
        switch (k) {
        case KEY_UP: top--; break;
        case KEY_DOWN: case '\n': top++; break;
        case KEY_PGUP: top -= V_ROWS - 1; break;
        case KEY_PGDN: case ' ': top += V_ROWS - 1; break;
        case KEY_HOME: top = 0; break;
        case KEY_END: top = total; break;
        case 'b': case 'B': case KEY_LEFT: case '\b': return ACT_BACK;
        case 'r': case 'R': return ACT_RELOAD;
        case 'q': case 'Q': case 27: return ACT_QUIT;
        case 's': case 'S': case '/': {
            int n = 0; v_query[0] = 0;
            for (;;) {
                char s[COLS + 1] = " Search: "; str_cat(s, sizeof(s), v_query);
                bar(ROWS - 1, s, 0x70);
                cursor_show(1); cursor_set(ROWS - 1, 9 + n);
                int c = getkey();
                if (c == '\n' && n) { cursor_show(0); return ACT_SEARCH; }
                if (c == 27) break;
                if (c == '\b' && n) v_query[--n] = 0;
                else if (c >= 32 && c < 127 && n < (int)sizeof(v_query) - 1) { v_query[n++] = (char)c; v_query[n] = 0; }
            }
            cursor_show(0);
            break;
        }
        default:
            if (k >= '0' && k <= '9') { num[0] = (char)k; num[1] = 0; nn = 1; }
        }
    }
}

static void sleep_ticks_web(uint32_t n) { uint32_t t0 = ticks; while (ticks - t0 < n) __asm__ volatile("hlt"); }

/* ------------------------------------------------------------------ */
/* Browser loop                                                        */
/* ------------------------------------------------------------------ */
#define HIST 16
static char hist[HIST][URL_MAX];
static int hist_n;

/* Search engines, tried in order. If one fails we move on to the next for this session. */
static const char *const engines[][3] = {
    { "DuckDuckGo", "https://html.duckduckgo.com/html/?q=", "ddg" },
    { "Mojeek",     "https://www.mojeek.com/search?q=",     "mojeek" },
    { "Wiby",       "http://wiby.me/?q=",                   "wiby" },
};
#define N_ENGINES 3
static int engine;

/* DuckDuckGo sometimes answers bots with a puzzle page instead of results. */
static int looks_blocked(const uint8_t *b, uint32_t len) {
    static const char *const marks[] = { "anomaly-modal", "Unfortunately, bots use DuckDuckGo", "challenge-form", 0 };
    for (const char *const *m = marks; *m; m++) {
        uint32_t ml = (uint32_t)strlen(*m);
        for (uint32_t i = 0; i + ml <= len && i < 200000; i++) if (!memcmp(b + i, *m, ml)) return 1;
    }
    return 0;
}

void web_engine(const char *name) {
    for (int i = 0; i < N_ENGINES; i++) if (*name && !strcmp(name, engines[i][2])) { engine = i; kprintf("Search engine: %s\n", engines[i][0]); return; }
    for (int i = 0; i < N_ENGINES; i++) {
        set_color(i == engine ? 0x0A : 0x07);
        kprintf("  %c %s", i == engine ? '*' : ' ', engines[i][2]);
        for (int n = (int)strlen(engines[i][2]); n < 8; n++) putc(' ');
        kprintf("%s\n", engines[i][0]);
    }
    reset_color();
    puts("usage: engine <ddg|mojeek|wiby>\n");
}
static char last_query[64];

static void search_url(const char *q, char *out, int cap) {
    char enc[200];
    urlencode(q, enc, sizeof(enc), 1);
    strncpy0(last_query, q, sizeof(last_query));
    out[0] = 0;
    str_cat(out, cap, engines[engine][1]);
    str_cat(out, cap, enc);
}
static int is_search_url(const char *u) { return !strncmp(u, engines[engine][1], strlen(engines[engine][1])); }

static void browse(const char *start) {
    static char url[URL_MAX];
    strncpy0(url, start, sizeof(url));
    hist_n = 0;
    int push = 1;

    for (;;) {
        clear_screen();
        set_color(0x08); kprintf("%s\n", url); reset_color();
        set_color(0x08); puts("(Esc cancels)\n"); reset_color();
        uint8_t *body; uint32_t len;
        int st = http_get(url, &body, &len, cur_url, sizeof(cur_url));
        int search_failed = is_search_url(url) && !http_cancelled && (st != 200 || looks_blocked(body, len));
        if (search_failed && engine + 1 < N_ENGINES) {
            if (st) { set_color(0x0C); kprintf("%s didn't return results.\n", engines[engine][0]); reset_color(); }
            engine++;
            set_color(0x0E); kprintf("\nTrying the next search engine (%s)...\n", engines[engine][0]); reset_color();
            sleep_ticks_web(150);
            search_url(last_query, url, sizeof(url));
            continue;
        }
        if (!st) {
            puts("\nPress any key...");
            getkey();
            if (hist_n > 0) { strncpy0(url, hist[hist_n - 1], sizeof(url)); push = 0; continue; }
            break;
        }
        if (push) {
            if (hist_n == HIST) { for (int i = 1; i < HIST; i++) strncpy0(hist[i - 1], hist[i], URL_MAX); hist_n--; }
            strncpy0(hist[hist_n++], cur_url, URL_MAX);
        }
        render(body, len);
        int act = view(st);
        push = 1;
        if (act == ACT_QUIT) break;
        if (act == ACT_LINK) strncpy0(url, LINKS[v_link], sizeof(url));
        else if (act == ACT_SEARCH) search_url(v_query, url, sizeof(url));
        else if (act == ACT_RELOAD) { push = 0; strncpy0(url, cur_url, sizeof(url)); }
        else if (act == ACT_BACK) {
            if (hist_n <= 1) break;
            hist_n--;
            strncpy0(url, hist[hist_n - 1], sizeof(url));
            push = 0;
        }
    }
    cursor_show(1);
    clear_screen();
}

void web_search(const char *query) {
    if (!*query) { puts("usage: search <words>    e.g. search how do volcanoes work\n"); return; }
    if (!net_up(1)) return;
    char url[URL_MAX];
    search_url(query, url, sizeof(url));
    browse(url);
}

void web_open(const char *addr) {
    if (!*addr) { puts("usage: open <web address>    e.g. open example.com\n"); return; }
    if (!net_up(1)) return;
    char url[URL_MAX];
    if (!strncmp(addr, "https://", 8) || !strncmp(addr, "http://", 7)) strncpy0(url, addr, sizeof(url));
    else { url[0] = 0; str_cat(url, sizeof(url), "https://"); str_cat(url, sizeof(url), addr); }
    browse(url);
}
