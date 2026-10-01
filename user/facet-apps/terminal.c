/*
 * facet-terminal [-s] [-n number] - Terminal emulator window on a
 * pseudo-terminal.  It runs the sia assistant (which falls back to /bin/sh
 * when no model is available), or with -s the plain shell.  Facet starts it
 * with the desktop control channel as fds 3 and 4, which the assistant
 * inherits; lines the desktop types (FCT_EV_TEXT) go to the program.
 * A Facet application (libfacet).
 *
 * Understands printable text, \n \r \b \t \f, and ANSI sequences for
 * colours (SGR 0/1/22/30-37/39/40-47/49/90-97), cursor movement
 * (A B C D H f), and erasing (J K).
 *
 * Text is DejaVu Sans Mono (TrueType, 13 px): Ctrl and + / - change the
 * size, Ctrl+0 restores it; the window keeps its size and the grid follows.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"
#include <facet/font.h>
#include <pty.h>

#define MAX_COLS 160
#define MAX_ROWS 64
#define PAD 4

struct term {
    int master;
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
    int px;                              /* font size */
    struct fct_face *face;               /* NULL: the bitmap font */
    int cw, chh;                         /* the character cell */
    /* the selection (mouse): from the cell where the button went down to the
     * one under the pointer, row by row; Ctrl+Shift+C copies it */
    bool sel_on, selecting;
    int sax, say, sbx, sby;
};

#define DEFAULT_PX 13

static void term_metrics(struct term *t)
{
    t->face = fct_ui_face_px(FCT_FONT_MONO, t->px);
    if (t->face) {
        t->cw = MAX(4, (int)(fct_face_advance(t->face, 'M') + 0.5f));
        t->chh = fct_face_height(t->face) + 1;
    } else {
        t->cw = FONT_W;
        t->chh = FONT_H;
    }
}

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
    if (t->sel_on || t->selecting) {             /* the selection goes up with its text */
        t->say--, t->sby--;
        if (t->say < 0 || t->sby < 0)
            t->sel_on = t->selecting = false;
    }
}

/* The selection's ends in reading order: (x0, y0) before (x1, y1). */
static void sel_range(const struct term *t, int *x0, int *y0, int *x1, int *y1)
{
    bool fwd = t->say < t->sby || (t->say == t->sby && t->sax <= t->sbx);
    *x0 = fwd ? t->sax : t->sbx, *y0 = fwd ? t->say : t->sby;
    *x1 = fwd ? t->sbx : t->sax, *y1 = fwd ? t->sby : t->say;
}

