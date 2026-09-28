/*
 * kernel.h - Core kernel types and helpers.
 */
#ifndef SIEOS_KERNEL_H
#define SIEOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include "abi.h"

#define KERNEL_VBASE  0xFFFFFFFF80000000UL
#define PHYS_OFFSET   0xFFFF800000000000UL   /* direct map of physical memory */
#define DIRECT_MAP_SIZE (4UL << 30)          /* 4 GiB mapped by boot tables  */

#define P2V(pa)  ((void *)((uintptr_t)(pa) + PHYS_OFFSET))
#define V2P(va)  ((uintptr_t)(va) >= KERNEL_VBASE ? \
                  (uintptr_t)(va) - KERNEL_VBASE : (uintptr_t)(va) - PHYS_OFFSET)

#define PAGE_SIZE 4096UL
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define UNUSED(x) ((void)(x))

#define OS_NAME     "SIEOS"
#define OS_LONGNAME "Synthetic Intelligence Enhanced Operating System"
#define OS_RELEASE  "0.4.0"

/* string.c */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

/* printk.c */
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define assert(x) do { if (!(x)) panic("assertion failed: %s at %s:%d", #x, __FILE__, __LINE__); } while (0)

/* console.c */
void console_init(uint64_t mb_info_phys);
int  console_cols(void);
int  console_rows(void);
const char *console_mode(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_set_color(uint8_t fg, uint8_t bg);
bool console_logo(int px, int py, int size);    /* SIEOS logo on the framebuffer console */
void console_clear(void);
void serial_init(void);
void serial_putc(char c);
int  serial_getc_nonblock(void);

struct fb_info;
bool console_fb_info(struct fb_info *fi, uint64_t *phys);
void console_suspend(bool on);

/* keyboard.c */
void keyboard_init(void);

/* timer.c */
extern volatile uint64_t ticks;
#define TIMER_HZ 100
void timer_init(void);
uint64_t rtc_unix_time(void);
uint64_t kernel_time(void);   /* current unix time */
uint64_t hrtime(void);        /* ns since boot (TSC) */
int64_t  realtime_ns(void);   /* ns since the epoch */
void     realtime_set(int64_t ns);
int64_t  realtime_adjust(int64_t delta_ns, bool set);
extern uint64_t tsc_hz;

/* Port I/O */
static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }

#endif
