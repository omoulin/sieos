/*
 * main.c - sicc's command line: compile, assemble, link (gcc-style flags).
 *
 *   sicc [-c|-S|-E] [-o out] [-I dir] [-D name[=v]] [-U name] [-O0|-O2|-Os]
 *        [-ffreestanding] [-mcmodel=kernel] [-mgeneral-regs-only] files...
 *   sicc --ld ...        the linker      (ld-style options, see link.c)
 *   sicc --ar rcs a.a o  an archive
 *   sicc --objcopy -O binary in out
 * Flags it does not need (-Wall, -fno-pic, -mno-red-zone...) are accepted and
 * ignored. Several sources may be given: each is compiled on its own (with
 * -c or -S, each to its own output unless -o names the one output).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

int target;                                 /* T_X86_64 or T_AARCH64 */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static char *with_ext(const char *src, const char *ext)
{
    const char *b = base_name(src);
    const char *dot = strrchr(b, '.');
    int n = dot ? dot - b : (int)strlen(b);
    return fmt("%.*s%s", n, b, ext);
}

static int ends(const char *s, const char *e) { int n = strlen(s), m = strlen(e); return n >= m && !strcmp(s + n - m, e); }

static Buf preprocessed(const char *path, int asm_mode)
{
    opt_asm_mode = asm_mode;
    Buf src = { 0 };
    if (read_file(path, &src) < 0) die("%s: cannot read", path);
    File *f = new_file(intern(path, strlen(path)), src.p);
    Token *t = preprocess(f);
    Buf out = { 0 };
    if (asm_mode) pp_print(t, &out);
    else {
        Obj *prog = parse(t);
        gen_program(prog, &out, path);
    }
    return out;
}

int main(int argc, char **argv)
{
    const char *me = base_name(argv[0]);
    if (argc > 1 && !strcmp(argv[1], "--ld")) return link_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--ar")) return ar_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--objcopy")) return objcopy_main(argc - 1, argv + 1);
    if (!strcmp(me, "ld")) return link_main(argc, argv);

    int mode = 0;                              /* 0 link, 'c', 'S', 'E' */
    const char *outfile = 0;
    Vec srcs = { 0 };
    Vec link = { 0 };
    vec_push(&link, "ld");
    Vec user_inc = { 0 };
    /* the target first: the predefined macros and some types depend on it.
     * By default, the machine sicc itself runs on; or a name ending in
     * aarch64-sicc or arm64-sicc; or --target=. */
#ifdef __aarch64__
    target = T_AARCH64;
#endif
    int ml = strlen(me);
    if ((ml >= 12 && !strcmp(me + ml - 12, "aarch64-sicc")) || (ml >= 10 && !strcmp(me + ml - 10, "arm64-sicc"))) target = T_AARCH64;
    for (int i = 1; i < argc; i++) {
        const char *t = !strncmp(argv[i], "--target=", 9) ? argv[i] + 9 : !strcmp(argv[i], "-target") && i + 1 < argc ? argv[i + 1] : 0;
        if (t) target = !strncmp(t, "aarch64", 7) || !strncmp(t, "arm64", 5) ? T_AARCH64 : T_X86_64;
    }
    target_init();
    pp_init();
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
#define ARG(opt) (!strncmp(a, opt, strlen(opt)) ? (a[strlen(opt)] ? a + strlen(opt) : (i + 1 < argc ? argv[++i] : (die("%s needs an argument", opt), ""))) : 0)
        char *v;
        if (!strcmp(a, "-c")) mode = 'c';
        else if (!strcmp(a, "-S")) mode = 'S';
        else if (!strcmp(a, "-E")) mode = 'E';
        else if (!strcmp(a, "-o")) { if (++i >= argc) die("-o needs a file"); outfile = argv[i]; }
        else if ((v = ARG("-I"))) vec_push(&user_inc, v);
        else if ((v = ARG("-isystem"))) vec_push(&user_inc, v);
        else if ((v = ARG("-D"))) pp_define(v);
        else if ((v = ARG("-U"))) pp_undef(v);
        else if (!strcmp(a, "-O0")) opt_optimize = 0;
        else if (!strcmp(a, "-Os") || !strcmp(a, "-Oz")) { opt_optimize = 2; pp_define("__OPTIMIZE__"); pp_define("__OPTIMIZE_SIZE__"); }
        else if (!strncmp(a, "-O", 2)) { opt_optimize = 1; pp_define("__OPTIMIZE__"); }
        else if (!strcmp(a, "-g") || !strcmp(a, "-g1") || !strcmp(a, "-g2") || !strcmp(a, "-g3") || !strcmp(a, "-ggdb")) opt_debug = 1;
        else if (!strcmp(a, "-g0")) opt_debug = 0;
        else if (!strcmp(a, "-ffreestanding")) opt_freestanding = 1;
        else if (!strcmp(a, "-mcmodel=kernel")) opt_kernel_model = 1;
        else if (!strcmp(a, "-fpic") || !strcmp(a, "-fPIC") || !strcmp(a, "-fpie") || !strcmp(a, "-fPIE")) opt_pic = 1;   /* x86-64: no absolute addresses in code */
        else if (!strcmp(a, "-mgeneral-regs-only")) opt_general_regs_only = 1;
        else if (!strcmp(a, "-mlse") || !strncmp(a, "-march=", 7)) {
            /* AArch64 extensions: LSE atomics from v8.1; the dot product instructions with +dotprod (v8.4: always) */
            if (!strcmp(a, "-mlse") || strstr(a, "+lse") || strstr(a, "armv8.1") || strstr(a, "armv8.2") || strstr(a, "armv8.3") ||
                strstr(a, "armv8.4") || strstr(a, "armv8.5") || strstr(a, "armv9")) opt_lse = 1;
            if (strstr(a, "+dotprod") || strstr(a, "armv8.4") || strstr(a, "armv8.5") || strstr(a, "armv9")) {
                opt_dotprod = 1;
                pp_define("__ARM_FEATURE_DOTPROD");
            }
            if (!strcmp(a, "-march=native")) { opt_avx = 1; pp_define("__AVX__"); pp_define("__AVX2__"); pp_define("__FMA__"); }
        }
        else if (!strcmp(a, "-mavx2") || !strcmp(a, "-mavx") || !strcmp(a, "-march=native")) { opt_avx = 1; pp_define("__AVX__"); pp_define("__AVX2__"); pp_define("__FMA__"); }
        else if (!strcmp(a, "-T") || !strcmp(a, "-L") || !strcmp(a, "-e") || !strcmp(a, "-z")) {
            vec_push(&link, a);
            if (++i < argc) vec_push(&link, argv[i]);
        } else if (!strncmp(a, "-Wl,", 4)) {
            for (char *p = xstrndup(a + 4, strlen(a + 4)), *q; p; p = q) {
                q = strchr(p, ',');
                if (q) *q++ = 0;
                vec_push(&link, p);
            }
        } else if (!strncmp(a, "-l", 2) || !strncmp(a, "-L", 2) || !strcmp(a, "-nostdlib") || !strcmp(a, "-static") || !strcmp(a, "-r")) vec_push(&link, a);
        else if (!strcmp(a, "-x") || !strcmp(a, "-target")) { i++; }
        else if (a[0] == '-' && a[1]) { /* -W..., -std=..., -f..., -m...: not needed */ }
        else if (ends(a, ".c") || ends(a, ".S") || ends(a, ".s") || ends(a, ".i")) vec_push(&srcs, a);
        else vec_push(&link, a);
