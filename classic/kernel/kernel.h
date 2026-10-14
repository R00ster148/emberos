/* Shared declarations between kernel.c, net.c and web.c */
#ifndef KERNEL_H
#define KERNEL_H
#include <stdint.h>
#include <stddef.h>

#define OS_NAME    "EmberOS"
#define OS_VERSION "2.0"

#define COLS 80
#define ROWS 25
#define TOP  1
#define HZ   100

/* Memory above 1 MB used for big buffers (no allocator needed). */
#define MEM_PAGE    0x00100000u   /* raw HTTP response, 1 MB      */
#define MEM_PAGE_SZ 0x00100000u
#define MEM_DOC     0x00200000u   /* rendered text, 80x9000 cells  */
#define DOC_LINES   9000
#define MEM_LINKS   0x00380000u   /* link URLs, 1024 x 512 bytes   */
#define MEM_NET     0x00400000u   /* NIC rings and DMA buffers     */
#define MEM_TCPRAW  0x00500000u   /* raw TCP receive buffer, 3 MB  */
#define MEM_TCPRAW_SZ 0x00300000u
#define MEM_TLS     0x00800000u   /* TLS engine state and buffers  */
#define MEM_MIN_MB  16            /* RAM the VM needs              */

enum { KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN };

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t r;  __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline uint16_t inw(uint16_t p) { uint16_t r; __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline uint32_t inl(uint16_t p) { uint32_t r; __asm__ volatile("inl %1,%0" : "=a"(r) : "Nd"(p)); return r; }

/* libc bits */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
void strncpy0(char *d, const char *s, size_t n);

/* console */
extern uint8_t attr, theme;
extern int cur_row, cur_col;
void putc(char c);
void puts(const char *s);
void kprintf(const char *fmt, ...);
void set_color(uint8_t fg);
void reset_color(void);
void clear_screen(void);
void put_at(int r, int c, char ch, uint8_t a);
void cursor_show(int on);
void cursor_set(int r, int c);
void status_draw(void);

/* time and keyboard */
extern volatile uint32_t ticks;
int kb_poll(void);
int getkey(void);
int kb_esc_pressed(void);

/* clock */
struct rtc_time_s { int sec, min, hour, day, mon, year; };
void rtc_now(struct rtc_time_s *t);

/* net.c */
int  net_raw_read(uint8_t *dst, uint32_t max);
int  net_raw_write(const uint8_t *src, uint32_t len);
extern int http_last_status;
int  net_up(int verbose);
void net_status(void);
void net_set_dns(const char *ip);
void net_test(void);
extern int http_cancelled;
int  http_get(const char *url, uint8_t **body, uint32_t *len, char *final_url, int final_cap);

/* tls.c */
int  tls_begin(const char *host);
int  tls_send(const void *data, uint32_t len);
int  tls_recv(void *buf, uint32_t len);
void tls_end(void);
int  tls_error(void);
int  tls_error_is_transport(void);
const char *tls_error_text(int e);
void tls_print_chain(void);

/* web.c */
void web_search(const char *query);
void web_open(const char *url);
void web_engine(const char *name);
#endif
