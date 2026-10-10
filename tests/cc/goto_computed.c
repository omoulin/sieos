/* goto_computed.c - GNU computed goto: &&label, goto *p, label tables (static and local).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"

/* a tiny bytecode interpreter dispatched through a static table */
static long run(const unsigned char *code, long x)
{
    static void *ops[] = { &&op_halt, &&op_inc, &&op_dbl, &&op_dec, &&op_jnz };
    long acc = x, steps = 0;
    const unsigned char *pc = code;
#define NEXT goto *ops[*pc++]
    NEXT;
op_inc: acc++; steps++; NEXT;
op_dbl: acc *= 2; steps++; NEXT;
op_dec: acc--; steps++; NEXT;
op_jnz: { unsigned char back = *pc++; steps++; if (acc % 7) pc -= back; } NEXT;
op_halt:
    return acc * 1000 + steps;
#undef NEXT
}

static int local_table(int k)
{
    void *t[3] = { &&a, &&b, &&c };
    int r = 0;
    for (int i = 0; i < 3; i++) {
        goto *t[(i + k) % 3];
    a: r = r * 10 + 1; continue;
    b: r = r * 10 + 2; continue;
    c: r = r * 10 + 3;
    }
    return r;
}

static int diff(void) { return &&end - &&start > 0; start: ; end: return 1; }

int main(void)
{
    static const unsigned char prog[] = { 1, 2, 3, 4, 3, 1, 0 };
    printf("%ld %ld\n", run(prog, 3), run(prog, 50));
    printf("%d %d %d %d\n", local_table(0), local_table(1), local_table(2), diff());
    void *p = &&out;
    int n = 0;
again:
    if (++n < 5) goto *(n & 1 ? &&again : p);
out:
    printf("n %d\n", n);
    return 0;
}