#undef ARG
    }
    for (int i = 0; i < user_inc.n; i++) pp_add_include(user_inc.v[i]);
    const char *inc = getenv("SICC_INCLUDE");
    if (!inc) {
        const char *s = strrchr(argv[0], '/');
        inc = s ? fmt("%.*s/sicc-include", (int)(s - argv[0]), argv[0]) : "sicc-include";
    }
    pp_add_include(inc);
    if (!srcs.n && mode) die("no input file");
    if (outfile && mode && srcs.n > 1) die("-o with -%c and several sources", mode);

    Vec temps = { 0 };
    for (int k = 0; k < srcs.n; k++) {
        const char *src = srcs.v[k];
        if (mode == 'E') {
            opt_asm_mode = ends(src, ".S");
            Buf in = { 0 };
            if (read_file(src, &in) < 0) die("%s: cannot read", src);
            Buf out = { 0 };
            pp_print(preprocess(new_file(intern(src, strlen(src)), in.p)), &out);
            if (outfile) { if (write_file(outfile, out.p, out.len) < 0) die("%s: cannot write", outfile); }
            else fwrite(out.p, 1, out.len, stdout);
            continue;
        }
        Buf text = { 0 };
        if (ends(src, ".s")) { if (read_file(src, &text) < 0) die("%s: cannot read", src); }
        else text = preprocessed(src, ends(src, ".S"));
        if (mode == 'S') {
            const char *of = outfile ? outfile : with_ext(src, ".s");
            if (!strcmp(of, "-")) fwrite(text.p, 1, text.len, stdout);      /* -o -: standard output */
            else if (write_file(of, text.p, text.len) < 0) die("%s: cannot write", of);
            continue;
        }
        Object *obj = assemble(src, text.p, text.len);
        Buf bin = { 0 };
        object_write(obj, &bin);
        char *objfile = mode == 'c' ? (outfile ? (char *)outfile : with_ext(src, ".o"))
                                    : fmt("%s.sicc-tmp%d.o", outfile ? outfile : "a.out", k);
        if (write_file(objfile, bin.p, bin.len) < 0) die("%s: cannot write", objfile);
        free(bin.p);
        if (mode != 'c') { vec_push(&link, objfile); vec_push(&temps, objfile); }
    }
    if (mode) return 0;
    vec_push(&link, "-o");
    vec_push(&link, (void *)(outfile ? outfile : "a.out"));
    int r = link_main(link.n, (char **)link.v);
    for (int k = 0; k < temps.n; k++) remove(temps.v[k]);
    return r;
}
