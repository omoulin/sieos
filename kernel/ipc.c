/*
 * ipc.c - Message passing: how the pieces of a microkernel system talk.
 *
 * A server creates a port and waits on it (ipc_recv). A client sends a
 * message to the port and sleeps until the answer (ipc_call). The server
 * gets the message along with a token naming the client, does the work and
 * answers with ipc_reply(token). It is synchronous, "rendezvous" style, as
 * in QNX or L4: the kernel never stores messages, it copies them directly
 * from one process to the other the moment both sides are ready. A client
 * that arrives first waits in the port's queue; server threads that arrive
 * first wait as the port's receivers (several threads of a server may wait
 * on one port: each message goes to one of them).
 *
 * Interrupts reach drivers the same way: an interrupt bound to a port
 * wakes a server thread with a message from "token 0", whose w[0] lists
 * the interrupts that occurred. A driver therefore needs no extra thread:
 * it waits for clients and hardware in the same ipc_recv.
 *
 * Locks: each bucket of the port table has its own, for its ports and
 * their queues (ports are numbered in order, so busy ports have buckets of
 * their own). A message is copied with no lock held: the thread taken off
 * a queue is ours until we wake it. A client waiting for its reply is
 * woken by whoever sets its `server` to 0, under its bucket's lock in the
 * thread table (the server's reply, its death, the client's own end, a
 * signal): exactly one of them. A reply token names the client thread and
 * its call (the thread's id, and in the high half a count of the calls it
 * gave up): a call a signal interrupted is answered nowhere, never in a
 * later call.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

/* Ports are allocated as needed and named by a number that is never
 * reused (a stale number gives ENOENT, never someone else's port). */
typedef struct port {
    long id;
    proc_t *owner;              /* the server: only its threads may receive */
    char name[16];              /* "" for an anonymous port */
    task_t *head, *tail;        /* clients waiting for the server to receive */
    task_t *receivers;          /* server threads waiting in ipc_recv */
    uint64_t irqs;              /* interrupts not yet delivered, one bit each */
    uint64_t notes;             /* other kernel events not yet delivered (NOTE_...) */
    struct port *hnext;         /* in the id hash table */
    struct port *nnext;         /* in the list of named ports */
    struct port *tnext;         /* in its owner's list */
} port_t;
#define NPORT 256                    /* hash buckets, one cache line each */
static struct { spinlock_t lock; port_t *head; char pad[48]; } port_hash[NPORT];
static spinlock_t names_lock;   /* named, lookup_waiters, wanted[] */
static port_t *named;
static long next_port = 1;
static task_t *lookup_waiters;  /* threads waiting in port_lookup */
#define IRQSHARE 4               /* drivers that may share one interrupt line (PCI lines are shared) */
static spinlock_t irq_lock;      /* the three below */
static long irq_port[MAXIRQ][IRQSHARE];   /* the ports each interrupt goes to (0: none) */
static uint8_t irq_need[MAXIRQ];  /* which of them still have to acknowledge the last one */
static int irq_cpu;             /* the CPU the next bound interrupt goes to */

/* The port with number id, its bucket locked (port_put unlocks), or 0. */
static port_t *port_get(long id)
{
    if (id <= 0) return 0;
    spin_lock(&port_hash[id % NPORT].lock);
    for (port_t *p = port_hash[id % NPORT].head; p; p = p->hnext)
        if (p->id == id) return p;
    spin_unlock(&port_hash[id % NPORT].lock);
    return 0;
}
static void port_put(port_t *p) { spin_unlock(&port_hash[p->id % NPORT].lock); }

static port_t *port_named(const char *name)      /* (under names_lock) */
{
    for (port_t *p = named; *name && p; p = p->nnext)
        if (!strcmp(p->name, name)) return p;
    return 0;
}

/* Copy sender s's message into receiver r's msg_t: the words, the data
 * (from s's sbuf into r's rbuf, as much as fits) and s's identity. Both
 * msg_t are in their owners' memory; s->umsg and r->umsg point at them. */
