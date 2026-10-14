/*
 * EmberOS - a tiny 32-bit hobby OS.
 * VGA text console, PIT timer, PS/2 keyboard, RTC clock,
 * a RAM filesystem, a shell, and Snake.
 */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Tiny libc                                                           */
/* ------------------------------------------------------------------ */
static inline void io_wait(void) { outb(0x80, 0); }

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (uint8_t)*a - (uint8_t)*b; if (!*a) return 0; }
    return 0;
}
void strncpy0(char *d, const char *s, size_t n) { size_t i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* ------------------------------------------------------------------ */
/* VGA text console (row 0 is the status bar)                          */
/* ------------------------------------------------------------------ */
#define VGA   ((volatile uint16_t *)0xB8000)

int cur_row = TOP, cur_col = 0;
uint8_t attr = 0x07;
uint8_t theme = 0x07;

void cursor_set(int r, int c) {
    uint16_t pos = (uint16_t)(r * COLS + c);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, pos >> 8);
}
static void cursor_update(void) { cursor_set(cur_row, cur_col); }
void cursor_show(int on) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, on ? 0x0D : 0x20);
    if (on) { outb(0x3D4, 0x0B); outb(0x3D5, 0x0E); }
}
void put_at(int r, int c, char ch, uint8_t a) { VGA[r * COLS + c] = (uint16_t)(uint8_t)ch | (uint16_t)a << 8; }

void clear_screen(void) {
    for (int i = TOP * COLS; i < ROWS * COLS; i++) VGA[i] = ' ' | (uint16_t)theme << 8;
    cur_row = TOP; cur_col = 0;
    cursor_update();
}
static void scroll_if_needed(void) {
    if (cur_row < ROWS) return;
    for (int i = TOP * COLS; i < (ROWS - 1) * COLS; i++) VGA[i] = VGA[i + COLS];
    for (int i = (ROWS - 1) * COLS; i < ROWS * COLS; i++) VGA[i] = ' ' | (uint16_t)theme << 8;
    cur_row = ROWS - 1;
}
void putc(char c) {
    if (c == '\n') { cur_col = 0; cur_row++; }
    else if (c == '\b') { if (cur_col > 0) { cur_col--; put_at(cur_row, cur_col, ' ', attr); } }
    else if (c == '\t') { cur_col = (cur_col + 4) & ~3; }
    else { put_at(cur_row, cur_col, c, attr); cur_col++; }
    if (cur_col >= COLS) { cur_col = 0; cur_row++; }
    scroll_if_needed();
    cursor_update();
}
void puts(const char *s) { while (*s) putc(*s++); }
void set_color(uint8_t fg) { attr = (theme & 0xF0) | (fg & 0x0F); }
void reset_color(void) { attr = theme; }

static void put_uint(uint32_t v, int base, int width, char pad) {
    char buf[12]; int n = 0;
    do { uint32_t d = v % (uint32_t)base; buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (uint32_t)base; } while (v);
    while (n < width) buf[n++] = pad;
    while (n) putc(buf[--n]);
}
static void put_int(int32_t v) {
    if (v < 0) { putc('-'); put_uint((uint32_t)0 - (uint32_t)v, 10, 0, ' '); }
    else put_uint((uint32_t)v, 10, 0, ' ');
}

/* kprintf: %s %c %d %u %x %02d-style zero padding, %% */
void kprintf(const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putc(*fmt); continue; }
        fmt++;
        char pad = ' '; int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *); puts(s ? s : "(null)"); break; }
        case 'c': putc((char)__builtin_va_arg(ap, int)); break;
        case 'd': put_int(__builtin_va_arg(ap, int32_t)); break;
        case 'u': put_uint(__builtin_va_arg(ap, uint32_t), 10, width, pad); break;
        case 'x': put_uint(__builtin_va_arg(ap, uint32_t), 16, width, pad); break;
        case '%': putc('%'); break;
        default: putc('%'); putc(*fmt); break;
        }
    }
    __builtin_va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Interrupts: IDT, PIC, PIT                                           */
/* ------------------------------------------------------------------ */
struct idt_entry { uint16_t off_lo, sel; uint8_t zero, flags; uint16_t off_hi; } __attribute__((packed));
struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct idt_entry idt[256];
extern void irq0_stub(void), irq1_stub(void), isr_ignore(void);
extern uint32_t exc_table[32];

