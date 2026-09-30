/*
 * facet-power - Power and Temperature: the power policy (performance,
 * balanced, power saver: a click sets it through poweradm, which keeps it
 * for the next boot), the processors' temperature over the last minute
 * against the thermal policy's thresholds, each processor's temperature
 * and frequency, the fans, and what the machine supports (/dev/power).
 * A Facet application (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "sieos/power.h"

#define PAD 14
#define HIST 60
#define CARD_W 150
#define CARD_H 54

static struct {
    struct sieos_power_info pi;
    bool ok;
    int hist[HIST], pos, ticks;
    int hover;
    char status[120];
} pa = { .hover = -1 };

static const char *const names[] = { "Performance", "Balanced", "Power saver" };
static const char *const hints[] = { "full speed", "the default", "cooler, quieter" };
static const char *const args[] = { "policy=performance", "policy=balanced", "policy=powersave" };

static void sample(void)
{
    int fd = open("/dev/power", O_RDONLY);
    pa.ok = fd >= 0 && ioctl(fd, SIEOS_POWER_GET, &pa.pi) == 0;
    if (fd >= 0)
        close(fd);
    pa.hist[pa.pos] = pa.ok ? pa.pi.dpi_temp : -1;
    pa.pos = (pa.pos + 1) % HIST;
}

static struct rect card(struct rect c, int i)
{
    return rect_make(c.x + PAD + i * (CARD_W + 10), c.y + 34, CARD_W, CARD_H);
}

static void draw(struct fct_view *w, struct surface *s, struct rect c)
{
    (void)w;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    char line[160];
    if (!pa.ok) {
        gfx_text(s, c.x + PAD, c.y + PAD, "No power management (/dev/power).", C_TEXT);
        return;
    }
    struct sieos_power_info *pi = &pa.pi;
    gfx_text_bold(s, c.x + PAD, c.y + 10, "Power policy", C_TEXT);
    for (int i = 0; i < 3; i++) {
        struct rect r = card(c, i);
        bool cur = (int)pi->dpi_policy == i;
        gfx_fill(s, r.x, r.y, r.w, r.h, cur ? C_SELECT : i == pa.hover ? C_CONTENT_ALT : C_CONTENT);
        gfx_bevel(s, r.x, r.y, r.w, r.h, 1, !cur, C_FACE_LIGHT, C_FACE_DARK);
        gfx_text_bold(s, r.x + 10, r.y + 10, names[i], cur ? C_ACCENT : C_TEXT);
        gfx_text(s, r.x + 10, r.y + 30, hints[i], C_DIM);
    }

    /* the temperature over the last minute, with the thresholds */
    int gy = c.y + 34 + CARD_H + 22;
    snprintf(line, sizeof(line), pi->dpi_temp >= 0 ? "Temperature  %d C" : "Temperature  (no sensor)", pi->dpi_temp);
    gfx_text_bold(s, c.x + PAD, gy - 16, line, pi->dpi_throttle ? C_BAD : C_TEXT);
    int lw = text_width_bold(line);
    if (pi->dpi_throttle) {
        snprintf(line, sizeof(line), "slowed %u%% (passive cooling)", pi->dpi_throttle);
        gfx_text(s, c.x + PAD + lw + 16, gy - 16, line, C_BAD);
    }
    struct rect g = rect_make(c.x + PAD, gy + 4, c.w - 2 * PAD, 96);
    gfx_fill(s, g.x, g.y, g.w, g.h, RGB(0x10, 0x18, 0x20));
    gfx_bevel(s, g.x, g.y, g.w, g.h, 1, false, C_LINE, C_FACE_DARK);
    int lo = 20, hi = MAX(pi->dpi_critical + 5, 60);
#define TY(t) (g.y + g.h - 3 - ((t) - lo) * (g.h - 6) / (hi - lo))
    for (int k = 0; k < 2; k++) {
        int t = k ? pi->dpi_critical : pi->dpi_passive;
        color_t col = k ? RGB(0xE0, 0x50, 0x40) : RGB(0xE0, 0xB0, 0x40);
        for (int x = g.x + 2; x < g.x + g.w - 2; x += 6)
            gfx_hline(s, x, TY(t), 3, col);
        snprintf(line, sizeof(line), "%s %d C", k ? "shutdown" : "passive", t);
        gfx_text(s, g.x + g.w - 8 - text_width(line), TY(t) - FONT_H - 1, line, col);
    }
    int px = -1, py = 0;
    for (int k = 0; k < HIST; k++) {
        int t = pa.hist[(pa.pos + k) % HIST];
        if (t < 0) {
            px = -1;
            continue;
        }
        t = MIN(MAX(t, lo), hi);
        int x = g.x + 2 + k * (g.w - 4) / (HIST - 1), y = TY(t);
        if (px >= 0)
            gfx_thick_line(s, px, py, x, y, 2, RGB(0x60, 0xD0, 0xF0));
        px = x;
        py = y;
    }
