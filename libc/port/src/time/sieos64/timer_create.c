#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <port.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos/signal.h"
#include "sieos/port.h"
#include "sieos_timer.h"

static void *thr_main(void *arg)
{
	struct thr_timer *t = arg;
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, 0);    /* (the timer's thread takes no signals) */
	for (;;) {
		port_event_t pe;
		if (port_get(t->port, &pe, 0) < 0)
			continue;
		if (pe.portev_source == PORT_SOURCE_TIMER)
			t->fn(t->val);
		else if (pe.portev_source == PORT_SOURCE_USER)
			break;                          /* timer_delete */
	}
	close(t->port);
	free(t);
	return 0;
}

static int thread_timer(clockid_t clk, struct sigevent *evp, timer_t *res)
{
	struct thr_timer *t = malloc(sizeof *t);
	if (!t)
		return -EAGAIN;
	t->fn = evp->sigev_notify_function;
	t->val = evp->sigev_value;
	t->port = port_create();
	if (t->port < 0) {
		int e = errno;
		free(t);
		return -e;
	}
	struct sieos_port_notify pn = { t->port, t };
	struct sieos_sigevent kev = { .sigev_notify = SIEOS_SIGEV_PORT };
	kev.sigev_value.sival_ptr = &pn;
	int r = __syscall(SIEOS_SYS_timer_create, clk, &kev, &t->id);
	if (r < 0) {
		close(t->port);
		free(t);
		return r;
	}
	pthread_attr_t attr;
	if (evp->sigev_notify_attributes)
		attr = *evp->sigev_notify_attributes;
	else
		pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t td;
	r = pthread_create(&td, &attr, thr_main, t);
	if (r) {
		__syscall(SIEOS_SYS_timer_delete, t->id);
		close(t->port);
		free(t);
		return -r;
	}
	*res = (timer_t)t;
	return 0;
}

int timer_create(clockid_t clk, struct sigevent *restrict evp, timer_t *restrict res)
{
	int r, id;
	if (evp && evp->sigev_notify == SIGEV_THREAD) {
		r = thread_timer(clk, evp, res);
	} else {
		struct sieos_sigevent kev;
		if (evp) {
			kev.sigev_notify = evp->sigev_notify;
			kev.sigev_signo = evp->sigev_signo;
			kev.sigev_value.sival_ptr = evp->sigev_value.sival_ptr;
			kev.sigev_function = 0;
			kev.sigev_attributes = 0;
			kev.__sigev_pad2 = 0;
		}
		r = __syscall(SIEOS_SYS_timer_create, clk, evp ? &kev : 0, &id);
		if (r == 0)
			*res = (timer_t)(((uintptr_t)id << 1) | 1);
	}
	return __syscall_ret(r);
}
