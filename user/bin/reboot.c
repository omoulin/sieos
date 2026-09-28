/* reboot - restart the machine */
#include "sieos.h"

int main(void)
{
    sync();
    sieos_reboot(REBOOT_RESTART);
    dprintf(STDERR_FILENO, "reboot: %s (must be superuser)\n", strerror(errno));
    return 1;
}
