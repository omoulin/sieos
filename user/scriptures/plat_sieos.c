/*
 * plat_sieos.c - The window on SIEOS (plat.h): a window of the desktop's
 * own kind through atlas's protocol for other programs' windows
 * (include/mk/atlas.h). The app draws into shared memory atlas shows; a
 * thread waits for the window's events (ATLAS_WIN_EVENT) and queues them,
 * turned into the app's (ev_t); plat_wait sleeps on a futex until one comes,
 * a job's thread calls plat_wake, or the timeout ends.
 *
 * Keys come as characters (Ctrl applied) or KEY_... (mk/proto.h): Ctrl+letter
 * is a control character, Ctrl+Shift+letter KEY_CSA... There is no Alt, and
 * Ctrl+click is the middle button. The clipboard is atlas's.
 *
 * Also the program's start: the app's main (app.c, renamed scriptures_main)
 * runs on a thread with a 4 MiB stack (a SIEOS program's own is 32 KiB), its
 * data in ~/.scriptures.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/atlas.h"
#define EV_KEY EV_KEY_                   /* (plat.h's, not mk/proto.h's) */
#include "plat.h"

int setenv(const char *k, const char *v, int over);
int snprintf(char *s, size_t n, const char *fmt, ...);
char *strdup(const char *s);
extern void (*libc_atexit)(void);
_Noreturn void exit(int code);

static long aport, win;
static uint32_t *px;                 /* the shared pixels: bw x bh */
static int bw, bh;
static volatile int ww, wh, zoom = 100;   /* the window's insides (ATW_SIZE) */

static long call(msg_t *m) { long e = call_named(&aport, "atlas", m, 0); return e < 0 ? e : (long)m->w[0]; }

