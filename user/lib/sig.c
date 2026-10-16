/*
 * sig.c - Signals for programs (mk/abi.h): signal() and raise(). The
 * kernel calls one function for every caught signal: sig_entry, which
 * calls the program's, then SYS_SIGRETURN takes the thread back where the
 * signal found it (the kernel kept its registers).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static sighandler_t handlers[NSIG];

static void sig_entry(int sig)
{
    sighandler_t fn = handlers[sig];
    if ((uintptr_t)fn > (uintptr_t)SIG_IGN) fn(sig);   /* (SIG_DFL meanwhile: the kernel decides) */
    SYS(SYS_SIGRETURN, 0, 0);
    __builtin_unreachable();
}

sighandler_t signal(int sig, sighandler_t fn)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL) return SIG_ERR;
    sighandler_t old = handlers[sig];
    handlers[sig] = fn;
    SYS(SYS_SIGNAL, sig, (uintptr_t)fn > (uintptr_t)SIG_IGN ? (sighandler_t)sig_entry : fn);
    return old;
}

int raise(int sig)
{
    mk_ident_t id;
    return sys_ident(0, &id) ? -1 : (int)kill(id.pid, sig);
}
