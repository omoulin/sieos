/*
 * sieinstall - install SIEOS on a disk (UEFI), from the running system.
 *
 *   sieinstall -l              the disks, one a line: name, size (MiB), flags ("-": none), description
 *   sieinstall [-y] [-P] DISK  install on /dev/dsk/DISK (a whole disk: "c4t0d0p0")
 *
 * It is set-user-ID root, like su: a user other than root must give root's
 * password (asked on the terminal, or with -P the first line of standard
 * input: the Facet installer's way).  Listing needs no password.
 *
 * Everything on DISK is erased.  It gets a new GPT with two partitions:
 *   s0  256 MiB  EFI system partition: GRUB (EFI/BOOT/BOOTX64.EFI, the
 *                removable-media path every UEFI firmware boots), the kernel,
 *                grub.cfg with root=<DISK>s1 (/usr/share/sieos/esp.img, patched)
 *   s1  the rest SIEOS's root file system: ext4 (mke2fs), then a copy of the
 *                running root (/proc, /tmp, /dev/pts, /dev/shm, /mnt and other
 *                mounts are left empty)
 *
 * Without -y it asks for the disk's name again as the confirmation.  The
 * progress is printed as lines the Facet installer reads:
 *   step N/M text       a step starts
 *   progress PCT        percent of the copy
 *   done | error text
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "sieos/dkio.h"
#include "sieos.h"

#define ESP_IMAGE   "/usr/share/sieos/esp.img"
#define PLACEHOLDER "root=SIEOS_INSTALL_ROOT_PLACEHOLDER"
#define TARGET      "/mnt/sieos-install"
#define ESP_MIB     256
#define NSTEPS      6

static void step(int n, const char *what)
{
    printf("step %d/%d %s\n", n, NSTEPS, what);
    fflush(stdout);
}

static void fail(const char *fmt, const char *arg)
{
    printf("error ");
    printf(fmt, arg ? arg : "", arg && errno ? strerror(errno) : "");
    printf("\n");
    fflush(stdout);
    exit(1);
}

static int disk_info(const char *name, struct sieos_dk_info *di)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/dsk/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = ioctl(fd, SIEOS_DKIOCINFO, di);
    close(fd);
    return r;
}

static int list(void)
{
    DIR *d = opendir("/dev/dsk");
    if (!d) {
        perror("sieinstall: /dev/dsk");
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        struct sieos_dk_info di;
        if (e->d_name[0] == '.' || disk_info(e->d_name, &di) < 0)
            continue;
        if (!(di.dki_flags & SIEOS_DK_DISK))
            continue;
        char flags[48];
        snprintf(flags, sizeof(flags), "%s%s%s%s", di.dki_flags & SIEOS_DK_ROOT ? "root," : "",
                 di.dki_flags & SIEOS_DK_INUSE ? "inuse," : "", di.dki_flags & SIEOS_DK_RDONLY ? "ro," : "",
                 di.dki_flags & SIEOS_DK_VIRTUAL ? "virtual," : "");
        printf("%s\t%llu\t%s\t%s\n", di.dki_name, di.dki_sectors / 2048, flags[0] ? flags : "-", di.dki_desc);
    }
    closedir(d);
    return 0;
}

/* ---------------------------------------------------------------- GPT */

static uint32_t crc32(const void *data, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ ((const uint8_t *)data)[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

static void guid_random(uint8_t g[16])
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, g, 16) != 16)
        for (int i = 0; i < 16; i++)
            g[i] = rand();
    if (fd >= 0)
        close(fd);
    g[7] = (g[7] & 0x0F) | 0x40;                  /* version 4 */
    g[8] = (g[8] & 0x3F) | 0x80;
}

