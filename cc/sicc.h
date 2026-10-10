/*
 * sicc.h - sicc, the SIEOS C compiler: what its parts share.
 *
 * One program does everything, in memory, without temporary files:
 *
 *   .c --> lex.c --> pp.c ------> parse.c ---> ir.c ----> ra.c ----> emit.c --+
 *          tokens   preprocessed  syntax tree  IR with    registers  assembly |
 *                   tokens        with types   virtual    assigned   text     |
 *                                              registers                      |
 *   .S --> lex.c --> pp.c --> (text) --------------------------------------> asm.c --> object
 *                                                                              |    (ELF64 .o)
 *   objects, archives, linker script ---------------------------------------> link.c --> program
 *
 * Memory: everything a compilation needs is taken from one arena (util.c)
 * and never freed one by one; the arena goes away with the process. That is
 * both the leanest bookkeeping and the fastest allocator.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef SICC_H
#define SICC_H
#include "sys.h"

#define VERSION "0.1"

/* ------------------------------------------------------------ the target
 * One sicc compiles for either machine (--target=aarch64, or a program name
 * ending in "aarch64-sicc"); the linker learns it from its input objects. */
enum { T_X86_64, T_AARCH64 };
extern int target;
void target_init(void);                     /* parse.c: the types that differ by target */

/* ------------------------------------------------------------ util.c */
typedef struct { char *p; long len, cap; } Buf;   /* a growable byte buffer */
typedef struct { void **v; int n, cap; } Vec;     /* a growable array of pointers */
typedef struct { const void **k; void **v; int cap, n; } Map;  /* keys: interned strings */

void *arena(long n);                        /* zeroed memory, never freed */
char *xstrndup(const char *s, long n);
char *fmt(const char *f, ...);              /* a formatted string, in the arena */
const char *intern(const char *s, long n);  /* one pointer per distinct string */
void buf_add(Buf *b, const void *p, long n);
void buf_c(Buf *b, int c);
void buf_s(Buf *b, const char *s);
void buf_f(Buf *b, const char *f, ...);
void buf_zero(Buf *b, long n);
void vec_push(Vec *v, void *x);
void *map_get(Map *m, const void *k);
void map_put(Map *m, const void *k, void *v);
int read_file(const char *path, Buf *out);  /* 0 or -1 */
int write_file(const char *path, const void *p, long n);
_Noreturn void die(const char *f, ...);     /* print "sicc: ..." and exit */
void warnf(const char *f, ...);

/* ------------------------------------------------------------ lex.c */
typedef struct File { const char *name, *text; int id; } File;

enum { TK_IDENT, TK_PUNCT, TK_NUM, TK_STR, TK_CHAR, TK_EOF };

/* Punctuators of more than one character (one-character ones are the character). */
enum { P_ARROW = 256, P_INC, P_DEC, P_SHL, P_SHR, P_LE, P_GE, P_EQ, P_NE, P_AND, P_OR,
       P_MULA, P_DIVA, P_MODA, P_ADDA, P_SUBA, P_SHLA, P_SHRA, P_ANDA, P_XORA, P_ORA,
       P_ELLIPSIS, P_HASHHASH };

/* Keywords (the identifier's kw field). */
enum { K_NONE, K_VOID, K_CHAR, K_SHORT, K_INT, K_LONG, K_FLOAT, K_DOUBLE, K_SIGNED,
       K_UNSIGNED, K_BOOL, K_STRUCT, K_UNION, K_ENUM, K_TYPEDEF, K_EXTERN, K_STATIC,
       K_AUTO, K_REGISTER, K_INLINE, K_CONST, K_VOLATILE, K_RESTRICT, K_SIZEOF,
       K_ALIGNOF, K_ALIGNAS, K_STATIC_ASSERT, K_RETURN, K_IF, K_ELSE, K_WHILE, K_DO,
       K_FOR, K_SWITCH, K_CASE, K_DEFAULT, K_BREAK, K_CONTINUE, K_GOTO, K_NORETURN,
       K_ATTRIBUTE, K_ASM, K_TYPEOF, K_EXTENSION, K_VA_LIST, K_GENERIC, K_THREAD_LOCAL,
       K_ATOMIC, K_COMPLEX, K_INT128, K_COUNT };

