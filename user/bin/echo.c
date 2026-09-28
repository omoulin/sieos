/* echo - print arguments (-n: no trailing newline) */
#include "sieos.h"

int main(int argc, char **argv)
{
    int i = 1;
    bool newline = true;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        i++;
    }
    for (; i < argc; i++) {
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        if (i + 1 < argc)
            write(STDOUT_FILENO, " ", 1);
    }
    if (newline)
        write(STDOUT_FILENO, "\n", 1);
    return 0;
}
