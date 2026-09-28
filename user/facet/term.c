/*
 * term.c - Terminal emulator window on a pseudo-terminal.  It runs the sia
 * assistant (which falls back to /bin/sh when no model is available), or
 * the plain shell for "Shell Terminal".
 *
 * Understands printable text, \n \r \b \t \f, and ANSI sequences for
 * colours (SGR 0/1/22/30-37/39/40-47/49/90-97), cursor movement
 * (A B C D H f), and erasing (J K).
 */
#include "facet.h"
#include <pty.h>

#define MAX_COLS 160
#define MAX_ROWS 64
#define PAD 4

struct term {
    int master;
    int chan;                            /* desktop control channel of the program */
    pid_t child;
    int cols, rows;
    uint16_t cell[MAX_ROWS][MAX_COLS];   /* char | attr << 8 (attr = fg | bg << 4) */
    int cx, cy;
    uint8_t attr;
    bool bold;
    int esc, nparam, param[8];
    bool exited;
    bool plain;                          /* plain shell, not the sia assistant */
    int number;
};

static const color_t term_palette[16] = {
    RGB(0x14, 0x19, 0x1D), RGB(0xC9, 0x4B, 0x3F), RGB(0x6C, 0xC0, 0x8A), RGB(0xE3, 0xA2, 0x33),
    RGB(0x4E, 0x8E, 0xC8), RGB(0xB0, 0x6C, 0xC8), RGB(0x4B, 0xB0, 0xB8), RGB(0xD8, 0xD4, 0xC8),
    RGB(0x5E, 0x66, 0x6C), RGB(0xF0, 0x70, 0x60), RGB(0x9E, 0xEC, 0xB0), RGB(0xFF, 0xD4, 0x70),
    RGB(0x86, 0xB8, 0xF0), RGB(0xD8, 0x9C, 0xF0), RGB(0x86, 0xE0, 0xE4), RGB(0xFF, 0xFC, 0xF4),
};
#define DEFAULT_ATTR 0x07

static void clear_row(struct term *t, int y, int from)
{
    for (int x = from; x < MAX_COLS; x++)
        t->cell[y][x] = (DEFAULT_ATTR << 8) | ' ';
}

static void scroll_up(struct term *t)
{
    memmove(t->cell[0], t->cell[1], sizeof(t->cell[0]) * (t->rows - 1));
    clear_row(t, t->rows - 1, 0);
}

static void newline(struct term *t)
{
    t->cx = 0;
    if (++t->cy >= t->rows) {
        scroll_up(t);
        t->cy = t->rows - 1;
    }
}