static void idt_set(int n, uint32_t handler) {
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].zero = 0;
    idt[n].flags = 0x8E;
    idt[n].off_hi = handler >> 16;
}

volatile uint32_t ticks;

void irq0_handler(void) { ticks++; outb(0x20, 0x20); }

#define KBUF 64
static volatile uint8_t kbuf[KBUF];
static volatile uint32_t kb_head, kb_tail;

void irq1_handler(void) {
    uint8_t sc = inb(0x60);
    uint32_t next = (kb_head + 1) % KBUF;
    if (next != kb_tail) { kbuf[kb_head] = sc; kb_head = next; }
    outb(0x20, 0x20);
}

static const char *exc_names[] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "x87 FPU error",
    "Alignment check", "Machine check", "SIMD exception"
};

void exception_handler(uint32_t vec) {
    theme = 0x4F; attr = 0x4F;
    clear_screen();
    kprintf("\n  *** KERNEL PANIC ***\n\n  CPU exception %u: %s\n\n  The system has been halted. Reset the VM to restart.\n",
            vec, vec < 20 ? exc_names[vec] : "Reserved");
    for (;;) __asm__ volatile("cli; hlt");
}

static void interrupts_init(void) {
    for (int i = 0; i < 256; i++) idt_set(i, (uint32_t)isr_ignore);
    for (int i = 0; i < 32; i++) idt_set(i, exc_table[i]);
    idt_set(0x20, (uint32_t)irq0_stub);
    idt_set(0x21, (uint32_t)irq1_stub);
    struct idt_ptr p = { sizeof(idt) - 1, (uint32_t)idt };
    __asm__ volatile("lidt %0" ::"m"(p));

    /* Remap PIC: master -> 0x20, slave -> 0x28. Only IRQ0 and IRQ1 unmasked. */
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFC); outb(0xA1, 0xFF);

    /* PIT channel 0 at 100 Hz */
    uint32_t div = 1193182 / HZ;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);

    while (inb(0x64) & 1) inb(0x60);      /* flush keyboard controller */
    __asm__ volatile("sti");
}

/* ------------------------------------------------------------------ */
/* RTC                                                                 */
/* ------------------------------------------------------------------ */
struct rtc_time { int sec, min, hour, day, mon, year; };

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static int bcd(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

static void rtc_read(struct rtc_time *t) {
    uint8_t s, m, h, d, mo, y, s2;
    do {
        while (cmos(0x0A) & 0x80) {}
        s = cmos(0x00); m = cmos(0x02); h = cmos(0x04);
        d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
        s2 = cmos(0x00);
    } while (s != s2);
    uint8_t b = cmos(0x0B);
    int pm = h & 0x80; h &= 0x7F;
    if (!(b & 0x04)) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(b & 0x02)) { if (h == 12) h = 0; if (pm) h += 12; }
    t->sec = s; t->min = m; t->hour = h; t->day = d; t->mon = mo; t->year = 2000 + y;
}

void rtc_now(struct rtc_time_s *o) {
    struct rtc_time t; rtc_read(&t);
    o->sec = t.sec; o->min = t.min; o->hour = t.hour; o->day = t.day; o->mon = t.mon; o->year = t.year;
}

static const char *months[] = { "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */
static void bar_text(int *c, const char *s, uint8_t a) { while (*s && *c < COLS) put_at(0, (*c)++, *s++, a); }
static void bar_num2(int *c, int v, uint8_t a) { put_at(0, (*c)++, (char)('0' + v / 10 % 10), a); put_at(0, (*c)++, (char)('0' + v % 10), a); }

static uint32_t last_status_sec = 0xFFFFFFFF;
void status_draw(void) {
    const uint8_t a = 0x70, hi = 0x4F;
    uint32_t up = ticks / HZ;
    if (up == last_status_sec) return;
    last_status_sec = up;
    int c = 0;
    bar_text(&c, " \x0F " OS_NAME " ", hi);
    bar_text(&c, "  up ", a);
    bar_num2(&c, (int)(up / 3600), a); bar_text(&c, ":", a);
    bar_num2(&c, (int)(up / 60 % 60), a); bar_text(&c, ":", a);
    bar_num2(&c, (int)(up % 60), a);
    while (c < COLS - 22) put_at(0, c++, ' ', a);
    struct rtc_time t; rtc_read(&t);
    bar_num2(&c, t.day, a); bar_text(&c, " ", a);
    bar_text(&c, months[(t.mon >= 1 && t.mon <= 12) ? t.mon : 0], a); bar_text(&c, " ", a);
    bar_num2(&c, t.year / 100, a); bar_num2(&c, t.year % 100, a); bar_text(&c, "  ", a);
    bar_num2(&c, t.hour, a); bar_text(&c, ":", a); bar_num2(&c, t.min, a);
    while (c < COLS) put_at(0, c++, ' ', a);
}

/* ------------------------------------------------------------------ */
/* Keyboard (scancode set 1)                                           */
/* ------------------------------------------------------------------ */

static const char sc_lo[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '
};
static const char sc_hi[0x3A] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' '
};

