/*
 * pins.c - The Pins window (docs/gpio.md): the Raspberry Pi's GPIO pins as
 * the tracks of a sequencer, the way music programs lay out their tracks.
 *
 *   [Play] [Loop]  Step 100 ms [-][+]  Steps 16 [-][+]  [+ Track]    Raspberry Pi 4: playing 5
 *               1       5       9       13
 *   BCM 17 <> [OUT] IN  ■ clk inv clr x   │■ □ □ □│■ □ □ □│■ □ □ □│■ □ □ □│
 *   BCM 18 <> [OUT] IN  □ clk inv clr x   │□ □ ■ □│□ □ ■ □│□ □ ■ □│□ □ ■ □│
 *   BCM 27 <>  OUT [IN] up ■ x            │▀ ▀ ▀ ▀│▀ ▀ ▄ ▄│▄ ▀ ▀ ▀│▀ ▀ ▀ ▀│ rec
 *
 * A track is a pin, an output or an input (OUT / IN; an input's pull:
 * up, dn, none, with a click). An output's row is its pattern, which the
 * sequencer plays: a click on a step turns it on or off; clk, inv, clr
 * fill it (a clock, the opposite, nothing). An input's row is its
 * recording: its level at each step of the sequence as it played, sampled
 * by the GPIO server at the end of each step (high: a tall block, low: a
 * low one), kept after it stops until the next play. The square (■) is the
 * pin's level now; a click on an output's sets the pin at once. < > choose
 * another pin.
 *
 * Play sends the outputs' patterns to the GPIO server, which plays them
 * with steady timing by itself (a step every 1 ms to 1 s, 8 to 64 steps,
 * once or looping); a change while it plays is sent again at once. The
 * tracks are kept in ~/.pins. On a machine without GPIO the server
 * simulates the pins, so all of this works anywhere.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "gpio.h"

#define TOP 48                           /* the insides start this far below the y0 we are given */
#define NTRK 8
#define MAXS 64
#define BAR 58                           /* the transport bar */
#define RULER 30
#define ROWH 50
#define HEADW 620                        /* a track's header */

typedef struct { int pin, mode, pull; uint8_t on[MAXS]; } track_t;
static track_t trk[NTRK];
static int ntrk, nsteps = 16, step_ms = 100, loop = 1, playing, at = -1, scroll, loaded;
static gpio_pin_t st[GPIO_PINS];
static char board[64];
static int real, nrec;                   /* the recording: the levels at each step as played (rec), steps in it */
static uint32_t rec[MAXS];
static long port, tick_tid;
static volatile int live;
static const int step_choices[] = { 1, 5, 10, 20, 50, 100, 250, 500, 1000 };
#define NSC (int)(sizeof step_choices / sizeof step_choices[0])

/* ---- Keeping the tracks: ~/.pins, "steps=16 step=100 loop=1" then a line a
 * track: "pin mode pull 1000100010001000". */
