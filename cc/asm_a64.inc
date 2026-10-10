/*
 * asm_a64.inc - The AArch64 half of the assembler (included by asm.c):
 * GNU syntax, every instruction 4 bytes, so no relaxation. What the code
 * generator emits, plus what an AArch64 kernel needs: system registers,
 * barriers, exception returns, cache and TLB maintenance.
 *
 * Operands: x0-x30, w0-w30, sp, wsp, xzr, wzr (lr = x30, fp = x29);
 * b/h/s/d/q0-31; #imm (the '#' may be left out; :lo12:sym, :tprel_hi12:sym...
 * select a relocation); [xn], [xn, #imm], [xn, #imm]!, [xn], #imm,
 * [xn, xm{, lsl|uxtw|sxtw|sxtx #s}]; labels and expressions. NEON:
 * v0.16b (a vector: lanes and element), v0.s[1] and v2.4b[1] (an element),
 * {v1.16b} or {v1.16b, v2.16b} (a register list).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */

enum { A_ABS64 = 257, A_ABS32 = 258, A_ABS16 = 259, A_PREL64 = 260, A_PREL32 = 261, A_PREL16 = 262,
       A_ADR_LO21 = 274, A_ADR_PAGE = 275, A_ADD_LO12 = 277, A_LDST8 = 278, A_TSTBR14 = 279, A_CONDBR19 = 280,
       A_JUMP26 = 282, A_CALL26 = 283, A_LDST16 = 284, A_LDST32 = 285, A_LDST64 = 286, A_LDST128 = 299,
       A_TPREL_HI12 = 549, A_TPREL_LO12 = 550, A_TPREL_LO12_NC = 551 };
enum { AM_NONE, AM_LO12, AM_TPREL_HI12, AM_TPREL_LO12, AM_TPREL_LO12_NC };

typedef struct {
    char kind;              /* 'r' general, 'v' float/SIMD, 'i' immediate, 'm' memory, 's' shift/extend, 'c' condition, 'n' a name,
                               'V' a NEON vector or element, 'L' a list of vectors */
    int reg, w;             /* 'r': number (31: sp or zr), width 4/8; 'v': number, width 1/2/4/8/16 */
    int sp;                 /* 'r': it was sp/wsp (not xzr/wzr) */
    Expr *e;                /* 'i', and 'm' offset */
    int mod;                /* AM_*: a relocation modifier */
    int fzero;              /* "#0.0" */
    int base, index, iw, ext, amt, pre, post, has_amt;   /* 'm' */
    const char *name;       /* 's' (lsl, sxtw...), 'c', 'n' */
    int es, lanes, lane, nreg;  /* 'V', 'L': element bytes, lanes (0: v0.s[1]), lane (-1: none), registers in a list */
} AOp;

/* "v3.16b", "v3.s[1]", "v2.4b[1]": a NEON operand (1), or not one (0) */
static int a64_vreg(const char *s, AOp *o)
{
    if ((s[0] | 0x20) != 'v' || s[1] < '0' || s[1] > '9') return 0;
    const char *p = s + 1;
    int n = 0, lanes = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + *p++ - '0';
    if (*p != '.' || n > 31) return 0;
    for (p++; *p >= '0' && *p <= '9'; p++) lanes = lanes * 10 + *p - '0';
    int c = *p | 0x20, es = c == 'b' ? 1 : c == 'h' ? 2 : c == 's' ? 4 : c == 'd' ? 8 : c == 'q' ? 16 : 0;
    if (!es) aerr("bad vector arrangement in '%s'", s);
    o->kind = 'V'; o->reg = n; o->es = es; o->lanes = lanes; o->lane = -1; o->w = lanes * es;
    if (*++p == '[') {
        o->lane = 0;
        for (p++; *p >= '0' && *p <= '9'; p++) o->lane = o->lane * 10 + *p - '0';
        if (*p++ != ']') aerr("bad element index in '%s'", s);
    }
    if (*p || (lanes && o->w != 8 && o->w != 16 && !(o->lane >= 0 && o->w == 4))) aerr("bad vector operand '%s'", s);
    return 1;
}

/* operands split at top-level commas (not inside [] or {}) */
static void a64_split(const char *s, Vec *out)
{
    int depth = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') depth--;
        if ((*p == ',' && depth == 0) || !*p) {
            vec_push(out, trim(xstrndup(start, p - start)));
            if (!*p) return;
            start = p + 1;
        }
    }
}

static int a64_regnum(const char *s, int *w, int *sp, char *vk)
{
    *sp = 0; *vk = 0;
    if (!strcmp(s, "sp")) { *w = 8; *sp = 1; return 31; }
    if (!strcmp(s, "wsp")) { *w = 4; *sp = 1; return 31; }
    if (!strcmp(s, "xzr")) { *w = 8; return 31; }
    if (!strcmp(s, "wzr")) { *w = 4; return 31; }
    if (!strcmp(s, "lr")) { *w = 8; return 30; }
    if (!strcmp(s, "fp")) { *w = 8; return 29; }
    if (!strcmp(s, "ip0")) { *w = 8; return 16; }
    if (!strcmp(s, "ip1")) { *w = 8; return 17; }
    if (!s[0] || !s[1]) return -1;
    int n = 0;
    for (const char *p = s + 1; *p; p++) { if (*p < '0' || *p > '9' || p - s > 2) return -1; n = n * 10 + *p - '0'; }
    switch (s[0]) {
    case 'x': if (n > 30) return -1; *w = 8; return n;
    case 'w': if (n > 30) return -1; *w = 4; return n;
    case 'b': case 'h': case 's': case 'd': case 'q': case 'v':
        if (n > 31) return -1;
        *vk = s[0];
        *w = s[0] == 'b' ? 1 : s[0] == 'h' ? 2 : s[0] == 's' ? 4 : s[0] == 'd' ? 8 : 16;
        return n;
    }
    return -1;
}

static void a64_imm(const char *s, AOp *o)
{
    o->kind = 'i';
    if (*s == '#') s++;
    while (*s == ' ') s++;
    if (!strcmp(s, "0.0")) { o->fzero = 1; o->e = parse_expr("0", 0); return; }
    if (*s == ':') {
        const char *e = strchr(s + 1, ':');
        if (!e) aerr("bad relocation modifier in '%s'", s);
        int n = e - s - 1;
        char m[24] = "";
        if (n >= 24) aerr("bad relocation modifier");
        memcpy(m, s + 1, n);
        o->mod = !strcmp(m, "lo12") ? AM_LO12 : !strcmp(m, "tprel_hi12") ? AM_TPREL_HI12 : !strcmp(m, "tprel_lo12") ? AM_TPREL_LO12 :
                 !strcmp(m, "tprel_lo12_nc") ? AM_TPREL_LO12_NC : -1;
        if (o->mod < 0) aerr("unsupported relocation modifier :%s:", m);
        s = e + 1;
    }
    const char *end;
    o->e = parse_expr(s, &end);
    if (*end) aerr("junk after an expression: '%s'", end);
}

static void a64_operand(char *s, AOp *o)
{
    memset(o, 0, sizeof *o);
    s = trim(s);
    if (*s == '{') {                                  /* a list: {v1.16b, v2.16b} or {v1.16b-v2.16b} */
        char *e = strrchr(s, '}');
        if (!e) aerr("missing '}' in '%s'", s);
        *e = 0;
        Vec parts = { 0 };
        a64_split(s + 1, &parts);
        AOp first;
        char *dash = parts.n == 1 ? strchr(parts.v[0], '-') : 0;
        if (dash) *dash = 0;
        if (!parts.n || !a64_vreg(trim(parts.v[0]), &first) || first.lane >= 0) aerr("bad register list '%s'", s);
        int last = first.reg;
        if (dash) { AOp t; if (!a64_vreg(trim(dash + 1), &t)) aerr("bad register list"); last = t.reg; }
        else for (int k = 1; k < parts.n; k++) {
            AOp t;
            if (!a64_vreg(trim(parts.v[k]), &t) || t.reg != (last + 1) % 32) aerr("a register list must be consecutive");
            last = t.reg;
        }
        *o = first;
        o->kind = 'L';
        o->nreg = (last - first.reg + 32) % 32 + 1;
        if (o->nreg > 4) aerr("at most 4 registers in a list");
        return;
    }
    if (a64_vreg(s, o)) return;
    if (*s == '[') {                                  /* memory */
        o->kind = 'm';
        o->index = -1;
        char *e = strrchr(s, ']');
        if (!e) aerr("missing ']' in '%s'", s);
        if (e[1] == '!') o->pre = 1;
        *e = 0;
        Vec parts = { 0 };
        a64_split(s + 1, &parts);
        int w, sp; char vk;
        if (parts.n < 1 || (o->base = a64_regnum(trim(parts.v[0]), &w, &sp, &vk)) < 0 || vk || w != 8) aerr("bad base register in '%s'", s);
        if (parts.n > 1) {
            char *p = trim(parts.v[1]);
            int r = a64_regnum(p, &w, &sp, &vk);
            if (r >= 0 && !vk) {
                o->index = r; o->iw = w;
                o->ext = w == 8 ? 3 : 2;              /* lsl (uxtx), or uxtw */
                if (parts.n > 2) {
                    char *x = trim(parts.v[2]), *q = x;
                    while (*q && *q != ' ' && *q != '#') q++;
                    int n = q - x;
                    o->ext = !strncmp(x, "lsl", n) && n == 3 ? 3 : !strncmp(x, "uxtw", n) && n == 4 ? 2 : !strncmp(x, "sxtw", n) && n == 4 ? 6 :
                             !strncmp(x, "sxtx", n) && n == 4 ? 7 : !strncmp(x, "uxtx", n) && n == 4 ? 3 : -1;
                    if (o->ext < 0) aerr("bad extend in '%s'", x);
                    q = trim(q);
                    if (*q) { if (*q == '#') q++; o->amt = pp_const(q); o->has_amt = 1; }
                }
            } else {
                AOp t = { 0 };
                a64_imm(p, &t);
                o->e = t.e;
                o->mod = t.mod;
            }
        }
        return;
    }
    int w, sp; char vk;
    char low[16] = "";
    for (int k = 0; k < 15 && s[k]; k++) low[k] = s[k] | (s[k] >= 'A' && s[k] <= 'Z' ? 0x20 : 0);
    int r = strlen(s) < 15 ? a64_regnum(low, &w, &sp, &vk) : -1;
    if (r >= 0) { o->kind = vk ? 'v' : 'r'; o->reg = r; o->w = w; o->sp = sp; return; }
    if (*s == '#' || (*s >= '0' && *s <= '9') || *s == '-' || *s == ':' || *s == '(' || *s == '~') { a64_imm(s, o); return; }
    /* shift or extend: "lsl #3", "sxtw", "uxtw #2" */
    static const char *sh[] = { "lsl", "lsr", "asr", "ror", "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx" };
    for (unsigned k = 0; k < sizeof sh / sizeof sh[0]; k++) {
        int n = strlen(sh[k]);
        if (!strncmp(s, sh[k], n) && (s[n] == 0 || s[n] == ' ' || s[n] == '#')) {
            o->kind = 's';
            o->name = sh[k];
            char *q = trim(s + n);
            if (*q) { if (*q == '#') q++; o->amt = pp_const(q); o->has_amt = 1; }
            return;
        }
    }
    o->kind = 'n';                                    /* a label, a condition or a name (system register...) */
    o->name = xstrndup(s, strlen(s));
    const char *end;
    o->e = parse_expr(s, &end);
    if (*end) o->e = 0;
}

