/* pkg-config: gio-unix-2.0 gio-2.0 gobject-2.0 glib-2.0 */
/*
 * wtest-glib (make webdeps-test): GLib, GObject and GIO on SIEOS, as WPE
 * WebKit uses them: a main loop with timeouts, idles and a thread's wakeup
 * (its eventfd), a thread pool, a GObject with a property and a signal (the
 * generic marshaller: libffi), GVariant, GRegex (PCRE2), checksums, the wall
 * clock, files, a key file, GSubprocess (posix_spawn, its output through a
 * pipe, its end seen by a child watch: a pidfd), and a unix socket pair
 * passing a file descriptor (GUnixFDList).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixfdmessage.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest glib: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

/* ---- a GObject with a property and a signal */
typedef struct { GObject parent; int level; } Probe;
typedef struct { GObjectClass parent; } ProbeClass;
G_DEFINE_TYPE(Probe, probe, G_TYPE_OBJECT)
enum { PROP_LEVEL = 1 };
static guint sig_changed;
static void probe_set(GObject *o, guint id, const GValue *v, GParamSpec *ps)
{
    (void)ps;
    if (id == PROP_LEVEL) { ((Probe *)o)->level = g_value_get_int(v); g_signal_emit(o, sig_changed, 0, ((Probe *)o)->level, "level"); }
}
static void probe_get(GObject *o, guint id, GValue *v, GParamSpec *ps) { (void)ps; if (id == PROP_LEVEL) g_value_set_int(v, ((Probe *)o)->level); }
static void probe_class_init(ProbeClass *k)
{
    GObjectClass *oc = G_OBJECT_CLASS(k);
    oc->set_property = probe_set; oc->get_property = probe_get;
    g_object_class_install_property(oc, PROP_LEVEL, g_param_spec_int("level", 0, 0, 0, 100, 0, G_PARAM_READWRITE));
    sig_changed = g_signal_new("changed", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, 0, 0, 0, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
}
static void probe_init(Probe *p) { p->level = 1; }
static int seen_level;
static void on_changed(Probe *p, int level, const char *what, gpointer data) { (void)p; (void)data; if (!strcmp(what, "level")) seen_level = level; }

/* ---- the main loop */
static int ticks, idles;
static gboolean on_tick(gpointer loop) { if (++ticks == 3) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; } return G_SOURCE_CONTINUE; }
static gboolean on_idle(gpointer d) { (void)d; idles++; return G_SOURCE_REMOVE; }
static gboolean from_thread(gpointer loop) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }
static gpointer waker(gpointer loop) { g_usleep(30000); g_idle_add(from_thread, loop); return 0; }
static void pool_job(gpointer data, gpointer sum) { g_atomic_int_add((gint *)sum, GPOINTER_TO_INT(data)); }

