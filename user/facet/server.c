/*
 * server.c - The Facet window server: application windows from other
 * processes (facet/protocol.h, the client side is libfacet).
 *
 * Facet listens on /tmp/.facet-<uid> and puts that in FACET_DISPLAY for
 * the programs it starts.  Each client window is an ordinary window here
 * whose content is the client's shared-memory buffer; its input is sent to
 * the client.  Replies and events go out through a per-connection queue so
 * a slow client never blocks the desktop.
 */
#include "facet.h"
#include "facet/protocol.h"
#include <sys/un.h>

#define MAXCONN 32
#define MAXFDS 8
#define OUTQ (256 * sizeof(struct fct_msg))

struct conn {
    bool used;
    int fd;
    pid_t pid;
    char in[sizeof(struct fct_msg)];
    size_t inlen;
    int fds[MAXFDS];                 /* descriptors received, for CREATE / BUFFER */
    int nfds;
    char *out;
    size_t outlen;
};

struct rwin {
    int conn;
    uint32_t id;
    uint32_t *px;
    int bw, bh;
    size_t size;
    int last_w, last_h;              /* content size last announced */
    unsigned flags;
    bool client_gone;                /* destroyed by the client, or it disconnected */
};

static struct conn conns[MAXCONN];
static int listen_fd = -1;
static char sock_path[64];

/* ---------------- output ---------------- */

static void flush(struct conn *c)
{
    while (c->outlen) {
        ssize_t n = write(c->fd, c->out, c->outlen);
        if (n <= 0)
            return;                                  /* EAGAIN: poll for POLLOUT */
        memmove(c->out, c->out + n, c->outlen - n);
        c->outlen -= n;
    }
}

static void send_to(int ci, const struct fct_msg *m)
{
    struct conn *c = &conns[ci];
    if (!c->used)
        return;
    if (c->outlen + sizeof(*m) > OUTQ) {
        if (m->type == FCT_EV_MOUSE)
            return;                                  /* a stuck client loses mouse motion */
        flush(c);
        if (c->outlen + sizeof(*m) > OUTQ)
            return;
    }
    memcpy(c->out + c->outlen, m, sizeof(*m));
    c->outlen += sizeof(*m);
    flush(c);
}

/* ---------------- windows ---------------- */

static struct rwin *rw(struct window *w) { return w->app; }

static void rwin_draw(struct window *w, struct surface *s, struct rect c)
{
    struct rwin *r = rw(w);
    int cw = MIN(c.w, r->bw), ch = MIN(c.h, r->bh);
    if (r->px && cw > 0 && ch > 0) {
        struct surface src = { r->px, r->bw, r->bh, r->bw, rect_make(0, 0, r->bw, r->bh) };
        gfx_blit(s, c.x, c.y, &src, rect_make(0, 0, cw, ch));
    }
    if (cw < c.w)
        gfx_fill(s, c.x + MAX(cw, 0), c.y, c.w - MAX(cw, 0), c.h, C_CONTENT);
    if (ch < c.h)
        gfx_fill(s, c.x, c.y + MAX(ch, 0), MAX(cw, 0), c.h - MAX(ch, 0), C_CONTENT);
}

static void rwin_key(struct window *w, const struct input_event *ev)
{
    struct rwin *r = rw(w);
    if (ev->type != EV_KEY || r->client_gone)
        return;
    struct fct_msg m = { .type = FCT_EV_KEY, .window = r->id, .code = ev->code, .value = ev->value,
                         .ascii = ev->ascii, .mods = ev->mods };
    send_to(r->conn, &m);
}

static void rwin_mouse(struct window *w, int x, int y, int kind, int buttons)
{
    struct rwin *r = rw(w);
    if (r->client_gone)
        return;
    struct fct_msg m = { .type = FCT_EV_MOUSE, .window = r->id, .x = x, .y = y, .kind = kind,
                         .buttons = buttons };
    send_to(r->conn, &m);
}

static void rwin_resized(struct window *w)
{
    struct rwin *r = rw(w);
    struct rect c = wm_content(w);
    if (r->client_gone || (c.w == r->last_w && c.h == r->last_h))
        return;
    r->last_w = c.w;
    r->last_h = c.h;
    struct fct_msg m = { .type = FCT_EV_RESIZE, .window = r->id, .w = c.w, .h = c.h };
    send_to(r->conn, &m);
}

static void unmap(struct rwin *r)
{
    if (r->px)
        munmap(r->px, r->size);
    r->px = NULL;
}

static void rwin_destroy(struct window *w)
{
    struct rwin *r = rw(w);
    if (!r->client_gone) {                            /* closed on the desktop: tell the client */
        struct fct_msg m = { .type = FCT_EV_CLOSE, .window = r->id };
        send_to(r->conn, &m);
    }
    unmap(r);
    free(r);
}

static struct window *find_window(int ci, uint32_t id)
{
    struct window *ws[64];
    int n = wm_window_list(ws, 64);
    for (int i = 0; i < n; i++)
        if (ws[i]->draw == rwin_draw && rw(ws[i])->conn == ci && rw(ws[i])->id == id && !ws[i]->dead)
            return ws[i];
    return NULL;
}

