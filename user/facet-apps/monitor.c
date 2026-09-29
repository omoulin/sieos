/*
 * facet-monitor - the System Monitor: processor load with history, memory, and the processes.
 * A Facet application (libfacet).
 */
#include "common.h"

#define HIST 60

struct monitor {
    int ncpus;
    int pct[16];
    unsigned long prev_busy[16], prev_total[16];
    int hist[16][HIST];
    int pos, ticks;
    struct meminfo mem;
    struct procinfo procs[64];
    int nprocs;
};

/* Busy percentage of each processor since the last sample. */
static void cpu_sample(struct monitor *m)
{
    struct cpuinfo ci[16];
    int n = cpuinfo(ci, 16);
    m->ncpus = n > 0 ? n : 1;
    for (int i = 0; i < n; i++) {
        unsigned long busy = ci[i].busy_ticks, total = ci[i].busy_ticks + ci[i].idle_ticks;
        unsigned long db = busy - m->prev_busy[i], dt = total - m->prev_total[i];
        m->pct[i] = dt ? (int)(db * 100 / dt) : 0;
        m->prev_busy[i] = busy;
        m->prev_total[i] = total;
    }
}

static void monitor_sample(struct monitor *m)
{
    cpu_sample(m);
    int n = m->ncpus;
    for (int i = 0; i < n && i < 16; i++)
        m->hist[i][m->pos] = m->pct[i];
    m->pos = (m->pos + 1) % HIST;
    meminfo(&m->mem);
    m->nprocs = procinfo(m->procs, 64);
}

static void monitor_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    struct monitor *m = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    int n = m->ncpus, y = c.y + 8;
    gfx_text(s, c.x + 10, y, "Processors", C_TEXT);
    y += 20;
    int gw = c.w - 190;
    for (int i = 0; i < n; i++) {
        char label[16];
        int pct = m->hist[i][(m->pos + HIST - 1) % HIST];
        snprintf(label, sizeof(label), "CPU %d", i);
        gfx_text(s, c.x + 10, y + 4, label, C_TEXT);
        ui_meter(s, rect_make(c.x + 60, y + 4, 70, 16), pct, pct > 80 ? C_ACCENT : RGB(0x6C, 0xC0, 0x8A));
        snprintf(label, sizeof(label), "%3d%%", pct);
        gfx_text(s, c.x + 136, y + 4, label, C_TEXT);
        /* history graph */
        struct rect g = rect_make(c.x + 176, y, gw, 24);
        gfx_fill(s, g.x, g.y, g.w, g.h, RGB(0x1A, 0x22, 0x26));
        gfx_bevel(s, g.x, g.y, g.w, g.h, 1, false, C_LINE, C_FACE_DARK);
        for (int k = 1; k < 4; k++)
            gfx_hline(s, g.x + 1, g.y + k * g.h / 4, g.w - 2, RGB(0x28, 0x34, 0x38));
        int px = -1, py = 0;
        for (int k = 0; k < HIST; k++) {
            int v = m->hist[i][(m->pos + k) % HIST];
            int xx = g.x + 2 + k * (g.w - 4) / (HIST - 1), yy = g.y + g.h - 3 - v * (g.h - 5) / 100;
            if (px >= 0)
                gfx_line(s, px, py, xx, yy, RGB(0x7C, 0xE0, 0x9A));
            px = xx;
            py = yy;
        }
        y += 30;
    }
    /* memory */
    y += 6;
    unsigned long used = m->mem.total_kb - m->mem.free_kb;
    char line[96];
    snprintf(line, sizeof(line), "Memory  %lu / %lu MiB", used / 1024, m->mem.total_kb / 1024);
    gfx_text(s, c.x + 10, y, line, C_TEXT);
    ui_meter(s, rect_make(c.x + 230, y, c.w - 240, 16),
             m->mem.total_kb ? (int)(used * 100 / m->mem.total_kb) : 0, C_TITLE_A1);
    y += 28;
    /* process table */
    struct rect tbl = rect_make(c.x + 8, y, c.w - 16, c.y + c.h - y - 8);
    gfx_fill(s, tbl.x, tbl.y, tbl.w, tbl.h, C_CONTENT);
    gfx_bevel(s, tbl.x, tbl.y, tbl.w, tbl.h, 1, false, C_LINE, C_FACE_DARK);
    gfx_fill(s, tbl.x + 1, tbl.y + 1, tbl.w - 2, 18, color_shade(C_FACE, 10));
    gfx_text_mono(s, tbl.x + 6, tbl.y + 2, "  PID USER     STATE   CPU  MEM(KB) COMMAND", C_TEXT);   /* columns: monospace */
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, tbl));
    static const char *states[] = { "?", "new", "ready", "run", "sleep", "zombie", "stopped" };
    int row = 0;
    for (int i = 0; i < m->nprocs; i++) {
        struct procinfo *p = &m->procs[i];
        if (p->pid == 0)
            continue;
        struct passwd *pw = getpwuid(p->euid);
        char cpu[6];
        if (p->cpu >= 0)
            snprintf(cpu, sizeof(cpu), "%d", p->cpu);
        else
            strcpy(cpu, "-");
        snprintf(line, sizeof(line), "%5d %-8s %-7s %3s %8lu %s", p->pid, pw ? pw->pw_name : "?",
                 p->state < 7 ? states[p->state] : "?", cpu, p->mem_kb, p->name);
        int ry = tbl.y + 20 + row * 17;
        if (p->state == PSTATE_RUNNING)
            gfx_fill(s, tbl.x + 1, ry, tbl.w - 2, 17, C_SELECT);
        gfx_text_mono(s, tbl.x + 6, ry, line, C_TEXT);
        row++;
    }
    gfx_set_clip(s, clip);
}

static void monitor_tick(struct fct_view *w)
{
    struct monitor *m = w->app;
    if (++m->ticks % 4)
        return;
    monitor_sample(m);
    fct_view_invalidate(w);
}

static void monitor_destroy(struct fct_view *w)
{
    free(w->app);
}

int main(void)
{
    struct monitor *m = calloc(1, sizeof(*m));
    if (!m || fct_app_init() < 0)
        return 1;
    monitor_sample(m);
    int n = m->ncpus;
    struct fct_window_attr a = { "System Monitor", FCT_POS_AUTO, FCT_POS_AUTO, 560, 36 + n * 30 + 40 + 220, 414, 211, 0 };
    struct fct_view *w = fct_view_create(&a);
    if (!w)
        return 1;
    w->app = m;
    w->draw = monitor_draw;
    w->tick = monitor_tick;
    w->destroy = monitor_destroy;
    return fct_main();
}
