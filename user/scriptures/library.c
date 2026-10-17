/*
 * library.c - Downloads and notes sync (library.h).
 *
 * Content: GET /study/api/v3/language-pages/type/content?lang=L&uri=U (a page, as
 * JSON) and .../type/dynamic (a volume's contents). Notes: the Gospel Library
 * notes service (/notes/api/v3, else v2), which is not a public API: the shape
 * here is what its web app uses - highlight.content[] = {uri: "/scriptures/x/1.p3",
 * color, style, startOffset, endOffset}, 1-based word offsets, -1: the whole paragraph.
 */
#include "library.h"
#include "net.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const volinfo_t VOLUMES[] = {
    { "ot", "Old Testament", 1 }, { "nt", "New Testament", 1 }, { "bofm", "Book of Mormon", 1 },
    { "dc-testament", "Doctrine and Covenants", 1 }, { "pgp", "Pearl of Great Price", 1 },
    { "tg", "Topical Guide", 0 }, { "bd", "Bible Dictionary", 0 }, { "gs", "Guide to the Scriptures", 0 },
    { "jst", "Joseph Smith Translation", 0 },
};
const int NVOLUMES = COUNT(VOLUMES);

static void say(job_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(job_t *j, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&j->mu);
    va_start(ap, fmt);
    vsnprintf(j->message, sizeof j->message, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&j->mu);
    if (j->wake) j->wake();
}
static void logln(job_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logln(job_t *j, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&j->mu);
    size_t n = strlen(j->log);
    snprintf(j->log + n, sizeof j->log - n, "%s\n", line);
    snprintf(j->message, sizeof j->message, "%.255s", line);
    pthread_mutex_unlock(&j->mu);
    if (j->wake) j->wake();
}

/* Requests to the content API, at most one every 150 ms across threads. */
static pthread_mutex_t pace_mu = PTHREAD_MUTEX_INITIALIZER;
static double pace_next;
static void pace(void)
{
    pthread_mutex_lock(&pace_mu);
    double now = now_s(), wait = pace_next - now;
    pace_next = MAX(now, pace_next) + 0.15;
    pthread_mutex_unlock(&pace_mu);
    if (wait > 0) usleep((useconds_t)(wait * 1e6));
}

static int api_get(const char *type, const char *lang, const char *uri, http_t *h)
{
    char *u = url_escape(uri);
    char *url = xprintf(SITE "/study/api/v3/language-pages/type/%s?lang=%s&uri=%s", type, lang, u);
    int r = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        pace();
        r = http_do("GET", url, NULL, NULL, h);
        if (h->status == 200 || h->status == 404 || h->status == 401 || h->status == 403) break;
        http_free(h);
        sleep(1 + attempt);
    }
    free(url);
    free(u);
    return h->status == 200 ? 0 : r ? r : -1;
}

int fetch_chapter(const char *lang, const char *uri, char **err)
{
    http_t h;
    if (api_get("content", lang, uri, &h)) {
        if (err) *err = h.status ? xprintf("HTTP %d", h.status) : xstrdup(h.err ? h.err : "no answer");
        http_free(&h);
        return -1;
    }
    jnode *j = json_parse(h.body, h.len);
    int ok = j && json_path(j, "content.body");
    json_free(j);
    int r = ok ? store_save_chapter(lang, uri, h.body, h.len) : -1;
    if (!ok && err) *err = xstrdup("not a page of the library");
    http_free(&h);
    return r;
}

/* ---- downloads ----------------------------------------------------------------------- */

typedef struct {
    job_t *j;
    char lang[16];
    char **codes; int ncodes;
    char **todo; int ntodo, next;
    pthread_mutex_t qmu;
} dl_t;

