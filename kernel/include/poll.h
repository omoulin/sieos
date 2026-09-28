#ifndef SIEOS_POLL_H
#define SIEOS_POLL_H

#include "kernel.h"
#include "abi.h"

void poll_wakeup(void);                       /* something may have become ready */
long sys_poll(struct pollfd *fds, int nfds, int timeout_ms);

/* input.c */
void input_init(void);
bool input_grabbed(void);
void input_key(uint16_t code, bool pressed, char ascii, unsigned mods);
long input_read(char *buf, size_t n);
bool input_readable(void);
bool input_absolute(void);
int  input_open(void);
void input_close(void);

/* fb.c */
int  fb_open(void);
void fb_close(void);
long fb_ioctl(unsigned long cmd, uint64_t arg);
long sys_fbmap(int fd);
void fb_release_owner(int pid);

#endif