static long transfer(task_t *s, task_t *r)
{
    msg_t sm, rm;
    uint64_t sas = s->proc->as, ras = r->proc->as;
    if (vm_copy(0, &sm, sas, s->umsg, sizeof sm) || vm_copy(0, &rm, ras, r->umsg, sizeof rm)) return -EFAULT;
    uint64_t n = sm.slen < rm.rlen ? sm.slen : rm.rlen;
    if (n && vm_copy(ras, rm.rbuf, sas, sm.sbuf, n)) return -EFAULT;
    memcpy(rm.w, sm.w, sizeof rm.w);
    rm.rlen = n;
    rm.pid = s->proc->pid; rm.uid = s->proc->uid; rm.gid = s->proc->gid;
    return vm_copy(ras, r->umsg, 0, &rm, sizeof rm);
}

/* Hand pending interrupts and notes to server thread r (a token 0 message). */
static long deliver(task_t *r, uint64_t irqs, uint64_t notes)
{
    msg_t m;
    if (vm_copy(0, &m, r->proc->as, r->umsg, sizeof m)) return -EFAULT;
    memset(m.w, 0, sizeof m.w);
    m.w[0] = irqs;
    m.w[1] = notes;
    m.rlen = 0; m.pid = m.uid = m.gid = 0;
    return vm_copy(r->proc->as, r->umsg, 0, &m, sizeof m);
}

static task_t *pop_receiver(port_t *p)           /* (bucket locked) -> ours to wake */
{
    task_t *r = p->receivers;
    if (r) { p->receivers = r->next; r->wport = 0; }
    return r;
}

static void unqueue(port_t *p, task_t *t)        /* t off p's client queue (bucket locked) */
{
    task_t *prev = 0;
    for (task_t **q = &p->head; *q; prev = *q, q = &(*q)->next)
        if (*q == t) {
            *q = t->next;
            if (p->tail == t) p->tail = prev;
            break;
        }
    t->wport = 0;
}

/* Wake the threads of list (linked by next) with result ret. */
static void wake_list(task_t *t, long ret)
{
    for (task_t *n; t; t = n) {
        n = t->next;
        t->ret = ret;
        ready(t);
    }
}

/* Threads waiting in port_lookup for name, off the list (under names_lock). */
static task_t *take_waiters(const char *name)
{
    task_t *list = 0;
    for (task_t **w = &lookup_waiters; *w; ) {
        task_t *t = *w;
        if (strcmp(t->wname, name)) { w = &t->next; continue; }
        *w = t->next;
        t->state = TS_READY;             /* (ours now: stop_thread leaves it) */
        t->next = list;
        list = t;
    }
    return list;
}

long port_create(const char *name)
{
    proc_t *me = cur->proc;
    port_t *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    long id = p->id = __atomic_fetch_add(&next_port, 1, __ATOMIC_RELAXED);
    p->owner = me;
    strlcpy(p->name, name, sizeof p->name);
    task_t *woken = 0;
    if (*name) {
        spin_lock(&names_lock);
        if (port_named(name)) { spin_unlock(&names_lock); kfree(p); return -EEXIST; }
    }
    spin_lock(&port_hash[id % NPORT].lock);
    p->hnext = port_hash[id % NPORT].head;
    port_hash[id % NPORT].head = p;
    spin_unlock(&port_hash[id % NPORT].lock);
    if (*name) {
        p->nnext = named;
        named = p;
        woken = take_waiters(name);      /* whoever waits for this name */
        spin_unlock(&names_lock);
    }
    spin_lock(&me->lock);
    p->tnext = me->ports;
    me->ports = p;
    spin_unlock(&me->lock);
    wake_list(woken, id);
    return id;
}

/* ---- Servers started on demand. The supervisor declares the port names
 * whose server it starts only when needed (SYS_WANT): the network, the
 * assistant... A lookup of such a name that finds no port wakes the
 * supervisor (NOTE_WANT); it asks which names are waited for (SYS_WANTED),
 * starts their servers, and the waiters wake when the port appears, as
 * with any server that starts late. No polling anywhere. */
#define NWANT 16
static char wanted[NWANT][16];

static int is_wanted(const char *name)           /* (under names_lock) */
{
    for (int i = 0; i < NWANT; i++) if (wanted[i][0] && !strcmp(wanted[i], name)) return 1;
    return 0;
}