static bool selected(const struct term *t, int x, int y)
{
    if (!t->sel_on)
        return false;
    int x0, y0, x1, y1;
    sel_range(t, &x0, &y0, &x1, &y1);
    if (y < y0 || y > y1)
        return false;
    return !(y == y0 && x < x0) && !(y == y1 && x > x1);
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

static void term_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    struct term *t = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, TERM_BG);
    for (int y = 0; y < t->rows; y++) {
        int py = c.y + PAD + y * t->chh;
        if (py + t->chh < s->clip.y || py > s->clip.y + s->clip.h)
            continue;
        for (int x = 0; x < t->cols; x++) {
            uint16_t cell = t->cell[y][x];
            uint8_t attr = cell >> 8;
            unsigned char ch = cell & 255;
            int px = c.x + PAD + x * t->cw;
            bool cursor = x == t->cx && y == t->cy && !t->exited, sel = selected(t, x, y);
            color_t fg = fg_color(attr), bgc = bg_color(attr);
            if (cursor) {
                fg = TERM_BG;
                bgc = TERM_FG;
            } else if (sel) {
                fg = TERM_FG;
                bgc = RGB(0x34, 0x5C, 0x8C);
            }
            if (bgc != TERM_BG || cursor || sel)
                gfx_fill(s, px, py, t->cw, t->chh, bgc);
            if (ch == ' ')
                continue;
            if (t->face && ch > ' ' && ch != 0x7F && !(ch >= 0x80 && ch < 0xA0)) {
                unsigned cp = ch;                /* (cells hold ISO 8859-1 bytes) */
                int adv = (int)(fct_face_advance(t->face, cp) + 0.5f);
                fct_face_draw_cp(s, t->face, px + (t->cw - adv) / 2,
                                 py + (t->chh - fct_face_height(t->face)) / 2 + fct_face_ascent(t->face), cp, fg);
            } else {
                gfx_char(s, px, py, ch, fg, bgc, false);
            }
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

static void term_resized(struct fct_view *w);

/* Ctrl and + / - / 0 (the main keys or the keypad): the font size. */
static bool term_zoom(struct fct_view *w, const struct fct_key *ev)
{
    struct term *t = w->app;
    if (!(ev->mods & FCT_MOD_CTRL) || !t->face)
        return false;
    int px = t->px;
    if (ev->code == 0x0D || ev->code == 0x4E)
        px = MIN(32, px + 1);
    else if (ev->code == 0x0C || ev->code == 0x4A)
        px = MAX(8, px - 1);
    else if (ev->code == 0x0B)
        px = DEFAULT_PX;
    else
        return false;
    if (px != t->px) {
        t->px = px;
        term_metrics(t);
        term_resized(w);
        fct_view_invalidate(w);
    }
    return true;
}

/* Ctrl+Shift+C: the selection to the clipboard (each row's trailing blanks dropped, rows
 * joined with newlines; the cells' ISO 8859-1 as UTF-8). */
static void term_copy(struct term *t)
{
    if (!t->sel_on)
        return;
    int x0, y0, x1, y1;
    sel_range(t, &x0, &y0, &x1, &y1);
    char *buf = malloc((size_t)(y1 - y0 + 1) * (MAX_COLS * 2 + 1) + 1);
    if (!buf)
        return;
    size_t n = 0;
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : t->cols - 1;
        size_t start = n;
        for (int x = a; x <= b && x < t->cols; x++) {
            unsigned char ch = t->cell[y][x] & 255;
            if (ch < 0x80) {
                buf[n++] = ch < ' ' ? ' ' : (char)ch;
            } else {
                buf[n++] = (char)(0xC0 | ch >> 6);
                buf[n++] = (char)(0x80 | (ch & 0x3F));
            }
        }
        while (n > start && buf[n - 1] == ' ')
            n--;
        if (y < y1)
            buf[n++] = '\n';
    }
    fct_clipboard_set(buf, n);
    free(buf);
}

static void term_cell_at(const struct term *t, int x, int y, int *cx, int *cy)
{
    *cx = (x - PAD) / t->cw;
    *cy = (y - PAD) / t->chh;
    *cx = *cx < 0 ? 0 : *cx >= t->cols ? t->cols - 1 : *cx;
    *cy = *cy < 0 ? 0 : *cy >= t->rows ? t->rows - 1 : *cy;
}

static void term_mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct term *t = w->app;
    int cx, cy;
    term_cell_at(t, x, y, &cx, &cy);
    if (kind == FCT_MOUSE_DOWN) {
        t->sax = t->sbx = cx;
        t->say = t->sby = cy;
        t->selecting = true;
        t->sel_on = false;
    } else if (kind == FCT_MOUSE_MOVE && t->selecting) {
        t->sbx = cx;
        t->sby = cy;
        t->sel_on = t->sbx != t->sax || t->sby != t->say;
    } else if (kind == FCT_MOUSE_UP) {
        t->selecting = false;
    } else if (kind == FCT_MOUSE_DOUBLE) {       /* the word under the pointer */
        int a = cx, b = cx;
        while (a > 0 && (t->cell[cy][a - 1] & 255) > ' ')
            a--;
        while (b < t->cols - 1 && (t->cell[cy][b + 1] & 255) > ' ')
            b++;
        if ((t->cell[cy][cx] & 255) > ' ') {
            t->sax = a, t->sbx = b, t->say = t->sby = cy;
            t->sel_on = true;
        }
        t->selecting = false;
    } else {
        return;
    }
    fct_view_invalidate(w);
}

static void term_key(struct fct_view *w, const struct fct_key *ev)
{
    struct term *t = w->app;
    if (!ev->value)
        return;
    if (term_zoom(w, ev))
        return;
    if (ev->code == 0x2E && (ev->mods & FCT_MOD_CTRL) && (ev->mods & FCT_MOD_SHIFT)) {   /* Ctrl+Shift+C: copy */
        term_copy(t);
        return;
    }
    bool ctrl_shift = (ev->mods & FCT_MOD_SHIFT) && (ev->mods & FCT_MOD_CTRL);
    if (t->sel_on && ev->ascii && !ctrl_shift) {   /* typing drops the selection (not Ctrl, Shift themselves) */
        t->sel_on = false;
        fct_view_invalidate(w);
    }
    if (ev->code == 0x2F && (ev->mods & FCT_MOD_CTRL) && (ev->mods & FCT_MOD_SHIFT)) {   /* Ctrl+Shift+V: paste */
        size_t n;
        char *clip = fct_clipboard_get(&n);
        if (clip) {
            for (size_t i = 0; i < n; i++)
                if (clip[i] == '\n')
                    clip[i] = '\r';             /* (as Enter types it) */
            term_send(t, clip, n);
            free(clip);
        }
        return;
    }
    switch (ev->code) {
    case FCT_KEY_UP:    term_send(t, "\033[A", 3); return;
    case FCT_KEY_DOWN:  term_send(t, "\033[B", 3); return;
    case FCT_KEY_RIGHT: term_send(t, "\033[C", 3); return;
    case FCT_KEY_LEFT:  term_send(t, "\033[D", 3); return;
    }
    if (ev->ascii) {
        char ch = ev->ascii == '\n' ? '\r' : (char)ev->ascii;
        term_send(t, &ch, 1);
    }
}

