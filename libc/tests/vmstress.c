/*
 * vmstress.c - page faults in parallel (M18: faults run without the kernel
 * lock).  Threads fault in their own slices of one mapping while others fork
 * (copy-on-write both ways), mprotect and munmap/mmap scratch areas; every
 * page is checked.  Then the time to fault in the same memory with 1 and
 * with N threads.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); __atomic_add_fetch(&fails, 1, __ATOMIC_RELAXED); } } while (0)

#define PG 4096
#define NT 4
#define SLICE (8u << 20)              /* per thread */
#define ROUNDS 6

static char *big;
static volatile int stop;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Fill the slice page by page (demand-zero faults), check it, rewrite it
 * after a fork has made it copy-on-write, and check again. */
static void *toucher(void *arg)
{
    long id = (long)arg;
    char *s = big + id * SLICE;
    for (int r = 0; r < ROUNDS; r++) {
        uint32_t tag = (uint32_t)(id << 24 | r << 16);
        for (size_t o = 0; o < SLICE; o += PG)
            *(uint32_t *)(s + o) = tag | (uint32_t)(o / PG);
        int bad = 0;
        for (size_t o = 0; o < SLICE; o += PG)
            bad += *(uint32_t *)(s + o) != (tag | (uint32_t)(o / PG));
        T(bad == 0);
        if (r % 2)                   /* drop the pages: they come back zero */
            T(madvise(s, SLICE, MADV_DONTNEED) == 0 && *(volatile uint32_t *)(s + PG) == 0);
    }
    return 0;
}

/* Fork while the touchers run: the child sees a consistent snapshot of
 * each page (our tag or zero) and its own writes stay its own. */
static void *forker(void *arg)
{
    (void)arg;
    int n = 0;
    while (!stop) {
        pid_t pid = fork();
        if (pid == 0) {
            int bad = 0;
            for (size_t o = 0; o < (size_t)NT * SLICE; o += 16 * PG) {
                uint32_t v = *(volatile uint32_t *)(big + o);
                if (v && (v & 0xffff) != (o % SLICE) / PG)
                    bad++;
                *(uint32_t *)(big + o) = 0xdeadbeef;
            }
            _exit(bad ? 1 : 0);
        }
        int st;
        T(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        n++;
    }
    return (void *)(long)n;
}

/* Map, fault in, protect and unmap scratch areas next to the others. */
static void *mapper(void *arg)
{
    (void)arg;
    int n = 0;
    while (!stop) {
        char *m = mmap(0, 64 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        T(m != MAP_FAILED);
        for (int i = 0; i < 64; i++)
            m[i * PG] = (char)i;
        T(mprotect(m, 32 * PG, PROT_READ) == 0 && m[31 * PG] == 31);
        T(munmap(m, 64 * PG) == 0);
        n++;
    }
    return (void *)(long)n;
}

static void *faulter(void *arg)
{
    char *s = arg;
    for (size_t o = 0; o < SLICE; o += PG)
        s[o] = 1;
    return 0;
}

/* Fault in NT slices with n threads; seconds. */
static double timed(int n)
{
    char *m = mmap(0, (size_t)NT * SLICE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    T(m != MAP_FAILED);
    pthread_t t[NT];
    double t0 = now();
    for (int k = 0; k < NT; k += n) {
        for (int i = 0; i < n; i++)
            pthread_create(&t[i], 0, faulter, m + (size_t)(k + i) * SLICE);
        for (int i = 0; i < n; i++)
            pthread_join(t[i], 0);
    }
    double dt = now() - t0;
    munmap(m, (size_t)NT * SLICE);
    return dt;
}

/* The same with n processes, each in its own address space. */
static double timed_procs(int n)
{
    char *m = mmap(0, (size_t)NT * SLICE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    T(m != MAP_FAILED);
    double t0 = now();
    pid_t pid[NT];
    for (int i = 0; i < n; i++)
        if ((pid[i] = fork()) == 0) {
            for (int k = i; k < NT; k += n)
                faulter(m + (size_t)k * SLICE);
            _exit(0);
        }
    for (int i = 0; i < n; i++)
        waitpid(pid[i], 0, 0);
    double dt = now() - t0;
    munmap(m, (size_t)NT * SLICE);
    return dt;
}

int main(void)
{
    big = mmap(0, (size_t)NT * SLICE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    T(big != MAP_FAILED);
    pthread_t t[NT], f, m;
    pthread_create(&f, 0, forker, 0);
    pthread_create(&m, 0, mapper, 0);
    for (long i = 0; i < NT; i++)
        pthread_create(&t[i], 0, toucher, (void *)i);
    for (int i = 0; i < NT; i++)
        pthread_join(t[i], 0);
    stop = 1;
    void *nf, *nm;
    pthread_join(f, &nf);
    pthread_join(m, &nm);
    printf("vmstress: %d threads x %d rounds of %u MiB, %ld forks, %ld map cycles\n", NT, ROUNDS, SLICE >> 20,
           (long)nf, (long)nm);
    T((long)nf > 0 && (long)nm > 0);

    double t1 = timed(1), tn = timed(NT);
    printf("fault in %u MiB: 1 thread %.3f s, %d threads %.3f s (%.1fx)\n", (NT * SLICE) >> 20, t1, NT, tn, t1 / tn);
    t1 = timed_procs(1), tn = timed_procs(NT);
    printf("fault in %u MiB: 1 process %.3f s, %d processes %.3f s (%.1fx)\n", (NT * SLICE) >> 20, t1, NT, tn, t1 / tn);
    printf("vmstress: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
