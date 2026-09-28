/* A C program built by x86_64-pc-sieos-gcc: stdio, math, TLS, the static-PIE variant too. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static _Thread_local int tls = 5;
static void *thr(void *a) { tls = 7; return (void *)(long)tls; }

int main(int argc, char **argv)
{
	void *r;
	pthread_t t;
	pthread_create(&t, 0, thr, 0);
	pthread_join(t, &r);
	int ok = tls == 5 && (long)r == 7 && fabs(sqrt(2.0) * sqrt(2.0) - 2.0) < 1e-12;
#ifdef __sieos__
	ok = ok && 1;
#else
	ok = 0;
#endif
	printf("c_hello (%s): %s\n", strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0], ok ? "ok" : "FAILED");
	return !ok;
}
