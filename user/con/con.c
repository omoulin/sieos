/*
 * con - The console server: the text terminal of SIEOS, and the owner of
 * the keyboard and the mouse.
 *
 * It owns the VGA text screen, the PS/2 controller (keyboard and mouse) and
 * the first serial port (COM1, which QEMU connects to your terminal), and
 * serves the port "console": CON_WRITE prints, CON_READ returns a line typed
 * on the keyboard or the serial port.
 *
 * The desktop (atlas) takes the keyboard and the mouse with CON_INPUT: from
 * then on their events go to it, unedited, while the serial port stays a
 * text console (you can always log in there). If the desktop ends, the
 * keyboard comes back to the text console.
 *
 * The mouse: QEMU's absolute pointer (the "VMware mouse" interface, I/O port
 * 0x5658), which follows your pointer without capturing it; it signals new
 * positions through the PS/2 mouse interrupt. Without it, a plain PS/2 mouse
 * (relative movements, added up here).
 *
 * On arm64 (QEMU's virt; the Raspberry Pis later) it owns the serial port
 * the device tree names as the console (a PL011): no screen, keyboard or
 * mouse there yet, so it is a serial text console.
 *
 * It is an ordinary program, not part of the kernel: the kernel only lets
 * it use these devices' I/O ports, maps the screen's memory into it, and
 * turns their interrupts into messages. If it crashes, init restarts it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static void output(const char *s, size_t n);   /* the device part, below */

/* ---- What both have: the typed line and the input owner. */
static tty_t tty;                            /* the typed line (user/lib/tty.c) */
static void echo_out(tty_t *t, const char *s, size_t n) { (void)t; output(s, n); }

/* ---- The input owner (CON_INPUT): events wait in a small queue until it
 * asks for them. Consecutive mouse moves are merged: only where the pointer
 * is now matters, so a busy desktop never falls behind. */
#define NQ 128
static con_event_t q[NQ];
static int nq, owner;                        /* owner: its pid (0: none) */
static long owner_tok;                       /* its CON_INPUT waiting for events */

static void flush_events(void)
{
    if (!owner_tok || !nq) return;
    msg_t m = { .w = { nq }, .sbuf = q, .slen = nq * sizeof *q };
    if (ipc_reply(owner_tok, &m) < 0) owner = 0;   /* it ended: the keyboard is the console's again */
    owner_tok = 0;
    nq = 0;
}

static void event(int type, int code, int buttons, int x, int y)
{
    if (owner && !owner_tok && nq >= NQ / 2) { /* not collecting: still alive? */
        mk_ident_t id;
        if (sys_ident(owner, &id) < 0) { owner = 0; nq = 0; }
    }
    if (!owner) { if (type == EV_KEY && code < 0x100) tty_input(&tty, code); return; }
    con_event_t *l = nq ? &q[nq - 1] : 0;
    if (type == EV_MOUSE && !code && l && l->type == EV_MOUSE && !l->code && l->buttons == buttons) {
        l->x = x; l->y = y;                  /* merge with the previous move */
        return;
    }
    if (nq < NQ) q[nq++] = (con_event_t){ type, buttons, code, x, y };
}

/* ---- Keys. The PS/2 keyboard sends "scancodes" (set 1): a number per
 * key, +0x80 when the key is released; 0xE0 announces an extended key
 * (arrows...). virtio-input keyboards (arm64) send the same numbers for
 * most keys (others translated below). These tables give the character
 * for each key on a US keyboard. */
