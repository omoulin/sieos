/*
 * html.h - Chapter HTML (from churchofjesuschrist.org's content API) into
 * paragraphs of styled runs, and a volume's contents page into a tree.
 *
 * A run is "countable" when Gospel Library counts its words for highlight
 * offsets: everything but verse numbers and superscripts (footnote letters).
 */
#ifndef HTML_H
#define HTML_H

enum {                                   /* run styles (bits) */
    ST_ITALIC = 1, ST_BOLD = 2, ST_SMALLCAPS = 4, ST_SUP = 8, ST_SUB = 16,
    ST_VERSENUM = 32, ST_MARKER = 64, ST_LINK = 128, ST_FOOTNOTE = 256, ST_UNDERLINE = 512,
};
enum {                                   /* paragraph kinds */
    BK_P, BK_VERSE, BK_TITLE, BK_SUBTITLE, BK_INTRO, BK_TITLENUM, BK_SUMMARY, BK_HEADING, BK_LABEL,
};

typedef struct {
    char *text;                          /* UTF-8, whitespace collapsed */
    int style;
    char *note;                          /* footnote id (ST_FOOTNOTE, ST_MARKER) */
    char *href;                          /* a link (ST_LINK) */
    int countable;
} run_t;

typedef struct {
    int kind;
    char *anchor;                        /* the paragraph's id ("p7") when it can be highlighted */
    int num;                             /* the verse number, or 0 */
    run_t *runs;
    int nruns;
} block_t;

typedef struct { block_t *b; int n; } doc_t;

doc_t *html_doc(const char *html);       /* a chapter body, or a footnote's text */
void   doc_free(doc_t *d);
char  *doc_text(const block_t *b, int countable_only);   /* the paragraph as plain text */

typedef struct toc {
    char *title, *uri;                   /* uri: NULL for a group (a book, a section) */
    struct toc *kids;
    int nkids;
} toc_t;

toc_t *html_toc(const char *html);       /* the root (a group) */
void   toc_free(toc_t *t);

/* A link: in this library ("/scriptures/...": *lang, *uri, *anchor set, 1) or
 * elsewhere (0, *url: an absolute address). The strings are malloc'd. */
int link_resolve(const char *href, const char *dflt_lang, char **lang, char **uri, char **anchor, char **url);
char *uri_normalize(const char *href);   /* "/study/scriptures/x?lang=eng#p1" -> "/scriptures/x" */

#endif
