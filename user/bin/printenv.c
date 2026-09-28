/* printenv - print all or some environment variables */
#include "sieos.h"

int main(int argc, char **argv)
{
    if (argc == 1) {
        for (char **e = environ; e && *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);
        if (v)
            printf("%s\n", v);
        else
            rc = 1;
    }
    return rc;
}
