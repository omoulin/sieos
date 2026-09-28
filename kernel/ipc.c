/*
 * ipc.c - System V IPC: message queues (msgsys), semaphore sets (semsys)
 * and shared memory segments (shmsys), with Solaris command values
 * (sieos/ipc.h).  Identifiers are slot + generation * NSLOT, so a removed
 * object's id stops working.  Semaphore SEM_UNDO adjustments are applied
 * when the process exits.
 */
#include "proc.h"
#include "mm.h"
#include "vm.h"
#include "fs.h"
#include "abi2.h"
#include "sieos/ipc.h"
#include "sieos/mman.h"
#include "sieos/time.h"

#define NSLOT     32
#define MSGMAX    8192             /* largest message */
#define MSGMNB    16384            /* default queue size */
#define SEMMSL    64               /* semaphores per set */
#define SEMOPM    32               /* operations per semop */
#define SHMMAX    (64UL << 20)
#define NUNDO     256

struct ipcobj {
    bool used;
    int gen;
    struct sieos_ipc_perm perm;
    sieos_time_t ctime;
};

/* ---------------- common ---------------- */

static bool is_root(void)
{
    return current->euid == 0;
}

static void perm_init(struct ipcobj *o, sieos_key_t key, int flags)
{
    o->used = true;
    o->gen++;
    memset(&o->perm, 0, sizeof(o->perm));
    o->perm.uid = o->perm.cuid = current->euid;
    o->perm.gid = o->perm.cgid = current->egid;
    o->perm.mode = flags & 0777;
    o->perm.key = key;
    o->perm.seq = o->gen;
    o->ctime = kernel_time();
}

/* want: 4 read, 2 write */
static int ipc_access(struct ipcobj *o, int want)
{
    if (is_root())
        return 0;
    int bits;
    if (current->euid == o->perm.uid || current->euid == o->perm.cuid)
        bits = o->perm.mode >> 6;
    else if (cred_in_group(current, o->perm.gid) || cred_in_group(current, o->perm.cgid))
        bits = o->perm.mode >> 3;
    else
        bits = o->perm.mode;
    return (bits & want) == want ? 0 : -EACCES;
}

static bool ipc_owner(struct ipcobj *o)
{
    return is_root() || current->euid == o->perm.uid || current->euid == o->perm.cuid;
}

static int ipc_id(int slot, struct ipcobj *o)
{
    return slot + o->gen * NSLOT;
}

/* Find or create: returns the slot or a negative errno.  *created tells which. */
static int ipc_get(struct ipcobj *(*obj)(int), sieos_key_t key, int flags, bool *created)
{
    *created = false;
    if (key != SIEOS_IPC_PRIVATE) {
        for (int i = 0; i < NSLOT; i++) {
            struct ipcobj *o = obj(i);
            if (o->used && o->perm.key == key) {
                if ((flags & SIEOS_IPC_CREAT) && (flags & SIEOS_IPC_EXCL))
                    return -EEXIST;
                int want = ((flags & 0400) ? 4 : 0) | ((flags & 0200) ? 2 : 0);
                int r = ipc_access(o, want);
                return r < 0 ? r : i;
            }
        }
        if (!(flags & SIEOS_IPC_CREAT))
            return -ENOENT;
    }
    for (int i = 0; i < NSLOT; i++)
        if (!obj(i)->used) {
            *created = true;
            return i;
        }
    return -ENOSPC;
}

static int ipc_lookup(struct ipcobj *(*obj)(int), long id)
{
    if (id < 0)
        return -EINVAL;
    int slot = id % NSLOT;
    struct ipcobj *o = obj(slot);
    if (!o->used || o->gen != id / NSLOT)
        return -EINVAL;
    return slot;
}

static int ipc_set(struct ipcobj *o, const struct sieos_ipc_perm *u)
{
    if (!ipc_owner(o))
        return -EPERM;
    o->perm.uid = u->uid;
    o->perm.gid = u->gid;
    o->perm.mode = (o->perm.mode & ~0777) | (u->mode & 0777);
    o->ctime = kernel_time();
    return 0;
}

/* ---------------- message queues ---------------- */

struct kmsg {
    struct kmsg *next;
    long type;
    size_t size;
    char data[];
};

struct msgq {
    struct ipcobj o;
    struct kmsg *head;
    unsigned long cbytes, qnum, qbytes;
    sieos_pid_t lspid, lrpid;
    sieos_time_t stime, rtime;
};

