/*
 * chat.c - A Facet window that talks to the assistant: libfacet for the
 * window, libsia for the model.  The request runs in the background
 * (sia_chat_send_async); the view's pollfd hands its descriptor to
 * fct_main, so the window stays responsive while the model thinks, and
 * each time it is readable sia_chat_poll gives the text that arrived: the
 * answer appears as it is written.
 *
 *   cc chat.c -lfacet -lsia -o chat && ./chat      (in a Facet terminal)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <facet/facet.h>
#include <sia/sia.h>

#define MAXLINES 400
#define PAD 8

struct chat {
    sia_chat *sia;
    int fd;                          /* the request in progress, or -1 */
    char input[256];
    int len;
    char *lines[MAXLINES];           /* the transcript, wrapped */
    color_t color[MAXLINES];
    int n;
    char *reply;                     /* the answer being written (UTF-8) */
    int reply_first;                 /* its first line */
};

/* Append text, wrapped to cols characters. */
static void say(struct chat *ch, const char *text, color_t color, int cols)
{
    const char *p = text;
    while (*p || p == text) {
        int n = 0, last_space = -1;
        while (p[n] && p[n] != '\n' && n < cols) {
            if (p[n] == ' ')
                last_space = n;
            n++;
        }
        if (p[n] && p[n] != '\n' && last_space > 0)
            n = last_space;
        if (ch->n == MAXLINES) {
            free(ch->lines[0]);
            memmove(ch->lines, ch->lines + 1, (MAXLINES - 1) * sizeof(char *));
            memmove(ch->color, ch->color + 1, (MAXLINES - 1) * sizeof(color_t));
            ch->n--;
            if (ch->reply_first > 0)
                ch->reply_first--;
        }
        ch->lines[ch->n] = strndup(p, n);
        ch->color[ch->n++] = color;
        p += n;
        if (*p == ' ' || *p == '\n')
            p++;
        if (!*p)
            break;
    }
}

/* Show the answer so far, re-wrapped in place. */
static void show_reply(struct chat *ch, int ncols)
{
    while (ch->n > ch->reply_first)
        free(ch->lines[--ch->n]);
    char *p = sia_plain(ch->reply);
    say(ch, p, C_TEXT, ncols);
    free(p);
}

static int cols(struct fct_view *v)
{
    return (fct_view_content(v).w - 2 * PAD) / FONT_W;
}

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    struct chat *ch = v->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
    int rows = (c.h - 40) / FONT_H;
    int first = ch->n > rows ? ch->n - rows : 0;
    for (int i = first; i < ch->n; i++)
        gfx_text(s, c.x + PAD, c.y + PAD + (i - first) * FONT_H, ch->lines[i], ch->color[i]);
    struct rect box = rect_make(c.x + 4, c.y + c.h - 30, c.w - 8, 26);
    ui_panel(s, box, true);
    char shown[300];
    snprintf(shown, sizeof(shown), "%s%s", ch->input, ch->fd >= 0 ? "" : "_");
    gfx_text(s, box.x + 6, box.y + 5, ch->fd >= 0 ? "(sia is thinking...)" : shown, ch->fd >= 0 ? C_DIM : C_TEXT);
}

static void key(struct fct_view *v, const struct fct_key *k)
{
    struct chat *ch = v->app;
    if (!k->value || ch->fd >= 0 || !ch->sia)
        return;
    if (k->ascii == '\n' && ch->len) {
        char line[300], err[256];
        snprintf(line, sizeof(line), "> %s", ch->input);
        say(ch, line, C_ACCENT, cols(v));
        ch->fd = sia_chat_send_async(ch->sia, ch->input, err, sizeof(err));
        if (ch->fd < 0)
            say(ch, err, C_BAD, cols(v));
        free(ch->reply);
        ch->reply = strdup("");
        ch->reply_first = ch->n;
        ch->len = 0;
        ch->input[0] = 0;
    } else if (k->ascii == '\b' && ch->len) {
        ch->input[--ch->len] = 0;
    } else if (k->ascii >= 32 && k->ascii < 127 && ch->len < (int)sizeof(ch->input) - 1) {
        ch->input[ch->len++] = (char)k->ascii;
        ch->input[ch->len] = 0;
    }
    fct_view_invalidate(v);
}

static int pollfd(struct fct_view *v)
{
    return ((struct chat *)v->app)->fd;
}

static void readable(struct fct_view *v)
{
    struct chat *ch = v->app;
    char err[256];
    bool done;
    char *piece = sia_chat_poll(ch->sia, &done, err, sizeof(err));
    if (piece && *piece) {
        size_t a = strlen(ch->reply), b = strlen(piece);
        ch->reply = realloc(ch->reply, a + b + 1);
        memcpy(ch->reply + a, piece, b + 1);
        show_reply(ch, cols(v));
    }
    if (done) {
        ch->fd = -1;
        if (!piece)
            say(ch, err, C_BAD, cols(v));
    }
    free(piece);
    fct_view_invalidate(v);
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    struct chat *ch = calloc(1, sizeof(*ch));
    ch->fd = -1;
    ch->sia = sia_chat_new(NULL);
    struct fct_view *v = fct_view_new("Chat with sia", 520, 360);
    if (!v)
        return 1;
    v->app = ch;
    v->draw = draw;
    v->key = key;
    v->pollfd = pollfd;
    v->readable = readable;
    if (ch->sia) {
        char line[160];
        snprintf(line, sizeof(line), "Connected to %s. Type a message and press Enter.", sia_model());
        say(ch, line, C_DIM, cols(v));
    } else {
        say(ch, "No model is registered: run sia in a terminal once to set one up.", C_BAD, cols(v));
    }
    return fct_main();
}
