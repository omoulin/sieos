/*
 * common.c - Pieces the desktop's applications share: the scroll bar.
 */
#include "common.h"

void draw_scrollbar(struct surface *s, struct rect r, int first, int visible, int total)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, color_shade(C_FACE, -20));
    ui_button(s, rect_make(r.x, r.y, r.w, SB_W), NULL, false);
    ui_button(s, rect_make(r.x, r.y + r.h - SB_W, r.w, SB_W), NULL, false);
    int cx = r.x + r.w / 2;
    gfx_triangle(s, cx - 4, r.y + 10, cx, r.y + 5, cx + 4, r.y + 10, C_TEXT);
    gfx_triangle(s, cx - 4, r.y + r.h - 10, cx, r.y + r.h - 5, cx + 4, r.y + r.h - 10, C_TEXT);
    int track = r.h - 2 * SB_W;
    if (total > visible && track > 10) {
        int th = MAX(12, track * visible / total);
        int ty = r.y + SB_W + (track - th) * first / MAX(1, total - visible);
        ui_button(s, rect_make(r.x + 1, ty, r.w - 2, th), NULL, false);
        gfx_hline(s, r.x + 4, ty + th / 2, r.w - 8, C_ACCENT);
    }
}

/* Returns the new first line after a click at y inside scroll bar r. */
int scrollbar_click(struct rect r, int y, int first, int visible, int total)
{
    if (y < r.y + SB_W)
        return MAX(0, first - 3);
    if (y >= r.y + r.h - SB_W)
        return MAX(0, MIN(total - visible, first + 3));
    if (y < r.y + r.h / 2)
        return MAX(0, first - visible);
    return MAX(0, MIN(total - visible, first + visible));
}


/* One request on the desktop channel (SIEOS_DESKTOP="RFD,WFD", from
 * Facet): the reply's "result" text (JSON string escapes undone) into
 * result, true if "ok". */
bool desktop_request(const char *json, char *result, size_t n)
{
    const char *e = getenv("SIEOS_DESKTOP");
    int rfd, wfd;
    result[0] = 0;
    if (!e || sscanf(e, "%d,%d", &rfd, &wfd) != 2) {
        snprintf(result, n, "not started by the Facet desktop");
        return false;
    }
    char line[4096];
    int len = snprintf(line, sizeof(line), "%s\n", json);
    if (write(wfd, line, len) != len) {
        snprintf(result, n, "the desktop does not answer");
        return false;
    }
    size_t got = 0;
    while (got < sizeof(line) - 1) {
        ssize_t r = read(rfd, line + got, 1);
        if (r <= 0 || line[got] == '\n')
            break;
        got++;
    }
    line[got] = 0;
    bool ok = strstr(line, "\"ok\":true") != NULL;
    const char *p = strstr(line, "\"result\":\"");
    if (!p)
        p = strstr(line, "\"error\":\"");
    if (p) {
        p = strchr(p, ':') + 2;
        size_t k = 0;
        for (; *p && *p != '"' && k + 1 < n; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                result[k++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            } else {
                result[k++] = *p;
            }
        }
        result[k] = 0;
    }
    return ok;
}
