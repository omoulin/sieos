/* solaris - checks of the Solaris interfaces of the SIEOS libc (run on SIEOS). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/processor.h>
#include <sys/procset.h>
#include <sys/systeminfo.h>
#include <sys/lwp.h>
#include <sys/loadavg.h>
#include <procfs.h>
#include <thread.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *thr(void *a) { return (void *)((long)a + 1); }
static volatile int got;
static void on_sig(int s) { got = s; }
static int count_fd(void *cd, int fd) { (void)fd; ++*(int *)cd; return 0; }

int main(void)
{
	hrtime_t t0 = gethrtime(), t1 = gethrtime();
	CHECK(t0 > 0 && t1 >= t0, "gethrtime %lld %lld", t0, t1);
	CHECK(gethrvtime() >= 0, "gethrvtime");

	char buf[257];
	long n = sysinfo(SI_SYSNAME, buf, sizeof buf);
	CHECK(n > 0 && !strcmp(buf, "SIEOS"), "SI_SYSNAME %ld %s", n, buf);
	n = sysinfo(SI_ARCHITECTURE_64, buf, sizeof buf);
	CHECK(n > 0 && !strcmp(buf, "amd64"), "SI_ARCHITECTURE_64 %s", buf);
	n = sysinfo(SI_ISALIST, buf, sizeof buf);
	CHECK(n > 0 && strstr(buf, "amd64"), "SI_ISALIST %s", buf);

	processor_info_t pi;
	CHECK(processor_info(0, &pi) == 0 && pi.pi_state == P_ONLINE && pi.pi_clock > 0, "processor_info");
	CHECK(p_online(0, P_STATUS) == P_ONLINE, "p_online");
	processorid_t old = 5;
	CHECK(processor_bind(P_LWPID, P_MYID, 0, &old) == 0 && old == PBIND_NONE, "processor_bind %d", old);
	for (volatile long i = 0; i < 20000000; i++);
	CHECK(getcpuid() == 0, "getcpuid after binding to 0: %d", getcpuid());
	processor_bind(P_LWPID, P_MYID, PBIND_NONE, 0);

	double la[3];
	CHECK(getloadavg(la, 3) == 3 && la[LOADAVG_1MIN] >= 0, "getloadavg");

	CHECK(_lwp_self() == 1 && thr_main(), "_lwp_self %u", _lwp_self());
	thread_t t, dep;
	void *rv;
	CHECK(thr_create(0, 0, thr, (void *)41, 0, &t) == 0 && thr_join(t, &dep, &rv) == 0 && rv == (void *)42 && dep == t,
	      "thr_create/thr_join");
	CHECK(thr_min_stack() >= 2048, "thr_min_stack");

	signal(SIGUSR1, on_sig);
	CHECK(sigsend(P_PID, P_MYID, SIGUSR1) == 0 && got == SIGUSR1, "sigsend P_PID");
	got = 0;
	CHECK(sigsend(P_LWPID, P_MYID, SIGUSR1) == 0 && got == SIGUSR1, "sigsend P_LWPID");

	char s[SIG2STR_MAX];
	int sig;
	CHECK(sig2str(SIGCHLD, s) == 0 && !strcmp(s, "CLD"), "sig2str CHLD %s", s);
	CHECK(sig2str(SIGRTMIN + 2, s) == 0 && !strcmp(s, "RTMIN+2"), "sig2str RTMIN+2 %s", s);
	CHECK(str2sig("USR2", &sig) == 0 && sig == SIGUSR2 && str2sig("RTMAX-1", &sig) == 0 && sig == SIGRTMAX - 1,
	      "str2sig");
	CHECK(str2sig("NOPE", &sig) == -1, "str2sig bad");

	const char *en = getexecname();
	CHECK(en && strstr(en, "solaris"), "getexecname %s", en ? en : "(null)");

	int fd = open("/dev/null", O_RDONLY), cnt = 0;
	fdwalk(count_fd, &cnt);
	CHECK(fd >= 3 && cnt >= 4, "fdwalk %d", cnt);
	closefrom(3);
	CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "closefrom");

	psinfo_t ps;
	FILE *f = fopen("/proc/self/psinfo", "r");
	CHECK(f && fread(&ps, sizeof ps, 1, f) == 1 && ps.pr_pid == getpid() && !strncmp(ps.pr_fname, "solaris", 7),
	      "psinfo");
	if (f) fclose(f);

	printf("%s: %d failures\n", fails ? "FAILED" : "solaris: all checks passed", fails);
	return fails != 0;
}
