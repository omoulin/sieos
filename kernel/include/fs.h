/*
 * fs.h - In-memory inodes, the ext4 file system interface and open files.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_FS_H
#define SIEOS_FS_H

#include "kernel.h"
#include "abi.h"
#include "ext4.h"
#include "sync.h"

#define INODE_RAW_MAX 1024

struct fs;

/*
 * The in-memory inode is the vnode of every file system.  Attributes live in
 * an ext4-layout inode (raw), which ext4 reads from disk and the other file
 * systems (tmpfs, procfs, devpts) fill in themselves; the accessors below
 * are therefore generic.  Operations dispatch on ip->fs->ops.
 */
struct inode {
    uint32_t ino;
    int ref;
    bool valid;
    struct fs *fs;
    void *priv;                   /* file-system private data */
    long aux[3];                  /* file-system private numbers */
    uint64_t *pcache;             /* MAP_SHARED pages (physical addresses) by page index */
    size_t npcache;
    uint8_t raw[INODE_RAW_MAX];   /* ext4-layout inode (inode_size bytes on ext4) */
};

static inline struct ext4_inode *DI(struct inode *ip) { return (struct ext4_inode *)ip->raw; }
static inline uint16_t inode_mode(struct inode *ip) { return DI(ip)->i_mode; }
static inline int inode_uid(struct inode *ip) { return DI(ip)->i_uid | ((int)DI(ip)->i_uid_high << 16); }
static inline int inode_gid(struct inode *ip) { return DI(ip)->i_gid | ((int)DI(ip)->i_gid_high << 16); }
static inline uint64_t inode_size(struct inode *ip)
{
    return DI(ip)->i_size_lo | ((uint64_t)DI(ip)->i_size_high << 32);
}
static inline void inode_set_size(struct inode *ip, uint64_t size)
{
    DI(ip)->i_size_lo = size & 0xFFFFFFFF;
    DI(ip)->i_size_high = size >> 32;
}
static inline bool same_inode(struct inode *a, struct inode *b)
{
    return a && b && a->fs == b->fs && a->ino == b->ino;
}

/* Attributes for stat (both ABIs are built from this). */
struct kstat {
    uint32_t dev_major, dev_minor;
    uint64_t ino;
    uint32_t mode, nlink;
    int uid, gid;
    uint32_t rdev;                /* MKDEV encoding */
    uint64_t size, blocks;        /* blocks: 512-byte units */
    int64_t atime, mtime, ctime;
    long atime_ns, mtime_ns, ctime_ns;
    uint32_t blksize;
    const char *fstype;
};

struct kstatvfs {
    uint64_t bsize, blocks, bfree, files, ffree;
    uint32_t namemax;
    bool rdonly;
};

/*
 * readdir callback: returns non-zero to stop (the entry is then not
 * consumed).  'next' is the directory offset (cookie) after this entry.
 */
typedef int (*filldir_t)(void *arg, const char *name, size_t len, uint64_t ino, int dtype, uint64_t next);

struct fs_ops {
    const char *name;
    long (*read)(struct inode *ip, void *dst, uint64_t off, size_t n);
    long (*write)(struct inode *ip, const void *src, uint64_t off, size_t n);
    int  (*truncate)(struct inode *ip, uint64_t len);
    int  (*lookup)(struct inode *dir, const char *name, size_t len, struct inode **out);
    int  (*readdir)(struct inode *dir, uint64_t *off, filldir_t fill, void *arg);
    int  (*create)(struct inode *dir, const char *name, uint16_t mode, uint32_t rdev, int uid, int gid,
                   struct inode **out);          /* any type but a directory; mode has S_IFMT */
    int  (*mkdir)(struct inode *dir, const char *name, uint16_t mode, int uid, int gid);
    int  (*unlink)(struct inode *dir, const char *name, bool is_dir);
    int  (*rename)(struct inode *od, const char *on, struct inode *nd, const char *nn);
    int  (*link)(struct inode *dir, const char *name, struct inode *ip);
    int  (*symlink)(struct inode *dir, const char *name, const char *target, int uid, int gid);
    int  (*update)(struct inode *ip);            /* write attributes back (NULL: nothing to do) */
    void (*release)(struct inode *ip);           /* the last reference was dropped */
    void (*statvfs)(struct fs *fs, struct kstatvfs *sv);
    void (*sync)(struct fs *fs);
    int  (*getpage)(struct inode *ip, uint64_t idx, uint64_t *pa);   /* file data frame, referenced (tmpfs) */
    void (*destroy)(struct fs *fs);              /* unmounted: free everything (NULL: cannot unmount) */
};