static int kb_ext, kb_shift, kb_caps, kb_ctrl;

/* Returns a key code, or -1 if no key is waiting. */
int kb_poll(void) {
    while (kb_tail != kb_head) {
        uint8_t sc = kbuf[kb_tail];
        kb_tail = (kb_tail + 1) % KBUF;
        if (sc == 0xE0) { kb_ext = 1; continue; }
        if (sc & 0x80) {
            uint8_t code = sc & 0x7F;
            if (code == 0x2A || code == 0x36) kb_shift = 0;
            if (code == 0x1D) kb_ctrl = 0;
            kb_ext = 0;
            continue;
        }
        if (kb_ext) {
            kb_ext = 0;
            switch (sc) {
            case 0x48: return KEY_UP;   case 0x50: return KEY_DOWN;
            case 0x4B: return KEY_LEFT; case 0x4D: return KEY_RIGHT;
            case 0x47: return KEY_HOME; case 0x4F: return KEY_END;
            case 0x53: return KEY_DEL;  case 0x1D: kb_ctrl = 1; continue;
            case 0x49: return KEY_PGUP; case 0x51: return KEY_PGDN;
            case 0x1C: return '\n';     case 0x35: return '/';
            default: continue;
            }
        }
        if (sc == 0x2A || sc == 0x36) { kb_shift = 1; continue; }
        if (sc == 0x1D) { kb_ctrl = 1; continue; }
        if (sc == 0x3A) { kb_caps ^= 1; continue; }
        if (sc >= 0x3A) continue;
        char c = kb_shift ? sc_hi[sc] : sc_lo[sc];
        if (!c) continue;
        if (kb_caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
        if (kb_ctrl && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) return (c | 0x20) - 'a' + 1;
        return (uint8_t)c;
    }
    return -1;
}

/* True if Esc was pressed; other queued keys are left alone. */
int kb_esc_pressed(void) {
    for (uint32_t i = kb_tail; i != kb_head; i = (i + 1) % KBUF)
        if (kbuf[i] == 0x01) { kb_tail = kb_head; return 1; }
    return 0;
}

int getkey(void) {
    for (;;) {
        int k = kb_poll();
        if (k >= 0) return k;
        status_draw();
        __asm__ volatile("hlt");
    }
}

static void sleep_ticks(uint32_t n) {
    uint32_t end = ticks + n;
    while ((int32_t)(ticks - end) < 0) { status_draw(); __asm__ volatile("hlt"); }
}

/* ------------------------------------------------------------------ */
/* RAM filesystem                                                      */
/* ------------------------------------------------------------------ */
#define MAX_FILES 16
#define NAME_LEN  24
#define FILE_SIZE 1024

struct file { int used; char name[NAME_LEN]; char data[FILE_SIZE]; int len; };
static struct file files[MAX_FILES];

static struct file *fs_find(const char *name) {
    for (int i = 0; i < MAX_FILES; i++) if (files[i].used && !strcmp(files[i].name, name)) return &files[i];
    return 0;
}
static struct file *fs_create(const char *name) {
    struct file *f = fs_find(name);
    if (f) return f;
    for (int i = 0; i < MAX_FILES; i++) if (!files[i].used) {
        files[i].used = 1; strncpy0(files[i].name, name, NAME_LEN); files[i].len = 0; files[i].data[0] = 0;
        return &files[i];
    }
    return 0;
}
static void fs_write(struct file *f, const char *text, int append) {
    if (!append) f->len = 0;
    while (*text && f->len < FILE_SIZE - 1) f->data[f->len++] = *text++;
    f->data[f->len] = 0;
}
static void fs_init(void) {
    fs_write(fs_create("readme.txt"),
        "Welcome to EmberOS!\n"
        "This is a tiny 32-bit operating system running straight on the\n"
        "(virtual) hardware - no Linux, no DOS, just a bootloader and a kernel.\n"
        "Files live in RAM and vanish when you power off.\n"
        "Type 'help' to see what you can do.\n", 0);
    fs_write(fs_create("todo.txt"), "- play snake\n- write a file with 'write'\n", 0);
}

/* ------------------------------------------------------------------ */
/* Calculator: + - * / % and parentheses, 32-bit integers               */
/* ------------------------------------------------------------------ */
static const char *cp;
static int cerr;
static int32_t c_expr(void);
static void c_ws(void) { while (*cp == ' ') cp++; }
static int32_t c_factor(void) {
    c_ws();
    if (*cp == '-') { cp++; return (int32_t)(0u - (uint32_t)c_factor()); }
    if (*cp == '+') { cp++; return c_factor(); }
    if (*cp == '(') { cp++; int32_t v = c_expr(); c_ws(); if (*cp == ')') cp++; else cerr = 1; return v; }
    if (*cp >= '0' && *cp <= '9') { uint32_t v = 0; while (*cp >= '0' && *cp <= '9') v = v * 10 + (uint32_t)(*cp++ - '0'); return (int32_t)v; }
    cerr = 1; return 0;
}
static int32_t c_term(void) {
    int32_t v = c_factor();
    for (;;) {
        c_ws();
        char op = *cp;
        if (op != '*' && op != '/' && op != '%') return v;
        cp++;
        int32_t r = c_factor();
        if (op == '*') v = (int32_t)((uint32_t)v * (uint32_t)r);
        else if (r == 0) { cerr = 2; return 0; }
        else if (r == -1) v = op == '/' ? (int32_t)(0u - (uint32_t)v) : 0;
        else v = op == '/' ? v / r : v % r;
    }
}
static int32_t c_expr(void) {
    int32_t v = c_term();
    for (;;) {
        c_ws();
        if (*cp == '+') { cp++; v = (int32_t)((uint32_t)v + (uint32_t)c_term()); }
        else if (*cp == '-') { cp++; v = (int32_t)((uint32_t)v - (uint32_t)c_term()); }
        else return v;
    }
}

/* ------------------------------------------------------------------ */
/* Snake                                                               */
/* ------------------------------------------------------------------ */
static uint32_t rng_state = 1;
static uint32_t rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 16; }

