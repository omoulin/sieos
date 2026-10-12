/* text.c - gfxhat's text part on the development machine: the log cut into
 * rows of 21 (wrapping, tabs, backspace, colour codes skipped), the 8 rows
 * shown (following, scrolled back, the bar), the picture as the screen gets
 * it. Run by tests/gfxhat/run.sh; -v draws the screens.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#define GFXHAT_HOST
#include "../../user/gfxhat/gfxhat.c"
#include <stdio.h>

static int fails, verbose;
static void check(const char *what, int ok) { printf("%s: %s\n", ok ? "ok" : "FAIL", what); fails += !ok; }
static void row_is(int i, const char *want, const char *what)   /* screen row i (0: top) */
{
    uint64_t r = nrows - 1 - back - (ROWS - 1 - i);
    char got[COLS + 1];
    memcpy(got, rows[r % KEEP], COLS); got[COLS] = 0;
    char w[COLS + 1];
    snprintf(w, sizeof w, "%-21s", want);
    if (strcmp(got, w)) printf("  row %d: got [%s], want [%s]\n", i, got, w);
    check(what, !strcmp(got, w));
}
static void draw(void)
{
    if (!verbose) return;
    render();
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) putchar(pic[y / 8][x] >> y % 8 & 1 ? '#' : '.');
        putchar('\n');
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    memset(rows, ' ', sizeof rows);
    const char *log = "mk: 4 CPUs, 3840 MiB\n\033[1;32minit:\033[0m con started\n"
                      "a very long line that goes past twenty-one characters\n"
                      "tab\there\nbackspace: abc\b\bXY\r\nlogin: ";
    text(log, strlen(log));
    row_is(7, "login:", "the last row: what is being written");
    row_is(6, "backspace: aXY", "backspace erases, \\r is nothing");
    row_is(5, "tab here", "a tab: to the next multiple of 4");
    row_is(4, " characters", "a long line wraps (3/3)");
    row_is(3, " goes past twenty-one", "a long line wraps (2/3)");
    row_is(2, "a very long line that", "a long line wraps (1/3)");
    row_is(1, "init: con started", "colour codes skipped");
    row_is(0, "mk: 4 CPUs, 3840 MiB", "the first line");
    draw();
    render();
    check("'m' of \"mk\" drawn in the top-left cell", !memcmp(pic[0], font5x7['m' - 32], 5));
    for (int i = 0; i < 40; i++) { char b[32]; int n = snprintf(b, sizeof b, "line %d\n", i); text(b, n); }
    row_is(7, "", "following: the newest row at the bottom");
    row_is(6, "line 39", "following: line 39 above it");
    scroll(10);
    row_is(7, "line 30", "back 10 rows");
    text("line 40\n", 8);
    row_is(7, "line 30", "new lines do not move what is shown");
    render();
    int bar = 0;
    for (int y = 0; y < 64; y++) bar += pic[y / 8][127] >> y % 8 & 1;
    check("the scroll bar is drawn", bar >= 4);
    draw();
    scroll(100000);
    row_is(0, "mk: 4 CPUs, 3840 MiB", "back to the first row, no further");
    back = 0;
    row_is(6, "line 40", "the newest again");
    render();
    bar = 0;
    for (int y = 0; y < 64; y++) bar += pic[y / 8][127] >> y % 8 & 1;
    check("following: no bar", bar == 0);
    for (int i = 0; i < 3000; i++) text("x\n", 2);
    scroll(100000);
    check("only KEEP rows kept", back == KEEP - ROWS);
    render();
    printf(fails ? "FAIL: %d check(s)\n" : "PASS\n", fails);
    return fails != 0;
}
