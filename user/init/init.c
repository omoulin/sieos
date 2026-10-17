/*
 * init - The supervisor: the only program the kernel starts. It starts the
 * servers, starts each again when it fails, and starts some only when
 * they are needed:
 *
 *     kernel ─► init ─┬─► con   (boot module, driver)  the console          at boot
 *                     ├─► vblk  (boot module, driver)  the disk             at boot
 *                     ├─► acpi  (/bin/acpid, driver)   power, battery, lid  at boot (x86-64)
 *                     ├─► fs    (boot module)          the files            at boot
 *                     ├─► login (/bin/login)           the console's prompt at boot
 *                     ├─► atlas (/bin/atlas, driver)   the desktop          at boot
 *                     ├─► auth  (/bin/auth)            accounts             port "auth"
 *                     ├─► vnet  (/bin/vnet, driver)    the network card     port "nic0"
 *                     ├─► netd  (/bin/netd)            TCP/IP, DHCP, DNS    port "net"
 *                     ├─► audio (/bin/audiod, driver)  sound                port "audio"
 *                     ├─► bt    (/bin/btd, driver)     Bluetooth            at boot
 *                     └─► sia   (/bin/siad, 4.5 GiB)   the assistant        port "sia"
 *
 * On demand: a server with a port in the table is not started at boot.
 * init declares the port's name to the kernel (SYS_WANT); the first program
 * that looks the name up waits as usual, and the kernel wakes init
 * (NOTE_WANT), which starts the server; the waiter wakes when the port
 * appears. A chain works the same way: netd looks up "nic0", which starts
 * vnet. Why these four: they serve occasional work (a login, the network,
 * a question) and hold memory meanwhile (the assistant's model: over 1
 * GiB). con, vblk, fs, login and atlas are needed all the time.
 *
 * Stopping unused ones: after its idle time without use, init's timer
 * thread asks the server "may you stop?" (SVC_MAYSTOP). A server says yes only
 * if nobody asked it anything since the last question and it holds no
 * client state (an open connection, a question being answered); it then
 * ends with status 0, and init marks it "idle" (no restart). The next use
 * starts it again. A call that arrives while it ends gets -ENOENT, which
 * the client's library always sends again: it reaches the new server.
 *
 * Failures: the kernel tells init when a child ends (NOTE_CHILD); init
 * starts it again at once, unless it failed right after starting (within
 * a second): then it waits 10 ms, 20 ms, 40 ms ... up to 1 s, and gives up
 * after 5 such failures in a row. `svc restart NAME` (root) tries again.
 *
 * The boot modules' images stay in the kernel (SPAWN_MODULE): a crashed
 * disk driver can come back even though the disk is unreachable without it.
 * Only init may give device rights (SPAWN_DRIVER), and it adopts processes
 * whose parent ended: if login crashes, its open session becomes init's
 * child, and the next login prompt waits until that session ends.
 *
 * Background work: a service marked so (bg in the table, `svc add -b`) is
 * started with SPAWN_BACKGROUND: on a processor with P-cores and E-cores,
 * its threads run on E-cores (the others, servers and the assistant
 * included, on P-cores first). No effect where all cores are alike.
 *
 * Stuck servers: a server that does not end but stops answering (a loop
 * that never ends, a deadlock) is as gone as a crashed one. A watcher thread
 * pings the core servers every 10 s (SVC_PING: any answer will do); one that
 * has not answered after 30 s is ended, and so started again like any that
 * failed. (Not sia: an answer may keep it computing for a minute.)
 *
 * Few wake-ups when nothing happens: the timer thread sleeps until the next
 * restart or idle question is due, or for good; the watcher wakes every 10 s.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#define MAXSVC     SVC_MAX
#define QUICK_NS   1000000000L          /* ended within 1 s of starting: a "quick" failure */
#define GIVE_UP    5                    /* quick failures in a row */
#define BACKOFF0   10000000L            /* 10 ms, doubled each quick failure, ... */
#define BACKOFF1   1000000000L          /* ... up to 1 s */
#define MIN        60000000000L         /* a minute, in ns */

