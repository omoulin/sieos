/*
 * fb.c - Framebuffer device (/dev/fb0).
 *
 * A process maps the linear framebuffer into its address space with
 * fbmap(fd).  While it does, the text console stops drawing (it still
 * mirrors to the serial port and keeps its cell buffer); when the owner
 * exits, the console repaints the screen.
 */
#include "proc.h"
#include "mm.h"
#include "fs.h"
#include "poll.h"

#define FB_USER_VA 0x0000600000000000UL

static int fb_owner;

int fb_open(void)
{
    struct fb_info fi;
    return console_fb_info(&fi, NULL) ? 0 : -ENODEV;
}

long fb_ioctl(unsigned long cmd, uint64_t arg)
{
    if (cmd != FBIOGET_INFO)
        return -ENOTTY;
    if (!user_range_ok(current->pml4, arg, sizeof(struct fb_info), true))
        return -EFAULT;
    if (!console_fb_info((struct fb_info *)arg, NULL))
        return -ENODEV;
    return 0;
}

long sys_fbmap(int fd)
{
    if (fd < 0 || fd >= NOFILE || !current->ofile[fd] || current->ofile[fd]->type != FD_FB)
        return -EBADF;
    if (fb_owner && fb_owner != current->pid && proc_find(fb_owner))
        return -EBUSY;
    struct fb_info fi;
    uint64_t phys;
    if (!console_fb_info(&fi, &phys))
        return -ENODEV;
    uint64_t size = PAGE_ALIGN_UP((uint64_t)fi.pitch * fi.height);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        int r = vmm_map(current->pml4, FB_USER_VA + off, phys + off, PTE_U | PTE_W | PTE_DEVICE | pte_nx);
        if (r < 0)
            return r;
    }
    fb_owner = current->pid;
    console_suspend(true);
    return FB_USER_VA;
}

void fb_release_owner(int pid)
{
    if (fb_owner && fb_owner == pid) {
        fb_owner = 0;
        console_suspend(false);
    }
}
