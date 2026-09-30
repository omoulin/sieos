/*
 * procfs.c - The Solaris-style process file system on /proc.
 *
 *   /proc/self                 -> <pid>
 *   /proc/mnttab               the mount table (text, as Solaris /etc/mnttab)
 *   /proc/msgbuf               the kernel's messages, the last 64 KiB (dmesg)
 *   /proc/<pid>/psinfo         sieos_psinfo_t   (0444)
 *   /proc/<pid>/status         sieos_pstatus_t  (0600)
 *   /proc/<pid>/cred           sieos_prcred_t   (0600)
 *   /proc/<pid>/usage          sieos_prusage_t  (0600)
 *   /proc/<pid>/lwp/<id>/lwpsinfo
 *   /proc/<pid>/cwd, root      symbolic links to the directories
 *   /proc/<pid>/fd/<n>         symbolic links to the open files
 *
 * Nodes are made on lookup and freed when unreferenced.  aux[] holds the
 * node type, the pid and the LWP id or descriptor number.  Records are
 * generated on every read.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "proc.h"
#include "mm.h"
#include "tty.h"
#include "abi2.h"
#include "sieos/procfs.h"
#include "sieos/sysinfo.h"
#include "sieos/priocntl.h"

enum { PN_ROOT = 1, PN_SELF, PN_PID, PN_PSINFO, PN_STATUS, PN_CRED, PN_USAGE, PN_LWPDIR, PN_LWP,
       PN_LWPSINFO, PN_FDDIR, PN_FD, PN_CWD, PN_ROOTLNK, PN_MNTTAB, PN_MSGBUF };

static const struct { const char *name; int type; uint16_t mode; } pid_entries[] = {
    { "psinfo", PN_PSINFO, S_IFREG | 0444 },
    { "status", PN_STATUS, S_IFREG | 0600 },
    { "cred",   PN_CRED,   S_IFREG | 0600 },
    { "usage",  PN_USAGE,  S_IFREG | 0600 },
    { "lwp",    PN_LWPDIR, S_IFDIR | 0555 },
    { "fd",     PN_FDDIR,  S_IFDIR | 0500 },
    { "cwd",    PN_CWD,    S_IFLNK | 0777 },
    { "root",   PN_ROOTLNK, S_IFLNK | 0777 },
};
#define NPID_ENTRIES (int)(sizeof(pid_entries) / sizeof(pid_entries[0]))

#define PTYPE(ip) ((int)(ip)->aux[0])
#define PPID(ip)  ((int)(ip)->aux[1])
#define PSUB(ip)  ((int)(ip)->aux[2])

static uint32_t pino(int type, int pid, int sub)
{
    if (type == PN_ROOT)
        return 1;
    return ((uint32_t)(pid & 0x7FFF) << 16) | ((uint32_t)(sub & 0x7FF) << 5) | type;
}

static int parse_num(const char *s, size_t len)
{
    if (!len || len > 9)
        return -1;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        n = n * 10 + s[i] - '0';
    }
    return n;
}

static struct proc *pfind(int pid)
{
    struct proc *p = proc_find(pid);
    return p && p->state != PSTATE_EMBRYO ? p : NULL;
}

static uint64_t boot_time(void)
{
    return kernel_time() - ticks / TIMER_HZ;
}

static void ticks_ts(uint64_t t, struct sieos_timespec *ts)
{
    ts->tv_sec = t / TIMER_HZ;
    ts->tv_nsec = (t % TIMER_HZ) * (1000000000L / TIMER_HZ);
}

static uint64_t proc_cpu_ticks(struct proc *p)
{
    uint64_t t = p->ticks;
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].proc == p && lwp_table[i].state != LWP_UNUSED)
            t += lwp_table[i].ticks;
    return t;
}

/* ---------------- records ---------------- */

