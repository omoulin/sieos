/*
 * hid.c - HID keyboards and pointers, for the USB (xhci.c) and I2C
 * (i2c_hid.c) transports.
 *
 * A report descriptor is parsed into its input fields; the keyboard ones
 * (usage page 7, as a modifier bitmap, a key bitmap or a key array) and
 * the mouse ones (buttons, X and Y, relative or absolute) of the keyboard,
 * mouse and pointer application collections are used, the rest ignored.
 * USB boot-protocol devices send fixed reports instead (8 bytes for a
 * keyboard, 3 or more for a mouse).
 *
 * Keys go to keyboard.c as set 1 scancodes, so they behave like the PS/2
 * keyboard's (the console, /dev/events); the keyboard repeats keys itself
 * there, so here a held key repeats after 500 ms, 30 times a second.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "hid.h"
#include "poll.h"

#define REPEAT_DELAY_NS (500UL * 1000000)
#define REPEAT_NS       (33UL * 1000000)

/* HID keyboard usages (page 7) to set 1 scancodes; 0x100 | code: E0-prefixed. */
static const uint16_t usage_set1[256] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21,
    [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13,
    [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C,
    [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40,
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = 0x137, [0x47] = 0x46,
    [0x49] = 0x152, [0x4A] = 0x147, [0x4B] = 0x149, [0x4C] = 0x153, [0x4D] = 0x14F, [0x4E] = 0x151,
    [0x4F] = 0x14D, [0x50] = 0x14B, [0x51] = 0x150, [0x52] = 0x148,
    [0x53] = 0x45, [0x54] = 0x135, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x11C,
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D,
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56,
    [0x65] = 0x15D,
    [0xE0] = 0x1D, [0xE1] = 0x2A, [0xE2] = 0x38, [0xE3] = 0x15B,
    [0xE4] = 0x11D, [0xE5] = 0x36, [0xE6] = 0x138, [0xE7] = 0x15C,
};

/* ---------------------------------------------------------------- parser */

static int32_t item_value(const uint8_t *p, int size, bool sign)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    if (sign && size && size < 4 && (v & (1U << (8 * size - 1))))
        v |= ~0U << (8 * size);
    return (int32_t)v;
}

int hid_parse(struct hid *h, const uint8_t *d, size_t n)
{
    struct globals { uint16_t page; int32_t lmin, lmax; uint32_t size, count; uint8_t id; } g = { 0 }, stack[4];
    int sp = 0, depth = 0;
    uint8_t app = HID_APP_OTHER;
    uint16_t usages[HID_MAX_USAGES], lpage = 0;
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;
    static uint16_t offset[256];                 /* input bits so far, per report ID (boot, one at a time) */
    memset(offset, 0, sizeof(offset));
    h->nfields = 0;
    h->ids = false;
    h->has_kbd = h->has_mouse = false;

    for (size_t i = 0; i < n;) {
        uint8_t b = d[i];
        if (b == 0xFE) {                         /* long item */
            if (i + 1 >= n)
                break;
            i += 3 + d[i + 1];
            continue;
        }
        int size = (b & 3) == 3 ? 4 : (b & 3), type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + size > n)
            break;
        const uint8_t *p = d + i + 1;
        uint32_t u = (uint32_t)item_value(p, size, false);
        i += 1 + size;
        if (type == 1) {                         /* global */
            switch (tag) {
            case 0: g.page = u; break;
            case 1: g.lmin = item_value(p, size, true); break;
            case 2: g.lmax = item_value(p, size, g.lmin < 0); break;
            case 7: g.size = u; break;
            case 8: g.id = u; h->ids = true; break;
            case 9: g.count = u; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {                  /* local */
            if (size == 4 && (tag <= 2))
                lpage = u >> 16;
            if (tag == 0 && nusages < HID_MAX_USAGES)
                usages[nusages++] = u & 0xFFFF;
            else if (tag == 1)
                umin = u & 0xFFFF, range = true;
            else if (tag == 2)
                umax = u & 0xFFFF, range = true;
        } else if (type == 0) {                  /* main */
            uint16_t page = lpage ? lpage : g.page;
            if (tag == 10) {                     /* collection */
                if (depth == 0 && u == 1) {      /* an application */
                    uint16_t usage = nusages ? usages[0] : (uint16_t)umin;
                    app = page != 1 ? HID_APP_OTHER : usage == 6 || usage == 7 ? HID_APP_KEYBOARD
                        : usage == 2 || usage == 1 ? HID_APP_MOUSE : HID_APP_OTHER;
                }
                depth++;
            } else if (tag == 12) {              /* end collection */
                if (depth > 0 && --depth == 0)
                    app = HID_APP_OTHER;
            } else if (tag == 8) {               /* input */
                uint32_t bits = g.size * g.count;
                if (!(u & 1) && app != HID_APP_OTHER && g.size && g.size <= 32 &&
                    h->nfields < HID_MAX_FIELDS) {
                    struct hid_field *f = &h->f[h->nfields++];
                    memset(f, 0, sizeof(*f));
                    f->report_id = g.id;
                    f->app = app;
                    f->flags = u;
                    f->page = page;
                    f->bit = offset[g.id];
                    f->size = g.size;
                    f->count = g.count;
                    f->lmin = g.lmin;
                    f->lmax = g.lmax;
                    if (nusages && !range) {
                        f->nusages = nusages;
                        memcpy(f->usages, usages, nusages * sizeof(uint16_t));
                    } else {
                        f->umin = umin;
                        f->umax = umax;
                        if (nusages && !umin && !umax)
                            f->umin = f->umax = usages[0];
                    }
                    if (page == 7)
                        h->has_kbd = true;
                    if (app == HID_APP_MOUSE && page == 1 && (f->nusages ? f->usages[0] : f->umin) >= 0x30)
                        h->has_mouse = true;
                }
                offset[g.id] += bits;
            }
            nusages = 0;                         /* locals end with each main item */
            umin = umax = 0;
            range = false;
            lpage = 0;
        }
    }
    return h->has_kbd || h->has_mouse ? 0 : -1;
}

/* ---------------------------------------------------------------- reports */

static uint32_t get_bits(const uint8_t *r, size_t n, unsigned bit, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned b = bit + i;
        if (b / 8 >= n)
            break;
        v |= (uint32_t)((r[b / 8] >> (b % 8)) & 1) << i;
    }
    return v;
}

static int32_t field_value(const struct hid_field *f, const uint8_t *r, size_t n, int i)
{
    uint32_t v = get_bits(r, n, f->bit + i * f->size, f->size);
    if (f->lmin < 0 && f->size < 32 && (v & (1U << (f->size - 1))))
        v |= ~0U << f->size;                     /* signed */
    return (int32_t)v;
}

static uint16_t field_usage(const struct hid_field *f, int idx)
{
    if (f->nusages)
        return idx < f->nusages ? f->usages[idx] : f->usages[f->nusages - 1];
    return f->umin + idx <= f->umax || !f->umax ? f->umin + idx : 0;
}

static bool is_modifier(uint8_t u) { return u >= 0xE0 && u <= 0xE7; }

/* The keyboard now holds keys[0..n): press and release the difference. */
static void kbd_update(struct hid *h, const uint8_t *keys, int n)
{
    for (int pass = 0; pass < 2; pass++)         /* releases: other keys, then the modifiers */
        for (int i = 0; i < h->nkeys; i++) {
            uint8_t u = h->keys[i];
            if (is_modifier(u) != pass)
                continue;
            bool still = false;
            for (int k = 0; k < n; k++)
                still |= keys[k] == u;
            if (!still && usage_set1[u]) {
                kbd_key(usage_set1[u], true);
                if (h->rep_code == usage_set1[u])
                    h->rep_code = 0;
            }
        }
    for (int pass = 0; pass < 2; pass++)         /* presses: the modifiers, then the other keys */
        for (int k = 0; k < n; k++) {
            uint8_t u = keys[k];
            if (is_modifier(u) == pass)
                continue;
            bool before = false;
            for (int i = 0; i < h->nkeys; i++)
                before |= h->keys[i] == u;
            if (before || !usage_set1[u])
                continue;
            kbd_key(usage_set1[u], false);
            if (!is_modifier(u) && u != 0x39 && u != 0x53) {   /* (not Caps Lock, Num Lock) */
                h->rep_code = usage_set1[u];
                h->rep_at = hrtime() + REPEAT_DELAY_NS;
            }
        }
    memcpy(h->keys, keys, n);
    h->nkeys = n;
}

static void add_key(uint8_t *keys, int *n, uint32_t u)
{
    if (u < 256 && u >= 4 && *n < HID_MAX_KEYS)
        keys[(*n)++] = u;
}

static void boot_keyboard(struct hid *h, const uint8_t *r, size_t n)
{
    if (n < 3)
        return;
    uint8_t keys[HID_MAX_KEYS];
    int nk = 0;
    for (int i = 0; i < 8; i++)
        if (r[0] & (1 << i))
            add_key(keys, &nk, 0xE0 + i);
    for (size_t i = 2; i < n && i < 8; i++) {
        if (r[i] == 1)
            return;                              /* too many keys (ErrorRollOver): keep the state */
        add_key(keys, &nk, r[i]);
    }
    kbd_update(h, keys, nk);
}

static void pointer(struct hid *h, int dx, int dy, bool abs, int ax, int ay, unsigned buttons)
{
    if (abs)
        input_mouse_abs(ax, ay, buttons);
    else if (dx || dy || buttons != h->buttons)
        input_mouse(dx, dy, buttons);
    h->buttons = buttons;
}

void hid_input(struct hid *h, const uint8_t *r, size_t n)
{
    if (h->debug) {
        kprintf("hid %s:", h->name);
        for (size_t i = 0; i < n && i < 24; i++)
            kprintf(" %02x", r[i]);
        kprintf("\n");
    }
    if (h->boot_kbd) {
        boot_keyboard(h, r, n);
        return;
    }
    if (h->boot_mouse) {
        if (n >= 3)
            pointer(h, (int8_t)r[1], (int8_t)r[2], false, 0, 0, r[0] & 7);
        return;
    }
    uint8_t id = 0;
    if (h->ids) {
        if (!n)
            return;
        id = *r++;
        n--;
    }
    uint8_t keys[HID_MAX_KEYS];
    int nk = 0;
    bool kbd = false, mouse = false, abs = false;
    int dx = 0, dy = 0, ax = 0, ay = 0;
    unsigned buttons = 0;
    for (int i = 0; i < h->nfields; i++) {
        const struct hid_field *f = &h->f[i];
        if (f->report_id != id)
            continue;
        if (f->page == 7 && f->app == HID_APP_KEYBOARD) {
            kbd = true;
            for (int k = 0; k < f->count; k++) {
                int32_t v = field_value(f, r, n, k);
                if (f->flags & 2) {              /* a bitmap: one bit per usage */
                    if (v)
                        add_key(keys, &nk, field_usage(f, k));
                } else if (v >= f->lmin && v <= f->lmax) {   /* an array of usages */
                    uint16_t u = field_usage(f, v - f->lmin);
                    if (u == 1)
                        return;                  /* ErrorRollOver */
                    add_key(keys, &nk, u);
                }
            }
        } else if (f->app == HID_APP_MOUSE && f->page == 9 && (f->flags & 2)) {
            for (int k = 0; k < f->count; k++) {
                uint16_t u = field_usage(f, k);
                if (u >= 1 && u <= 3 && field_value(f, r, n, k))
                    buttons |= 1U << (u - 1);
            }
            mouse = true;
        } else if (f->app == HID_APP_MOUSE && f->page == 1 && (f->flags & 2)) {
            for (int k = 0; k < f->count; k++) {
                uint16_t u = field_usage(f, k);
                if (u != 0x30 && u != 0x31)
                    continue;                    /* (the wheel, and the rest) */
                int32_t v = field_value(f, r, n, k);
                mouse = true;
                if (f->flags & 4) {
                    *(u == 0x30 ? &dx : &dy) = v;
                } else {                         /* absolute: 0..65535 across the screen */
                    int64_t span = (int64_t)f->lmax - f->lmin;
                    int32_t s = span > 0 ? (int32_t)(((int64_t)(v - f->lmin) * 65535) / span) : 0;
                    s = s < 0 ? 0 : s > 65535 ? 65535 : s;
                    *(u == 0x30 ? &ax : &ay) = s;
                    abs = true;
                }
            }
        }
    }
    if (kbd)
        kbd_update(h, keys, nk);
    if (mouse)
        pointer(h, dx, dy, abs, ax, ay, buttons);
}

void hid_tick(struct hid *h)
{
    if (!h->rep_code)
        return;
    uint64_t now = hrtime();
    if (now < h->rep_at)
        return;
    kbd_key(h->rep_code, false);                 /* as a PS/2 keyboard repeats: the press again */
    h->rep_at = now + REPEAT_NS;
}

void hid_release(struct hid *h)
{
    kbd_update(h, NULL, 0);
    if (h->buttons)
        input_mouse(0, 0, 0);
    h->buttons = 0;
    h->rep_code = 0;
}
