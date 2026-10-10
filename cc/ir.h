/*
 * ir.h - sicc's intermediate representation, shared by ir.c (building it
 * from the syntax tree), ra.c (register allocation) and emit.c (assembly).
 *
 * A function is a list of basic blocks; a block, a list of instructions in
 * three-address form ("dst = a op b") over virtual registers, numbered from
 * 1 and unlimited in number. A virtual register may be assigned more than
 * once (local variables become virtual registers too), so the IR is not in
 * SSA form; liveness is computed over the control-flow graph instead.
 *
 * Integer values live in 64-bit registers. Operations work on 32 or 64 bits
 * (w = 4 or 8): char and short values are kept extended to 32 bits, which
 * matches C's integer promotions. Floating-point values (w = 4 float,
 * 8 double) live in xmm registers.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef SICC_IR_H
#define SICC_IR_H
#include "sicc.h"

enum { C_INT, C_FLT };                  /* register classes */

/* Physical registers: 0-15 the general ones (in the hardware's numbering),
 * 16-31 xmm0-xmm15. */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15,
       XMM0 = 16 };
typedef unsigned long long RegSet;  /* a set of physical registers, one bit each */
#define BIT(r) (1ULL << (r))
#define CALLER_SAVED (BIT(RAX) | BIT(RCX) | BIT(RDX) | BIT(RSI) | BIT(RDI) | BIT(R8) | BIT(R9) | BIT(R10) | BIT(R11) | 0xFFFF0000ULL)
#define CALLEE_SAVED (BIT(RBX) | BIT(R12) | BIT(R13) | BIT(R14) | BIT(R15))

/* AArch64: 0-30 the general registers x0-x30, 31 sp, 32-63 v0-v31. x14-x17
 * and v30-v31 are the code generator's scratch registers; x18 is reserved
 * (the platform register), x29 is the frame pointer, x30 the link register.
 * Callee-saved: x19-x28 and (their low 64 bits) v8-v15. */
enum { A64_SP = 31, A64_V0 = 32 };
#define A64_CALLER_SAVED (0x7FFFFULL | BIT(30) | 0xFFULL << 32 | 0xFFFFULL << 48)
#define A64_CALLEE_SAVED (0x1FF80000ULL | 0xFFULL << 40)
#define CALLER_SAVED_T (target == T_AARCH64 ? A64_CALLER_SAVED : CALLER_SAVED)
#define FREG0 (target == T_AARCH64 ? A64_V0 : XMM0)     /* the first floating-point register */

/* A memory address: [sym or frame slot or incoming-argument area] + base +
 * index*scale + off. */
typedef struct {
    int base, index;            /* virtual registers (0: none) */
    int scale;
    int slot;                   /* frame slot (0: none) */
    int args;                   /* 1: the caller's argument area (16(%rbp) + off) */
    const char *sym;            /* a global */
    int tls;                    /* sym is thread-local, defined in this module: %fs:sym@tpoff */
    long off;
} Addr;