/* A mounted file system. */
struct fs {
    const struct fs_ops *ops;
    krmutex_t *lockp;             /* the file system's lock (fs_enter): its own, or shared (ext4's volumes) */
    krmutex_t lockbuf;
    uint32_t dev_major, dev_minor;
    uint32_t bsize;
    bool rdonly;
    bool nosuid;                  /* exec ignores set-id bits */
    struct inode *root;           /* held */
    struct inode *covered;        /* mount point in the parent file system (held); NULL for / */
    char mntpoint[64];
    char special[64];             /* mount's spec argument */
    int64_t mount_time;
    void *priv;
    struct fs *next;
};

/*
 * A file system's lock: held by the VFS around every operation on its
 * inodes (their attributes, data, directories, the shared pages); entered
 * again by the same LWP (a file system's operation may need another, and a
 * copy to user memory may fault on a mapping of the same file system).
 */
static inline void fs_enter(struct fs *fs) { rmutex_enter(fs->lockp); }
static inline void fs_exit(struct fs *fs) { rmutex_exit(fs->lockp); }
static inline void fs_lock_init(struct fs *fs) { if (!fs->lockp) fs->lockp = &fs->lockbuf; }

/* vfs.c */
extern struct fs *root_fs;
void vfs_init(void);
void vfs_mount_all(void);
int  vfs_mount(struct fs *fs, const char *path);
int  vfs_umount(struct fs *fs);                  /* -EBUSY while in use */
struct fs *vfs_mounted_on(struct inode *ip);     /* the file system whose root ip is, if mounted */
long vfs_mnttab(char *buf, size_t size);         /* the mount table as text; its length */
bool file_table_uses(struct fs *fs);             /* file.c: an open file is on fs */
struct fs *vfs_mounts(void);
struct inode *vfs_root(void);                    /* the global root, referenced */
struct inode *proc_root(void);                   /* namei.c: the caller's root directory, referenced */
struct inode *proc_cwd(void);                    /* ... its working directory, referenced */
struct inode *vfs_covering(struct inode *ip);    /* root of a file system mounted on ip, referenced */
struct inode *idup(struct inode *ip);
void iput(struct inode *ip);
int  iupdate(struct inode *ip);
bool inode_readonly(struct inode *ip);
uint32_t inode_rdev(struct inode *ip);
void inode_set_rdev(struct inode *ip, uint32_t dev);
int  inode_setattr(struct inode *ip, int mode, int uid, int gid);
int  inode_settimes(struct inode *ip, int64_t as, long ans, int64_t ms, long mns);   /* ns < 0: omit */
void inode_time_set(struct inode *ip, int which, int64_t s, long ns);     /* which: 0 a, 1 m, 2 c */
void inode_touch(struct inode *ip, bool m, bool c);
void inode_getstat(struct inode *ip, struct kstat *st);
long readi(struct inode *ip, void *dst, uint64_t off, size_t n);
long writei(struct inode *ip, const void *src, uint64_t off, size_t n);
int  itruncate(struct inode *ip, uint64_t len);
int  vfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out);
int  vfs_readdir(struct inode *dir, uint64_t *off, filldir_t fill, void *arg);
int  vfs_create(struct inode *dir, const char *name, uint16_t mode, uint32_t rdev, int uid, int gid,
                struct inode **out);
int  vfs_mkdir(struct inode *dir, const char *name, uint16_t mode, int uid, int gid);
int  vfs_unlink(struct inode *dir, const char *name, bool is_dir);
void vfs_blk_nodes(void);            /* the /dev/dsk nodes of the block devices there are */
int  vfs_rename(struct inode *od, const char *on, struct inode *nd, const char *nn);
int  vfs_link(struct inode *dir, const char *name, struct inode *ip);
int  vfs_symlink(struct inode *dir, const char *name, const char *target, int uid, int gid);
long vfs_readlink(struct inode *ip, char *buf, size_t n);
int  vfs_statvfs(struct inode *ip, struct kstatvfs *sv);
void vfs_sync(void);
int  vfs_dir_path(struct inode *dir, char *buf, size_t size);
struct inode *vfs_new_inode(struct fs *fs, uint32_t ino, uint16_t mode, int uid, int gid);
void vfs_free_inode(struct inode *ip);
int  dir_name_ok(const char *name, size_t *len);
int  vfs_getpage(struct inode *ip, uint64_t idx, uint64_t *pa);     /* frame shared by MAP_SHARED mappers */
void vfs_writeback(struct inode *ip, uint64_t idx);
void vfs_pcache_trim(struct inode *ip);

/* ext4.c */
struct fs *ext4_mount(int dev, bool ro);           /* the ext4 file system on a block device */