#define SN_MAX 400
static void snake(void) {
    const int L = 1, R = COLS - 2, T = TOP + 1, B = ROWS - 2;   /* playfield, inclusive */
    int sx[SN_MAX], sy[SN_MAX];
    int len, dx, dy, fx, fy, score, best = 0, alive;
    rng_state ^= ticks;
    cursor_show(0);

again:
    for (int i = TOP * COLS; i < ROWS * COLS; i++) VGA[i] = ' ' | 0x0000;
    for (int c = L - 1; c <= R + 1; c++) { put_at(T - 1, c, (char)0xCD, 0x06); put_at(B + 1, c, (char)0xCD, 0x06); }
    for (int r = T - 1; r <= B + 1; r++) { put_at(r, L - 1, (char)0xBA, 0x06); put_at(r, R + 1, (char)0xBA, 0x06); }
    put_at(T - 1, L - 1, (char)0xC9, 0x06); put_at(T - 1, R + 1, (char)0xBB, 0x06);
    put_at(B + 1, L - 1, (char)0xC8, 0x06); put_at(B + 1, R + 1, (char)0xBC, 0x06);

    len = 4; dx = 1; dy = 0; score = 0; alive = 1;
    for (int i = 0; i < len; i++) { sx[i] = 20 - i; sy[i] = 12; }
    do { fx = L + (int)(rnd() % (uint32_t)(R - L + 1)); fy = T + (int)(rnd() % (uint32_t)(B - T + 1)); } while (fy == 12);

    while (alive) {
        /* draw */
        put_at(fy, fx, (char)0x04, 0x0C);
        for (int i = 1; i < len; i++) put_at(sy[i], sx[i], (char)0xDB, 0x02);
        put_at(sy[0], sx[0], (char)0x02, 0x0A);
        int c = 3;
        const char *hud = " SNAKE  arrows/WASD to steer, Q to quit   score ";
        for (const char *p = hud; *p; p++) put_at(T - 1, c++, *p, 0x0E);
        char nb[8]; int n = 0; int s = score; do { nb[n++] = (char)('0' + s % 10); s /= 10; } while (s);
        while (n) put_at(T - 1, c++, nb[--n], 0x0F);
        put_at(T - 1, c++, ' ', 0x0E);

        uint32_t delay = score > 30 ? 4 : (uint32_t)(10 - score / 5);
        uint32_t end = ticks + delay;
        int ndx = dx, ndy = dy;
        while ((int32_t)(ticks - end) < 0) {
            int k;
            while ((k = kb_poll()) >= 0) {
                if (k == 'q' || k == 'Q' || k == 27) { cursor_show(1); clear_screen(); return; }
                if ((k == KEY_UP || k == 'w' || k == 'W') && dy != 1)    { ndx = 0; ndy = -1; }
                if ((k == KEY_DOWN || k == 's' || k == 'S') && dy != -1) { ndx = 0; ndy = 1; }
                if ((k == KEY_LEFT || k == 'a' || k == 'A') && dx != 1)  { ndx = -1; ndy = 0; }
                if ((k == KEY_RIGHT || k == 'd' || k == 'D') && dx != -1){ ndx = 1; ndy = 0; }
            }
            status_draw();
            __asm__ volatile("hlt");
        }
        dx = ndx; dy = ndy;

        int nx = sx[0] + dx, ny = sy[0] + dy;
        if (nx < L || nx > R || ny < T || ny > B) alive = 0;
        for (int i = 0; i < len - 1 && alive; i++) if (sx[i] == nx && sy[i] == ny) alive = 0;
        if (!alive) break;

        int grow = (nx == fx && ny == fy);
        if (!grow) put_at(sy[len - 1], sx[len - 1], ' ', 0x00);
        else if (len < SN_MAX) len++;
        for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
        sx[0] = nx; sy[0] = ny;
        if (grow) {
            score++;
            int ok;
            do {
                fx = L + (int)(rnd() % (uint32_t)(R - L + 1)); fy = T + (int)(rnd() % (uint32_t)(B - T + 1));
                ok = 1; for (int i = 0; i < len; i++) if (sx[i] == fx && sy[i] == fy) ok = 0;
            } while (!ok);
        }
    }

    if (score > best) best = score;
    const char *msg1 = "  GAME OVER  ";
    const char *msg2 = "  R = restart   Q = quit  ";
    int r = 11;
    for (int i = 0; msg1[i]; i++) put_at(r, 40 - 6 + i, msg1[i], 0x4F);
    for (int i = 0; msg2[i]; i++) put_at(r + 2, 40 - 13 + i, msg2[i], 0x70);
    for (;;) {
        int k = getkey();
        if (k == 'r' || k == 'R' || k == '\n') goto again;
        if (k == 'q' || k == 'Q' || k == 27) break;
    }
    (void)best;
    cursor_show(1);
    clear_screen();
}

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */
static void reboot(void) {
    puts("Rebooting...\n");
    sleep_ticks(30);
    __asm__ volatile("cli");
    while (inb(0x64) & 2) {}
    outb(0x64, 0xFE);                      /* pulse CPU reset line */
    struct idt_ptr z = { 0, 0 };           /* fallback: triple fault */
    __asm__ volatile("lidt %0; int $3" ::"m"(z));
    for (;;) __asm__ volatile("hlt");
}

