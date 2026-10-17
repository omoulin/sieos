/*
 * json.h - A small JSON reader and writer (a tree of nodes).
 */
#ifndef JSON_H
#define JSON_H

#include "util.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jnode {
    jtype_t t;
    char *key;                      /* in an object: this member's name */
    char *s;                        /* J_STR: the text (UTF-8) */
    double num;                     /* J_NUM, J_BOOL */
    struct jnode *kid, *last, *next;  /* J_ARR, J_OBJ: the members */
} jnode;

jnode *json_parse(const char *text, size_t n);   /* NULL if it is not JSON */
void   json_free(jnode *j);
jnode *json_get(const jnode *obj, const char *key);
jnode *json_path(const jnode *j, const char *path);     /* "meta.title": members, dot separated */
const char *json_str(const jnode *obj, const char *key);  /* NULL unless a string */
double json_num(const jnode *obj, const char *key, double dflt);
int    json_count(const jnode *arr);

jnode *json_new(jtype_t t);
jnode *json_mkstr(const char *s);
jnode *json_mknum(double v);
jnode *json_clone(const jnode *j);
void   json_add(jnode *parent, const char *key, jnode *kid);   /* key: NULL in an array */
void   json_set(jnode *obj, const char *key, jnode *kid);      /* replaces a member of that name */
void   json_del(jnode *obj, const char *key);

void   json_write(buf_t *b, const jnode *j);
void   json_write_str(buf_t *b, const char *s);

#endif