/* Find a port by name; if it does not exist yet, wait until it does (so
 * programs need not care in which order the servers start), unless
 * LOOKUP_NOWAIT. */
long port_lookup(const char *name, int flags)
{
    task_t *t = cur;
    if (!*name) return -EINVAL;
    spin_lock(&names_lock);
    port_t *p = port_named(name);
    if (p || (flags & LOOKUP_NOWAIT)) {
        long id = p ? p->id : -ENOENT;
        spin_unlock(&names_lock);
        return id;
    }
    strlcpy(t->wname, name, sizeof t->wname);
    t->next = lookup_waiters;
    lookup_waiters = t;
    long e = blocked(t, TS_LOOKUP);
    if (e) lookup_waiters = t->next;
    int want = !e && is_wanted(name);
    spin_unlock(&names_lock);
    if (e) return e;
    if (want && super && super->child_port) port_notify(super->child_port, super, NOTE_WANT);
    schedule();
    return t->ret;
}

/* SYS_WANT: declare an on-demand name (err 0), or fail its waiters (err < 0). */
long port_want(const char *name, long err)
{
    if (!cur->proc->super) return -EPERM;
    if (!*name || err > 0 || err < -4095) return -EINVAL;
    spin_lock(&names_lock);
    if (!err) {
        long r = -ENOMEM;
        if (is_wanted(name)) r = 0;
        else for (int i = 0; i < NWANT && r; i++)
            if (!wanted[i][0]) { strlcpy(wanted[i], name, sizeof wanted[i]); r = 0; }
        spin_unlock(&names_lock);
        return r;
    }
    task_t *woken = take_waiters(name);
    spin_unlock(&names_lock);
    wake_list(woken, err);
    return 0;
}

/* SYS_WANTED: the on-demand names without a port that someone waits for. */
long port_wanted(uint64_t buf, uint64_t size)
{
    if (!cur->proc->super) return -EPERM;
    char out[NWANT * 16];
    size_t n = 0;
    spin_lock(&names_lock);
    for (int i = 0; i < NWANT; i++) {
        if (!wanted[i][0] || port_named(wanted[i])) continue;
        for (task_t *t = lookup_waiters; t; t = t->next)
            if (!strcmp(t->wname, wanted[i])) {
                size_t l = strlen(wanted[i]) + 1;
                memcpy(out + n, wanted[i], l);
                n += l;
                break;
            }
    }
    spin_unlock(&names_lock);
    if (n > size) n = size;
    if (n && vm_copy(cur->proc->as, (void *)buf, 0, out, n)) return -EFAULT;
    return n;
}

/* Take thread t (its tid bucket locked) off its wait for a reply from
 * srv (from any server if 0): 1 if it waited, and is now ours to wake.
 * Every such change is made under that lock: one waker, never two. (It
 * starts waiting with no lock: it is nobody's to wake before.) */
static int unwait(task_t *t, proc_t *srv)
{
    proc_t *s = t->server;
    if (!s || (srv && s != srv)) return 0;
    t->server = 0;
    return 1;
}
/* The same, for a call given up before its reply (a signal, its process
 * ending): its token is stale from now on, so the server's later reply
 * goes nowhere, not into the thread's next call. */
static int abandon(task_t *t, proc_t *srv)
{
    if (!unwait(t, srv)) return 0;
    t->gen = (t->gen + 1) & 0x7FFFFFFF;
    return 1;
}
static long call_token(task_t *t) { return t->tid | (long)t->gen << 32; }   /* (t's call: gen stays) */
/* A call that must not wait for its reply: its process is ending (-EPIPE),
 * or a signal came for the thread (-EINTR). */
static long call_stop(task_t *t) { return may_stop(t) ? stop_why(t) : 0; }
int ipc_unreply(task_t *t)          /* (proc_stop) */
{
    task_t *x = task_lock(t->tid);  /* (0: it is ending, so not waiting) */
    int got = x && abandon(x, 0);
    if (x) task_unlock(x);
    return got;
}