static const char keys[2][58] = {
    { 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
      'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
      'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
      'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' ' },
    { 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
      'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
      'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
      'Z','X','C','V','B','N','M','<','>','?', 0,'*', 0,' ' },
};
static int mx = 32768, my = 32768, mb;       /* the pointer: position (0..65535), buttons */

static void keyboard(uint8_t sc)
{
    static int shift, ctrl, caps, ext;
    if (sc == 0xE0) { ext = 1; return; }
    int up = sc & 0x80, key = sc & 0x7F, e = ext;
    ext = 0;
    if (key == 0x2A || key == 0x36) { shift = !up; return; }   /* Shift */
    if (key == 0x1D) { ctrl = !up; return; }                   /* Ctrl (left or right) */
    if (up) return;
    if (e) {                                                   /* arrows and friends */
        static const uint8_t sc_e[] = { 0x48, 0x50, 0x4B, 0x4D, 0x49, 0x51, 0x47, 0x4F, 0x53 };
        for (int i = 0; i < 9; i++) if (key == sc_e[i]) event(EV_KEY, KEY_UP + i, mb, mx, my);
        return;
    }
    if (key == 0x3A) { caps = !caps; return; }                 /* Caps Lock */
    if (key >= 0x3B && key <= 0x44) { event(EV_KEY, KEY_F1 + key - 0x3B, mb, mx, my); return; }
    if (key == 0x57 || key == 0x58) { event(EV_KEY, KEY_F1 + 10 + key - 0x57, mb, mx, my); return; }
    if (key == 0x0F && shift) { event(EV_KEY, KEY_BTAB, mb, mx, my); return; }   /* Shift+Tab */
    if (key == 0x0F && ctrl) { event(EV_KEY, KEY_CTAB, mb, mx, my); return; }    /* Ctrl+Tab */
    if (ctrl && key >= 0x02 && key <= 0x0A) { event(EV_KEY, KEY_C1 + key - 0x02, mb, mx, my); return; }   /* Ctrl+1..9 */
    if (key < 58 && keys[0][key]) {
        char c = keys[shift][key];
        if (caps && (c | 32) >= 'a' && (c | 32) <= 'z') c ^= 32;
        if (ctrl) c &= 0x1F;                                   /* Ctrl-C = 3, Ctrl-D = 4 */
        event(EV_KEY, (uint8_t)c, mb, mx, my);
    }
}

#if defined(__x86_64__)
#define COM1 0x3F8

/* ---- The screen: 80x25 cells at physical address 0xB8000, each a
 * character and its colour (0x07: light grey on black). */
static volatile uint16_t *vga;
static int row, col, esc;    /* esc: inside an "ESC [ ... letter" sequence */

static void vga_clear(int from)
{
    for (int i = from; i < 80 * 25; i++) vga[i] = 0x0700 | ' ';
}

static void vga_putc(char c)
{
    if (esc == 1) { esc = c == '[' ? 2 : 0; return; }
    if (esc == 2) {                          /* the few ANSI sequences we need */
        if (c < '@' || c > '~') return;      /* parameters: ignored */
        esc = 0;
        if (c == 'H') row = col = 0;         /* cursor home */
        if (c == 'J') vga_clear(0);          /* clear the screen */
        if (c == 'K') for (int i = col; i < 80; i++) vga[row * 80 + i] = 0x0700 | ' ';  /* clear to end of line */
        return;
    }
    switch (c) {
    case 033:  esc = 1; return;
    case '\n': col = 0; row++; break;
    case '\r': col = 0; break;
    case '\b': if (col) col--; break;
    case '\t': col = (col + 8) & ~7; break;
    default:   vga[row * 80 + col++] = 0x0700 | (uint8_t)c;
    }
    if (col >= 80) { col = 0; row++; }
    if (row >= 25) {                         /* scroll up one line */
        memmove((void *)vga, (void *)(vga + 80), 80 * 24 * 2);
        vga_clear(80 * 24);
        row = 24;
    }
}

/* The serial port is shared with the kernel's log (and the programs that
 * write to it): our text goes through the kernel (SYS_DEBUG), which writes
 * each piece whole, under its lock, so lines of the two never mix. */
static void output(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) vga_putc(s[i]);
    for (size_t i = 0; i < n; i += 199) sys_debug(s + i, n - i < 199 ? n - i : 199);
    int pos = row * 80 + col;                /* move the blinking cursor, once */
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos);
}

/* ---- The mouse. */
static int vmmouse;                          /* QEMU's absolute pointer is there */

/* QEMU's "backdoor": a special IN instruction on port 0x5658, with a magic
 * number in eax, a command in ecx, an argument in ebx; answers in eax-edx. */
