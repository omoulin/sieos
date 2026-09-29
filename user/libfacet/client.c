/*
 * client.c - libfacet: the connection to the desktop, windows with
 * shared-memory buffers, events, and the view toolkit (facet/facet.h).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "facet/facet.h"

struct fct_window {
    fct_display *d;
    uint32_t id;
    int w, h;
    uint32_t *px;
    size_t size;
    struct surface s;
    void *user;
    bool closed;                /* Facet closed it */
    fct_window *next;
};

#define EVQ 32

struct fct_display {
    int fd;
    int screen_w, screen_h;
    uint32_t next_id;
    fct_window *wins;
    struct fct_msg q[EVQ];      /* messages read while waiting for a reply */
    int qh, qn;
    char text[FCT_TITLE_MAX * 16 + 1];   /* FCT_TEXT being assembled */
    size_t textlen;
    bool dead;
};

/* ---------------- the wire ---------------- */

static int send_msg(fct_display *d, const struct fct_msg *m, int fd)
{
    if (d->dead)
        return -1;
    struct iovec iov = { (void *)m, sizeof(*m) };
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(&ctl, 0, sizeof(ctl));
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof(ctl.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(d->fd, &mh, MSG_NOSIGNAL);
        if (n == (ssize_t)sizeof(*m))
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n > 0) {                             /* the rest, without the descriptor */
            const char *p = (const char *)m + n;
            size_t left = sizeof(*m) - n;
            while (left) {
                ssize_t k = write(d->fd, p, left);
                if (k <= 0 && errno != EINTR)
                    break;
                if (k > 0)
                    p += k, left -= k;
            }
            if (!left)
                return 0;
        }
        d->dead = true;
        return -1;
    }
}

static int read_msg(fct_display *d, struct fct_msg *m)
{
    size_t got = 0;
    while (got < sizeof(*m)) {
        ssize_t n = read(d->fd, (char *)m + got, sizeof(*m) - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            d->dead = true;
            return -1;
        }
        got += n;
    }
    return 0;
}

/* ---------------- display ---------------- */

