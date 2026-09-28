/*
 * runall - run libc-test binaries on SIEOS.
 *
 *   runall REPORT DIR...
 *
 * Runs every *.exe in each DIR (sorted) in its own process with a timeout,
 * like libc-test's runtest: exit status 0 is a pass (math tests print notes
 * about inexact results they accept).  Each
 * result line goes to REPORT (with the test's output for failures); the
 * console gets the failures and a summary.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT 30

static int cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static volatile sig_atomic_t alarmed;
static void on_alarm(int s) { (void)s; alarmed = 1; }

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: runall REPORT DIR...\n");
		return 2;
	}
	FILE *rep = fopen(argv[1], "w");
	if (!rep) {
		perror(argv[1]);
		return 2;
	}
	struct sigaction sa = { .sa_handler = on_alarm };
	sigaction(SIGALRM, &sa, 0);
	int pass = 0, fail = 0;
	for (int d = 2; d < argc; d++) {
		DIR *dir = opendir(argv[d]);
		if (!dir) {
			perror(argv[d]);
			continue;
		}
		char *names[1024];
		int n = 0;
		struct dirent *e;
		while ((e = readdir(dir)) && n < 1024) {
			size_t l = strlen(e->d_name);
			if (l > 4 && !strcmp(e->d_name + l - 4, ".exe"))
				names[n++] = strdup(e->d_name);
		}
		closedir(dir);
		qsort(names, n, sizeof *names, cmp);
		for (int i = 0; i < n; i++) {
			char path[512], out[4096];
			snprintf(path, sizeof path, "%s/%s", argv[d], names[i]);
			int p[2];
			if (pipe2(p, O_CLOEXEC) < 0) {
				perror("pipe");
				return 2;
			}
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			pid_t pid = fork();
			if (pid == 0) {
				dup2(p[1], 1);
				dup2(p[1], 2);
				setpgid(0, 0);
				char *av[] = { path, 0 };
				execv(path, av);
				_exit(126);
			}
			close(p[1]);
			alarmed = 0;
			alarm(TIMEOUT);
			size_t got = 0;
			for (;;) {
				ssize_t r = read(p[0], out + got, sizeof out - 1 - got);
				if (r > 0) {
					got += r;
					if (got == sizeof out - 1)
						got -= r;              /* keep the start; drain the rest */
					continue;
				}
				if (r < 0 && errno == EINTR && alarmed) {
					kill(-pid, SIGKILL);
					kill(pid, SIGKILL);
				}
				if (r < 0 && errno == EINTR)
					continue;
				break;
			}
			out[got] = 0;
			close(p[0]);
			int st = 0;
			while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
				if (alarmed) kill(pid, SIGKILL);
			alarm(0);
			kill(-pid, SIGKILL);                   /* leftover children of the test */
			clock_gettime(CLOCK_MONOTONIC, &t1);
			long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
			char why[64] = "";
			if (alarmed)
				snprintf(why, sizeof why, "timeout");
			else if (WIFSIGNALED(st))
				snprintf(why, sizeof why, "signal %d", WTERMSIG(st));
			else if (WEXITSTATUS(st))
				snprintf(why, sizeof why, "status %d", WEXITSTATUS(st));
			if (!why[0]) {
				pass++;
				fprintf(rep, "PASS %s (%ld ms)\n%s", path, ms, out);
			} else {
				fail++;
				fprintf(rep, "FAIL %s [%s] (%ld ms)\n%s%s", path, why, ms, out,
				        got && out[got - 1] != '\n' ? "\n" : "");
				printf("FAIL %s [%s]\n", path, why);
			}
			fflush(rep);
			fflush(stdout);
			free(names[i]);
		}
	}
	fprintf(rep, "SUMMARY %d passed, %d failed\n", pass, fail);
	fclose(rep);
	printf("SUMMARY %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
