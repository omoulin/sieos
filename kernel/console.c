/*
 * console.c - Text console on VGA text mode (BIOS) or a linear framebuffer
 * (UEFI GOP / VBE), mirrored to the COM1 serial port.
 *
 * The screen is a grid of character cells.  On a framebuffer, glyphs from
 * an 8x16 bitmap font are rendered and a shadow copy of the cells is kept
 * so the cursor can be drawn and erased.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "smp.h"

#define COM1 0x3F8
#define FONT_W 8
#define FONT_H 16
#define MAX_COLS 240
#define MAX_ROWS 80

extern const uint8_t font8x16[256][16];

static enum { SCR_NONE, SCR_VGA, SCR_FB } scr;
static volatile uint16_t *vga;
static uint8_t *fb;
static uint32_t fb_pitch, fb_width, fb_height, fb_bpp;
static uint8_t fb_rpos, fb_gpos, fb_bpos;
static uint32_t palette[16];
static uint16_t cells[MAX_COLS * MAX_ROWS];     /* char | attr << 8 */

static int cols = 80, rows = 25;
static bool suspended;             /* a graphical program owns the screen */
static uint64_t fb_phys;
static bool fb_deferred;           /* a framebuffer above the direct map: used once display_init maps it */
static int cur_x, cur_y;
static uint8_t color = 0x07;

int console_cols(void) { return cols; }
int console_rows(void) { return rows; }

/* ------------------------------------------------------------------ */
/* Framebuffer rendering                                               */
/* ------------------------------------------------------------------ */

static const uint8_t vga_rgb[16][3] = {
    { 0, 0, 0 },       { 0, 0, 170 },     { 0, 170, 0 },     { 0, 170, 170 },
    { 170, 0, 0 },     { 170, 0, 170 },   { 170, 85, 0 },    { 170, 170, 170 },
    { 85, 85, 85 },    { 85, 85, 255 },   { 85, 255, 85 },   { 85, 255, 255 },
    { 255, 85, 85 },   { 255, 85, 255 },  { 255, 255, 85 },  { 255, 255, 255 },
};

static inline void fb_put_pixel(uint8_t *row, uint32_t x, uint32_t c)
{
    if (fb_bpp == 32) {
        ((uint32_t *)row)[x] = c;
    } else {
        uint8_t *p = row + x * 3;
        p[0] = c;
        p[1] = c >> 8;
        p[2] = c >> 16;
    }
}

static void fb_draw_cell(int x, int y, uint16_t cell, bool cursor)
{
    uint8_t ch = cell & 0xFF, attr = cell >> 8;
    uint32_t fg = palette[attr & 0x0F], bg = palette[attr >> 4];
    if (cursor) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    const uint8_t *glyph = font8x16[ch];
    for (int gy = 0; gy < FONT_H; gy++) {
        uint8_t *row = fb + (uint64_t)(y * FONT_H + gy) * fb_pitch;
        uint8_t bits = glyph[gy];
        for (int gx = 0; gx < FONT_W; gx++)
            fb_put_pixel(row, x * FONT_W + gx, (bits & (0x80 >> gx)) ? fg : bg);
    }
}

/* ------------------------------------------------------------------ */
/* Cell operations                                                     */
/* ------------------------------------------------------------------ */

static void put_cell(int x, int y, uint16_t cell)
{
    cells[y * cols + x] = cell;
    if (suspended)
        return;
    if (scr == SCR_VGA)
        vga[y * 80 + x] = cell;
    else if (scr == SCR_FB)
        fb_draw_cell(x, y, cell, false);
}

static int cursor_drawn_x = -1, cursor_drawn_y = -1;

static void update_cursor(void)
{
    if (suspended)
        return;
    if (scr == SCR_VGA) {
        uint16_t pos = cur_y * 80 + cur_x;
        outb(0x3D4, 0x0F);
        outb(0x3D5, pos & 0xFF);
        outb(0x3D4, 0x0E);
        outb(0x3D5, pos >> 8);
    } else if (scr == SCR_FB) {
        if (cursor_drawn_x >= 0 && cursor_drawn_x < cols && cursor_drawn_y < rows)
            fb_draw_cell(cursor_drawn_x, cursor_drawn_y, cells[cursor_drawn_y * cols + cursor_drawn_x], false);
        fb_draw_cell(cur_x, cur_y, cells[cur_y * cols + cur_x], true);
        cursor_drawn_x = cur_x;
        cursor_drawn_y = cur_y;
    }
}