typedef struct {
    svc_info_t i;                       /* name, state, pid, restarts, ..., port, idle_ns, starts */
    char src[64], args[64];             /* boot module name, or a path on the disk */
    int module;                         /* its boot module number, or 0: from the disk */
    int console;                        /* uses the console: never two at once (login) */
    int quick;                          /* quick failures in a row */
    int64_t when;                       /* SVC_WAITING: when to start it (ns since boot) */
    int64_t died;                       /* when it ended, to measure the restart */
    uint64_t quota;                     /* its memory limit in bytes (0: none; SPAWN_QUOTA) */
    int64_t check_at;                   /* on demand: when to ask "may you stop?" */
    int asking, stopping;               /* the question is out / the server said yes */
    const char *watch;                  /* a core server: its port, pinged (see "Stuck servers") */
    char watchbuf[16];                  /* (svc add -w: the port's name) */
    int64_t ping_t0;                    /* a ping out since then (0: none) ... */
    int ping_pid;                       /* ... to this process */
} svc_t;
#define WATCH_EVERY 10000000000L        /* 10 s between pings */
#define WATCH_NS    30000000000L        /* no answer for 30 s: stuck */

/* .i: name, state, pid, restarts, last status, policy, driver, started, and
 * for a service started on demand: the port that starts it, its idle time. */