static void collect(dl_t *d, const toc_t *t)
{
    for (int i = 0; i < t->nkids; i++) {
        if (t->kids[i].uri && !store_has_chapter(d->lang, t->kids[i].uri)) {
            int dup = 0;
            for (int k = 0; k < d->ntodo && !dup; k++) dup = !strcmp(d->todo[k], t->kids[i].uri);
            if (!dup) { d->todo = xrealloc(d->todo, (d->ntodo + 1) * sizeof *d->todo); d->todo[d->ntodo++] = xstrdup(t->kids[i].uri); }
        }
        collect(d, &t->kids[i]);
    }
}

static void *dl_worker(void *arg)
{
    dl_t *d = arg;
    for (;;) {
        pthread_mutex_lock(&d->qmu);
        int i = d->j->cancel ? d->ntodo : d->next++;
        pthread_mutex_unlock(&d->qmu);
        if (i >= d->ntodo) return NULL;
        int r = fetch_chapter(d->lang, d->todo[i], NULL);
        pthread_mutex_lock(&d->j->mu);
        d->j->done++;
        d->j->failed += r != 0;
        snprintf(d->j->message, sizeof d->j->message, "%d of %d pages", d->j->done, d->j->total);
        pthread_mutex_unlock(&d->j->mu);
        if (d->j->wake) d->j->wake();
    }
}

static void *dl_main(void *arg)
{
    dl_t *d = arg;
    job_t *j = d->j;
    const char *order[32];
    for (int i = 0; i < NVOLUMES; i++) order[i] = VOLUMES[i].code;
    for (int c = 0; c < d->ncodes && !j->cancel; c++) {
        say(j, "Reading the contents of %s…", d->codes[c]);
        char *uri = xprintf("/scriptures/%s", d->codes[c]);
        http_t h;
        int r = api_get("dynamic", d->lang, uri, &h);
        free(uri);
        jnode *js = r ? NULL : json_parse(h.body, h.len);
        const char *body = json_str(json_path(js, "content.content"), "body");
        const char *title = json_str(json_path(js, "content.meta"), "title");
        if (!body) {
            say(j, "Could not read the contents of %s (%s)", d->codes[c], h.status ? "not in this language?" : h.err ? h.err : "no answer");
            j->failed++;
        } else {
            toc_t *toc = html_toc(body);
            store_save_volume(d->lang, d->codes[c], title ? title : d->codes[c], toc, order, NVOLUMES);
            collect(d, toc);
            toc_free(toc);
        }
        json_free(js);
        http_free(&h);
    }
    pthread_mutex_lock(&j->mu);
    j->total = d->ntodo;
    pthread_mutex_unlock(&j->mu);
    say(j, "Downloading %d pages…", d->ntodo);
    pthread_t w[4];
    for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, dl_worker, d);
    for (int i = 0; i < 4; i++) pthread_join(w[i], NULL);
    say(j, "Building the search index…");
    store_rebuild_index(d->lang);
    if (j->cancel) say(j, "Cancelled: %d of %d pages downloaded", j->done - j->failed, j->total);
    else if (j->failed) say(j, "Downloaded %d of %d pages (%d failed: download again to retry)", j->done - j->failed, j->total, j->failed);
    else say(j, "Downloaded %d pages", j->total);
    for (int i = 0; i < d->ntodo; i++) free(d->todo[i]);
    for (int i = 0; i < d->ncodes; i++) free(d->codes[i]);
    free(d->todo); free(d->codes); free(d);
    j->running = 0;
    if (j->wake) j->wake();
    return NULL;
}

int download_start(job_t *j, const char *lang, const char **codes, int ncodes)
{
    if (j->running) return -1;
    dl_t *d = xcalloc(1, sizeof *d);
    d->j = j;
    snprintf(d->lang, sizeof d->lang, "%s", lang);
    d->codes = xcalloc(ncodes, sizeof *d->codes);
    for (int i = 0; i < ncodes; i++) d->codes[i] = xstrdup(codes[i]);
    d->ncodes = ncodes;
    pthread_mutex_init(&d->qmu, NULL);
    j->running = 1; j->cancel = 0; j->total = j->done = j->failed = 0; j->log[0] = 0;
    say(j, "Starting…");
    pthread_t t;
    pthread_create(&t, NULL, dl_main, d);
    pthread_detach(t);
    return 0;
}

