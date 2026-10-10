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
#define NPORT 256                    /* hash buckets */
static port_t *port_hash[NPORT], *named;
static long next_port = 1;
static task_t *lookup_waiters;  /* threads waiting in port_lookup */
#define IRQSHARE 4               /* drivers that may share one interrupt line (PCI lines are shared) */
static long irq_port[MAXIRQ][IRQSHARE];   /* the ports each interrupt goes to (0: none) */
static uint8_t irq_need[MAXIRQ];  /* which of them still have to acknowledge the last one */
static int irq_cpu;             /* the CPU the next bound interrupt goes to */

static port_t *port_get(long id)
{
    for (port_t *p = port_hash[id % NPORT]; id > 0 && p; p = p->hnext)
        if (p->id == id) return p;
    return 0;
}

static port_t *port_named(const char *name)
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

/* Hand the pending interrupts of port p to server thread r (a token 0 message). */
static long irq_deliver(port_t *p, task_t *r)
{
    msg_t m;
    if (vm_copy(0, &m, r->proc->as, r->umsg, sizeof m)) return -EFAULT;
    memset(m.w, 0, sizeof m.w);
    m.w[0] = p->irqs;
    m.w[1] = p->notes;
    m.rlen = 0; m.pid = m.uid = m.gid = 0;
    p->irqs = p->notes = 0;
    return vm_copy(r->proc->as, r->umsg, 0, &m, sizeof m);
}

static task_t *pop_receiver(port_t *p)
{
    task_t *r = p->receivers;
    if (r) p->receivers = r->next;
    return r;
}

long port_create(const char *name)
{
    if (port_named(name)) return -EEXIST;
    port_t *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    p->id = next_port++;
    p->owner = cur->proc;
    strlcpy(p->name, name, sizeof p->name);
    p->hnext = port_hash[p->id % NPORT]; port_hash[p->id % NPORT] = p;
    p->tnext = p->owner->ports; p->owner->ports = p;
    if (*name) {
        p->nnext = named; named = p;
        for (task_t **w = &lookup_waiters; *w; ) {     /* wake whoever waits for this name */
            task_t *t = *w;
            if (strcmp(t->wname, name)) { w = &t->next; continue; }
            *w = t->next;
            t->ret = p->id;
            ready(t);
        }
    }
    return p->id;
}

/* ---- Servers started on demand. The supervisor declares the port names
 * whose server it starts only when needed (SYS_WANT): the network, the
 * assistant... A lookup of such a name that finds no port wakes the
 * supervisor (NOTE_WANT); it asks which names are waited for (SYS_WANTED),
 * starts their servers, and the waiters wake when the port appears, as
 * with any server that starts late. No polling anywhere. */
#define NWANT 16
static char wanted[NWANT][16];

static int is_wanted(const char *name)
{
    for (int i = 0; i < NWANT; i++) if (wanted[i][0] && !strcmp(wanted[i], name)) return 1;
    return 0;
}

/* Find a port by name; if it does not exist yet, wait until it does (so
 * programs need not care in which order the servers start), unless
 * LOOKUP_NOWAIT. */
long port_lookup(const char *name, int flags)
{
    port_t *p = port_named(name);
    if (p) return p->id;
    if (!*name) return -EINVAL;
    if (flags & LOOKUP_NOWAIT) return -ENOENT;
    strlcpy(cur->wname, name, sizeof cur->wname);
    cur->state = TS_LOOKUP;
    cur->next = lookup_waiters;
    lookup_waiters = cur;
    if (is_wanted(name) && super && super->child_port) port_notify(super->child_port, super, NOTE_WANT);
    schedule();
    return cur->ret;
}

/* SYS_WANT: declare an on-demand name (err 0), or fail its waiters (err < 0). */
long port_want(const char *name, long err)
{
    if (!cur->proc->super) return -EPERM;
    if (!*name || err > 0 || err < -4095) return -EINVAL;
    if (!err) {
        if (is_wanted(name)) return 0;
        for (int i = 0; i < NWANT; i++)
            if (!wanted[i][0]) { strlcpy(wanted[i], name, sizeof wanted[i]); return 0; }
        return -ENOMEM;
    }
    for (task_t **w = &lookup_waiters; *w; ) {
        task_t *t = *w;
        if (strcmp(t->wname, name)) { w = &t->next; continue; }
        *w = t->next;
        t->ret = err;
        ready(t);
    }
    return 0;
}