/* ---- encoding helpers */
static void a64_put(unsigned w) { put(w, 4); }

static long a64_const(Expr *e) { return const_val(e, C.sec, C.base); }

/* a relocation for this instruction's word, of value v */
static void a64_rel(int type, Val v)
{
    if (v.sub) aerr("a symbol difference here is not supported");
    add_rel(C.base, type, v);
}

/* N:immr:imms for a logical immediate (0: not encodable) */
static int a64_bitmask(unsigned long v, int w, unsigned *bits)
{
    if (w == 4) { if (v >> 32 && (v >> 32) != 0xffffffffUL) return 0; v = (v & 0xffffffffUL) | (v & 0xffffffffUL) << 32; }
    if (v == 0 || v == ~0UL) return 0;
    int size = 64;
    for (; size > 2; size /= 2) {                     /* the smallest repeating element */
        unsigned long m = (1UL << (size / 2)) - 1;
        if ((v & m) != (v >> (size / 2) & m)) break;
    }
    unsigned long m = size == 64 ? ~0UL : (1UL << size) - 1, e = v & m;
    for (int r = 0; r < size; r++) {
        unsigned long x = r ? ((e >> r) | (e << (size - r))) & m : e;   /* rotated right by r */
        if (x & (x + 1)) continue;                     /* not ones at the bottom */
        int ones = __builtin_popcountl(x);
        if (ones == size) return 0;
        int immr = (size - r) % size;
        int imms = ((~(size - 1) << 1) | (ones - 1)) & 0x3f;
        int N = size == 64;
        *bits = N << 22 | immr << 16 | imms << 10;
        return 1;
    }
    return 0;
}

static int a64_cond(const char *s)
{
    static const char *c[] = { "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv" };
    if (!strcmp(s, "cs")) return 2;
    if (!strcmp(s, "cc")) return 3;
    for (int k = 0; k < 16; k++) if (!strcmp(s, c[k])) return k;
    return -1;
}

static void need_n(int nop, int n, const char *mn) { if (nop != n) aerr("%s: expected %d operands", mn, n); }
static void need_r(AOp *o, const char *mn) { if (o->kind != 'r') aerr("%s: expected a general register", mn); }

/* system registers: op0, op1, CRn, CRm, op2 (packed as op0<<14|op1<<11|CRn<<7|CRm<<3|op2) */
static int a64_sysreg(const char *name)
{
    static const struct { const char *n; int v; } t[] = {
#define SR(n, o0, o1, cn, cm, o2) { n, (o0) << 14 | (o1) << 11 | (cn) << 7 | (cm) << 3 | (o2) }
        SR("sctlr_el1", 3, 0, 1, 0, 0), SR("actlr_el1", 3, 0, 1, 0, 1), SR("cpacr_el1", 3, 0, 1, 0, 2),
        SR("ttbr0_el1", 3, 0, 2, 0, 0), SR("ttbr1_el1", 3, 0, 2, 0, 1), SR("tcr_el1", 3, 0, 2, 0, 2),
        SR("spsr_el1", 3, 0, 4, 0, 0), SR("elr_el1", 3, 0, 4, 0, 1), SR("sp_el0", 3, 0, 4, 1, 0),
        SR("spsel", 3, 0, 4, 2, 0), SR("currentel", 3, 0, 4, 2, 2), SR("daif", 3, 3, 4, 2, 1), SR("nzcv", 3, 3, 4, 2, 0),
        SR("fpcr", 3, 3, 4, 4, 0), SR("fpsr", 3, 3, 4, 4, 1),
        SR("afsr0_el1", 3, 0, 5, 1, 0), SR("afsr1_el1", 3, 0, 5, 1, 1), SR("esr_el1", 3, 0, 5, 2, 0), SR("far_el1", 3, 0, 6, 0, 0),
        SR("par_el1", 3, 0, 7, 4, 0), SR("mair_el1", 3, 0, 10, 2, 0), SR("amair_el1", 3, 0, 10, 3, 0),
        SR("vbar_el1", 3, 0, 12, 0, 0), SR("isr_el1", 3, 0, 12, 1, 0),
        SR("contextidr_el1", 3, 0, 13, 0, 1), SR("tpidr_el1", 3, 0, 13, 0, 4), SR("tpidr_el0", 3, 3, 13, 0, 2), SR("tpidrro_el0", 3, 3, 13, 0, 3),
        SR("midr_el1", 3, 0, 0, 0, 0), SR("mpidr_el1", 3, 0, 0, 0, 5), SR("revidr_el1", 3, 0, 0, 0, 6),
        SR("id_aa64pfr0_el1", 3, 0, 0, 4, 0), SR("id_aa64pfr1_el1", 3, 0, 0, 4, 1), SR("id_aa64dfr0_el1", 3, 0, 0, 5, 0),
        SR("id_aa64isar0_el1", 3, 0, 0, 6, 0), SR("id_aa64isar1_el1", 3, 0, 0, 6, 1),
        SR("id_aa64mmfr0_el1", 3, 0, 0, 7, 0), SR("id_aa64mmfr1_el1", 3, 0, 0, 7, 1), SR("id_aa64mmfr2_el1", 3, 0, 0, 7, 2),
        SR("ctr_el0", 3, 3, 0, 0, 1), SR("dczid_el0", 3, 3, 0, 0, 7), SR("clidr_el1", 3, 1, 0, 0, 1), SR("ccsidr_el1", 3, 1, 0, 0, 0),
        SR("csselr_el1", 3, 2, 0, 0, 0),
        SR("cntfrq_el0", 3, 3, 14, 0, 0), SR("cntpct_el0", 3, 3, 14, 0, 1), SR("cntvct_el0", 3, 3, 14, 0, 2),
        SR("cntp_tval_el0", 3, 3, 14, 2, 0), SR("cntp_ctl_el0", 3, 3, 14, 2, 1), SR("cntp_cval_el0", 3, 3, 14, 2, 2),
        SR("cntv_tval_el0", 3, 3, 14, 3, 0), SR("cntv_ctl_el0", 3, 3, 14, 3, 1), SR("cntv_cval_el0", 3, 3, 14, 3, 2),
        SR("cntkctl_el1", 3, 0, 14, 1, 0),
        SR("sctlr_el2", 3, 4, 1, 0, 0), SR("hcr_el2", 3, 4, 1, 1, 0), SR("cptr_el2", 3, 4, 1, 1, 2), SR("hstr_el2", 3, 4, 1, 1, 3),
        SR("spsr_el2", 3, 4, 4, 0, 0), SR("elr_el2", 3, 4, 4, 0, 1), SR("sp_el1", 3, 4, 4, 1, 0), SR("vbar_el2", 3, 4, 12, 0, 0),
        SR("cnthctl_el2", 3, 4, 14, 1, 0), SR("cntvoff_el2", 3, 4, 14, 0, 3), SR("vmpidr_el2", 3, 4, 0, 0, 5), SR("vpidr_el2", 3, 4, 0, 0, 0),
        SR("esr_el2", 3, 4, 5, 2, 0), SR("far_el2", 3, 4, 6, 0, 0),
        SR("scr_el3", 3, 6, 1, 1, 0), SR("spsr_el3", 3, 6, 4, 0, 0), SR("elr_el3", 3, 6, 4, 0, 1), SR("sctlr_el3", 3, 6, 1, 0, 0),
        SR("icc_sre_el1", 3, 0, 12, 12, 5), SR("icc_pmr_el1", 3, 0, 4, 6, 0), SR("icc_iar1_el1", 3, 0, 12, 12, 0),
        SR("icc_eoir1_el1", 3, 0, 12, 12, 1), SR("icc_igrpen1_el1", 3, 0, 12, 12, 7), SR("icc_bpr1_el1", 3, 0, 12, 12, 3),
        SR("icc_ctlr_el1", 3, 0, 12, 12, 4), SR("icc_sgi1r_el1", 3, 0, 12, 11, 5), SR("icc_sre_el2", 3, 4, 12, 9, 5),
        SR("pmcr_el0", 3, 3, 9, 12, 0), SR("pmccntr_el0", 3, 3, 9, 13, 0),
#undef SR
    };
    char low[40] = "";
    for (int k = 0; k < 39 && name[k]; k++) low[k] = name[k] | (name[k] >= 'A' && name[k] <= 'Z' ? 0x20 : 0);
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) if (!strcmp(low, t[k].n)) return t[k].v;
    static const char *pre[5] = { "s", "_", "_c", "_c", "_" };   /* generic: s<op0>_<op1>_c<n>_c<m>_<op2> */
    int f[5], max[5] = { 3, 7, 15, 15, 7 };
    const char *p = low;
    for (int k = 0; k < 5; k++) {
        size_t n = strlen(pre[k]);
        if (strncmp(p, pre[k], n) || p[n] < '0' || p[n] > '9') return -1;
        for (p += n, f[k] = 0; *p >= '0' && *p <= '9'; p++) f[k] = f[k] * 10 + *p - '0';
        if (f[k] > max[k]) return -1;
    }
    if (*p || f[0] < 2) return -1;
    return f[0] << 14 | f[1] << 11 | f[2] << 7 | f[3] << 3 | f[4];
}