/* ---- notes sync ------------------------------------------------------------------------- */

static const char *BASES[] = { SITE "/notes/api/v3", SITE "/notes/api/v2" };
#define TYPES "highlight,journal,reference"

int session_check(const char *cookie, char **err)
{
    for (int i = 0; i < COUNT(BASES); i++) {
        char *url = xprintf("%s/annotations?start=1&numberToReturn=1&type=" TYPES, BASES[i]);
        http_t h;
        http_do("GET", url, cookie, NULL, &h);
        free(url);
        int st = h.status;
        if (!st && err) *err = xstrdup(h.err ? h.err : "no answer");
        http_free(&h);
        if (st == 200) return 1;
        if (st == 401 || st == 403) return 0;
        if (!st) return -1;
    }
    return 0;
}

static double iso_time(const char *s)
{
    if (!s) return now_s();
    struct tm tm = {0};
    int ms = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d.%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) < 6)
        return now_s();
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (double)timegm(&tm) + ms / 1000.0;
}
static void iso_write(buf_t *b, double t)
{
    time_t s = (time_t)t;
    struct tm tm;
    gmtime_r(&s, &tm);
    buf_printf(b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec, (int)((t - s) * 1000));
}

/* A server annotation into our note's JSON form (bookmarks: NULL). */
static jnode *remote_to_local(const jnode *a)
{
    const char *type = json_str(a, "type");
    if (type && !strcmp(type, "bookmark")) return NULL;
    const char *id = json_str(a, "id") ? json_str(a, "id") : json_str(a, "annotationId");
    if (!id) return NULL;
    buf_t b = {0};
    char uri[512] = "", color[32] = "", style[32] = "";
    buf_t spans = {0};
    /* v2: highlight.content[]; v3: highlights[] (and the chapter's uri at the top) */
    jnode *hl = json_path(a, "highlight.content");
    if (!hl) hl = json_get(a, "highlights");
    for (jnode *h = hl ? hl->kid : NULL; h; h = h->next) {
        const char *u = json_str(h, "uri");
        const char *dot = u ? strrchr(u, '.') : NULL;
        if (!dot || strchr(dot, '/')) continue;
        if (!*uri) snprintf(uri, sizeof uri, "%.*s", (int)(dot - u), u);
        if (strncmp(u, uri, dot - u) || strlen(uri) != (size_t)(dot - u)) continue;   /* (other chapters: dropped) */
        int s = (int)json_num(h, "startOffset", -1), e = (int)json_num(h, "endOffset", -1);
        if (spans.n) buf_putc(&spans, ',');
        buf_printf(&spans, "{\"anchor\":");
        json_write_str(&spans, dot + 1);
        buf_printf(&spans, ",\"start\":%d,\"end\":%d}", s > 0 ? s - 1 : -1, e > 0 ? e - 1 : -1);
        if (!*color && json_str(h, "color")) snprintf(color, sizeof color, "%s", json_str(h, "color"));
        if (!*style && json_str(h, "style")) snprintf(style, sizeof style, "%s", json_str(h, "style"));
    }
    if (!*uri && json_str(a, "uri")) {                  /* (no highlight: the note's own place) */
        const char *u = json_str(a, "uri"), *dot = strrchr(u, '.');
        size_t n = dot && !strchr(dot, '/') ? (size_t)(dot - u) : strlen(u);
        snprintf(uri, sizeof uri, "%.*s", (int)n, u);
    }
    double updated = iso_time(json_str(a, "lastUpdated"));
    const char *status = json_str(a, "status");
    buf_puts(&b, "{\"id\":"); json_write_str(&b, id);
    buf_puts(&b, ",\"lang\":"); json_write_str(&b, json_str(a, "locale") ? json_str(a, "locale") : "eng");
    buf_puts(&b, ",\"uri\":"); json_write_str(&b, uri);
    if (spans.n) {
        const char *q = strstr(spans.p, "\"anchor\":\"");
        if (q) { buf_puts(&b, ",\"anchor\":"); q += 10; buf_putc(&b, '"'); buf_add(&b, q, strcspn(q, "\"")); buf_putc(&b, '"'); }
    }
    buf_printf(&b, ",\"spans\":[%s]", spans.p ? spans.p : "");
    if (*color) { buf_puts(&b, ",\"color\":"); json_write_str(&b, color); }
    if (*style) { buf_puts(&b, ",\"style\":"); json_write_str(&b, style); }
    jnode *note = json_get(a, "note");
    buf_puts(&b, ",\"title\":"); json_write_str(&b, json_str(note, "title") ? json_str(note, "title") : "");
    buf_puts(&b, ",\"content\":"); json_write_str(&b, json_str(note, "content") ? json_str(note, "content") : "");
    buf_puts(&b, ",\"tags\":[");
    int nt = 0;
    jnode *tags = json_get(a, "tags");
    for (jnode *t = tags ? tags->kid : NULL; t; t = t->next) {
        const char *name = t->t == J_STR ? t->s : json_str(t, "name");
        if (!name || !*name) continue;
        if (nt++) buf_putc(&b, ',');
        json_write_str(&b, name);
    }
    buf_printf(&b, "],\"created_at\":%.3f,\"updated_at\":%.3f,\"deleted\":%d,\"dirty\":0,\"remote\":",
               json_str(a, "created") ? iso_time(json_str(a, "created")) : updated, updated,
               status && (!strcmp(status, "trashed") || !strcmp(status, "deleted")));
    json_write(&b, a);
    buf_puts(&b, "}");
    jnode *j = json_parse(b.p, b.n);
    buf_free(&b);
    buf_free(&spans);
    return j;
}