#undef TY

    /* the processors */
    int y = g.y + g.h + 12;
    int cols = 4, cw = (c.w - 2 * PAD) / cols;
    for (unsigned i = 0; i < pi->dpi_ncpu && i < 16; i++) {
        char t[16] = "", f[16] = "";
        if (pi->dpi_cpu[i].temp >= 0)
            snprintf(t, sizeof(t), " %d C", pi->dpi_cpu[i].temp);
        if (pi->dpi_cpu[i].mhz)
            snprintf(f, sizeof(f), " %u MHz", pi->dpi_cpu[i].mhz);
        snprintf(line, sizeof(line), "CPU %u%s%s", i, t, f[0] || t[0] ? f : "  -");
        gfx_text(s, c.x + PAD + (i % cols) * cw, y + (i / cols) * (FONT_H + 4), line, C_TEXT);
    }
    y += ((MIN(pi->dpi_ncpu, 16u) + cols - 1) / cols) * (FONT_H + 4) + 8;

    /* fans and the machine */
    int n = snprintf(line, sizeof(line), "Fans: %s", pi->dpi_fans);
    for (unsigned i = 0; i < pi->dpi_nfans && n < (int)sizeof(line) - 24; i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%s %u RPM", i ? ", " : ": ", pi->dpi_fan[i].name,
                      pi->dpi_fan[i].rpm);
    gfx_text(s, c.x + PAD, y, line, C_TEXT);
    snprintf(line, sizeof(line), "Frequency: %s   Idle: %s   Sensor: %s", pi->dpi_cpufreq, pi->dpi_idle,
             pi->dpi_sensor);
    gfx_text(s, c.x + PAD, y + FONT_H + 4, line, C_DIM);
    snprintf(line, sizeof(line), "%s%s%s%s", pi->dpi_flags & SIEOS_PWR_ACPI_OFF ? "ACPI power-off" : "no power-off",
             pi->dpi_flags & SIEOS_PWR_ACPI_RESET ? ", ACPI reset" : "",
             pi->dpi_flags & SIEOS_PWR_PWRBTN ? ", the power button shuts down" : "",
             pi->dpi_flags & SIEOS_PWR_HOT ? ", the processor has throttled itself" : "");
    gfx_text(s, c.x + PAD, y + 2 * (FONT_H + 4), line, C_DIM);
    if (pa.status[0])
        gfx_text(s, c.x + PAD, c.y + c.h - FONT_H - 8, pa.status, C_BAD);
}

static void set_policy(int i)
{
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 1);
        execl("/bin/poweradm", "poweradm", "set", args[i], (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st))
        snprintf(pa.status, sizeof(pa.status), "Could not change the policy (poweradm).");
    else
        pa.status[0] = 0;
    sample();
}

static void mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct rect c = fct_view_content(w);
    int hit = -1;
    for (int i = 0; i < 3; i++)
        if (rect_contains(card(c, i), x, y))
            hit = i;
    if (kind == FCT_MOUSE_MOVE && hit != pa.hover) {
        pa.hover = hit;
        fct_view_invalidate(w);
    }
    if (kind == FCT_MOUSE_DOWN && hit >= 0 && pa.ok && (int)pa.pi.dpi_policy != hit) {
        set_policy(hit);
        fct_view_invalidate(w);
    }
}

static void tick(struct fct_view *w)
{
    if (++pa.ticks % 4)                          /* (ticks come about 4 times a second) */
        return;
    sample();
    fct_view_invalidate(w);
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    for (int i = 0; i < HIST; i++)
        pa.hist[i] = -1;
    sample();
    struct fct_window_attr at = { "Power and Temperature", FCT_POS_AUTO, FCT_POS_AUTO, 520, 380, 500, 360, 0 };
    struct fct_view *w = fct_view_create(&at);
    if (!w)
        return 1;
    w->draw = draw;
    w->mouse = mouse;
    w->tick = tick;
    return fct_main();
}