static void path(char *p, size_t cap) { strlcpy(p + strlcpy(p, atlas_home(), cap), "/.pins", 7); }
static void save(void)
{
    if (!atlas_home()[0]) return;
    static char b[NTRK * (MAXS + 24) + 64];
    char *o = b;
    o += strlcpy(o, "steps=", 8); o += num(o, nsteps);
    o += strlcpy(o, " step=", 8); o += num(o, step_ms);
    o += strlcpy(o, " loop=", 8); o += num(o, loop); *o++ = '\n';
    for (int t = 0; t < ntrk; t++) {
        o += num(o, trk[t].pin); *o++ = ' '; o += num(o, trk[t].mode); *o++ = ' '; o += num(o, trk[t].pull); *o++ = ' ';
        for (int s = 0; s < MAXS; s++) *o++ = (char)('0' + trk[t].on[s]);
        *o++ = '\n';
    }
    char p[96];
    path(p, sizeof p);
    long f = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f >= 0) { fs_write(f, 0, b, (size_t)(o - b)); fs_close(f); }
}
static int rd(const char **p) { int v = 0; while (**p == ' ') (*p)++; while (**p >= '0' && **p <= '9') v = v * 10 + *(*p)++ - '0'; return v; }
static void load(void)
{
    char p[96];
    size_t n = 0;
    path(p, sizeof p);
    char *f = atlas_home()[0] ? file_get(p, &n) : 0;
    if (!f) {                                            /* the first time: a little rhythm on three pins, a button on a fourth */
        static const struct { int pin, mode, pull; const char *on; } demo[] = {
            { 17, GPIO_OUT, 0, "1000100010001000" }, { 18, GPIO_OUT, 0, "0010001000100010" },
            { 22, GPIO_OUT, 0, "1111000011110000" }, { 27, GPIO_IN, GPIO_PULL_UP, "" } };
        ntrk = 4;
        for (int t = 0; t < 4; t++) {
            memset(&trk[t], 0, sizeof trk[t]);
            trk[t].pin = demo[t].pin; trk[t].mode = demo[t].mode; trk[t].pull = demo[t].pull;
            for (int s = 0; demo[t].on[s]; s++) trk[t].on[s] = demo[t].on[s] == '1';
        }
        return;
    }
    const char *q = f;
    ntrk = 0;
    while (*q && *q != '\n') {
        if (!memcmp(q, "steps=", 6)) { q += 6; nsteps = rd(&q); }
        else if (!memcmp(q, "step=", 5)) { q += 5; step_ms = rd(&q); }
        else if (!memcmp(q, "loop=", 5)) { q += 5; loop = rd(&q); }
        else q++;
    }
    if (nsteps < 8 || nsteps > MAXS) nsteps = 16;
    if (step_ms < 1 || step_ms > 1000) step_ms = 100;
    while (*q == '\n') {
        q++;
        if (!*q || ntrk == NTRK) break;
        track_t *t = &trk[ntrk];
        memset(t, 0, sizeof *t);
        t->pin = rd(&q); t->mode = rd(&q); t->pull = rd(&q);
        while (*q == ' ') q++;
        for (int s = 0; s < MAXS && (*q == '0' || *q == '1'); s++) t->on[s] = *q++ == '1';
        while (*q && *q != '\n') q++;
        if (t->pin >= 0 && t->pin < GPIO_PINS && t->mode <= GPIO_OUT && t->pull <= GPIO_PULL_UP) ntrk++;
    }
    free(f);
}

/* ---- The server. */
static void refresh(void) { if (gpio_state(st) < 0) memset(st, 0, sizeof st); }
static void apply_mode(track_t *t)
{
    int e = gpio_mode(t->pin, t->mode, t->mode == GPIO_IN ? t->pull : GPIO_PULL_NONE);
    if (e < 0) say("atlas: pins: BCM %d refused (%d)\n", t->pin, e);
}
static void send(void)                                   /* the outputs' patterns, to the server */
{
    static uint32_t steps[MAXS];
    uint32_t mask = 0;
    for (int s = 0; s < nsteps; s++) steps[s] = 0;
    for (int t = 0; t < ntrk; t++) {
        if (trk[t].mode != GPIO_OUT || st[trk[t].pin].mode == GPIO_BUSY) continue;
        mask |= 1u << trk[t].pin;
        for (int s = 0; s < nsteps; s++) if (trk[t].on[s]) steps[s] |= 1u << trk[t].pin;
    }
    if (!mask) { gpio_stop(); playing = 0; say("atlas: pins: nothing to play\n"); return; }
    nrec = 0;                                            /* (a new recording) */
    int e = gpio_play(mask, steps, nsteps, (uint32_t)step_ms * 1000, loop);
    playing = e == 0;
    say("atlas: pins: playing %d steps of %d ms on %x (%d)\n", nsteps, step_ms, mask, e);
}