/* Our note back into the server's shape, keeping the fields we do not know. */
static jnode *local_to_remote(const note_t *n, int v3)
{
    jnode *a = n->remote ? json_clone(n->remote) : json_new(J_OBJ);
    if (n->remote) v3 = json_get(a, "highlights") || json_get(a, "annotationId");   /* (the shape it came in) */
    json_set(a, v3 ? "annotationId" : "id", json_mkstr(n->id));
    json_set(a, "locale", json_mkstr(n->lang));
    buf_t t = {0};
    iso_write(&t, n->updated);
    json_set(a, "lastUpdated", json_mkstr(t.p));
    buf_free(&t);
    int dict_tags = 0;
    jnode *old = json_get(a, "tags");
    for (jnode *k = old ? old->kid : NULL; k; k = k->next) dict_tags |= k->t == J_OBJ;
    jnode *tags = json_new(J_ARR);
    for (int i = 0; i < n->ntags; i++) {
        if (dict_tags) { jnode *o = json_new(J_OBJ); json_add(o, "name", json_mkstr(n->tags[i])); json_add(tags, NULL, o); }
        else json_add(tags, NULL, json_mkstr(n->tags[i]));
    }
    json_set(a, "tags", tags);
    if (!json_get(a, "folders")) json_add(a, "folders", json_new(J_ARR));
    if (*n->title || *n->content) {
        jnode *note = json_new(J_OBJ);
        json_add(note, "title", json_mkstr(n->title));
        json_add(note, "content", json_mkstr(n->content));
        json_set(a, "note", note);
    } else json_del(a, "note");
    if (n->nspans && *n->uri) {
        jnode *oldc = v3 ? json_get(a, "highlights") : json_path(a, "highlight.content"), *content = json_new(J_ARR);
        for (int i = 0; i < n->nspans; i++) {
            char *uri = xprintf("%s.%s", n->uri, n->spans[i].anchor);
            jnode *h = NULL;
            for (jnode *k = oldc ? oldc->kid : NULL; k && !h; k = k->next)
                if (json_str(k, "uri") && !strcmp(json_str(k, "uri"), uri)) h = json_clone(k);
            if (!h) h = json_new(J_OBJ);
            json_set(h, "uri", json_mkstr(uri));
            json_set(h, "color", json_mkstr(n->color ? n->color : "clear"));
            json_set(h, "style", json_mkstr(n->style ? n->style : ""));
            json_set(h, "startOffset", json_mknum(n->spans[i].start >= 0 ? n->spans[i].start + 1 : -1));
            json_set(h, "endOffset", json_mknum(n->spans[i].end >= 0 ? n->spans[i].end + 1 : -1));
            json_add(content, NULL, h);
            free(uri);
        }
        if (v3) {
            json_set(a, "highlights", content);
            json_set(a, "uri", json_mkstr(n->uri));
        } else {
            jnode *hl = json_new(J_OBJ);
            json_add(hl, "content", content);
            json_set(a, "highlight", hl);
        }
        json_set(a, "type", json_mkstr(json_get(a, "refs") ? "reference" : "highlight"));
    } else {
        json_set(a, "type", json_mkstr("journal"));
        json_del(a, "highlight");
        json_del(a, "highlights");
    }
    return a;
}

