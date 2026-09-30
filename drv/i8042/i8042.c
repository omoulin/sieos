/*
 * i8042.c - The PS/2 controller (i8042): its keyboard (scancode set 1,
 * translated by the controller) and mouse (3-byte packets, or 4 with the
 * wheel: IntelliMouse mode, when the mouse takes it), and the VMware-compatible
 * absolute pointer hypervisors add to it (its fourth word: the wheel).  Matched as
 * "platform,i8042" when the controller's ports do not read all ones.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "arch.h"
#include "tty.h"
#include "poll.h"
#include "ddi.h"

DDI_DRIVER("i8042", DDI_PHASE_BOOT, "PS/2 keyboard and mouse controller (i8042), vmmouse");
DDI_ALIAS("platform,i8042");

void kbd_key(uint16_t code, bool release);      /* keyboard.c: the console keyboard */
void input_set_absolute(bool on);              /* input.c */

static bool e0;
static uint8_t packet[4];
static int packet_idx, packet_len = 3;

static void kbd_irq(struct trapframe *tf)
{
    UNUSED(tf);
    while (inb(0x64) & 1) {
        uint8_t status = inb(0x64);
        if (status & 0x20)
            break;                           /* mouse byte: leave it to IRQ12 */
        uint8_t sc = inb(0x60);
        if (sc == 0xE0) {
            e0 = true;
            continue;
        }
        bool ext = e0;
        e0 = false;
        kbd_key((ext ? 0x100 : 0) | (sc & 0x7F), sc & 0x80);
    }
}

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

static void kbd_init(void)
{
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++)
        inb(0x60);
    /* Enable the keyboard IRQ and scancode translation in the controller
     * configuration byte; UEFI firmware does not always leave them on. */
    i8042_wait_write();
    outb(0x64, 0x20);
    i8042_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x01 | 0x40;
    cfg &= ~0x10;                    /* keyboard clock enabled */
    i8042_wait_write();
    outb(0x64, 0x60);
    i8042_wait_write();
    outb(0x60, cfg);
    i8042_wait_write();
    outb(0x64, 0xAE);                /* enable first port */
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++)
        inb(0x60);
    irq_register(IRQ_KBD, kbd_irq);
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
        unsigned buttons = ((d.a & VMM_LEFT) ? 1 : 0) | ((d.a & VMM_RIGHT) ? 2 : 0) | ((d.a & VMM_MIDDLE) ? 4 : 0);
        if (d.a & 0x00010000)                    /* relative packet */
            input_mouse((int32_t)d.b, (int32_t)d.c, buttons);
        else
            input_mouse_abs(d.b & 0xFFFF, d.c & 0xFFFF, buttons);   /* 0..65535 across the screen */
        input_wheel((int32_t)d.d);               /* (> 0: down) */
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
        if (packet_idx < packet_len)
            continue;
        packet_idx = 0;
        if (packet[0] & 0xC0)
            continue;                        /* overflow */
        input_mouse((int)packet[1] - ((packet[0] << 4) & 0x100), -((int)packet[2] - ((packet[0] << 3) & 0x100)),
                    packet[0] & 7);
        if (packet_len == 4)
            input_wheel((int8_t)packet[3]);  /* (> 0: down) */
    }
    if (vmmouse)
        vmmouse_poll();
}

static void mouse_init(void)
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
    /* The wheel: sample rates 200, 100, 80 ask for IntelliMouse mode, which a
     * wheel mouse then reports as its id (3), with a fourth, wheel byte. */
    static const uint8_t knock[] = { 200, 100, 80 };
    for (unsigned i = 0; i < sizeof(knock); i++) {
        mouse_write(0xF3);
        mouse_write(knock[i]);
    }
    mouse_write(0xF2);                       /* get the id */
    i8042_wait_read();
    packet_len = (inb(0x64) & 1) && inb(0x60) == 3 ? 4 : 3;
    mouse_write(0xF3);                       /* back to the default rate */
    mouse_write(100);
    mouse_write(0xF4);                       /* enable streaming */
    drain();
    packet_idx = 0;
    vmmouse = vmmouse_enable();
    input_set_absolute(vmmouse);
    irq_register(12, mouse_irq);
}

int _init(void)
{
    if (inb(0x64) == 0xFF)
        return -ENODEV;
    ps2_present = true;
    kbd_init();
    mouse_init();
    return 0;
}
