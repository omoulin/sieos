/* pkg-config: libpcre2-8 */
/*
 * wtest-pcre2 (make webdeps-test): PCRE2 (8-bit, no JIT): a pattern
 * compiled, matched, its groups taken; UTF-8 and Unicode properties;
 * substitution; an error reported.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest pcre2: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

static pcre2_code *re(const char *p, uint32_t opt)
{
    int e; PCRE2_SIZE off;
    return pcre2_compile((PCRE2_SPTR)p, PCRE2_ZERO_TERMINATED, opt, &e, &off, 0);
}

int main(void)
{
    pcre2_code *c = re("(\\w+)@(\\w+)\\.org", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(c, 0);
    const char *s = "write to olivier@sieos.org today";
    int r = pcre2_match(c, (PCRE2_SPTR)s, strlen(s), 0, 0, md, 0);
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
    check("a match and its groups", r == 3 && ov[2] == 9 && ov[3] == 16 && !memcmp(s + ov[4], "sieos", 5));
    pcre2_code *u = re("^\\p{Lu}\\p{Ll}+é$", PCRE2_UTF | PCRE2_UCP);
    pcre2_match_data *mu = pcre2_match_data_create_from_pattern(u, 0);
    check("UTF-8, Unicode properties", pcre2_match(u, (PCRE2_SPTR)"café", PCRE2_ZERO_TERMINATED, 0, 0, mu, 0) < 0 &&
          pcre2_match(u, (PCRE2_SPTR)"Olivé", PCRE2_ZERO_TERMINATED, 0, 0, mu, 0) == 1);
    PCRE2_UCHAR out[64];
    PCRE2_SIZE ol = sizeof out;
    pcre2_code *d = re("[0-9]+", 0);
    r = pcre2_substitute(d, (PCRE2_SPTR)"a1b22c333", PCRE2_ZERO_TERMINATED, 0, PCRE2_SUBSTITUTE_GLOBAL, 0, 0,
                         (PCRE2_SPTR)"#", 1, out, &ol);
    check("global substitution", r == 3 && !strcmp((char *)out, "a#b#c#"));
    check("a bad pattern refused", re("(unclosed", 0) == 0);
    printf("wtest pcre2: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