static void term_readable(struct fct_view *w)
{
    struct term *t = w->app;
    char buf[2048];
    long n = read(t->master, buf, sizeof(buf));
    if (n <= 0) {
        t->exited = true;                /* all slave descriptors closed */
        char title[FCT_TITLE_MAX];
        snprintf(title, sizeof(title), "%s %d (exited)", t->plain ? "Shell" : "Terminal", t->number);
        fct_view_set_title(w, title);
        fct_view_invalidate(w);
        return;
    }
    for (long i = 0; i < n; i++)
        term_putc(t, buf[i]);
    fct_view_invalidate(w);
}

static int term_pollfd(struct fct_view *w)
{
    struct term *t = w->app;
    return t->exited ? -1 : t->master;
}

static void term_resized(struct fct_view *w)
{
    struct term *t = w->app;
    struct rect c = fct_view_content(w);
    int cols = MIN(MAX_COLS, MAX(20, (c.w - 2 * PAD) / t->cw));
    int rows = MIN(MAX_ROWS, MAX(4, (c.h - 2 * PAD) / t->chh));
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

static void term_destroy(struct fct_view *w)
{
    struct term *t = w->app;
    close(t->master);                    /* hangs up the shell's session */
    if (t->child > 0 && !t->exited)
        kill(t->child, SIGHUP);
    free(t);
}

/* A line from the desktop (the sia strip, "open terminal" with a command): type it. */
static void term_text(struct fct_view *w, const char *line)
{
    struct term *t = w->app;
    term_send(t, line, strlen(line));
    term_send(t, "\n", 1);
}

int main(int argc, char **argv)
{
    struct term *t = calloc(1, sizeof(*t));
    if (!t)
        return 1;
    t->number = 1;
    const char *first_line = NULL;               /* -e LINE: typed into the shell once it starts */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s"))
            t->plain = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            t->number = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-e") && i + 1 < argc)
            first_line = argv[++i];
    }
    signal(SIGPIPE, SIG_IGN);
    t->cols = 80;
    t->rows = 25;
    t->attr = DEFAULT_ATTR;
    for (int y = 0; y < MAX_ROWS; y++)
        clear_row(t, y, 0);

    int fds[2];
    if (openpty(&fds[0], &fds[1], NULL, NULL, NULL) < 0) {
        perror("facet-terminal: openpty");
        return 1;
    }
    struct winsize ws = { t->rows, t->cols, 0, 0 };
    ioctl(fds[0], TIOCSWINSZ, &ws);
    pid_t pid = fork();
    if (pid == 0) {                              /* (the display socket is close-on-exec) */
        close(fds[0]);
        setsid();
        dup2(fds[1], 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        if (fds[1] > 2)
            close(fds[1]);
        ioctl(0, TIOCSCTTY, 0);
        signal(SIGPIPE, SIG_DFL);
        setenv("TERM", "sieos", 1);
        struct stat st;
        if (!t->plain && stat("/bin/sia", &st) == 0) {
            char *av[] = { "sia", NULL };
            execv("/bin/sia", av);
        }
        char *av[] = { "sh", NULL };
        execv("/bin/sh", av);
        _exit(127);
    }
    close(fds[1]);
    t->master = fds[0];
    t->child = pid;
    if (first_line) {
        term_send(t, first_line, strlen(first_line));
        term_send(t, "\n", 1);
    }
    fcntl(t->master, F_SETFD, FD_CLOEXEC);
    if (getenv("SIEOS_DESKTOP")) {               /* the desktop channel (3, 4) is the child's now */
        close(3);
        close(4);
    }
    if (fct_app_init() < 0) {
        kill(pid, SIGHUP);
        return 1;
    }

    struct passwd *pw = getpwuid(geteuid());
    char title[FCT_TITLE_MAX];
    snprintf(title, sizeof(title), "%s %d - %s", t->plain ? "Shell" : "Terminal", t->number, pw ? pw->pw_name : "?");
    t->px = DEFAULT_PX;
    term_metrics(t);
    struct fct_window_attr a = { title, FCT_POS_AUTO, FCT_POS_AUTO, t->cols * t->cw + 2 * PAD,
                                 t->rows * t->chh + 2 * PAD, 30 * t->cw, 8 * t->chh,
                                 t->plain ? 0u : FCT_WIN_ASSISTANT };
    struct fct_view *w = fct_view_create(&a);
    if (!w)
        return 1;
    w->app = t;
    w->draw = term_draw;
    w->key = term_key;
    w->mouse = term_mouse;
    w->readable = term_readable;
    w->pollfd = term_pollfd;
    w->resized = term_resized;
    w->destroy = term_destroy;
    w->text = term_text;
    return fct_main();
}