static void fill_lwpsinfo(struct proc *p, struct lwp *l, sieos_lwpsinfo_t *li)
{
    memset(li, 0, sizeof(*li));
    li->pr_lwpid = l->lwpid;
    li->pr_addr = (uint64_t)l;
    int st;
    char sn;
    switch (l->state) {
    case LWP_RUNNING:   st = SIEOS_SONPROC; sn = 'O'; break;
    case LWP_RUNNABLE:  st = SIEOS_SRUN;    sn = 'R'; break;
    case LWP_ZOMBIE:    st = SIEOS_SZOMB;   sn = 'Z'; break;
    case LWP_STOPPED:
    case LWP_SUSPENDED: st = SIEOS_SSTOP;   sn = 'T'; break;
    case LWP_EMBRYO:    st = SIEOS_SIDL;    sn = 'I'; break;
    default:            st = SIEOS_SSLEEP;  sn = 'S'; break;
    }
    if (p->stopped && l->state != LWP_ZOMBIE) {
        st = SIEOS_SSTOP;
        sn = 'T';
    }
    li->pr_state = st;
    li->pr_sname = sn;
    li->pr_wchan = l->state == LWP_SLEEPING ? (uint64_t)l->chan : 0;
    if (l->state == LWP_STOPPED || l->state == LWP_SUSPENDED)
        li->pr_flag |= SIEOS_PR_STOPPED;
    li->pr_pri = sched_gpri(l);
    li->pr_nice = (l->cid == SIEOS_CID_TS || l->cid == SIEOS_CID_FX) ? 20 - l->upri / 3 : 0;
    strlcpy(li->pr_clname, sched_class_name(l->cid), sizeof(li->pr_clname));
    li->pr_start.tv_sec = boot_time() + p->start_tick / TIMER_HZ;
    ticks_ts(l->ticks, &li->pr_time);
    li->pr_onpro = l->cpu;
    li->pr_bindpro = l->bound ? l->bound - 1 : SIEOS_PBIND_NONE;
    strlcpy(li->pr_name, l->name, sizeof(li->pr_name));
}

static sieos_dev_t ttydev(struct proc *p)
{
    struct tty *t = tty_of_session(p->sid);
    if (!t)
        return (sieos_dev_t)-1;
    int n = pty_index(t);
    return n >= 0 ? SIEOS_MAKEDEV(DEV_PTS_MAJOR, n) : SIEOS_MAKEDEV(DEV_TTY_MAJOR, 1);
}

void procfs_psinfo(struct proc *p, sieos_psinfo_t *ps)
{
    memset(ps, 0, sizeof(*ps));
    int nlwp = 0, nzomb = 0;
    struct lwp *rep = NULL;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc != p || l->state == LWP_UNUSED)
            continue;
        if (l->state == LWP_ZOMBIE) {
            nzomb++;
            continue;
        }
        nlwp++;
        if (!rep || l->state == LWP_RUNNING || (l->state == LWP_RUNNABLE && rep->state != LWP_RUNNING))
            rep = l;
    }
    ps->pr_nlwp = nlwp;
    ps->pr_nzomb = nzomb;
    ps->pr_pid = p->pid;
    ps->pr_ppid = p->parent ? p->parent->pid : 0;
    ps->pr_pgid = p->pgid;
    ps->pr_sid = p->sid;
    ps->pr_uid = p->uid;
    ps->pr_euid = p->euid;
    ps->pr_gid = p->gid;
    ps->pr_egid = p->egid;
    ps->pr_addr = (uint64_t)p;
    if (p->pml4 && p->pml4 != kernel_pml4_phys && p->state != PSTATE_ZOMBIE) {
        ps->pr_rssize = vmm_user_pages(p->pml4) * 4;
        ps->pr_size = ps->pr_rssize;
    }
    uint64_t total = pmm_total_pages() * 4;
    ps->pr_pctmem = total ? (unsigned short)(ps->pr_rssize * 0x8000 / total) : 0;
    ps->pr_ttydev = ttydev(p);
    ps->pr_start.tv_sec = boot_time() + p->start_tick / TIMER_HZ;
    ticks_ts(proc_cpu_ticks(p), &ps->pr_time);
    ticks_ts(p->child_ticks, &ps->pr_ctime);
    uint64_t age = ticks - p->start_tick;
    ps->pr_pctcpu = age ? (unsigned short)MIN(0x8000, proc_cpu_ticks(p) * 0x8000 / age) : 0;
    strlcpy(ps->pr_fname, p->name, sizeof(ps->pr_fname));
    strlcpy(ps->pr_psargs, p->psargs[0] ? p->psargs : p->name, sizeof(ps->pr_psargs));
    ps->pr_argc = p->argc;
    ps->pr_argv = p->argv_addr;
    ps->pr_envp = p->envp_addr;
    ps->pr_dmodel = 2;
    if (p->state == PSTATE_ZOMBIE) {
        ps->pr_flag |= SIEOS_PR_ZOMBIE;
        ps->pr_wstat = p->exit_status;
        ps->pr_lwp.pr_state = SIEOS_SZOMB;
        ps->pr_lwp.pr_sname = 'Z';
    } else if (rep) {
        fill_lwpsinfo(p, rep, &ps->pr_lwp);
    }
    if (p->stopped)
        ps->pr_flag |= SIEOS_PR_STOPPED;
}