/* SYS_WANTED: the on-demand names without a port that someone waits for. */
long port_wanted(uint64_t buf, uint64_t size)
{
    if (!cur->proc->super) return -EPERM;
    char out[NWANT * 16];
    size_t n = 0;
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
    if (n > size) n = size;
    if (n && vm_copy(cur->proc->as, (void *)buf, 0, out, n)) return -EFAULT;
    return n;
}

long ipc_call(long id, msg_t *um)
{
    port_t *p = port_get(id);
    if (!p) return -ENOENT;
    cur->umsg = um;
    task_t *r = pop_receiver(p);
    if (r) {                           /* a server thread waits: deliver now */
        long e = transfer(cur, r);
        r->ret = e ? e : cur->tid;     /* ipc_recv returns the token: the client thread's id */
        if (e) { ready(r); return e; }
        ready_first(r);                /* we wait now: the server gets this CPU at once ("handoff") */
        cur->state = TS_REPLY;
        cur->server = p->owner;
    } else {                           /* the server is busy: queue up */
        cur->next = 0;
        if (p->tail) p->tail->next = cur; else p->head = cur;
        p->tail = cur;
        cur->wport = p;
        cur->state = TS_SEND;
    }
    schedule();                        /* sleep until ipc_reply (or the server dies) */
    return cur->ret;
}

long ipc_recv(long id, msg_t *um)
{
    port_t *p = port_get(id);
    if (!p || p->owner != cur->proc) return -EPERM;
    cur->umsg = um;
    if (p->irqs || p->notes) return irq_deliver(p, cur);
    if (p->head) {                     /* a client waits: take it */
        task_t *s = p->head;
        if (!(p->head = s->next)) p->tail = 0;
        long e = transfer(s, cur);
        if (e) { s->ret = e; ready(s); return e; }
        s->state = TS_REPLY;
        s->server = p->owner;
        return s->tid;
    }
    cur->next = p->receivers;          /* nothing yet: sleep */
    p->receivers = cur;
    cur->wport = p;
    cur->state = TS_RECV;
    schedule();
    return cur->ret;
}

/* Answer client `token`. The server does not wait: it goes on at once. */
long ipc_reply(long token, msg_t *um)
{
    task_t *c = task_find(token);
    if (!c || c->state != TS_REPLY || c->server != cur->proc) return -ESRCH;
    cur->umsg = um;
    c->ret = transfer(cur, c);
    ready(c);
    return c->ret;
}

/* Reply, then receive the next message: what a server does in its loop.
 * If no message is waiting, the server is about to sleep, so the client
 * it answered takes this CPU at once: a whole round trip then stays on one
 * CPU, with no inter-processor interrupt. (A stale token is ignored: that
 * client is gone; the server just receives.) */
long ipc_reply_recv(long token, long id, msg_t *um)
{
    port_t *p = port_get(id);
    if (!p || p->owner != cur->proc) return -EPERM;
    task_t *c = task_find(token);
    if (c && c->state == TS_REPLY && c->server == cur->proc) {
        cur->umsg = um;
        c->ret = transfer(cur, c);
        if (p->irqs || p->notes || p->head) ready(c); else ready_first(c);
    }
    return ipc_recv(id, um);
}

/* ---- Interrupts for drivers. An interrupt line is masked when it fires
 * (cpu.c); every driver bound to it gets the message (a PCI line may be
 * shared by several devices: each driver checks its own device's status),
 * and the line is unmasked once all of them have said they are done
 * (irq_ack). Each bound line is sent to a CPU of its own, in turn: an
 * interrupt costs only the kernel lock and a wake-up, so spreading them
 * keeps any one CPU from taking all of them; the driver thread it wakes
 * then runs on whichever CPU is free. */