/* tmpfs.c, procfs.c, devpts.c */
struct fs *tmpfs_create(void);
struct fs *procfs_create(void);
int procfs_dir_pid(struct inode *ip);
struct fs *devpts_create(void);

/* perm.c */
int  inode_permission(struct inode *ip, int mask);      /* mask: R_OK|W_OK|X_OK */
int  may_delete(struct inode *dir, struct inode *victim);
bool inode_owner_or_root(struct inode *ip);

/* namei.c */
#define NAMEI_NOFOLLOW 1           /* do not follow a final symbolic link */
#define MAXSYMLINKS    20
#define SYMLINK_MAX    4095        /* longest target (paths passed in: MAXPATH - 1) */

/* A MAXPATH buffer (a page: kernel stacks are too small for them); NULL if none. */
char *path_get(void);
void path_put(char *p);
struct inode *namei(const char *path, int *err);
struct inode *nameiparent(const char *path, char *name, int *err);
struct inode *namei_at(struct inode *start, const char *path, int flags, int *err);
struct inode *nameiparent_at(struct inode *start, const char *path, char *name, int *err);

/* file.c */
#define FD_CLOEXEC 1           /* descriptor flag (p->fdflags) */
#define FD_NONE  0
#define FD_TTY   1
#define FD_INODE 2
#define FD_PIPE  3
#define FD_NULL  4
#define FD_ZERO  5
#define FD_PTM   6             /* pseudo-terminal master */
#define FD_FB    7             /* framebuffer */
#define FD_EVENTS 8            /* input events */
#define FD_SOCKET 9
#define FD_RANDOM 10           /* /dev/random, /dev/urandom */
#define FD_UNIX  11            /* AF_UNIX socket */
#define FD_LOFICTL 12          /* /dev/lofictl */
#define FD_BLK   13            /* a block device (/dev/dsk/...): f->minor, f->off */
#define FD_POWER 14            /* /dev/power */
#define FD_OPS   15            /* its own operations (f->ops, f->priv): eventfd, timerfd, pidfd, epoll */

struct file;
/* The operations of an FD_OPS file (fdext.c); a NULL one: EINVAL. */
struct file_ops {
    const char *name;                            /* for /proc's fd links: "eventfd"... */
    long (*read)(struct file *f, void *buf, size_t n);
    long (*write)(struct file *f, const void *buf, size_t n);
    short (*poll)(struct file *f);               /* POLLIN, POLLOUT, POLLHUP... ready now */
    void (*close)(struct file *f);               /* the last close: f->priv goes */
    long (*ioctl)(struct file *f, unsigned long cmd, void *arg);   /* optional: a device's commands */
    uint64_t (*page)(struct file *f, uint64_t off, bool *wc);       /* optional: mmap's page at off (its
                                                                     * physical address, 0: none; wc: write-combined) */
};

/* Character devices of the drivers (cdev.c): a major's open, which makes f an FD_OPS file. */
int  cdev_register(int major, int (*open)(struct file *f, int minor));
int  cdev_open(struct file *f, uint32_t dev);    /* -ENXIO: no driver has the major */
void dev_node(const char *path, uint16_t mode, uint32_t rdev);   /* a /dev node, created if missing */

struct pipe;
struct pty;
struct socket;
struct tty;
struct usock;

/* Kernel-internal open flags beyond the ABI v1 ones. */
#define O_NONBLOCK_K 0x00000800
#define O_NOFOLLOW_K 0x00020000
#define O_CLOEXEC_K  0x00080000
#define O_SYNC_K     0x00100000
#define O_SETFL_K    (O_APPEND | O_NONBLOCK_K | O_SYNC_K)   /* flags F_SETFL may change */

struct file {
    int type;
    int ref;                   /* (atomic) */
    kmutex_t f_offlock;        /* off, across the read or write that moves it */
    int flags;
    uint64_t off;
    struct inode *pdir;        /* directory the file was opened from (for /proc fd links) */
    char *pname;               /* ... and the path used to open it */
    struct inode *ip;          /* backing inode (files, dirs, device nodes) */
    struct pipe *pipe;
    struct pty *pty;
    struct tty *tty;           /* FD_TTY: console or pty slave */
    struct socket *sock;
    struct usock *usock;       /* FD_UNIX */
    int minor;                 /* FD_FB: the display */
    const struct file_ops *ops;  /* FD_OPS */
    void *priv;
    uint64_t gen;              /* which use of this entry (epoll tells a reused one apart) */
};