static void scroll(void)
{
    memmove(cells, cells + cols, cols * (rows - 1) * sizeof(uint16_t));
    for (int i = 0; i < cols; i++)
        cells[(rows - 1) * cols + i] = (color << 8) | ' ';
    if (suspended) {
        /* cells only */
    } else if (scr == SCR_VGA) {
        memmove((void *)vga, (void *)(vga + 80), 80 * (rows - 1) * 2);
        for (int i = 0; i < 80; i++)
            vga[(rows - 1) * 80 + i] = (color << 8) | ' ';
    } else if (scr == SCR_FB) {
        size_t line = (size_t)fb_pitch * FONT_H;
        memmove(fb, fb + line, line * (rows - 1));
        for (int i = 0; i < cols; i++)
            fb_draw_cell(i, rows - 1, cells[(rows - 1) * cols + i], false);
        cursor_drawn_y--;
    }
    cur_y = rows - 1;
}

static void screen_putc(char c)
{
    switch (c) {
    case '\n':
        cur_x = 0;
        cur_y++;
        break;
    case '\r':
        cur_x = 0;
        break;
    case '\b':
        if (cur_x > 0) {
            cur_x--;
        } else if (cur_y > 0) {
            cur_y--;
            cur_x = cols - 1;
        }
        break;
    case '\t':
        cur_x = (cur_x + 8) & ~7;
        break;
    default:
        put_cell(cur_x, cur_y, (color << 8) | (uint8_t)c);
        cur_x++;
    }
    if (cur_x >= cols) {
        cur_x = 0;
        cur_y++;
    }
    if (cur_y >= rows)
        scroll();
}

/*
 * The console's state (the cells, the cursor, the escapes, the serial line)
 * is shared by every processor: kprintf from anywhere (interrupts too), the
 * console terminal's output, the displays' mode changes.
 */
static struct spinlock cons_lock;

static void clear_locked(void)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            put_cell(x, y, (color << 8) | ' ');
    cur_x = cur_y = 0;
    cursor_drawn_x = -1;
    update_cursor();
}

/* ------------------------------------------------------------------ */
/* Serial port                                                         */
/* ------------------------------------------------------------------ */

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR, OUT2 (IRQs routed) */
    outb(COM1 + 1, 0x01);   /* interrupt on received data */
}

void serial_putc(char c)
{
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, c);
}