/* Map a client's buffer of w x h pixels. */
static bool map_buffer(struct rwin *r, int fd, int w, int h)
{
    if (w < 1 || h < 1 || w > 8192 || h > 8192)
        return false;
    size_t size = (size_t)w * h * 4;
    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < size)
        return false;
    void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return false;
    unmap(r);
    r->px = p;
    r->size = size;
    r->bw = w;
    r->bh = h;
    return true;
}

static int take_fd(struct conn *c)
{
    if (!c->nfds)
        return -1;
    int fd = c->fds[0];
    memmove(c->fds, c->fds + 1, --c->nfds * sizeof(int));
    return fd;
}

static void create(int ci, const struct fct_msg *m, int fd)
{
    struct rwin *r = calloc(1, sizeof(*r));
    if (!r)
        return;
    r->conn = ci;
    r->id = m->window;
    r->flags = m->kind;
    if (fd < 0 || !map_buffer(r, fd, m->w, m->h)) {
        free(r);
        return;
    }
    char title[FCT_TITLE_MAX];
    memcpy(title, m->title, sizeof(title));
    title[sizeof(title) - 1] = 0;
    int x = m->x, y = m->y;
    struct frame_insets fi = wm_frame();
    int fw = m->w + fi.left + fi.right, fh = m->h + fi.top + fi.bottom;
    if (x == FCT_POS_CENTER) {
        x = (screen_w - fw) / 2;
        y = screen_h / 3;
    } else if (x < 0 || y < 0) {
        x = y = -1;
    }
    struct window *w = wm_create(title, x, y, m->w, m->h);
    if (!w) {
        unmap(r);
        free(r);
        struct fct_msg c = { .type = FCT_EV_CLOSE, .window = m->window };
        send_to(ci, &c);
        return;
    }
    w->app = r;
    w->draw = rwin_draw;
    w->key = rwin_key;
    w->mouse = rwin_mouse;
    w->resized = rwin_resized;
    w->destroy = rwin_destroy;
    if (m->code > 0)
        w->min_w = MAX(w->min_w, (int)m->code + fi.left + fi.right);
    if (m->value > 0)
        w->min_h = MAX(w->min_h, (int)m->value + fi.top + fi.bottom);
    struct rect c = wm_content(w);
    r->last_w = m->w;
    r->last_h = m->h;
    if (c.w != m->w || c.h != m->h)                   /* the desktop made it smaller: say so */
        rwin_resized(w);
    (void)fh;
}

static void handle(int ci, const struct fct_msg *m)
{
    struct conn *c = &conns[ci];
    switch (m->type) {
    case FCT_HELLO: {
        c->pid = m->x;
        struct fct_msg r = { .type = FCT_WELCOME, .code = FCT_PROTOCOL_VERSION, .w = screen_w, .h = screen_h };
        send_to(ci, &r);
        return;
    }
    case FCT_CREATE: {
        int fd = take_fd(c);
        if (!find_window(ci, m->window))
            create(ci, m, fd);
        if (fd >= 0)
            close(fd);
        return;
    }
    }
    struct window *w = find_window(ci, m->window);
    switch (m->type) {
    case FCT_BUFFER: {
        int fd = take_fd(c);
        if (w && fd >= 0 && map_buffer(rw(w), fd, m->w, m->h))
            wm_invalidate_rect(wm_content(w));
        if (fd >= 0)
            close(fd);
        return;
    }
    case FCT_DAMAGE:
        if (w) {
            struct rect cr = wm_content(w);
            wm_invalidate_rect(rect_intersect(cr, rect_make(cr.x + m->x, cr.y + m->y, m->w, m->h)));
        }
        return;
    case FCT_TITLE:
        if (w) {
            memcpy(w->title, m->title, sizeof(w->title) - 1);
            w->title[sizeof(w->title) - 1] = 0;
            wm_invalidate(w);
        }
        return;
    case FCT_DESTROY:
        if (w) {
            rw(w)->client_gone = true;
            wm_close(w);
        }
        return;
    }
}

static void drop(int ci)
{
    struct conn *c = &conns[ci];
    struct window *ws[64];
    int n = wm_window_list(ws, 64);
    for (int i = 0; i < n; i++)
        if (ws[i]->draw == rwin_draw && rw(ws[i])->conn == ci) {
            rw(ws[i])->client_gone = true;
            wm_close(ws[i]);
        }
    close(c->fd);
    while (c->nfds)
        close(take_fd(c));
    free(c->out);
    memset(c, 0, sizeof(*c));
}

