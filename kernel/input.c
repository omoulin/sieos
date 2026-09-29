/*
 * input.c - Input event queue (/dev/events) and the PS/2 mouse driver.
 *
 * While /dev/events is open, keyboard input is delivered as key events
 * instead of going to the console terminal (a "grab"), and PS/2 mouse
 * packets are delivered as relative motion events.
 */
#include "proc.h"
#include "arch.h"
#include "poll.h"

#define QSIZE 512

static struct input_event queue[QSIZE];
static uint32_t q_head, q_tail;
static int open_count;
static uint8_t packet[3];
static int packet_idx;

bool input_grabbed(void)
{
    return open_count > 0;
}

static void push(struct input_event *e)
{
    if (!open_count)
        return;
    if (q_tail - q_head == QSIZE)
        q_head++;                            /* drop the oldest */
    queue[q_tail++ % QSIZE] = *e;
    wakeup(queue);
    poll_wakeup();
}

void input_key(uint16_t code, bool pressed, char ascii, unsigned mods)
{
    struct input_event e = { 0 };
    e.type = EV_KEY;
    e.code = code;
    e.value = pressed;
    e.ascii = (unsigned char)ascii;
    e.mods = mods;
    push(&e);
}

void input_mouse(int dx, int dy, unsigned buttons)
{
    struct input_event e = { 0 };
    e.type = EV_MOUSE;
    e.dx = dx;
    e.dy = dy;
    e.buttons = buttons;
    push(&e);
}

void input_mouse_abs(int x, int y, unsigned buttons)
{
    struct input_event e = { 0 };
    e.type = EV_MOUSE_ABS;
    e.dx = x;
    e.dy = y;
    e.buttons = buttons;
    push(&e);
}

bool input_readable(void)
{
    return q_head != q_tail;
}

long input_read(char *buf, size_t n)
{
    size_t sz = sizeof(struct input_event);
    if (n < sz)
        return -EINVAL;
    while (q_head == q_tail) {
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(queue);
    }
    size_t got = 0;
    while (got + sz <= n && q_head != q_tail) {
        memcpy(buf + got, &queue[q_head++ % QSIZE], sz);
        got += sz;
    }
    return got;
}

int input_open(void)
{
    if (open_count == 0)
        q_head = q_tail = 0;
    open_count++;
    return 0;
}

void input_close(void)
{
    if (open_count > 0)
        open_count--;
}

/* ---------------- PS/2 mouse ---------------- */

static void i8042_wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
}

static void i8042_wait_read(void)
{
    for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++)
        ;
}

static void mouse_write(uint8_t v)
{
    i8042_wait_write();
    outb(0x64, 0xD4);
    i8042_wait_write();
    outb(0x60, v);
    i8042_wait_read();
    inb(0x60);                               /* ACK */
}

static void drain(void)
{
    for (int i = 0; i < 16 && (inb(0x64) & 1); i++) {
        inb(0x60);
        for (int k = 0; k < 1000; k++)
            io_wait();
    }
}

/*
 * VMware-compatible absolute pointer ("vmmouse"), provided by QEMU and
 * other hypervisors.  With it the guest cursor follows the host pointer
 * exactly and the emulator never has to grab the mouse.  Motion is still
 * signalled through PS/2 interrupts; the data is read through the
 * hypervisor I/O port.
 */
#define VMM_MAGIC          0x564D5868
#define VMM_PORT           0x5658
#define VMM_GETVERSION     10
#define VMM_ABS_DATA       39
#define VMM_ABS_STATUS     40
#define VMM_ABS_COMMAND    41
#define VMM_CMD_ENABLE     0x45414552
#define VMM_CMD_DISABLE    0x000000F5
#define VMM_CMD_ABSOLUTE   0x53424152
#define VMM_VERSION_ID     0x3442554A
#define VMM_ERROR          0xFFFF0000
#define VMM_LEFT           0x20
#define VMM_RIGHT          0x10
#define VMM_MIDDLE         0x08

static bool vmmouse;

struct vmm_regs {
    uint32_t a, b, c, d;
};

static struct vmm_regs vmm_cmd(uint32_t cmd, uint32_t arg)
{
    struct vmm_regs r;
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d)
                     : "a"(VMM_MAGIC), "b"(arg), "c"(cmd), "d"(VMM_PORT)
                     : "memory");
    return r;
}

