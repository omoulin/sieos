/*
 * ksig.h - Kernel signal numbering.
 *
 * Inside the kernel, signals use the Solaris numbers of ABI v2
 * (SIGUSR1 = 16, SIGCHLD = 18, SIGSTOP = 23, real-time 42..73) and 128-bit
 * sets.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_KSIG_H
#define SIEOS_KSIG_H

#include "abi.h"                      /* v1 definitions: undone below */
#include "sieos/signal.h"

#undef SIGHUP
#undef SIGINT
#undef SIGQUIT
#undef SIGILL
#undef SIGTRAP
#undef SIGABRT
#undef SIGBUS
#undef SIGFPE
#undef SIGKILL
#undef SIGUSR1
#undef SIGSEGV
#undef SIGUSR2
#undef SIGPIPE
#undef SIGALRM
#undef SIGTERM
#undef SIGCHLD
#undef SIGCONT
#undef SIGSTOP
#undef SIGTSTP
#undef SIGTTIN
#undef SIGTTOU
#undef SIGURG
#undef SIGWINCH
#undef SIGSYS
#undef NSIG
#undef SIGBIT

#define SIGHUP    SIEOS_SIGHUP
#define SIGINT    SIEOS_SIGINT
#define SIGQUIT   SIEOS_SIGQUIT
#define SIGILL    SIEOS_SIGILL
#define SIGTRAP   SIEOS_SIGTRAP
#define SIGABRT   SIEOS_SIGABRT
#define SIGEMT    SIEOS_SIGEMT
#define SIGFPE    SIEOS_SIGFPE
#define SIGKILL   SIEOS_SIGKILL
#define SIGBUS    SIEOS_SIGBUS
#define SIGSEGV   SIEOS_SIGSEGV
#define SIGSYS    SIEOS_SIGSYS
#define SIGPIPE   SIEOS_SIGPIPE
#define SIGALRM   SIEOS_SIGALRM
#define SIGTERM   SIEOS_SIGTERM
#define SIGUSR1   SIEOS_SIGUSR1
#define SIGUSR2   SIEOS_SIGUSR2
#define SIGCHLD   SIEOS_SIGCHLD
#define SIGPWR    SIEOS_SIGPWR
#define SIGWINCH  SIEOS_SIGWINCH
#define SIGURG    SIEOS_SIGURG
#define SIGPOLL   SIEOS_SIGPOLL
#define SIGSTOP   SIEOS_SIGSTOP
#define SIGTSTP   SIEOS_SIGTSTP
#define SIGCONT   SIEOS_SIGCONT
#define SIGTTIN   SIEOS_SIGTTIN
#define SIGTTOU   SIEOS_SIGTTOU
#define SIGVTALRM SIEOS_SIGVTALRM
#define SIGPROF   SIEOS_SIGPROF
#define SIGXCPU   SIEOS_SIGXCPU
#define SIGXFSZ   SIEOS_SIGXFSZ
#define KNSIG     SIEOS_NSIG          /* signals are 1 .. KNSIG - 1 */

typedef unsigned __int128 ksigset_t;
#define KSIGBIT(s) ((ksigset_t)1 << (s))

/* The information delivered with a signal (siginfo). */
struct ksiginfo {
    int code;                          /* SIEOS_SI_* or a CLD_/SEGV_... code */
    int pid, uid;
    int status;
    uint64_t value;                    /* sigqueue value */
    uint64_t addr;                     /* faulting address */
};

/* ABI v1 translation (signal.c) */

#endif
