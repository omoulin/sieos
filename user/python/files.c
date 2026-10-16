/*
 * files.c - Files and folders for Python on SIEOS: open() and its file
 * objects, and the os module, on the file server (user/lib/fs.c).
 *
 * A file object keeps the file server's handle and its own position (the
 * server's reads and writes say where). Small reads, as readline() makes,
 * are served from a 512-byte buffer; writes go to the server at once.
 * Errors become OSError with the POSIX number (os.stat('x'): ENOENT).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "mk.h"

static long check(long e) { if (e < 0) mp_raise_OSError((int)-e); return e; }

/* ---- files */
typedef struct {
    mp_obj_base_t base;
    long h;                         /* -1: closed */
    uint64_t pos, boff;             /* the position; the buffer holds the bytes from boff */
    uint32_t blen;
    uint8_t buf[512];
} file_t;

static mp_uint_t file_read(mp_obj_t self, void *out, mp_uint_t size, int *err)
{
    file_t *f = MP_OBJ_TO_PTR(self);
    long n;
    if (f->h < 0) { *err = MP_EBADF; return MP_STREAM_ERROR; }
    if (f->pos < f->boff || f->pos >= f->boff + f->blen) {          /* not in the buffer */
        if (size >= sizeof f->buf) {
            if ((n = fs_read(f->h, f->pos, out, size)) < 0) { *err = (int)-n; return MP_STREAM_ERROR; }
            f->pos += (uint64_t)n;
            return (mp_uint_t)n;
        }
        if ((n = fs_read(f->h, f->pos, f->buf, sizeof f->buf)) <= 0) {
            if (n < 0) { *err = (int)-n; return MP_STREAM_ERROR; }
            return 0;
        }
        f->boff = f->pos, f->blen = (uint32_t)n;
    }
    mp_uint_t k = MIN(size, (mp_uint_t)(f->boff + f->blen - f->pos));
    memcpy(out, f->buf + (f->pos - f->boff), k);
    f->pos += k;
    return k;
}

static mp_uint_t file_write(mp_obj_t self, const void *in, mp_uint_t size, int *err)
{
    file_t *f = MP_OBJ_TO_PTR(self);
    long n = f->h < 0 ? -MP_EBADF : fs_write(f->h, f->pos, in, size);
    if (n < 0) { *err = (int)-n; return MP_STREAM_ERROR; }
    f->pos += (uint64_t)n;
    f->blen = 0;
    return (mp_uint_t)n;
}

static mp_uint_t file_ioctl(mp_obj_t self, mp_uint_t req, uintptr_t arg, int *err)
{
    file_t *f = MP_OBJ_TO_PTR(self);
    if (req == MP_STREAM_CLOSE) {
        if (f->h >= 0) fs_close(f->h);
        f->h = -1;
        return 0;
    }
    if (f->h < 0) { *err = MP_EBADF; return MP_STREAM_ERROR; }
    if (req == MP_STREAM_FLUSH) return 0;
    if (req == MP_STREAM_SEEK) {
        struct mp_stream_seek_t *s = (void *)arg;
        siefs_stat_t st;
        int64_t base = s->whence == 1 ? (int64_t)f->pos : 0;
        long e = s->whence == 2 ? fs_fstat(f->h, &st) : 0;
        if (e < 0) { *err = (int)-e; return MP_STREAM_ERROR; }
        if (s->whence == 2) base = (int64_t)st.size;
        if (base + s->offset < 0) { *err = MP_EINVAL; return MP_STREAM_ERROR; }
        s->offset = (mp_off_t)(f->pos = (uint64_t)(base + s->offset));
        return 0;
    }
    *err = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static void file_print(const mp_print_t *print, mp_obj_t self, mp_print_kind_t kind)
{
    (void)kind;
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self), (int)((file_t *)MP_OBJ_TO_PTR(self))->h);
}

static const mp_rom_map_elem_t file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals, file_locals_table);
static const mp_stream_p_t bin_stream = { .read = file_read, .write = file_write, .ioctl = file_ioctl };
static const mp_stream_p_t text_stream = { .read = file_read, .write = file_write, .ioctl = file_ioctl, .is_text = true };
static MP_DEFINE_CONST_OBJ_TYPE(file_bin, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
                                print, file_print, protocol, &bin_stream, locals_dict, &file_locals);
static MP_DEFINE_CONST_OBJ_TYPE(file_text, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM,
                                print, file_print, protocol, &text_stream, locals_dict, &file_locals);