static bool vmmouse_enable(void)
{
    /* Probing an absent port is harmless: inl returns all ones. */
    if (vmm_cmd(VMM_GETVERSION, 0).b != VMM_MAGIC)
        return false;
    vmm_cmd(VMM_ABS_COMMAND, VMM_CMD_ENABLE);
    uint32_t status = vmm_cmd(VMM_ABS_STATUS, 0).a;
    if ((status & VMM_ERROR) == VMM_ERROR || (status & 0xFFFF) == 0)
        return false;
    if (vmm_cmd(VMM_ABS_DATA, 1).a != VMM_VERSION_ID) {
        vmm_cmd(VMM_ABS_COMMAND, VMM_CMD_DISABLE);
        return false;
    }
    vmm_cmd(VMM_ABS_COMMAND, VMM_CMD_ABSOLUTE);
    return true;
}

static void vmmouse_poll(void)
{
    for (int guard = 0; guard < 64; guard++) {
        uint32_t status = vmm_cmd(VMM_ABS_STATUS, 0).a;
        if ((status & VMM_ERROR) == VMM_ERROR) {
            vmmouse = vmmouse_enable();          /* reset after an error */
            return;
        }
        uint32_t queued = status & 0xFFFF;
        if (queued < 4)
            return;
        struct vmm_regs d = vmm_cmd(VMM_ABS_DATA, 4);
        struct input_event e = { 0 };
        e.buttons = ((d.a & VMM_LEFT) ? 1 : 0) | ((d.a & VMM_RIGHT) ? 2 : 0) | ((d.a & VMM_MIDDLE) ? 4 : 0);
        if (d.a & 0x00010000) {                  /* relative packet */
            e.type = EV_MOUSE;
            e.dx = (int32_t)d.b;
            e.dy = (int32_t)d.c;
        } else {
            e.type = EV_MOUSE_ABS;
            e.dx = d.b & 0xFFFF;                 /* 0..65535 across the screen */
            e.dy = d.c & 0xFFFF;
        }
        push(&e);
    }
}

static void mouse_irq(struct trapframe *tf)
{
    UNUSED(tf);
    while (inb(0x64) & 1) {
        uint8_t status = inb(0x64);
        if (!(status & 0x20))
            break;                           /* keyboard byte: leave it to IRQ1 */
        uint8_t b = inb(0x60);
        if (vmmouse)
            continue;                        /* PS/2 packet only signals new data */
        if (packet_idx == 0 && !(b & 0x08))
            continue;                        /* resynchronise on the "always 1" bit */
        packet[packet_idx++] = b;
        if (packet_idx < 3)
            continue;
        packet_idx = 0;
        if (packet[0] & 0xC0)
            continue;                        /* overflow */
        struct input_event e = { 0 };
        e.type = EV_MOUSE;
        e.dx = (int)packet[1] - ((packet[0] << 4) & 0x100);
        e.dy = -((int)packet[2] - ((packet[0] << 3) & 0x100));
        e.buttons = packet[0] & 7;
        push(&e);
    }
    if (vmmouse)
        vmmouse_poll();
}

bool input_absolute(void)
{
    return vmmouse;
}

void input_init(void)
{
    i8042_wait_write();
    outb(0x64, 0xA8);                        /* enable auxiliary port */
    i8042_wait_write();
    outb(0x64, 0x20);
    i8042_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x02;                             /* IRQ12 */
    cfg &= ~0x20;                            /* mouse clock on */
    i8042_wait_write();
    outb(0x64, 0x60);
    i8042_wait_write();
    outb(0x60, cfg);
    /* Full reset: firmware may have left the mouse in 4-byte wheel mode,
     * which would desynchronise our 3-byte packet parser. */
    mouse_write(0xFF);
    drain();                                 /* self-test result 0xAA, id 0x00 */
    mouse_write(0xF6);                       /* defaults: 3-byte packets */
    mouse_write(0xF4);                       /* enable streaming */
    drain();
    packet_idx = 0;
    vmmouse = vmmouse_enable();
    irq_register(12, mouse_irq);
}
