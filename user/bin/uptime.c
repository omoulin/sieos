/* uptime - time since boot */
#include "sieos.h"

int main(void)
{
    long s = uptime_ms() / 1000;
    struct procinfo p[64];
    int n = procinfo(p, 64);
    printf("up %ld:%02ld:%02ld, %d processes\n", s / 3600, s / 60 % 60, s % 60, n);
    return 0;
}