/* SYS aliases: tlbi, dc, ic, at (op1, CRn, CRm, op2; 'r': takes a register) */
static int a64_sysop(const char *mn, const char *op, int *needreg)
{
    static const struct { const char *m, *o; int v, r; } t[] = {
#define SO(m, o, o1, cn, cm, o2, r) { m, o, (o1) << 11 | (cn) << 7 | (cm) << 3 | (o2), r }
        SO("tlbi", "vmalle1", 0, 8, 7, 0, 0), SO("tlbi", "vmalle1is", 0, 8, 3, 0, 0), SO("tlbi", "vae1", 0, 8, 7, 1, 1),
        SO("tlbi", "vae1is", 0, 8, 3, 1, 1), SO("tlbi", "aside1", 0, 8, 7, 2, 1), SO("tlbi", "aside1is", 0, 8, 3, 2, 1),
        SO("tlbi", "vaae1", 0, 8, 7, 3, 1), SO("tlbi", "vaae1is", 0, 8, 3, 3, 1), SO("tlbi", "vale1", 0, 8, 7, 5, 1),
        SO("tlbi", "vale1is", 0, 8, 3, 5, 1), SO("tlbi", "vaale1is", 0, 8, 3, 7, 1), SO("tlbi", "alle2", 4, 8, 7, 0, 0),
        SO("dc", "ivac", 0, 7, 6, 1, 1), SO("dc", "isw", 0, 7, 6, 2, 1), SO("dc", "csw", 0, 7, 10, 2, 1), SO("dc", "cisw", 0, 7, 14, 2, 1),
        SO("dc", "zva", 3, 7, 4, 1, 1), SO("dc", "cvac", 3, 7, 10, 1, 1), SO("dc", "cvau", 3, 7, 11, 1, 1), SO("dc", "civac", 3, 7, 14, 1, 1),
        SO("ic", "ialluis", 0, 7, 1, 0, 0), SO("ic", "iallu", 0, 7, 5, 0, 0), SO("ic", "ivau", 3, 7, 5, 1, 1),
        SO("at", "s1e1r", 0, 7, 8, 0, 1), SO("at", "s1e1w", 0, 7, 8, 1, 1), SO("at", "s1e0r", 0, 7, 8, 2, 1), SO("at", "s1e0w", 0, 7, 8, 3, 1),
#undef SO
    };
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++)
        if (!strcmp(mn, t[k].m) && !strcmp(op, t[k].o)) { *needreg = t[k].r; return t[k].v; }
    return -1;
}

static int a64_barrier(const char *s)
{
    static const struct { const char *n; int v; } t[] = {
        { "sy", 15 }, { "st", 14 }, { "ld", 13 }, { "ish", 11 }, { "ishst", 10 }, { "ishld", 9 },
        { "nsh", 7 }, { "nshst", 6 }, { "nshld", 5 }, { "osh", 3 }, { "oshst", 2 }, { "oshld", 1 },
    };
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) if (!strcmp(s, t[k].n)) return t[k].v;
    return -1;
}

/* a pc-relative branch field of `bits` bits (words), at bit `pos`: resolved here or by the linker */
static unsigned a64_branch(Expr *e, int bits, int pos, int rtype)
{
    Val v = evalf(e, C.sec, C.base);
    if (v.add && v.add->defined && v.add->sec == C.sec && !v.sub && !v.add->global && !v.add->weak) {
        long d = v.add->val + v.v - C.base;
        if (d & 3) aerr("branch target not 4-byte aligned");
        d >>= 2;
        if (d < -(1L << (bits - 1)) || d >= (1L << (bits - 1))) aerr("branch target out of range");
        return (unsigned)(d & ((1L << bits) - 1)) << pos;
    }
    if (!v.add) aerr("a branch to a constant address");
    a64_rel(rtype, v);
    return 0;
}

/* the 12-bit low part of an address: :lo12:sym (a relocation, scaled by the access) or a number */
static unsigned a64_lo12(AOp *o, int scale, int ldst)
{
    if (o->mod == AM_LO12) {
        Val v = evalf(o->e, C.sec, C.base);
        static const int ty[] = { 0, A_LDST8, A_LDST16, 0, A_LDST32, 0, 0, 0, A_LDST64, 0, 0, 0, 0, 0, 0, 0, A_LDST128 };
        a64_rel(ldst ? ty[scale] : A_ADD_LO12, v);
        return 0;
    }
    if (o->mod == AM_TPREL_LO12 || o->mod == AM_TPREL_LO12_NC) {
        a64_rel(o->mod == AM_TPREL_LO12 ? A_TPREL_LO12 : A_TPREL_LO12_NC, evalf(o->e, C.sec, C.base));
        return 0;
    }
    if (o->mod) aerr("this relocation modifier cannot be used here");
    long v = a64_const(o->e);
    if (v < 0 || v % scale || v / scale > 4095) return ~0u;
    return v / scale;
}

/* ---- loads and stores */
static void a64_ldst(const char *mn, AOp *ops, int nop)
{
    int load = mn[0] == 'l';
    int unscaled = mn[2] == 'u';                      /* ldur, sturb, ldursw...: always the 9-bit signed offset */
    const char *m = mn + 3 + unscaled;                /* after "ldr"/"str"/"ldur"/"stur": "", "b", "h", "sb", "sh", "sw" */
    if (nop < 2) aerr("%s: expected a register and an address", mn);
    AOp *t = &ops[0], *a = &ops[1];
    if (a->kind != 'm') aerr("%s: expected an address", mn);
    int size, opc, V = 0;
    if (t->kind == 'v') {
        V = 1;
        size = t->w == 16 ? 0 : t->w == 1 ? 0 : t->w == 2 ? 1 : t->w == 4 ? 2 : 3;
        opc = (t->w == 16 ? 2 : 0) | load;
    } else {
        need_r(t, mn);
        if (!*m) { size = t->w == 8 ? 3 : 2; opc = load; }
        else if (!strcmp(m, "b")) { size = 0; opc = load; }
        else if (!strcmp(m, "h")) { size = 1; opc = load; }
        else if (!strcmp(m, "sb")) { size = 0; opc = t->w == 8 ? 2 : 3; }
        else if (!strcmp(m, "sh")) { size = 1; opc = t->w == 8 ? 2 : 3; }
        else if (!strcmp(m, "sw")) { size = 2; opc = 2; }
        else aerr("unknown instruction %s", mn);
    }
    int scale = V && t->w == 16 ? 16 : 1 << size;
    unsigned base = (unsigned)size << 30 | 7u << 27 | V << 26 | opc << 22 | a->base << 5 | t->reg;
    if (nop == 3) {                                   /* post-index: [xn], #imm */
        long v = a64_const(ops[2].e);
        if (v < -256 || v > 255) aerr("%s: post-index offset out of range", mn);
        a64_put(base | (unsigned)(v & 0x1ff) << 12 | 1 << 10);
        return;
    }
    if (a->index >= 0) {                               /* register offset */
        int S = a->has_amt && (a->amt || scale == 1);  /* (a byte access: "lsl #0" written sets S, as in GNU as) */
        if (S && a->amt != __builtin_ctz(scale)) aerr("%s: shift must be %d", mn, __builtin_ctz(scale));
        a64_put(base | 1u << 21 | a->index << 16 | a->ext << 13 | S << 12 | 2 << 10);
        return;
    }
    if (a->pre) {
        long v = a64_const(a->e);
        if (v < -256 || v > 255) aerr("%s: pre-index offset out of range", mn);
        a64_put(base | (unsigned)(v & 0x1ff) << 12 | 3 << 10);
        return;
    }
    if (!a->e) { a64_put(base | 1u << 24); return; }
    if (a->mod) { a64_put(base | 1u << 24 | a64_lo12(a, scale, 1) << 10); return; }
    long v = a64_const(a->e);
    if (!unscaled && v >= 0 && v % scale == 0 && v / scale < 4096) { a64_put(base | 1u << 24 | (unsigned)(v / scale) << 10); return; }
    if (v >= -256 && v <= 255) { a64_put(base | (unsigned)(v & 0x1ff) << 12); return; }   /* ldur/stur */
    aerr("%s: offset %ld out of range", mn, v);
}

static void a64_pair(const char *mn, AOp *ops, int nop)
{
    int load = mn[0] == 'l';
    if (nop < 3 || ops[2].kind != 'm') aerr("%s: expected two registers and an address", mn);
    AOp *t1 = &ops[0], *t2 = &ops[1], *a = &ops[2];
    int V = t1->kind == 'v', opc, scale;
    if (V) { opc = t1->w == 4 ? 0 : t1->w == 8 ? 1 : 2; scale = t1->w; }
    else { need_r(t1, mn); opc = t1->w == 8 ? 2 : 0; scale = t1->w; }
    if (!strcmp(mn, "ldpsw")) { opc = 1; scale = 4; }
    long v = nop == 4 ? a64_const(ops[3].e) : a->e ? a64_const(a->e) : 0;
    if (v % scale || v / scale < -64 || v / scale > 63) aerr("%s: offset out of range", mn);
    int mode = nop == 4 ? 1 : a->pre ? 3 : 2;         /* post, pre, signed offset */
    a64_put((unsigned)opc << 30 | 5u << 27 | V << 26 | mode << 23 | load << 22 | (unsigned)((v / scale) & 0x7f) << 15 |
            t2->reg << 10 | a->base << 5 | t1->reg);
}

/* ---- the instructions */

/* ---- NEON (Advanced SIMD): the instructions the code generator's vector
 * operations and arm_neon.h use. Returns 0 when mn with these operands is
 * not one (then the scalar forms are tried). */
static int a64_sz(int es) { return es == 1 ? 0 : es == 2 ? 1 : es == 4 ? 2 : 3; }
static int a64_q(AOp *o) { return o->w == 16; }
static void need_v(AOp *o, const char *mn) { if (o->kind != 'V' || o->lane >= 0) aerr("%s: expected a vector (v0.4s...)", mn); }
static unsigned a64_imm5(AOp *o) { int l = a64_sz(o->es); return (unsigned)(o->lane << (l + 1) | 1 << l); }   /* an element's index */