long irq_bind(int irq, long id)
{
    port_t *p = port_get(id);
    int level = irq & IRQ_LEVEL;
    irq &= ~IRQ_LEVEL;
    if (!cur->proc->driver) return -EPERM;
    if (irq < 1 || irq >= MAXIRQ || ARCH_IRQ_RESERVED(irq) || !p || p->owner != cur->proc) return -EINVAL;
    int k = -1, first = 1;
    for (int i = 0; i < IRQSHARE; i++) {
        if (irq_port[irq][i] == id) return 0;            /* already bound to this port */
        if (irq_port[irq][i]) first = 0; else if (k < 0) k = i;
    }
    if (k < 0) return -EBUSY;                            /* IRQSHARE drivers on it already */
    if (level) irq_set_level(irq);
    if (first && irq_route(irq, irq_cpu++ % ncpu)) return -EINVAL;
    irq_port[irq][k] = id;
    if (!irq_need[irq]) irq_unmask(irq);
    return 0;
}

long irq_ack(int irq)
{
    if (irq < 1 || irq >= MAXIRQ) return -EPERM;
    for (int i = 0; i < IRQSHARE; i++) {
        port_t *p = port_get(irq_port[irq][i]);
        if (!p || p->owner != cur->proc) continue;
        irq_need[irq] &= ~(1 << i);
        if (!irq_need[irq]) irq_unmask(irq);             /* all its drivers are done */
        return 0;
    }
    return -EPERM;
}

void irq_raise(int irq)
{
    for (int i = 0; i < IRQSHARE; i++) {
        port_t *p = port_get(irq_port[irq][i]);
        if (!p) continue;
        irq_need[irq] |= 1 << i;
        p->irqs |= IRQ_BIT(irq);
        task_t *r = pop_receiver(p);
        if (r) { r->ret = irq_deliver(p, r); ready(r); }
    }
}

/* Thread t of an ending process stops waiting on a port or a name. */
void ipc_unblock(task_t *t)
{
    task_t **q = t->state == TS_LOOKUP ? &lookup_waiters
               : t->state == TS_RECV ? &t->wport->receivers : &t->wport->head;
    task_t *prev = 0;
    for (; *q; prev = *q, q = &(*q)->next)
        if (*q == t) {
            *q = t->next;
            if (t->state == TS_SEND && t->wport->tail == t) t->wport->tail = prev;
            return;
        }
}

/* Process p is ending: close its ports, and fail the calls waiting on it. */
void ipc_exit(proc_t *p)
{
    while (p->ports) {
        port_t *pt = p->ports;
        p->ports = pt->tnext;
        /* Calls still queued never reached the server: -ENOENT tells the
         * client's library that sending them again is always safe. */
        for (task_t *s = pt->head, *n; s; s = n) { n = s->next; s->ret = -ENOENT; ready(s); }
        for (task_t *s = pt->receivers, *n; s; s = n) { n = s->next; s->ret = -EPIPE; ready(s); }
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
        for (port_t **h = &port_hash[pt->id % NPORT]; *h; h = &(*h)->hnext) if (*h == pt) { *h = pt->hnext; break; }
        for (port_t **h = &named; *h; h = &(*h)->nnext) if (*h == pt) { *h = pt->nnext; break; }
        kfree(pt);
    }
    for (proc_t *q = all_procs; q; q = q->anext)
        for (task_t *c = q->threads; c; c = c->tnext)
            if (c->state == TS_REPLY && c->server == p) { c->ret = -EPIPE; ready(c); }
}

/* A kernel event for a server (NOTE_CHILD: one of its children ended), on
 * a port it owns: delivered like interrupts, as a "token 0" message. */
void port_notify(long id, proc_t *owner, uint64_t note)
{
    port_t *p = port_get(id);
    if (!p || p->owner != owner) return;
    p->notes |= note;
    task_t *r = pop_receiver(p);
    if (r) { r->ret = irq_deliver(p, r); ready(r); }
}

/* SYS_CHILD_PORT: where to tell the caller about its ending children. */
long child_port(long id)
{
    port_t *p = port_get(id);
    if (id && (!p || p->owner != cur->proc)) return -EINVAL;
    cur->proc->child_port = id;
    return 0;
}