static svc_t svc[MAXSVC] = {
    { .i = { "con",   0, 0, 0, 0, SVC_ALWAYS, 1 }, "con",        "con", .watch = "console" },
    /* USB (keys, keyboards, mice): before vblk, which may find SIEOS's disk on a
     * USB key; with no USB controller it ends with 0 and is not started again */
    { .i = { "usb",   0, 0, 0, 0, SVC_ON_FAILURE, 1 }, "usb",        "usb", .watch = "usb" },
    { .i = { "vblk",  0, 0, 0, 0, SVC_ALWAYS, 1 }, "vblk",       "vblk", .watch = "disk" },
    { .i = { "fs",    0, 0, 0, 0, SVC_ALWAYS, 0 }, "fs",         "fs", .watch = "fs" },
    /* accounts: a login or a passwd now and then; 1 ms to start */
    { .i = { "auth",  0, 0, 0, 0, SVC_ALWAYS, 0, 0, "auth", 5 * MIN }, "/bin/auth", "auth", .watch = "auth" },
    { .i = { "login", 0, 0, 0, 0, SVC_ALWAYS, 0 }, "/bin/login", "login", .console = 1 },
#if defined(__x86_64__)
    /* ACPI (a driver): sleep types for the kernel, battery, lid, buttons; with no
     * ACPI tables it ends with 0 */
    { .i = { "acpi",  0, 0, 0, 0, SVC_ON_FAILURE, 1 }, "/bin/acpid", "acpid", .watch = "acpi" },
    /* a laptop's I2C touchpad (a driver, from the ACPI tables); with none it ends with 0 */
    { .i = { "i2chid", 0, 0, 0, 0, SVC_ON_FAILURE, 1 }, "/bin/i2chid", "i2chid" },
#endif
    /* the desktop (screen: a driver); with no display card it ends with 0: not restarted */
    { .i = { "atlas", 0, 0, 0, 0, SVC_ON_FAILURE, 1 }, "/bin/atlas", "atlas", .watch = "atlas" },
    /* the console's log on a Pimoroni GFX HAT (a Pi's screen and buttons, a driver);
     * with no HAT (or no Pi) it ends with 0 at once and is not started again.
     * Background work: on a processor with E-cores, it runs there */
    { .i = { "gfxhat", 0, 0, 0, 0, SVC_ON_FAILURE, 1, .bg = 1 }, "/bin/gfxhat", "gfxhat" },
    /* the pins (a Pi's GPIO; simulated elsewhere): started by its first user, then
     * kept (a pattern may be playing) */
    { .i = { "gpio",  0, 0, 0, 0, SVC_ALWAYS, 1, 0, "gpio", 0 }, "/bin/gpiod", "gpiod" },
    /* sound (a driver): started by its first user, then kept (the volume) */
    { .i = { "audio", 0, 0, 0, 0, SVC_ALWAYS, 1, 0, "audio", 0 }, "/bin/audiod", "audiod" },
    /* the network: kept a while, since starting means a new DHCP exchange;
     * the card's driver stops after netd (it is busy while netd runs) */
    { .i = { "vnet",  0, 0, 0, 0, SVC_ALWAYS, 1, 0, "nic0", 10 * MIN }, "/bin/vnet", "vnet" },
    /* Wi-Fi (a driver): the port "wifi" (netd, Settings, /bin/wifi), the
     * Raspberry Pis' CYW43455 or a PC's Intel AX210. Started at boot so the
     * port is there to find; on a machine with no Wi-Fi chip it ends with 0
     * and is not started again */
    { .i = { "wifi",  0, 0, 0, 0, SVC_ON_FAILURE, 1 }, "/bin/wifid", "wifid", .watch = "wifi" },
    /* Bluetooth (a driver, docs/bluetooth.md): the port "bt"; at boot, so bonded
     * keyboards and mice come back; with no adapter it ends with 0 */
    { .i = { "bt",    0, 0, 0, 0, SVC_ON_FAILURE, 1 }, "/bin/btd", "btd", .watch = "bt" },
    { .i = { "netd",  0, 0, 0, 0, SVC_ALWAYS, 0, 0, "net", 10 * MIN },  "/bin/netd", "netd" },
    /* pipes and unix sockets for programs built on libposix (docs/webkit-plan.md):
     * started by the first, stopped when no end is held */
    { .i = { "unixd", 0, 0, 0, 0, SVC_ALWAYS, 0, 0, "unix", 10 * MIN }, "/bin/unixd", "unixd" },
    /* the SSH server (docs/ssh.md): only where /etc/ssh/sshd_config exists, else it
     * ends with 0 at once and is not started again; it listens, so it starts the
     * network at boot */
    { .i = { "sshd",  0, 0, 0, 0, SVC_ON_FAILURE, 0 }, "/bin/sshd", "sshd" },
    /* the assistant: loading the model takes seconds, then it holds 1+ GiB */
    { .i = { "sia",   0, 0, 0, 0, SVC_ALWAYS, 0, 0, "sia", 15 * MIN },  "/bin/siad", "siad",
      .quota = 4608UL << 20 },      /* the model may use 4 GiB, plus the server: 4.5 GiB at most */
};
static int nsvc, self;                  /* nsvc: counted in main (the table ends at an empty name) */
static long port, tick_from;            /* tick_from: the timer thread, waiting for work */
static long ticker_tid;                 /* the timer thread, ... */
static int64_t asleep_until;            /* ... and when its current sleep ends (0: not asleep) */

/* A line in the kernel log. */
static struct { char b[160]; size_t n; } line;
static void put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line.n = 0;
    vformat(put, 0, fmt, ap);
    va_end(ap);
    sys_debug(line.b, line.n);
}

static void start(svc_t *s)
{
    mk_spawn_t o = { .flags = (s->i.driver ? SPAWN_DRIVER : 0) | (s->quota ? SPAWN_QUOTA : 0) |
                              (s->i.bg ? SPAWN_BACKGROUND : 0), .quota = s->quota };
    long pid;
    if (s->module) {
        o.flags |= SPAWN_MODULE;
        pid = sys_spawn(0, s->module, s->args, 0, 0, &o);
    } else pid = spawn_file(s->src, s->args, 0, 0, &o);
    s->i.started_ns = sys_clock();
    s->asking = s->stopping = 0;
    s->check_at = s->i.started_ns + s->i.idle_ns;
    if (pid < 0) {                      /* counts as a quick failure */
        say("init: cannot start %s (error %ld)\n", s->i.name, -pid);
        s->i.pid = 0;
        s->i.last_status = pid;
        s->died = s->i.started_ns;
        s->quick++;
        s->i.state = s->quick >= GIVE_UP ? SVC_GIVEN_UP : SVC_WAITING;
        if (s->i.state == SVC_GIVEN_UP && s->i.port[0]) sys_want(s->i.port, -ESRCH);   /* its waiters: an error */
        s->when = s->i.started_ns + (BACKOFF0 << (s->quick - 1 < 7 ? s->quick - 1 : 7));
        if (s->when - s->i.started_ns > BACKOFF1) s->when = s->i.started_ns + BACKOFF1;
        return;
    }
    s->i.pid = pid;
    s->i.state = SVC_RUNNING;
    s->i.starts++;
    if (s->died) say("init: %s restarted in %ld us (restart %d)\n", s->i.name, (sys_clock() - s->died) / 1000, s->i.restarts);
    else say("init: %s started (pid %ld)\n", s->i.name, pid);   /* (a photo of a screen with no serial port shows how far it got) */
}

