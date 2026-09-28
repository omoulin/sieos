/*
 * sigtest - self-test for signals: user handlers, sigreturn, masks,
 * SIGPIPE, SIGSEGV and wait status decoding.
 */
#include "sieos.h"

static volatile int hits, last_sig;

static void handler(int sig)
{
    hits++;
    last_sig = sig;
}

static int check(const char *what, bool ok)
{
    printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int main(void)
{
    int bad = 0;
    printf("sigtest:\n");

    signal(SIGUSR1, handler);
    raise(SIGUSR1);
    bad += check("handler runs and returns (sigreturn)", hits == 1 && last_sig == SIGUSR1);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    raise(SIGUSR1);
    sigset_t pend;
    sigpending(&pend);
    bad += check("blocked signal stays pending", hits == 1 && sigismember(&pend, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    bad += check("unblocking delivers it", hits == 2);

    signal(SIGUSR2, SIG_IGN);
    raise(SIGUSR2);
    bad += check("ignored signal is discarded", hits == 2);

    int fds[2];
    pipe(fds);
    close(fds[0]);
    signal(SIGPIPE, handler);
    long w = write(fds[1], "x", 1);
    bad += check("write to closed pipe: EPIPE + SIGPIPE", w < 0 && errno == EPIPE && last_sig == SIGPIPE);
    close(fds[1]);

    pid_t pid = fork();
    if (pid == 0) {
        static int *volatile bad_ptr = (int *)8;
        *bad_ptr = 1;
        exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    bad += check("child SIGSEGV reported by wait", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);

    pid = fork();
    if (pid == 0) {
        pause();
        exit(0);
    }
    kill(pid, SIGSTOP);
    waitpid(pid, &st, WUNTRACED);
    bad += check("WUNTRACED reports stopped child", WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
    kill(pid, SIGTERM);
    kill(pid, SIGCONT);
    waitpid(pid, &st, 0);
    bad += check("SIGTERM after SIGCONT terminates it", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

    pid = fork();
    if (pid == 0)
        exit(42);
    waitpid(pid, &st, 0);
    bad += check("exit status 42", WIFEXITED(st) && WEXITSTATUS(st) == 42);

    printf("sigtest: %s\n", bad ? "FAILED" : "all tests passed");
    return bad;
}
