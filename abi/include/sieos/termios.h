/*
 * sieos/termios.h - terminal interface and ioctl requests (ABI v2).
 * SVR4/Solaris values (octal flag constants, VMIN == VEOF, VTIME == VEOL).
 */
#ifndef SIEOS_ABI_TERMIOS_H
#define SIEOS_ABI_TERMIOS_H

#include "types.h"

#define SIEOS_NCCS 19

struct sieos_termios {
    sieos_tcflag_t c_iflag;
    sieos_tcflag_t c_oflag;
    sieos_tcflag_t c_cflag;
    sieos_tcflag_t c_lflag;
    sieos_cc_t     c_cc[SIEOS_NCCS];
};

/* c_cc indices */
#define SIEOS_VINTR    0
#define SIEOS_VQUIT    1
#define SIEOS_VERASE   2
#define SIEOS_VKILL    3
#define SIEOS_VEOF     4
#define SIEOS_VEOL     5
#define SIEOS_VEOL2    6
#define SIEOS_VMIN     4
#define SIEOS_VTIME    5
#define SIEOS_VSWTCH   7
#define SIEOS_VSTART   8
#define SIEOS_VSTOP    9
#define SIEOS_VSUSP   10
#define SIEOS_VDSUSP  11
#define SIEOS_VREPRINT 12
#define SIEOS_VDISCARD 13
#define SIEOS_VWERASE 14
#define SIEOS_VLNEXT  15

/* c_iflag */
#define SIEOS_IGNBRK  0000001
#define SIEOS_BRKINT  0000002
#define SIEOS_IGNPAR  0000004
#define SIEOS_PARMRK  0000010
#define SIEOS_INPCK   0000020
#define SIEOS_ISTRIP  0000040
#define SIEOS_INLCR   0000100
#define SIEOS_IGNCR   0000200
#define SIEOS_ICRNL   0000400
#define SIEOS_IUCLC   0001000
#define SIEOS_IXON    0002000
#define SIEOS_IXANY   0004000
#define SIEOS_IXOFF   0010000
#define SIEOS_IMAXBEL 0020000

/* c_oflag */
#define SIEOS_OPOST   0000001
#define SIEOS_OLCUC   0000002
#define SIEOS_ONLCR   0000004
#define SIEOS_OCRNL   0000010
#define SIEOS_ONOCR   0000020
#define SIEOS_ONLRET  0000040
#define SIEOS_TABDLY  0014000
#define SIEOS_XTABS   0014000

/* c_cflag */
#define SIEOS_CBAUD   0000017
#define SIEOS_CSIZE   0000060
#define SIEOS_CS5     0000000
#define SIEOS_CS6     0000020
#define SIEOS_CS7     0000040
#define SIEOS_CS8     0000060
#define SIEOS_CSTOPB  0000100
#define SIEOS_CREAD   0000200
#define SIEOS_PARENB  0000400
#define SIEOS_PARODD  0001000
#define SIEOS_HUPCL   0002000
#define SIEOS_CLOCAL  0004000
#define SIEOS_B0      0
#define SIEOS_B9600   13
#define SIEOS_B19200  14
#define SIEOS_B38400  15

/* c_lflag */
#define SIEOS_ISIG    0000001
#define SIEOS_ICANON  0000002
#define SIEOS_XCASE   0000004
#define SIEOS_ECHO    0000010
#define SIEOS_ECHOE   0000020
#define SIEOS_ECHOK   0000040
#define SIEOS_ECHONL  0000100
#define SIEOS_NOFLSH  0000200
#define SIEOS_TOSTOP  0000400
#define SIEOS_ECHOCTL 0001000
#define SIEOS_ECHOPRT 0002000
#define SIEOS_ECHOKE  0004000
#define SIEOS_FLUSHO  0020000
#define SIEOS_PENDIN  0040000
#define SIEOS_IEXTEN  0100000

struct sieos_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* ioctl requests: termio ('T') */
#define SIEOS__TIOC   ('T' << 8)
#define SIEOS_TCGETA  (SIEOS__TIOC | 1)
#define SIEOS_TCSBRK  (SIEOS__TIOC | 5)
#define SIEOS_TCXONC  (SIEOS__TIOC | 6)
#define SIEOS_TCFLSH  (SIEOS__TIOC | 7)
#define SIEOS_TCGETS  (SIEOS__TIOC | 13)
#define SIEOS_TCSETS  (SIEOS__TIOC | 14)
#define SIEOS_TCSETSW (SIEOS__TIOC | 15)
#define SIEOS_TCSETSF (SIEOS__TIOC | 16)
#define SIEOS_TIOCSWINSZ (SIEOS__TIOC | 103)
#define SIEOS_TIOCGWINSZ (SIEOS__TIOC | 104)

/* BSD-style tty ioctls ('t') */
#define SIEOS_tIOC       ('t' << 8)
#define SIEOS_TIOCGPGRP  (SIEOS_tIOC | 20)
#define SIEOS_TIOCSPGRP  (SIEOS_tIOC | 21)
#define SIEOS_TIOCGSID   (SIEOS_tIOC | 22)
#define SIEOS_TIOCNOTTY  (SIEOS_tIOC | 113)
#define SIEOS_TIOCSCTTY  (SIEOS_tIOC | 132)

/* pseudo-terminals ('P'): used by grantpt/unlockpt/ptsname on /dev/ptmx */
#define SIEOS__PTIOC    ('P' << 8)
#define SIEOS_ISPTM     (SIEOS__PTIOC | 1)
#define SIEOS_UNLKPT    (SIEOS__PTIOC | 2)
#define SIEOS_PTSNAME   (SIEOS__PTIOC | 3)     /* arg: char buf[32] */

/* generic file ioctls ('f'), _IOR/_IOW encoded as on Solaris */
#define SIEOS_FIONREAD  0x4004667F
#define SIEOS_FIONBIO   0x8004667E
#define SIEOS_FIOCLEX   0x20006601
#define SIEOS_FIONCLEX  0x20006602

/* tcflush()/tcflow() arguments */
#define SIEOS_TCIFLUSH  0
#define SIEOS_TCOFLUSH  1
#define SIEOS_TCIOFLUSH 2
#define SIEOS_TCOOFF    0
#define SIEOS_TCOON     1
#define SIEOS_TCIOFF    2
#define SIEOS_TCION     3

SIEOS_STATIC_ASSERT(sizeof(struct sieos_termios) == 36, "termios size");

#endif