static void backdoor(uint32_t cmd, uint32_t arg, uint32_t r[4])
{
    uint32_t a = 0x564D5868, b = arg, c = cmd, d = 0x5658;
    asm volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d));
    r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}

static void kbc_wait(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ; }
static void kbc_cmd(uint8_t c) { kbc_wait(); outb(0x64, c); }
static void kbc_data(uint8_t d) { kbc_wait(); outb(0x60, d); }
static uint8_t kbc_read(void) { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) ; return inb(0x60); }
static void mouse_cmd(uint8_t c) { kbc_cmd(0xD4); kbc_data(c); kbc_read(); }   /* to the mouse; reads its ACK */

static void mouse_init(void)
{
    kbc_cmd(0xA8);                           /* enable the mouse port */
    kbc_cmd(0x20);                           /* the controller's configuration ... */
    uint8_t cfg = (kbc_read() | 2) & ~0x20;  /* ... + mouse interrupts, mouse clock on */
    kbc_cmd(0x60); kbc_data(cfg);
    mouse_cmd(0xF6);                         /* defaults */
    mouse_cmd(0xF4);                         /* report movements */
    uint32_t r[4];
    backdoor(10, 0, r);                      /* GETVERSION: QEMU answers with the magic */
    if (r[1] != 0x564D5868) return;
    backdoor(41, 0x45414552, r);             /* the absolute pointer: enable, */
    backdoor(40, 0, r);                      /* status: one word queued (its id) */
    if ((r[0] & 0xFFFF) < 1) return;
    backdoor(39, 1, r);                      /* read the id */
    backdoor(41, 0x53424152, r);             /* absolute positions, please */
    vmmouse = 1;
}

/* A byte from the mouse. With QEMU's absolute pointer it only says
 * "something moved" (the data comes through the backdoor); a plain PS/2
 * mouse sends packets of 3: buttons, dx, dy (9-bit signed). */
static void mouse_byte(uint8_t b)
{
    static uint8_t pk[3];
    static int n;
    if (vmmouse) return;
    if (!n && !(b & 8)) return;              /* resynchronise on a packet's first byte */
    pk[n++] = b;
    if (n < 3) return;
    n = 0;
    int dx = pk[1] - ((pk[0] & 0x10) << 4), dy = pk[2] - ((pk[0] & 0x20) << 3);
    mx += dx * 34; my -= dy * 60;            /* ~1 pixel per count on a 1920x1080 screen */
    mx = mx < 0 ? 0 : mx > 65535 ? 65535 : mx;
    my = my < 0 ? 0 : my > 65535 ? 65535 : my;
    mb = pk[0] & 7;
    event(EV_MOUSE, 0, mb, mx, my);
}

static void vmmouse_read(void)
{
    for (;;) {                               /* QEMU's queue: 4 words per event */
        uint32_t r[4];
        backdoor(40, 0, r);
        if (r[0] == 0xFFFF0000) { backdoor(41, 0x45414552, r); return; }  /* error: reset */
        if ((r[0] & 0xFFFF) < 4) return;
        backdoor(39, 4, r);                  /* buttons, x, y, wheel */
        mb = (r[0] & 0x20 ? MB_LEFT : 0) | (r[0] & 0x10 ? MB_RIGHT : 0) | (r[0] & 0x08 ? MB_MIDDLE : 0);
        mx = r[1] & 0xFFFF; my = r[2] & 0xFFFF;
        event(EV_MOUSE, (uint16_t)(int8_t)r[3], mb, mx, my);
    }
}

/* The x86 devices: the VGA text screen, the serial port's interrupt, the
 * mouse; then their interrupts, to our port. */
static void dev_init(long port)
{
    vga = map_phys(0xB8000, 80 * 25 * 2);
    vga_clear(0);
    outb(COM1 + 1, 1);                        /* serial: interrupt when a byte arrives */
    outb(COM1 + 4, 0x0B);                     /* ... and connect the interrupt line (OUT2) */
    mouse_init();
    while (inb(0x64) & 1) inb(0x60);          /* drop what came during start-up */
    irq_bind(1, port);                        /* the keyboard */
    irq_bind(4, port);                        /* COM1 */
    irq_bind(12, port);                       /* the mouse */
}