/* A GUID as GPT stores it (the first three fields little-endian). */
static void guid_parse(const char *s, uint8_t g[16])
{
    unsigned v[16];
    sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[3], &v[2], &v[1], &v[0], &v[5], &v[4],
           &v[7], &v[6], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
    for (int i = 0; i < 16; i++)
        g[i] = v[i];
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void pwrite_all(int fd, const void *buf, size_t n, uint64_t off, const char *what)
{
    const uint8_t *b = buf;
    while (n) {
        ssize_t w = pwrite(fd, b, n, off);
        if (w <= 0)
            fail("writing the %s: %s", what);
        b += w;
        n -= w;
        off += w;
    }
}

/* Partition entry i: type, name, [first, last] in blocks. */
static void gpt_entry(uint8_t *e, const char *type, const char *name, uint64_t first, uint64_t last)
{
    guid_parse(type, e);
    guid_random(e + 16);
    put64(e + 32, first);
    put64(e + 40, last);
    for (int i = 0; name[i] && i < 36; i++)
        e[56 + 2 * i] = name[i];                  /* UTF-16LE */
}

static void write_gpt(int fd, uint64_t bytes, uint32_t bs, uint64_t *esp_first, uint64_t *esp_last,
                      uint64_t *root_first, uint64_t *root_last)
{
    uint64_t blocks = bytes / bs;
    uint32_t entries_blocks = 128 * 128 / bs;
    uint64_t first_usable = 2 + entries_blocks, last_usable = blocks - 2 - entries_blocks;
    uint64_t align = (1024 * 1024) / bs;          /* 1 MiB */
    *esp_first = align;
    *esp_last = *esp_first + (uint64_t)ESP_MIB * 1024 * 1024 / bs - 1;
    *root_first = *esp_last + 1;
    *root_last = (last_usable + 1) / align * align - 1;
    if (*root_last <= *root_first + align * 512)
        fail("the disk is too small (at least 1 GiB)%s%s", NULL);

    uint8_t *entries = calloc(128, 128);
    gpt_entry(entries, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system partition", *esp_first, *esp_last);
    gpt_entry(entries + 128, "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "SIEOS", *root_first, *root_last);
    uint32_t ecrc = crc32(entries, 128 * 128);

    uint8_t disk_guid[16];
    guid_random(disk_guid);
    uint8_t *hdr = calloc(1, bs);
    for (int backup = 0; backup < 2; backup++) {
        memset(hdr, 0, bs);
        memcpy(hdr, "EFI PART", 8);
        put32(hdr + 8, 0x00010000);               /* revision 1.0 */
        put32(hdr + 12, 92);
        put64(hdr + 24, backup ? blocks - 1 : 1);  /* this header, the other one */
        put64(hdr + 32, backup ? 1 : blocks - 1);
        put64(hdr + 40, first_usable);
        put64(hdr + 48, last_usable);
        memcpy(hdr + 56, disk_guid, 16);
        put64(hdr + 72, backup ? blocks - 1 - entries_blocks : 2);
        put32(hdr + 80, 128);
        put32(hdr + 84, 128);
        put32(hdr + 88, ecrc);
        put32(hdr + 16, crc32(hdr, 92));
        pwrite_all(fd, entries, 128 * 128, (backup ? blocks - 1 - entries_blocks : 2) * bs, "partition table");
        pwrite_all(fd, hdr, bs, (backup ? blocks - 1 : 1) * bs, "partition table");
    }
    uint8_t mbr[512] = { 0 };                     /* protective MBR: one partition of type EE */
    uint8_t *p = mbr + 0x1BE;
    p[1] = 0x00, p[2] = 0x02, p[3] = 0x00, p[4] = 0xEE, p[5] = 0xFF, p[6] = 0xFF, p[7] = 0xFF;
    put32(p + 8, 1);
    put32(p + 12, blocks - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)(blocks - 1));
    mbr[510] = 0x55, mbr[511] = 0xAA;
    uint8_t *lba0 = calloc(1, bs);
    memcpy(lba0, mbr, 512);
    pwrite_all(fd, lba0, bs, 0, "partition table");
    free(lba0);
    free(hdr);
    free(entries);
}

/* ---------------------------------------------------------------- the copy */

static unsigned long long total_bytes, copied_bytes;
static int last_pct = -1;
static dev_t root_dev;

static void progress(void)
{
    int pct = total_bytes ? (int)(copied_bytes * 100 / total_bytes) : 100;
    if (pct != last_pct) {
        printf("progress %d\n", pct > 100 ? 100 : pct);
        fflush(stdout);
        last_pct = pct;
    }
}

/* Directories whose contents are not copied (created empty). */
static bool skipped(const char *path)
{
    static const char *const empty[] = { "/proc", "/tmp", "/dev/pts", "/dev/shm", "/mnt", NULL };
    for (int i = 0; empty[i]; i++)
        if (!strcmp(path, empty[i]))
            return true;
    return false;
}

static void measure(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return;
    if (S_ISREG(st.st_mode))
        total_bytes += st.st_size;
    if (!S_ISDIR(st.st_mode) || skipped(path) || st.st_dev != root_dev)
        return;
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
        measure(sub);
    }
    closedir(d);
}

/* Hard links: the first path of each (device, inode) copied with st_nlink > 1. */
static struct link { ino_t ino; char path[256]; } links[512];
static int nlinks;

static void set_attrs(const char *dst, const struct stat *st)
{
    lchown(dst, st->st_uid, st->st_gid);
    if (!S_ISLNK(st->st_mode))
        chmod(dst, st->st_mode & 07777);          /* (after chown, which clears set-ID bits) */
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW);
}

