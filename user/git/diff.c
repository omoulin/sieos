/*
 * diff.c - A unified diff of two versions of a file, as git prints it.
 *
 * The lines that change are found with Myers' algorithm (1986): the
 * shortest edit script is the path through the grid of "line i of the old
 * text, line j of the new" with the fewest steps right (a line removed) or
 * down (a line added), diagonals (equal lines) being free. Step d reaches,
 * on each diagonal k = i - j, the furthest point d edits allow; the ends
 * of step d's points are kept to walk the path back. Changes are then
 * printed in hunks with 3 lines of context.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "git.h"

typedef struct { const uint8_t *p; uint32_t n, h; } line_t;
#define DMAX 3000                     /* more edits than that: everything replaced */

static int lines(const uint8_t *t, size_t n, line_t **out)
{
    int c = 0;
    for (size_t i = 0; i < n; i++) if (t[i] == '\n' || i == n - 1) c++;
    line_t *l = *out = malloc(sizeof(line_t) * (size_t)(c + 1));
    c = 0;
    for (size_t i = 0, s = 0; i < n; i++)
        if (t[i] == '\n' || i == n - 1) {
            uint32_t h = 5381;
            for (size_t k = s; k <= i; k++) h = h * 33 + t[k];
            l[c++] = (line_t){ t + s, (uint32_t)(i + 1 - s), h };
            s = i + 1;
        }
    return c;
}
static int eq(const line_t *a, const line_t *b) { return a->h == b->h && a->n == b->n && !memcmp(a->p, b->p, a->n); }

/* The edit script, forward: ops[] = '=' / '-' / '+' -> its length. */
static int myers(const line_t *a, int n, const line_t *b, int m, char *ops)
{
    int max = n + m, *v = malloc(sizeof(int) * (size_t)(2 * max + 3)), **trace = malloc(sizeof(int *) * (DMAX + 1)), d, k;
    v += max + 1;
    v[1] = 0;
    for (d = 0; d <= max && d <= DMAX; d++) {
        for (k = -d; k <= d; k += 2) {
            int x = k == -d || (k != d && v[k - 1] < v[k + 1]) ? v[k + 1] : v[k - 1] + 1, y = x - k;
            while (x < n && y < m && eq(&a[x], &b[y])) x++, y++;
            v[k] = x;
            if (x >= n && y >= m) break;
        }
        trace[d] = malloc(sizeof(int) * (size_t)(2 * d + 1));
        memcpy(trace[d], v - d, sizeof(int) * (size_t)(2 * d + 1));
        if (k <= d) break;
    }
    int len = 0;
    if (d > DMAX || d > max) {                       /* too different: all out, all in */
        for (int i = 0; i < n; i++) ops[len++] = '-';
        for (int i = 0; i < m; i++) ops[len++] = '+';
    } else {
        int x = n, y = m, at = n + m;                /* back from the end, writing ops from the end */
        char *rev = ops + at;
        for (; d >= 0; d--) {
            int *pv = d ? trace[d - 1] + (d - 1) : 0;
            k = x - y;
            int pk = d && (k == -d || (k != d && pv[k - 1] < pv[k + 1])) ? k + 1 : k - 1;
            int px = d ? pv[pk] : 0, py = px - pk;
            if (!d) px = py = 0;
            while (x > px && y > py) { *--rev = '='; x--; y--; }
            if (d) { *--rev = pk == k + 1 ? '+' : '-'; x = px; y = py; }
        }
        len = (int)(ops + at - rev);
        memmove(ops, rev, (size_t)len);
    }
    for (int i = 0; i <= d && i <= DMAX; i++) free(trace[i]);
    free(trace);
    free(v - max - 1);
    return len;
}

static void range(char s, int start, int count)
{
    outf(" %c%d", s, count ? start + 1 : start);
    if (count != 1) outf(",%d", count);
}
static void line(char c, const line_t *l)
{
    out(&c, 1);
    out(l->p, l->n);
    if (l->p[l->n - 1] != '\n') out("\n\\ No newline at end of file\n", 29);
}

static int binary(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n && i < 8000; i++) if (!p[i]) return 1;
    return 0;
}

void diff_text(const char *path, const uint8_t *a, size_t an, const oid_t ida, uint32_t ma,
               const uint8_t *b, size_t bn, const oid_t idb, uint32_t mb)
{
    char ha[41], hb[41];
    hex(ha, ida); hex(hb, idb);
    if (!ma) memset(ha, '0', 40);
    if (!mb) memset(hb, '0', 40);
    ha[7] = hb[7] = 0;
    outf("diff --git a/%s b/%s\n", path, path);
    if (!ma) outf("new file mode %o\n", mb);
    else if (!mb) outf("deleted file mode %o\n", ma);
    else if (ma != mb) outf("old mode %o\nnew mode %o\n", ma, mb);
    if (ma && mb && an == bn && !memcmp(a, b, an)) return;   /* (the mode only) */
    outf("index %s..%s", ha, hb);
    if (ma && ma == mb) outf(" %o", ma);
    outf("\n");
    if (binary(a, an) || binary(b, bn)) { outf("Binary files %s%s and %s%s differ\n", ma ? "a/" : "", ma ? path : "/dev/null", mb ? "b/" : "", mb ? path : "/dev/null"); return; }
    outf(ma ? "--- a/%s\n" : "--- /dev/null\n", path);
    outf(mb ? "+++ b/%s\n" : "+++ /dev/null\n", path);
    line_t *la, *lb;
    int n = lines(a, an, &la), m = lines(b, bn, &lb);
    char *ops = malloc((size_t)(n + m + 1));
    int len = myers(la, n, lb, m, ops);
    /* hunks: changes with up to 3 equal lines around them (6 between: one hunk) */
    for (int i = 0, ai = 0, bi = 0; i < len; ) {
        if (ops[i] == '=') { i++; ai++; bi++; continue; }
        int s = i, sa = ai, sb = bi, e = i, ea = ai, eb = bi;
        for (int ctx = 0; s > 0 && ops[s - 1] == '=' && ctx < 3; ctx++) s--, sa--, sb--;
        for (int j = i, ja = ai, jb = bi, eqs = 0; j < len; j++) {   /* to the last change within reach */
            if (ops[j] == '=') { if (++eqs > 6) break; ja++; jb++; continue; }
            eqs = 0;
            if (ops[j] == '-') ja++; else jb++;
            e = j + 1; ea = ja; eb = jb;
        }
        for (int ctx = 0; e < len && ops[e] == '=' && ctx < 3; ctx++) e++, ea++, eb++;
        out("@@", 2); range('-', sa, ea - sa); range('+', sb, eb - sb); out(" @@\n", 4);
        for (int j = s, ja = sa, jb = sb; j < e; j++)
            if (ops[j] == '=') { line(' ', &la[ja++]); jb++; }
            else if (ops[j] == '-') line('-', &la[ja++]);
            else line('+', &lb[jb++]);
        i = e; ai = ea; bi = eb;
    }
    free(ops); free(la); free(lb);
}