static void sgr(struct term *t, int code)
{
    static const uint8_t map[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t fg = t->attr & 15, bg = t->attr >> 4;
    if (code == 0) {
        fg = 7;
        bg = 0;
        t->bold = false;
    } else if (code == 1) {
        t->bold = true;
        fg |= 8;
    } else if (code == 22) {
        t->bold = false;
        fg &= 7;
    } else if (code >= 30 && code <= 37) {
        fg = map[code - 30] | (t->bold ? 8 : 0);
    } else if (code == 39) {
        fg = 7;
    } else if (code >= 90 && code <= 97) {
        fg = map[code - 90] | 8;
    } else if (code >= 40 && code <= 47) {
        bg = map[code - 40];
    } else if (code == 49) {
        bg = 0;
    }
    t->attr = fg | (bg << 4);
}

static void csi(struct term *t, char c)
{
    int p0 = t->param[0], n = p0 ? p0 : 1;
    switch (c) {
    case 'm':
        for (int i = 0; i <= t->nparam; i++)
            sgr(t, t->param[i]);
        break;
    case 'A': t->cy = MAX(0, t->cy - n); break;
    case 'B': t->cy = MIN(t->rows - 1, t->cy + n); break;
    case 'C': t->cx = MIN(t->cols - 1, t->cx + n); break;
    case 'D': t->cx = MAX(0, t->cx - n); break;
    case 'H':
    case 'f':
        t->cy = MIN(t->rows - 1, MAX(0, (p0 ? p0 : 1) - 1));
        t->cx = MIN(t->cols - 1, MAX(0, (t->nparam >= 1 && t->param[1] ? t->param[1] : 1) - 1));
        break;
    case 'J':
        if (p0 == 2) {
            for (int y = 0; y < t->rows; y++)
                clear_row(t, y, 0);
        } else {
            clear_row(t, t->cy, t->cx);
            for (int y = t->cy + 1; y < t->rows; y++)
                clear_row(t, y, 0);
        }
        break;
    case 'K':
        clear_row(t, t->cy, t->cx);
        break;
    }
}

static void term_putc(struct term *t, char c)
{
    if (t->esc == 1) {
        if (c == '[') {
            t->esc = 2;
            t->nparam = 0;
            memset(t->param, 0, sizeof(t->param));
        } else {
            t->esc = 0;
        }
        return;
    }
    if (t->esc == 2) {
        if (c >= '0' && c <= '9') {
            t->param[t->nparam] = t->param[t->nparam] * 10 + (c - '0');
        } else if (c == ';') {
            if (t->nparam < 7)
                t->nparam++;
        } else if (c != '?') {
            csi(t, c);
            t->esc = 0;
        }
        return;
    }
    switch (c) {
    case 27:
        t->esc = 1;
        return;
    case '\n':
        newline(t);
        return;
    case '\r':
        t->cx = 0;
        return;
    case '\b':
        if (t->cx > 0)
            t->cx--;
        return;
    case '\t':
        t->cx = MIN(t->cols - 1, (t->cx + 8) & ~7);
        return;
    case '\f':
        for (int y = 0; y < t->rows; y++)
            clear_row(t, y, 0);
        t->cx = t->cy = 0;
        return;
    case 7:
        return;
    }
    if ((unsigned char)c < 32)
        return;
    if (t->cx >= t->cols)
        newline(t);
    t->cell[t->cy][t->cx++] = (t->attr << 8) | (unsigned char)c;
}

#define TERM_BG RGB(0x16, 0x18, 0x1B)
#define TERM_FG RGB(0xE4, 0xE0, 0xD8)

/* Text colours for the dark (Strata) background; default fg (7) is warm white. */
static const color_t dark_fg[16] = {
    RGB(0x6D, 0x73, 0x7A), RGB(0xE0, 0x7A, 0x6A), RGB(0x8C, 0xCF, 0x9A), RGB(0xD9, 0xA1, 0x5F),
    RGB(0x8F, 0xB4, 0xDC), RGB(0xC4, 0x9C, 0xD8), RGB(0x7C, 0xC8, 0xCC), TERM_FG,
    RGB(0x8D, 0x88, 0x80), RGB(0xF2, 0x8C, 0x7C), RGB(0xA6, 0xE8, 0xB4), RGB(0xF0, 0xC0, 0x80),
    RGB(0xA8, 0xCC, 0xF4), RGB(0xDC, 0xB4, 0xF0), RGB(0x9C, 0xE4, 0xE8), RGB(0xFF, 0xFC, 0xF4),
};

static color_t fg_color(uint8_t attr)
{
    return dark_fg[attr & 15];
}

static color_t bg_color(uint8_t attr)
{
    int b = attr >> 4;
    return b == 0 ? TERM_BG : term_palette[b];
}

static void term_draw(struct window *w, struct surface *s, struct rect c)
{
    struct term *t = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, TERM_BG);
    for (int y = 0; y < t->rows; y++) {
        int py = c.y + PAD + y * FONT_H;
        if (py + FONT_H < s->clip.y || py > s->clip.y + s->clip.h)
            continue;
        for (int x = 0; x < t->cols; x++) {
            uint16_t cell = t->cell[y][x];
            uint8_t attr = cell >> 8;
            unsigned char ch = cell & 255;
            int px = c.x + PAD + x * FONT_W;
            bool cursor = x == t->cx && y == t->cy && !t->exited && w == wm_focused();
            color_t fg = fg_color(attr), bgc = bg_color(attr);
            if (cursor) {
                fg = TERM_BG;
                bgc = TERM_FG;
            }
            if (bgc != TERM_BG || cursor)
                gfx_fill(s, px, py, FONT_W, FONT_H, bgc);
            if (ch != ' ')
                gfx_char(s, px, py, ch, fg, bgc, false);
        }
    }
    if (t->exited)
        gfx_text(s, c.x + PAD, c.y + c.h - FONT_H - 2, "[process exited - close this window]", RGB(0xB0, 0, 0));
}

static void term_send(struct term *t, const char *s, size_t n)
{
    if (!t->exited)
        write(t->master, s, n);
}

static void term_key(struct window *w, const struct input_event *ev)
{
    struct term *t = w->app;
    if (ev->type != EV_KEY || !ev->value)
        return;
    switch (ev->code) {
    case KEY_UP:    term_send(t, "\033[A", 3); return;
    case KEY_DOWN:  term_send(t, "\033[B", 3); return;
    case KEY_RIGHT: term_send(t, "\033[C", 3); return;
    case KEY_LEFT:  term_send(t, "\033[D", 3); return;
    }
    if (ev->ascii) {
        char ch = ev->ascii == '\n' ? '\r' : (char)ev->ascii;
        term_send(t, &ch, 1);
    }
}