static void shutdown(void) {
    puts("Shutting down...\n");
    sleep_ticks(30);
    outw(0x4004, 0x3400);                  /* VirtualBox */
    outw(0x604, 0x2000);                   /* QEMU */
    outw(0xB004, 0x2000);                  /* Bochs / older QEMU */
    __asm__ volatile("cli");
    theme = 0x0E; attr = 0x0E; clear_screen();
    cur_row = 12; cur_col = 18;
    puts("It is now safe to turn off your computer.");
    cursor_show(0);
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* System info                                                         */
/* ------------------------------------------------------------------ */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void cmd_info(void) {
    static const char *logo[] = {
        "      (     ",
        "     ) )    ",
        "    ( ( (   ",
        "   ) ) ) )  ",
        "  .-------. ",
        "  | EMBER | ",
        "  '-------' ",
    };
    uint32_t a, b, c, d;
    char vendor[13]; char brand[49];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
    brand[0] = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *w = (uint32_t *)brand;
        for (uint32_t l = 0; l < 3; l++) cpuid(0x80000002 + l, &w[l * 4], &w[l * 4 + 1], &w[l * 4 + 2], &w[l * 4 + 3]);
        brand[48] = 0;
    }
    const char *bp = brand; while (*bp == ' ') bp++;
    uint32_t up = ticks / HZ;
    int used = 0; for (int i = 0; i < MAX_FILES; i++) used += files[i].used;

    for (int i = 0; i < 7; i++) {
        set_color(i < 4 ? (i % 2 ? 0x0C : 0x0E) : 0x06);
        puts(logo[i]);
        set_color(0x0E);
        switch (i) {
        case 0: kprintf("%s %s", OS_NAME, OS_VERSION); break;
        case 1: reset_color(); puts("---------------"); break;
        case 2: puts("Kernel: "); reset_color(); puts("32-bit protected mode, x86"); break;
        case 3: puts("CPU:    "); reset_color(); puts(*bp ? bp : vendor); break;
        case 4: puts("Uptime: "); reset_color(); kprintf("%uh %um %us", up / 3600, up / 60 % 60, up % 60); break;
        case 5: puts("Files:  "); reset_color(); kprintf("%u / %u in RAM", (uint32_t)used, (uint32_t)MAX_FILES); break;
        case 6: puts("Shell:  "); reset_color(); puts("ember-sh"); break;
        }
        putc('\n');
    }
    reset_color();
    puts("            ");
    for (int i = 0; i < 16; i++) { attr = (uint8_t)(i << 4); puts("  "); }
    reset_color();
    putc('\n');
}