static struct msgq msgqs[NSLOT];
static struct ipcobj *msgobj(int i) { return &msgqs[i].o; }

static void msgq_free(struct msgq *q)
{
    for (struct kmsg *m = q->head, *n; m; m = n) {
        n = m->next;
        kfree(m);
    }
    int gen = q->o.gen;
    memset(q, 0, sizeof(*q));
    q->o.gen = gen;
    wakeup(q);
}

static long msg_get(long key, long flags)
{
    bool created;
    int i = ipc_get(msgobj, key, flags, &created);
    if (i < 0)
        return i;
    struct msgq *q = &msgqs[i];
    if (created) {
        perm_init(&q->o, key, flags);
        q->qbytes = MSGMNB;
    }
    return ipc_id(i, &q->o);
}

static long msg_ctl(long id, long cmd, struct sieos_msqid_ds *u)
{
    int i = ipc_lookup(msgobj, id);
    if (i < 0)
        return i;
    struct msgq *q = &msgqs[i];
    switch (cmd) {
    case SIEOS_IPC_STAT: {
        if (!user_ok(u, sizeof(*u), true))
            return -EFAULT;
        int r = ipc_access(&q->o, 4);
        if (r < 0)
            return r;
        struct sieos_msqid_ds d;
        memset(&d, 0, sizeof(d));
        d.msg_perm = q->o.perm;
        d.msg_cbytes = q->cbytes;
        d.msg_qnum = q->qnum;
        d.msg_qbytes = q->qbytes;
        d.msg_lspid = q->lspid;
        d.msg_lrpid = q->lrpid;
        d.msg_stime = q->stime;
        d.msg_rtime = q->rtime;
        d.msg_ctime = q->o.ctime;
        memcpy(u, &d, sizeof(d));
        return 0;
    }
    case SIEOS_IPC_SET: {
        if (!user_ok(u, sizeof(*u), false))
            return -EFAULT;
        if (u->msg_qbytes > q->qbytes && !is_root())
            return -EPERM;
        int r = ipc_set(&q->o, &u->msg_perm);
        if (r == 0 && u->msg_qbytes)
            q->qbytes = u->msg_qbytes;
        return r;
    }
    case SIEOS_IPC_RMID:
        if (!ipc_owner(&q->o))
            return -EPERM;
        msgq_free(q);
        return 0;
    }
    return -EINVAL;
}

static long msg_snd(long id, const void *umsg, size_t size, long flags)
{
    if (size > MSGMAX)
        return -EINVAL;
    if (!user_ok(umsg, sizeof(long) + size, false))
        return -EFAULT;
    long type = *(const long *)umsg;
    if (type < 1)
        return -EINVAL;
    bool waited = false;
    for (;;) {
        int i = ipc_lookup(msgobj, id);
        if (i < 0)
            return waited ? -EIDRM_K : i;
        struct msgq *q = &msgqs[i];
        int r = ipc_access(&q->o, 2);
        if (r < 0)
            return r;
        if (q->cbytes + size <= q->qbytes) {
            struct kmsg *m = kmalloc(sizeof(*m) + size);
            if (!m)
                return -ENOMEM;
            m->next = NULL;
            m->type = type;
            m->size = size;
            memcpy(m->data, (const char *)umsg + sizeof(long), size);
            struct kmsg **pp = &q->head;
            while (*pp)
                pp = &(*pp)->next;
            *pp = m;
            q->cbytes += size;
            q->qnum++;
            q->lspid = current->pid;
            q->stime = kernel_time();
            wakeup(q);
            return 0;
        }
        if (flags & SIEOS_IPC_NOWAIT)
            return -EAGAIN;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(q);
        waited = true;
    }
}