typedef struct Hideset Hideset;
typedef struct Token Token;
struct Token {
    Token *next;
    const char *loc;        /* its spelling in the source */
    int len;
    short kind, punct, kw;  /* TK_*, P_* or a character, K_* */
    char bol, space;        /* first on its line; preceded by white space */
    const char *name;       /* interned spelling (identifiers) */
    File *file;
    int line;
    Hideset *hs;            /* macros that must not expand it again */
    Token *origin;          /* the macro use it came from */
};

File *new_file(const char *name, const char *text);
Token *tokenize(File *f);
Token *new_eof(Token *at);
Token *copy_token(Token *t);
int tok_is(Token *t, int punct);            /* the punctuator punct? */
int tok_id(Token *t, const char *s);        /* the identifier s? */
_Noreturn void error_at(Token *t, const char *f, ...);
void warn_at(Token *t, const char *f, ...);
extern int nerrors;
extern const char *kw_names[K_COUNT];

/* ------------------------------------------------------------ pp.c */
void pp_init(void);
void pp_define(const char *def);            /* "NAME" or "NAME=value" */
void pp_undef(const char *name);
void pp_add_include(const char *dir);
Token *preprocess(File *f);
void pp_print(Token *t, Buf *out);          /* tokens back to text (-E, and .S files) */
extern int opt_asm_mode;                    /* preprocessing assembly (.S) */
extern int opt_freestanding;

/* ------------------------------------------------------------ types and the syntax tree */
enum { TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_FLOAT, TY_DOUBLE,
       TY_ENUM, TY_PTR, TY_FUNC, TY_ARRAY, TY_STRUCT, TY_UNION, TY_INT128, TY_LDOUBLE,
       TY_COMPLEX };           /* complex: base = float, double or long double */

typedef struct Type Type;
typedef struct Member Member;
typedef struct Node Node;
typedef struct Obj Obj;

struct Type {
    int kind, size, align;
    char is_unsigned, is_const, is_volatile, is_variadic, is_incomplete, is_flex, is_llong, noproto, is_packed;
    char is_atomic;         /* _Atomic: every access indivisible, sequentially consistent */
    char is_vector;         /* vector_size(N): a struct holding one array (base: the element type) */
    char is_vla, vla_fixed; /* an array whose size is known only when running (fixed: by a typedef) */
    Node *vla_len;          /* VLA: its number of elements */
    Obj *vla_size;          /* VLA: the local variable that holds its size in bytes */
    Obj *pobj;              /* a parameter's type: the parameter (VLA lengths refer to it) */
    Type *base;             /* pointer target, array element, function's return type */
    int len;                /* array length, -1 if not known */
    Member *members;        /* struct/union */
    Type *params;           /* function parameters, linked by next */
    Type *next;
    Type *origin;           /* the type this one is a qualified copy of */
    Token *name;            /* the declared identifier, while parsing declarators */
    int noreturn;
};

struct Member {
    Member *next;
    Type *ty;
    const char *name;       /* interned; 0 for anonymous struct/union members */
    Token *tok;
    int offset, align;
    char is_bitfield;
    int bit_off, bit_width;
    int unit;               /* bit-field: the bytes it is read and written by (packed: 1-9) */
};

typedef struct Reloc Reloc;          /* an address inside a global's initial value */
struct Reloc { Reloc *next; long off; const char *sym; long addend; };

typedef struct AsmStmt AsmStmt;      /* GNU extended asm */
typedef struct AsmOp { const char *cons; Node *expr; const char *name; } AsmOp;
struct AsmStmt {
    const char *tmpl;
    AsmOp *out, *in; int nout, nin;
    const char **clob; int nclob;
    int is_basic;
};

enum {
    ND_NUM, ND_VAR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_AND, ND_OR, ND_XOR,
    ND_SHL, ND_SHR, ND_EQ, ND_NE, ND_LT, ND_LE, ND_NEG, ND_NOT, ND_BITNOT,
    ND_LOGAND, ND_LOGOR, ND_ASSIGN, ND_COND, ND_COMMA, ND_MEMBER, ND_ADDR, ND_DEREF,
    ND_CAST, ND_CALL, ND_STMT_EXPR, ND_MEMZERO, ND_BUILTIN, ND_SELF,
    ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE, ND_BLOCK, ND_GOTO, ND_LABEL, ND_RETURN,
    ND_EXPR_STMT, ND_ASM, ND_BREAK, ND_CONTINUE, ND_NULL,
    ND_LABEL_VAL,           /* &&label (GNU): label is its assembler name */
    ND_VEC,                 /* a vector operation: val VOP_*, lhs, rhs, hi: an immediate, args: shuffle indexes */
    ND_IRVAL,               /* (code generator) a value already computed: val = its register, lo = 1: constant val */
};

