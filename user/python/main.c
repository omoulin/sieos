/*
 * main.c - /bin/python: MicroPython on SIEOS (docs/python.md).
 *
 *   python                     the interactive prompt (>>> and ... lines)
 *   python FILE [ARGS...]      run a script; sys.argv = [FILE, ARGS...]
 *   python -c CODE...          run CODE (the words after -c, joined by spaces)
 *
 * The interpreter runs in a thread with a 512 KiB stack (a program's own
 * stack is 32 KiB); Python's frames go on a "Python stack" beside it, and
 * objects in the garbage-collected heap: 1 MiB at first, more from malloc
 * when it is full. Here also: the console (output buffered until a newline,
 * input a typed line at a time), clocks, and reading .py files for import.
 *
 * Ctrl-C: SIGINT, caught by the interpreter's thread, schedules a
 * KeyboardInterrupt, which the interpreter raises at its next check (a
 * loop's jump, a call). A wait it interrupts (a typed line, a sleep, the
 * network) returns -EINTR at once and raises it there.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "shared/readline/readline.h"
#include "genhdr/mpversion.h"
#include "mk.h"
#include "net.h"

#define CSTACK  (512 << 10)
#define PYSTACK (256 << 10)
#define HEAP    (1 << 20)

/* ---- the console */
static char obuf[512], ibuf[256];
static size_t olen, ipos, ilen;

void sieos_flush(void) { if (olen) con_write(obuf, olen); olen = 0; }

mp_uint_t mp_hal_stdout_tx_strn(const char *s, size_t n)
{
    for (size_t k = n; k; ) {
        size_t c = k < sizeof obuf - olen ? k : sizeof obuf - olen;
        memcpy(obuf + olen, s, c);
        olen += c, s += c, k -= c;
        if (olen == sizeof obuf) sieos_flush();
    }
    if (n && memchr(obuf, '\n', olen)) sieos_flush();
    return n;
}
void mp_hal_stdout_tx_strn_cooked(const char *s, size_t n) { mp_hal_stdout_tx_strn(s, n); }
void mp_hal_stdout_tx_str(const char *s) { mp_hal_stdout_tx_strn(s, strlen(s)); }
uintptr_t mp_hal_stdio_poll(uintptr_t flags) { (void)flags; return 0; }

/* A character typed: lines come whole from the terminal; at its end
 * (Ctrl-D on an empty line, or a closed window) CHAR_CTRL_D; Ctrl-C while
 * waiting for a line: CHAR_CTRL_C (KeyboardInterrupt, raised by the caller). */
int mp_hal_stdin_rx_chr(void)
{
    if (ipos == ilen) {
        sieos_flush();
        long n = con_read(ibuf, sizeof ibuf);
        if (n == -EINTR) { MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL; return CHAR_CTRL_C; }
        if (n <= 0) return CHAR_CTRL_D;
        ipos = 0, ilen = (size_t)n;
    }
    return (unsigned char)ibuf[ipos++];
}

/* A line for input() and the prompt, without its newline -> 0, CHAR_CTRL_D
 * at the end, CHAR_CTRL_C for Ctrl-C. */
int sieos_readline(vstr_t *line, const char *prompt)
{
    mp_hal_stdout_tx_str(prompt);
    sieos_flush();
    for (;;) {
        int c = mp_hal_stdin_rx_chr();
        if (c == CHAR_CTRL_C) return c;
        if (c == CHAR_CTRL_D) return line->len ? 0 : CHAR_CTRL_D;
        if (c == '\n') return 0;
        vstr_add_byte(line, (byte)c);
    }
}

/* ---- clocks, randomness */
mp_uint_t mp_hal_ticks_ms(void)  { return (mp_uint_t)(sys_clock() / 1000000); }
mp_uint_t mp_hal_ticks_us(void)  { return (mp_uint_t)(sys_clock() / 1000); }
mp_uint_t mp_hal_ticks_cpu(void) { return (mp_uint_t)sys_clock(); }
/* (a sleep Ctrl-C ends: -EINTR, its KeyboardInterrupt raised now) */
static void delay_ns(uint64_t ns) { sieos_flush(); if (sys_sleep(ns) < 0) mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); }
void mp_hal_delay_ms(mp_uint_t ms) { delay_ns((uint64_t)ms * 1000000); }
void mp_hal_delay_us(mp_uint_t us) { delay_ns((uint64_t)us * 1000); }