enum {
    O_MOV,                      /* dst = a, or imm */
    O_ADD, O_SUB, O_MUL, O_AND, O_OR, O_XOR, O_SHL, O_SHR, O_SAR,
    O_DIV, O_UDIV, O_MOD, O_UMOD,
    O_NEG, O_NOT,
    O_EXT,                      /* dst = a extended from sz bytes (sgn) to w */
    O_SET,                      /* dst = a cc b ? 1 : 0 */
    O_LOAD,                     /* dst = sz bytes at m (sgn: sign-extend) */
    O_STORE,                    /* sz bytes at m = a (or imm) */
    O_LEA,                      /* dst = address of m */
    O_FADD, O_FSUB, O_FMUL, O_FDIV, O_FNEG,
    O_I2F,                      /* dst (float w) = integer a of sz bytes (sgn) */
    O_F2I,                      /* dst (integer w) = float a (sz: its size), truncated */
    O_F2F,                      /* dst (float w) = float a of the other size */
    O_CALL,
    O_BR,                       /* if (a cc b) goto t; else goto f */
    O_JMP,
    O_RET,
    O_SWITCH,                   /* goto tab[a] (a already checked in range) */
    O_COPY,                     /* copy imm bytes from address b to address a */
    O_ZERO,                     /* zero imm bytes at address a */
    O_ASM,
    O_XADD,                     /* dst = old *a; *a += b (lock) */
    O_XCHG,                     /* dst = old *a; *a = b */
    O_CMPXCHG,                  /* *a == old(dst in) ? *a = b : ... ; dst = old value; t: result flag in imm reg */
    O_FENCE,
    O_ENTRY,                    /* parameters arrive (the function's first instruction) */
    O_UNREACH,
    O_BITOP,                    /* imm: 0 clz, 1 ctz, 2 popcount, 3 bswap */
    O_FRAMEADDR,                /* dst = %rbp, or the return address (imm 1) */
    O_MULSHR,                   /* dst = (128-bit a * b) >> imm (sgn: signed) */
    /* long double (x87, 80 bits): values in memory, a/b/m.base their addresses */
    O_LDBIN,                    /* *m.base = *a op *b (imm: 0 add, 1 sub, 2 mul, 3 div) */
    O_LDNEG,                    /* *m.base = -*a */
    O_LDCMP,                    /* dst = *a cc *b ? 1 : 0 (b == 0: compare with 0) */
    O_LDCVT,                    /* conversions, imm: 0 int a (sz, sgn) -> *m.base, 1 *a -> int dst (w, sgn),
                                   2 float/double a (sz) -> *m.base, 3 *a -> float/double dst (w); m.slot: scratch */
    /* __int128: values in memory (16 bytes), a/b/m.base their addresses */
    O_I128,                     /* *m.base = *a op *b, imm: 0 add 1 sub 2 mul 3 and 4 or 5 xor 6 neg 7 not */
    O_I128SH,                   /* *m.base = *a shifted by b (or bimm: imm), sz: 0 shl 1 shr 2 sar */
    O_I128CMP,                  /* dst = *a cc *b ? 1 : 0 */
    O_I128EXT,                  /* *m.base = 64-bit a, extended (sgn) */
    O_BITS,                     /* dst = the bits of a, into the other register class (movd/movq) */
    O_ALLOCA,                   /* dst = a bytes taken from the stack (16-aligned) */
    O_SPSAVE,                   /* dst = the stack pointer */
    O_SPRESTORE,                /* the stack pointer = a */
    O_VEC,                      /* vectors in memory: *m.base = *a op *b (cc VOP_*, sz element size, sgn: 0 signed,
                                   1 unsigned, 2 floating, w vector size, imm immediate); VOP_MOVEMASK: dst */
    O_ROL,                      /* dst = a rotated left by imm bits (w) */
    O_FSQRT,                    /* dst = square root of float a (w) */
    O_TLSADDR,                  /* dst = the address of thread-local m.sym (imm 1: defined elsewhere, through the GOT) */
    O_NOP,
};

/* Conditions (O_SET, O_BR); F*: floating point, unordered-aware */
enum { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE, CC_ULT, CC_ULE, CC_UGT, CC_UGE,
       CC_FEQ, CC_FNE, CC_FLT, CC_FLE, CC_FGT, CC_FGE };

typedef struct Block Block;
typedef struct Ins Ins;

/* A call: its arguments' places, and its results. */
typedef struct {
    int v;                      /* the value (a register argument / a scalar on the stack) */
    int addr;                   /* a struct copied onto the stack: its address */
    int size;                   /* ... and its size */
    int phys;                   /* the argument register, or -1: on the stack */
    long stack_off;             /* offset in the outgoing argument area */
    int cls, w;
    int isimm; long imm;        /* a constant for a register argument (v: 0) */
} CallArg;

typedef struct {
    const char *sym;            /* a direct call */
    int fn;                     /* or the function's address, in a virtual register */
    CallArg *args;
    int nargs, nsse;            /* nsse: for a variadic callee, %al */
    long stack;                 /* bytes of stack arguments (16-aligned) */
    int ret[4], retphys[4], retw[4], nret;         /* (AArch64: up to 4 floats of a struct) */
    int vret_slot, vret_n, vret_es;  /* AArch64: vectors returned in v0-v3, stored to this slot (n of es bytes) */
    int variadic;
    int x87slot;                /* a long double result: the slot it is stored to */
    int x87cplx;                /* ... a complex long double: two of them (st0, st1) */
} Call;

