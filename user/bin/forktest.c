/* forktest - exercise fork/exec/wait, sbrk and the scheduler */
#include "sieos.h"

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 8;
    printf("forktest: spawning %d children\n", n);
    for (int i = 0; i < n; i++) {
        int pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            /* Each child allocates memory, fills it and computes a checksum. */
            size_t sz = 64 * 1024 * (i + 1);
            unsigned char *buf = malloc(sz);
            if (!buf)
                exit(100);
            unsigned long sum = 0;
            for (size_t k = 0; k < sz; k++)
                buf[k] = (unsigned char)(k * (i + 1));
            for (size_t k = 0; k < sz; k++)
                sum += buf[k];
            volatile unsigned long spin = 0;
            for (unsigned long k = 0; k < 2000000UL * (n - i); k++)
                spin += k;
            printf("  child %d (pid %d): %lu KB, checksum %lu\n", i, getpid(), sz / 1024, sum);
            exit(i);
        }
    }
    int ok = 0;
    for (int i = 0; i < n; i++) {
        int status;
        int pid = wait(&status);
        if (pid > 0 && !WIFSIGNALED(status))
            ok++;
    }
    printf("forktest: %d/%d children exited normally\n", ok, n);
    return ok == n ? 0 : 1;
}
