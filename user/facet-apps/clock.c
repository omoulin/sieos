/*
 * facet-clock - an analogue clock with the date (UTC).
 * A Facet application (libfacet).
 */
#include "common.h"

/* sin(6k degrees) * 1000 for k = 0..15 */
static const int sin6[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };

static int isin60(int k)                 /* k in 0..59 : sin(k*6deg)*1000 */
{
    k = ((k % 60) + 60) % 60;
    if (k <= 15) return sin6[k];
    if (k <= 30) return sin6[30 - k];
    if (k <= 45) return -sin6[k - 30];
    return -sin6[60 - k];
}

static int icos60(int k) { return isin60(k + 15); }

struct clockapp {
    long last;
};

static void hand(struct surface *s, int cx, int cy, int pos60, int len, int thick, color_t c)
{
    int x = cx + isin60(pos60) * len / 1000, y = cy - icos60(pos60) * len / 1000;
    gfx_thick_line(s, cx, cy, x, y, thick, c);
}

static void clock_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    (void)w;
    gfx_vgradient(s, c.x, c.y, c.w, c.h, C_CONTENT_ALT, C_CONTENT);
    int r = MIN(c.w, c.h - 30) / 2 - 12;
    int cx = c.x + c.w / 2, cy = c.y + 12 + r;
    gfx_disc(s, cx + 3, cy + 3, r + 4, RGB(0x0C, 0x0D, 0x0F));
    gfx_disc(s, cx, cy, r + 4, C_TITLE_A2);
    gfx_disc(s, cx, cy, r, RGB(0x22, 0x25, 0x2A));
    for (int k = 0; k < 60; k++) {
        int outer = r - 4, inner = k % 5 ? r - 8 : r - 16;
        int x0 = cx + isin60(k) * inner / 1000, y0 = cy - icos60(k) * inner / 1000;
        int x1 = cx + isin60(k) * outer / 1000, y1 = cy - icos60(k) * outer / 1000;
        gfx_thick_line(s, x0, y0, x1, y1, k % 5 ? 1 : 3, k % 15 ? C_TEXT : C_ACCENT);
    }
    long now = time(NULL);
    struct tm tm;
    time_t now_t = now;
    gmtime_r(&now_t, &tm);
    int hpos = (tm.tm_hour % 12) * 5 + tm.tm_min / 12;
    hand(s, cx, cy, hpos, r * 50 / 100, 5, C_TEXT);
    hand(s, cx, cy, tm.tm_min, r * 75 / 100, 3, C_TEXT);
    hand(s, cx, cy, tm.tm_sec, r * 85 / 100, 1, RGB(0xC9, 0x4B, 0x3F));
    gfx_disc(s, cx, cy, 5, C_ACCENT);
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    char date[64];
    snprintf(date, sizeof(date), "%s %04d-%02d-%02d  %02d:%02d:%02d UTC", days[tm.tm_wday], tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    gfx_text(s, c.x + (c.w - text_width(date)) / 2, c.y + c.h - 22, date, C_TEXT);
}

static void clock_tick(struct fct_view *w)
{
    struct clockapp *ca = w->app;
    long now = time(NULL);
    if (now != ca->last) {
        ca->last = now;
        fct_view_invalidate(w);
    }
}

static void clock_destroy(struct fct_view *w)
{
    free(w->app);
}

int main(void)
{
    struct clockapp *ca = calloc(1, sizeof(*ca));
    if (!ca || fct_app_init() < 0)
        return 1;
    struct fct_window_attr a = { "Clock", FCT_POS_AUTO, FCT_POS_AUTO, 300, 330, 194, 191, 0 };
    struct fct_view *w = fct_view_create(&a);
    if (!w)
        return 1;
    w->app = ca;
    w->draw = clock_draw;
    w->tick = clock_tick;
    w->destroy = clock_destroy;
    return fct_main();
}
