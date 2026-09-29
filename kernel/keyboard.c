/*
 * keyboard.c - PS/2 keyboard (scancode set 1, US layout) and serial input.
 */
#include "arch.h"
#include "tty.h"
#include "poll.h"

static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

static bool shift, ctrl, caps, alt, e0;

/*
 * One key, as a set 1 scancode (0x100 | code for the E0-prefixed keys):
 * from the PS/2 keyboard and from the USB and I2C HID keyboards (hid.c).
 */
void kbd_key(uint16_t code, bool release)
{
    uint8_t sc = code & 0x7F;
    bool ext = code & 0x100;
    unsigned mods = (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0);
    if (sc == 0x2A || sc == 0x36 || sc == 0x1D || sc == 0x38) {    /* modifiers */
        if (sc == 0x1D)
            ctrl = !release;
        else if (sc == 0x38)
            alt = !release;
        else if (!ext)
            shift = !release;
        if (input_grabbed())                  /* the desktop wants to see them */
            input_key(sc, !release, 0,
                      (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0));
        return;
    }
    if (ext) {                           /* arrows, Home, End, ... */
        if (input_grabbed())
            input_key(0x100 | sc, !release, 0, mods);
        return;
    }
    if (input_grabbed() && release) {
        input_key(sc, false, 0, mods);
        return;
    }
    if (release)
        return;
    if (sc == 0x3A) {
        caps = !caps;
        return;
    }
    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (!c) {
        if (input_grabbed())
            input_key(sc, true, 0, mods);   /* F-keys etc. */
        return;
    }
    if (caps && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps && c >= 'A' && c <= 'Z')
        c += 32;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= '@' && c <= '_')))
        c = (c & 0x1F);                  /* ^A..^Z, ^[ ^\ ^] ^^ ^_ */
    if (input_grabbed())
        input_key(sc, true, c, mods);
    else
        tty_input(&console_tty, c);
}

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

static void serial_irq(struct trapframe *tf)
{
    UNUSED(tf);
    int c;
    while ((c = serial_getc_nonblock()) >= 0) {
        if (c == '\r')
            c = '\n';
        else if (c == 127)
            c = '\b';
        tty_input(&console_tty, (char)c);
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

void keyboard_init(void)
{
    while (inb(0x64) & 1)
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
    while (inb(0x64) & 1)
        inb(0x60);
    irq_register(IRQ_KBD, kbd_irq);
    irq_register(IRQ_COM1, serial_irq);
}
