/*
 * start.c - Where a program built on libposix (musl + the Linux
 * personality, docs/webkit-plan.md) starts: crt1.o.
 *
 * SIEOS starts a program at _start with its argument string ("name arg1
 * arg2", and after a \001 its parent's current folder) in the first
 * argument register, on a 32 KiB stack. Here: an 8 MiB stack (its pages
 * given on first touch), on it the initial stack a Linux program expects
 * (argc, argv, envp, the auxiliary vector: where the program headers are,
 * so musl finds the thread-local template; the page size; 16 random
 * bytes), then musl's __libc_start_main, which calls main. A program started
 * by posix_spawn finds a cookie after a \002 instead of its arguments: it
 * claims its arguments, environment and descriptors from unixd (spawn.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#define MAIN_STACK (8UL << 20)

int main(int, char **, char **);
int __libc_start_main(int (*)(int, char **, char **), int, char **, void (*)(void), void (*)(void), void (*)(void));
extern const Elf64_Ehdr __ehdr_start;              /* the linker's: the program's own ELF header */
__attribute__((weak)) void *__dso_handle = &__dso_handle;   /* (C++ static destructors name their module by it;
                                                              crtbegin.o has one too) */

/* The personality's (linux.c): memory, randomness, the start-up folder. */
void *__sieos_main_stack(size_t size);
void __sieos_random(void *buf, size_t n);
void __sieos_startdir(const char *dir);
unsigned long __sieos_hwcap(void);
int __sieos_claim(uint64_t cookie, char *buf, size_t size, char ***argv, char ***envp);
_Noreturn void __sieos_exit(int code);

__asm__(".text\n.global _start\n.type _start,%function\n_start:\n"
#if defined(__x86_64__)
        "    xor %ebp, %ebp\n    and $-16, %rsp\n    call __sieos_start\n    ud2\n"
        ".global __sieos_switch\n__sieos_switch:\n"      /* (sp, fn, arg): fn(arg) on stack sp */
        "    mov %rdi, %rsp\n    mov %rdx, %rdi\n    call *%rsi\n    ud2\n"
#else
        "    mov x29, #0\n    mov x30, #0\n    bl __sieos_start\n    udf #0\n"
        ".global __sieos_switch\n__sieos_switch:\n"
        "    mov sp, x0\n    mov x0, x2\n    blr x1\n    udf #0\n"
#endif
);
_Noreturn void __sieos_switch(void *sp, void (*fn)(long *), long *arg);

static _Noreturn void start(long *sp)
{
    __libc_start_main(main, (int)sp[0], (char **)(sp + 1), 0, 0, 0);
    for (;;) ;
}

static char *put(char **top, const void *s, size_t n)    /* below *top, n bytes */
{
    *top -= n;
    for (size_t i = 0; i < n; i++) (*top)[i] = ((const char *)s)[i];
    return *top;
}

_Noreturn void __sieos_start(char *args)
{
    char *top = (char *)__sieos_main_stack(MAIN_STACK) + MAIN_STACK;
    char *dir = args, *ck = args;
    while (*dir && *dir != '\001') dir++;
    if (*dir) { *dir++ = 0; __sieos_startdir(dir); }
    while (*ck && *ck != '\002') ck++;
    static const char *env0[] = { "PATH=/bin", "HOME=/", "LANG=C.UTF-8", 0 };
    static char *argv0[64], *envp0[4];
    char **argv = argv0, **envp = envp0;
    int argc = 0, envc = 3;
    if (*ck) {                                       /* started by posix_spawn: its stash, from unixd */
        uint64_t cookie = 0;
        for (char *h = ck + 1; (*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f'); h++)
            cookie = cookie << 4 | (uint64_t)(*h <= '9' ? *h - '0' : *h - 'a' + 10);
        char *buf = top - (256 << 10);
        argc = __sieos_claim(cookie, buf, 256 << 10, &argv, &envp);
        if (argc < 0) __sieos_exit(argc);
        top = buf;
        for (envc = 0; envp[envc]; envc++) ;
    } else {                                         /* started by SIEOS's own (sh, spawn_file) */
        size_t len = 0;
        while (args[len]) len++;
        char *a = put(&top, args, len + 1);
        while (*a && argc < 63) {                    /* words, split at spaces */
            while (*a == ' ') *a++ = 0;
            if (*a) argv[argc++] = a;
            while (*a && *a != ' ') a++;
        }
        for (int i = 0; i < envc; i++) {
            size_t n = 0;
            while (env0[i][n]) n++;
            envp[i] = put(&top, env0[i], n + 1);
        }
    }
    char rnd[16];
    __sieos_random(rnd, sizeof rnd);
    char *random = put(&top, rnd, sizeof rnd);
    const Elf64_Ehdr *eh = &__ehdr_start;
    unsigned long aux[] = {
        AT_PHDR, (unsigned long)eh + eh->e_phoff, AT_PHENT, eh->e_phentsize, AT_PHNUM, eh->e_phnum,
        AT_PAGESZ, 4096, AT_ENTRY, eh->e_entry, AT_HWCAP, __sieos_hwcap(), AT_SECURE, 0,
        AT_RANDOM, (unsigned long)random, AT_NULL, 0,
    };
    size_t words = 1 + argc + 1 + envc + 1 + sizeof aux / sizeof aux[0];
    long *sp = (long *)(((uintptr_t)top - words * 8) & ~15UL), *w = sp;
    *w++ = argc;
    for (int i = 0; i < argc; i++) *w++ = (long)argv[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = (long)envp[i];
    *w++ = 0;
    for (size_t i = 0; i < sizeof aux / sizeof aux[0]; i++) *w++ = (long)aux[i];
    __sieos_switch(sp, start, sp);
}