/* ------------------------------------------------------------------ */
/* Shell                                                               */
/* ------------------------------------------------------------------ */
#define LINE_MAX 76
#define HIST 8
static char history[HIST][LINE_MAX];
static int hist_count;

static void readline(char *buf) {
    int len = 0, hpos = hist_count;
    buf[0] = 0;
    for (;;) {
        int k = getkey();
        if (k == '\n') { putc('\n'); break; }
        if (k == '\b') { if (len) { len--; putc('\b'); } continue; }
        if (k == 3) { puts("^C\n"); len = 0; break; }                 /* Ctrl+C */
        if (k == 12) {                                               /* Ctrl+L */
            buf[len] = 0; clear_screen();
            set_color(0x0A); puts("ember"); reset_color(); puts("> "); puts(buf);
            continue;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            if (k == KEY_UP && hpos > 0 && hpos > hist_count - HIST) hpos--;
            else if (k == KEY_DOWN && hpos < hist_count) hpos++;
            else continue;
            while (len) { len--; putc('\b'); }
            if (hpos < hist_count) {
                const char *h = history[hpos % HIST];
                while (h[len]) { buf[len] = h[len]; putc(h[len]); len++; }
            }
            continue;
        }
        if (k >= 32 && k < 127 && len < LINE_MAX - 1) { buf[len++] = (char)k; putc((char)k); }
    }
    buf[len] = 0;
    if (len) { strncpy0(history[hist_count % HIST], buf, LINE_MAX); hist_count++; }
}

/* Split "cmd rest" in-place: returns cmd, sets *rest. */
static char *split(char *s, char **rest) {
    while (*s == ' ') s++;
    char *p = s;
    while (*p && *p != ' ') p++;
    if (*p) { *p++ = 0; while (*p == ' ') p++; }
    *rest = p;
    return s;
}

