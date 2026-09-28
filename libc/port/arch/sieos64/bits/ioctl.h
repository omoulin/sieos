/* SIEOS: Solaris ioctl encoding and terminal/file requests (from the ABI headers). */
#define IOCPARM_MASK 0xff
#define IOC_VOID  0x20000000
#define IOC_OUT   0x40000000
#define IOC_IN    0x80000000
#define IOC_INOUT (IOC_IN | IOC_OUT)
#define _IOC(inout, group, num, len) ((inout) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num))
#define _IO(g, n)      _IOC(IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)  _IOC(IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)  _IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))

@DEFINES termios.h:_TIOC|tIOC|_PTIOC@
@DEFINES termios.h:TC(GETA|SBRK|XONC|FLSH|GETS|SETS|SETSW|SETSF)@
@DEFINES termios.h:TIOC(SWINSZ|GWINSZ|GPGRP|SPGRP|GSID|NOTTY|SCTTY)@
@DEFINES termios.h:ISPTM|UNLKPT|PTSNAME@
@DEFINES termios.h:FIO(NREAD|NBIO|CLEX|NCLEX)@
#define TIOCINQ FIONREAD

#define TIOCM_LE        0x001
#define TIOCM_DTR       0x002
#define TIOCM_RTS       0x004
#define TIOCM_ST        0x008
#define TIOCM_SR        0x010
#define TIOCM_CTS       0x020
#define TIOCM_CAR       0x040
#define TIOCM_RNG       0x080
#define TIOCM_DSR       0x100
#define TIOCM_CD        TIOCM_CAR
#define TIOCM_RI        TIOCM_RNG

#define SIOCATMARK _IOR('s', 7, int)
#define SIOCSPGRP  _IOW('s', 8, int)
#define SIOCGPGRP  _IOR('s', 9, int)
