/*
 * setup.c - The first start, on the screen: there is no account yet, and
 * the desktop's welcome screen creates them, in three steps:
 *
 *   1  The administrator    root's password, twice
 *   2  Your account         login name, full name, password twice
 *   3  Ready                the list; "Add another user" (back to 2),
 *                           "Create accounts"
 *
 * Nothing is created until "Create accounts": then everything goes to the
 * accounts server in one request (AUTH_SETUP), which checks it all again
 * and decides (only the screen that said "I do the first start" may, and
 * only while root has no account). The new user is then logged in at once:
 * they have just typed their password twice. Passwords live in memory only
 * until then, and are wiped.
 *
 * Keys: Tab / Shift+Tab move between fields, Enter goes to the next field
 * (or the next step), Esc goes back a step. The mouse works too.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"                  /* wipe */

static int step;                        /* 0, 1, 2 (see above) */
static int fld;                         /* the field with the keys */
static char rootpw[2][128];             /* step 1: password, again */
static struct { char name[32], full[64], pw[128], again[128]; } u[AUTH_SETUP_MAX];
static int nu, cur;                     /* accounts ready; the one being typed */
static char msg[96];                    /* what is wrong, in red */

/* The fields of a step: label, buffer, size, hidden. */
typedef struct { const char *label; char *buf; size_t size; int hidden; } field_t;
static int fields(field_t *f)
{
    if (step == 0) {
        f[0] = (field_t){ "Password", rootpw[0], sizeof rootpw[0], 1 };
        f[1] = (field_t){ "Again", rootpw[1], sizeof rootpw[1], 1 };
        return 2;
    }
    if (step == 1) {
        f[0] = (field_t){ "Login name", u[cur].name, sizeof u[cur].name, 0 };
        f[1] = (field_t){ "Full name", u[cur].full, sizeof u[cur].full, 0 };
        f[2] = (field_t){ "Password", u[cur].pw, sizeof u[cur].pw, 1 };
        f[3] = (field_t){ "Again", u[cur].again, sizeof u[cur].again, 1 };
        return 4;
    }
    return 0;
}

/* The rules (the same as the accounts server's; docs/accounts.md). */
static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 31 || !((*s >= 'a' && *s <= 'z') || *s == '_') || !strcmp(s, "root")) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}
static const char *pw_problem(const char *name, const char *pw, const char *again)
{
    if (strlen(pw) < AUTH_PWMIN) return "The password needs at least 8 characters.";
    if (!strcmp(pw, name)) return "The password can't be the name itself.";
    if (strcmp(pw, again)) return "The two passwords differ.";
    return 0;
}

/* Can this step be left forwards? If not, say why. */
static int step_ok(void)
{
    const char *p = 0;
    if (step == 0) p = pw_problem("root", rootpw[0], rootpw[1]);
    if (step == 1) {
        if (!name_ok(u[cur].name))
            p = !strcmp(u[cur].name, "root") ? "\"root\" exists already: choose another name."
              : "Login name: a-z first, then a-z, 0-9, _ or -.";
        for (int i = 0; !p && i < nu; i++)
            if (i != cur && !strcmp(u[i].name, u[cur].name)) p = "That login name is already in the list.";
        if (!p && strchr(u[cur].full, '\t')) p = "The full name can't contain a tab.";
        if (!p) p = pw_problem(u[cur].name, u[cur].pw, u[cur].again);
    }
    strlcpy(msg, p ? p : "", sizeof msg);
    if (p) say("atlas: setup: not yet: %s\n", p);
    return !p;
}

static void forget(void)
{
    wipe(rootpw, sizeof rootpw);
    wipe(u, sizeof u);
    nu = cur = 0;
}

/* "Create accounts": one request to auth, then log the first user in. */
static void create(void)
{
    char req[1 + 128 + AUTH_SETUP_MAX * (32 + 64 + 128)], *p = req;
    p += strlcpy(p, rootpw[0], 128) + 1;
    for (int i = 0; i < nu; i++) {
        p += strlcpy(p, u[i].name, 32) + 1;
        p += strlcpy(p, u[i].full, 64) + 1;
        p += strlcpy(p, u[i].pw, 128) + 1;
    }
    strlcpy(msg, "Creating the accounts...", sizeof msg);
    redraw();
    msg_t d = { .w = { AUTH_DISPLAY, 1 } };              /* (again: auth may have restarted) */
    atlas_auth(&d);
    msg_t m = { .w = { AUTH_SETUP }, .sbuf = req, .slen = p - req };
    long r = atlas_auth(&m);
    wipe(req, sizeof req);
    if (r < 0) {
        strlcpy(msg, r == -EPERM ? "The accounts exist already, or the setup is done elsewhere."
                                 : "That did not work: check the names and passwords.", sizeof msg);
        say("atlas: setup: refused (%ld)\n", r);
        if (r == -EPERM) { forget(); greeter_check(); }
        return;
    }
    say("atlas: setup: accounts created (root and %d user(s))\n", nu);
    msg[0] = 0;
    char name[32], pw[128];
    strlcpy(name, u[0].name, sizeof name);
    strlcpy(pw, u[0].pw, sizeof pw);
    forget();
    greeter_login(name, pw);                            /* (it wipes pw) */
    wipe(pw, sizeof pw);
}

static void next(void)
{
    if (!step_ok()) return;
    if (step == 0) { step = 1; cur = nu; fld = 0; }
    else if (step == 1) { if (cur == nu) nu++; step = 2; fld = 0; }
    say("atlas: setup: step %d\n", step + 1);
}