int serial_getc_nonblock(void)
{
    if (inb(COM1 + 5) & 1)
        return inb(COM1);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Initialisation from the multiboot2 framebuffer tag                  */
/* ------------------------------------------------------------------ */

struct mb2_fb_tag {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, fb_type;
    uint16_t reserved;
    uint8_t rpos, rsize, gpos, gsize, bpos, bsize;
} __attribute__((packed));

static bool setup_framebuffer(struct mb2_fb_tag *t)
{
    if (t->fb_type == 2) {                      /* EGA text mode */
        vga = P2V(t->addr ? t->addr : 0xB8000);
        cols = MIN((int)t->width, 80);
        rows = MIN((int)t->height, 25);
        scr = SCR_VGA;
        return true;
    }
    if (t->fb_type != 1 || (t->bpp != 32 && t->bpp != 24))
        return false;
    fb_phys = t->addr;
    fb_pitch = t->pitch;
    fb_width = t->width;
    fb_height = t->height;
    fb_bpp = t->bpp;
    fb_rpos = t->rpos;
    fb_gpos = t->gpos;
    fb_bpos = t->bpos;
    for (int i = 0; i < 16; i++)
        palette[i] = ((uint32_t)vga_rgb[i][0] << fb_rpos) | ((uint32_t)vga_rgb[i][1] << fb_gpos) |
                     ((uint32_t)vga_rgb[i][2] << fb_bpos);
    if (t->addr + (uint64_t)t->pitch * t->height > DIRECT_MAP_SIZE) {
        fb_deferred = true;                     /* (UEFI: in a GPU aperture above 4 GiB) */
        return false;
    }
    fb = P2V(t->addr);
    cols = MIN((int)(fb_width / FONT_W), MAX_COLS);
    rows = MIN((int)(fb_height / FONT_H), MAX_ROWS);
    scr = SCR_FB;
    return true;
}

/* The display layer's mapping of the framebuffer (write-combining); a
 * deferred framebuffer comes into use here. */
void console_fb_remap(void *kva)
{
    if (!kva)
        return;
    spin_lock(&cons_lock);
    if (scr != SCR_FB && !fb_deferred) {
        spin_unlock(&cons_lock);
        return;
    }
    fb = kva;
    if (fb_deferred) {
        fb_deferred = false;
        cols = MIN((int)(fb_width / FONT_W), MAX_COLS);
        rows = MIN((int)(fb_height / FONT_H), MAX_ROWS);
        scr = SCR_FB;
        clear_locked();
    }
    spin_unlock(&cons_lock);
}

/* The display's mode changed: the console follows it (a clear screen). */
void console_fb_mode(void *kva, uint32_t width, uint32_t height, uint32_t pitch)
{
    spin_lock(&cons_lock);
    if (scr != SCR_FB) {
        spin_unlock(&cons_lock);
        return;
    }
    fb = kva;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = 32;
    cols = MIN((int)(fb_width / FONT_W), MAX_COLS);
    rows = MIN((int)(fb_height / FONT_H), MAX_ROWS);
    if (!suspended)
        clear_locked();
    spin_unlock(&cons_lock);
}

void console_init(uint64_t mb_info_phys)
{
    serial_init();
    scr = SCR_NONE;
    if (mb_info_phys) {
        uint32_t total = *(uint32_t *)P2V(mb_info_phys);
        uint8_t *p = (uint8_t *)P2V(mb_info_phys) + 8, *end = p + total - 8;
        while (p < end) {
            uint32_t type = ((uint32_t *)p)[0], size = ((uint32_t *)p)[1];
            if (type == 0)
                break;
            if (type == 8)
                setup_framebuffer((struct mb2_fb_tag *)p);
            p += (size + 7) & ~7;
        }
    }
    if (scr == SCR_NONE) {                      /* BIOS text mode default */
        vga = P2V(0xB8000);
        cols = 80;
        rows = 25;
        scr = SCR_VGA;
    }
    clear_locked();
}

const char *console_mode(void)
{
    static char buf[48];
    if (scr == SCR_FB)
        snprintf(buf, sizeof(buf), "framebuffer %ux%u (%dx%d text)", fb_width, fb_height, cols, rows);
    else
        snprintf(buf, sizeof(buf), "VGA text %dx%d", cols, rows);
    return buf;
}

/* Describe the framebuffer for /dev/fb0 (32 bpp only). */
bool console_fb_info(struct fb_info *fi, uint64_t *phys)
{
    if ((scr != SCR_FB && !fb_deferred) || fb_bpp != 32)
        return false;
    fi->width = fb_width;
    fi->height = fb_height;
    fi->pitch = fb_pitch;
    fi->bpp = fb_bpp;
    fi->red_pos = fb_rpos;
    fi->green_pos = fb_gpos;
    fi->blue_pos = fb_bpos;
    fi->pad = 0;
    if (phys)
        *phys = fb_phys;
    return true;
}

/* Stop drawing while a graphical program owns the screen; repaint after. */
void console_suspend(bool on)
{
    spin_lock(&cons_lock);
    suspended = on;
    if (!on && scr == SCR_FB) {
        memset(fb, 0, (size_t)fb_pitch * fb_height);
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++)
                fb_draw_cell(x, y, cells[y * cols + x], false);
        cursor_drawn_x = -1;
        update_cursor();
    }
    spin_unlock(&cons_lock);
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    color = (bg << 4) | (fg & 0x0F);
}

/* ------------------------------------------------------------------ */
/* ANSI escapes (SGR colours, clear, cursor home, erase line)          */
/* ------------------------------------------------------------------ */

static int esc_state, esc_nparam, esc_param[8];
static bool bold;

static void ansi_sgr(int code)
{
    static const uint8_t map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    uint8_t fg = color & 0x0F, bg = color >> 4;
    if (code == 0) {
        fg = 7;
        bg = 0;
        bold = false;
    } else if (code == 1) {
        bold = true;
        fg |= 8;
    } else if (code == 22) {
        bold = false;
        fg &= 7;
    } else if (code >= 30 && code <= 37) {
        fg = map[code - 30] | (bold ? 8 : 0);
    } else if (code == 39) {
        fg = 7 | (bold ? 8 : 0);
    } else if (code >= 90 && code <= 97) {
        fg = map[code - 90] | 8;
    } else if (code >= 40 && code <= 47) {
        bg = map[code - 40];
    } else if (code == 49) {
        bg = 0;
    }
    color = (bg << 4) | fg;
}

