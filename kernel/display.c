/*
 * display.c - Displays (/dev/fb0, /dev/fb1, ...) and the firmware
 * framebuffer driver.
 *
 * The framebuffer GRUB or the UEFI firmware set up, if there is one, is
 * registered first; the text console draws on it.  Native drivers probe
 * afterwards, the physical cards' before QEMU's: one for the same device
 * takes that display over (display_takeover), others add displays.  Then
 * the displays are put in order - physical cards, virtual cards, a
 * firmware framebuffer left without a driver - and the console moves to
 * display 0.  A process maps a display with fbmap(fd), all of the memory
 * the display can scan out, write-combining; while it does, it owns the
 * display: the console stops drawing there, and only the owner may change
 * the mode.  When the owner exits, the console repaints.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "display.h"
#include "vm.h"
#include "proc.h"
#include "mm.h"
#include "fs.h"
#include "cpu.h"
#include "abi2.h"

#define FB_USER_VA   0x0000600000000000UL
#define FB_USER_SPAN (16UL << 30)             /* per display: fbN at FB_USER_VA + N * 16 GiB */

struct display displays[DISPLAY_MAX];
int ndisplays;

static const struct display_ops firmware_ops = { "firmware", DISPLAY_FIRMWARE, NULL, NULL };

struct display *display_register(const struct display_ops *ops, void *drv, const char *desc,
                                 const struct display_mode *m, uint64_t fb_phys, uint64_t fb_size)
{
    if (ndisplays == DISPLAY_MAX)
        return NULL;
    struct display *d = &displays[ndisplays];
    memset(d, 0, sizeof(*d));
    d->index = ndisplays++;
    d->ops = ops;
    d->drv = drv;
    strlcpy(d->desc, desc, sizeof(d->desc));
    d->mode = *m;
    d->red_pos = 16;
    d->green_pos = 8;
    d->blue_pos = 0;
    d->fb_phys = fb_phys;
    d->fb_size = PAGE_ALIGN_UP(fb_size);
    d->fb = mmio_map_wc(fb_phys, d->fb_size);
    d->present = d->fb != NULL;
    return d;
}

struct display *display_takeover(uint64_t lo, uint64_t hi, const struct display_ops *ops, void *drv,
                                 const char *desc, const struct display_mode *m, uint64_t fb_phys,
                                 uint64_t fb_size)
{
    for (int i = 0; i < ndisplays; i++) {
        struct display *d = &displays[i];
        if (d->ops != &firmware_ops || d->fb_phys < lo || d->fb_phys >= hi)
            continue;
        void *fb = mmio_map_wc(fb_phys, PAGE_ALIGN_UP(fb_size));
        if (!fb)
            return NULL;
        d->ops = ops;                           /* the same screen, now with its own driver */
        d->drv = drv;
        strlcpy(d->desc, desc, sizeof(d->desc));
        d->fb_phys = fb_phys;
        d->fb_size = PAGE_ALIGN_UP(fb_size);
        d->fb = fb;
        if (m->width != d->mode.width || m->height != d->mode.height || m->pitch != d->mode.pitch) {
            d->mode = *m;
            display_mode_changed(d);
        } else {
            d->mode = *m;
            if (d->console)
                console_fb_remap(d->fb);
        }
        return d;
    }
    return display_register(ops, drv, desc, m, fb_phys, fb_size);
}

void display_mode_changed(struct display *d)
{
    if (d->console)
        console_fb_mode(d->fb, d->mode.width, d->mode.height, d->mode.pitch);
}

/* Number the displays by kind (a stable sort: each kind keeps the probe
 * order) and put the console on display 0.  Before any process opens one. */
void display_order(void)
{
    for (int i = 1; i < ndisplays; i++)
        for (int k = i; k > 0 && displays[k].ops->kind < displays[k - 1].ops->kind; k--) {
            struct display t = displays[k];
            displays[k] = displays[k - 1];
            displays[k - 1] = t;
        }
    for (int i = 0; i < ndisplays; i++)
        displays[i].index = i;
    struct display *d0 = &displays[0];
    if (ndisplays > 1 && d0->present && !d0->console) {
        for (int i = 1; i < ndisplays; i++)
            displays[i].console = false;
        d0->console = true;
        console_fb_mode(d0->fb, d0->mode.width, d0->mode.height, d0->mode.pitch);
    }
}

void display_init(void)
{
    struct fb_info fi;
    uint64_t phys;
    if (console_fb_info(&fi, &phys)) {
        struct display_mode m = { fi.width, fi.height, fi.pitch, 0, true };
        char desc[48];
        snprintf(desc, sizeof(desc), "boot framebuffer %ux%u", fi.width, fi.height);
        struct display *d = display_register(&firmware_ops, NULL, desc, &m, phys, (uint64_t)fi.pitch * fi.height);
        if (d) {
            d->red_pos = fi.red_pos;
            d->green_pos = fi.green_pos;
            d->blue_pos = fi.blue_pos;
            d->console = true;
            console_fb_remap(d->fb);            /* write-combining from now on */
        }
    }
    /* (then the displays' drivers, loaded with the console: modules_attach(DDI_PHASE_DISPLAY), display_order) */
}

/* ------------------------------------------------------------------ */
/* /dev/fbN                                                            */
/* ------------------------------------------------------------------ */

static struct display *display_of(int minor)
{
    return minor >= 0 && minor < ndisplays && displays[minor].present ? &displays[minor] : NULL;
}

int fb_open(int minor)
{
    return display_of(minor) ? 0 : -ENODEV;
}

static int owner_of(struct display *d)
{
    if (d->owner && !proc_find(d->owner))
        d->owner = 0;
    return d->owner;
}