/* Builtin functions (ND_BUILTIN's val). */
enum { B_VA_START, B_VA_ARG, B_VA_END, B_VA_COPY, B_UNREACHABLE, B_TRAP, B_ATOMIC_LOAD,
       B_ATOMIC_STORE, B_ATOMIC_FETCH_ADD, B_ATOMIC_FETCH_SUB, B_ATOMIC_FETCH_AND,
       B_ATOMIC_FETCH_OR, B_ATOMIC_EXCHANGE, B_ATOMIC_TAS, B_ATOMIC_CLEAR, B_FENCE,
       B_CAS, B_CLZ, B_CTZ, B_POPCOUNT, B_BSWAP32, B_BSWAP64, B_FRAME_ADDRESS,
       B_ATOMIC_FETCH_XOR, B_ATOMIC_FETCH_NAND, B_ALLOCA, B_SPSAVE, B_SPRESTORE };   /* (atomic read-modify-writes: hi = 1 returns the new value) */

struct Node {
    int kind;
    Node *next;
    Type *ty;
    Token *tok;
    Node *lhs, *rhs;
    Node *cond, *then, *els, *init, *inc, *body;   /* statements */
    Node *args;             /* call arguments, builtin arguments */
    Type *fty;              /* a call's function type */
    Member *member;
    Obj *var;
    long val;               /* ND_NUM; ND_ASSIGN: 0 plain, 1 compound (rhs uses ND_SELF), 2 compound giving the old value */
    double fval;
    long double ldval;      /* ND_NUM of type long double (fval: the same, rounded); of a complex type: the imaginary part */
    /* switch and case */
    Node *cases, *dflt, *case_next;
    long lo, hi;
    /* goto and labels */
    const char *label;
    void *blk;              /* the IR block of a label/case (ir.c) */
    Node *brk, *cont;       /* loops/switch: where break/continue go (ND_NULL markers) */
    void *vla;              /* break/continue markers, labels, gotos: the VLA scope there (parse.c) */
    AsmStmt *asm_;
};

struct Obj {
    Obj *next;
    const char *name;       /* interned */
    const char *asm_name;   /* the symbol name (asm("...") may change it) */
    Type *ty;
    Token *tok;
    char is_local, is_static, is_extern, is_func, is_def, is_inline, is_tentative, is_tls;
    char used, emitted, addr_taken, is_param, is_extern_decl, is_str;
    int align;
    const char *section;
    Obj *vla_ptr;           /* a VLA: the hidden pointer to its storage */
    /* local variables */
    int vreg, slot;         /* in a register (vreg) or in the frame (slot) */
    const char *reg;        /* register variable: asm("r10") */
    int param_stack_off;    /* a parameter passed on the stack: its offset from rbp */
    int ref_vreg;           /* slot -2: its address is in this virtual register (AArch64: a large struct passed by reference) */
    /* global variables */
    char *data; Reloc *rel; char has_init;
    /* functions */
    Obj *params, *locals;
    Obj *pnext;             /* next parameter, in order */
    Node *body;
    Vec refs;               /* the globals and functions it uses */
    int npar_gp, npar_fp;   /* registers used by named parameters (varargs) */
    long par_stack;         /* bytes of named parameters on the stack */
    Obj *va_area;
    int is_noreturn;
    /* inlining (ir.c) */
    char noinline, always_inline;
    char inl_all;           /* static, only called: inlined at every call, not emitted ... */
    char inl_needed;        /* ... unless a call could not be (then emitted after all) */
    char inl_active;        /* its body is being inlined (no recursion) */
    int inl_cost;           /* nodes in its body (-1: cannot be inlined, 0: not counted yet) */
    int inl_frame;          /* bytes of its locals that live in memory */
    int nrefs, ncalls;      /* uses of it, and how many are direct calls */
};

