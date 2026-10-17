/*
 * store.h - The library on disk: plain files, so it moves to any file system.
 *
 *   DATA/lib/LANG/volumes.txt      "code<TAB>title" lines, in library order
 *   DATA/lib/LANG/toc/CODE.txt     the contents: "depth<TAB>uri or -<TAB>title" lines
 *   DATA/lib/LANG/ch/URI.json      a chapter as the content API sent it ('/' -> '~')
 *   DATA/lib/LANG/search.txt       "uri<TAB>anchor<TAB>num<TAB>title<TAB>text": every verse
 *   DATA/notes.json                highlights and notes
 *   DATA/settings.txt              "key=value" lines
 *   DATA/account                   the Church Account session (a Cookie header; mode 600)
 */
#ifndef STORE_H
#define STORE_H

#include "html.h"
#include "json.h"

void store_init(const char *datadir);
const char *store_dir(void);

/* ---- volumes and contents ---- */
typedef struct { char code[32]; char *title; toc_t *toc; } volume_t;
typedef struct { volume_t *v; int n; } volumes_t;
volumes_t *store_volumes(const char *lang);         /* those downloaded (their contents) */
void  store_volumes_free(volumes_t *v);
int   store_save_volume(const char *lang, const char *code, const char *title, const toc_t *toc, const char **order, int norder);
char **store_languages(int *n);                       /* languages with a library */

/* ---- chapters ---- */
typedef struct {
    char *lang, *uri, *title, *audio;
    jnode *json, *footnotes;          /* footnotes: an object of id -> {marker, context, text} */
    doc_t *doc;
} chapter_t;
int        store_has_chapter(const char *lang, const char *uri);
chapter_t *store_chapter(const char *lang, const char *uri);   /* NULL if not downloaded */
void       chapter_free(chapter_t *c);
int        store_save_chapter(const char *lang, const char *uri, const char *json, size_t n);
char      *store_chapter_title(const char *lang, const char *uri);

/* ---- search ---- */
typedef struct { char *uri, *anchor, *title, *snippet; int num; int hl0, hl1; } hit_t;
int   store_rebuild_index(const char *lang);         /* after downloads: every verse, in order */
hit_t *store_search(const char *lang, const char *query, int max, int *n);
void  hits_free(hit_t *h, int n);
const char *store_verse_text(const char *lang, const char *uri, const char *anchor, int *num);

/* ---- notes ---- */
typedef struct { char anchor[24]; int start, end; } span_t;   /* word indexes, -1/-1: the whole paragraph */
typedef struct {
    char *id, *lang, *uri, *anchor, *color, *style, *title, *content;
    span_t *spans; int nspans;
    char **tags; int ntags;
    double created, updated;
    int deleted, dirty;
    jnode *remote;                    /* the server's last copy (kept to send back unknown fields) */
} note_t;

void    notes_lock(void);
void    notes_unlock(void);
note_t **notes_all(int *n);           /* (under the lock) every note, deleted ones included */
note_t *note_find(const char *id);
note_t *note_new(void);               /* added; fill it, then notes_save() */
note_t *note_add_json(const jnode *j); /* added from its JSON form (notes.json's) */
void    note_touch(note_t *n);        /* changed now: dirty, updated */
void    note_delete(note_t *n);       /* marked deleted (removed for good once synced) */
void    note_purge(note_t *n);
void    note_set_tags(note_t *n, const char *comma_list);
char   *note_tags_str(const note_t *n);
int     notes_save(void);
int     notes_version(void);           /* changes whenever the notes do */
int     notes_count_for(const char *uri);

/* ---- settings and account ---- */
const char *setting(const char *key, const char *dflt);
int   setting_int(const char *key, int dflt);
void  setting_set(const char *key, const char *value);
void  setting_set_int(const char *key, int v);
char *account_cookie(void);           /* NULL: not connected */
int   account_save(const char *cookie);
void  account_clear(void);

#endif
