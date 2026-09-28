/* tty - print the terminal name */
#include "sieos.h"

int main(void)
{
    if (!isatty(STDIN_FILENO)) {
        printf("not a tty\n");
        return 1;
    }
    printf("/dev/console\n");
    return 0;
}
