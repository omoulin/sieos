/*
 * hashlib.c - The hashlib module for Python on SIEOS: sha256, sha384 and
 * sha512 from SIEOS's own SHA-2 (lib/sha2.c).
 *
 *   h = hashlib.sha256(b'abc'); h.update(b'more'); h.digest(); h.hexdigest()
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "py/runtime.h"
#include "mk/crypto.h"

typedef struct { mp_obj_base_t base; sha_t s; } hash_t;

static mp_obj_t hash_update(mp_obj_t self, mp_obj_t data)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    sha_update(&((hash_t *)MP_OBJ_TO_PTR(self))->s, b.buf, b.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(hash_update_obj, hash_update);

/* (on a copy: more update() calls may follow, as in CPython) */
static mp_obj_t hash_digest(mp_obj_t self)
{
    sha_t s = ((hash_t *)MP_OBJ_TO_PTR(self))->s;
    uint8_t d[64];
    sha_final(&s, d);
    return mp_obj_new_bytes(d, sha_len(s.alg));
}
static MP_DEFINE_CONST_FUN_OBJ_1(hash_digest_obj, hash_digest);

static mp_obj_t hash_hexdigest(mp_obj_t self)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(hash_digest(self), &b, MP_BUFFER_READ);
    vstr_t v;
    vstr_init_len(&v, 2 * b.len);
    for (size_t i = 0; i < b.len; i++) {
        v.buf[2 * i] = "0123456789abcdef"[((uint8_t *)b.buf)[i] >> 4];
        v.buf[2 * i + 1] = "0123456789abcdef"[((uint8_t *)b.buf)[i] & 15];
    }
    return mp_obj_new_str_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_1(hash_hexdigest_obj, hash_hexdigest);

static const mp_rom_map_elem_t hash_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_update), MP_ROM_PTR(&hash_update_obj) },
    { MP_ROM_QSTR(MP_QSTR_digest), MP_ROM_PTR(&hash_digest_obj) },
    { MP_ROM_QSTR(MP_QSTR_hexdigest), MP_ROM_PTR(&hash_hexdigest_obj) },
};
static MP_DEFINE_CONST_DICT(hash_locals, hash_locals_table);

static mp_obj_t hash_new(int alg, const mp_obj_type_t *type, size_t n, const mp_obj_t *args)
{
    hash_t *h = mp_obj_malloc(hash_t, type);
    sha_init(&h->s, alg);
    if (n) hash_update(MP_OBJ_FROM_PTR(h), args[0]);
    return MP_OBJ_FROM_PTR(h);
}
#define HASH(name, alg) \
    static mp_obj_t name##_make(const mp_obj_type_t *t, size_t n, size_t nkw, const mp_obj_t *a) \
    { mp_arg_check_num(n, nkw, 0, 1, false); return hash_new(alg, t, n, a); } \
    static MP_DEFINE_CONST_OBJ_TYPE(name##_type, MP_QSTR_##name, MP_TYPE_FLAG_NONE, make_new, name##_make, locals_dict, &hash_locals);
HASH(sha256, SHA256)
HASH(sha384, SHA384)
HASH(sha512, SHA512)

static const mp_rom_map_elem_t hashlib_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hashlib) },
    { MP_ROM_QSTR(MP_QSTR_sha256), MP_ROM_PTR(&sha256_type) },
    { MP_ROM_QSTR(MP_QSTR_sha384), MP_ROM_PTR(&sha384_type) },
    { MP_ROM_QSTR(MP_QSTR_sha512), MP_ROM_PTR(&sha512_type) },
};
static MP_DEFINE_CONST_DICT(hashlib_globals, hashlib_table);
const mp_obj_module_t mp_module_hashlib = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&hashlib_globals };
MP_REGISTER_MODULE(MP_QSTR_hashlib, mp_module_hashlib);
