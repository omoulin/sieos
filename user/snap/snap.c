/*
 * snap - Snapshots of a volume (docs/siefs.md): "/" is one volume, and a
 * user's home can be another. A snapshot is a read-only picture of the
 * whole volume at one moment, shared with it block by block, so taking one
 * costs almost nothing. They are found in the volume's hidden .snapshots
 * folder (/home/alice/.snapshots/before-upgrade/...), to copy an old
 * version back; restore makes the whole volume go back.
 *
 *   snap                       the snapshots of the current folder's volume
 *   snap list [PATH]           ... of PATH's volume
 *   snap take NAME [PATH]      take one now
 *   snap delete NAME [PATH]    let one go (its blocks are freed)
 *   snap restore NAME [PATH]   the volume goes back to it (newer ones go)
 *
 * The file server also takes one of every volume each hour, "auto-..."
 * (kept a day). Taking, deleting and restoring need root, or the owner of
 * the volume's root (a home's user).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static void date_str(int64_t ns, char *s)
{
    int64_t t = ns / 1000000000, d = t / 86400 + 719468, sec = t % 86400;   /* days -> civil date */
    int64_t era = d / 146097, doe = d - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1, mon = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (mon <= 2);
    int v[5] = { (int)y, (int)mon, (int)day, (int)(sec / 3600), (int)(sec / 60 % 60) };
    for (int i = 0; i < 5; i++) {
        int w = i ? 2 : 4, x = v[i];
        for (int k = w - 1; k >= 0; k--) { s[k] = (char)('0' + x % 10); x /= 10; }
        s += w;
        if (i < 4) *s++ = "-- :"[i];
    }
    *s = 0;
}

static int fail(const char *what, long e)
{
    printf("snap: %s: %s\n", what, e == -EPERM ? "only root or the volume's owner may" : e == -ENOENT ? "no such snapshot or path" :
           e == -EEXIST ? "a snapshot has that name" : e == -EINVAL ? "a name is 1 to 63 characters, without /" :
           e == -ENOSPC ? "the disk is full" : e == -EROFS ? "that is a snapshot already" : "failed");
    return 1;
}

static int list(const char *path)
{
    static char buf[4096];
    uint64_t cur = 0;
    int n = 0;
    do {
        long r = fs_snaplist(path, &cur, buf, sizeof buf);
        if (r < 0) return fail(path, r);
        for (long p = 0; p + 9 <= r; ) {
            int64_t t;
            char d[20];
            memcpy(&t, buf + p, 8);
            int l = (uint8_t)buf[p + 8];
            char nm[64];
            memcpy(nm, buf + p + 9, l < 63 ? l : 63);
            nm[l < 63 ? l : 63] = 0;
            date_str(t, d);
            printf("%s  %s\n", d, nm);
            p += 9 + l;
            n++;
        }
    } while (cur);
    if (!n) printf("no snapshots of this volume yet (snap take NAME)\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "list";
    if (!strcmp(cmd, "list")) return list(argc > 2 ? argv[2] : ".");
    int op = !strcmp(cmd, "take") ? SNAP_TAKE : !strcmp(cmd, "delete") ? SNAP_DELETE : !strcmp(cmd, "restore") ? SNAP_RESTORE : 0;
    if (!op || argc < 3) {
        printf("usage: snap [list [PATH]] | take NAME [PATH] | delete NAME [PATH] | restore NAME [PATH]\n");
        return 2;
    }
    long r = fs_snap(argc > 3 ? argv[3] : ".", op, argv[2]);
    if (r) return fail(argv[2], r);
    printf("snapshot %s %s\n", argv[2], op == SNAP_TAKE ? "taken" : op == SNAP_DELETE ? "deleted" : "restored");
    return 0;
}