static long msg_rcv(long id, void *umsg, size_t size, long type, long flags)
{
    if ((long)size < 0)
        return -EINVAL;
    if (!user_ok(umsg, sizeof(long) + size, true))
        return -EFAULT;
    bool waited = false;
    for (;;) {
        int i = ipc_lookup(msgobj, id);
        if (i < 0)
            return waited ? -EIDRM_K : i;
        struct msgq *q = &msgqs[i];
        int r = ipc_access(&q->o, 4);
        if (r < 0)
            return r;
        struct kmsg **best = NULL;
        for (struct kmsg **pp = &q->head; *pp; pp = &(*pp)->next) {
            struct kmsg *m = *pp;
            if (type == 0 || (type > 0 && m->type == type)) {
                best = pp;
                break;
            }
            if (type < 0 && m->type <= -type && (!best || m->type < (*best)->type))
                best = pp;
        }
        if (best) {
            struct kmsg *m = *best;
            if (m->size > size && !(flags & SIEOS_MSG_NOERROR))
                return -E2BIG;
            size_t n = MIN(m->size, size);
            *(long *)umsg = m->type;
            memcpy((char *)umsg + sizeof(long), m->data, n);
            *best = m->next;
            q->cbytes -= m->size;
            q->qnum--;
            q->lrpid = current->pid;
            q->rtime = kernel_time();
            kfree(m);
            wakeup(q);
            return n;
        }
        if (flags & SIEOS_IPC_NOWAIT)
            return -ENOMSG_K;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(q);
        waited = true;
    }
}

/* ---------------- semaphores ---------------- */

struct ksem {
    unsigned short val;
    sieos_pid_t pid;
    int ncnt, zcnt;
};

struct semset {
    struct ipcobj o;
    int nsems;
    struct ksem *sems;
    sieos_time_t otime;
};

struct undo {
    int pid, slot, gen, num;
    int adj;
};

static struct semset semsets[NSLOT];
static struct undo undos[NUNDO];
static struct ipcobj *semobj(int i) { return &semsets[i].o; }

static void undo_clear(int slot, int num)            /* num < 0: the whole set */
{
    for (int i = 0; i < NUNDO; i++)
        if (undos[i].pid && undos[i].slot == slot && undos[i].gen == semsets[slot].o.gen &&
            (num < 0 || undos[i].num == num))
            undos[i].pid = 0;
}

static int undo_add(int slot, int num, int adj)
{
    struct undo *freeu = NULL;
    for (int i = 0; i < NUNDO; i++) {
        struct undo *u = &undos[i];
        if (u->pid == current->pid && u->slot == slot && u->gen == semsets[slot].o.gen && u->num == num) {
            u->adj += adj;
            if (!u->adj)
                u->pid = 0;
            return 0;
        }
        if (!u->pid && !freeu)
            freeu = u;
    }
    if (!freeu)
        return -ENOSPC;
    *freeu = (struct undo){ current->pid, slot, semsets[slot].o.gen, num, adj };
    return 0;
}

static long sem_get(long key, long nsems, long flags)
{
    bool created;
    int i = ipc_get(semobj, key, flags, &created);
    if (i < 0)
        return i;
    struct semset *s = &semsets[i];
    if (!created) {
        if (nsems > s->nsems)
            return -EINVAL;
        return ipc_id(i, &s->o);
    }
    if (nsems <= 0 || nsems > SEMMSL)
        return -EINVAL;
    s->sems = kzalloc(nsems * sizeof(struct ksem));
    if (!s->sems)
        return -ENOMEM;
    perm_init(&s->o, key, flags);
    s->nsems = nsems;
    s->otime = 0;
    return ipc_id(i, &s->o);
}

static void semset_free(int slot)
{
    struct semset *s = &semsets[slot];
    undo_clear(slot, -1);
    kfree(s->sems);
    int gen = s->o.gen;
    memset(s, 0, sizeof(*s));
    s->o.gen = gen;
    wakeup(s);
}

