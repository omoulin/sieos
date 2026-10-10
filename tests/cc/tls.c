/* tls.c - _Thread_local and __thread: local-exec, and initial-exec for tls_gcc.c's variable.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
typedef unsigned long pthread_t;
int pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
_Thread_local int counter = 5;
__thread long big[100];
__thread char name[16] = "main";
static __thread double scale = 1.5;
extern __thread int other_tls;                 /* defined by the host compiler's object */
int bump_other(void);

static int next(void) { static _Thread_local int n; return ++n; }

static void *worker(void *arg) {
    long id = (long)arg;
    counter += (int)id;
    big[id] = id * 10;
    name[0] = (char)('a' + id);
    scale *= id;
    other_tls += (int)id;
    for (int i = 0; i < (int)id; i++) next();
    long sum = 0;
    for (int i = 0; i < 100; i++) sum += big[i];
    char buf[96];
    sprintf(buf, "%d %ld %c %g %d %d %d", counter, sum, name[0], scale, other_tls, next(), *&counter == counter);
    return strcpy(malloc(96), buf);
}

int main(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, worker, (void *)(i + 1));
    for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); printf("thread %d: %s\n", i + 1, (char *)r); free(r); }
    int *p = &counter;
    *p = 77;
    int o = other_tls, nx = next(), b = bump_other();
    printf("main: %d %s %g %d %d %d %d\n", counter, name, scale, o, nx, b, other_tls);
    return 0;
}
