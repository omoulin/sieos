/*
 * sys.h - The only things sicc needs from the C library it runs on: files,
 * memory, formatting, exit. All of it is ISO C.
 *
 * Compiled by another compiler, the C library's own headers are used.
 * Compiled by sicc itself (bootstrap), sicc does not read those headers:
 * the few functions are declared here. That is the one place tied to the
 * C library underneath: on the development machine its stdin, stdout and
 * stderr are objects of those names, which these declarations name.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef SICC_SYS_H
#define SICC_SYS_H

#ifndef __SICC__
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#else
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int fflush(FILE *f);
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
_Noreturn void exit(int code);
char *getenv(const char *name);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
double strtod(const char *s, char **end);
long double strtold(const char *s, char **end);
long strtol(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
int remove(const char *path);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *s, const char *t);
char *strpbrk(const char *s, const char *accept);
#endif

#endif