static void conn_readable(int ci)
{
    struct conn *c = &conns[ci];
    char buf[sizeof(struct fct_msg) * 8];
    union {
        struct cmsghdr h;
        char b[CMSG_SPACE(MAXFDS * sizeof(int))];
    } ctl;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl.b;
    mh.msg_controllen = sizeof(ctl.b);
    ssize_t n = recvmsg(c->fd, &mh, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (n <= 0) {
        drop(ci);
        return;
    }
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&mh); h; h = CMSG_NXTHDR(&mh, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS)
            continue;
        int k = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (int i = 0; i < k; i++) {
            int fd;
            memcpy(&fd, CMSG_DATA(h) + i * sizeof(int), sizeof(int));
            if (c->nfds < MAXFDS)
                c->fds[c->nfds++] = fd;
            else
                close(fd);
        }
    }
    for (ssize_t off = 0; off < n;) {
        size_t k = MIN((size_t)(n - off), sizeof(c->in) - c->inlen);
        memcpy(c->in + c->inlen, buf + off, k);
        c->inlen += k;
        off += k;
        if (c->inlen == sizeof(c->in)) {
            struct fct_msg m;
            memcpy(&m, c->in, sizeof(m));
            c->inlen = 0;
            handle(ci, &m);
            if (!c->used)
                return;
        }
    }
}

/* ---------------- the socket ---------------- */

int server_start(void)
{
    snprintf(sock_path, sizeof(sock_path), "/tmp/.facet-%u", (unsigned)getuid());
    unlink(sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strlcpy(a.sun_path, sock_path, sizeof(a.sun_path));
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        perror("facet: window server socket");
        if (fd >= 0)
            close(fd);
        return -1;
    }
    chmod(sock_path, 0700);
    listen_fd = fd;
    setenv("FACET_DISPLAY", sock_path, 1);
    return 0;
}

/* Every application: the desktop's skin changed. */
void server_broadcast_skin(const char *name)
{
    struct fct_msg m;
    memset(&m, 0, sizeof(m));
    m.type = FCT_EV_SKIN;
    snprintf(m.title, sizeof(m.title), "%s", name);
    for (int i = 0; i < MAXCONN; i++)
        if (conns[i].used)
            send_to(i, &m);
}

void server_stop(void)
{
    for (int i = 0; i < MAXCONN; i++)
        if (conns[i].used)
            drop(i);
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(sock_path);
    }
    listen_fd = -1;
}

static void accept_client(void)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    for (int i = 0; i < MAXCONN; i++)
        if (!conns[i].used) {
            struct conn *c = &conns[i];
            memset(c, 0, sizeof(*c));
            c->out = malloc(OUTQ);
            if (!c->out)
                break;
            c->used = true;
            c->fd = fd;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            return;
        }
    close(fd);
}

/* Descriptors to poll: ids[i] is -1 for the listening socket, else the connection. */
int server_poll_fds(struct pollfd *p, int max, int *ids)
{
    int n = 0;
    if (listen_fd >= 0 && n < max) {
        p[n].fd = listen_fd;
        p[n].events = POLLIN;
        p[n].revents = 0;
        ids[n++] = -1;
    }
    for (int i = 0; i < MAXCONN && n < max; i++)
        if (conns[i].used) {
            p[n].fd = conns[i].fd;
            p[n].events = POLLIN | (conns[i].outlen ? POLLOUT : 0);
            p[n].revents = 0;
            ids[n++] = i;
        }
    return n;
}

void server_ready(int id, short revents)
{
    if (id < 0) {
        accept_client();
        return;
    }
    if (!conns[id].used)
        return;
    if (revents & POLLOUT)
        flush(&conns[id]);
    if (revents & (POLLIN | POLLHUP | POLLERR))
        conn_readable(id);
}

/* ---------------- for the desktop ---------------- */

bool server_window(struct window *w)
{
    return w->draw == rwin_draw;
}

pid_t server_window_pid(struct window *w)
{
    return server_window(w) ? conns[rw(w)->conn].pid : 0;
}

bool server_is_assistant(struct window *w)
{
    return server_window(w) && !w->dead && (rw(w)->flags & FCT_WIN_ASSISTANT) && !rw(w)->client_gone;
}

/* Type a line into a client window (the assistant terminal). */
void server_type_line(struct window *w, const char *text)
{
    if (!server_window(w) || rw(w)->client_gone)
        return;
    struct rwin *r = rw(w);
    size_t len = strlen(text), chunk = FCT_TITLE_MAX - 1;
    for (size_t off = 0;; off += chunk) {
        struct fct_msg m = { .type = FCT_EV_TEXT, .window = r->id };
        size_t k = len - off < chunk ? len - off : chunk;
        memcpy(m.title, text + off, k);
        m.value = off + k >= len;
        send_to(r->conn, &m);
        if (m.value)
            break;
    }
}

/* Serve clients (only) until a window of process pid appears, or timeout_ms passes. */
struct window *server_wait_window(pid_t pid, int timeout_ms)
{
    long end = uptime_ms() + timeout_ms;
    for (;;) {
        struct window *ws[64];
        int n = wm_window_list(ws, 64);
        for (int i = 0; i < n; i++)
            if (server_window_pid(ws[i]) == pid && !ws[i]->dead)
                return ws[i];
        long left = end - uptime_ms();
        if (left <= 0)
            return NULL;
        struct pollfd p[MAXCONN + 1];
        int ids[MAXCONN + 1];
        int k = server_poll_fds(p, MAXCONN + 1, ids);
        if (poll(p, k, (int)MIN(left, 50)) > 0)
            for (int i = 0; i < k; i++)
                if (p[i].revents)
                    server_ready(ids[i], p[i].revents);
    }
}
