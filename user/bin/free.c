/* free - show memory usage */
#include "sieos.h"

int main(void)
{
    struct meminfo mi;
    if (meminfo(&mi) < 0) {
        perror("free");
        return 1;
    }
    unsigned long used = mi.total_kb - mi.free_kb;
    printf("            total       used       free\n");
    printf("Mem:   %8lu KB %7lu KB %7lu KB\n", mi.total_kb, used, mi.free_kb);
    printf("Kernel image: %lu KB, kernel heap in use: %lu KB\n", mi.kernel_kb, mi.heap_kb);
    return 0;
}