/* Processes init adopted (a session whose login ended): still running? */
static int sessions(void)
{
    mk_task_t t[32];
    int n = 0, after = 0;
    long k;
    do {
        k = sys_tasks(t, 32, after);
        for (long j = 0; j < k; j++) {
            if (t[j].ppid != self) continue;
            int known = 0;
            for (int i = 0; i < nsvc; i++) if (svc[i].i.pid == t[j].pid) known = 1;
            n += !known;
        }
        if (k > 0) after = t[k - 1].pid;
    } while (k == 32);
    return n;
}

/* A service ended with status st: idle, or decide when to start it again. */
static void ended(svc_t *s, int st)
{
    int64_t now = sys_clock();
    /* It said yes to "may you stop?" and ended: idle. (Its end may come
     * before init reads the yes: ending cleanly while asked means yes.) */
    int pid = s->i.pid, stopping = s->stopping || s->asking;
    s->i.pid = 0;
    s->i.last_status = st;
    s->asking = s->stopping = 0;
    if (s->i.state == SVC_STOPPED) return;
    /* It agreed to stop, or (on demand) ended cleanly by itself, e.g. siad
     * after a new configuration: idle, started again when needed. */
    if (st == 0 && (stopping || s->i.port[0])) {
        s->i.state = SVC_IDLE;
        s->died = 0;
        if (stopping)
            say("init: %s stopped after %lu s unused; it starts again when needed\n", s->i.name,
                (unsigned long)(s->i.idle_ns / 1000000000));
        else say("init: %s ended cleanly; it starts again when needed\n", s->i.name);
        return;
    }
    s->died = now;
    if (s->i.policy == SVC_NEVER || (s->i.policy == SVC_ON_FAILURE && st == 0)) { s->i.state = SVC_STOPPED; return; }
    s->quick = now - (int64_t)s->i.started_ns < QUICK_NS ? s->quick + 1 : 0;
    s->i.restarts++;
    if (s->quick >= GIVE_UP) {
        s->i.state = SVC_GIVEN_UP;
        if (s->i.port[0]) sys_want(s->i.port, -ESRCH);
        say("init: %s keeps failing (%d times within 1 s of starting, last status %d): given up; "
            "'svc restart %s' tries again\n", s->i.name, s->quick, st, s->i.name);
        return;
    }
    int64_t wait = 0;
    if (s->quick) { wait = BACKOFF0 << (s->quick - 1); if (wait > BACKOFF1) wait = BACKOFF1; }
    say("init: %s (pid %d) ended with status %d: restarting%s\n", s->i.name, pid, st, wait ? " after a pause" : "");
    s->i.state = SVC_WAITING;
    s->when = now + wait;
}

static svc_t *find(const char *name)
{
    for (int i = 0; i < nsvc; i++) if (!strcmp(svc[i].i.name, name)) return &svc[i];
    return 0;
}

/* Someone waits for an on-demand server: start the idle ones; tell the
 * waiters of a stopped or given-up one that it will not come. */