static void copy(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) < 0) {
        printf("warning %s: %s\n", src, strerror(errno));
        return;
    }
    if (S_ISREG(st.st_mode) && st.st_nlink > 1) {
        for (int i = 0; i < nlinks; i++)
            if (links[i].ino == st.st_ino) {
                char to[1024];
                snprintf(to, sizeof(to), "%s%s", TARGET, links[i].path);
                if (link(to, dst) == 0)
                    return;
            }
        if (nlinks < (int)(sizeof(links) / sizeof(links[0]))) {
            links[nlinks].ino = st.st_ino;
            strncpy(links[nlinks++].path, src, sizeof(links[0].path) - 1);
        }
    }
    if (S_ISDIR(st.st_mode)) {
        if (strcmp(dst, TARGET) && mkdir(dst, 0700) < 0 && errno != EEXIST)
            fail("creating %s: %s", dst);
        if (!skipped(src) && st.st_dev == root_dev) {
            DIR *d = opendir(src);
            if (!d)
                fail("reading %s: %s", src);
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                if (!strcmp(src, "/") && !strcmp(e->d_name, "lost+found"))
                    continue;                     /* (mke2fs made one) */
                char s2[1024], d2[1024];
                snprintf(s2, sizeof(s2), "%s/%s", strcmp(src, "/") ? src : "", e->d_name);
                snprintf(d2, sizeof(d2), "%s/%s", dst, e->d_name);
                copy(s2, d2);
            }
            closedir(d);
        }
    } else if (S_ISREG(st.st_mode)) {
        int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in < 0 || out < 0)
            fail("copying %s: %s", src);
        static char buf[65536];
        ssize_t n;
        while ((n = read(in, buf, sizeof(buf))) > 0) {
            if (write(out, buf, n) != n)
                fail("writing %s: %s", dst);
            copied_bytes += n;
            progress();
        }
        close(in);
        if (close(out) < 0 || n < 0)
            fail("copying %s: %s", src);
    } else if (S_ISLNK(st.st_mode)) {
        char t[1024];
        ssize_t n = readlink(src, t, sizeof(t) - 1);
        if (n < 0)
            fail("reading the link %s: %s", src);
        t[n] = 0;
        if (symlink(t, dst) < 0)
            fail("creating the link %s: %s", dst);
    } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode)) {
        if (mknod(dst, st.st_mode, st.st_rdev) < 0)
            printf("warning %s: %s\n", dst, strerror(errno));
    } else {
        return;                                   /* (sockets: not copied) */
    }
    set_attrs(dst, &st);
}

/* ---------------------------------------------------------------- the steps */

static void write_esp(const char *part, const char *root_name)
{
    int in = open(ESP_IMAGE, O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st) < 0)
        fail("%s: %s", ESP_IMAGE);
    uint8_t *img = malloc(st.st_size);
    if (!img || read(in, img, st.st_size) != st.st_size)
        fail("reading %s: %s", ESP_IMAGE);
    close(in);
    char repl[sizeof(PLACEHOLDER)];               /* root=<disk>s1, padded to the placeholder's length */
    int n = snprintf(repl, sizeof(repl), "root=%s", root_name);
    memset(repl + n, ' ', sizeof(PLACEHOLDER) - 1 - n);
    int found = 0;
    for (off_t i = 0; i + (off_t)sizeof(PLACEHOLDER) - 1 <= st.st_size; i++)
        if (img[i] == 'r' && !memcmp(img + i, PLACEHOLDER, sizeof(PLACEHOLDER) - 1)) {
            memcpy(img + i, repl, sizeof(PLACEHOLDER) - 1);
            found++;
        }
    if (!found)
        fail("%s has no root= to set%s", ESP_IMAGE);
    char path[64];
    snprintf(path, sizeof(path), "/dev/dsk/%s", part);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        fail("opening %s: %s", path);
    pwrite_all(fd, img, st.st_size, 0, "EFI system partition");
    if (close(fd) < 0)
        fail("writing %s: %s", path);
    free(img);
}

static void run_mke2fs(const char *part, unsigned long long blocks)
{
    char dev[64], count[32];
    snprintf(dev, sizeof(dev), "/dev/dsk/%s", part);
    snprintf(count, sizeof(count), "%llu", blocks);
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 1);
        execl("/sbin/mke2fs", "mke2fs", "-q", "-F", "-F", "-t", "ext4", "-b", "4096", "-L", "sieos-root",
              "-E", "nodiscard,root_owner=0:0", dev, count, (char *)NULL);
        _exit(127);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
        fail("formatting %s failed (mke2fs)%s", dev);
}

