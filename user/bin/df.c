/* df - show file system usage */
#include "sieos.h"

int main(void)
{
    struct fsinfo fi;
    if (fsinfo(&fi) < 0) {
        perror("df");
        return 1;
    }
    unsigned long kb = fi.block_size / 1024;
    unsigned long total = fi.total_blocks * kb, avail = fi.free_blocks * kb;
    unsigned long used = total - avail;
    printf("Filesystem      Type   1K-blocks     Used    Avail Use%% Mounted on\n");
    printf("%-15s ext4 %11lu %8lu %8lu %3lu%% /\n", fi.volname[0] ? fi.volname : "/dev/hda",
           total, used, avail, total ? used * 100 / total : 0);
    printf("Inodes: %lu total, %lu free\n", fi.total_inodes, fi.free_inodes);
    return 0;
}
