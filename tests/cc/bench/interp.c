/* interp.c - benchmark: a switch-dispatched bytecode interpreter (branchy code).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int printf(const char *fmt, ...);
enum { PUSH, ADD, SUB, MUL, DUP, SWAP, JNZ, DEC, POP, HALT, XOR, SHR };
static long run(const int *code, long n)
{
    long st[64];
    int sp = 0, pc = 0;
    st[sp++] = n;
    for (;;) {
        switch (code[pc++]) {
        case PUSH: st[sp++] = code[pc++]; break;
        case ADD: sp--; st[sp - 1] += st[sp]; break;
        case SUB: sp--; st[sp - 1] -= st[sp]; break;
        case MUL: sp--; st[sp - 1] *= st[sp]; break;
        case XOR: sp--; st[sp - 1] ^= st[sp]; break;
        case SHR: st[sp - 1] >>= 1; break;
        case DUP: st[sp] = st[sp - 1]; sp++; break;
        case SWAP: { long t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; break; }
        case DEC: st[sp - 1]--; break;
        case POP: sp--; break;
        case JNZ: { int to = code[pc++]; if (st[sp - 1]) pc = to; break; }
        case HALT: return st[sp - 1];
        }
    }
}
int main(void)
{
    /* acc n: loop { acc = acc*3 ^ n >> 1 ... ; n-- } */
    static const int prog[] = { PUSH, 1, SWAP, /*3:*/ SWAP, PUSH, 3, MUL, DUP, SHR, XOR, SWAP, DEC, JNZ, 3, POP, HALT };
    long r = 0;
    for (int k = 0; k < 40; k++) r += run(prog, 1000000 + k);
    printf("%ld\n", r);
    return 0;
}