struct file *file_alloc(void);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
long file_read(struct file *f, void *buf, size_t n);
long file_write(struct file *f, const void *buf, size_t n);
long file_pread(struct file *f, void *buf, size_t n, uint64_t off);
long file_pwrite(struct file *f, const void *buf, size_t n, uint64_t off);
short file_poll(struct file *f, short events);   /* poll.c */
int  file_path(struct file *f, char *buf, size_t size);
struct proc;
bool fd_close(struct proc *p, int fd);          /* close a descriptor, dropping its record locks */
struct file *getf(int fd);                      /* the caller's file on fd, referenced */
void releasef(struct file *f);
bool fd_still(int fd, struct file *f);          /* the caller's fd still names f */
struct file *fd_file(int fd);                   /* ... referenced until the system call returns */
void fd_release_held(void);                     /* (the system call returns) */
int  fd_alloc(struct proc *p, struct file *f, int from, int fdflags);   /* a descriptor for f; -EMFILE */
struct file *fd_replace(struct proc *p, int fd, struct file *f, int fdflags);   /* dup2: the file replaced */
int  fd_getflags(struct proc *p, int fd);
int  fd_setflags(struct proc *p, int fd, int flags);
void fd_copy_table(struct proc *np, struct proc *cp);
void fd_close_all(struct proc *p);
void fd_close_exec(struct proc *p);

/* flock.c: POSIX record locks */
struct kflock {
    short type;                /* F_RDLCK_K / F_WRLCK_K / F_UNLCK_K */
    uint64_t start, end;       /* inclusive; end = UINT64_MAX: to end of file */
    int pid;
};
#define F_RDLCK_K 1
#define F_WRLCK_K 2
#define F_UNLCK_K 3
int  flock_get(struct inode *ip, struct kflock *l);
int  flock_set(struct inode *ip, struct kflock *l, bool wait);
void flock_release(struct inode *ip, int pid);

/* fsys.c: file system calls shared by both ABIs.  dirfd AT_FDCWD_K = cwd. */
#define AT_FDCWD_K (-100)
#define AT_NOFOLLOW_K 1
#define AT_REMOVEDIR_K 2
#define AT_FOLLOW_K 4
#define AT_EACCESS_K 8
long fsys_open(int dirfd, const char *upath, int flags, int mode);
long fsys_mkdir(int dirfd, const char *upath, int mode);
long fsys_mknod(int dirfd, const char *upath, int mode, uint32_t rdev);
long fsys_unlink(int dirfd, const char *upath, int flags);
long fsys_rename(int ofd, const char *uold, int nfd, const char *unew);
long fsys_link(int ofd, const char *uold, int nfd, const char *unew, int flags);
long fsys_symlink(const char *utarget, int dirfd, const char *upath);
long fsys_readlink(int dirfd, const char *upath, char *ubuf, size_t n);
long fsys_stat(int dirfd, const char *upath, struct kstat *st, int flags);
long fsys_chmod(int dirfd, const char *upath, int mode, int flags);
long fsys_chown(int dirfd, const char *upath, int uid, int gid, int flags);
long fsys_access(int dirfd, const char *upath, int mode, int flags);
long fsys_utimens(int dirfd, const char *upath, const int64_t *ts, int flags);   /* ts: {as, ans, ms, mns} */
long fsys_ftruncate(int fd, int64_t len);
long fsys_chdir(const char *upath);
long fsys_fchdir(int fd);
long fsys_chroot(const char *upath);
long fsys_getcwd(char *ubuf, size_t n);
long fsys_statvfs(int fd, const char *upath, struct kstatvfs *sv);
long fsys_lseek(int fd, int64_t off, int whence);
long fsys_dup(int fd, int from, bool cloexec);
long fsys_dup2(int fd, int to, bool cloexec);
struct file *fsys_file(int fd);
int  fsys_fdalloc(struct file *f, int from);

/* pipe.c */
int  pipe_create(struct file **rf, struct file **wf);
void pipe_close(struct pipe *p, int acc);
int  fifo_open(struct file *f, struct inode *ip, int flags);

/* unix.c: AF_UNIX sockets */
long unix_socket(int type);
long unix_socketpair(int type, int *usv);
bool unix_fd(long fd);
long unix_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
long unix_read(struct file *f, void *buf, size_t n);
long unix_write(struct file *f, const void *buf, size_t n);
short unix_poll(struct usock *u);
long unix_nread(struct usock *u);              /* FIONREAD: the bytes (the next datagram's) to read */
void unix_close(struct usock *u);
long pipe_read(struct pipe *p, char *buf, size_t n);
long pipe_nread(struct pipe *p);               /* FIONREAD: the bytes in it */
long pipe_write(struct pipe *p, const char *buf, size_t n, bool nonblock);

#endif