void procfs_pstatus(struct proc *p, sieos_pstatus_t *st)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].proc == p && lwp_table[i].state != LWP_UNUSED && lwp_table[i].state != LWP_ZOMBIE)
            st->pr_nlwp++;
    st->pr_flags = p->stopped ? SIEOS_PR_STOPPED : 0;
    st->pr_pid = p->pid;
    st->pr_ppid = p->parent ? p->parent->pid : 0;
    st->pr_pgid = p->pgid;
    st->pr_sid = p->sid;
    sig_set_to_v2(p->sig_pending, &st->pr_sigpend);
    st->pr_brkbase = p->heap_start;
    st->pr_brksize = p->brk - p->heap_start;
    st->pr_stkbase = USER_STACK_TOP - USER_STACK_SIZE;
    st->pr_stksize = USER_STACK_SIZE;
    ticks_ts(proc_cpu_ticks(p), &st->pr_utime);
    ticks_ts(p->child_ticks, &st->pr_cutime);
    memcpy(st->pr_clname, "TS", 3);
}

static void fill_cred(struct proc *p, sieos_prcred_t *c)
{
    memset(c, 0, sizeof(*c));
    c->pr_euid = p->euid;
    c->pr_ruid = p->uid;
    c->pr_suid = p->suid;
    c->pr_egid = p->egid;
    c->pr_rgid = p->gid;
    c->pr_sgid = p->sgid;
    c->pr_ngroups = MIN(p->ngroups, 16);
    for (int i = 0; i < c->pr_ngroups; i++)
        c->pr_groups[i] = p->groups[i];
}

static void fill_usage(struct proc *p, sieos_prusage_t *u)
{
    memset(u, 0, sizeof(*u));
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].proc == p && lwp_table[i].state != LWP_UNUSED)
            u->pr_count++;
    ticks_ts(ticks, &u->pr_tstamp);
    ticks_ts(p->start_tick, &u->pr_create);
    ticks_ts(ticks - p->start_tick, &u->pr_rtime);
    ticks_ts(proc_cpu_ticks(p), &u->pr_utime);
}

/* The target of a symbolic-link node. */
static long link_target(struct inode *ip, char *buf, size_t size)
{
    struct proc *p = PTYPE(ip) == PN_SELF ? current : pfind(PPID(ip));
    if (!p)
        return -ENOENT;
    switch (PTYPE(ip)) {
    case PN_SELF:
        return snprintf(buf, size, "%d", p->pid);
    case PN_CWD:
    case PN_ROOTLNK: {
        struct inode *d = PTYPE(ip) == PN_CWD ? p->cwd : p->root;
        if (!d)
            return -ENOENT;
        long r = vfs_dir_path(d, buf, size);
        return r < 0 ? r : r - 1;
    }
    case PN_FD: {
        int fd = PSUB(ip);
        struct file *f = fd < NOFILE ? p->ofile[fd] : NULL;
        if (!f)
            return -ENOENT;
        return file_path(f, buf, size);
    }
    }
    return -EINVAL;
}

/* ---------------- nodes ---------------- */

static struct inode *pnode(struct fs *fs, int type, struct proc *p, int sub, uint16_t mode)
{
    int uid = p ? p->euid : 0, gid = p ? p->egid : 0;
    struct inode *ip = vfs_new_inode(fs, pino(type, p ? p->pid : 0, sub), mode, uid, gid);
    if (!ip)
        return NULL;
    ip->aux[0] = type;
    ip->aux[1] = p ? p->pid : 0;
    ip->aux[2] = sub;
    if (p && type != PN_SELF) {
        int64_t t = boot_time() + p->start_tick / TIMER_HZ;
        for (int w = 0; w < 3; w++)
            inode_time_set(ip, w, t, 0);
    }
    size_t size = 0;
    switch (type) {
    case PN_PSINFO:   size = sizeof(sieos_psinfo_t); break;
    case PN_STATUS:   size = sizeof(sieos_pstatus_t); break;
    case PN_CRED:     size = sizeof(sieos_prcred_t); break;
    case PN_USAGE:    size = sizeof(sieos_prusage_t); break;
    case PN_LWPSINFO: size = sizeof(sieos_lwpsinfo_t); break;
    case PN_SELF: case PN_CWD: case PN_ROOTLNK: case PN_FD: {
        char buf[256];
        long n = link_target(ip, buf, sizeof(buf));
        size = n > 0 ? (size_t)n : 0;
        break;
    }
    case PN_MSGBUF:
        size = klog_size();
        break;
    case PN_MNTTAB:
        size = vfs_mnttab(NULL, 0);
        break;
    }
    inode_set_size(ip, size);
    return ip;
}