static int a64_neon(const char *mn, AOp *ops, int nop)
{
    AOp *o0 = &ops[0], *o1 = &ops[1], *o2 = &ops[2], *o3 = &ops[3];
    int any = 0;
    for (int k = 0; k < nop; k++) if (ops[k].kind == 'V' || ops[k].kind == 'L') any = 1;
    if (!any && strcmp(mn, "movi")) return 0;
    char base[16];
    snprintf(base, sizeof base, "%s", mn);
    size_t bl = strlen(base);
    static const char *has2[] = {                    /* the families with a "2" form (the high halves, Q = 1) */
        "smull", "umull", "smlal", "umlal", "smlsl", "umlsl", "saddl", "uaddl", "saddw", "uaddw", "ssubl", "usubl", "ssubw",
        "usubw", "sabal", "uabal", "sabdl", "uabdl", "xtn", "sqxtn", "uqxtn", "sqxtun", "sxtl", "uxtl", "sshll", "ushll",
        "shrn", "rshrn", "sqshrn", "uqshrn", "sqshrun", "fcvtl", "fcvtn",
    };
    int two = 0;
    if (bl > 1 && base[bl - 1] == '2') {
        base[bl - 1] = 0;
        for (unsigned k = 0; k < sizeof has2 / sizeof has2[0]; k++) if (!strcmp(base, has2[k])) two = 1;
        if (!two) base[bl - 1] = '2';
    }
#define B(x) (!strcmp(base, x))
    unsigned Rd = o0->reg, Rn = nop > 1 ? o1->reg : 0, Rm = nop > 2 ? o2->reg : 0;
    /* three registers, the same arrangement: integers */
    static const struct { const char *n; int u, op; } i3[] = {
        { "add", 0, 0x10 }, { "sub", 1, 0x10 }, { "mul", 0, 0x13 }, { "mla", 0, 0x12 }, { "mls", 1, 0x12 },
        { "cmgt", 0, 6 }, { "cmhi", 1, 6 }, { "cmge", 0, 7 }, { "cmhs", 1, 7 }, { "cmeq", 1, 0x11 }, { "cmtst", 0, 0x11 },
        { "smax", 0, 0xc }, { "umax", 1, 0xc }, { "smin", 0, 0xd }, { "umin", 1, 0xd }, { "sqadd", 0, 1 }, { "uqadd", 1, 1 },
        { "sqsub", 0, 5 }, { "uqsub", 1, 5 }, { "srhadd", 0, 2 }, { "urhadd", 1, 2 }, { "sshl", 0, 8 }, { "ushl", 1, 8 },
        { "addp", 0, 0x17 }, { "smaxp", 0, 0x14 }, { "umaxp", 1, 0x14 }, { "sminp", 0, 0x15 }, { "uminp", 1, 0x15 },
        { "sabd", 0, 0xe }, { "uabd", 1, 0xe },
    };
    if (nop == 3 && o0->kind == 'V' && o2->kind == 'V' && o0->lane < 0 && o2->lane < 0)
        for (unsigned k = 0; k < sizeof i3 / sizeof i3[0]; k++)
            if (B(i3[k].n)) {
                a64_put(0x0e200400 | a64_q(o0) << 30 | i3[k].u << 29 | a64_sz(o0->es) << 22 | Rm << 16 | i3[k].op << 11 | Rn << 5 | Rd);
                return 1;
            }
    static const struct { const char *n; int u, sz; } lg[] = {    /* logical: the size field says which */
        { "and", 0, 0 }, { "bic", 0, 1 }, { "orr", 0, 2 }, { "orn", 0, 3 }, { "eor", 1, 0 }, { "bsl", 1, 1 }, { "bit", 1, 2 }, { "bif", 1, 3 },
    };
    if (nop == 3 && o0->kind == 'V' && o2->kind == 'V')
        for (unsigned k = 0; k < sizeof lg / sizeof lg[0]; k++)
            if (B(lg[k].n)) { a64_put(0x0e201c00 | a64_q(o0) << 30 | lg[k].u << 29 | lg[k].sz << 22 | Rm << 16 | Rn << 5 | Rd); return 1; }
    if (B("mov") && nop == 2 && o0->kind == 'V' && o1->kind == 'V' && o0->lane < 0 && o1->lane < 0) {   /* orr vd, vn, vn */
        a64_put(0x0ea01c00 | a64_q(o0) << 30 | Rn << 16 | Rn << 5 | Rd);
        return 1;
    }
    /* three registers, floating point (a: bit 23) */
    static const struct { const char *n; int u, a, op; } f3[] = {
        { "fadd", 0, 0, 0x1a }, { "fsub", 0, 1, 0x1a }, { "fmul", 1, 0, 0x1b }, { "fdiv", 1, 0, 0x1f }, { "fmla", 0, 0, 0x19 },
        { "fmls", 0, 1, 0x19 }, { "fmax", 0, 0, 0x1e }, { "fmin", 0, 1, 0x1e }, { "fmaxnm", 0, 0, 0x18 }, { "fminnm", 0, 1, 0x18 },
        { "fcmeq", 0, 0, 0x1c }, { "fcmge", 1, 0, 0x1c }, { "fcmgt", 1, 1, 0x1c }, { "faddp", 1, 0, 0x1a }, { "fmaxp", 1, 0, 0x1e },
        { "fminp", 1, 1, 0x1e }, { "fabd", 1, 1, 0x1a },
    };
    if (nop == 3 && o0->kind == 'V' && o1->kind == 'V' && o2->kind == 'V')
        for (unsigned k = 0; k < sizeof f3 / sizeof f3[0]; k++) {
            if (!B(f3[k].n)) continue;
            if (o2->lane >= 0) {                      /* by element: fmla/fmls/fmul v0.4s, v1.4s, v2.s[1] */
                unsigned op = B("fmla") ? 0x0f801000 : B("fmls") ? 0x0f805000 : B("fmul") ? 0x0f809000 : 0;
                if (!op) aerr("%s: no by-element form", mn);
                int d = o0->es == 8, H = d ? o2->lane : o2->lane >> 1, L = d ? 0 : o2->lane & 1;
                a64_put(op | a64_q(o0) << 30 | d << 22 | L << 21 | Rm << 16 | H << 11 | Rn << 5 | Rd);
                return 1;
            }
            a64_put(0x0e200400 | a64_q(o0) << 30 | f3[k].u << 29 | f3[k].a << 23 | (o0->es == 8) << 22 | Rm << 16 | f3[k].op << 11 | Rn << 5 | Rd);
            return 1;
        }
    /* two registers: integers (size: of the destination, or of the source for the widening ones) */
    static const struct { const char *n; int u, op, src, fix; } i2[] = {
        { "neg", 1, 0x0b, 0, -1 }, { "abs", 0, 0x0b, 0, -1 }, { "not", 1, 0x05, 0, 0 }, { "mvn", 1, 0x05, 0, 0 }, { "cnt", 0, 0x05, 0, 0 },
        { "rev64", 0, 0, 0, -1 }, { "rev32", 1, 0, 0, -1 }, { "rev16", 0, 1, 0, -1 }, { "cls", 0, 4, 0, -1 }, { "clz", 1, 4, 0, -1 },
        { "saddlp", 0, 2, 1, -1 }, { "uaddlp", 1, 2, 1, -1 }, { "sadalp", 0, 6, 1, -1 }, { "uadalp", 1, 6, 1, -1 },
        { "xtn", 0, 0x12, 0, -1 }, { "sqxtn", 0, 0x14, 0, -1 }, { "uqxtn", 1, 0x14, 0, -1 }, { "sqxtun", 1, 0x12, 0, -1 },
        { "cmgt", 0, 8, 0, -1 }, { "cmge", 1, 8, 0, -1 }, { "cmeq", 0, 9, 0, -1 }, { "cmle", 1, 9, 0, -1 }, { "cmlt", 0, 0xa, 0, -1 },
    };
    if ((nop == 2 || (nop == 3 && o2->kind == 'i')) && o0->kind == 'V' && o1->kind == 'V' && o0->lane < 0 && o1->lane < 0)
        for (unsigned k = 0; k < sizeof i2 / sizeof i2[0]; k++) {
            if (!B(i2[k].n)) continue;
            int sz = i2[k].fix >= 0 ? i2[k].fix : a64_sz(i2[k].src ? o1->es : o0->es);
            a64_put(0x0e200800 | a64_q(i2[k].src ? o1 : o0) << 30 | i2[k].u << 29 | sz << 22 | i2[k].op << 12 | Rn << 5 | Rd);
            return 1;
        }
    /* two registers, floating point */
    static const struct { const char *n; int u, a, op; } f2[] = {
        { "fneg", 1, 1, 0x0f }, { "fabs", 0, 1, 0x0f }, { "fsqrt", 1, 1, 0x1f }, { "scvtf", 0, 0, 0x1d }, { "ucvtf", 1, 0, 0x1d },
        { "fcvtzs", 0, 1, 0x1b }, { "fcvtzu", 1, 1, 0x1b }, { "fcvtns", 0, 0, 0x1a }, { "fcvtnu", 1, 0, 0x1a },
        { "fcvtms", 0, 0, 0x1b }, { "fcvtmu", 1, 0, 0x1b }, { "fcvtps", 0, 1, 0x1a }, { "fcvtpu", 1, 1, 0x1a },
        { "fcvtas", 0, 0, 0x1c }, { "fcvtau", 1, 0, 0x1c }, { "frintn", 0, 0, 0x18 }, { "frintm", 0, 0, 0x19 },
        { "frintp", 0, 1, 0x18 }, { "frintz", 0, 1, 0x19 }, { "frinta", 1, 0, 0x18 }, { "frintx", 1, 0, 0x19 },
        { "frecpe", 0, 1, 0x1d }, { "frsqrte", 1, 1, 0x1d },
    };
    if (nop == 2 && o0->kind == 'V' && o1->kind == 'V' && o0->lane < 0 && o1->lane < 0) {
        for (unsigned k = 0; k < sizeof f2 / sizeof f2[0]; k++)
            if (B(f2[k].n)) { a64_put(0x0e200800 | a64_q(o0) << 30 | f2[k].u << 29 | f2[k].a << 23 | (o0->es == 8) << 22 | f2[k].op << 12 | Rn << 5 | Rd); return 1; }
        if (B("fcvtl")) { a64_put(0x0e217800 | a64_q(o1) << 30 | (o0->es == 8) << 22 | Rn << 5 | Rd); return 1; }   /* (Q: the source's high half) */
        if (B("fcvtn")) { a64_put(0x0e216800 | a64_q(o0) << 30 | (o1->es == 8) << 22 | Rn << 5 | Rd); return 1; }
    }
    /* three registers, different widths: the narrow operand gives Q and the size */
    static const struct { const char *n; int u, op, wide; } d3[] = {
        { "saddl", 0, 0, 0 }, { "uaddl", 1, 0, 0 }, { "saddw", 0, 1, 1 }, { "uaddw", 1, 1, 1 }, { "ssubl", 0, 2, 0 }, { "usubl", 1, 2, 0 },
        { "ssubw", 0, 3, 1 }, { "usubw", 1, 3, 1 }, { "sabal", 0, 5, 0 }, { "uabal", 1, 5, 0 }, { "sabdl", 0, 7, 0 }, { "uabdl", 1, 7, 0 },
        { "smlal", 0, 8, 0 }, { "umlal", 1, 8, 0 }, { "smlsl", 0, 0xa, 0 }, { "umlsl", 1, 0xa, 0 }, { "smull", 0, 0xc, 0 }, { "umull", 1, 0xc, 0 },
    };
    if (nop == 3 && o1->kind == 'V' && o2->kind == 'V' && o2->lane < 0)
        for (unsigned k = 0; k < sizeof d3 / sizeof d3[0]; k++) {
            if (!B(d3[k].n)) continue;
            AOp *nw = d3[k].wide ? o2 : o1;
            if (two != a64_q(nw)) aerr("%s: the '2' form takes the high halves (16-byte sources)", mn);
            a64_put(0x0e200000 | a64_q(nw) << 30 | d3[k].u << 29 | a64_sz(nw->es) << 22 | Rm << 16 | d3[k].op << 12 | Rn << 5 | Rd);
            return 1;
        }
    if ((B("sdot") || B("udot")) && nop == 3) {       /* ARMv8.2 dot product: 4-byte groups into 32-bit sums */
        need_v(o0, mn); need_v(o1, mn);
        if (o2->lane < 0) { a64_put(0x0e809400 | a64_q(o0) << 30 | B("udot") << 29 | Rm << 16 | Rn << 5 | Rd); return 1; }
        if (o2->lane > 3) aerr("%s: element index 0-3", mn);
        a64_put(0x0f80e000 | a64_q(o0) << 30 | B("udot") << 29 | (o2->lane & 1) << 21 | Rm << 16 | (o2->lane >> 1) << 11 | Rn << 5 | Rd);
        return 1;
    }
    /* shifts by an immediate */
    static const struct { const char *n; int u, op, left, src; } sh[] = {
        { "sshr", 0, 0, 0, 0 }, { "ushr", 1, 0, 0, 0 }, { "ssra", 0, 2, 0, 0 }, { "usra", 1, 2, 0, 0 }, { "srshr", 0, 4, 0, 0 },
        { "urshr", 1, 4, 0, 0 }, { "shl", 0, 0xa, 1, 0 }, { "sli", 1, 0xa, 1, 0 }, { "sri", 1, 8, 0, 0 }, { "sqshl", 0, 0xe, 1, 0 },
        { "uqshl", 1, 0xe, 1, 0 }, { "sshll", 0, 0x14, 1, 1 }, { "ushll", 1, 0x14, 1, 1 }, { "sxtl", 0, 0x14, 1, 1 }, { "uxtl", 1, 0x14, 1, 1 },
        { "shrn", 0, 0x10, 0, 0 }, { "rshrn", 0, 0x11, 0, 0 }, { "sqshrn", 0, 0x12, 0, 0 }, { "uqshrn", 1, 0x12, 0, 0 }, { "sqshrun", 1, 0x10, 0, 0 },
    };
    for (unsigned k = 0; k < sizeof sh / sizeof sh[0]; k++) {
        if (!B(sh[k].n) || o0->kind != 'V' || o1->kind != 'V') continue;
        int ext = B("sxtl") || B("uxtl");
        if (nop != (ext ? 2 : 3) || (!ext && o2->kind != 'i')) aerr("%s: bad operands", mn);
        long amt = ext ? 0 : a64_const(o2->e);
        AOp *e = sh[k].src ? o1 : o0;                 /* the element size: the source's for the widening ones */
        int bits = e->es * 8;
        if (sh[k].op >= 0x10 && sh[k].op <= 0x12 && !sh[k].src) { e = o0; bits = o0->es * 8; }   /* narrowing: the destination's */
        if (sh[k].left ? amt < 0 || amt >= bits : amt < 1 || amt > bits) aerr("%s: shift out of range", mn);
        unsigned hb = sh[k].left ? bits + amt : 2 * bits - amt;
        AOp *qo = sh[k].src ? o1 : o0;
        a64_put(0x0f000400 | a64_q(qo) << 30 | sh[k].u << 29 | hb << 16 | sh[k].op << 11 | Rn << 5 | Rd);
        return 1;
    }
    static const struct { const char *n; int op; } pm[] = {
        { "uzp1", 1 }, { "trn1", 2 }, { "zip1", 3 }, { "uzp2", 5 }, { "trn2", 6 }, { "zip2", 7 },
    };
    for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
        if (!strcmp(mn, pm[k].n)) {
            need_n(nop, 3, mn); need_v(o0, mn);
            a64_put(0x0e000800 | a64_q(o0) << 30 | a64_sz(o0->es) << 22 | Rm << 16 | pm[k].op << 12 | Rn << 5 | Rd);
            return 1;
        }
    if (B("ext")) {
        need_n(nop, 4, mn);
        long i = a64_const(o3->e);
        if (i < 0 || i >= o0->w) aerr("ext: index out of range");
        a64_put(0x2e000000 | a64_q(o0) << 30 | Rm << 16 | (unsigned)i << 11 | Rn << 5 | Rd);
        return 1;
    }
    if (B("tbl") || B("tbx")) {
        need_n(nop, 3, mn);
        if (o1->kind != 'L') aerr("%s: expected a table {v1.16b...}", mn);
        a64_put(0x0e000000 | a64_q(o0) << 30 | Rm << 16 | (unsigned)(o1->nreg - 1) << 13 | B("tbx") << 12 | Rn << 5 | Rd);
        return 1;
    }
    if (B("dup") && nop == 2 && o0->kind == 'V') {
        if (o1->kind == 'V' && o1->lane >= 0) { a64_put(0x0e000400 | a64_q(o0) << 30 | a64_imm5(o1) << 16 | Rn << 5 | Rd); return 1; }
        if (o1->kind == 'r') { AOp e = *o0; e.lane = 0; a64_put(0x0e000c00 | a64_q(o0) << 30 | a64_imm5(&e) << 16 | Rn << 5 | Rd); return 1; }
    }
    if ((B("umov") || B("smov") || B("mov")) && nop == 2 && o0->kind == 'r' && o1->kind == 'V' && o1->lane >= 0) {
        int sm = B("smov");
        a64_put((sm ? 0x0e002c00 : 0x0e003c00) | (o0->w == 8) << 30 | a64_imm5(o1) << 16 | Rn << 5 | Rd);
        return 1;
    }
    if ((B("mov") || B("ins")) && nop == 2 && o0->kind == 'V' && o0->lane >= 0) {
        if (o1->kind == 'r') { a64_put(0x4e001c00 | a64_imm5(o0) << 16 | Rn << 5 | Rd); return 1; }
        if (o1->kind == 'V' && o1->lane >= 0) {
            a64_put(0x6e000400 | a64_imm5(o0) << 16 | (unsigned)(o1->lane << a64_sz(o1->es)) << 11 | Rn << 5 | Rd);
            return 1;
        }
    }
    if (B("movi") && nop >= 2 && o0->kind == 'V' && o1->kind == 'i') {
        long v = a64_const(o1->e);
        if (o0->es == 1 && v >= 0 && v < 256) { a64_put(0x0f00e400 | a64_q(o0) << 30 | (unsigned)(v >> 5) << 16 | (unsigned)(v & 31) << 5 | Rd); return 1; }
        if (o0->es == 4 && v >= 0 && v < 256 && nop == 2) { a64_put(0x0f000400 | a64_q(o0) << 30 | (unsigned)(v >> 5) << 16 | (unsigned)(v & 31) << 5 | Rd); return 1; }
        if (o0->es == 8 && v == 0 && a64_q(o0)) { a64_put(0x6f00e400 | Rd); return 1; }
        aerr("movi: unsupported immediate");
    }
    /* across all lanes, and scalar pairwise adds */
    static const struct { const char *n; int u, op; } av[] = {
        { "addv", 0, 0x1b }, { "saddlv", 0, 3 }, { "uaddlv", 1, 3 }, { "smaxv", 0, 0xa }, { "umaxv", 1, 0xa }, { "sminv", 0, 0x1a }, { "uminv", 1, 0x1a },
    };
    if (nop == 2 && o0->kind == 'v' && o1->kind == 'V') {
        for (unsigned k = 0; k < sizeof av / sizeof av[0]; k++)
            if (B(av[k].n)) { a64_put(0x0e300800 | a64_q(o1) << 30 | av[k].u << 29 | a64_sz(o1->es) << 22 | av[k].op << 12 | Rn << 5 | Rd); return 1; }
        if (B("addp") && o1->es == 8) { a64_put(0x5ef1b800 | Rn << 5 | Rd); return 1; }
        if (B("faddp")) { a64_put(0x7e30d800 | (o1->es == 8) << 22 | Rn << 5 | Rd); return 1; }
    }
    /* loads and stores of whole registers: ld1/st1 {list}, [xn] */
    if ((B("ld1") || B("st1")) && nop == 2 && o0->kind == 'L' && o1->kind == 'm' && !o1->e && o1->index < 0) {
        static const int op[] = { 0, 7, 10, 6, 2 };
        a64_put(0x0c000000 | a64_q(o0) << 30 | B("ld1") << 22 | (unsigned)op[o0->nreg] << 12 | (unsigned)a64_sz(o0->es) << 10 | (unsigned)o1->base << 5 | Rd);
        return 1;
    }
#undef B
    aerr("unknown NEON instruction %s", mn);
    return 0;
}