static void activate(void)
{
    char w[256];
    long n = sys_wanted(w, sizeof w);
    for (char *p = w; n > 0 && p < w + n; p += strlen(p) + 1)
        for (int i = 0; i < nsvc; i++) {
            svc_t *s = &svc[i];
            if (strcmp(s->i.port, p)) continue;
            if (s->i.state == SVC_IDLE) start(s);
            else if (s->i.state == SVC_STOPPED || s->i.state == SVC_GIVEN_UP) sys_want(p, -ESRCH);
        }                               /* (RUNNING: it is starting; WAITING: it comes) */
}

/* Start what is due; returns how long until the next restart or idle
 * question is due (0: nothing pending). */
static int64_t due(void)
{
    int64_t now = sys_clock(), next = 0;
    int open = -1;                      /* adopted sessions, counted only if needed */
    for (int i = 0; i < nsvc; i++) {
        svc_t *s = &svc[i];
        int64_t left;
        if (s->i.state == SVC_RUNNING && s->ping_t0 && s->ping_pid == s->i.pid) {
            int64_t p = s->ping_t0 + WATCH_NS - now;  /* a ping out: stuck once its time is up */
            if (p <= 0) {
                say("init: %s (pid %d) does not answer for %ld s: stuck, ended\n", s->i.name, s->i.pid, WATCH_NS / 1000000000L);
                s->ping_t0 = 0;
                sys_kill(s->i.pid);              /* (its end comes as NOTE_CHILD: started again) */
                continue;
            }
            if (!next || p < next) next = p;
        }
        if (s->i.state == SVC_RUNNING && s->i.idle_ns && !s->asking && !s->stopping)
            left = s->check_at - now > 0 ? s->check_at - now : 1;
        else if (s->i.state == SVC_WAITING) {
            if (s->console && (open < 0 ? (open = sessions()) : open)) continue;   /* after the session */
            if (s->when <= now) { start(s); if (s->i.state != SVC_WAITING) continue; }
            left = s->when - now;
        } else continue;
        if (left > 0 && (!next || left < next)) next = left;
    }
    return next;
}

/* An on-demand server whose idle question is due, or 0. */
static svc_t *idle_due(void)
{
    int64_t now = sys_clock();
    for (int i = 0; i < nsvc; i++)
        if (svc[i].i.state == SVC_RUNNING && svc[i].i.idle_ns && !svc[i].asking && !svc[i].stopping &&
            svc[i].check_at <= now) return &svc[i];
    return 0;
}

/* The timer thread asks INIT_TICK. The answer is either a server to ask
 * "may you stop?" (its port name in rbuf), or how long to sleep; it waits
 * until something is pending, so there are no wake-ups otherwise. The
 * question is asked here, not by init's main thread, which must never
 * wait for a busy server. */
static void ticker(void *arg)
{
    (void)arg;
    char name[16];
    for (;;) {
        msg_t m = { .w = { INIT_TICK }, .rbuf = name, .rlen = sizeof name - 1 };
        if (ipc_call(port, &m) < 0) return;
        if (!m.rlen) { sys_sleep(m.w[0]); continue; }
        name[m.rlen] = 0;
        long p = port_find(name), r = -ENOENT;          /* never port_lookup: it would start it */
        msg_t q = { .w = { SVC_MAYSTOP, m.w[1] } };
        if (p > 0 && ipc_call(p, &q) >= 0) r = (long)q.w[0];
        msg_t a = { .w = { INIT_ANSWER, m.w[2], (uint64_t)r } };
        ipc_call(port, &a);
    }
}

/* The watcher: every 10 s, each running core server is pinged; init's main
 * thread hears when a ping goes out and when it comes back (INIT_PING), and
 * its timer ends a server whose ping stays out too long. This thread may wait
 * for a stuck server: the main thread never does. */
static void watcher(void *arg)
{
    (void)arg;
    for (;;) {
        sys_sleep(WATCH_EVERY);
        for (int i = 0; i < nsvc; i++) {
            if (!svc[i].watch) continue;
            msg_t go = { .w = { INIT_PING, (uint64_t)i, 1 } };
            if (ipc_call(port, &go) < 0 || (long)go.w[0] < 0) continue;   /* (not running) */
            long p = port_find(svc[i].watch);
            msg_t q = { .w = { SVC_PING } };
            if (p > 0) ipc_call(p, &q);                 /* (any answer; an error: it ended) */
            msg_t back = { .w = { INIT_PING, (uint64_t)i, 0 } };
            ipc_call(port, &back);
        }
    }
}

