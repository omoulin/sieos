/*
 * abi-check.c - compile-time and run-time checks of the ABI v2 headers.
 * Built by "make abi-check" as C (hosted and freestanding) and as C++.
 */
#include "sieos/abi.h"

#define SHOW(t) { #t, sizeof(t) }
static const struct { const char *name; unsigned long size; } sizes[] = {
    SHOW(struct sieos_timespec), SHOW(sieos_sigset_t), SHOW(sieos_siginfo_t),
    SHOW(struct sieos_sigaction), SHOW(sieos_stack_t), SHOW(sieos_mcontext_t),
    SHOW(sieos_ucontext_t), SHOW(struct sieos_stat), SHOW(struct sieos_statvfs),
    SHOW(struct sieos_flock), SHOW(struct sieos_termios), SHOW(struct sieos_rusage),
    SHOW(struct sieos_sockaddr_in), SHOW(struct sieos_msghdr), SHOW(sieos_auxv_t),
    SHOW(struct sieos_sockaddr_un), SHOW(struct sieos_cmsghdr), SHOW(struct sieos_netinfo),
    SHOW(struct sieos_sockinfo), SHOW(struct sieos_cpuinfo), SHOW(struct sieos_meminfo),
    SHOW(struct sieos_procinfo), SHOW(struct sieos_fb_info), SHOW(struct sieos_input_event),
    SHOW(sieos_processor_info_t), SHOW(sieos_lwpsinfo_t), SHOW(sieos_psinfo_t),
    SHOW(sieos_pstatus_t), SHOW(sieos_prcred_t), SHOW(sieos_prusage_t),
};

#ifndef ABI_CHECK_FREESTANDING
#include <stdio.h>
int main(void)
{
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        printf("%-28s %4lu\n", sizes[i].name, sizes[i].size);
    printf("syscalls defined up to %d, errno up to %d, signals 1..%d\n",
           SIEOS_SYS_procinfo, SIEOS_ELAST, SIEOS_NSIG - 1);
    return 0;
}
#else
unsigned long abi_check_count(void) { return sizeof(sizes) / sizeof(sizes[0]); }
#endif