static void ticker(void *arg)                            /* ten times a second while the window is open */
{
    (void)arg;
    for (;;) {
        sys_sleep(100000000);
        if (!live) continue;
        msg_t m = { .w = { ATLAS_PINS } };
        ipc_call(port, &m);
    }
}
void pins_init(long atlas_port) { port = atlas_port; }

void pins_open(void)
{
    if (!loaded) { load(); loaded = 1; }
    int n = gpio_info(board, sizeof board, &real);
    if (n < 0) strlcpy(board, "no GPIO server", sizeof board);
    refresh();
    for (int t = 0; t < ntrk; t++) if (st[trk[t].pin].mode != GPIO_BUSY) apply_mode(&trk[t]);
    refresh();
    playing = gpio_playing(0) >= 0;
    live = 1;
    if (tick_tid <= 0) tick_tid = thread_start(ticker, 0, 8192);
    say("atlas: pins open: %s, %d tracks\n", board, ntrk);
}
void pins_close(void) { live = 0; save(); }
void pins_logout(void) { live = 0; loaded = 0; ntrk = 0; }

/* ATLAS_PINS: the levels and the step now; 1 if the window must be drawn again. */
int pins_tick(void)
{
    if (!live) return 0;
    gpio_pin_t old[GPIO_PINS];
    memcpy(old, st, sizeof st);
    refresh();
    int a = gpio_playing(0), changed = a != at || memcmp(old, st, sizeof st);
    at = a;
    if (playing && a < 0) playing = 0;                   /* (a pattern played once has ended) */
    if (playing || a >= 0) {                             /* the inputs' recording, as it grows */
        uint32_t r[MAXS];
        int n = gpio_record(r, MAXS);
        if (n >= 0 && (n != nrec || memcmp(r, rec, (size_t)n * 4))) {
            int full = n >= nsteps && nrec < nsteps;     /* (a whole pass recorded: said once) */
            memcpy(rec, r, (size_t)n * 4); nrec = n; changed = 1;
            for (int t = 0; full && t < ntrk; t++) {
                if (trk[t].mode != GPIO_IN) continue;
                char b[MAXS + 1];
                for (int s = 0; s < nsteps; s++) b[s] = (char)('0' + (rec[s] >> trk[t].pin & 1));
                b[nsteps] = 0;
                say("atlas: pins: recorded BCM %d %s\n", trk[t].pin, b);
            }
        }
    }
    return changed;
}

/* ---- Drawing. */
static int X0, Y0, X1, Y1;
static void dbg(int arg, int x, int y) { if (atlas_debug) say("atlas: dbg pins %d at %d %d\n", arg, x, y); }
static int button(int x, int y, const char *s, int on, int arg) { int r = pill(x, y, s, on, H_PINS, arg); dbg(arg, (x + r) / 2, y + 21); return r; }

