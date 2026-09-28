/* clear - clear the screen */
#include "sieos.h"

int main(void)
{
    write(STDOUT_FILENO, "\f", 1);
    return 0;
}