static int procfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    struct fs *fs = dir->fs;
    int type = PTYPE(dir);
    bool dotdot = len == 2 && name[0] == '.' && name[1] == '.';
    if (len == 1 && name[0] == '.') {
        *out = idup(dir);
        return 0;
    }
    if (type == PN_ROOT) {
        if (dotdot) {
            *out = idup(dir);
            return 0;
        }
        if (len == 4 && !memcmp(name, "self", 4)) {
            *out = pnode(fs, PN_SELF, NULL, 0, S_IFLNK | 0777);
            return *out ? 0 : -ENOMEM;
        }
        if (len == 6 && !memcmp(name, "msgbuf", 6)) {
            *out = pnode(fs, PN_MSGBUF, NULL, 0, S_IFREG | 0444);
            return *out ? 0 : -ENOMEM;
        }
        if (len == 6 && !memcmp(name, "mnttab", 6)) {
            *out = pnode(fs, PN_MNTTAB, NULL, 0, S_IFREG | 0444);
            return *out ? 0 : -ENOMEM;
        }
        struct proc *p = pfind(parse_num(name, len));
        if (!p)
            return -ENOENT;
        *out = pnode(fs, PN_PID, p, 0, S_IFDIR | 0555);
        return *out ? 0 : -ENOMEM;
    }
    struct proc *p = pfind(PPID(dir));
    if (!p)
        return -ENOENT;
    if (dotdot) {
        if (type == PN_PID)
            *out = idup(fs->root);
        else if (type == PN_LWP)
            *out = pnode(fs, PN_LWPDIR, p, 0, S_IFDIR | 0555);
        else
            *out = pnode(fs, PN_PID, p, 0, S_IFDIR | 0555);
        return *out ? 0 : -ENOMEM;
    }
    switch (type) {
    case PN_PID:
        for (int i = 0; i < NPID_ENTRIES; i++)
            if (strlen(pid_entries[i].name) == len && !memcmp(pid_entries[i].name, name, len)) {
                *out = pnode(fs, pid_entries[i].type, p, 0, pid_entries[i].mode);
                return *out ? 0 : -ENOMEM;
            }
        return -ENOENT;
    case PN_LWPDIR: {
        struct lwp *l = lwp_find(p, parse_num(name, len));
        if (!l)
            return -ENOENT;
        *out = pnode(fs, PN_LWP, p, l->lwpid, S_IFDIR | 0555);
        return *out ? 0 : -ENOMEM;
    }
    case PN_LWP:
        if (len != 8 || memcmp(name, "lwpsinfo", 8) || !lwp_find(p, PSUB(dir)))
            return -ENOENT;
        *out = pnode(fs, PN_LWPSINFO, p, PSUB(dir), S_IFREG | 0444);
        return *out ? 0 : -ENOMEM;
    case PN_FDDIR: {
        int fd = parse_num(name, len);
        if (fd < 0 || fd >= NOFILE || !p->ofile[fd])
            return -ENOENT;
        *out = pnode(fs, PN_FD, p, fd, S_IFLNK | 0777);
        return *out ? 0 : -ENOMEM;
    }
    }
    return -ENOTDIR;
}