long ipc_call(long id, msg_t *um)
{
    task_t *me = cur;
    port_t *p = port_get(id);
    if (!p) return -ENOENT;
    me->umsg = um;
    task_t *r = pop_receiver(p);
    if (!r) {                          /* the server is busy: queue up */
        me->next = 0;
        if (p->tail) p->tail->next = me; else p->head = me;
        p->tail = me;
        me->wport = p;
        me->wport_id = id;
        long e = blocked(me, TS_SEND);
        if (e) unqueue(p, me);
        port_put(p);
        if (e) return e;
        schedule();                    /* sleep until ipc_reply (or the server dies) */
        return me->ret;
    }
    proc_t *owner = p->owner;          /* a server thread waits: deliver now */
    port_put(p);
    long e = transfer(me, r);
    r->ret = e ? e : call_token(me);   /* ipc_recv returns the token, for the reply */
    if (e) { ready(r); return e; }
    ready_first(r);                    /* we wait now: the server gets this CPU at once ("handoff") */
    me->state = TS_REPLY;
    __atomic_store_n(&me->server, owner, __ATOMIC_RELEASE);   /* from now on its reply may come */
    if ((e = call_stop(me))) {         /* ending, or a signal: no waiting, unless a reply is coming */
        task_lock(me->tid);
        int bail = abandon(me, owner);
        task_unlock(me);
        if (bail) { me->state = TS_RUN; return e; }   /* (r runs next all the same) */
    }
    schedule();
    return me->ret;
}

/* Receive on p (its bucket locked; unlocked on return). */
static long recv_locked(port_t *p, msg_t *um)
{
    task_t *me = cur;
    me->umsg = um;
    if (p->irqs || p->notes) {
        uint64_t irqs = p->irqs, notes = p->notes;
        p->irqs = p->notes = 0;
        port_put(p);
        return deliver(me, irqs, notes);
    }
    task_t *s = p->head;
    if (s) {                           /* a client waits: take it */
        if (!(p->head = s->next)) p->tail = 0;
        s->wport = 0;
        proc_t *owner = p->owner;
        port_put(p);
        long e = transfer(s, me);
        if (e) { s->ret = e; ready(s); return e; }
        /* It waits for our reply now. Under its tid bucket's lock, so that
         * no reply can take it before it is set, and it cannot end before
         * we have checked whether its process is ending (proc_stop may
         * have found it queued, then taken off: then we wake it). */
        long tok = call_token(s);
        task_lock(s->tid);
        s->state = TS_REPLY;
        __atomic_store_n(&s->server, owner, __ATOMIC_RELEASE);
        long stop = call_stop(s);
        if (stop && !abandon(s, owner)) stop = 0;
        task_unlock(s);
        if (stop) { s->ret = stop; ready(s); }
        return tok;
    }
    me->next = p->receivers;           /* nothing yet: sleep */
    p->receivers = me;
    me->wport = p;
    me->wport_id = p->id;
    long e = blocked(me, TS_RECV);
    if (e) { p->receivers = me->next; me->wport = 0; }
    port_put(p);
    if (e) return e;
    schedule();
    return me->ret;
}

long ipc_recv(long id, msg_t *um)
{
    port_t *p = port_get(id);
    if (!p || p->owner != cur->proc) { if (p) port_put(p); return -EPERM; }
    return recv_locked(p, um);
}

/* The client waiting for our reply (tok: its thread and call), now ours
 * to wake; 0 if there is none (a stale token: that client is gone, or
 * that call was answered or interrupted). */
static task_t *claim_client(long tok)
{
    if (tok <= 0) return 0;
    task_t *c = task_lock((int)(tok & 0x7FFFFFFF));
    if (!c) return 0;
    int ok = c->gen == (uint64_t)tok >> 32 && unwait(c, cur->proc);
    task_unlock(c);
    return ok ? c : 0;
}

/* Answer client `token`. The server does not wait: it goes on at once. */
long ipc_reply(long token, msg_t *um)
{
    task_t *c = claim_client(token);
    if (!c) return -ESRCH;
    cur->umsg = um;
    long r = c->ret = transfer(cur, c);
    ready(c);
    return r;
}

/* Reply, then receive the next message: what a server does in its loop.
 * If no message is waiting, the server is about to sleep, so the client
 * it answered takes this CPU at once: a whole round trip then stays on one
 * CPU, with no inter-processor interrupt. (A stale token is ignored: that
 * client is gone; the server just receives. The reply goes out even if
 * the port is not the caller's: -EPERM then.) */
