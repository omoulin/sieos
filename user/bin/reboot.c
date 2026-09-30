/*
 * reboot - restart the machine
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    sync();
    sieos_reboot(REBOOT_RESTART);
    dprintf(STDERR_FILENO, "reboot: %s (must be superuser)\n", strerror(errno));
    return 1;
}