static int procfs_readdir(struct inode *dir, uint64_t *off, filldir_t fill, void *arg)
{
    int type = PTYPE(dir);
    struct proc *p = type == PN_ROOT ? NULL : pfind(PPID(dir));
    if (type != PN_ROOT && !p)
        return -ENOENT;
    char name[16];
    uint64_t pos = 0;
#define EMIT(nm, ino, dt)                                                   \
    do {                                                                    \
        if (pos >= *off) {                                                  \
            if (fill(arg, nm, strlen(nm), ino, dt, pos + 1))                \
                return 0;                                                   \
            *off = pos + 1;                                                 \
        }                                                                   \
        pos++;                                                              \
    } while (0)
    EMIT(".", dir->ino, DT_DIR);
    EMIT("..", dir->ino, DT_DIR);
    switch (type) {
    case PN_ROOT:
        EMIT("self", pino(PN_SELF, 0, 0), DT_LNK);
        EMIT("mnttab", pino(PN_MNTTAB, 0, 0), DT_REG);
        EMIT("msgbuf", pino(PN_MSGBUF, 0, 0), DT_REG);
        for (int i = 1; i < NPROC; i++) {
            struct proc *q = &proc_table[i];
            if (q->state == PSTATE_UNUSED || q->state == PSTATE_EMBRYO) {
                pos++;
                continue;
            }
            snprintf(name, sizeof(name), "%d", q->pid);
            EMIT(name, pino(PN_PID, q->pid, 0), DT_DIR);
        }
        break;
    case PN_PID:
        for (int i = 0; i < NPID_ENTRIES; i++) {
            uint16_t m = pid_entries[i].mode;
            EMIT(pid_entries[i].name, pino(pid_entries[i].type, p->pid, 0),
                 S_ISDIR(m) ? DT_DIR : S_ISLNK(m) ? DT_LNK : DT_REG);
        }
        break;
    case PN_LWPDIR:
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->proc != p || l->state == LWP_UNUSED) {
                pos++;
                continue;
            }
            snprintf(name, sizeof(name), "%d", l->lwpid);
            EMIT(name, pino(PN_LWP, p->pid, l->lwpid), DT_DIR);
        }
        break;
    case PN_LWP:
        EMIT("lwpsinfo", pino(PN_LWPSINFO, p->pid, PSUB(dir)), DT_REG);
        break;
    case PN_FDDIR:
        for (int fd = 0; fd < NOFILE; fd++) {
            if (!p->ofile[fd]) {
                pos++;
                continue;
            }
            snprintf(name, sizeof(name), "%d", fd);
            EMIT(name, pino(PN_FD, p->pid, fd), DT_LNK);
        }
        break;
    }
#undef EMIT
    *off = pos;
    return 0;
}

static long procfs_read(struct inode *ip, void *dst, uint64_t off, size_t n)
{
    union {
        sieos_psinfo_t ps;
        sieos_pstatus_t st;
        sieos_prcred_t cr;
        sieos_prusage_t us;
        sieos_lwpsinfo_t li;
        char path[256];
    } rec;
    size_t size;
    int type = PTYPE(ip);
    if (type == PN_MSGBUF)
        return klog_read(dst, off, n);
    if (type == PN_MNTTAB) {
        char *text = kmalloc(4096);
        if (!text)
            return -ENOMEM;
        size_t len = MIN((size_t)vfs_mnttab(text, 4096), 4095);
        long r = off < len ? (long)MIN(n, len - off) : 0;
        if (r > 0)
            memcpy(dst, text + off, r);
        kfree(text);
        return r;
    }
    if (type == PN_SELF || type == PN_CWD || type == PN_ROOTLNK || type == PN_FD) {
        long r = link_target(ip, rec.path, sizeof(rec.path));
        if (r < 0)
            return r;
        size = MIN((size_t)r, sizeof(rec.path) - 1);
    } else {
        struct proc *p = pfind(PPID(ip));
        if (!p)
            return -ENOENT;
        switch (type) {
        case PN_PSINFO: procfs_psinfo(p, &rec.ps); size = sizeof(rec.ps); break;
        case PN_STATUS: procfs_pstatus(p, &rec.st); size = sizeof(rec.st); break;
        case PN_CRED:   fill_cred(p, &rec.cr);   size = sizeof(rec.cr); break;
        case PN_USAGE:  fill_usage(p, &rec.us);  size = sizeof(rec.us); break;
        case PN_LWPSINFO: {
            struct lwp *l = lwp_find(p, PSUB(ip));
            if (!l)
                return -ENOENT;
            fill_lwpsinfo(p, l, &rec.li);
            size = sizeof(rec.li);
            break;
        }
        default:
            return -EISDIR;
        }
    }
    if (off >= size)
        return 0;
    n = MIN(n, size - off);
    memcpy(dst, (char *)&rec + off, n);
    return n;
}

static void procfs_release(struct inode *ip)
{
    if (ip != ip->fs->root)
        vfs_free_inode(ip);
}

static void procfs_destroy(struct fs *fs)
{
    vfs_free_inode(fs->root);
    kfree(fs);
}

static const struct fs_ops procfs_ops = {
    .name = "proc",
    .read = procfs_read,
    .lookup = procfs_lookup,
    .readdir = procfs_readdir,
    .release = procfs_release,
    .destroy = procfs_destroy,
};

struct fs *procfs_create(void)
{
    struct fs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return NULL;
    fs->ops = &procfs_ops;
    fs->rdonly = true;
    fs->dev_major = 21;
    fs->bsize = 512;
    fs->root = pnode(fs, PN_ROOT, NULL, 0, S_IFDIR | 0555);
    return fs->root ? fs : NULL;
}