static void cmd_help(void) {
    static const char *cmds[][2] = {
        { "help",              "show this list" },
        { "info",              "system information" },
        { "clear",             "clear the screen (or Ctrl+L)" },
        { "echo <text>",       "print text" },
        { "time",              "show date and time" },
        { "uptime",            "time since boot" },
        { "calc <expr>",       "integer calculator, e.g. calc (3+4)*12" },
        { "color [fg] [bg]",   "change text colours (0-15)" },
        { "ls",                "list files" },
        { "cat <file>",        "show a file" },
        { "write <file> <txt>","create/overwrite a file" },
        { "append <file> <txt>","add a line to a file" },
        { "rm <file>",         "delete a file" },
        { "search <words>",    "search the web (DuckDuckGo)" },
        { "open <address>",    "open a web page, e.g. open wikipedia.org" },
        { "engine [name]",     "pick the search engine: ddg, mojeek, wiby" },
        { "net",               "network status (net dns <ip> to set DNS)" },
        { "nettest",           "check which websites your connection can reach" },
        { "snake",             "play Snake" },
        { "reboot",            "restart the machine" },
        { "shutdown",          "power off" },
    };
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        set_color(0x0B); puts("  "); puts(cmds[i][0]);
        for (int n = (int)strlen(cmds[i][0]); n < 22; n++) putc(' ');
        reset_color(); puts(cmds[i][1]); putc('\n');
    }
    set_color(0x08); puts("  Up/Down = command history, Ctrl+C = cancel line\n"); reset_color();
}

static void cmd_color(char *args) {
    static const char *names[16] = { "black", "blue", "green", "cyan", "red", "magenta", "brown", "lightgray",
                                     "darkgray", "lightblue", "lightgreen", "lightcyan", "lightred", "pink", "yellow", "white" };
    char *rest; char *a = split(args, &rest); char *b = split(rest, &rest);
    if (!*a) {
        for (int i = 0; i < 16; i++) {
            attr = (uint8_t)((theme & 0xF0) | i); if (i == (theme >> 4)) attr = (uint8_t)(i | 0x70);
            kprintf("  %u%s %s\n", (uint32_t)i, i < 10 ? " " : "", names[i]);
        }
        reset_color(); puts("usage: color <fg> [bg]\n");
        return;
    }
    int fg = 0, bg = theme >> 4;
    for (char *p = a; *p; p++) { if (*p < '0' || *p > '9') { puts("color: numbers 0-15 only\n"); return; } fg = fg * 10 + *p - '0'; }
    if (*b) { bg = 0; for (char *p = b; *p; p++) { if (*p < '0' || *p > '9') { puts("color: numbers 0-15 only\n"); return; } bg = bg * 10 + *p - '0'; } }
    if (fg > 15 || bg > 15) { puts("color: numbers 0-15 only\n"); return; }
    if (fg == bg) { puts("color: that would be invisible!\n"); return; }
    uint8_t old = theme;
    theme = (uint8_t)((bg << 4) | fg); attr = theme;
    if ((old >> 4) != bg) clear_screen();      /* repaint background */
}