static long sem_ctl(long id, long num, long cmd, uint64_t arg)
{
    int slot = ipc_lookup(semobj, id);
    if (slot < 0)
        return slot;
    struct semset *s = &semsets[slot];
    bool one = cmd == SIEOS_GETVAL || cmd == SIEOS_SETVAL || cmd == SIEOS_GETPID || cmd == SIEOS_GETNCNT ||
               cmd == SIEOS_GETZCNT;
    if (one && (num < 0 || num >= s->nsems))
        return -EINVAL;
    int r;
    switch (cmd) {
    case SIEOS_IPC_STAT: {
        struct sieos_semid_ds *u = (struct sieos_semid_ds *)arg;
        if (!user_ok(u, sizeof(*u), true))
            return -EFAULT;
        if ((r = ipc_access(&s->o, 4)) < 0)
            return r;
        struct sieos_semid_ds d;
        memset(&d, 0, sizeof(d));
        d.sem_perm = s->o.perm;
        d.sem_nsems = s->nsems;
        d.sem_otime = s->otime;
        d.sem_ctime = s->o.ctime;
        memcpy(u, &d, sizeof(d));
        return 0;
    }
    case SIEOS_IPC_SET: {
        struct sieos_semid_ds *u = (struct sieos_semid_ds *)arg;
        if (!user_ok(u, sizeof(*u), false))
            return -EFAULT;
        return ipc_set(&s->o, &u->sem_perm);
    }
    case SIEOS_IPC_RMID:
        if (!ipc_owner(&s->o))
            return -EPERM;
        semset_free(slot);
        return 0;
    case SIEOS_GETVAL:
    case SIEOS_GETPID:
    case SIEOS_GETNCNT:
    case SIEOS_GETZCNT:
        if ((r = ipc_access(&s->o, 4)) < 0)
            return r;
        return cmd == SIEOS_GETVAL ? s->sems[num].val : cmd == SIEOS_GETPID ? s->sems[num].pid
             : cmd == SIEOS_GETNCNT ? s->sems[num].ncnt : s->sems[num].zcnt;
    case SIEOS_SETVAL:
        if ((r = ipc_access(&s->o, 2)) < 0)
            return r;
        if ((int)arg < 0 || (int)arg > SIEOS_SEMVMX)
            return -ERANGE;
        s->sems[num].val = (int)arg;
        s->sems[num].pid = current->pid;
        s->o.ctime = kernel_time();
        undo_clear(slot, num);
        wakeup(s);
        return 0;
    case SIEOS_GETALL:
    case SIEOS_SETALL: {
        unsigned short *u = (unsigned short *)arg;
        if (!user_ok(u, s->nsems * sizeof(*u), cmd == SIEOS_GETALL))
            return -EFAULT;
        if ((r = ipc_access(&s->o, cmd == SIEOS_GETALL ? 4 : 2)) < 0)
            return r;
        for (int k = 0; k < s->nsems; k++)
            if (cmd == SIEOS_SETALL && u[k] > SIEOS_SEMVMX)
                return -ERANGE;
        for (int k = 0; k < s->nsems; k++) {
            if (cmd == SIEOS_GETALL) {
                u[k] = s->sems[k].val;
            } else {
                s->sems[k].val = u[k];
                s->sems[k].pid = current->pid;
            }
        }
        if (cmd == SIEOS_SETALL) {
            undo_clear(slot, -1);
            s->o.ctime = kernel_time();
            wakeup(s);
        }
        return 0;
    }
    }
    return -EINVAL;
}

/* Can all the operations run now?  Returns the index of one that must wait, or -1. */
static int sem_blocking(struct semset *s, const struct sieos_sembuf *ops, int n)
{
    int vals[SEMMSL];
    for (int k = 0; k < s->nsems; k++)
        vals[k] = s->sems[k].val;
    for (int k = 0; k < n; k++) {
        int *v = &vals[ops[k].sem_num];
        if (ops[k].sem_op > 0) {
            *v += ops[k].sem_op;
        } else if (ops[k].sem_op < 0) {
            if (*v < -ops[k].sem_op)
                return k;
            *v += ops[k].sem_op;
        } else if (*v != 0) {
            return k;
        }
    }
    return -1;
}

