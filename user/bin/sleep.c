/* sleep - pause for N seconds */
#include "sieos.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: sleep seconds\n");
        return 2;
    }
    return msleep(atoi(argv[1]) * 1000UL) < 0;
}