static void term_readable(struct window *w)
{
    struct term *t = w->app;
    char buf[2048];
    long n = read(t->master, buf, sizeof(buf));
    if (n <= 0) {
        t->exited = true;                /* all slave descriptors closed */
        snprintf(w->title, sizeof(w->title), "Terminal %d (exited)", t->number);
        wm_invalidate(w);
        return;
    }
    for (long i = 0; i < n; i++)
        term_putc(t, buf[i]);
    struct rect c = wm_content(w);
    wm_invalidate_rect(c);
}

static int term_pollfd(struct window *w)
{
    struct term *t = w->app;
    return t->exited ? -1 : t->master;
}

static void term_resized(struct window *w)
{
    struct term *t = w->app;
    struct rect c = wm_content(w);
    int cols = MIN(MAX_COLS, MAX(20, (c.w - 2 * PAD) / FONT_W));
    int rows = MIN(MAX_ROWS, MAX(4, (c.h - 2 * PAD) / FONT_H));
    if (cols == t->cols && rows == t->rows)
        return;
    while (t->cy >= rows) {
        scroll_up(t);
        t->cy--;
    }
    t->cols = cols;
    t->rows = rows;
    t->cx = MIN(t->cx, cols - 1);
    struct winsize ws = { rows, cols, 0, 0 };
    ioctl(t->master, TIOCSWINSZ, &ws);
}

static void term_destroy(struct window *w)
{
    struct term *t = w->app;
    close(t->master);                    /* hangs up the shell's session */
    desktop_channel_close(t->chan);
    if (t->child > 0 && !t->exited)
        kill(t->child, SIGHUP);
    free(t);
}

struct window *term_open(bool plain)
{
    static int count;
    struct term *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->plain = plain;
    t->cols = 80;
    t->rows = 25;
    t->attr = DEFAULT_ATTR;
    t->number = ++count;
    for (int y = 0; y < MAX_ROWS; y++)
        clear_row(t, y, 0);

    int fds[2];
    if (openpty(&fds[0], &fds[1], NULL, NULL, NULL) < 0) {
        free(t);
        return NULL;
    }
    struct winsize ws = { t->rows, t->cols, 0, 0 };
    ioctl(fds[0], TIOCSWINSZ, &ws);
    t->chan = desktop_channel_new();
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        setsid();
        dup2(fds[1], 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        if (fds[1] > 2)
            close(fds[1]);
        ioctl(0, TIOCSCTTY, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        setenv("TERM", "sieos", 1);
        desktop_channel_child(t->chan);         /* fds 3/4; closes everything else of Facet's */
        const char *home = getenv("HOME");
        if (home)
            chdir(home);
        struct stat st;
        if (!plain && stat("/bin/sia", &st) == 0) {
            char *argv[] = { "sia", NULL };
            execv("/bin/sia", argv);
        }
        char *argv[] = { "sh", NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    desktop_channel_parent(t->chan);
    close(fds[1]);
    t->master = fds[0];
    t->child = pid;

    char title[32];
    snprintf(title, sizeof(title), "%s %d - %s", plain ? "Shell" : "Terminal", t->number, desktop_user);
    struct window *w = wm_create(title, -1, -1, t->cols * FONT_W + 2 * PAD, t->rows * FONT_H + 2 * PAD);
    if (!w) {
        close(t->master);
        desktop_channel_close(t->chan);
        free(t);
        return NULL;
    }
    w->app = t;
    w->draw = term_draw;
    w->key = term_key;
    w->readable = term_readable;
    w->pollfd = term_pollfd;
    w->resized = term_resized;
    w->destroy = term_destroy;
    w->min_w = 30 * FONT_W;
    w->min_h = 8 * FONT_H;
    return w;
}

bool term_is_assistant(struct window *w)
{
    struct term *t = w->app;
    return w->draw == term_draw && t && !t->plain && !t->exited;
}

/* Type a line into the terminal, as if the user had entered it. */
void term_type_line(struct window *w, const char *text)
{
    struct term *t = w->app;
    if (w->draw != term_draw || !t || t->exited)
        return;
    write(t->master, text, strlen(text));
    write(t->master, "\n", 1);
}

void app_terminal(void)
{
    term_open(false);
}

void app_shell_terminal(void)
{
    term_open(true);
}