/* Notes pulled before the v3 shape was understood: rebuilt from the server copy each one
 * kept (notes changed here since are left alone). Returns how many were repaired. */
int notes_repair(void)
{
    int fixed = 0, n;
    notes_lock();
    note_t **all = notes_all(&n);
    note_t **todo = xcalloc(n + 1, sizeof *todo);
    int nt = 0;
    for (int i = 0; i < n; i++)
        if (!*all[i]->uri && all[i]->remote && !all[i]->dirty && json_str(all[i]->remote, "uri")) todo[nt++] = all[i];
    for (int i = 0; i < nt; i++) {
        jnode *loc = remote_to_local(todo[i]->remote);
        if (!loc || !*json_str(loc, "uri")) { json_free(loc); continue; }
        note_purge(todo[i]);
        note_add_json(loc);
        json_free(loc);
        fixed++;
    }
    free(todo);
    if (fixed) notes_save();
    notes_unlock();
    return fixed;
}

typedef struct { job_t *j; int push; } sync_t;

static void *sync_main(void *arg)
{
    sync_t *s = arg;
    job_t *j = s->j;
    char *cookie = account_cookie();
    const char *base = NULL;
    int pulled = 0, created = 0, updated = 0, kept = 0, skipped = 0, pushed = 0, deleted = 0, errors = 0;
    if (!cookie) { logln(j, "Not connected."); goto out; }
    for (int i = 0; i < COUNT(BASES) && !base; i++) {
        char *url = xprintf("%s/annotations?start=1&numberToReturn=1&type=" TYPES, BASES[i]);
        http_t h;
        http_do("GET", url, cookie, NULL, &h);
        free(url);
        if (h.status == 200) base = BASES[i];
        else if (h.status == 401 || h.status == 403) {
            j->auth_error = 1;
            logln(j, "The Church Account session was refused (expired). Sign in again.");
            http_free(&h);
            goto out;
        } else if (!h.status) { logln(j, "No answer: %s", h.err ? h.err : "?"); http_free(&h); goto out; }
        http_free(&h);
    }
    if (!base) { logln(j, "The notes service did not answer."); goto out; }
    logln(j, "Connected to %s", base + strlen(SITE));

    for (int start = 1;; start += 500) {
        char *url = xprintf("%s/annotations?start=%d&numberToReturn=500&type=" TYPES "&notesAsHtml=false", base, start);
        http_t h;
        http_do("GET", url, cookie, NULL, &h);
        free(url);
        jnode *page = h.status == 200 ? json_parse(h.body, h.len) : NULL;
        http_free(&h);
        if (page && page->t == J_OBJ) {
            jnode *inner = json_get(page, "annotations") ? json_get(page, "annotations") : json_get(page, "items");
            jnode *copy = inner ? json_clone(inner) : NULL;
            json_free(page);
            page = copy;
        }
        int n = json_count(page);
        notes_lock();
        for (jnode *a = page ? page->kid : NULL; a; a = a->next) {
            jnode *loc = remote_to_local(a);
            if (!loc) { skipped++; continue; }
            pulled++;
            note_t *mine = note_find(json_str(loc, "id"));
            if (mine && mine->dirty && mine->updated > json_num(loc, "updated_at", 0)) { kept++; json_free(loc); continue; }
            if (mine) { note_purge(mine); updated++; } else created++;
            note_add_json(loc);
            json_free(loc);
        }
        notes_save();
        notes_unlock();
        json_free(page);
        if (n < 500) break;
    }
    logln(j, "Pulled %d annotations (%d new, %d updated, %d kept local, %d skipped)", pulled, created, updated, kept, skipped);

    if (s->push) {
        notes_lock();
        int nn;
        note_t **all = notes_all(&nn);
        /* (copy the list: purging changes it) */
        note_t **dirty = xcalloc(nn + 1, sizeof *dirty);
        int nd = 0;
        for (int i = 0; i < nn; i++) if (all[i]->dirty) dirty[nd++] = all[i];
        for (int i = 0; i < nd; i++) {
            note_t *n = dirty[i];
            char *esc = url_escape(n->id), *url = xprintf("%s/annotations/%s", base, esc);
            http_t h;
            if (n->deleted) {
                if (n->remote) http_do("DELETE", url, cookie, NULL, &h); else { memset(&h, 0, sizeof h); h.status = 200; }
                if (h.status / 100 == 2 || h.status == 404) { note_purge(n); deleted++; }
                else { errors++; logln(j, "Delete failed for %s: HTTP %d", n->id, h.status); }
            } else {
                jnode *a = local_to_remote(n, strstr(base, "/v3") != NULL);
                buf_t body = {0};
                json_write(&body, a);
                http_do("PUT", url, cookie, body.p, &h);
                if (h.status == 404 || h.status == 405) {      /* (new on the server: POST a list) */
                    http_free(&h);
                    buf_t list = {0};
                    buf_puts(&list, "[");
                    buf_add(&list, body.p, body.n);
                    buf_puts(&list, "]");
                    char *purl = xprintf("%s/annotations", base);
                    http_do("POST", purl, cookie, list.p, &h);
                    free(purl);
                    buf_free(&list);
                }
                if (h.status / 100 == 2) { n->dirty = 0; json_free(n->remote); n->remote = a; a = NULL; pushed++; }
                else { errors++; logln(j, "Upload failed for %s: HTTP %d %.120s", n->id, h.status, h.body ? h.body : ""); }
                json_free(a);
                buf_free(&body);
            }
            http_free(&h);
            free(url); free(esc);
        }
        free(dirty);
        notes_save();
        notes_unlock();
        logln(j, "Pushed %d, deleted %d, errors %d", pushed, deleted, errors);
    }
    {
        char v[160];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char when[64];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        snprintf(v, sizeof v, "%s: %d pulled, %d pushed%s", when, pulled, pushed, errors ? ", with errors" : "");
        setting_set("last_sync", v);
    }
out:
    free(cookie);
    free(s);
    j->running = 0;
    if (j->wake) j->wake();
    return NULL;
}

int sync_start(job_t *j, int push)
{
    if (j->running) return -1;
    sync_t *s = xcalloc(1, sizeof *s);
    s->j = j;
    s->push = push;
    j->running = 1; j->cancel = 0; j->auth_error = 0; j->log[0] = 0; j->message[0] = 0;
    pthread_t t;
    pthread_create(&t, NULL, sync_main, s);
    pthread_detach(t);
    return 0;
}
