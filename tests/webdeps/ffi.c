/* pkg-config: libffi */
/*
 * wtest-ffi (make webdeps-test): libffi calls a function it only knows by
 * its signature: integers, a double, a pointer, a struct returned; and a
 * variadic one (printf's way), as GObject's generic marshaller does.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <ffi.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest ffi: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

typedef struct { int a; double b; } pair_t;
static double mix(int a, long b, double c, const char *s) { return a + b + c + strlen(s); }
static pair_t make(int a, double b) { return (pair_t){ a * 2, b / 2 }; }
static int sum(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    int t = 0;
    for (int i = 0; i < n; i++) t += __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return t;
}

int main(void)
{
    ffi_cif cif;
    ffi_type *args[4] = { &ffi_type_sint, &ffi_type_slong, &ffi_type_double, &ffi_type_pointer };
    int a = 1; long b = 20; double c = 0.5; const char *s = "four";
    void *vals[4] = { &a, &b, &c, &s };
    double r = 0;
    check("a call by signature", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &ffi_type_double, args) == FFI_OK &&
          (ffi_call(&cif, FFI_FN(mix), &r, vals), r == 25.5));
    ffi_type *el[3] = { &ffi_type_sint, &ffi_type_double, 0 };
    ffi_type st = { 0, 0, FFI_TYPE_STRUCT, el };
    ffi_type *a2[2] = { &ffi_type_sint, &ffi_type_double };
    int x = 21; double y = 3.0;
    void *v2[2] = { &x, &y };
    pair_t p = { 0, 0 };
    check("a struct returned", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &st, a2) == FFI_OK &&
          (ffi_call(&cif, FFI_FN(make), &p, v2), p.a == 42 && p.b == 1.5));
    ffi_type *a3[4] = { &ffi_type_sint, &ffi_type_sint, &ffi_type_sint, &ffi_type_sint };
    int n = 3, i1 = 5, i2 = 6, i3 = 7;
    void *v3[4] = { &n, &i1, &i2, &i3 };
    ffi_arg out = 0;
    check("a variadic call", ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 4, &ffi_type_sint, a3) == FFI_OK &&
          (ffi_call(&cif, FFI_FN(sum), &out, v3), (int)out == 18));
    printf("wtest ffi: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
