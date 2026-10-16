/*
 * git.h - What git's files share (docs/git.md): output, buffers, files and
 * the configuration (git.c), the object store, the index, trees and refs
 * (odb.c), diffs (diff.c), and the network: fetch and push (proto.c).
 * Everything is in git's own format, so host git reads what we write.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "net.h"
#include "mk/crypto.h"
#include "mk/zlib.h"

typedef uint8_t oid_t[20];                    /* an object's name: SHA-1 of "type size\0" + its content */
enum { OBJ_COMMIT = 1, OBJ_TREE, OBJ_BLOB, OBJ_TAG, OBJ_OFS = 6, OBJ_REF };

/* ---- git.c: output (buffered: one message per 4 KiB), errors, buffers, files */
void out(const void *s, size_t n);
void outf(const char *fmt, ...);
void oflush(void);
int  fail(const char *fmt, ...);               /* "fatal: ..." -> 1 (the exit status) */
typedef struct { char *p; size_t n, cap; } buf_t;
void bput(buf_t *b, const void *p, size_t n);
void bfmt(buf_t *b, const char *fmt, ...);
int  sfmt(char *o, size_t cap, const char *fmt, ...);   /* printf into o -> the length */
void hex(char *o, const uint8_t *id);          /* 40 digits and a 0 */
int  unhex(uint8_t *id, const char *s);        /* 40 digits -> 0, else -1 */
char *get_file(const char *path, size_t *n);   /* a whole file, malloc'd (+ a 0), or 0 */
int  put_file(const char *path, const void *p, size_t n, int mode);   /* parents made -> 0 */
void sort(void *base, int n, size_t size, int (*cmp)(const void *, const void *));
char *cfg_get(const char *key);                /* "user.name", "remote.origin.url": malloc'd, or 0 */
int  cfg_set(const char *file, const char *key, const char *val);

/* ---- odb.c: objects (loose and packed), refs, the index, trees */
void obj_hash(int type, const void *p, size_t n, oid_t id);
int  obj_write(int type, const void *p, size_t n, oid_t id);         /* loose, if new -> 0 */
uint8_t *obj_read(const oid_t id, int *type, size_t *len);             /* malloc'd (+ a 0), or 0 */
int  obj_has(const oid_t id);
const char *type_name(int t);
void odb_reload(void);                         /* (new packs arrived) */
/* a pack received (in pk): its objects named, a thin pack completed, kept
 * as .pack and .idx -> 0 (*count: its objects) */
int  pack_store(buf_t *pk, int *count);
uint8_t *pack_entry(const uint8_t *pack, size_t n, size_t off, int *type, size_t *len, size_t *end);

typedef struct { uint8_t (*id)[20]; int n, cap; } set_t;   /* a set of object names */
int  set_add(set_t *s, const oid_t id);        /* -> 1 if it was new */
int  set_has(const set_t *s, const oid_t id);
/* breadth first through the commits from `from`: the first one in stop -> 0
 * (found), else -1; all those seen are added to seen */
int  walk(const oid_t from, const set_t *stop, set_t *seen, oid_t found);

/* an entry: of a flattened tree, or of the index (with the file's stat) */
typedef struct { char *path; uint32_t mode, size; oid_t id; int64_t mtime; } ent_t;
typedef struct { ent_t *e; int n, cap; } ents_t;
ent_t *ents_find(const ents_t *l, const char *path);
ent_t *ents_put(ents_t *l, const char *path);  /* found, or a new one in its place (sorted) */
void ents_del(ents_t *l, ent_t *e);
int  tree_flat(const oid_t tree, ents_t *out); /* every file under it, sorted by path -> 0 */
int  tree_write(const ents_t *l, oid_t out);   /* the trees of these files -> 0 */
int  index_read(ents_t *ix);
int  index_write(const ents_t *ix);

/* commits */
typedef struct { oid_t tree, parent[2]; int np; char *author, *committer, *msg; int64_t time; uint8_t *raw; } commit_t;
int  commit_read(const oid_t id, commit_t *c); /* -> 0 (free c->raw after) */
int  commit_tree(const oid_t id, oid_t tree);  /* a commit's tree -> 0 */
int  merge_base(const oid_t a, const oid_t b, oid_t base);   /* -> 0, or -1: none */
int  is_ancestor(const oid_t a, const oid_t b);              /* a is b or one of its ancestors */

/* refs: "HEAD", "refs/heads/main" (loose files, then packed-refs) */
int  ref_read(const char *name, oid_t id);     /* (HEAD followed) -> 0, or -1 */
int  ref_write(const char *name, const oid_t id);
int  ref_delete(const char *name);
const char *head_ref(void);                    /* HEAD's ref ("refs/heads/main"), or 0: detached */
int  rev_parse(const char *s, oid_t id);       /* HEAD, a branch, a hash (or its start), ~N, ^ */
typedef void ref_fn(const char *name, const oid_t id, void *ctx);
void ref_each(const char *prefix, ref_fn *fn, void *ctx);   /* every ref under prefix, sorted */

/* ---- diff.c: unified diffs of two texts */
void diff_text(const char *path, const uint8_t *a, size_t an, const oid_t ida, uint32_t ma,
               const uint8_t *b, size_t bn, const oid_t idb, uint32_t mb);

/* ---- proto.c: git:// and smart HTTP(S), protocol v0 */
/* the remote's branches into refs/remotes/<remote>/ (and its HEAD's branch
 * into head, "refs/heads/main") -> 0 */
int  fetch(const char *remote, const char *url, char *head, size_t cap);
int  push(const char *url, const char *ref);  /* our ref (refs/heads/x) to the same name there */
