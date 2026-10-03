/*
 * port.h - Event ports (port.c) and POSIX timers (ptimer.c), for the rest
 * of the kernel.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_PORT_H
#define SIEOS_PORT_H

#include "kernel.h"

struct port;
struct inode;
struct fs;
struct proc;
struct trapframe;

struct port *port_of_fd(int fd);                 /* the caller's port on fd, held, or NULL */
void port_rele(struct port *pt);
bool port_post_timer(struct port *pt, int expirations, int timerid, void *user);
long sys2_portfs(struct trapframe *tf);

/* File Event Notification: the VFS tells what happened to a file (SIEOS_FILE_*) */
void fem_notify(struct inode *ip, int events);
bool fem_any(void);                              /* a file is watched (the hooks that look up names first) */
void fem_unmount(struct fs *fs);

/* POSIX timers (ptimer.c) */
long sys2_timer(struct trapframe *tf, bool *handled);
void ptimer_tick(void);                          /* the clock thread: expirations */
void ptimer_proc_exit(struct proc *p);           /* exit and exec: the process's timers go */

#endif
