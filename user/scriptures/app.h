/*
 * app.h - The application's state, shared by app.c (tabs, the reader, input,
 * the main loop) and panels.c (the library sidebar, popovers, dialogs).
 */
#ifndef APP_H
#define APP_H

#include "draw.h"
#include "layout.h"
#include "library.h"
#include "plat.h"
#include "store.h"
#include "ui.h"

#define MAXTABS 24
#define MAXHIST 64

typedef struct { char lang[16]; char *uri; char *anchor; } place_t;

typedef struct {
    char lang[16];
    char *uri;                    /* NULL: a new, empty tab */
    char *title;
    chapter_t *ch;                /* loaded when the tab is first shown */
    layout_t *lay;
    int lay_w, lay_px;
    int scroll;
    char *goto_anchor;            /* scroll there once laid out */
    char *flash_anchor; uint32_t flash_until;
    int loading;
    char *error;
    place_t hist[MAXHIST]; int nhist;
    int sel0, sel1;               /* the selection (offsets), sel0 == sel1: none */
} tab_t;

/* The sidebar's pages: a stack. */
enum { PG_ROOT, PG_GROUP, PG_SEARCH };
typedef struct { int kind; const toc_t *node; char *title; hit_t *hits; int nhits; int scroll, content_h; } page_t;

/* What clicks land on (zone ids). */
enum {
    Z_NONE, Z_APPTAB, Z_TAB, Z_TABCLOSE, Z_NEWTAB, Z_BACK, Z_PREV, Z_NEXT, Z_TOP, Z_SETTINGS,
    Z_READER, Z_SCROLLBAR, Z_SIDE, Z_SIDEBACK, Z_VOLUME, Z_GROUP, Z_CHAPTER, Z_HIT, Z_NOTESPAGE,
    Z_FIELD, Z_SWATCH, Z_POP_NOTE, Z_POP_COPY, Z_FNLINK, Z_POPUP, Z_SHADE,
    Z_DLG, Z_N_SWATCH, Z_N_SAVE, Z_N_CANCEL, Z_N_DELETE,
    Z_S_SECTION, Z_S_LOOK, Z_S_SIZE, Z_S_TEXT, Z_S_LANG, Z_S_LANGCODE, Z_S_VOL, Z_S_DOWNLOAD, Z_S_CANCEL,
    Z_NV_REF, Z_NV_GO, Z_NV_CARD, Z_NV_TAG, Z_NV_SORT, Z_NV_CLEAR,
    Z_S_SIGNIN, Z_S_FIREFOX, Z_S_PASTE, Z_S_PASTECLIP, Z_S_DISCONNECT, Z_S_PULL, Z_S_PUSH, Z_S_CLOSE, Z_DOWNLOADS,
};

enum { POP_NONE, POP_FOOTNOTE, POP_SELECTION };
enum { DLG_NONE, DLG_NOTE, DLG_SETTINGS };
enum { SEC_LOOK, SEC_LIBRARY, SEC_ACCOUNT, SEC_ABOUT, NSEC };

typedef struct {
    int W, H;
    tab_t *tabs[MAXTABS]; int ntabs, cur;
    char lang[16];
    volumes_t *vols;
    page_t pages[16]; int npages;
    int sidebar;                  /* shown */
    int reader_px;
    /* where things are (this frame) */
    int top_h, side_x0, side_x1, read_x0, read_x1, read_y0, read_y1, text_x0, text_w;
    /* popovers */
    int pop; int pop_x, pop_y;
    char *fn_id; layout_t *fn_lay; doc_t *fn_doc; int fn_x0, fn_y0;
    /* dialogs */
    int dlg; int dlg_x0, dlg_y0, dlg_x1, dlg_y1;
    char *note_id; int note_color; char *note_quote, *note_ref;
    field_t note_title, note_tags, note_body;
    int section; int confirm_push;
    field_t search, langcode, cookie, nv_filter;
    int notes_gen;                /* bumped when notes change (the notes view rebuilds) */
    field_t *focus;
    int vol_pick[16];
    /* jobs */
    job_t dl, sync;
    int dl_was, sync_was;
    int watching; uint32_t watch_since, watch_next; volatile int watch_busy;
    char toast[200]; uint32_t toast_until;
    int busy;                     /* importing the old library */
    int dirty;
} app_t;

extern app_t A;

/* app.c */
tab_t *cur_tab(void);
void open_place(const char *lang, const char *uri, const char *anchor, int new_tab, int history);
void tab_new(int select);
void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void reload_volumes(void);
void set_lang(const char *lang);
void refresh_notes(void);
void follow_link(const char *href, int new_tab);
void save_session(void);
void jump_to(const char *lang, const char *uri, const char *anchor, int background);

#define NOTES_URI "notes:"         /* the uri of a tab showing every note */
int  is_notes_tab(const tab_t *t);
int  order_of(const char *uri);    /* place in the library's reading order, or -1 */
const char *leaf_title(const char *uri);

/* notesview.c */
void open_notes_tab(void);
void draw_notes_view(tab_t *t, int x0, int y0, int x1, int y1);
void notes_view_click(const zone_t *z, int ctrl);

/* panels.c */
void draw_sidebar(int x0, int y0, int x1, int y1);
void side_click(const zone_t *z, int button, int ctrl);
void side_push(int kind, const toc_t *node, const char *title);
void side_search(const char *q);
void draw_popover(void);
void popover_click(const zone_t *z, int ctrl);
void open_footnote(tab_t *t, const char *id, int x, int y);
void close_popover(void);
void draw_dialog(void);
void dialog_click(const zone_t *z, int clicks);
void open_note(const char *id);
void open_settings(int section);
void close_dialog(void);
void jobs_tick(void);
void signin_start(void);
int  note_color_index(const char *color);
extern const char *COLOR_NAMES[8];

#endif