/* The date: SIEOS keeps it in the network server (from the clock chip),
 * asked once; then the time since boot is added. */
uint64_t mp_hal_time_ns(void)
{
    static int64_t base;
    if (!base) {
        net_info_t in;
        base = net_info(&in) >= 0 && in.time > 0 ? in.time * 1000000000LL - sys_clock() : 1;
    }
    return (uint64_t)(base + sys_clock());
}

uint32_t sieos_random_u32(void) { uint32_t r; sys_random(&r, sizeof r); return r; }

/* ---- files for import */
mp_import_stat_t mp_import_stat(const char *path)
{
    siefs_stat_t st;
    if (fs_stat(path, &st, 0) < 0) return MP_IMPORT_STAT_NO_EXIST;
    return (st.mode & SIEFS_IFMT) == SIEFS_IFDIR ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    long h = fs_open(qstr_str(filename), FS_RDONLY, 0), n;
    siefs_stat_t st;
    if (h < 0 || (n = fs_fstat(h, &st)) < 0) {
        if (h >= 0) fs_close(h);
        mp_raise_OSError((int)-(h < 0 ? h : n));
    }
    size_t len = (size_t)st.size;
    char *src = m_new(char, len ? len : 1);
    for (size_t got = 0; got < len; got += (size_t)n)
        if ((n = fs_read(h, got, src + got, len - got)) <= 0) { len = got; break; }
    fs_close(h);
    return mp_lexer_new_from_str_len(filename, src, len, len ? len : 1);
}

/* ---- the garbage collector: the roots are the stack and the registers
 * that keep values across calls (copied onto the stack first). */
void gc_collect(void)
{
    void *regs[12];
#if defined(__x86_64__)
    asm volatile("mov %%rbx, 0(%0); mov %%rbp, 8(%0); mov %%r12, 16(%0); mov %%r13, 24(%0); mov %%r14, 32(%0); mov %%r15, 40(%0)"
                 : : "r"(regs) : "memory");
#else
    asm volatile("stp x19, x20, [%0]; stp x21, x22, [%0, #16]; stp x23, x24, [%0, #32]; stp x25, x26, [%0, #48];"
                 "stp x27, x28, [%0, #64]; str x29, [%0, #80]" : : "r"(regs) : "memory");
#endif
    gc_collect_start();
    gc_collect_root(regs, ((uintptr_t)MP_STATE_THREAD(stack_top) - (uintptr_t)regs) / sizeof(void *));
    gc_collect_end();
}

/* The garbage-collected heap may grow by this much more each time. */
size_t gc_get_max_new_split(void) { return (size_t)1 << 30; }

void nlr_jump_fail(void *val)
{
    (void)val;
    sieos_flush();
    printf("python: fatal error (an exception with no handler)\n");
    sys_exit(1);
}

/* ---- running code */
#define EXITED 0x100                        /* SystemExit: EXITED | its status */

static int execute(const char *src, const char *file, mp_parse_input_kind_t kind)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = file ? mp_lexer_new_from_file(qstr_from_str(file))
                               : mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr name = lex->source_name;
        if (kind == MP_PARSE_FILE_INPUT) mp_store_global(MP_QSTR___file__, MP_OBJ_NEW_QSTR(name));
        mp_parse_tree_t tree = mp_parse(lex, kind);
        mp_call_function_0(mp_compile(&tree, name, kind == MP_PARSE_SINGLE_INPUT));
        nlr_pop();
        return 0;
    }
    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)), MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
        mp_obj_t v = mp_obj_exception_get_value(exc);
        mp_int_t code = 0;
        if (v != mp_const_none && !mp_obj_get_int_maybe(v, &code)) { mp_obj_print(v, PRINT_STR); mp_print_str(&mp_plat_print, "\n"); code = 1; }
        return EXITED | (code & 255);
    }
    mp_obj_print_exception(&mp_plat_print, exc);
    return 1;
}