/* open(file, mode='r'): r, w, a, x, + and b as in Python (text is UTF-8, no newline translation) */
static mp_obj_t py_open(size_t n, const mp_obj_t *args, mp_map_t *kw)
{
    mp_map_elem_t *m = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_mode), MP_MAP_LOOKUP);
    const char *mode = n > 1 ? mp_obj_str_get_str(args[1]) : m ? mp_obj_str_get_str(m->value) : "r";
    int flags = FS_RDONLY;
    const mp_obj_type_t *type = &file_text;
    for (; *mode; mode++)
        switch (*mode) {
        case 'w': flags = FS_WRONLY | FS_CREAT | FS_TRUNC; break;
        case 'a': flags = FS_WRONLY | FS_CREAT | FS_APPEND; break;
        case 'x': flags = FS_WRONLY | FS_CREAT | FS_EXCL; break;
        case '+': flags = (flags & ~FS_WRONLY) | FS_RDWR; break;
        case 'b': type = &file_bin; break;
        }
    file_t *f = mp_obj_malloc_with_finaliser(file_t, type);
    f->h = -1, f->pos = f->boff = 0, f->blen = 0;
    f->h = check(fs_open(mp_obj_str_get_str(args[0]), flags, 0644));
    return MP_OBJ_FROM_PTR(f);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, py_open);

/* ---- os */
static mp_obj_t os_getcwd(void) { const char *d = fs_getcwd(); return mp_obj_new_str(d, strlen(d)); }
static MP_DEFINE_CONST_FUN_OBJ_0(os_getcwd_obj, os_getcwd);

static mp_obj_t os_chdir(mp_obj_t p) { check(fs_chdir(mp_obj_str_get_str(p))); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(os_chdir_obj, os_chdir);

static mp_obj_t os_mkdir(size_t n, const mp_obj_t *a)
{
    check(fs_mkdir(mp_obj_str_get_str(a[0]), n > 1 ? (int)mp_obj_get_int(a[1]) : 0755));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_mkdir_obj, 1, 2, os_mkdir);

static mp_obj_t os_rmdir(mp_obj_t p) { check(fs_rmdir(mp_obj_str_get_str(p))); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(os_rmdir_obj, os_rmdir);

static mp_obj_t os_remove(mp_obj_t p) { check(fs_unlink(mp_obj_str_get_str(p))); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(os_remove_obj, os_remove);

static mp_obj_t os_rename(mp_obj_t a, mp_obj_t b)
{
    check(fs_rename(mp_obj_str_get_str(a), mp_obj_str_get_str(b)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(os_rename_obj, os_rename);

static mp_obj_t os_listdir(size_t n, const mp_obj_t *a)
{
    long h = check(fs_open(n ? mp_obj_str_get_str(a[0]) : ".", FS_RDONLY | FS_DIRECTORY, 0)), r;
    mp_obj_t list = mp_obj_new_list(0, NULL);
    uint64_t cur = 0, ino;
    int type;
    char buf[1024], name[256];
    while ((r = fs_readdir(h, &cur, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + r, &ino, &type, name); )
            if (strcmp(name, ".") && strcmp(name, "..")) mp_obj_list_append(list, mp_obj_new_str(name, strlen(name)));
    fs_close(h);
    check(r);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_listdir_obj, 0, 1, os_listdir);

/* stat(path) -> (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime), times in seconds */
static mp_obj_t os_stat(mp_obj_t p)
{
    siefs_stat_t st;
    check(fs_stat(mp_obj_str_get_str(p), &st, 0));
    mp_obj_t t[10] = {
        mp_obj_new_int_from_uint(st.mode), mp_obj_new_int_from_ull(st.ino), MP_OBJ_NEW_SMALL_INT(0),
        mp_obj_new_int_from_uint(st.nlink), mp_obj_new_int_from_uint(st.uid), mp_obj_new_int_from_uint(st.gid),
        mp_obj_new_int_from_ull(st.size), mp_obj_new_int_from_ll(st.atime / 1000000000),
        mp_obj_new_int_from_ll(st.mtime / 1000000000), mp_obj_new_int_from_ll(st.ctime / 1000000000),
    };
    return mp_obj_new_tuple(10, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_stat_obj, os_stat);

static mp_obj_t os_urandom(mp_obj_t n)
{
    vstr_t v;
    vstr_init_len(&v, (size_t)mp_obj_get_int(n));
    for (size_t i = 0; i < v.len; i += 4096) sys_random(v.buf + i, MIN(4096, v.len - i));
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_urandom_obj, os_urandom);

/* system(command): sh -c command -> its exit status */
static mp_obj_t os_system(mp_obj_t cmd)
{
    char args[256] = "sh -c ";
    strlcpy(args + 6, mp_obj_str_get_str(cmd), sizeof args - 6);
    sieos_flush();
    return mp_obj_new_int(run("/bin/sh", args));
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_system_obj, os_system);

static const mp_rom_map_elem_t os_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&os_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&os_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_urandom), MP_ROM_PTR(&os_urandom_obj) },
    { MP_ROM_QSTR(MP_QSTR_system), MP_ROM_PTR(&os_system_obj) },
};
static MP_DEFINE_CONST_DICT(os_globals, os_table);
const mp_obj_module_t mp_module_os = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&os_globals };
MP_REGISTER_MODULE(MP_QSTR_os, mp_module_os);