static long sem_op(long id, const struct sieos_sembuf *uops, long n, const struct sieos_timespec *uto)
{
    if (n <= 0 || n > SEMOPM)
        return n <= 0 ? -EINVAL : -E2BIG;
    if (!user_ok(uops, n * sizeof(*uops), false) || (uto && !user_ok(uto, sizeof(*uto), false)))
        return -EFAULT;
    struct sieos_sembuf ops[SEMOPM];
    memcpy(ops, uops, n * sizeof(*uops));
    uint64_t deadline = 0;
    if (uto) {
        if (uto->tv_sec < 0 || uto->tv_nsec < 0 || uto->tv_nsec >= 1000000000L)
            return -EINVAL;
        uint64_t ns = (uint64_t)uto->tv_sec * 1000000000UL + uto->tv_nsec;
        deadline = ticks + (ns * TIMER_HZ + 999999999UL) / 1000000000UL;
    }
    bool waited = false;
    for (;;) {
        int slot = ipc_lookup(semobj, id);
        if (slot < 0)
            return waited ? -EIDRM_K : slot;
        struct semset *s = &semsets[slot];
        bool alter = false;
        for (int k = 0; k < n; k++) {
            if (ops[k].sem_num >= s->nsems)
                return -EFBIG;
            alter |= ops[k].sem_op != 0;
        }
        int r = ipc_access(&s->o, alter ? 2 : 4);
        if (r < 0)
            return r;
        int w = sem_blocking(s, ops, n);
        if (w < 0) {
            for (int k = 0; k < n; k++) {
                struct ksem *m = &s->sems[ops[k].sem_num];
                if (ops[k].sem_op + m->val > SIEOS_SEMVMX)
                    return -ERANGE;
            }
            for (int k = 0; k < n; k++) {
                struct ksem *m = &s->sems[ops[k].sem_num];
                m->val += ops[k].sem_op;
                m->pid = current->pid;
                if ((ops[k].sem_flg & SIEOS_SEM_UNDO) && ops[k].sem_op)
                    undo_add(slot, ops[k].sem_num, -ops[k].sem_op);
            }
            s->otime = kernel_time();
            wakeup(s);
            return 0;
        }
        if (ops[w].sem_flg & SIEOS_IPC_NOWAIT)
            return -EAGAIN;
        if (uto && ticks >= deadline)
            return -EAGAIN;
        if (signal_pending(current))
            return -EINTR;
        struct ksem *m = &s->sems[ops[w].sem_num];
        bool zero = ops[w].sem_op == 0;
        zero ? m->zcnt++ : m->ncnt++;
        int gen = s->o.gen;
        curlwp->wake_tick = deadline;
        sleep_on(s);
        curlwp->wake_tick = 0;
        waited = true;
        if (s->o.used && s->o.gen == gen)
            zero ? m->zcnt-- : m->ncnt--;
    }
}

/* ---------------- shared memory ---------------- */

struct shmseg {
    struct ipcobj o;
    uint64_t *frames;
    size_t npages, size;
    unsigned long nattch;
    bool removed;
    sieos_pid_t cpid, lpid;
    sieos_time_t atime, dtime;
    bool locked;
};

static struct shmseg shmsegs[NSLOT];
static struct ipcobj *shmobj(int i) { return &shmsegs[i].o; }

static void shm_destroy(struct shmseg *g)
{
    for (size_t i = 0; i < g->npages; i++)
        pmm_unref(g->frames[i]);
    kfree(g->frames);
    int gen = g->o.gen;
    memset(g, 0, sizeof(*g));
    g->o.gen = gen;
}

void shm_attach_ref(struct shmseg *g, int delta)
{
    g->nattch += delta;
    if (delta > 0) {
        g->atime = kernel_time();
    } else {
        g->dtime = kernel_time();
    }
    if (current)
        g->lpid = current->pid;
    if (g->removed && g->nattch == 0)
        shm_destroy(g);
}

uint64_t shm_frame(struct shmseg *g, uint64_t idx)
{
    return g->frames[idx];
}

static long shm_get(long key, uint64_t size, long flags)
{
    bool created;
    int i = ipc_get(shmobj, key, flags, &created);
    if (i < 0)
        return i;
    struct shmseg *g = &shmsegs[i];
    if (!created) {
        if (size > g->size)
            return -EINVAL;
        return ipc_id(i, &g->o);
    }
    if (size == 0 || size > SHMMAX)
        return -EINVAL;
    size_t np = PAGE_ALIGN_UP(size) / PAGE_SIZE;
    if (np + 256 > pmm_free_pages())
        return -ENOMEM;
    g->frames = kzalloc(np * sizeof(uint64_t));
    if (!g->frames)
        return -ENOMEM;
    for (size_t k = 0; k < np; k++) {
        g->frames[k] = pmm_alloc();                  /* zero-filled */
        if (!g->frames[k]) {
            g->npages = k;
            shm_destroy(g);
            return -ENOMEM;
        }
    }
    perm_init(&g->o, key, flags);
    g->npages = np;
    g->size = size;
    g->cpid = current->pid;
    return ipc_id(i, &g->o);
}

static long shm_at(long id, uint64_t addr, long flags)
{
    int i = ipc_lookup(shmobj, id);
    if (i < 0)
        return i;
    struct shmseg *g = &shmsegs[i];
    bool ro = flags & SIEOS_SHM_RDONLY;
    int r = ipc_access(&g->o, ro ? 4 : 6);
    if (r < 0)
        return r;
    if (addr) {
        if (flags & SIEOS_SHM_RND)
            addr &= ~(uint64_t)(SIEOS_SHMLBA - 1);
        if (addr & (SIEOS_SHMLBA - 1))
            return -EINVAL;
    }
    int prot = SIEOS_PROT_READ | (ro ? 0 : SIEOS_PROT_WRITE);
    return vm_map_shm(g, addr, g->npages * PAGE_SIZE, prot, addr != 0);
}