static void dev_irq(uint64_t bits)
{
    if (bits & (1 << 1 | 1 << 12)) {
        /* The keyboard and the mouse share the controller's one output
         * byte (status bit 0; bit 5: it is the mouse's), so both are
         * read together, in order. Their interrupts are edge-triggered:
         * a byte that arrives before we re-enable them would not
         * signal again, so look once more after re-enabling. */
        do {
            uint8_t st;
            while ((st = inb(0x64)) & 1) {
                uint8_t b = inb(0x60);
                if (st & 0x20) mouse_byte(b); else keyboard(b);
            }
            if (vmmouse) vmmouse_read();
            irq_ack(1);
            irq_ack(12);
        } while (inb(0x64) & 1);
    }
    if (bits & 1 << 4) {
        while (inb(COM1 + 5) & 1) tty_input(&tty, inb(COM1));
        irq_ack(4);
    }
}

static int has_input(void) { return 1; }   /* (the PS/2 keyboard and mouse) */
#elif defined(__aarch64__)
#include "mk/fdt.h"
#include "virtio.h"
static void input_init(long port);         /* (the keyboard and pointer, below) */
/* ---- arm64: the serial console, a PL011 UART. The device tree says
 * which (/chosen's "stdout-path", maybe through /aliases) and where;
 * output goes through the kernel's log, as on x86 (SYS_DEBUG: whole pieces
 * under its lock), input comes from its receive interrupt. */
enum { DR = 0x00 / 4, FR = 0x18 / 4, CR = 0x30 / 4, IMSC = 0x38 / 4, ICR = 0x44 / 4 };
static volatile uint32_t *uart;
static int uart_irq = -1;

static void output(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i += 199) sys_debug(s + i, n - i < 199 ? n - i : 199);
}

static int console_node(const void *f)
{
    int len, n = -1;
    const char *p = fdt_prop(f, fdt_path(f, "/chosen"), "stdout-path", &len);
    if (p && len > 1) {
        char path[64];
        int k = 0;
        while (k < 63 && k < len && p[k] && p[k] != ':') { path[k] = p[k]; k++; }
        path[k] = 0;
        if (path[0] != '/') {
            int a = fdt_path(f, "/aliases");
            p = a >= 0 ? fdt_prop(f, a, path, &len) : 0;
            if (p) n = fdt_path(f, p);
        } else n = fdt_path(f, path);
    }
    /* (a Pi's firmware names its "mini UART", which is not a PL011: then the
     * first PL011, the Pi's UART0, which config.txt puts on the GPIO pins) */
    return n >= 0 && fdt_compatible(f, n, "arm,pl011") ? n : fdt_find(f, -1, "arm,pl011");
}

static void dev_init(long port)
{
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    int n, flags;
    uint64_t a, len;
    if (!f || sys_fdt(f, size) != size || (n = console_node(f)) < 0 || fdt_reg(f, n, 0, &a, &len)) return;
    uart = map_phys(a, 4096);
    if ((long)uart < 0) { uart = 0; return; }
    if (!fdt_spi(f, n, 0, &uart_irq, &flags)) {
        uart[ICR] = 0x7FF;                    /* nothing pending */
        uart[IMSC] = 1 << 4 | 1 << 6;         /* interrupt: a byte received, or the line idle after some */
        irq_bind(uart_irq | (flags & 4 ? IRQ_LEVEL : 0), port);
    }
    free(f);
    input_init(port);
}

/* ---- The keyboard and pointer: virtio-input devices (QEMU's virt:
 * "-device virtio-keyboard-device -device virtio-tablet-device"). Each
 * sends events (type, code, value) into buffers we lend it in its queue 0;
 * we read them on its interrupt and lend the buffers again. (The Raspberry
 * Pis' keyboards and mice are USB devices: not supported yet.) */
typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } vdesc_t;
typedef struct { uint16_t type, code; uint32_t value; } vev_t;
enum { E_SYN = 0, E_KEY = 1, E_REL = 2, E_ABS = 3 };
#define NIN 4
#define QN  64
static struct { vdev_t d; vdesc_t *desc; volatile uint16_t *avail, *used; vev_t *ev; uint16_t last; } in[NIN];
static int nin, moved;

static void input_init(long port)
{
    for (int k = 0; nin < NIN && !vdev_find_n(&in[nin].d, VIRTIO_INPUT, k); k++) {
        typeof(in[0]) *p = &in[nin];
        vdev_status(&p->d, 0);
        vdev_status(&p->d, VS_ACK | VS_DRIVER);
        vdev_accept(&p->d, 0);
        if (vdev_qmax(&p->d, 0) < QN) continue;
        uint64_t used_off = (16 * QN + 6 + 2 * QN + 4095) & ~4095UL, pa, epa;
        char *r = dma_alloc(used_off + 4096, &pa);
        p->ev = dma_alloc(QN * sizeof(vev_t), &epa);
        if ((long)r < 0 || (long)p->ev < 0) continue;
        p->desc = (vdesc_t *)r;
        p->avail = (uint16_t *)(r + 16 * QN);
        p->used = (uint16_t *)(r + used_off);
        for (int i = 0; i < QN; i++) {                  /* every buffer, lent to the device */
            p->desc[i] = (vdesc_t){ epa + i * sizeof(vev_t), sizeof(vev_t), 2, 0 };
            p->avail[2 + i] = i;
        }
        p->avail[1] = QN;
        vdev_qset(&p->d, 0, QN, pa);
        if (irq_bind(p->d.irq, port)) continue;
        vdev_status(&p->d, VS_ACK | VS_DRIVER | VS_OK);
        dma_wmb();
        vdev_notify(&p->d, 0);
        nin++;
    }
}

/* One event. Keys: the scancode set 1 numbers for most keys (see keys[]);
 * the others as their extended scancodes. Buttons and the wheel; the
 * tablet's position (0..32767), reported at the end of its group (E_SYN). */
static void input_event(vev_t e)
{
    static const uint8_t ext[][2] = { { 97, 0x1D }, { 102, 0x47 }, { 103, 0x48 }, { 104, 0x49 }, { 105, 0x4B },
                                      { 106, 0x4D }, { 107, 0x4F }, { 108, 0x50 }, { 109, 0x51 }, { 111, 0x53 } };
    if (e.type == E_KEY && e.code >= 0x110 && e.code <= 0x112) {        /* left, right, middle button */
        int b = e.code == 0x110 ? MB_LEFT : e.code == 0x111 ? MB_RIGHT : MB_MIDDLE;
        mb = e.value ? mb | b : mb & ~b;
        event(EV_MOUSE, 0, mb, mx, my);
    } else if (e.type == E_KEY && e.code < 89) {
        keyboard((uint8_t)(e.code | (e.value ? 0 : 0x80)));
    } else if (e.type == E_KEY) {
        for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++)
            if (ext[i][0] == e.code) { keyboard(0xE0); keyboard((uint8_t)(ext[i][1] | (e.value ? 0 : 0x80))); }
    } else if (e.type == E_ABS && e.code < 2) {
        int v = (int)((uint64_t)(e.value > 32767 ? 32767 : e.value) * 65535 / 32767);
        if (e.code) my = v; else mx = v;
        moved = 1;
    } else if (e.type == E_REL && e.code == 8) {                        /* the wheel: up is +1 */
        event(EV_MOUSE, (uint16_t)-(int32_t)e.value, mb, mx, my);
    } else if (e.type == E_SYN && moved) {
        moved = 0;
        event(EV_MOUSE, 0, mb, mx, my);
    }
}