/* Vector operations (ND_VEC, O_VEC). The numbers are also in include/xmmintrin.h (__builtin_vec). */
enum { VOP_ADD, VOP_SUB, VOP_MUL, VOP_DIV, VOP_AND, VOP_OR, VOP_XOR, VOP_SHL, VOP_SHR, VOP_NEG, VOP_NOT,
       VOP_EQ, VOP_NE, VOP_LT, VOP_LE, VOP_GT, VOP_GE, VOP_SHLI, VOP_SHRI, VOP_SARI, VOP_MIN, VOP_MAX, VOP_SQRT,
       VOP_ANDNOT, VOP_UNPCKLO, VOP_UNPCKHI, VOP_CVTI2F, VOP_CVTF2I, VOP_MADD, VOP_MADDUBS, VOP_AVG, VOP_ADDS,
       VOP_SUBS, VOP_PACKS, VOP_PACKUS, VOP_MULHI, VOP_MULUDQ, VOP_SAD, VOP_SHUFB, VOP_MOVEMASK, VOP_SPLAT,
       VOP_SHUF, VOP_CVTF2IR, VOP_SLLDQ, VOP_SRLDQ, VOP_PSHUFD, VOP_ABS, VOP_MOD,
       /* AArch64 NEON (include/arm_neon.h, through __builtin_neon). "acc": the
        * operation also reads its result's old value (the third operand). */
       VOP_FMA, VOP_FMS,                       /* acc +/- a * b (floating) */
       VOP_MLA, VOP_MLS,                       /* acc +/- a * b (integers) */
       VOP_DOT, VOP_DOTL,                      /* acc (int32) += dot of 4-byte groups of a, b (DOTL: group imm of b) */
       VOP_MULL, VOP_MULL2, VOP_MLAL, VOP_MLAL2,   /* widening multiply (2: the high halves), and accumulate */
       VOP_PADDL, VOP_PADAL,                   /* add neighbours, widening (PADAL: into acc) */
       VOP_ADDP, VOP_ADDV,                     /* add neighbours of a then b; add all elements (a scalar) */
       VOP_MOVL, VOP_MOVL2, VOP_XTN, VOP_QXTN, VOP_QXTUN,   /* widen (half), narrow (truncating, saturating) */
       VOP_UZP1, VOP_UZP2, VOP_TRN1, VOP_TRN2, /* (zip: VOP_UNPCKLO/HI) */
       VOP_EXT, VOP_DUPL, VOP_TBL, VOP_CNT, VOP_BSL,   /* bytes from a:b at imm; element imm everywhere; table; bit counts; acc ? a : b */
       VOP_FCVTL, VOP_FCVTL2, VOP_FCVTN,       /* half <-> single precision */
       VOP_COUNT };
Type *vector_of(Type *elem, int size, Token *tok);

extern Type *ty_int128, *ty_uint128, *ty_ldouble;
extern Type *ty_void, *ty_bool, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort,
            *ty_int, *ty_uint, *ty_long, *ty_ulong, *ty_float, *ty_double;

Type *pointer_to(Type *base);
Type *array_of(Type *base, int len);
Type *func_type(Type *ret);
Type *copy_type(Type *t);
Type *complex_of(Type *base);
int is_integer(Type *t);
int is_flonum(Type *t);
int is_numeric(Type *t);
int is_scalar(Type *t);
int is_compatible(Type *a, Type *b);
void add_type(Node *n);
int align_to(long n, int a);

/* parse.c: the whole translation unit, as a list of globals and functions */
Obj *parse(Token *tok);
long const_eval(Node *n);
long eval_addr(Node *n, const char **label);   /* an address constant: label + result */
int is_const_expr(Node *n);
double eval_double(Node *n);
long double eval_ld(Node *n);
void eval_complex(Node *n, long double *re, long double *im);
extern Buf global_asm;                     /* file-scope asm("...") */
extern int opt_general_regs_only;
extern int opt_lse;
extern int opt_dotprod;             /* AArch64: sdot/udot allowed (-march=...+dotprod, armv8.4+) */                 /* AArch64: the LSE atomics (ARMv8.1), not load/store-exclusive loops */

/* ------------------------------------------------------------ ir.c, ra.c, emit.c */
void gen_program(Obj *prog, Buf *out, const char *src);  /* assembly text for a parsed unit (src: its file, for -g) */
extern int opt_kernel_model, opt_optimize, opt_debug, opt_avx, opt_pic;

/* ------------------------------------------------------------ asm.c */
typedef struct Object Object;
Object *assemble(const char *name, const char *text, long len);
void object_write(Object *o, Buf *out);     /* an ELF64 relocatable file */

/* ------------------------------------------------------------ link.c */
int link_main(int argc, char **argv);       /* "ld"-style command line */
int ar_main(int argc, char **argv);         /* "ar rcs lib.a objects" */
int objcopy_main(int argc, char **argv);    /* "objcopy -O binary in out" */

#endif