static long shm_ctl(long id, long cmd, struct sieos_shmid_ds *u)
{
    int i = ipc_lookup(shmobj, id);
    if (i < 0)
        return i;
    struct shmseg *g = &shmsegs[i];
    int r;
    switch (cmd) {
    case SIEOS_IPC_STAT: {
        if (!user_ok(u, sizeof(*u), true))
            return -EFAULT;
        if ((r = ipc_access(&g->o, 4)) < 0)
            return r;
        struct sieos_shmid_ds d;
        memset(&d, 0, sizeof(d));
        d.shm_perm = g->o.perm;
        d.shm_segsz = g->size;
        d.shm_lpid = g->lpid;
        d.shm_cpid = g->cpid;
        d.shm_nattch = g->nattch;
        d.shm_atime = g->atime;
        d.shm_dtime = g->dtime;
        d.shm_ctime = g->o.ctime;
        memcpy(u, &d, sizeof(d));
        return 0;
    }
    case SIEOS_IPC_SET:
        if (!user_ok(u, sizeof(*u), false))
            return -EFAULT;
        return ipc_set(&g->o, &u->shm_perm);
    case SIEOS_IPC_RMID:
        if (!ipc_owner(&g->o))
            return -EPERM;
        g->removed = true;
        g->o.perm.key = SIEOS_IPC_PRIVATE;           /* no new shmget finds it */
        if (g->nattch == 0)
            shm_destroy(g);
        return 0;
    case SIEOS_SHM_LOCK:
    case SIEOS_SHM_UNLOCK:
        if (!is_root())
            return -EPERM;
        g->locked = cmd == SIEOS_SHM_LOCK;
        return 0;
    }
    return -EINVAL;
}

/* ---------------- process exit and the calls ---------------- */

/* Apply the exiting process's SEM_UNDO adjustments. */
void ipc_proc_exit(struct proc *p)
{
    for (int i = 0; i < NUNDO; i++) {
        struct undo *u = &undos[i];
        if (u->pid != p->pid)
            continue;
        struct semset *s = &semsets[u->slot];
        if (s->o.used && s->o.gen == u->gen && u->num < s->nsems) {
            int v = s->sems[u->num].val + u->adj;
            s->sems[u->num].val = v < 0 ? 0 : v > SIEOS_SEMVMX ? SIEOS_SEMVMX : v;
            s->sems[u->num].pid = p->pid;
            wakeup(s);
        }
        u->pid = 0;
    }
}

long sys2_msgsys(long op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    switch (op) {
    case SIEOS_MSGGET: return msg_get(a1, a2);
    case SIEOS_MSGCTL: return msg_ctl(a1, a2 & ~0x100, (struct sieos_msqid_ds *)a3);
    case SIEOS_MSGRCV: return msg_rcv(a1, (void *)a2, a3, a4, a5);
    case SIEOS_MSGSND: return msg_snd(a1, (const void *)a2, a3, a4);
    }
    return -EINVAL;
}

long sys2_semsys(long op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    switch (op) {
    case SIEOS_SEMCTL:     return sem_ctl(a1, a2, a3 & ~0x100, a4);
    case SIEOS_SEMGET:     return sem_get(a1, a2, a3);
    case SIEOS_SEMOP:      return sem_op(a1, (const struct sieos_sembuf *)a2, a3, NULL);
    case SIEOS_SEMTIMEDOP: return sem_op(a1, (const struct sieos_sembuf *)a2, a3, (const struct sieos_timespec *)a4);
    }
    return -EINVAL;
}

long sys2_shmsys(long op, uint64_t a1, uint64_t a2, uint64_t a3)
{
    switch (op) {
    case SIEOS_SHMAT:  return shm_at(a1, a2, a3);
    case SIEOS_SHMCTL: return shm_ctl(a1, a2 & ~0x100, (struct sieos_shmid_ds *)a3);
    case SIEOS_SHMDT:  return vm_unmap_shm(a1);
    case SIEOS_SHMGET: return shm_get(a1, a2, a3);
    }
    return -EINVAL;
}
