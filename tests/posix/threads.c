/*
 * ptest-threads (make posix-test): POSIX threads on the kernel's futexes:
 * 64 threads, a mutex, a condition variable, join with results,
 * thread-local variables, once, read-write locks, semaphores, detached
 * threads, timed waits; and the clocks.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails;
static void check(const char *what, int ok)
{
    printf("ptest threads: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter;
static __thread int mine = 7;

static void *adder(void *a)
{
    for (int i = 0; i < 10000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    mine += (int)(long)a;                              /* each thread its own copy */
    return (void *)(long)mine;
}

static int turn, rounds;
static void *ping(void *a)
{
    int me = (int)(long)a;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&mu);
        while (turn != me) pthread_cond_wait(&cv, &mu);
        turn = !me; rounds++;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    return 0;
}

static int once_n;
static void once_fn(void) { once_n++; }
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void *onced(void *a) { (void)a; pthread_once(&once, once_fn); return 0; }

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static int shared[16];
static void *rwer(void *a)
{
    long k = (long)a, bad = 0;
    for (int i = 0; i < 2000; i++) {
        if (k % 4 == 0) { pthread_rwlock_wrlock(&rw); for (int j = 0; j < 16; j++) shared[j] = i; pthread_rwlock_unlock(&rw); }
        else { pthread_rwlock_rdlock(&rw); for (int j = 1; j < 16; j++) bad += shared[j] != shared[0]; pthread_rwlock_unlock(&rw); }
    }
    return (void *)bad;
}

static sem_t sem;
static volatile int detached_done;
static void *detached(void *a) { (void)a; detached_done = 1; sem_post(&sem); return 0; }

static double secs(struct timespec a, struct timespec b) { return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9; }

int main(void)
{
    pthread_t t[64];
    int okc = 1;
    for (long i = 0; i < 64; i++) okc &= pthread_create(&t[i], 0, adder, (void *)i) == 0;
    long sum = 0;
    for (int i = 0; i < 64; i++) { void *r; okc &= pthread_join(t[i], &r) == 0; sum += (long)r; }
    check("64 threads, one mutex", okc && counter == 640000);
    check("join results, thread-local variables", sum == 64 * 7 + 63 * 64 / 2 && mine == 7);
    pthread_create(&t[0], 0, ping, (void *)0);
    pthread_create(&t[1], 0, ping, (void *)1);
    pthread_join(t[0], 0); pthread_join(t[1], 0);
    check("condition variable: 2000 turns", rounds == 2000);
    for (int i = 0; i < 8; i++) pthread_create(&t[i], 0, onced, 0);
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    check("pthread_once", once_n == 1);
    for (long i = 0; i < 8; i++) pthread_create(&t[i], 0, rwer, (void *)i);
    long bad = 0;
    for (int i = 0; i < 8; i++) { void *r; pthread_join(t[i], &r); bad += (long)r; }
    check("read-write lock", bad == 0);
    sem_init(&sem, 0, 0);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t[0], &at, detached, 0);
    check("a detached thread, a semaphore", sem_wait(&sem) == 0 && detached_done);
    struct timespec a, b, dl;
    clock_gettime(CLOCK_MONOTONIC, &a);
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 50000000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&mu);
    int r = pthread_cond_timedwait(&cv, &mu, &dl);
    pthread_mutex_unlock(&mu);
    clock_gettime(CLOCK_MONOTONIC, &b);
    check("a timed wait times out", r == ETIMEDOUT && secs(a, b) >= 0.045 && secs(a, b) < 1);
    clock_gettime(CLOCK_MONOTONIC, &a);
    nanosleep(&(struct timespec){ 0, 30000000 }, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    check("nanosleep 30 ms", secs(a, b) >= 0.029 && secs(a, b) < 0.5);
    check("the wall clock is after 2026", time(0) > 1767225600);
    printf("ptest threads: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