static int repl(void)
{
    mp_hal_stdout_tx_str("MicroPython " MICROPY_GIT_TAG " on SIEOS (" MICROPY_PY_SYS_PLATFORM ", "
#if defined(__x86_64__)
                         "x86-64"
#else
                         "AArch64"
#endif
                         "); Ctrl-D to leave\n");
    for (;;) {
        vstr_t line;
        vstr_init(&line, 64);
        int c = sieos_readline(&line, mp_repl_get_ps1());
        if (c == CHAR_CTRL_D) { mp_hal_stdout_tx_str("\n"); return 0; }
        while (c != CHAR_CTRL_C && mp_repl_continue_with_input(vstr_null_terminated_str(&line))) {
            vstr_add_byte(&line, '\n');
            size_t at = line.len;
            if ((c = sieos_readline(&line, mp_repl_get_ps2())) == CHAR_CTRL_D || line.len == at) break;
        }
        if (c == CHAR_CTRL_C) { mp_hal_stdout_tx_str("KeyboardInterrupt\n"); vstr_clear(&line); continue; }
        int r = execute(vstr_null_terminated_str(&line), 0, MP_PARSE_SINGLE_INPUT);
        vstr_clear(&line);
        if (r & EXITED) return r & 255;
    }
}

static int ac;
static char **av;

static void on_sigint(int sig) { (void)sig; mp_sched_keyboard_interrupt(); }

static void python(void *arg)
{
    (void)arg;
    int top;
    mp_cstack_init_with_top(&top, CSTACK - 4096);
    char *heap = malloc(HEAP), *pystack = malloc(PYSTACK);
    if (!heap || !pystack) { printf("python: no memory\n"); sys_exit(1); }
    gc_init(heap, heap + HEAP);
    mp_pystack_init(pystack, pystack + PYSTACK);
    mp_init();
    signal(SIGINT, on_sigint);              /* (this thread takes it: its waits end at once) */

    const char *a1 = ac > 1 ? av[1] : "";
    int c = !strcmp(a1, "-c"), r;
    if (!strcmp(a1, "--version") || !strcmp(a1, "-V")) {
        printf("MicroPython %s (Python 3 language)\n", MICROPY_GIT_TAG);
        sys_exit(0);
    }
    /* sys.path: the script's folder (or the current one), then /lib/python */
    const char *file = *a1 && !c ? a1 : 0, *slash = file ? strrchr(file, '/') : 0;
    mp_obj_list_append(mp_sys_path, slash ? mp_obj_new_str(file, slash > file ? (size_t)(slash - file) : 1)
                                          : MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, mp_obj_new_str("/lib/python", 11));
    if (c) {                                /* -c CODE: the words after -c, joined again */
        vstr_t code;
        vstr_init(&code, 64);
        for (int k = 2; k < ac; k++) { if (k > 2) vstr_add_byte(&code, ' '); vstr_add_str(&code, av[k]); }
        mp_obj_list_append(mp_sys_argv, mp_obj_new_str("-c", 2));
        r = execute(vstr_null_terminated_str(&code), 0, MP_PARSE_FILE_INPUT);
    } else if (file) {
        for (int k = 1; k < ac; k++) mp_obj_list_append(mp_sys_argv, mp_obj_new_str(av[k], strlen(av[k])));
        r = execute(0, file, MP_PARSE_FILE_INPUT);
    } else {
        mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR_));
        r = repl();
    }
    gc_sweep_all();                         /* (closes files left open: their finalisers) */
    sieos_flush();
    sys_exit(r & ~EXITED);
}

int main(int argc, char **argv)
{
    ac = argc, av = argv;
    if (thread_start(python, 0, CSTACK) < 0) { printf("python: no memory for a stack\n"); return 1; }
    for (;;) sys_sleep(1000000000000ULL);   /* (the interpreter's sys_exit ends the program) */
}