static bool ansi_filter(char c)
{
    if (esc_state == 0) {
        if (c != 27)
            return false;
        esc_state = 1;
        return true;
    }
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_nparam = 0;
            esc_param[0] = 0;
            return true;
        }
        esc_state = 0;
        return true;
    }
    if (c >= '0' && c <= '9') {
        esc_param[esc_nparam] = esc_param[esc_nparam] * 10 + (c - '0');
        return true;
    }
    if (c == ';') {
        if (esc_nparam < 7)
            esc_param[++esc_nparam] = 0;
        return true;
    }
    if (c == '?')
        return true;
    switch (c) {
    case 'm':
        for (int i = 0; i <= esc_nparam; i++)
            ansi_sgr(esc_param[i]);
        break;
    case 'J':
        if (esc_param[0] == 2)
            clear_locked();
        break;
    case 'H':
        cur_x = cur_y = 0;
        break;
    case 'K':
        for (int x = cur_x; x < cols; x++)
            put_cell(x, cur_y, (color << 8) | ' ');
        break;
    }
    esc_state = 0;
    return true;
}

static void putc_locked(char c)
{
    if (c == '\f') {             /* form feed clears the screen */
        clear_locked();
        const char *esc = "\033[2J\033[H";
        while (*esc)
            serial_putc(*esc++);
        return;
    }
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
    if (!ansi_filter(c))
        screen_putc(c);
}

void console_write(const char *s, size_t n)
{
    spin_lock(&cons_lock);
    for (size_t i = 0; i < n; i++)
        putc_locked(s[i]);
    update_cursor();
    spin_unlock(&cons_lock);
}

void console_putc(char c)
{
    console_write(&c, 1);
}

void console_clear(void)
{
    spin_lock(&cons_lock);
    clear_locked();
    spin_unlock(&cons_lock);
}

/* panic: the lock may be held by the processor that failed. */
void console_panic(void)
{
    spin_unlock(&cons_lock);
}

/*
 * The SIEOS logo (the Facet cube: docs/logo.svg), drawn from its 512-unit
 * design on the (black) boot screen: a three-faced blue cube seen from a
 * corner, its dark seams meeting at an amber node.  Each pixel is sampled
 * 4 x 4 times (integers only: no floating point in the kernel).
 */

/* The design's colour at (X, Y), in 1/16 units; false outside the cube. */
static bool logo_at(int X, int Y, uint32_t *rgb)
{
    int dx = X - 256 * 16, dy = Y - 256 * 16, adx = dx < 0 ? -dx : dx;
    long d2 = (long)dx * dx + (long)dy * dy;
    if (d2 <= 42L * 42 * 256) {                          /* the node */
        *rgb = 0xD9A35F;
        return true;
    }
    if (d2 <= 58L * 58 * 256) {                          /* its ring */
        *rgb = 0x1C1C1C;
        return true;
    }
    /* the hexagon: |dx| <= 200, between the slopes 115/200 of its top and bottom edges */
    if (adx > 200 * 16 || dy * 200 < -230 * 16 * 200 + adx * 115 || dy * 200 > 230 * 16 * 200 - adx * 115)
        return false;
    /* the seams, 16 units wide: from the centre to the upper left, upper right and bottom corners */
    long up = (long)adx * 115 + (long)dy * 200;          /* 0 on the upper seams */
    if ((up < 0 ? -up : up) <= 8L * 16 * 231 || (adx <= 8 * 16 && dy >= 0)) {
        *rgb = 0x1C1C1C;
        return true;
    }
    *rgb = up < 0 ? 0x8FB8E6 : dx < 0 ? 0x6A95D2 : 0x4A78BC;   /* the top, left and right faces */
    return true;
}

bool console_logo(int px, int py, int size)
{
    if (scr != SCR_FB || suspended || px < 0 || py < 0 || px + size > (int)fb_width || py + size > (int)fb_height)
        return false;
    for (int y = 0; y < size; y++) {
        uint8_t *row = fb + (uint64_t)(py + y) * fb_pitch;
        for (int x = 0; x < size; x++) {
            int r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    uint32_t c;
                    int X = (int)(((long)(x * 8 + sx * 2 + 1)) * 512 * 16 / (size * 8));
                    int Y = (int)(((long)(y * 8 + sy * 2 + 1)) * 512 * 16 / (size * 8));
                    if (logo_at(X, Y, &c)) {
                        r += c >> 16 & 0xFF;
                        g += c >> 8 & 0xFF;
                        b += c & 0xFF;
                    }
                }
            r /= 16;
            g /= 16;
            b /= 16;
            if (r | g | b)
                fb_put_pixel(row, px + x, ((uint32_t)r << fb_rpos) | ((uint32_t)g << fb_gpos) | ((uint32_t)b << fb_bpos));
        }
    }
    return true;
}
