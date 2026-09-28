/* nproc - print the number of online CPUs */
#include "sieos.h"

int main(void)
{
    struct cpuinfo ci[16];
    int n = cpuinfo(ci, 16), online = 0;
    for (int i = 0; i < n; i++)
        online += ci[i].online != 0;
    printf("%d\n", online ? online : 1);
    return 0;
}