static void run(char *line) {
    char *args; char *cmd = split(line, &args);
    if (!*cmd) return;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
    else if (!strcmp(cmd, "clear") || !strcmp(cmd, "cls")) clear_screen();
    else if (!strcmp(cmd, "echo")) { puts(args); putc('\n'); }
    else if (!strcmp(cmd, "info") || !strcmp(cmd, "about")) cmd_info();
    else if (!strcmp(cmd, "time") || !strcmp(cmd, "date")) {
        struct rtc_time t; rtc_read(&t);
        kprintf("%u %s %u  %02u:%02u:%02u\n", (uint32_t)t.day, months[(t.mon >= 1 && t.mon <= 12) ? t.mon : 0],
                (uint32_t)t.year, (uint32_t)t.hour, (uint32_t)t.min, (uint32_t)t.sec);
    }
    else if (!strcmp(cmd, "uptime")) {
        uint32_t up = ticks / HZ;
        kprintf("up %u hours, %u minutes, %u seconds (%u ticks)\n", up / 3600, up / 60 % 60, up % 60, (uint32_t)ticks);
    }
    else if (!strcmp(cmd, "calc")) {
        if (!*args) { puts("usage: calc <expression>   e.g. calc 7*(2+4)\n"); return; }
        cp = args; cerr = 0;
        int32_t v = c_expr(); c_ws();
        if (*cp && !cerr) cerr = 1;
        if (cerr == 2) puts("calc: division by zero\n");
        else if (cerr) puts("calc: syntax error\n");
        else { set_color(0x0E); puts("= "); put_int(v); reset_color(); putc('\n'); }
    }
    else if (!strcmp(cmd, "color")) cmd_color(args);
    else if (!strcmp(cmd, "ls") || !strcmp(cmd, "dir")) {
        int any = 0;
        for (int i = 0; i < MAX_FILES; i++) if (files[i].used) {
            any = 1; set_color(0x0B); puts("  "); puts(files[i].name);
            for (int n = (int)strlen(files[i].name); n < NAME_LEN; n++) putc(' ');
            reset_color(); kprintf("%u bytes\n", (uint32_t)files[i].len);
        }
        if (!any) puts("(no files)\n");
    }
    else if (!strcmp(cmd, "cat") || !strcmp(cmd, "type")) {
        char *rest; char *name = split(args, &rest);
        struct file *f = *name ? fs_find(name) : 0;
        if (!*name) puts("usage: cat <file>\n");
        else if (!f) kprintf("cat: %s: no such file\n", name);
        else { puts(f->data); if (f->len && f->data[f->len - 1] != '\n') putc('\n'); }
    }
    else if (!strcmp(cmd, "write") || !strcmp(cmd, "append")) {
        char *text; char *name = split(args, &text);
        if (!*name) { kprintf("usage: %s <file> <text>\n", cmd); return; }
        struct file *f = fs_create(name);
        if (!f) { puts("write: filesystem full\n"); return; }
        fs_write(f, text, cmd[0] == 'a');
        fs_write(f, "\n", 1);
        kprintf("%s: %u bytes\n", f->name, (uint32_t)f->len);
    }
    else if (!strcmp(cmd, "rm") || !strcmp(cmd, "del")) {
        char *rest; char *name = split(args, &rest);
        struct file *f = *name ? fs_find(name) : 0;
        if (!*name) puts("usage: rm <file>\n");
        else if (!f) kprintf("rm: %s: no such file\n", name);
        else f->used = 0;
    }
    else if (!strcmp(cmd, "search") || !strcmp(cmd, "google") || !strcmp(cmd, "find")) web_search(args);
    else if (!strcmp(cmd, "open") || !strcmp(cmd, "browse") || !strcmp(cmd, "web")) web_open(args);
    else if (!strcmp(cmd, "net") || !strcmp(cmd, "ifconfig") || !strcmp(cmd, "ipconfig")) {
        char *rest; char *sub = split(args, &rest);
        if (!strcmp(sub, "dns")) net_set_dns(rest); else if (!strcmp(sub, "test")) net_test(); else net_status();
    }
    else if (!strcmp(cmd, "nettest")) net_test();
    else if (!strcmp(cmd, "engine")) web_engine(args);
    else if (!strcmp(cmd, "snake")) snake();
    else if (!strcmp(cmd, "reboot") || !strcmp(cmd, "restart")) reboot();
    else if (!strcmp(cmd, "shutdown") || !strcmp(cmd, "halt") || !strcmp(cmd, "poweroff")) shutdown();
    else { set_color(0x0C); kprintf("%s: command not found", cmd); reset_color(); puts(" - try 'help'\n"); }
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */
void kmain(void) {
    theme = 0x07; attr = theme;
    cursor_show(1);
    clear_screen();
    interrupts_init();
    fs_init();
    status_draw();

    set_color(0x0C); puts("  ______           _                  ____   _____\n");
    set_color(0x0C); puts(" |  ____|         | |                / __ \\ / ____|\n");
    set_color(0x06); puts(" | |__   _ __ ___ | |__   ___ _ __  | |  | | (___\n");
    set_color(0x06); puts(" |  __| | '_ ` _ \\| '_ \\ / _ \\ '__| | |  | |\\___ \\\n");
    set_color(0x0E); puts(" | |____| | | | | | |_) |  __/ |    | |__| |____) |\n");
    set_color(0x0E); puts(" |______|_| |_| |_|_.__/ \\___|_|     \\____/|_____/\n\n");
    reset_color();
    kprintf(" Welcome to %s %s. Type ", OS_NAME, OS_VERSION);
    set_color(0x0B); puts("help"); reset_color(); puts(" for commands.\n\n");

    char line[LINE_MAX];
    for (;;) {
        set_color(0x0A); puts("ember"); reset_color(); puts("> ");
        readline(line);
        run(line);
    }
}