void pins_draw(int x0, int y0, int x1, int y1, int focused)
{
    (void)focused;
    X0 = x0; Y0 = y0 + TOP; X1 = x1; Y1 = y1;
    dl_rect(x0 + 2, Y0, x1 - 2, y1 - 2, 0, C_TERM);
    hitbox(x0, Y0, x1, y1, H_PINS, 0);
    /* the transport */
    int ty = Y0 + 8, x = x0 + 14;
    x = button(x, ty, playing ? "Stop" : "Play", playing, 1) + 10;
    x = button(x, ty, "Loop", loop, 2) + 24;
    char t[64], *p = t + strlcpy(t, "Step ", 8);
    p += num(p, step_ms); strlcpy(p, " ms", 4);
    label(x, ty + 10, t, 2, C_TXT); x += tw(strlen(t), 2) + 10;
    x = button(x, ty, "-", 0, 3) + 6; x = button(x, ty, "+", 0, 4) + 24;
    p = t + strlcpy(t, "Steps ", 8); p[num(p, nsteps)] = 0;
    label(x, ty + 10, t, 2, C_TXT); x += tw(strlen(t), 2) + 10;
    x = button(x, ty, "-", 0, 5) + 6; x = button(x, ty, "+", 0, 6) + 24;
    x = button(x, ty, "+ Track", 0, 7) + 24;
    p = t + strlcpy(t, board, sizeof t);
    if (playing && at >= 0) { p += strlcpy(p, ": step ", 8); p[num(p, at + 1)] = 0; }
    if (x + tw(strlen(t), 2) < x1 - 20) label(x1 - 20 - tw(strlen(t), 2), ty + 10, t, 2, playing ? C_ACC : C_DIM);

    /* the grid: a ruler, then a row a track */
    int gx0 = x0 + 14 + HEADW, gx1 = x1 - 16, ry = Y0 + BAR;
    if (gx1 - gx0 < nsteps * 8) gx0 = x0 + 14 + 60;      /* (narrow: the headers shrink) */
    int cw = (gx1 - gx0) / nsteps;
    if (cw < 6) cw = 6;
    int all = cw >= text_w(2, SS) + 8;                   /* (room: every step numbered, the beats brighter) */
    for (int s = 0; s < nsteps; s += all ? 1 : 4) {
        char nb[8]; nb[num(nb, s + 1)] = 0;
        int beat = s % 4 == 0, on = playing && s == at;
        dl_text(gx0 + s * cw + 4, ry + 8, nb, strlen(nb), SS, on ? C_ACC : beat ? C_TXT : C_LINE, 0, 0, SW, SH);
    }
    int rows = (Y1 - 8 - (ry + RULER)) / ROWH;
    if (scroll > ntrk - rows) scroll = ntrk - rows;
    if (scroll < 0) scroll = 0;
    int gy0 = ry + RULER, head = playing && at >= 0 && at < nsteps;
    if (head)                                            /* the playhead: its column lit, under the cells */
        dl_rect(gx0 + at * cw, ry + 2, gx0 + (at + 1) * cw, gy0 + (ntrk - scroll < rows ? ntrk - scroll : rows) * ROWH, 3, C_SEL);
    for (int r = 0; r < rows && scroll + r < ntrk; r++) {
        int ti = scroll + r, y = gy0 + r * ROWH, base = 100 + ti * 10;
        track_t *k = &trk[ti];
        gpio_pin_t *ps = &st[k->pin];
        int busy = ps->mode == GPIO_BUSY, hx = x0 + 14;
        dl_rect(x0 + 6, y + ROWH - 1, x1 - 6, y + ROWH, 0, C_LINE);
        if (gx0 - hx >= HEADW - 10) {                    /* the header */
            p = t + strlcpy(t, "BCM ", 8); p[num(p, k->pin)] = 0;
            dl_text(hx, y + 14, t, strlen(t), S(2), busy ? C_DIM : C_TXT, hx, y, hx + 90, y + ROWH);
            dl_text(hx + 1, y + 14, t, strlen(t), S(2), busy ? C_DIM : C_TXT, hx, y, hx + 90, y + ROWH);
            int bx = button(hx + 84, y + 4, "<", 0, base + 0) + 2;
            bx = button(bx, y + 4, ">", 0, base + 1) + 8;
            if (busy) bx = button(bx, y + 4, "busy", 0, base + 2) + 8;
            else {                                       /* the mode: OUT or IN (and an input's pull) */
                bx = button(bx, y + 4, "OUT", k->mode == GPIO_OUT, base + 2) + 2;
                bx = button(bx, y + 4, "IN", k->mode == GPIO_IN, base + 8) + 4;
                if (k->mode == GPIO_IN) bx = button(bx, y + 4, k->pull == GPIO_PULL_UP ? "up" : k->pull == GPIO_PULL_DOWN ? "dn" : "none", 0, base + 9) + 4;
                bx += 4;
            }
            /* the level now: a square, lit when high (a click: an output's level flips) */
            int lv = ps->level;
            dl_frame(bx, y + 13, bx + 24, y + 37, 3, 2, lv ? C_ACC : C_LINE);
            if (lv) dl_rect(bx + 5, y + 18, bx + 19, y + 32, 2, C_ACC);
            hitbox(bx - 4, y + 6, bx + 28, y + 44, H_PINS, base + 3);
            dbg(base + 3, bx + 12, y + 25);
            bx += 36;
            if (k->mode == GPIO_OUT) {                   /* the pattern's tools, after a line */
                dl_rect(bx - 6, y + 12, bx - 5, y + ROWH - 12, 0, C_LINE);
                bx = button(bx, y + 4, "clk", 0, base + 4) + 4;
                bx = button(bx, y + 4, "inv", 0, base + 5) + 4;
                bx = button(bx, y + 4, "clr", 0, base + 6) + 4;
            }
            button(k->mode == GPIO_OUT ? bx + 8 : hx + HEADW - 190, y + 4, "x", 0, base + 7);
        } else {
            p = t; p[num(p, k->pin)] = 0;
            label(hx, y + 14, t, 2, C_TXT);
        }
        /* its row: bars of 4 steps; an output's steps, or an input's samples */
        for (int s = 0; s <= nsteps; s += 4) dl_rect(gx0 + s * cw - 1, y + 2, gx0 + s * cw + 1, y + ROWH - 2, 0, C_LINE);
        if (k->mode == GPIO_OUT) {
            for (int s = 0; s < nsteps; s++) {
                int cx = gx0 + s * cw;
                int now = head && s == at;               /* (under the playhead: brighter) */
                dl_rect(cx + 3, y + 8, cx + cw - 3, y + ROWH - 8, 3, k->on[s] ? (now ? C_TXT : C_ACC) : now ? C_LINE : C_BAR);
                hitbox(cx, y + 2, cx + cw, y + ROWH - 2, H_PINS, 1000 + ti * 100 + s);
            }
            if (atlas_debug) for (int s = 0; s < nsteps; s += nsteps / 4) dbg(1000 + ti * 100 + s, gx0 + s * cw + cw / 2, y + ROWH / 2);
        } else {                                         /* the input: its recording, a block a step */
            for (int s = 0; s < nsteps; s++) {
                int cx = gx0 + s * cw;
                if (s >= nrec) { dl_rect(cx + cw / 2 - 1, y + ROWH / 2 - 1, cx + cw / 2 + 1, y + ROWH / 2 + 1, 0, C_LINE); continue; }
                int v = rec[s] >> k->pin & 1;
                if (v) dl_rect(cx + 3, y + 8, cx + cw - 3, y + ROWH - 8, 3, C_DIM);   /* (recorded high: filled, as an output's, dimmer) */
                else dl_rect(cx + 3, y + ROWH - 12, cx + cw - 3, y + ROWH - 8, 2, C_DIM);
            }
            if (playing && gx0 - (x0 + 14) >= HEADW - 10) label(gx0 - 80, y + 14, "rec", 2, C_ERR);
        }
        if (busy) label(gx0 + 10, y + 14, "used by a device", 2, C_ERR);
    }
    if (!ntrk) label(x0 + 24, gy0 + 16, "No track: + Track adds one.", 2, C_DIM);
}