int main(int argc, char **argv)
{
    bool yes = false, pass_stdin = false;
    const char *disk = NULL;
    setenv("PATH", "/sbin:/bin", 1);
    signal(SIGPIPE, SIG_IGN);                     /* (the Facet installer may go away: the install goes on) */              /* (set-user-ID: nothing from the caller's environment) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l"))
            return list();
        if (!strcmp(argv[i], "-y"))
            yes = true;
        else if (!strcmp(argv[i], "-P"))
            pass_stdin = true;
        else
            disk = argv[i];
    }
    if (!disk) {
        fprintf(stderr, "usage: sieinstall -l | sieinstall [-y] DISK   (DISK: a name under /dev/dsk, c4t0d0p0)\n");
        return 2;
    }
    if (!strncmp(disk, "/dev/dsk/", 9))
        disk += 9;
    errno = 0;
    if (geteuid() != 0)
        fail("installing needs root (sieinstall is set-user-ID root)%s%s", NULL);
    if (getuid() != 0) {                          /* root's password, as su asks it */
        char line[256], *pass = NULL;
        if (pass_stdin) {
            if (fgets(line, sizeof(line), stdin)) {
                line[strcspn(line, "\n")] = 0;
                pass = line;
            }
        } else {
            pass = getpass("Root password: ");
        }
        struct spwd *sp = getspnam("root");
        const char *hash = sp ? sp->sp_pwdp : "!";
        if (!pass || hash[0] == '!' || hash[0] == '*' || !check_password(pass, hash)) {
            sleep(1);
            fail("wrong root password: nothing was changed%s%s", NULL);
        }
        memset(line, 0, sizeof(line));
        setuid(0);
    }
    struct sieos_dk_info di;
    if (disk_info(disk, &di) < 0)
        fail("%s: no such disk (%s)", disk);
    if (!(di.dki_flags & SIEOS_DK_DISK) || (di.dki_flags & SIEOS_DK_VIRTUAL))
        fail("%s is not a whole disk%s", disk);
    if (di.dki_flags & (SIEOS_DK_INUSE | SIEOS_DK_ROOT)) {
        errno = 0;
        fail("%s is in use (mounted, or SIEOS runs from it)%s", disk);
    }
    size_t len = strlen(disk);
    if (len < 3 || strcmp(disk + len - 2, "p0"))
        fail("%s: a whole disk's name ends with p0%s", disk);
    if (!yes) {
        printf("All the data on %s (%s, %llu MiB) will be lost. Type its name to go on: ", disk, di.dki_desc,
               di.dki_sectors / 2048);
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof(line), stdin) || strncmp(line, disk, len) || (line[len] != '\n' && line[len]))
            fail("not confirmed: nothing was changed%s%s", NULL);
    }
    char base[24], esp[28], rootp[28];
    snprintf(base, sizeof(base), "%.*s", (int)(len - 2), disk);
    snprintf(esp, sizeof(esp), "%ss0", base);
    snprintf(rootp, sizeof(rootp), "%ss1", base);
    errno = 0;

    step(1, "Writing the partition table");
    char path[64];
    snprintf(path, sizeof(path), "/dev/dsk/%s", disk);
    int fd = open(path, O_RDWR);
    if (fd < 0)
        fail("opening %s: %s", path);
    uint32_t bs = di.dki_lbsize ? di.dki_lbsize : 512;
    uint64_t ef, el, rf, rl;
    write_gpt(fd, di.dki_sectors * 512, bs, &ef, &el, &rf, &rl);
    if (ioctl(fd, SIEOS_DKIOCREREAD) < 0)
        fail("reading the new partition table: %s%s", "");
    close(fd);
    struct sieos_dk_info pi;
    if (disk_info(rootp, &pi) < 0 || disk_info(esp, &pi) < 0)
        fail("the new partitions did not appear%s%s", NULL);

    step(2, "Installing the boot loader and the kernel");
    write_esp(esp, rootp);

    step(3, "Creating the file system");
    run_mke2fs(rootp, (rl - rf + 1) * bs / 4096);

    step(4, "Copying SIEOS");
    mkdir(TARGET, 0755);
    char spec[64];
    snprintf(spec, sizeof(spec), "/dev/dsk/%s", rootp);
    if (mount(spec, TARGET, "ext4", 0, NULL) < 0)
        fail("mounting %s: %s", spec);
    struct stat rs;
    stat("/", &rs);
    root_dev = rs.st_dev;
    measure("/");
    progress();
    copy("/", TARGET);
    copied_bytes = total_bytes;
    progress();

    step(5, "Finishing");
    FILE *f = fopen(TARGET "/etc/sieos-installed", "w");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "installed on %s (%s) at %s", disk, di.dki_desc, ctime(&now));
        fclose(f);
    }
    sync();
    if (umount(TARGET) < 0)
        fail("unmounting %s: %s", TARGET);
    rmdir(TARGET);

    step(6, "Done");
    printf("done %s\n", rootp);
    fflush(stdout);
    return 0;
}