/* ---- the event queue */
#define QN 256
static ev_t q[QN];
static int qh, qn;
static char qlock;
static volatile int qseq;            /* futex word: bumped at each push */
static void lock(void) { while (__atomic_test_and_set(&qlock, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_clear(&qlock, __ATOMIC_RELEASE); }

static void push(const ev_t *e)
{
    lock();
    ev_t *last = qn ? &q[(qh + qn - 1) % QN] : 0;
    if (last && ((e->type == EV_MOVE && last->type == EV_MOVE) || (e->type == EV_WAKE && last->type == EV_WAKE) ||
                 (e->type == EV_RESIZE && last->type == EV_RESIZE)))
        *last = *e;                                  /* (moved on; woken already) */
    else if (qn < QN) q[(qh + qn++) % QN] = *e;
    __atomic_add_fetch(&qseq, 1, __ATOMIC_RELEASE);
    unlock();
    sys_futex(FUTEX_WAKE, &qseq, 1, 0);
}

int plat_wait(ev_t *e, int timeout)
{
    uint64_t end = timeout > 0 ? (uint64_t)sys_clock() + (uint64_t)timeout * 1000000 : 0;
    for (;;) {
        lock();
        int seq = qseq;
        if (qn) { *e = q[qh]; qh = (qh + 1) % QN; qn--; unlock(); return 1; }
        unlock();
        if (!timeout) return 0;
        uint64_t now = (uint64_t)sys_clock();
        if (timeout > 0 && now >= end) return 0;
        sys_futex(FUTEX_WAIT, &qseq, seq, timeout > 0 ? end - now : 0);
    }
}

void plat_wake(void) { ev_t e = { .type = EV_WAKE }; push(&e); }

/* ---- input: atlas's events -> the app's */
static int buttons;                  /* held (MB_...) */
static uint64_t last_down; static int last_b, last_x, last_y, clicks;

static void key(int c)
{
    ev_t e = { .type = EV_KEY };
    switch (c) {
    case 27: e.key = K_ESC; break;
    case '\n': case '\r': e.key = K_ENTER; break;
    case '\t': e.key = K_TAB; break;
    case '\b': case 127: e.key = K_BACKSPACE; break;
    case KEY_DEL: e.key = K_DELETE; break;
    case KEY_UP: e.key = K_UP; break;
    case KEY_DOWN: e.key = K_DOWN; break;
    case KEY_LEFT: e.key = K_LEFT; break;
    case KEY_RIGHT: e.key = K_RIGHT; break;
    case KEY_PGUP: e.key = K_PGUP; break;
    case KEY_PGDN: e.key = K_PGDN; break;
    case KEY_HOME: e.key = K_HOME; break;
    case KEY_END: e.key = K_END; break;
    case KEY_F1: e.key = K_F1; break;
    case KEY_BTAB: e.key = K_TAB; e.mod = M_SHIFT; break;
    case KEY_CTAB: e.key = K_TAB; e.mod = M_CTRL; break;
    case KEY_CRET: e.key = K_ENTER; e.mod = M_CTRL; break;
    default:
        if (c >= 1 && c <= 26) { e.key = 'a' + c - 1; e.mod = M_CTRL; }
        else if (c >= KEY_CSA && c < KEY_CSA + 26) { e.key = 'a' + c - KEY_CSA; e.mod = M_CTRL | M_SHIFT; }
        else if (c >= KEY_C1 && c < KEY_C1 + 9) { e.key = '1' + c - KEY_C1; e.mod = M_CTRL; }
        else if (c >= 32 && c < 0x100) {             /* a character: its key, then its text (as SDL) */
            e.key = c >= 'A' && c <= 'Z' ? c + 32 : c < 128 ? c : 0;
            if (c >= 'A' && c <= 'Z') e.mod = M_SHIFT;
            if (e.key) push(&e);
            ev_t t = { .type = EV_TEXT };
            if (c < 128) t.text[0] = (char)c;
            else { t.text[0] = (char)(0xc0 | c >> 6); t.text[1] = (char)(0x80 | (c & 63)); }
            push(&t);
            return;
        } else return;
    }
    push(&e);
}

static void pointer(const atlas_event_t *a)
{
    ev_t e = { .x = a->x, .y = a->y };
    if (a->wheel) { e.type = EV_WHEEL; e.wheel = -a->wheel * 120; push(&e); }   /* (atlas: + towards the user) */
    static const int mb[3] = { MB_LEFT, MB_MIDDLE, MB_RIGHT };
    int changed = (a->buttons ^ buttons) & 7;
    for (int i = 0; i < 3; i++) {
        if (!(changed & mb[i])) continue;
        e.button = i + 1;
        if (a->buttons & mb[i]) {
            uint64_t now = (uint64_t)sys_clock();
            int near = a->x - last_x < 5 && last_x - a->x < 5 && a->y - last_y < 5 && last_y - a->y < 5;
            clicks = last_b == e.button && near && now - last_down < 400000000ull ? clicks % 3 + 1 : 1;
            last_down = now; last_b = e.button; last_x = a->x; last_y = a->y;
            e.type = EV_DOWN; e.clicks = clicks;
        } else { e.type = EV_UP; e.clicks = clicks; }
        push(&e);
    }
    buttons = a->buttons;
    if (!changed && !a->wheel) { e.type = EV_MOVE; e.button = buttons & MB_LEFT ? 1 : 0; push(&e); }
}

static void reader(void *arg)
{
    (void)arg;
    static atlas_event_t ev[64];
    for (;;) {
        msg_t m = { .w = { ATLAS_WIN_EVENT, (uint64_t)win }, .rbuf = ev, .rlen = sizeof ev };
        long n = call(&m);
        if (n < 0) { ev_t e = { .type = EV_QUIT }; push(&e); return; }   /* (closed) */
        for (long i = 0; i < n; i++) {
            const atlas_event_t *a = &ev[i];
            if (a->type == ATW_SIZE) {
                ww = a->x; wh = a->y; zoom = a->code ? a->code : 100;
                ev_t e = { .type = EV_RESIZE }; push(&e);
            } else if (a->type == ATW_KEY) key(a->code);
            else if (a->type == ATW_POINTER) pointer(a);
            else if (a->type == ATW_FOCUS && !a->code) { ev_t e = { .type = EV_LEAVE }; push(&e); }
            else if (a->type == ATW_CLOSE) { win = 0; ev_t e = { .type = EV_QUIT }; push(&e); return; }
        }
    }
}

/* ---- the window */
static void closed(void) { if (win > 0) { msg_t m = { .w = { ATLAS_WIN_CLOSE, (uint64_t)win } }; call(&m); win = 0; } }

int plat_open(const char *title, int w, int h)
{
    (void)w; (void)h;                                /* (atlas places and sizes it) */
    msg_t m = { .w = { ATLAS_WIN_OPEN }, .sbuf = title, .slen = strlen(title) };
    win = call(&m);
    if (win < 0) { printf("scriptures: no window on the desktop (%ld): start it from the desktop\n", win); return -1; }
    libc_atexit = closed;
    thread_start(reader, 0, 16384);
    for (int i = 0; i < 500 && !ww; i++) sys_sleep(10000000);   /* its size comes first */
    return ww ? 0 : -1;
}

void plat_size(int *w, int *h) { *w = ww > 0 ? ww : 1; *h = wh > 0 ? wh : 1; }

uint32_t *plat_begin(int *pitch)
{
    int w = ww, h = wh;
    if (w < 1 || h < 1) return 0;
    if (!px || w != bw || h != bh) {                 /* a new buffer, the window's size */
        size_t size = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;
        uint64_t id;
        uint32_t *p;
        do sys_random(&id, sizeof id); while (sys_shm_create(id &= ~(1ULL << 63), size) == -EEXIST);
        if ((long)(p = sys_shm_map(id, 0, PROT_READ | PROT_WRITE)) <= 0) { sys_shm_close(id); return 0; }
        msg_t m = { .w = { ATLAS_WIN_BUFFER, (uint64_t)win, id, (uint64_t)w | (uint64_t)h << 16 } };
        long r = call(&m);
        sys_shm_close(id);                           /* (atlas holds it now) */
        if (r < 0) { sys_munmap(p, size, 0); return 0; }
        if (px) sys_munmap(px, (((size_t)bw * bh * 4) + 4095) & ~(size_t)4095, 0);
        px = p; bw = w; bh = h;
    }
    *pitch = bw;
    return px;
}

void debug_frame(void);
void sieos_log(const char *s) { sys_debug(s, strlen(s)); }
void plat_end(void)                                  /* -> once it is on the screen */
{
    atlas_rect_t r = { 0, 0, bw, bh };
    msg_t m = { .w = { ATLAS_WIN_DAMAGE, (uint64_t)win }, .sbuf = &r, .slen = sizeof r };
    call(&m);
    debug_frame();                                   /* (debug_sieos.c: for the test) */
}

void plat_title(const char *t) { msg_t m = { .w = { ATLAS_WIN_TITLE, (uint64_t)win }, .sbuf = t, .slen = strlen(t) }; call(&m); }
void plat_cursor(int kind) { (void)kind; }          /* (atlas has one pointer) */
void plat_resize(int w, int h) { (void)w; (void)h; } /* (the user sizes windows) */
uint32_t plat_ms(void) { return (uint32_t)(sys_clock() / 1000000); }
int plat_scale(void) { return zoom; }

/* ---- the clipboard: atlas's (a terminal's selection, Ctrl+V) */
void plat_clip_set(const char *s)
{
    msg_t m = { .w = { ATLAS_WIN_CLIP_SET }, .sbuf = s, .slen = strlen(s) };
    call(&m);
}
char *plat_clip_get(void)
{
    char *b = malloc(16385);
    msg_t m = { .w = { ATLAS_WIN_CLIP_GET }, .rbuf = b, .rlen = 16384 };
    long n = b ? call(&m) : -1;
    if (n <= 0) { free(b); return 0; }
    b[n < 16384 ? n : 16384] = 0;
    return b;
}

/* A web address: in the web browser, if there is one. */
void plat_open_url(const char *url)
{
    mk_ident_t me;
    char a[256];
    snprintf(a, sizeof a, "web %s", url);
    if (sys_ident(0, &me) < 0 || spawn_file("/bin/web", a, me.uid, me.gid, 0) < 0) printf("scriptures: %s\n", url);
}

/* ---- the start */
int scriptures_main(int argc, char **argv);
static int argc_; static char **argv_;
static void app(void *arg) { (void)arg; exit(scriptures_main(argc_, argv_)); }

int main(int argc, char **argv)
{
    mk_ident_t me;
    acct_user_t u;
    char d[96];
    if (sys_ident(0, &me) < 0 || acct_user(0, me.uid, &u)) { printf("scriptures: who are you?\n"); return 1; }
    snprintf(d, sizeof d, "%s/.scriptures", u.home);
    setenv("HOME", u.home, 1);
    setenv("SCRIPTURES_DATA", d, 0);
    argc_ = argc; argv_ = argv;
    if (thread_start(app, 0, 4 << 20) < 0) return 1;
    for (;;) sys_sleep(3600000000000ull);            /* (the program ends with the app's thread) */
}

/* ---- Linux only (linux_import.h): the old Python version's library, Firefox's session */
int import_python_library(void (*progress)(const char *msg, void *ctx), void *ctx) { (void)progress; (void)ctx; return 0; }
char *firefox_cookie(char **profile) { if (profile) *profile = 0; return 0; }
const char *default_browser(int *is_firefox) { if (is_firefox) *is_firefox = 0; return "Web"; }