long ipc_reply_recv(long token, long id, msg_t *um)
{
    task_t *c = claim_client(token);
    if (c) {
        cur->umsg = um;
        c->ret = transfer(cur, c);
    }
    port_t *p = port_get(id);
    if (!p || p->owner != cur->proc) {
        if (p) port_put(p);
        if (c) ready(c);
        return -EPERM;
    }
    if (c) { if (p->irqs || p->notes || p->head) ready(c); else ready_first(c); }
    return recv_locked(p, um);
}

/* ---- Interrupts for drivers. An interrupt line is masked when it fires
 * (cpu.c); every driver bound to it gets the message (a PCI line may be
 * shared by several devices: each driver checks its own device's status),
 * and the line is unmasked once all of them have said they are done
 * (irq_ack). Each bound line is sent to a CPU of its own, in turn: an
 * interrupt costs only a wake-up, so spreading them keeps any one CPU
 * from taking all of them; the driver thread it wakes then runs on
 * whichever CPU is free. */
long irq_bind(int irq, long id)
{
    int level = irq & IRQ_LEVEL;
    irq &= ~IRQ_LEVEL;
    if (!cur->proc->driver) return -EPERM;
    port_t *p = port_get(id);
    int mine = p && p->owner == cur->proc;
    if (p) port_put(p);
    if (irq < 1 || irq >= MAXIRQ || ARCH_IRQ_RESERVED(irq) || !mine) return -EINVAL;
    spin_lock(&irq_lock);
    int k = -1, first = 1;
    long r = 0;
    for (int i = 0; i < IRQSHARE; i++) {
        if (irq_port[irq][i] == id) goto out;            /* already bound to this port */
        if (irq_port[irq][i]) first = 0; else if (k < 0) k = i;
    }
    r = -EBUSY;
    if (k < 0) goto out;                                 /* IRQSHARE drivers on it already */
    if (level) irq_set_level(irq);
    r = -EINVAL;
    if (first && irq_route(irq, irq_cpu++ % ncpus())) goto out;
    irq_port[irq][k] = id;
    if (!irq_need[irq]) irq_unmask(irq);
    r = 0;
out:
    spin_unlock(&irq_lock);
    return r;
}

long irq_ack(int irq)
{
    if (irq < 1 || irq >= MAXIRQ) return -EPERM;
    long r = -EPERM;
    spin_lock(&irq_lock);
    for (int i = 0; i < IRQSHARE && r; i++) {
        port_t *p = port_get(irq_port[irq][i]);
        int mine = p && p->owner == cur->proc;
        if (p) port_put(p);
        if (!mine) continue;
        irq_need[irq] &= ~(1 << i);
        if (!irq_need[irq]) irq_unmask(irq);             /* all its drivers are done */
        r = 0;
    }
    spin_unlock(&irq_lock);
    return r;
}

void irq_raise(int irq)
{
    task_t *r[IRQSHARE];
    uint64_t irqs[IRQSHARE], notes[IRQSHARE];
    int n = 0;
    spin_lock(&irq_lock);
    for (int i = 0; i < IRQSHARE; i++) {
        port_t *p = port_get(irq_port[irq][i]);
        if (!p) continue;
        irq_need[irq] |= 1 << i;
        p->irqs |= IRQ_BIT(irq);
        if ((r[n] = pop_receiver(p))) {
            irqs[n] = p->irqs; notes[n] = p->notes;
            p->irqs = p->notes = 0;
            n++;
        }
        port_put(p);
    }
    spin_unlock(&irq_lock);
    for (int i = 0; i < n; i++) { r[i]->ret = deliver(r[i], irqs[i], notes[i]); ready(r[i]); }
}

/* Thread t of an ending process stops waiting on a port or a name, if it
 * still does (proc_stop): 1 if we took it off, and so must wake it. */