static void input_irq(uint64_t bits)
{
    for (int k = 0; k < nin; k++) {
        typeof(in[0]) *p = &in[k];
        int irq = p->d.irq & ~IRQ_LEVEL;
        if (!(bits & IRQ_BIT(irq))) continue;
        vdev_isr(&p->d);
        for (uint16_t idx; p->last != (idx = __atomic_load_n(&p->used[1], __ATOMIC_ACQUIRE)); p->last++) {
            uint32_t id = *(volatile uint32_t *)(p->used + 2 + 4 * (p->last % QN)) % QN;
            input_event(p->ev[id]);
            p->avail[2 + p->avail[1] % QN] = (uint16_t)id;     /* lend it again */
            __atomic_store_n(&p->avail[1], p->avail[1] + 1, __ATOMIC_RELEASE);
        }
        dma_wmb();
        vdev_notify(&p->d, 0);
        irq_ack(irq);
    }
}

static void dev_irq(uint64_t bits)
{
    input_irq(bits);
    if (!uart || uart_irq < 0 || !(bits & IRQ_BIT(uart_irq))) return;
    while (!(uart[FR] & 1 << 4)) tty_input(&tty, uart[DR] & 0xFF);   /* until "receive FIFO empty" */
    uart[ICR] = 1 << 4 | 1 << 6;
    irq_ack(uart_irq);
}
static int has_input(void) { return nin > 0; }
#endif

/* ---- USB keyboards and mice (CON_INJECT, from the USB server): the
 * keyboard's keys arrive as set-1 scancodes, so they go through keyboard()
 * like the PS/2 ones; a mouse's moves are scaled to our 0..65535 range as if
 * on a 1920 x 1080 screen; a tablet gives its position directly. */
static int usb_input;                        /* the USB server has a keyboard or pointer */
static void inject(const con_inject_t *v, int n)
{
    usb_input = 1;
    for (int i = 0; i < n; i++, v++) {
        if (v->kind == INJ_KEY) { keyboard(v->sc); continue; }
        if (v->kind == INJ_REL) {
            int x = mx + v->dx * 34, y = my + v->dy * 61;
            mx = x < 0 ? 0 : x > 65535 ? 65535 : x;
            my = y < 0 ? 0 : y > 65535 ? 65535 : y;
        } else if (v->kind == INJ_ABS) { mx = v->x; my = v->y; }
        else continue;
        if (v->buttons != mb || v->kind != INJ_KEY) { mb = v->buttons; event(EV_MOUSE, 0, mb, mx, my); }
        if (v->wheel) event(EV_MOUSE, (uint16_t)-(int)v->wheel, mb, mx, my);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    tty_init(&tty, echo_out, 0);
    long port = port_create("console");
    dev_init(port);

    /* The loop: answer the last client and receive the next message in one
     * system call (the fast path), or just receive when there is no answer
     * to give (interrupts; a CON_READ answered later, when a line is typed). */
    static char buf[4096] __attribute__((aligned(8)));
    long from = 0, answer = 0;
    int reply = 0;
    for (;;) {
        msg_t m = { .w = { answer }, .rbuf = buf, .rlen = sizeof buf };
        from = reply ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
        reply = 0;
        if (from == 0) {                      /* interrupts: w[0] says which */
            dev_irq(m.w[0]);
            flush_events();
            continue;
        }
        if (from < 0) continue;
        reply = 1;
        switch (m.w[0]) {
        case CON_WRITE: output(buf, m.rlen); answer = m.rlen; break;
        case CON_READ:
            reply = 0;                        /* answered by the tty, now or when a line is typed */
            if (tty_read(&tty, from, m.w[1]) < 0) { answer = -EBUSY; reply = 1; }
            break;
        case CON_ECHO:  tty.echo = m.w[1]; answer = 0; break;
        case CON_HASINPUT: answer = has_input() || usb_input; break;
        case CON_INJECT:
            if (m.uid) { answer = -EPERM; break; }
            inject((const con_inject_t *)buf, (int)(m.rlen / sizeof(con_inject_t)));
            flush_events();
            answer = 0;
            break;
        case CON_INPUT:
            if (m.uid) { answer = -EPERM; break; }
            if (owner_tok && owner != m.pid) reply_val(owner_tok, -EBUSY);   /* a new desktop takes over */
            owner = m.pid;
            owner_tok = from;
            reply = 0;
            flush_events();
            break;
        default:        answer = -ENOSYS;
        }
    }
}
