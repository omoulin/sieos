/* abi.h - structs of every System V passing class, shared by abi.c (sicc)
 * and abi_gcc.c (the host compiler). Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
typedef struct { char c; } S1;
typedef struct { short a; char b; } S3;
typedef struct { int a, b; } S8;
typedef struct { long a; int b; } S12;
typedef struct { long a, b; } S16;
typedef struct { long a, b, c; } S24;               /* memory */
typedef struct { double x, y; } D2;                 /* SSE, SSE */
typedef struct { double x; long y; } DL;            /* SSE, INTEGER */
typedef struct { int i; float f; double d; } IFD;   /* INTEGER, SSE */
typedef struct { float a, b, c; } F3;               /* SSE, SSE */
typedef struct { char s[5]; } C5;
long g_take(S1 a, S3 b, S8 c, S12 d, S16 e, S24 f, D2 g, DL h, IFD i, F3 j, C5 k, int x, double y);
S1 g_s1(int v); S3 g_s3(int v); S8 g_s8(int v); S12 g_s12(int v); S16 g_s16(int v); S24 g_s24(int v);
D2 g_d2(double v); DL g_dl(double v); IFD g_ifd(int v); F3 g_f3(float v); C5 g_c5(int v);
long s_take(S1 a, S3 b, S8 c, S12 d, S16 e, S24 f, D2 g, DL h, IFD i, F3 j, C5 k, int x, double y);
S16 s_s16(int v); S24 s_s24(int v); D2 s_d2(double v); DL s_dl(double v); IFD s_ifd(int v); F3 s_f3(float v); C5 s_c5(int v);
long g_many(long a, long b, long c, long d, long e, long f, long g, long h, S16 s, double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8, double d9, S12 t);
long g_calls_back(void);
