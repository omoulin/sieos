/*
 * noscreen - /bin/atlas on processors whose screen driver does not exist
 * yet (arm64, until the framebuffer phase): it says so and ends with 0, so
 * init does not restart it and login runs the first start on the terminal.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char s[] = "atlas: no screen driver on this machine yet: no desktop\n";
    sys_debug(s, sizeof s - 1);
    return 0;
}
