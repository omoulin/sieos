/*
 * cdev.c - Character devices of the drivers.
 *
 * A driver registers its major number with an open function, which makes the
 * opened file an FD_OPS file (its file_ops: read, write, poll, close, and for
 * devices ioctl and mmap's pages) and gives it its state (f->priv); then it
 * makes its nodes (dev_node: /dev/NAME, created if missing).  The majors the
 * kernel itself has (ttys, the framebuffer, block devices...) stay in fsys.c.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"

#define NCDEV 16
static struct { int major; int (*open)(struct file *f, int minor); } cdevs[NCDEV];
static int ncdevs;

int cdev_register(int major, int (*open)(struct file *f, int minor))
{
    for (int i = 0; i < ncdevs; i++)
        if (cdevs[i].major == major)
            return -EEXIST;
    if (ncdevs == NCDEV)
        return -ENOSPC;
    cdevs[ncdevs].major = major;
    cdevs[ncdevs].open = open;
    ncdevs++;
    return 0;
}

int cdev_open(struct file *f, uint32_t dev)
{
    for (int i = 0; i < ncdevs; i++)
        if (cdevs[i].major == (int)MAJOR(dev))
            return cdevs[i].open(f, MINOR(dev));
    return -ENXIO;
}