int ipc_unblock(task_t *t, int state)
{
    int got = 0;
    if (state == TS_LOOKUP) {
        spin_lock(&names_lock);
        if (t->state == TS_LOOKUP)
            for (task_t **w = &lookup_waiters; *w; w = &(*w)->next)
                if (*w == t) { *w = t->next; got = 1; break; }
        spin_unlock(&names_lock);
        return got;
    }
    port_t *p = port_get(t->wport_id);
    if (!p) return 0;
    if (t->wport == p && t->state == state) {
        if (state == TS_SEND) unqueue(p, t);
        else {
            for (task_t **q = &p->receivers; *q; q = &(*q)->next)
                if (*q == t) { *q = t->next; break; }
            t->wport = 0;
        }
        got = 1;
    }
    port_put(p);
    return got;
}

/* Process p has ended (all its threads): close its ports, and fail the
 * calls waiting on it. Nobody can find a port once it is out of its
 * bucket; those still queued are ours to wake. */
void ipc_exit(proc_t *p)
{
    spin_lock(&p->lock);
    port_t *list = p->ports;
    p->ports = 0;
    spin_unlock(&p->lock);
    while (list) {
        port_t *pt = list;
        list = pt->tnext;
        spin_lock(&port_hash[pt->id % NPORT].lock);
        for (port_t **h = &port_hash[pt->id % NPORT].head; *h; h = &(*h)->hnext) if (*h == pt) { *h = pt->hnext; break; }
        task_t *clients = pt->head, *servers = pt->receivers;
        for (task_t *s = clients; s; s = s->next) s->wport = 0;
        for (task_t *s = servers; s; s = s->next) s->wport = 0;
        spin_unlock(&port_hash[pt->id % NPORT].lock);
        if (pt->name[0]) {
            spin_lock(&names_lock);
            for (port_t **h = &named; *h; h = &(*h)->nnext) if (*h == pt) { *h = pt->nnext; break; }
            spin_unlock(&names_lock);
        }
        spin_lock(&irq_lock);
        for (int q = 0; q < MAXIRQ; q++) {               /* its interrupt lines: the others carry on */
            int left = 0, had = 0;
            for (int i = 0; i < IRQSHARE; i++) {
                if (irq_port[q][i] == pt->id) { irq_port[q][i] = 0; irq_need[q] &= ~(1 << i); had = 1; }
                left |= irq_port[q][i] != 0;
            }
            if (!had) continue;
            if (!left) irq_mask(q);
            else if (!irq_need[q]) irq_unmask(q);
        }
        spin_unlock(&irq_lock);
        /* Calls still queued never reached the server: -ENOENT tells the
         * client's library that sending them again is always safe. */
        wake_list(clients, -ENOENT);
        wake_list(servers, -EPIPE);
        kfree(pt);
    }
    /* Clients waiting for a reply from p: none can start waiting now (p
     * has no thread left to take a message). */
    spin_lock(&proc_lock);
    for (proc_t *q = all_procs; q; q = q->anext) {
        spin_lock(&q->lock);
        for (task_t *c = q->threads; c; c = c->tnext) {
            if (c->server != p) continue;
            task_t *x = task_lock(c->tid);           /* (0: it is ending, so not waiting) */
            int got = x && unwait(x, p);
            if (x) task_unlock(x);
            if (got) { c->ret = -EPIPE; ready(c); }
        }
        spin_unlock(&q->lock);
    }
    spin_unlock(&proc_lock);
}

/* A kernel event for a server (NOTE_CHILD: one of its children ended), on
 * a port it owns: delivered like interrupts, as a "token 0" message. */
void port_notify(long id, proc_t *owner, uint64_t note)
{
    port_t *p = port_get(id);
    if (!p) return;
    if (p->owner != owner) { port_put(p); return; }
    p->notes |= note;
    task_t *r = pop_receiver(p);
    uint64_t irqs = 0, notes = 0;
    if (r) { irqs = p->irqs; notes = p->notes; p->irqs = p->notes = 0; }
    port_put(p);
    if (r) { r->ret = deliver(r, irqs, notes); ready(r); }
}

/* SYS_CHILD_PORT: where to tell the caller about its ending children. */
long child_port(long id)
{
    port_t *p = id ? port_get(id) : 0;
    int mine = p && p->owner == cur->proc;
    if (p) port_put(p);
    if (id && !mine) return -EINVAL;
    cur->proc->child_port = id;
    return 0;
}
