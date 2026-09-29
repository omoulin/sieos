/* host - look up a host name (hosts file, then DNS): IPv4 and IPv6 addresses */
#include "sieos.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: host name...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct sockaddr_storage a[16];
        socklen_t l[16];
        int n = resolve_addrs(argv[i], AF_UNSPEC, 0, a, l, 16);
        if (!n) {
            printf("Host %s not found\n", argv[i]);
            rc = 1;
        }
        for (int k = 0; k < n; k++) {
            char ip[INET6_ADDRSTRLEN];
            printf("%s has %saddress %s\n", argv[i], a[k].ss_family == AF_INET6 ? "IPv6 " : "",
                   addr_to_str((struct sockaddr *)&a[k], ip, sizeof(ip)));
        }
    }
    return rc;
}
