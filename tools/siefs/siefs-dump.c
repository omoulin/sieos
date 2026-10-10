/*
 * siefs-dump - Show what is inside a SieFS image: both superblocks, then
 * every node of the trees with its items decoded. For learning and
 * debugging.
 *
 *   siefs-dump IMAGE
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdlib.h>
#include "siefs_int.h"
#include "host.h"

static const char *tname(int t)
{
    static const char *n[] = { "?", "INODE", "INLINE", "EXTENT", "DIRENT", "XATTR", "XINDEX" };
    return t == T_VOLUME ? "VOLUME" : t < 7 ? n[t] : "?";
}

static void item(skey_t k, const uint8_t *v, unsigned len)
{
    int t = KTYPE(k.off);
    uint64_t off = k.off & OFFMASK;
    printf("      (%llu %s %llu) %u bytes: ", (unsigned long long)(k.id & ~IDX_BIT), tname(t), (unsigned long long)off, len);
    if (t == T_INODE && len == sizeof(inode_t)) {
        inode_t in; memcpy(&in, v, len);
        printf("mode %o uid %u gid %u links %u size %llu parent %llu%s", in.mode, in.uid, in.gid, in.nlink,
               (unsigned long long)in.size, (unsigned long long)in.parent, in.flags & F_INLINE ? " inline" : "");
    } else if (t == T_EXTENT) {
        extent_t x; memcpy(&x, v, len < sizeof x ? len : sizeof x);
        printf("%u blocks at %llu", x.n, (unsigned long long)x.blk);
    } else if (t == T_DIRENT) {
        for (unsigned p = 0; p + 10 <= len; p += 10 + v[p + 9]) {
            uint64_t ino; memcpy(&ino, v + p, 8);
            printf("\"%.*s\" -> %llu  ", v[p + 9], v + p + 10, (unsigned long long)ino);
        }
    } else if (t == T_XATTR) {
        for (unsigned p = 0; p + 3 <= len; ) {
            unsigned nl = v[p], vl = v[p + 1] | v[p + 2] << 8;
            printf("%.*s = \"%.*s\"  ", (int)nl, v + p + 3, (int)(vl < 60 ? vl : 60), v + p + 3 + nl);
            p += 3 + nl + vl;
        }
    } else if (t == T_INLINE) {
        printf("\"");
        for (unsigned i = 0; i < len && i < 48; i++) putchar(v[i] >= 32 && v[i] < 127 ? v[i] : '.');
        printf(len > 48 ? "...\"" : "\"");
    } else if (t == T_VOLUME && len == sizeof(vol_t)) {
        vol_t vo; memcpy(&vo, v, len);
        printf("file tree at %llu, next id %llu, %llu objects", (unsigned long long)vo.root.blk,
               (unsigned long long)vo.next_id, (unsigned long long)vo.inodes);
    }
    printf("\n");
}

static void dump(siefs_t *fs, const bptr_t *p, int depth)
{
    node_t *cn;
    if (t_get_node(fs, p, &cn)) { printf("%*sblock %llu: UNREADABLE OR DAMAGED\n", depth * 2, "", (unsigned long long)p->blk); return; }
    node_t *n = malloc(sizeof *n);
    memcpy(n->d, cn->d, BS);
    cache_trim(fs);
    printf("%*snode %llu: level %u, %u %s, written by commit %llu\n", depth * 2, "", (unsigned long long)p->blk,
           NH(n)->level, NH(n)->n, NH(n)->level ? "children" : "items", (unsigned long long)NH(n)->gen);
    for (int i = 0; i < NH(n)->n; i++)
        if (NH(n)->level) dump(fs, &IE(n, i)->p, depth + 1);
        else item(LI(n, i)->k, n->d + LI(n, i)->off, LI(n, i)->len);
    free(n);
}

int main(int argc, char **argv)
{
    image_t im;
    siefs_env_t env;
    siefs_t *fs;
    if (argc != 2) { fprintf(stderr, "usage: siefs-dump IMAGE\n"); return 2; }
    if (image_open(&im, argv[1], 0, &env)) { fprintf(stderr, "%s: cannot open\n", argv[1]); return 1; }
    for (int s = 0; s < 2; s++) {
        sb_t sb;
        env.read(env.ctx, 1 + s, 1, &sb);
        uint32_t c = sb.csum;
        sb.csum = 0;
        int ok = sb.magic == SB_MAGIC && siefs_crc32c(0, &sb, BS) == c;
        printf("superblock slot %d: %s", s, ok ? "valid" : "INVALID");
        if (ok) printf(", commit %llu, \"%s\", %llu blocks, root tree at %llu, bitmap at %llu, data from %llu",
                       (unsigned long long)sb.seq, sb.label, (unsigned long long)sb.nblocks, (unsigned long long)sb.root.blk,
                       (unsigned long long)sb.bm[s], (unsigned long long)sb.data_start);
        printf("\n");
    }
    image_close(&im);
    if (!(fs = host_mount(argv[1], &im))) return 1;
    printf("mounted commit %llu\nroot tree:\n", (unsigned long long)fs->sb.seq);
    dump(fs, &fs->rt.root, 1);
    printf("file tree (volume 1):\n");
    dump(fs, &fs->ft.root, 1);
    siefs_unmount(fs);
    image_close(&im);
    return 0;
}