/* ---- GSubprocess */
static void sub_done(GObject *src, GAsyncResult *res, gpointer loop)
{
    GError *e = 0;
    char *out = 0;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, 0, &e);
    check("GSubprocess: the child's output through a pipe", out && strstr(out, "wtest glib: child"));
    check("... and its end seen (a child watch: pidfd)", g_subprocess_get_if_exited(G_SUBPROCESS(src)) && g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 3);
    g_free(out);
    g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) { printf("wtest glib: child\n"); return 3; }
    GMainLoop *loop = g_main_loop_new(0, FALSE);
    g_timeout_add(10, on_tick, loop);
    g_idle_add(on_idle, 0);
    g_main_loop_run(loop);
    check("a main loop: timeouts and an idle", ticks == 3 && idles == 1);
    GThread *t = g_thread_new("waker", waker, loop);
    gint64 t0 = g_get_monotonic_time();
    g_main_loop_run(loop);
    g_thread_join(t);
    check("woken by another thread (its idle source)", g_get_monotonic_time() - t0 >= 25000);
    int sum = 0;
    GThreadPool *pool = g_thread_pool_new(pool_job, &sum, 4, FALSE, 0);
    for (int i = 1; i <= 100; i++) g_thread_pool_push(pool, GINT_TO_POINTER(i), 0);
    g_thread_pool_free(pool, FALSE, TRUE);
    check("a thread pool: 100 jobs", sum == 5050);

    Probe *p = g_object_new(probe_get_type(), 0);
    g_signal_connect(p, "changed", G_CALLBACK(on_changed), 0);
    g_object_set(p, "level", 42, NULL);
    int lv = 0;
    g_object_get(p, "level", &lv, NULL);
    check("a GObject: property, signal (libffi marshaller)", lv == 42 && seen_level == 42);
    g_object_unref(p);

    GVariant *v = g_variant_ref_sink(g_variant_new("(sia{sv})", "web", 7, NULL));
    char *txt = g_variant_print(v, FALSE);
    check("GVariant", !strcmp(txt, "('web', 7, {})"));
    g_free(txt); g_variant_unref(v);
    GRegex *re = g_regex_new("^(\\w+)://([^/]+)", 0, 0, 0);
    GMatchInfo *mi;
    int m = g_regex_match(re, "https://webkit.org/", 0, &mi);
    char *host = m ? g_match_info_fetch(mi, 2) : 0;
    check("GRegex (PCRE2)", host && !strcmp(host, "webkit.org"));
    g_free(host); g_match_info_free(mi); g_regex_unref(re);
    char *sum256 = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "abc", -1);
    check("SHA-256", !strcmp(sum256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    g_free(sum256);
    GDateTime *now = g_date_time_new_now_utc();
    check("the wall clock (GDateTime)", g_date_time_get_year(now) >= 2026);
    g_date_time_unref(now);
    GError *e = 0;
    char *back = 0;
    gsize len = 0;
    check("files: written atomically, read back", g_file_set_contents("/tmp/wtest-glib.txt", "contents\n", -1, &e) &&
          g_file_get_contents("/tmp/wtest-glib.txt", &back, &len, 0) && len == 9);
    g_free(back);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_data(kf, "[web]\nhome=https://sieos.org\n", -1, 0, 0);
    char *home = g_key_file_get_string(kf, "web", "home", 0);
    check("a key file", home && !strcmp(home, "https://sieos.org"));
    g_free(home); g_key_file_free(kf);

    /* a unix socket pair: a file descriptor (a pipe end) passed with GUnixFDList */
    int sv[2], pp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pipe(pp);
    GSocket *s0 = g_socket_new_from_fd(sv[0], 0), *s1 = g_socket_new_from_fd(sv[1], 0);
    GUnixFDList *fl = g_unix_fd_list_new_from_array((int[]){ pp[1] }, 1);
    GSocketControlMessage *cm = g_unix_fd_message_new_with_fd_list(fl);
    GOutputVector ov = { "x", 1 };
    gssize sent = s0 ? g_socket_send_message(s0, 0, &ov, 1, &cm, 1, 0, 0, &e) : -1;
    GInputVector iv = { (char[8]){ 0 }, 8 };
    GSocketControlMessage **msgs = 0;
    int nm = 0;
    gssize got = s1 ? g_socket_receive_message(s1, 0, &iv, 1, &msgs, &nm, 0, 0, &e) : -1;
    int pfd = -1;
    if (nm == 1 && G_IS_UNIX_FD_MESSAGE(msgs[0])) { int n; int *fds = g_unix_fd_message_steal_fds(G_UNIX_FD_MESSAGE(msgs[0]), &n); if (n == 1) pfd = fds[0]; g_free(fds); }
    char c = 0;
    check("a socket pair passing a descriptor (GUnixFDList)", sent == 1 && got == 1 && pfd >= 0 && write(pfd, "!", 1) == 1 && read(pp[0], &c, 1) == 1 && c == '!');

    /* (as WebKit starts its processes: descriptors inherited, so GLib uses posix_spawn;
     * GLib's "close every other descriptor" spawns need fork, which SIEOS has not) */
    GSubprocess *sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_INHERIT_FDS, &e, "/bin/wtest-glib", "child", NULL);
    if (sp) { g_subprocess_communicate_utf8_async(sp, 0, 0, sub_done, loop); g_main_loop_run(loop); }
    else { printf("wtest glib: (%s)\n", e ? e->message : "?"); check("GSubprocess started", 0); }
    printf("wtest glib: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