fct_display *fct_open(void)
{
    fct_skin_load();                             /* the user's skin, until the desktop says otherwise */
    const char *path = getenv("FACET_DISPLAY");
    char def[64];
    if (!path || !*path) {
        snprintf(def, sizeof(def), "/tmp/.facet-%u", (unsigned)getuid());
        path = def;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(a.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    fct_display *d = calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    d->fd = fd;
    d->next_id = 1;
    struct fct_msg m;
    memset(&m, 0, sizeof(m));
    m.type = FCT_HELLO;
    m.code = FCT_PROTOCOL_VERSION;
    m.x = getpid();
    if (send_msg(d, &m, -1) < 0 || read_msg(d, &m) < 0 || m.type != FCT_WELCOME) {
        close(fd);
        free(d);
        errno = EPROTO;
        return NULL;
    }
    d->screen_w = m.w;
    d->screen_h = m.h;
    return d;
}

void fct_disconnect(fct_display *d)
{
    if (!d)
        return;
    while (d->wins)
        fct_window_destroy(d->wins);
    close(d->fd);
    free(d);
}

int fct_fd(fct_display *d) { return d->fd; }

void fct_screen_size(fct_display *d, int *w, int *h)
{
    *w = d->screen_w;
    *h = d->screen_h;
}

/* ---------------- windows ---------------- */

/* A shared buffer of w x h pixels: a POSIX shared memory object, unlinked
 * at once (Facet gets the descriptor). */
static int new_buffer(fct_window *win, int w, int h)
{
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    size_t size = (size_t)w * h * 4;
    static unsigned serial;
    char name[64];
    int fd = -1;
    for (int tries = 0; fd < 0 && tries < 100; tries++) {
        snprintf(name, sizeof(name), "/facet-%d-%u-%u", (int)getpid(), win->id, serial++);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0)
        return -1;
    shm_unlink(name);
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return -1;
    }
    if (win->px)
        munmap(win->px, win->size);
    win->px = px;
    win->size = size;
    win->w = w;
    win->h = h;
    win->s.px = px;
    win->s.w = win->s.stride = w;
    win->s.h = h;
    win->s.clip = rect_make(0, 0, w, h);
    return fd;
}

fct_window *fct_window_create(fct_display *d, const struct fct_window_attr *a)
{
    fct_window *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;
    win->d = d;
    win->id = d->next_id++;
    int fd = new_buffer(win, a->w, a->h);
    if (fd < 0) {
        free(win);
        return NULL;
    }
    struct fct_msg m;
    memset(&m, 0, sizeof(m));
    m.type = FCT_CREATE;
    m.window = win->id;
    m.x = a->x;
    m.y = a->y;
    m.w = win->w;
    m.h = win->h;
    m.code = a->min_w;
    m.value = a->min_h;
    m.kind = a->flags;
    snprintf(m.title, sizeof(m.title), "%s", a->title ? a->title : "");
    int r = send_msg(d, &m, fd);
    close(fd);
    if (r < 0) {
        munmap(win->px, win->size);
        free(win);
        return NULL;
    }
    win->next = d->wins;
    d->wins = win;
    return win;
}

void fct_window_destroy(fct_window *win)
{
    fct_display *d = win->d;
    if (!win->closed) {
        struct fct_msg m;
        memset(&m, 0, sizeof(m));
        m.type = FCT_DESTROY;
        m.window = win->id;
        send_msg(d, &m, -1);
    }
    for (fct_window **p = &d->wins; *p; p = &(*p)->next)
        if (*p == win) {
            *p = win->next;
            break;
        }
    munmap(win->px, win->size);
    free(win);
}

struct surface *fct_window_surface(fct_window *w) { return &w->s; }

void fct_window_size(fct_window *w, int *width, int *height)
{
    *width = w->w;
    *height = w->h;
}

void fct_window_damage(fct_window *w, struct rect r)
{
    r = rect_intersect(r, rect_make(0, 0, w->w, w->h));
    if (rect_empty(r) || w->closed)
        return;
    struct fct_msg m;
    memset(&m, 0, sizeof(m));
    m.type = FCT_DAMAGE;
    m.window = w->id;
    m.x = r.x;
    m.y = r.y;
    m.w = r.w;
    m.h = r.h;
    send_msg(w->d, &m, -1);
}

void fct_window_set_title(fct_window *w, const char *title)
{
    struct fct_msg m;
    memset(&m, 0, sizeof(m));
    m.type = FCT_TITLE;
    m.window = w->id;
    snprintf(m.title, sizeof(m.title), "%s", title);
    send_msg(w->d, &m, -1);
}

void fct_window_set_user(fct_window *w, void *p) { w->user = p; }
void *fct_window_user(fct_window *w) { return w->user; }

/* ---------------- events ---------------- */

static fct_window *find(fct_display *d, uint32_t id)
{
    for (fct_window *w = d->wins; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

/* A message into an event: 1, or 0 if it is not one for the application. */
static int translate(fct_display *d, const struct fct_msg *m, struct fct_event *ev)
{
    if (m->type == FCT_EV_SKIN) {                /* for the whole application */
        char name[sizeof(m->title) + 1];
        memcpy(name, m->title, sizeof(m->title));
        name[sizeof(m->title)] = 0;
        fct_skin_use(fct_skin_named(name));
        memset(ev, 0, sizeof(*ev));
        ev->type = FCT_SKIN;
        return 1;
    }
    fct_window *w = find(d, m->window);
    if (!w || w->closed)
        return 0;
    memset(ev, 0, sizeof(*ev));
    ev->window = w;
    switch (m->type) {
    case FCT_EV_KEY:
        ev->type = FCT_KEY;
        ev->key.code = m->code;
        ev->key.value = m->value;
        ev->key.ascii = m->ascii;
        ev->key.mods = m->mods;
        return 1;
    case FCT_EV_MOUSE:
        ev->type = FCT_MOUSE;
        ev->x = m->x;
        ev->y = m->y;
        ev->kind = m->kind;
        ev->buttons = m->buttons;
        return 1;
    case FCT_EV_RESIZE: {
        if (m->w == w->w && m->h == w->h)
            return 0;
        int fd = new_buffer(w, m->w, m->h);
        if (fd < 0)
            return 0;                            /* keep the old buffer */
        struct fct_msg b;
        memset(&b, 0, sizeof(b));
        b.type = FCT_BUFFER;
        b.window = w->id;
        b.w = w->w;
        b.h = w->h;
        send_msg(d, &b, fd);
        close(fd);
        ev->type = FCT_RESIZE;
        ev->w = w->w;
        ev->h = w->h;
        return 1;
    }
    case FCT_EV_CLOSE:
        w->closed = true;
        ev->type = FCT_CLOSE;
        return 1;
    case FCT_EV_TEXT: {
        size_t n = strnlen(m->title, sizeof(m->title));
        if (d->textlen + n < sizeof(d->text)) {
            memcpy(d->text + d->textlen, m->title, n);
            d->textlen += n;
        }
        if (!m->value)
            return 0;
        d->text[d->textlen] = 0;
        d->textlen = 0;
        ev->type = FCT_TEXT;
        ev->text = d->text;
        return 1;
    }
    }
    return 0;
}

int fct_next_event(fct_display *d, struct fct_event *ev, int timeout_ms)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (d->dead)
            return -1;
        struct fct_msg m;
        if (d->qn) {
            m = d->q[d->qh];
            d->qh = (d->qh + 1) % EVQ;
            d->qn--;
        } else {
            int wait = timeout_ms;
            if (timeout_ms > 0) {
                struct timespec t;
                clock_gettime(CLOCK_MONOTONIC, &t);
                long spent = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
                wait = spent >= timeout_ms ? 0 : (int)(timeout_ms - spent);
            }
            struct pollfd p = { d->fd, POLLIN, 0 };
            int r = poll(&p, 1, wait);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                return 0;
            if (read_msg(d, &m) < 0)
                return -1;
        }
        if (translate(d, &m, ev))
            return 1;
    }
}

/* ---------------- views ---------------- */

static fct_display *app_d;
static struct fct_view *views;
static bool quit;

int fct_app_init(void)
{
    if (app_d)
        return 0;
    app_d = fct_open();
    if (!app_d) {
        fprintf(stderr, "cannot connect to the Facet desktop (%s): %s\n",
                getenv("FACET_DISPLAY") ? getenv("FACET_DISPLAY") : "FACET_DISPLAY is not set", strerror(errno));
        return -1;
    }
    return 0;
}

fct_display *fct_app_display(void) { return app_d; }

void fct_screen(int *w, int *h)
{
    if (app_d)
        fct_screen_size(app_d, w, h);
    else
        *w = *h = 0;
}

struct fct_view *fct_view_create(const struct fct_window_attr *a)
{
    if (!app_d && fct_app_init() < 0)
        return NULL;
    struct fct_view *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->win = fct_window_create(app_d, a);
    if (!v->win) {
        free(v);
        return NULL;
    }
    fct_window_set_user(v->win, v);
    snprintf(v->title, sizeof(v->title), "%s", a->title ? a->title : "");
    v->dirty = true;
    v->next = views;
    views = v;
    return v;
}

struct fct_view *fct_view_new(const char *title, int w, int h)
{
    struct fct_window_attr a = { title, FCT_POS_AUTO, FCT_POS_AUTO, w, h, 0, 0, 0 };
    return fct_view_create(&a);
}

void fct_view_invalidate(struct fct_view *v) { v->dirty = true; }

void fct_view_set_title(struct fct_view *v, const char *title)
{
    if (!strcmp(v->title, title))
        return;
    snprintf(v->title, sizeof(v->title), "%s", title);
    fct_window_set_title(v->win, v->title);
}

struct rect fct_view_content(struct fct_view *v)
{
    int w, h;
    fct_window_size(v->win, &w, &h);
    return rect_make(0, 0, w, h);
}

void fct_view_close(struct fct_view *v) { v->closing = true; }

void fct_quit(void) { quit = true; }

static void reap(void)
{
    for (struct fct_view **p = &views; *p;) {
        struct fct_view *v = *p;
        if (!v->closing) {
            p = &v->next;
            continue;
        }
        *p = v->next;
        if (v->destroy)
            v->destroy(v);
        fct_window_destroy(v->win);
        free(v);
    }
}

static void redraw(void)
{
    for (struct fct_view *v = views; v; v = v->next) {
        if (!v->dirty || v->closing)
            continue;
        v->dirty = false;
        struct surface *s = fct_window_surface(v->win);
        struct rect c = fct_view_content(v);
        gfx_set_clip(s, c);
        if (v->draw)
            v->draw(v, s, c);
        else
            gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
        fct_window_damage(v->win, c);
    }
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void dispatch(const struct fct_event *ev)
{
    if (ev->type == FCT_SKIN) {                  /* every view, in the new colours */
        for (struct fct_view *v = views; v; v = v->next)
            v->dirty = true;
        return;
    }
    struct fct_view *v = fct_window_user(ev->window);
    if (!v || v->closing)
        return;
    switch (ev->type) {
    case FCT_KEY:
        if (v->key)
            v->key(v, &ev->key);
        break;
    case FCT_MOUSE:
        if (v->mouse)
            v->mouse(v, ev->x, ev->y, ev->kind, ev->buttons);
        break;
    case FCT_RESIZE:
        if (v->resized)
            v->resized(v);
        v->dirty = true;
        break;
    case FCT_CLOSE:
        v->closing = true;
        break;
    case FCT_TEXT:
        if (v->text)
            v->text(v, ev->text);
        break;
    }
}

int fct_main(void)
{
    if (!app_d && fct_app_init() < 0)
        return 1;
    long last_tick = now_ms();
    quit = false;
    while (views && !quit) {
        reap();
        redraw();
        if (!views)
            break;
        struct pollfd p[65];
        struct fct_view *owner[65];
        int n = 0;
        p[n].fd = fct_fd(app_d);
        p[n].events = POLLIN;
        owner[n++] = NULL;
        for (struct fct_view *v = views; v && n < 65; v = v->next) {
            int fd = v->pollfd && !v->closing ? v->pollfd(v) : -1;
            if (fd >= 0) {
                p[n].fd = fd;
                p[n].events = POLLIN;
                owner[n++] = v;
            }
        }
        long wait = 250 - (now_ms() - last_tick);
        if (poll(p, n, wait < 0 ? 0 : (int)wait) > 0) {
            if (p[0].revents) {
                struct fct_event ev;
                int r;
                while ((r = fct_next_event(app_d, &ev, 0)) > 0)
                    dispatch(&ev);
                if (r < 0)
                    return 1;
            }
            for (int i = 1; i < n; i++)
                if (p[i].revents && owner[i]->readable && !owner[i]->closing)
                    owner[i]->readable(owner[i]);
        }
        if (now_ms() - last_tick >= 250) {
            last_tick = now_ms();
            for (struct fct_view *v = views; v; v = v->next)
                if (v->tick && !v->closing)
                    v->tick(v);
        }
    }
    reap();
    return 0;
}
