/* host - look up a host name (hosts file, then DNS) */
#include "sieos.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: host name...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        unsigned int addr;
        if (resolve_host(argv[i], &addr) == 0) {
            printf("%s has address %s\n", argv[i], inet_ntoa((struct in_addr){ addr }));
        } else {
            printf("Host %s not found\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
