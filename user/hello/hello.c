/*
 * hello - The first program loaded from the disk: it shows its arguments
 * and who it runs as.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

int main(int argc, char **argv)
{
    mk_info_t in;
    sys_info(&in);
    printf("Hello from the disk! I am pid %d, uid %d, with %d argument%s:", in.self_pid, in.self_uid,
           argc - 1, argc == 2 ? "" : "s");
    for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    return 0;
}
