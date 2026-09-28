#ifndef _THREAD_H
#define _THREAD_H
/* Solaris threads, on POSIX threads. */
#ifdef __cplusplus
extern "C" {
#endif
#include <pthread.h>
#include <signal.h>
#include <stddef.h>

typedef pthread_t thread_t;
typedef pthread_key_t thread_key_t;

#define THR_BOUND     0x00000001
#define THR_NEW_LWP   0x00000002
#define THR_DETACHED  0x00000040
#define THR_SUSPENDED 0x00000080
#define THR_DAEMON    0x00000100

int thr_create(void *, size_t, void *(*)(void *), void *, long, thread_t *);
int thr_join(thread_t, thread_t *, void **);
void thr_exit(void *) __attribute__((__noreturn__));
thread_t thr_self(void);
int thr_main(void);
int thr_kill(thread_t, int);
int thr_sigsetmask(int, const sigset_t *, sigset_t *);
void thr_yield(void);
size_t thr_min_stack(void);
int thr_keycreate(thread_key_t *, void (*)(void *));
int thr_setspecific(thread_key_t, void *);
int thr_getspecific(thread_key_t, void **);
int thr_getconcurrency(void);
int thr_setconcurrency(int);

#ifdef __cplusplus
}
#endif
#endif