/* A request on port "init": fills the reply a; returns -EBUSY to leave it unanswered. */
static long serve(msg_t *m, msg_t *a, long from, char *req)
{
    static svc_info_t out[MAXSVC];
    svc_t *s;
    switch (m->w[0]) {
    case INIT_LIST:
        for (int i = 0; i < nsvc; i++) out[i] = svc[i].i;
        a->sbuf = out;
        a->slen = nsvc * sizeof *out;
        return nsvc;
    case INIT_RESTART:
    case INIT_STOP:
        if (m->uid) return -EPERM;
        if (!(s = find(req))) return -ENOENT;
        if (s->i.state == SVC_RUNNING) sys_kill(s->i.pid);      /* its end comes as NOTE_CHILD */
        s->quick = 0;
        s->i.state = m->w[0] == INIT_STOP ? SVC_STOPPED : SVC_WAITING;
        if (s->i.state == SVC_STOPPED && s->i.port[0]) sys_want(s->i.port, -ESRCH);
        s->when = 0;
        if (m->w[0] == INIT_RESTART && s->i.pid == 0) s->died = 0;
        return 0;
    case INIT_IDLE:
        if (m->uid) return -EPERM;
        if (!(s = find(req))) return -ENOENT;
        if (!s->i.port[0] || (long)m->w[1] < 0) return -EINVAL;  /* only on-demand services stop when idle */
        s->i.idle_ns = m->w[1] * 1000000000UL;
        s->check_at = sys_clock() + s->i.idle_ns;
        return 0;
    case INIT_ADD: {                    /* "name\0path\0args[\0port]", w[2] = idle seconds */
        if (m->uid) return -EPERM;
        char *path = req + strlen(req) + 1, *args = path + strlen(path) + 1, *dp = args + strlen(args) + 1;
        char *wp = "";                  /* the watched port, after the on-demand one */
        if (dp >= req + m->rlen) dp = "";
        else if (dp + strlen(dp) + 1 < req + m->rlen) wp = dp + strlen(dp) + 1;
        if (strlen(wp) > 15) wp = "";
        if (nsvc == MAXSVC) return -ENOMEM;
        if (!*req || strlen(req) > 15 || find(req) || path[0] != '/' || m->w[1] > SVC_NEVER || strlen(dp) > 15 ||
            (long)m->w[2] < 0)
            return -EINVAL;
        s = &svc[nsvc++];
        *s = (svc_t){ .i = { .policy = (int)m->w[1], .state = SVC_WAITING, .bg = m->w[3] == 1 } };
        strlcpy(s->i.name, req, sizeof s->i.name);
        strlcpy(s->src, path, sizeof s->src);
        strlcpy(s->args, *args ? args : req, sizeof s->args);
        if (*wp) { strlcpy(s->watchbuf, wp, sizeof s->watchbuf); s->watch = s->watchbuf; }
        if (*dp) {                      /* on demand: started when its port is looked up */
            strlcpy(s->i.port, dp, sizeof s->i.port);
            s->i.idle_ns = m->w[2] * 1000000000UL;
            s->i.state = SVC_IDLE;
            sys_want(dp, 0);
        }
        return 0;
    }
    case INIT_TICK: {
        if (m->pid != self) return -EPERM;
        asleep_until = 0;
        if ((s = idle_due())) {
            s->asking = 1;
            a->w[1] = s->i.idle_ns;
            a->w[2] = s - svc;
            a->sbuf = s->i.port;
            a->slen = strlen(s->i.port);
            return 0;
        }
        int64_t next = due();
        if (next) { asleep_until = sys_clock() + next; return next; }
        tick_from = from;               /* answered when something becomes pending */
        return -EBUSY;
    }
    case INIT_PING:
        if (m->pid != self || m->w[1] >= (uint64_t)nsvc) return -EPERM;
        s = &svc[m->w[1]];
        if (!m->w[2]) { s->ping_t0 = 0; return 0; }
        if (s->i.state != SVC_RUNNING) return -ESRCH;
        s->ping_t0 = sys_clock();
        s->ping_pid = s->i.pid;
        return 0;
    case INIT_RESUME:                   /* the devices lost their state while asleep */
        if (m->uid) return -EPERM;
        for (int i = 0; i < nsvc; i++)
            if ((s = &svc[i])->i.driver && s->i.state == SVC_RUNNING && s->i.pid != m->pid && strcmp(s->i.name, "atlas")) {
                say("init: %s started anew after the sleep\n", s->i.name);
                sys_kill(s->i.pid);     /* (its end comes as NOTE_CHILD: started again) */
            }
        return 0;
    case INIT_ANSWER: {
        if (m->pid != self || m->w[1] >= (uint64_t)nsvc) return -EPERM;
        s = &svc[m->w[1]];
        if (!s->asking) return 0;       /* (it ended meanwhile) */
        s->asking = 0;
        long r = (long)m->w[2];
        if (r == 0 && s->i.state == SVC_RUNNING) s->stopping = 1;   /* it ends now: NOTE_CHILD follows */
        else s->check_at = sys_clock() + (r > 0 ? r : (long)s->i.idle_ns);
        return 0;
    }
    }
    return -ENOSYS;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    mk_info_t in;
    sys_info(&in);
    self = in.self_pid;
    while (nsvc < MAXSVC && svc[nsvc].i.name[0]) nsvc++;

    /* Which boot module is which: the kernel lists their names in order. */
    char names[256];
    long n = sys_modules(names, sizeof names);
    for (int i = 0; i < nsvc; i++) {
        int k = 0;
        for (char *p = names; n > 0 && p < names + n; p += strlen(p) + 1, k++)
            if (k && !strcmp(p, svc[i].src)) svc[i].module = k;
        if (svc[i].src[0] != '/' && !svc[i].module) { svc[i].i.state = SVC_STOPPED; say("init: no boot module %s\n", svc[i].src); }
    }

    port = port_create("init");
    sys_child_port(port);
    for (int i = 0; i < nsvc; i++) {
        svc_t *s = &svc[i];
        if (s->i.state == SVC_STOPPED) continue;
        if (s->i.port[0]) { s->i.state = SVC_IDLE; sys_want(s->i.port, 0); }   /* started when needed */
        else start(s);
    }
    ticker_tid = thread_start(ticker, 0, 4096);
    thread_start(watcher, 0, 4096);

    static char req[256];
    for (;;) {
        msg_t m = { .rbuf = req, .rlen = sizeof req - 1 };
        long from = ipc_recv(port, &m);
        if (from < 0) continue;
        if (from == 0) {                /* children ended (services, or adopted sessions); wanted names */
            int st;
            long pid;
            while ((pid = sys_wait(-1, &st, WAIT_NOHANG)) > 0)
                for (int i = 0; i < nsvc; i++) if (svc[i].i.pid == pid) ended(&svc[i], st);
        } else {
            req[m.rlen < sizeof req ? m.rlen : sizeof req - 1] = 0;
            msg_t a = { 0 };
            long r = serve(&m, &a, from, req);
            if (r != -EBUSY || m.w[0] != INIT_TICK) {
                a.w[0] = r;
                if (r < 0) a.sbuf = 0, a.slen = 0;
                ipc_reply(from, &a);
            }
        }
        activate();                     /* someone waits for an on-demand server? */
        /* Start what is due now (a service that ended, a session that
         * closed); if a restart or a question must wait, wake the timer. */
        int64_t next = due();
        if (tick_from && (next || idle_due())) {
            reply_val(tick_from, next ? next : 1);
            tick_from = 0;
            asleep_until = sys_clock() + (next ? next : 1);
        } else if (asleep_until && next && sys_clock() + next < asleep_until) {
            sys_wake(ticker_tid);       /* something is due sooner (svc idle, a restart): wake it */
            asleep_until = 0;
        }
    }
}