/* An extended asm statement, operands bound to registers or memory. */
typedef struct {
    char kind;                  /* 'r' register, 'm' memory, 'i' immediate */
    signed char phys;           /* register: which one (-1: none yet; plain char may be unsigned) */
    char size;                  /* the operand's size (for register names) */
    char out, in;               /* is an output, is an input */
    int v;                      /* input value (virtual register) */
    int outv;                   /* output: the virtual register that receives it */
    Addr m;                     /* memory operand (base in a register: mreg) */
    int mreg;                   /* memory operand through a register: which */
    long imm;
    const char *name;
} AsmOperand;

typedef struct {
    const char *tmpl;
    AsmOperand *op;
    int n;
    int is_basic;
} AsmIns;

typedef struct {                /* O_ENTRY: where each parameter comes from */
    int v;                      /* into this virtual register ... */
    int slot; long off;         /* ... or into this frame slot + off */
    int phys;                   /* from this register, or -1: */
    long stack_off;             /* from 16(%rbp) + this */
    int w, cls;
} ParamIn;

struct Ins {
    Ins *next;
    short op, cc;
    char w, sz, sgn, bimm;
    int dst, a, b;
    int src;                    /* -g: source position, file << 20 | line (0: unknown) */
    long imm;
    Addr m;
    Block *t, *f;
    Call *call;
    AsmIns *asmi;
    ParamIn *pin;
    int npin;
    Block **tab;
    int ntab;
    int pos;                    /* position, for register allocation */
    RegSet clob;                /* physical registers it destroys */
};

void count_calls(Node *n, Obj *owner);
const char *vec_insn(int op, int kind, int es);
int a64_vec_ok(int op, int kind, int es);     /* (emit_a64.c) a NEON instruction for this generic operation? */
int inline_candidate(Obj *g);
extern Vec dbg_files;           /* -g: the source files positions name (File *), number k+1 */
int src_pos(Token *t);

struct Block {
    Block *next;                /* in layout order */
    Ins *first, *last;
    int id;
    int start, end;             /* positions */
    unsigned *in, *out;         /* live sets (bitsets over virtual registers) */
    int used;                   /* reachable */
    const char *sym;            /* a C label's assembler name (&&label) */
    int addr_taken;             /* &&label: a computed goto may reach it */
};

typedef struct {
    int size, align;
    long off;                   /* from %rbp (negative) */
} Slot;

typedef struct {
    char cls;
    char w;                     /* the widest use (4 or 8) */
    int phys;                   /* -1: in a stack slot */
    int slot;                   /* its spill slot */
    int start, end;             /* live interval */
    const char *fixed;          /* a register variable: asm("r10") */
    int hint;                   /* a preferred register (-1: none) */
} VReg;

typedef struct {
    Obj *obj;
    Block *blocks, *last;
    int nblocks;
    VReg *vr;
    int nvr, capvr;
    Slot *slots;
    int nslots, capslots;
    long frame;                 /* bytes below the saved registers */
    RegSet used_regs;           /* physical registers used (for saving callee-saved ones) */
    int va_slot;                /* variadic: the register save area's slot */
    int ret_slot;               /* the hidden struct-return pointer's slot */
    int has_calls;
    Buf consts;                 /* floating-point constants, as assembly */
} Fn;

extern Fn *F;
int new_vreg(int cls);
int new_slot(int size, int align);
int classify(Type *ty, int cls[2]);     /* struct: eightbytes in registers (0: memory) */

void lower_function(Obj *fn);           /* ir.c: the IR of a function, into F */
void regalloc(void);                    /* ra.c */
int ins_uses(Ins *i, int *u);
int ins_defs(Ins *i, int *d);
void optimize(void);                    /* opt.c */
void emit_function(Buf *out);           /* emit.c */
void emit_function_a64(Buf *out);       /* emit_a64.c */
int a64_fn_id(void);
int a64_logical_imm(unsigned long v, int w);
const char *reg_name(int phys, int size);

#endif
