/* halt / reboot - stop or restart the machine */
#include "sieos.h"

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    bool restart = strcmp(name, "reboot") == 0 || (argc > 1 && strcmp(argv[1], "-r") == 0);
    sync();
    sieos_reboot(restart ? REBOOT_RESTART : REBOOT_HALT);
    dprintf(STDERR_FILENO, "%s: %s (must be superuser)\n", name, strerror(errno));
    return 1;
}
