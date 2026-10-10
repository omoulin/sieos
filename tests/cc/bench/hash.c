/* hash.c - benchmark: strings and an open-addressing hash table (word counting).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int printf(const char *fmt, ...);
#define N 65536
static struct { char key[16]; int len, count; } tab[N];
static unsigned hash(const char *s, int n) { unsigned h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u; return h; }
static int same(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static void add(const char *w, int n)
{
    unsigned h = hash(w, n) & (N - 1);
    while (tab[h].len && !(tab[h].len == n && same(tab[h].key, w, n))) h = (h + 1) & (N - 1);
    if (!tab[h].len) { for (int i = 0; i < n; i++) tab[h].key[i] = w[i]; tab[h].len = n; }
    tab[h].count++;
}
int main(void)
{
    char w[16];
    unsigned seed = 1;
    long total = 0;
    for (int k = 0; k < 6000000; k++) {
        seed = seed * 1103515245 + 12345;
        unsigned v = (seed >> 8) % 20000;      /* at most 20000 different words */
        int n = 3 + v % 8;
        for (int i = 0; i < n; i++) { w[i] = 'a' + v % 26; v = v / 3 + i; }
        add(w, n);
    }
    for (int i = 0; i < N; i++) total += (long)tab[i].count * (i & 7);
    printf("%ld\n", total);
    return 0;
}