static void a64_insn(Item *it)
{
    const char *mn = it->mn;
    AOp ops[5];
    int nop = 0;
    if (it->str && *it->str) {
        Vec v = { 0 };
        a64_split(it->str, &v);
        if (v.n > 5) aerr("too many operands");
        for (int k = 0; k < v.n; k++) a64_operand(v.v[k], &ops[k]);
        nop = v.n;
    }
#define IS(x) (!strcmp(mn, x))
    AOp *o0 = &ops[0], *o1 = &ops[1], *o2 = &ops[2], *o3 = &ops[3];
    int sf = nop && o0->kind == 'r' && o0->w == 8;
    if (a64_neon(mn, ops, nop)) return;
    /* fixed words */
    static const struct { const char *n; unsigned w; } fixed[] = {
        { "nop", 0xd503201f }, { "yield", 0xd503203f }, { "wfe", 0xd503205f }, { "wfi", 0xd503207f }, { "sev", 0xd503209f },
        { "sevl", 0xd50320bf }, { "eret", 0xd69f03e0 }, { "clrex", 0xd503305f }, { "drps", 0xd6bf03e0 },
    };
    for (unsigned k = 0; k < sizeof fixed / sizeof fixed[0]; k++) if (IS(fixed[k].n) && !nop) { a64_put(fixed[k].w); return; }
    if (IS("isb")) { a64_put(0xd5033fdf); return; }
    if (IS("dsb") || IS("dmb")) {
        need_n(nop, 1, mn);
        int c = o0->kind == 'n' ? a64_barrier(o0->name) : a64_const(o0->e);
        if (c < 0) aerr("%s: unknown option", mn);
        a64_put(0xd503309f | c << 8 | (IS("dmb") ? 0x20 : 0));
        return;
    }
    if (IS("svc") || IS("hvc") || IS("smc") || IS("brk") || IS("hlt") || IS("udf")) {
        long v = nop ? a64_const(o0->e) : 0;
        if (v < 0 || v > 0xffff) aerr("%s: immediate out of range", mn);
        a64_put(IS("udf") ? (unsigned)v : IS("svc") ? 0xd4000001 | v << 5 : IS("hvc") ? 0xd4000002 | v << 5 : IS("smc") ? 0xd4000003 | v << 5 :
                IS("brk") ? 0xd4200000 | v << 5 : 0xd4400000 | v << 5);
        return;
    }
    if (IS("mrs")) {
        need_n(nop, 2, mn); need_r(o0, mn);
        int s = o1->kind == 'n' ? a64_sysreg(o1->name) : -1;
        if (s < 0) aerr("mrs: unknown system register");
        a64_put(0xd5300000 | (s & 0x7fff) << 5 | o0->reg);
        return;
    }
    if (IS("msr")) {
        need_n(nop, 2, mn);
        if (o0->kind == 'n' && (!strcmp(o0->name, "daifset") || !strcmp(o0->name, "daifclr") || !strcmp(o0->name, "spsel"))) {
            long v = a64_const(o1->e);
            int op1 = !strcmp(o0->name, "spsel") ? 0 : 3, op2 = !strcmp(o0->name, "daifset") ? 6 : !strcmp(o0->name, "daifclr") ? 7 : 5;
            a64_put(0xd500401f | op1 << 16 | (unsigned)(v & 15) << 8 | op2 << 5);
            return;
        }
        int s = o0->kind == 'n' ? a64_sysreg(o0->name) : -1;
        if (s < 0) aerr("msr: unknown system register");
        need_r(o1, mn);
        a64_put(0xd5100000 | (s & 0x7fff) << 5 | o1->reg);
        return;
    }
    if (IS("tlbi") || IS("dc") || IS("ic") || IS("at")) {
        int need;
        int s = nop && o0->kind == 'n' ? a64_sysop(mn, o0->name, &need) : -1;
        if (s < 0) aerr("%s: unknown operation", mn);
        if (need && (nop != 2 || o1->kind != 'r')) aerr("%s %s: needs a register", mn, o0->name);
        a64_put(0xd5080000 | s << 5 | (need ? o1->reg : 31));
        return;
    }
    /* branches */
    if (IS("b") || IS("bl")) {
        need_n(nop, 1, mn);
        if (!o0->e) aerr("%s: expected a label", mn);
        a64_put((IS("bl") ? 0x94000000 : 0x14000000) | a64_branch(o0->e, 26, 0, IS("bl") ? A_CALL26 : A_JUMP26));
        return;
    }
    if (!strncmp(mn, "b.", 2)) {
        int c = a64_cond(mn + 2);
        if (c < 0 || nop != 1 || !o0->e) aerr("bad conditional branch %s", mn);
        a64_put(0x54000000 | a64_branch(o0->e, 19, 5, A_CONDBR19) | c);
        return;
    }
    if (IS("cbz") || IS("cbnz")) {
        need_n(nop, 2, mn); need_r(o0, mn);
        a64_put((unsigned)sf << 31 | 0x34000000 | IS("cbnz") << 24 | a64_branch(o1->e, 19, 5, A_CONDBR19) | o0->reg);
        return;
    }
    if (IS("tbz") || IS("tbnz")) {
        need_n(nop, 3, mn); need_r(o0, mn);
        long b = a64_const(o1->e);
        if (b < 0 || b > 63) aerr("%s: bit number out of range", mn);
        a64_put((unsigned)(b >> 5) << 31 | 0x36000000 | IS("tbnz") << 24 | (unsigned)(b & 31) << 19 | a64_branch(o2->e, 14, 5, A_TSTBR14) | o0->reg);
        return;
    }
    if (IS("br") || IS("blr") || IS("ret")) {
        int r = nop ? (need_r(o0, mn), o0->reg) : 30;
        a64_put((IS("br") ? 0xd61f0000 : IS("blr") ? 0xd63f0000 : 0xd65f0000) | r << 5);
        return;
    }
    if (IS("adr") || IS("adrp")) {
        need_n(nop, 2, mn); need_r(o0, mn);
        Expr *e = o1->e;
        if (!e) aerr("%s: expected a label", mn);
        unsigned w = (IS("adrp") ? 0x90000000 : 0x10000000) | o0->reg;
        Val v = evalf(e, C.sec, C.base);
        if (IS("adr") && v.add && v.add->defined && v.add->sec == C.sec && !v.sub && !v.add->global) {
            long d = v.add->val + v.v - C.base;
            if (d < -(1L << 20) || d >= (1L << 20)) aerr("adr: target out of range");
            a64_put(w | (unsigned)(d & 3) << 29 | (unsigned)((d >> 2) & 0x7ffff) << 5);
            return;
        }
        if (!v.add) aerr("%s: expected a label", mn);
        a64_rel(IS("adrp") ? A_ADR_PAGE : A_ADR_LO21, v);
        a64_put(w);
        return;
    }
    /* loads and stores */
    if (IS("ldr") || IS("str") || IS("ldrb") || IS("strb") || IS("ldrh") || IS("strh") || IS("ldrsb") || IS("ldrsh") || IS("ldrsw") ||
        IS("ldur") || IS("stur") || IS("ldurb") || IS("sturb") || IS("ldurh") || IS("sturh") || IS("ldursb") || IS("ldursh") || IS("ldursw")) {
        a64_ldst(mn, ops, nop);
        return;
    }
    if (IS("ldp") || IS("stp") || IS("ldpsw")) { a64_pair(mn, ops, nop); return; }
    {                                                  /* exclusive and ordered: size|001000|o2|L|o1|Rs|o0|11111|Rn|Rt */
        static const struct { const char *n; int sz, o2, L, o0, st; } ex[] = {
            { "ldxr", -1, 0, 1, 0, 0 }, { "ldaxr", -1, 0, 1, 1, 0 }, { "stxr", -1, 0, 0, 0, 1 }, { "stlxr", -1, 0, 0, 1, 1 },
            { "ldxrb", 0, 0, 1, 0, 0 }, { "ldaxrb", 0, 0, 1, 1, 0 }, { "stxrb", 0, 0, 0, 0, 1 }, { "stlxrb", 0, 0, 0, 1, 1 },
            { "ldxrh", 1, 0, 1, 0, 0 }, { "ldaxrh", 1, 0, 1, 1, 0 }, { "stxrh", 1, 0, 0, 0, 1 }, { "stlxrh", 1, 0, 0, 1, 1 },
            { "ldar", -1, 1, 1, 1, 0 }, { "stlr", -1, 1, 0, 1, 0 }, { "ldarb", 0, 1, 1, 1, 0 }, { "stlrb", 0, 1, 0, 1, 0 },
            { "ldarh", 1, 1, 1, 1, 0 }, { "stlrh", 1, 1, 0, 1, 0 },
        };
        for (unsigned k = 0; k < sizeof ex / sizeof ex[0]; k++) {
            if (!IS(ex[k].n)) continue;
            AOp *rs = ex[k].st ? o0 : 0, *rt = ex[k].st ? o1 : o0, *a = ex[k].st ? o2 : o1;
            if (nop != (ex[k].st ? 3 : 2) || a->kind != 'm' || rt->kind != 'r') aerr("%s: bad operands", mn);
            if (a->e && a64_const(a->e)) aerr("%s: no offset allowed", mn);
            int sz = ex[k].sz >= 0 ? ex[k].sz : rt->w == 8 ? 3 : 2;
            a64_put((unsigned)sz << 30 | 0x08000000 | ex[k].o2 << 23 | ex[k].L << 22 | (rs ? rs->reg : 31) << 16 | ex[k].o0 << 15 |
                    31 << 10 | a->base << 5 | rt->reg);
            return;
        }
    }
    {                                                  /* LSE (ARMv8.1): ld<op>[a][l][b|h] s, t, [n]; swp...; cas[a][l][b|h] s, t, [n] */
        static const struct { const char *n; unsigned w; } at[] = {
            { "ldadd", 0x38200000 }, { "ldclr", 0x38201000 }, { "ldeor", 0x38202000 }, { "ldset", 0x38203000 },
            { "swp", 0x38208000 }, { "cas", 0x08a07c00 },
        };
        for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
            size_t n = strlen(at[k].n);
            if (strncmp(mn, at[k].n, n)) continue;
            const char *r = mn + n;
            int A = *r == 'a' ? (r++, 1) : 0, L = *r == 'l' ? (r++, 1) : 0, sz = -1;
            if (*r == 'b' || *r == 'h') sz = *r++ == 'b' ? 0 : 1;
            if (*r) break;
            need_n(nop, 3, mn);
            need_r(o0, mn); need_r(o1, mn);
            if (o2->kind != 'm' || (o2->e && a64_const(o2->e))) aerr("%s: expected [xn]", mn);
            if (sz < 0) sz = o1->w == 8 ? 3 : 2;
            unsigned acq = k == 5 ? (unsigned)A << 22 | (unsigned)L << 15 : (unsigned)A << 23 | (unsigned)L << 22;   /* (cas: L, o0) */
            a64_put((unsigned)sz << 30 | at[k].w | acq | o0->reg << 16 | o2->base << 5 | o1->reg);
            return;
        }
    }
    /* floating point */
    if (mn[0] == 'f' || IS("scvtf") || IS("ucvtf")) {
        int ty = nop && o0->kind == 'v' ? (o0->w == 4 ? 0 : 1) : nop > 1 && o1->kind == 'v' ? (o1->w == 4 ? 0 : 1) : 0;
        if (IS("fmov")) {
            need_n(nop, 2, mn);
            if (o0->kind == 'v' && o1->kind == 'v') { a64_put(0x1e204000 | ty << 22 | o1->reg << 5 | o0->reg); return; }
            if (o0->kind == 'v' && o1->kind == 'r') { a64_put((o1->w == 8 ? 0x9e670000 : 0x1e270000) | o1->reg << 5 | o0->reg); return; }
            if (o0->kind == 'r' && o1->kind == 'v') { a64_put((o0->w == 8 ? 0x9e660000 : 0x1e260000) | o1->reg << 5 | o0->reg); return; }
            aerr("fmov: unsupported operands");
        }
        static const struct { const char *n; unsigned w; } f2[] = {
            { "fmul", 0x1e200800 }, { "fdiv", 0x1e201800 }, { "fadd", 0x1e202800 }, { "fsub", 0x1e203800 },
            { "fmax", 0x1e204800 }, { "fmin", 0x1e205800 }, { "fmaxnm", 0x1e206800 }, { "fminnm", 0x1e207800 },
            { "fnmul", 0x1e208800 },
        };
        for (unsigned k = 0; k < sizeof f2 / sizeof f2[0]; k++)
            if (IS(f2[k].n)) { need_n(nop, 3, mn); a64_put(f2[k].w | ty << 22 | o2->reg << 16 | o1->reg << 5 | o0->reg); return; }
        static const struct { const char *n; unsigned w; } f3[] = {          /* d = a +- n * m: fmadd d, n, m, a */
            { "fmadd", 0x1f000000 }, { "fmsub", 0x1f008000 }, { "fnmadd", 0x1f200000 }, { "fnmsub", 0x1f208000 },
        };
        for (unsigned k = 0; k < sizeof f3 / sizeof f3[0]; k++)
            if (IS(f3[k].n)) { need_n(nop, 4, mn); a64_put(f3[k].w | ty << 22 | o2->reg << 16 | o3->reg << 10 | o1->reg << 5 | o0->reg); return; }
        static const struct { const char *n; unsigned w; } f1[] = {
            { "fabs", 0x1e20c000 }, { "fneg", 0x1e214000 }, { "fsqrt", 0x1e21c000 },
            { "frintn", 0x1e244000 }, { "frintp", 0x1e24c000 }, { "frintm", 0x1e254000 }, { "frintz", 0x1e25c000 },
            { "frinta", 0x1e264000 }, { "frintx", 0x1e274000 }, { "frinti", 0x1e27c000 },
        };
        for (unsigned k = 0; k < sizeof f1 / sizeof f1[0]; k++)
            if (IS(f1[k].n)) { need_n(nop, 2, mn); a64_put(f1[k].w | ty << 22 | o1->reg << 5 | o0->reg); return; }
        if (IS("fcvt")) {
            need_n(nop, 2, mn);
            int from = o1->w == 4 ? 0 : 1, to = o0->w == 4 ? 0 : 1;
            a64_put(0x1e224000 | from << 22 | to << 15 | o1->reg << 5 | o0->reg);
            return;
        }
        if (IS("fcmp") || IS("fcmpe")) {
            need_n(nop, 2, mn);
            int e = IS("fcmpe") << 4;
            if (o1->kind == 'i') a64_put(0x1e202008 | ty << 22 | o0->reg << 5 | e);
            else a64_put(0x1e202000 | ty << 22 | o1->reg << 16 | o0->reg << 5 | e);
            return;
        }
        if (IS("scvtf") || IS("ucvtf")) {
            need_n(nop, 2, mn);
            a64_put((o1->w == 8 ? 0x80000000 : 0) | 0x1e220000 | (o0->w == 4 ? 0 : 1) << 22 | IS("ucvtf") << 16 | o1->reg << 5 | o0->reg);
            return;
        }
        if (!strncmp(mn, "fcvt", 4) && strlen(mn) == 6 && strchr("npmza", mn[4]) && strchr("su", mn[5])) {
            need_n(nop, 2, mn);                        /* to an integer, rounding: to nearest (even, or away), +inf, -inf, zero */
            unsigned rmode = mn[4] == 'p' ? 1 : mn[4] == 'm' ? 2 : mn[4] == 'z' ? 3 : 0, op = (mn[4] == 'a') << 2 | (mn[5] == 'u');
            a64_put((o0->w == 8 ? 0x80000000 : 0) | 0x1e200000 | (o1->w == 4 ? 0 : 1) << 22 | rmode << 19 | op << 16 | o1->reg << 5 | o0->reg);
            return;
        }
        if (IS("fccmp") || IS("fccmpe")) {           /* fccmp n, m, #nzcv, cond */
            need_n(nop, 4, mn);
            a64_put(0x1e200400 | ty << 22 | o1->reg << 16 | a64_cond(o3->name) << 12 | o0->reg << 5 | IS("fccmpe") << 4 |
                    (unsigned)(a64_const(o2->e) & 15));
            return;
        }
        if (IS("fcsel")) {
            need_n(nop, 4, mn);
            int c = a64_cond(o3->name);
            a64_put(0x1e200c00 | ty << 22 | o2->reg << 16 | c << 12 | o1->reg << 5 | o0->reg);
            return;
        }
        aerr("unknown instruction %s", mn);
    }
    /* everything else works on general registers */
    if (!nop) aerr("unknown instruction %s", mn);
    need_r(o0, mn);
    int Rd = o0->reg;
    if (IS("mov") || IS("mvn") || IS("neg") || IS("negs") || IS("ngc") || IS("ngcs")) {
        unsigned shift = 0;                            /* mvn, neg, negs: an optional shift of the operand */
        if (nop == 3 && !IS("mov") && o2->kind == 's') {
            const char *k = o2->name;
            shift = (unsigned)(!strcmp(k, "lsl") ? 0 : !strcmp(k, "lsr") ? 1 : !strcmp(k, "asr") ? 2 : 3) << 22 | (unsigned)o2->amt << 10;
            nop = 2;
        }
        need_n(nop, 2, mn);
        if (IS("mov") && o1->kind == 'i') {           /* one instruction: movz, movn, or a logical immediate */
            long v = a64_const(o1->e);
            unsigned long u = sf ? (unsigned long)v : (unsigned long)(unsigned)v;
            int n = sf ? 4 : 2;
            for (int k = 0; k < n; k++)
                if (!(u & ~(0xffffUL << 16 * k))) { a64_put((unsigned)sf << 31 | 0x52800000 | k << 21 | (unsigned)(u >> 16 * k & 0xffff) << 5 | Rd); return; }
            unsigned long nu = sf ? ~u : ~u & 0xffffffffUL;
            for (int k = 0; k < n; k++)
                if (!(nu & ~(0xffffUL << 16 * k))) { a64_put((unsigned)sf << 31 | 0x12800000 | k << 21 | (unsigned)(nu >> 16 * k & 0xffff) << 5 | Rd); return; }
            unsigned bits;
            if (a64_bitmask(u, sf ? 8 : 4, &bits)) { a64_put((unsigned)sf << 31 | 0x32000000 | bits | 31 << 5 | Rd); return; }
            aerr("mov: the immediate %ld needs several instructions", v);
        }
        need_r(o1, mn);
        if (IS("mov")) {
            if (o0->sp || o1->sp) a64_put((unsigned)sf << 31 | 0x11000000 | o1->reg << 5 | Rd);    /* add rd, rn, #0 */
            else a64_put((unsigned)sf << 31 | 0x2a000000 | o1->reg << 16 | 31 << 5 | Rd);          /* orr rd, zr, rm */
            return;
        }
        if (IS("mvn")) { a64_put((unsigned)sf << 31 | 0x2a200000 | shift | o1->reg << 16 | 31 << 5 | Rd); return; }
        if (IS("neg") || IS("negs")) { a64_put((unsigned)sf << 31 | 0x4b000000 | IS("negs") << 29 | shift | o1->reg << 16 | 31 << 5 | Rd); return; }
        a64_put((unsigned)sf << 31 | 0x5a000000 | IS("ngcs") << 29 | o1->reg << 16 | 31 << 5 | Rd);
        return;
    }
    if (IS("movz") || IS("movn") || IS("movk")) {
        long v = a64_const(o1->e), sh = nop > 2 && o2->kind == 's' ? o2->amt : 0;
        if (v < 0 || v > 0xffff || sh % 16 || sh / 16 >= (sf ? 4 : 2)) aerr("%s: bad immediate", mn);
        int opc = IS("movn") ? 0 : IS("movz") ? 2 : 3;
        a64_put((unsigned)sf << 31 | opc << 29 | 0x12800000 | (unsigned)(sh / 16) << 21 | (unsigned)v << 5 | Rd);
        return;
    }
    /* add, sub (immediate, shifted or extended register) and their compare aliases */
    int cmp = IS("cmp") || IS("cmn"), tst = IS("tst");
    if (IS("add") || IS("adds") || IS("sub") || IS("subs") || cmp) {
        int op = IS("sub") || IS("subs") || IS("cmp"), S = IS("adds") || IS("subs") || cmp;
        AOp *n = cmp ? o0 : o1, *m = cmp ? o1 : o2, *shf = cmp ? o2 : o3;
        int nn = cmp ? nop + 1 : nop;
        if (cmp) Rd = 31;
        sf = n->w == 8;
        if (nn < 3) aerr("%s: expected more operands", mn);
        if (m->kind == 'i') {
            unsigned imm;
            int sh = 0;
            if (m->mod == AM_LO12) imm = a64_lo12(m, 1, 0);
            else if (m->mod == AM_TPREL_HI12) { a64_rel(A_TPREL_HI12, evalf(m->e, C.sec, C.base)); imm = 0; sh = 1; }
            else if (m->mod) imm = a64_lo12(m, 1, 0);
            else {
                long v = a64_const(m->e);
                if (nn > 3 && shf->kind == 's' && !strcmp(shf->name, "lsl") && shf->amt == 12) v <<= 12;
                if (v < 0) { v = -v; op ^= 1; }
                if (v < 4096) imm = v;
                else if (!(v & 0xfff) && v < (1L << 24)) { imm = v >> 12; sh = 1; }
                else aerr("%s: immediate %ld out of range", mn, v);
            }
            if (nn > 3 && shf->kind == 's' && shf->amt == 12) sh = 1;
            a64_put((unsigned)sf << 31 | op << 30 | S << 29 | 0x11000000 | sh << 22 | imm << 10 | n->reg << 5 | Rd);
            return;
        }
        need_r(m, mn);
        int ext = n->sp || (!cmp && o0->sp) || (nn > 3 && shf->kind == 's' && strchr(shf->name, 'x') && shf->name[0] != 'l');
        if (ext || (nn > 3 && shf->kind == 's' && (shf->name[0] == 'u' || shf->name[0] == 's'))) {   /* extended register */
            int option = 3, amt = 0;                   /* uxtx (lsl) */
            if (nn > 3 && shf->kind == 's') {
                static const char *ex[] = { "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx" };
                for (int k = 0; k < 8; k++) if (!strcmp(shf->name, ex[k])) option = k;
                amt = shf->amt;
            }
            if (m->w == 4 && option == 3) option = 2;
            a64_put((unsigned)sf << 31 | op << 30 | S << 29 | 0x0b200000 | m->reg << 16 | option << 13 | amt << 10 | n->reg << 5 | Rd);
            return;
        }
        int st = 0, amt = 0;
        if (nn > 3 && shf->kind == 's') { st = !strcmp(shf->name, "lsl") ? 0 : !strcmp(shf->name, "lsr") ? 1 : !strcmp(shf->name, "asr") ? 2 : 3; amt = shf->amt; }
        a64_put((unsigned)sf << 31 | op << 30 | S << 29 | 0x0b000000 | st << 22 | m->reg << 16 | amt << 10 | n->reg << 5 | Rd);
        return;
    }
    /* logical */
    {
        static const struct { const char *n; int opc, N; } lg[] = {
            { "and", 0, 0 }, { "bic", 0, 1 }, { "orr", 1, 0 }, { "orn", 1, 1 }, { "eor", 2, 0 }, { "eon", 2, 1 }, { "ands", 3, 0 }, { "bics", 3, 1 }, { "tst", 3, 0 },
        };
        for (unsigned k = 0; k < sizeof lg / sizeof lg[0]; k++) {
            if (!IS(lg[k].n)) continue;
            AOp *n = tst ? o0 : o1, *m = tst ? o1 : o2, *shf = tst ? o2 : o3;
            int nn = tst ? nop + 1 : nop;
            if (tst) Rd = 31;
            if (nn < 3) aerr("%s: expected more operands", mn);
            sf = n->w == 8;
            if (m->kind == 'i') {
                unsigned bits;
                long v = a64_const(m->e);
                if (lg[k].N) v = ~v;
                if (!a64_bitmask((unsigned long)v, sf ? 8 : 4, &bits)) aerr("%s: the immediate %ld cannot be encoded", mn, v);
                a64_put((unsigned)sf << 31 | lg[k].opc << 29 | 0x12000000 | bits | n->reg << 5 | Rd);
                return;
            }
            need_r(m, mn);
            int st = 0, amt = 0;
            if (nn > 3 && shf->kind == 's') { st = !strcmp(shf->name, "lsl") ? 0 : !strcmp(shf->name, "lsr") ? 1 : !strcmp(shf->name, "asr") ? 2 : 3; amt = shf->amt; }
            a64_put((unsigned)sf << 31 | lg[k].opc << 29 | 0x0a000000 | st << 22 | lg[k].N << 21 | m->reg << 16 | amt << 10 | n->reg << 5 | Rd);
            return;
        }
    }
    /* shifts: immediate (bitfield aliases) or register (lslv...) */
    if (IS("lsl") || IS("lsr") || IS("asr") || IS("ror")) {
        need_n(nop, 3, mn);
        int bits = sf ? 64 : 32;
        if (o2->kind == 'i') {
            long s = a64_const(o2->e);
            if (s < 0 || s >= bits) aerr("%s: shift out of range", mn);
            unsigned N = sf << 22;
            if (IS("ror")) { a64_put((unsigned)sf << 31 | 0x13800000 | N | o1->reg << 16 | (unsigned)s << 10 | o1->reg << 5 | Rd); return; }
            if (IS("lsl")) { a64_put((unsigned)sf << 31 | 0x53000000 | N | (unsigned)((bits - s) % bits) << 16 | (unsigned)(bits - 1 - s) << 10 | o1->reg << 5 | Rd); return; }
            a64_put((unsigned)sf << 31 | (IS("asr") ? 0x13000000 : 0x53000000) | N | (unsigned)s << 16 | (unsigned)(bits - 1) << 10 | o1->reg << 5 | Rd);
            return;
        }
        int op = IS("lsl") ? 8 : IS("lsr") ? 9 : IS("asr") ? 10 : 11;
        a64_put((unsigned)sf << 31 | 0x1ac00000 | o2->reg << 16 | op << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("lslv") || IS("lsrv") || IS("asrv") || IS("rorv") || IS("udiv") || IS("sdiv")) {
        need_n(nop, 3, mn);
        int op = IS("lslv") ? 8 : IS("lsrv") ? 9 : IS("asrv") ? 10 : IS("rorv") ? 11 : IS("udiv") ? 2 : 3;
        a64_put((unsigned)sf << 31 | 0x1ac00000 | o2->reg << 16 | op << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("sxtb") || IS("sxth") || IS("sxtw") || IS("uxtb") || IS("uxth")) {
        need_n(nop, 2, mn);
        int s = mn[3] == 'b' ? 7 : mn[3] == 'h' ? 15 : 31;
        if (mn[0] == 's') a64_put((unsigned)sf << 31 | 0x13000000 | sf << 22 | s << 10 | o1->reg << 5 | Rd);
        else a64_put(0x53000000 | s << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("ubfx") || IS("sbfx") || IS("ubfiz") || IS("sbfiz") || IS("bfi") || IS("bfxil") || IS("ubfm") || IS("sbfm") || IS("bfm")) {
        need_n(nop, 4, mn);
        int bits = sf ? 64 : 32;
        long a = a64_const(o2->e), b = a64_const(o3->e), immr, imms;
        if (IS("ubfm") || IS("sbfm") || IS("bfm")) { immr = a; imms = b; }
        else if (mn[3] == 'x' || IS("bfxil")) { immr = a; imms = a + b - 1; }   /* extract: lsb, width */
        else { immr = (bits - a) % bits; imms = b - 1; }                      /* insert in zero / bfi */
        unsigned opc = mn[0] == 's' ? 0 : mn[0] == 'u' ? 2 : 1;
        a64_put((unsigned)sf << 31 | opc << 29 | 0x13000000 | sf << 22 | (unsigned)(immr & 63) << 16 | (unsigned)(imms & 63) << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("extr")) {
        need_n(nop, 4, mn);
        a64_put((unsigned)sf << 31 | 0x13800000 | sf << 22 | o2->reg << 16 | (unsigned)a64_const(o3->e) << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("adc") || IS("adcs") || IS("sbc") || IS("sbcs")) {
        need_n(nop, 3, mn);
        a64_put((unsigned)sf << 31 | (mn[0] == 's') << 30 | (mn[3] == 's') << 29 | 0x1a000000 | o2->reg << 16 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("mul") || IS("madd") || IS("msub") || IS("mneg") || IS("smulh") || IS("umulh") || IS("smull") || IS("umull") || IS("smaddl") || IS("umaddl")) {
        int ra = IS("madd") || IS("msub") || IS("smaddl") || IS("umaddl") ? (need_n(nop, 4, mn), o3->reg) : 31;
        if (!(IS("madd") || IS("msub") || IS("smaddl") || IS("umaddl"))) need_n(nop, 3, mn);
        unsigned op;
        if (IS("smulh")) op = 0x9b400000;
        else if (IS("umulh")) op = 0x9bc00000;
        else if (IS("smull") || IS("smaddl")) op = 0x9b200000;
        else if (IS("umull") || IS("umaddl")) op = 0x9ba00000;
        else op = (unsigned)sf << 31 | 0x1b000000 | (IS("msub") || IS("mneg")) << 15;
        a64_put(op | o2->reg << 16 | ra << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("clz") || IS("cls") || IS("rbit") || IS("rev") || IS("rev16") || IS("rev32")) {
        need_n(nop, 2, mn);
        int op = IS("rbit") ? 0 : IS("rev16") ? 1 : IS("rev32") ? 2 : IS("rev") ? (sf ? 3 : 2) : IS("clz") ? 4 : 5;
        a64_put((unsigned)sf << 31 | 0x5ac00000 | op << 10 | o1->reg << 5 | Rd);
        return;
    }
    /* conditional select and its aliases */
    if (IS("csel") || IS("csinc") || IS("csinv") || IS("csneg")) {
        need_n(nop, 4, mn);
        int c = a64_cond(o3->name);
        if (c < 0) aerr("%s: bad condition", mn);
        int op = IS("csinv") || IS("csneg"), o2b = IS("csinc") || IS("csneg");
        a64_put((unsigned)sf << 31 | op << 30 | 0x1a800000 | o2->reg << 16 | c << 12 | o2b << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("cset") || IS("csetm")) {
        need_n(nop, 2, mn);
        int c = a64_cond(o1->name);
        if (c < 0) aerr("%s: bad condition", mn);
        a64_put((unsigned)sf << 31 | IS("csetm") << 30 | 0x1a800000 | 31 << 16 | (c ^ 1) << 12 | IS("cset") << 10 | 31 << 5 | Rd);   /* csinc or csinv */
        return;
    }
    if (IS("cinc") || IS("cinv") || IS("cneg")) {
        need_n(nop, 3, mn);
        int c = a64_cond(o2->name);
        if (c < 0) aerr("%s: bad condition", mn);
        int op = IS("cinv") || IS("cneg"), o2b = IS("cinc") || IS("cneg");
        a64_put((unsigned)sf << 31 | op << 30 | 0x1a800000 | o1->reg << 16 | (c ^ 1) << 12 | o2b << 10 | o1->reg << 5 | Rd);
        return;
    }
    if (IS("ccmp") || IS("ccmn")) {
        need_n(nop, 4, mn);
        int c = a64_cond(o3->name);
        long nzcv = a64_const(o2->e);
        unsigned w = (unsigned)sf << 31 | IS("ccmp") << 30 | 0x3a400000 | c << 12 | o0->reg << 5 | (unsigned)(nzcv & 15);
        if (o1->kind == 'i') a64_put(w | 1 << 11 | (unsigned)(a64_const(o1->e) & 31) << 16);
        else a64_put(w | o1->reg << 16);
        return;
    }
#undef IS
    aerr("unknown instruction %s", mn);
}