static int list_modes(struct display *d, struct display_mode *out, int max)
{
    int n = d->ops->modes ? d->ops->modes(d, out, max) : 0;
    if (n <= 0) {
        out[0] = d->mode;
        n = 1;
    }
    return n;
}

long fb_ioctl(struct file *f, unsigned long cmd, uint64_t arg)
{
    struct display *d = display_of(f->minor);
    if (!d)
        return -ENODEV;
    switch (cmd) {
    case SIEOS_FBIOGET_INFO: {
        if (!user_range_ok(current->pml4, arg, sizeof(struct sieos_fb_info), true))
            return -EFAULT;
        struct sieos_fb_info fi = { d->mode.width, d->mode.height, d->mode.pitch, 32, d->red_pos, d->green_pos,
                                    d->blue_pos, 0 };
        memcpy((void *)arg, &fi, sizeof(fi));
        return 0;
    }
    case SIEOS_FBIOGET_DISPLAY: {
        if (!user_range_ok(current->pml4, arg, sizeof(struct sieos_fb_display), true))
            return -EFAULT;
        struct display_mode *ms = kmalloc(sizeof(*ms) * SIEOS_FB_MODES_MAX);
        if (!ms)
            return -ENOMEM;
        struct sieos_fb_display di;
        memset(&di, 0, sizeof(di));
        strlcpy(di.driver, d->ops->name, sizeof(di.driver));
        strlcpy(di.desc, d->desc, sizeof(di.desc));
        di.index = d->index;
        di.flags = (d->ops->set_mode ? SIEOS_FB_SETMODE : 0) | (d->console ? SIEOS_FB_CONSOLE : 0);
        di.nmodes = list_modes(d, ms, SIEOS_FB_MODES_MAX);
        di.owner = owner_of(d);
        di.vram = d->fb_size;
        kfree(ms);
        memcpy((void *)arg, &di, sizeof(di));
        return 0;
    }
    case SIEOS_FBIOGET_MODES: {
        if (!user_range_ok(current->pml4, arg, sizeof(struct sieos_fb_modes), true))
            return -EFAULT;
        struct display_mode *ms = kmalloc(sizeof(*ms) * SIEOS_FB_MODES_MAX);
        struct sieos_fb_modes *out = kmalloc(sizeof(*out));
        if (!ms || !out) {
            kfree(ms);
            kfree(out);
            return -ENOMEM;
        }
        memset(out, 0, sizeof(*out));
        int n = list_modes(d, ms, SIEOS_FB_MODES_MAX);
        for (int i = 0; i < n; i++) {
            out->mode[i].width = ms[i].width;
            out->mode[i].height = ms[i].height;
            out->mode[i].refresh = ms[i].refresh;
            out->mode[i].flags = (ms[i].preferred ? SIEOS_FB_MODE_PREFERRED : 0) |
                                 (ms[i].width == d->mode.width && ms[i].height == d->mode.height
                                      ? SIEOS_FB_MODE_CURRENT : 0);
        }
        out->n = n;
        memcpy((void *)arg, out, sizeof(*out));
        kfree(ms);
        kfree(out);
        return 0;
    }
    case SIEOS_FBIOSET_MODE: {
        if (!user_range_ok(current->pml4, arg, sizeof(struct sieos_fb_mode), false))
            return -EFAULT;
        struct sieos_fb_mode want;
        memcpy(&want, (void *)arg, sizeof(want));
        if (!(f->flags & O_WRONLY) && !(f->flags & O_RDWR))
            return -EBADF;
        if (!d->ops->set_mode)
            return -ENOTSUP_K;
        int own = owner_of(d);
        if (own && own != current->pid)
            return -EBUSY;
        struct display_mode *ms = kmalloc(sizeof(*ms) * SIEOS_FB_MODES_MAX);
        if (!ms)
            return -ENOMEM;
        int n = list_modes(d, ms, SIEOS_FB_MODES_MAX), found = -1;
        for (int i = 0; i < n; i++)
            if (ms[i].width == want.width && ms[i].height == want.height)
                found = i;
        int r = -EINVAL;
        if (found >= 0) {
            struct display_mode m = ms[found];
            r = d->ops->set_mode(d, &m);
            if (r == 0)
                display_mode_changed(d);
        }
        kfree(ms);
        return r;
    }
    }
    return -ENOTTY;
}

long sys_fbmap(int fd)
{
    if (fd < 0 || fd >= NOFILE || !current->ofile[fd] || current->ofile[fd]->type != FD_FB)
        return -EBADF;
    struct display *d = display_of(current->ofile[fd]->minor);
    if (!d)
        return -ENODEV;
    int own = owner_of(d);
    if (own && own != current->pid)
        return -EBUSY;
    uint64_t va = FB_USER_VA + (uint64_t)d->index * FB_USER_SPAN;
    uint64_t cache = pat_wc ? PTE_WC : 0;
    for (uint64_t off = 0; off < d->fb_size; off += PAGE_SIZE) {
        vm_space_lock(current);
        int r = vmm_map(current->pml4, va + off, d->fb_phys + off, PTE_U | PTE_W | PTE_DEVICE | cache | pte_nx);
        vm_space_unlock(current);
        if (r < 0)
            return r;
    }
    d->owner = current->pid;
    if (d->console)
        console_suspend(true);
    return va;
}

void fb_release_owner(int pid)
{
    for (int i = 0; i < ndisplays; i++) {
        struct display *d = &displays[i];
        if (d->owner && d->owner == pid) {
            d->owner = 0;
            if (d->console)
                console_suspend(false);
        }
    }
}