/* ---- Doing. */
static int pin_free(int pin, int self)
{
    if (pin < 0 || pin >= GPIO_PINS || st[pin].mode == GPIO_BUSY) return 0;
    for (int t = 0; t < ntrk; t++) if (t != self && trk[t].pin == pin) return 0;
    return 1;
}
static void changed(void) { if (playing) send(); save(); }

int pins_click(int kind, int arg)
{
    if (kind != H_PINS) return 0;
    if (arg == 1) { if (playing) { gpio_stop(); playing = 0; at = -1; say("atlas: pins: stopped\n"); } else send(); save(); return 1; }
    if (arg == 2) { loop = !loop; changed(); return 1; }
    if (arg == 3 || arg == 4) {
        int k = 0;
        for (int i = 0; i < NSC; i++) if (step_choices[i] == step_ms) k = i;
        k += arg == 4 ? 1 : -1;
        step_ms = step_choices[k < 0 ? 0 : k >= NSC ? NSC - 1 : k];
        changed(); return 1;
    }
    if (arg == 5 || arg == 6) {
        nsteps = arg == 6 ? (nsteps < MAXS ? nsteps * 2 : MAXS) : (nsteps > 8 ? nsteps / 2 : 8);
        changed(); return 1;
    }
    if (arg == 7) {
        if (ntrk == NTRK) return 1;
        int pin = -1;
        for (int p = 2; p < GPIO_PINS && pin < 0; p++) if (pin_free(p, -1)) pin = p;
        if (pin < 0) return 1;
        memset(&trk[ntrk], 0, sizeof trk[ntrk]);
        trk[ntrk].pin = pin; trk[ntrk].mode = GPIO_OUT;
        apply_mode(&trk[ntrk++]);
        refresh();
        say("atlas: pins: track BCM %d added\n", pin);
        changed(); return 1;
    }
    if (arg >= 100 && arg < 100 + NTRK * 10) {
        int ti = (arg - 100) / 10, k = (arg - 100) % 10;
        if (ti >= ntrk) return 1;
        track_t *t = &trk[ti];
        if (k == 0 || k == 1) {                          /* another pin */
            int p = t->pin;
            for (int i = 0; i < GPIO_PINS; i++) {
                p = (p + (k ? 1 : GPIO_PINS - 1)) % GPIO_PINS;
                if (pin_free(p, ti)) { t->pin = p; break; }
            }
            apply_mode(t);
        } else if (k == 2 || k == 8) {                   /* OUT, IN */
            if (st[t->pin].mode == GPIO_BUSY) return 1;
            t->mode = k == 2 ? GPIO_OUT : GPIO_IN;
            if (t->mode == GPIO_IN && t->pull == GPIO_PULL_NONE) t->pull = GPIO_PULL_UP;
            apply_mode(t);
            say("atlas: pins: BCM %d %s\n", t->pin, t->mode == GPIO_OUT ? "out" : "in");
        } else if (k == 9) {                             /* an input's pull: up, down, none */
            t->pull = t->pull == GPIO_PULL_UP ? GPIO_PULL_DOWN : t->pull == GPIO_PULL_DOWN ? GPIO_PULL_NONE : GPIO_PULL_UP;
            apply_mode(t);
        } else if (k == 3) {                             /* the level: an output's flips now */
            if (t->mode == GPIO_OUT) { gpio_write(t->pin, !st[t->pin].level); say("atlas: pins: BCM %d set %d\n", t->pin, !st[t->pin].level); }
        } else if (k == 4) for (int s = 0; s < MAXS; s++) t->on[s] = !(s & 1);
        else if (k == 5) for (int s = 0; s < MAXS; s++) t->on[s] = !t->on[s];
        else if (k == 6) memset(t->on, 0, sizeof t->on);
        else if (k == 7) {
            say("atlas: pins: track BCM %d removed\n", t->pin);
            memmove(&trk[ti], &trk[ti + 1], (size_t)(ntrk - ti - 1) * sizeof trk[0]);
            ntrk--;
        }
        refresh();
        changed();
        return 1;
    }
    if (arg >= 1000 && arg < 1000 + NTRK * 100) {
        int ti = (arg - 1000) / 100, s = (arg - 1000) % 100;
        if (ti >= ntrk || s >= MAXS) return 1;
        trk[ti].on[s] = !trk[ti].on[s];
        say("atlas: pins: BCM %d step %d %s\n", trk[ti].pin, s + 1, trk[ti].on[s] ? "on" : "off");
        changed();
        return 1;
    }
    return 1;
}
void pins_wheel(int w) { scroll -= w; if (scroll < 0) scroll = 0; }
int pins_key(int c)
{
    if (c == ' ') return pins_click(H_PINS, 1);         /* Space: play / stop */
    return 0;
}
