/*
 * m18.c - scheduling classes: priocntl, nice/setpriority, sched_* and
 * pthread scheduling, RT preemption, TS fairness.  Run as root.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "sieos/syscall.h"
#include "sieos/priocntl.h"
#include "sieos/wait.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

static long pc(long idtype, long id, long cmd, void *arg)
{
    return syscall(SIEOS_SYS_priocntl, idtype, id, cmd, arg);
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void classes(void)
{
    sieos_pcinfo_t pi;
    memset(&pi, 0, sizeof(pi));
    strcpy(pi.pc_clname, "RT");
    T(pc(0, 0, SIEOS_PC_GETCID, &pi) >= 0 && pi.pc_cid == SIEOS_CID_RT && pi.pc_clinfo[0] == 59);
    memset(&pi, 0, sizeof(pi));
    pi.pc_cid = SIEOS_CID_TS;
    T(pc(0, 0, SIEOS_PC_GETCLINFO, &pi) >= 0 && !strcmp(pi.pc_clname, "TS"));
    sieos_pcparms_t pp;
    pp.pc_cid = SIEOS_PC_CLNULL;
    T(pc(SIEOS_P_PID, SIEOS_P_MYID, SIEOS_PC_GETPARMS, &pp) == 0 && pp.pc_cid == SIEOS_CID_TS);
}

static void nices(void)
{
    errno = 0;
    int p0 = getpriority(PRIO_PROCESS, 0);
    T(errno == 0 && p0 == 0);
    T(setpriority(PRIO_PROCESS, 0, 10) == 0 && getpriority(PRIO_PROCESS, 0) == 10);
    pid_t pid = fork();
    if (pid == 0) {                                  /* inherited; a user may not go back down */
        int ok = getpriority(PRIO_PROCESS, 0) == 10;
        setuid(100);
        ok &= setpriority(PRIO_PROCESS, 0, 0) < 0;
        ok &= setpriority(PRIO_PROCESS, 0, 15) == 0;
        _exit(ok ? 0 : 1);
    }
    int st;
    T(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    T(setpriority(PRIO_PROCESS, 0, 0) == 0);        /* root may */
    T(nice(3) == 3 && nice(-3) == 0);
}

static void policies(void)
{
    struct sched_param sp = { .sched_priority = 10 };
    T(sched_get_priority_max(SCHED_FIFO) == 59 && sched_get_priority_min(SCHED_RR) == 0);
    T(sched_setscheduler(0, SCHED_RR, &sp) == 0 && sched_getscheduler(0) == SCHED_RR);
    struct sched_param got;
    T(sched_getparam(0, &got) == 0 && got.sched_priority == 10);
    struct timespec q;
    T(sched_rr_get_interval(0, &q) == 0 && q.tv_nsec == 100000000);
    sp.sched_priority = 0;
    T(sched_setscheduler(0, SCHED_OTHER, &sp) == 0 && sched_getscheduler(0) == SCHED_OTHER);
    int pol;
    sp.sched_priority = 5;
    T(pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0);
    T(pthread_getschedparam(pthread_self(), &pol, &got) == 0 && pol == SCHED_FIFO && got.sched_priority == 5);
    sp.sched_priority = 0;
    T(pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) == 0);
    pid_t pid = fork();
    if (pid == 0) {                                  /* users may not enter RT */
        setuid(100);
        sp.sched_priority = 1;
        _exit(sched_setscheduler(0, SCHED_FIFO, &sp) < 0 && errno == EPERM ? 0 : 1);
    }
    int st;
    T(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* On every CPU a TS busy loop; an RT process that wakes every 5 ms must still run on time. */
static void rt_preempts(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    pid_t hogs[16];
    for (int i = 0; i < ncpu && i < 16; i++)
        if ((hogs[i] = fork()) == 0)
            for (;;)
                ;
    pid_t rt = fork();
    if (rt == 0) {
        struct sched_param sp = { .sched_priority = 30 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
            _exit(2);
        long worst = 0;
        for (int i = 0; i < 100; i++) {
            long t0 = now_ms();
            struct timespec d = { 0, 5000000 };
            nanosleep(&d, 0);
            long late = now_ms() - t0 - 5;
            if (late > worst)
                worst = late;
        }
        printf("RT under %ld busy TS processes: worst wake-up latency %ld ms\n", ncpu, worst);
        _exit(worst <= 3 ? 0 : 1);
    }
    int st;
    T(waitpid(rt, &st, 0) == rt && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    for (int i = 0; i < ncpu && i < 16; i++)
        kill(hogs[i], SIGKILL), waitpid(hogs[i], 0, 0);
}

/* Two CPU-bound TS processes on one CPU share it. */
static void ts_fair(void)
{
    int fd[2];
    pipe(fd);
    pid_t k[2];
    for (int i = 0; i < 2; i++)
        if ((k[i] = fork()) == 0) {
            syscall(SIEOS_SYS_processor_bind, SIEOS_P_PID, SIEOS_P_MYID, 0, 0);   /* both on CPU 0 */
            long n = 0, end = now_ms() + 1500;
            while (now_ms() < end)
                n++;
            write(fd[1], &n, sizeof(n));
            _exit(0);
        }
    long n[2];
    for (int i = 0; i < 2; i++)
        read(fd[0], &n[i], sizeof(n[i])), waitpid(k[i], 0, 0);
    double r = (double)(n[0] < n[1] ? n[0] : n[1]) / (double)(n[0] > n[1] ? n[0] : n[1]);
    printf("TS fairness on one CPU: %.2f\n", r);
    T(r > 0.6);
}

int main(void)
{
    classes();
    nices();
    policies();
    rt_preempts();
    ts_fair();
    printf("m18: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