static void back(void)
{
    msg[0] = 0;
    if (step == 1) {                                     /* drop the account being typed */
        if (cur == nu) wipe(&u[cur], sizeof u[cur]);
        step = nu ? 2 : 0;
    } else if (step == 2) { step = 1; cur = nu - 1; }    /* edit the last one again */
    fld = 0;
    say("atlas: setup: step %d\n", step + 1);
}

/* ---- What desktop's greeter calls. */
enum { SB_NEXT = 1, SB_BACK, SB_ADD, SB_CREATE };

void setup_draw(void)
{
    int x0 = SW / 2 - 420, y0 = SH / 2 - 330, x1 = SW / 2 + 420, y1 = SH / 2 + 330;
    static const char *titles[3] = { "Step 1 of 3 - the administrator", "Step 2 of 3 - your account",
                                     "Step 3 of 3 - ready" };
    dl_rect(x0, y0, x1, y1, 24, C_CARD);
    dl_frame(x0, y0, x1, y1, 24, 2, C_ISLB);
    label(x0 + 60, y0 + 46, "Welcome to SIEOS", 5, C_ACC);
    label(x0 + 60, y0 + 126, step == 1 && cur > 0 ? "Step 2 of 3 - another account" : titles[step], 2, C_TXT);
    for (int i = 0; i < 3; i++)                           /* where we are: three dots */
        dl_rect(x1 - 150 + i * 34, y0 + 132, x1 - 130 + i * 34, y0 + 152, 10, i <= step ? C_ACC : C_ISLB);
    if (step == 0) label(x0 + 60, y0 + 170, "root can change the whole system: choose its password.", 2, C_DIM);
    if (step == 1) label(x0 + 60, y0 + 170, "The account you will use every day.", 2, C_DIM);

    field_t f[4];
    int n = fields(f);
    for (int i = 0; i < n; i++) {
        int fy = y0 + 220 + i * 76;
        label(x0 + 60, fy + 12, f[i].label, 2, C_DIM);
        dl_rect(x0 + 270, fy, x1 - 60, fy + 50, 10, C_BG);
        dl_frame(x0 + 270, fy, x1 - 60, fy + 50, 10, fld == i ? 2 : 1, fld == i ? C_ACC : C_ISLB);
        char dots[64];
        int len = strlen(f[i].buf);
        if (len > (int)sizeof dots - 1) len = sizeof dots - 1;
        memset(dots, '*', len);
        dl_text(x0 + 290, fy + 14, f[i].hidden ? dots : f[i].buf, len, S(2), C_TXT, 0, 0, x1 - 70, SH);
        if (fld == i) { int cw = tw(len, 2); dl_rect(x0 + 292 + cw, fy + 12, x0 + 304 + cw, fy + 38, 0, C_ACC); }
        hitbox(x0 + 270, fy, x1 - 60, fy + 50, H_FIELD, i);
    }
    if (step == 2) {                                      /* the list */
        label(x0 + 60, y0 + 190, "These accounts will be created:", 2, C_DIM);
        label(x0 + 90, y0 + 240, "root - the administrator", 2, C_TXT);
        for (int i = 0; i < nu; i++) {
            char line[110];
            strlcpy(line, u[i].name, sizeof line);
            if (u[i].full[0]) { strlcpy(line + strlen(line), " - ", sizeof line - strlen(line));
                                strlcpy(line + strlen(line), u[i].full, sizeof line - strlen(line)); }
            label(x0 + 90, y0 + 290 + i * 50, line, 2, C_TXT);
        }
        label(x0 + 60, y0 + 520, "You will be logged in as the first one.", 2, C_DIM);
    }
    if (msg[0]) label(x0 + 60, y1 - 130, msg, 2, C_ERR);

    int bx = x1 - 60, by = y1 - 80;                       /* buttons, right to left */
    const char *main_s = step == 2 ? "Create accounts" : "Next";
    bx -= tw(strlen(main_s), 2) + 28;
    pill(bx, by, main_s, 1, H_SETUP, step == 2 ? SB_CREATE : SB_NEXT);
    if (step == 2 && nu < AUTH_SETUP_MAX) {
        bx -= tw(16, 2) + 28 + 20;
        pill(bx, by, "Add another user", 0, H_SETUP, SB_ADD);
    }
    if (step) pill(x0 + 60, by, "Back", 0, H_SETUP, SB_BACK);
}

void setup_click(int kind, int arg)
{
    if (kind == H_FIELD) fld = arg;
    else if (arg == SB_NEXT) next();
    else if (arg == SB_BACK) back();
    else if (arg == SB_ADD && nu < AUTH_SETUP_MAX) { step = 1; cur = nu; fld = 0; msg[0] = 0; say("atlas: setup: another user\n"); }
    else if (arg == SB_CREATE) create();
}

void setup_key(int c)
{
    field_t f[4];
    int n = fields(f);
    if (c == 27) { if (step) back(); }
    else if (c == '\t') { if (n) fld = (fld + 1) % n; }
    else if (c == KEY_BTAB) { if (n) fld = (fld + n - 1) % n; }
    else if (c == '\n') {
        if (step == 2) create();
        else if (fld < n - 1) fld++;
        else next();
    } else if (n && c == '\b') { size_t l = strlen(f[fld].buf); if (l) f[fld].buf[l - 1] = 0; }
    else if (n && c >= 32 && c < 127) {
        size_t l = strlen(f[fld].buf);
        if (l + 1 < f[fld].size) { f[fld].buf[l] = c; f[fld].buf[l + 1] = 0; }
    }
}
