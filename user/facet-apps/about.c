/*
 * facet-about - About SIEOS: the system, the processors and memory, who is
 * logged in; the author and the licence.
 * A Facet application (libfacet).
 */
#include "common.h"

static void about_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    (void)w;
    gfx_vgradient(s, c.x, c.y, c.w, c.h, C_CONTENT_ALT, C_CONTENT);
    logo_draw(s, c.x + 72, c.y + 66, 100);
    gfx_text_scaled(s, c.x + 140, c.y + 22, "SIEOS", 4, C_TITLE_A2);
    gfx_text(s, c.x + 142, c.y + 84, "Synthetic Intelligence", C_TEXT);
    gfx_text(s, c.x + 142, c.y + 102, "Enhanced Operating System", C_TEXT);
    struct utsname u;
    uname(&u);
    struct meminfo mi;
    meminfo(&mi);
    char line[96];
    int y = c.y + 140;
    snprintf(line, sizeof(line), "Facet Desktop 2.0 on %s %s (%s)", u.sysname, u.release, u.machine);
    gfx_text(s, c.x + 24, y, line, C_TEXT);
    struct cpuinfo ci[16];
    int ncpu = cpuinfo(ci, 16);
    snprintf(line, sizeof(line), "Processors: %d    Memory: %lu MiB", ncpu > 0 ? ncpu : 1, mi.total_kb / 1024);
    gfx_text(s, c.x + 24, y + 20, line, C_TEXT);
    struct passwd *pw = getpwuid(geteuid());
    snprintf(line, sizeof(line), "Logged in as: %s", pw ? pw->pw_name : "?");
    gfx_text(s, c.x + 24, y + 40, line, C_TEXT);
    gfx_hline(s, c.x + 24, y + 66, c.w - 48, C_FACE_SHADOW);
    gfx_text_bold(s, c.x + 24, y + 76, "Developed by Olivier Moulin", C_TEXT);
    gfx_text(s, c.x + 24, y + 94, "Released under the GNU General Public License,", C_TEXT);
    gfx_text(s, c.x + 24, y + 112, "version 3 (GPL-3.0).", C_TEXT);
    gfx_hline(s, c.x + 24, y + 138, c.w - 48, C_FACE_SHADOW);
    gfx_text(s, c.x + 24, y + 148, "Start programs from the dock or the menu, or type in", C_DIM);
    gfx_text(s, c.x + 24, y + 166, "the sia strip at the top (Ctrl+Space).", C_DIM);
    gfx_text(s, c.x + 24, y + 184, "Ctrl+Alt+1..4 switch workspaces; Alt+Tab windows.", C_DIM);
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    int sw, sh;
    fct_screen(&sw, &sh);
    struct fct_window_attr a = { "About SIEOS", sw - 580, sh - 460, 440, 362, 0, 0, 0 };
    struct fct_view *w = fct_view_create(&a);
    if (!w)
        return 1;
    w->draw = about_draw;
    return fct_main();
}
