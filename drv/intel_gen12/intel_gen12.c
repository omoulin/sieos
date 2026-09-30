/*
 * intel_gen12.c - Intel integrated graphics, display versions 11 to 14:
 * Ice Lake Iris Plus / UHD Graphics G1-G7 (8086:8A50-8A71, display 11),
 * Raptor Lake-S UHD Graphics (8086:A780-A78B, display 12) and Arrow
 * Lake-P Arc 130T/140T (8086:7D51, 7DD1, display 14).
 *
 * Display 11 has the same pipes, universal planes and pipe scalers; only
 * TRANS_DDI_FUNC_CTL differs: 30:28 selects the DDI (the port itself,
 * A = 0), and its ports are A and B (combo PHYs) and C to F (Type-C,
 * TC1 to TC4).  And a laptop's panel (eDP, on DDI A) has a transcoder of
 * its own, which any pipe can feed: TRANSCONF_EDP 0x7F008,
 * TRANS_DDI_FUNC_CTL_EDP 0x6F400 (14:12 the pipe: 0 or 4 A, 5 B, 6 C),
 * its timings at 0x6F000 (TRANS_HTOTAL_EDP) and 0x6F00C; the pipe it
 * feeds has transcoder A...C off.
 *
 * The driver takes over the mode the firmware (UEFI GOP) set: it reads
 * which pipes are running, their primary planes and the outputs (DDI) their
 * transcoders drive.  A pipe whose primary plane is linear XRGB8888 scans
 * out of the graphics aperture (BAR2) at the plane's GGTT offset; this
 * driver takes that display over from the firmware driver (same memory, now
 * named and write-combining), or adds it when it is not the firmware's.
 * Anything else (tiled or other formats, no aperture, an unexpected
 * address) leaves the firmware driver in charge.
 *
 * Mode setting, within the running output: the transcoder keeps the
 * native timings the firmware trained the link and the panel for (read
 * from TRANS_HTOTAL/VTOTAL); a smaller mode is a smaller pipe source and
 * plane, which the pipe's first scaler stretches to the output, keeping
 * the aspect ratio (black bars): the pipe scaler's purpose in Intel's
 * display documentation.  Every mode up to the native one
 * fits in the framebuffer: when the firmware's is smaller (it set a small
 * mode, scaled up), the driver gives the plane one of the native size in
 * RAM, mapped in the GGTT (BAR0's upper half, 8-byte entries: the address | 1)
 * at the top of the aperture (over what the firmware had there, unless a
 * plane scans from it), with the firmware's picture copied in; it
 * stays for good, so /dev/fb0's memory never moves.
 *
 * Not done: lighting an output that is off (the PHYs and PLLs - C10/C20
 * on display 14 - DP link training, panel power sequencing and backlight),
 * modes above the native one, the cursor plane (it needs a display buffer
 * allocation and watermarks), page flips, EDID.  Written from Intel's
 * register documentation; not tested on the hardware.
 *
 * Registers (pipe/transcoder A; B, C, D at +0x1000 each):
 *   TRANS_DDI_FUNC_CTL 0x60400  31 enable, 30:27 DDI select (port + 1), 26:24 mode
 *                               (display 11: 30:28 DDI select, the port)
 *   PIPE_SRC           0x6001C  (width - 1) << 16 | (height - 1)
 *   TRANSCONF          0x70008  31 enable
 *   PLANE_CTL_1        0x70180  31 enable, 27:24 format (4: XRGB8888), 12:10 tiling (0 linear)
 *   PLANE_STRIDE_1     0x70188  in 64-byte units (linear)
 *   PLANE_SIZE_1       0x70190  (height - 1) << 16 | (width - 1)
 *   PLANE_SURF_1       0x7019C  GGTT address, 4 KiB aligned; writing it arms the plane update
 *   PLANE_POS_1        0x7018C  PLANE_OFFSET_1 0x701A4 (both 0: the whole plane, from its start)
 *   TRANS_HTOTAL       0x60000  12:0 active width - 1   TRANS_VTOTAL 0x6000C  12:0 active height - 1
 *   PS_CTRL_1          0x68180  31 enable, 26:25 binding (0: the pipe), 24:23 filter (0: medium)
 *   PS_WIN_POS_1       0x68170  x << 16 | y   PS_WIN_SZ_1 0x68174  width << 16 | height
 *                               (the first scaler of pipe A; +0x800 per pipe)
 * Pipe source, plane and scaler registers are double-buffered: they take
 * effect together at the next vertical blank, armed by PLANE_SURF.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifdef INTEL_HOST_TEST
#include "intel_host_test.h"
#else
#include "display.h"
#include "ddi.h"
#include "pci.h"
#include "mm.h"
#endif

#define TRANS_DDI_FUNC_CTL 0x60400
#define PIPE_SRC     0x6001C
#define TRANSCONF    0x70008
#define PLANE_CTL    0x70180
#define PLANE_STRIDE 0x70188
#define PLANE_SIZE   0x70190
#define PLANE_SURF   0x7019C
#define PLANE_POS    0x7018C
#define PLANE_OFFSET 0x701A4
#define TRANS_HTOTAL 0x60000
#define TRANS_DDI_FUNC_CTL_EDP 0x6F400      /* display 11 (9-11): the eDP transcoder */
#define TRANSCONF_EDP          0x7F008
#define TRANS_HTOTAL_EDP       0x6F000
#define TRANS_VTOTAL_EDP       0x6F00C
#define TRANS_VTOTAL 0x6000C
#define PS_WIN_POS   0x68170
#define PS_WIN_SZ    0x68174
#define PS_CTRL      0x68180
#define PS_EN        (1U << 31)
#define PS_STEP      0x800
#define PIPE_STEP    0x1000
#define NPIPES       4
#define MIN_W        640                         /* the smallest mode offered */
#define MIN_H        480

struct intel_model {
    uint16_t id;
    int ver;                                     /* display version */
    const char *name;
};

static const struct intel_model models[] = {
    { 0x8A50, 11, "Iris Plus Graphics (Ice Lake)" },
    { 0x8A51, 11, "Iris Plus Graphics G7 (Ice Lake)" },
    { 0x8A52, 11, "Iris Plus Graphics G7 (Ice Lake)" },
    { 0x8A53, 11, "Iris Plus Graphics G7 (Ice Lake)" },
    { 0x8A54, 11, "Iris Plus Graphics (Ice Lake)" },
    { 0x8A56, 11, "UHD Graphics G1 (Ice Lake)" },
    { 0x8A57, 11, "UHD Graphics (Ice Lake)" },
    { 0x8A58, 11, "UHD Graphics G1 (Ice Lake-Y)" },
    { 0x8A59, 11, "UHD Graphics (Ice Lake)" },
    { 0x8A5A, 11, "Iris Plus Graphics G4 (Ice Lake)" },
    { 0x8A5B, 11, "UHD Graphics (Ice Lake)" },
    { 0x8A5C, 11, "Iris Plus Graphics G4 (Ice Lake)" },
    { 0x8A5D, 11, "UHD Graphics (Ice Lake)" },
    { 0x8A70, 11, "UHD Graphics (Ice Lake)" },
    { 0x8A71, 11, "UHD Graphics (Ice Lake)" },
    { 0x7D51, 14, "Arc 130T/140T (Arrow Lake-P)" },
    { 0x7DD1, 14, "Graphics (Arrow Lake-P)" },
    { 0xA780, 12, "UHD Graphics 770 (Raptor Lake-S)" },
    { 0xA781, 12, "UHD Graphics (Raptor Lake-S)" },
    { 0xA782, 12, "UHD Graphics (Raptor Lake-S)" },
    { 0xA783, 12, "UHD Graphics (Raptor Lake-S)" },
    { 0xA788, 12, "UHD Graphics (Raptor Lake-S)" },
    { 0xA789, 12, "UHD Graphics (Raptor Lake-S)" },
    { 0xA78A, 12, "UHD Graphics (Raptor Lake-S)" },
    { 0xA78B, 12, "UHD Graphics (Raptor Lake-S)" },
};

/* What one running pipe shows. */
struct intel_pipe {
    int pipe;
    bool usable;                                 /* linear XRGB8888, in the aperture */
    uint32_t width, height, stride;
    uint32_t native_w, native_h;                 /* the transcoder's active size (the output's mode) */
    bool scaled;                                 /* the pipe scaler is on (the firmware's mode is not native) */
    uint64_t surf;                               /* GGTT offset */
    char output[40];                             /* "DDI A (DP)" */
    char why[48];                                /* when not usable */
};

static const char *port_name(int ver, int idx, char *buf, size_t n)
{
    int combo = ver == 11 ? 2 : 3;               /* DDI A, B (display 11); A, B, C (12+) */
    if (idx < combo)
        snprintf(buf, n, "DDI %c", 'A' + idx);
    else
        snprintf(buf, n, "DDI TC%d", idx - combo + 1);   /* Type-C / Thunderbolt ports */
    return buf;
}

/*
 * The running pipes, from the registers read by rd; aperture_size bounds
 * the scan-out addresses.  Returns the number found (into out).
 */
static int intel_scan(uint32_t (*rd)(void *ctx, uint32_t reg), void *ctx, int ver, uint64_t aperture_size,
                      struct intel_pipe *out, int max)
{
    int n = 0, edp_pipe = -1;
    if (ver == 11) {                             /* the eDP transcoder, and the pipe that feeds it */
        uint32_t ec = rd(ctx, TRANSCONF_EDP), ef = rd(ctx, TRANS_DDI_FUNC_CTL_EDP);
        if (ec != 0xFFFFFFFF && (ec & (1U << 31)) && (ef & (1U << 31))) {
            int sel = (ef >> 12) & 7;
            edp_pipe = sel == 5 ? 1 : sel == 6 ? 2 : 0;
        }
    }
    for (int p = 0; p < NPIPES && n < max; p++) {
        uint32_t conf = rd(ctx, TRANSCONF + p * PIPE_STEP), ctl = rd(ctx, PLANE_CTL + p * PIPE_STEP);
        bool edp = p == edp_pipe;
        if ((!edp && (conf == 0xFFFFFFFF || !(conf & (1U << 31)))) || ctl == 0xFFFFFFFF || !(ctl & (1U << 31)))
            continue;                            /* off (or a missing pipe reading as all ones) */
        struct intel_pipe *ip = &out[n++];
        memset(ip, 0, sizeof(*ip));
        ip->pipe = p;
        uint32_t size = rd(ctx, PLANE_SIZE + p * PIPE_STEP);
        ip->width = (size & 0x1FFF) + 1;
        ip->height = ((size >> 16) & 0x1FFF) + 1;
        ip->stride = (rd(ctx, PLANE_STRIDE + p * PIPE_STEP) & 0xFFF) * 64;
        ip->surf = rd(ctx, PLANE_SURF + p * PIPE_STEP) & ~0xFFFU;
        uint32_t ht = rd(ctx, edp ? TRANS_HTOTAL_EDP : TRANS_HTOTAL + p * PIPE_STEP);
        uint32_t vt = rd(ctx, edp ? TRANS_VTOTAL_EDP : TRANS_VTOTAL + p * PIPE_STEP);
        ip->native_w = ht ? (ht & 0x1FFF) + 1 : ip->width;
        ip->native_h = vt ? (vt & 0x1FFF) + 1 : ip->height;
        ip->scaled = (rd(ctx, PS_CTRL + p * PS_STEP) & PS_EN) != 0;
        uint32_t ddi = rd(ctx, TRANS_DDI_FUNC_CTL + p * PIPE_STEP);
        if (edp) {
            snprintf(ip->output, sizeof(ip->output), "DDI A (eDP)");
        } else if (ddi & (1U << 31)) {
            static const char *const modes[] = { "HDMI", "DVI", "DP", "DP MST", "FDI", "?", "?", "?" };
            char pn[16];
            int sel = ver == 11 ? ((ddi >> 28) & 7) + 1 : (ddi >> 27) & 0xF;   /* port + 1 */
            if (sel)
                snprintf(ip->output, sizeof(ip->output), "%s (%s)", port_name(ver, sel - 1, pn, sizeof(pn)),
                         modes[(ddi >> 24) & 7]);
            else
                snprintf(ip->output, sizeof(ip->output), "no DDI");
        } else {
            snprintf(ip->output, sizeof(ip->output), "transcoder off");
        }
        uint32_t fmt = (ctl >> 24) & 0xF, tiling = (ctl >> 10) & 7;
        if (fmt != 4)
            snprintf(ip->why, sizeof(ip->why), "plane format %u, not XRGB8888", fmt);
        else if (tiling)
            snprintf(ip->why, sizeof(ip->why), "tiled plane (%u)", tiling);
        else if (ip->stride < ip->width * 4)
            snprintf(ip->why, sizeof(ip->why), "stride %u too small", ip->stride);
        else if (!aperture_size || ip->surf + (uint64_t)ip->stride * ip->height > aperture_size)
            snprintf(ip->why, sizeof(ip->why), "surface outside the aperture");
        else
            ip->usable = true;
    }
    return n;
}

/* The standard modes offered below the native one: 4:3, 16:9, 16:10 and 3:2 (the Surface's panels). */
static const uint16_t std_modes[][2] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
    { 1366, 768 }, { 1368, 912 }, { 1400, 1050 }, { 1440, 900 }, { 1440, 960 }, { 1500, 1000 }, { 1600, 900 },
    { 1600, 1200 }, { 1680, 1050 }, { 1824, 1216 }, { 1920, 1080 }, { 1920, 1200 }, { 1920, 1280 },
    { 2048, 1536 }, { 2160, 1440 }, { 2256, 1504 }, { 2560, 1440 }, { 2560, 1600 }, { 2880, 1800 },
    { 3000, 2000 }, { 3200, 1800 }, { 3840, 2160 }, { 3840, 2400 },
};

/* A mode can be shown: at least MIN_W x MIN_H, at most the native size, in the surface memory. */
static bool intel_mode_ok(const struct intel_pipe *ip, uint64_t surf_size, uint32_t w, uint32_t h)
{
    uint64_t stride = ((uint64_t)w * 4 + 63) & ~63ULL;
    return w >= MIN_W && h >= MIN_H && w <= ip->native_w && h <= ip->native_h && stride * h <= surf_size;
}

struct intel_reg {
    uint32_t reg, val;
};
#define INTEL_MODE_REGS 9

/*
 * The register writes, in order, that show a w x h linear XRGB8888 plane
 * (at GGTT surf) on pipe p: native size unscaled, a smaller one through
 * the pipe scaler, centred with the aspect ratio kept.  The last one
 * (PLANE_SURF) arms the update.  Returns the number of writes.
 */
static int intel_mode_regs(const struct intel_pipe *ip, uint32_t w, uint32_t h, struct intel_reg *out)
{
    uint32_t po = ip->pipe * PIPE_STEP, so = ip->pipe * PS_STEP, stride = (w * 4 + 63) & ~63U;
    uint32_t nw = ip->native_w, nh = ip->native_h;
    int n = 0;
    out[n++] = (struct intel_reg){ PIPE_SRC + po, (w - 1) << 16 | (h - 1) };
    out[n++] = (struct intel_reg){ PLANE_SIZE + po, (h - 1) << 16 | (w - 1) };
    out[n++] = (struct intel_reg){ PLANE_STRIDE + po, stride / 64 };
    out[n++] = (struct intel_reg){ PLANE_POS + po, 0 };
    out[n++] = (struct intel_reg){ PLANE_OFFSET + po, 0 };
    if (w == nw && h == nh) {
        out[n++] = (struct intel_reg){ PS_CTRL + so, 0 };
        out[n++] = (struct intel_reg){ PS_WIN_POS + so, 0 };
        out[n++] = (struct intel_reg){ PS_WIN_SZ + so, 0 };
    } else {
        uint32_t sw = nw, sh = nh;               /* the largest window of the mode's shape */
        if ((uint64_t)nw * h > (uint64_t)nh * w)
            sw = (uint32_t)(((uint64_t)w * nh + h / 2) / h) & ~1U;   /* a wider output: bars left and right */
        else
            sh = (uint32_t)(((uint64_t)h * nw + w / 2) / w) & ~1U;   /* a taller one: bars above and below */
        sw = sw > nw ? nw : sw;
        sh = sh > nh ? nh : sh;
        out[n++] = (struct intel_reg){ PS_CTRL + so, PS_EN };        /* bound to the pipe, medium filter */
        out[n++] = (struct intel_reg){ PS_WIN_POS + so, ((nw - sw) / 2) << 16 | (nh - sh) / 2 };
        out[n++] = (struct intel_reg){ PS_WIN_SZ + so, sw << 16 | sh };
    }
    out[n++] = (struct intel_reg){ PLANE_SURF + po, (uint32_t)ip->surf };
    return n;
}

#ifndef INTEL_HOST_TEST

/* The GGTT: the upper half of BAR0 (GTTMMADR), 8-byte entries (gen8 and later). */
#define GFX_FLSH_CNTL   0x101008                 /* write 1: the GGTT's new entries take effect */

struct intel {
    volatile uint32_t *regs;
    const struct intel_model *model;
    uint64_t aperture, aperture_size;
    struct intel_pipe pipes[NPIPES];
    int npipes;
    struct intel_pipe *shown;                    /* the pipe the display is on */
    uint64_t surf_size;                          /* the memory of its framebuffer */
    uint64_t bar0, bar0_size;
    char note[128];
};


static struct intel card;

static uint32_t mmio_rd(void *ctx, uint32_t reg)
{
    struct intel *c = ctx;
    return c->regs[reg / 4];
}

static void mmio_wr(struct intel *c, uint32_t reg, uint32_t v)
{
    c->regs[reg / 4] = v;
}

/*
 * A framebuffer of need bytes in RAM for the plane of ip, mapped in the
 * GGTT at the top of the aperture, with the picture (len bytes at the
 * current surface) copied in; the plane is moved there.  False if there
 * is no room (or the GGTT entries there are in use).
 */
static bool intel_grow_fb(struct intel *c, struct intel_pipe *ip, uint64_t len, uint64_t need)
{
    uint64_t pages = PAGE_ALIGN_UP(need) / PAGE_SIZE, bytes = pages * PAGE_SIZE, align = 2UL << 20;
    /* where: the top of the aperture, else just after the firmware's framebuffer, else below it */
    uint64_t fw_lo = ip->surf, fw_hi = ip->surf + len, off = ~0UL;
    uint64_t cand[3] = { (c->aperture_size > bytes ? c->aperture_size - bytes : 0) & ~(align - 1),
                         (fw_hi + align - 1) & ~(align - 1), fw_lo >= bytes ? (fw_lo - bytes) & ~(align - 1) : ~0UL };
    for (int i = 0; i < 3 && off == ~0UL; i++)
        if (cand[i] != ~0UL && cand[i] + bytes <= c->aperture_size && (cand[i] + bytes <= fw_lo || cand[i] >= fw_hi))
            off = cand[i];
    if (off == ~0UL || c->bar0_size < (4UL << 20)) {
        kprintf("intel: no room for a %lu KiB framebuffer (aperture %lu MiB, BAR0 %lu MiB, the firmware's at %#lx)\n",
                bytes / 1024, c->aperture_size >> 20, c->bar0_size >> 20, fw_lo);
        return false;
    }
    volatile uint64_t *gtt = mmio_map(c->bar0 + c->bar0_size / 2 + (off / PAGE_SIZE) * 8, pages * 8);
    if (!gtt) {
        kprintf("intel: the GGTT cannot be mapped\n");
        return false;
    }
    /* Nothing but the planes uses the GGTT before a driver: the region is ours unless a plane scans from it
     * (the firmware often maps the whole aperture, to its stolen memory, so the old entries do not say). */
    for (int p = 0; p < NPIPES; p++) {
        uint32_t ctl = mmio_rd(c, PLANE_CTL + p * PIPE_STEP), surf = mmio_rd(c, PLANE_SURF + p * PIPE_STEP) & ~0xFFFU;
        if (ctl != 0xFFFFFFFF && (ctl & (1U << 31)) && surf >= off && surf < off + bytes) {
            kprintf("intel: the GGTT at %#lx is pipe %c's: the framebuffer stays %lu KiB\n", off, 'A' + p, len / 1024);
            return false;
        }
    }
    uint64_t *pa = kmalloc(pages * sizeof(uint64_t));
    if (!pa) {
        kprintf("intel: no memory for the framebuffer\n");
        return false;
    }
    for (uint64_t i = 0; i < pages; i++)
        if (!(pa[i] = pmm_alloc())) {            /* (zeroed; never freed: the plane keeps it) */
            while (i--)
                pmm_free(pa[i]);
            kfree(pa);
            kprintf("intel: no memory for the framebuffer\n");
            return false;
        }
    __asm__ volatile("wbinvd" ::: "memory");     /* the zeros to memory: the display does not snoop */
    for (uint64_t i = 0; i < pages; i++)
        gtt[i] = pa[i] | 1;                      /* present */
    (void)gtt[pages - 1];
    mmio_wr(c, GFX_FLSH_CNTL, 1);
    (void)mmio_rd(c, GFX_FLSH_CNTL);
    kfree(pa);
    uint8_t *from = mmio_map_wc(c->aperture + ip->surf, len), *to = mmio_map_wc(c->aperture + off, bytes);
    if (!from || !to) {
        kprintf("intel: the aperture cannot be mapped\n");
        return false;
    }
    memcpy(to, from, len);                       /* the picture (the same stride) */
    mmio_wr(c, PLANE_SURF + ip->pipe * PIPE_STEP, (uint32_t)off);   /* at the next vertical blank */
    snprintf(c->note, sizeof(c->note), "intel: pipe %c: a %lu KiB framebuffer for modes up to %ux%u (GGTT %#lx)\n",
             'A' + ip->pipe, bytes / 1024, ip->native_w, ip->native_h, off);   /* (said once the console is there) */
    ip->surf = off;
    return true;
}

static int intel_modes(struct display *d, struct display_mode *out, int max)
{
    struct intel *c = d->drv;
    struct intel_pipe *ip = c->shown;
    int n = 0;
    for (size_t i = 0; i < ARRAY_SIZE(std_modes) && n < max; i++) {
        uint32_t w = std_modes[i][0], h = std_modes[i][1];
        if (intel_mode_ok(ip, c->surf_size, w, h) && !(w == ip->native_w && h == ip->native_h))
            out[n++] = (struct display_mode){ w, h, (w * 4 + 63) & ~63U, 0, false };
    }
    if (n < max && intel_mode_ok(ip, c->surf_size, ip->native_w, ip->native_h))   /* the output's own */
        out[n++] = (struct display_mode){ ip->native_w, ip->native_h, (ip->native_w * 4 + 63) & ~63U, 0, true };
    return n;
}

static int intel_set_mode(struct display *d, const struct display_mode *m)
{
    struct intel *c = d->drv;
    struct intel_pipe *ip = c->shown;
    if (!intel_mode_ok(ip, c->surf_size, m->width, m->height))
        return -EINVAL;
    struct intel_reg regs[INTEL_MODE_REGS];
    memset(d->fb, 0, (size_t)c->surf_size);      /* (black, before the new mode shows) */
    int n = intel_mode_regs(ip, m->width, m->height, regs);
    for (int i = 0; i < n; i++)
        mmio_wr(c, regs[i].reg, regs[i].val);
    uint32_t size = mmio_rd(c, PLANE_SIZE + ip->pipe * PIPE_STEP);
    if (size != regs[1].val) {
        kprintf("intel: pipe %c: the plane did not take %ux%u\n", 'A' + ip->pipe, m->width, m->height);
        return -EIO;
    }
    ip->width = m->width;
    ip->height = m->height;
    ip->stride = (m->width * 4 + 63) & ~63U;
    ip->scaled = m->width != ip->native_w || m->height != ip->native_h;
    d->mode = *m;
    d->mode.pitch = ip->stride;
    return 0;
}

static const struct display_ops intel_ops = { "intel-gen12", DISPLAY_PHYSICAL, intel_modes, intel_set_mode };

void intel_probe(void)
{
    struct pci_dev pd;
    const struct intel_model *model = NULL;
    for (int i = 0; i < pci_count() && !model; i++) {
        const struct pci_dev *d = pci_at(i);
        if (d->vendor != 0x8086 || d->class_code != 3)
            continue;
        for (size_t k = 0; k < ARRAY_SIZE(models); k++)
            if (models[k].id == d->device) {
                model = &models[k];
                pd = *d;
            }
    }
    if (!model)
        return;
    struct intel *c = &card;
    c->model = model;
    bool io;
    uint64_t bar0 = pci_bar_addr(&pd, 0, &io);
    if (!bar0 || io) {
        kprintf("intel: %s: no register BAR\n", model->name);
        return;
    }
    c->aperture = pci_bar_addr(&pd, 2, &io);
    c->aperture_size = c->aperture && !io ? pci_bar_size(&pd, 2) : 0;
    c->bar0 = bar0;
    c->bar0_size = pci_bar_size(&pd, 0);
    c->regs = mmio_map(bar0, 2 << 20);           /* the display registers (and GFX_FLSH_CNTL) */
    if (!c->regs)
        return;
    c->npipes = intel_scan(mmio_rd, c, model->ver, c->aperture_size, c->pipes, NPIPES);
    if (!c->npipes)
        kprintf("intel: %s: no running pipe (TRANSCONF %08x %08x %08x, eDP %08x): the firmware's framebuffer stays\n",
                model->name, mmio_rd(c, TRANSCONF), mmio_rd(c, TRANSCONF + PIPE_STEP), mmio_rd(c, TRANSCONF + 2 * PIPE_STEP),
                model->ver == 11 ? mmio_rd(c, TRANSCONF_EDP) : 0);
    bool taken = false;
    for (int i = 0; i < c->npipes; i++) {
        struct intel_pipe *ip = &c->pipes[i];
        kprintf("intel: %s: pipe %c %ux%u (output %ux%u%s) -> %s%s%s\n", model->name, 'A' + ip->pipe, ip->width,
                ip->height, ip->native_w, ip->native_h, ip->scaled ? ", scaled" : "", ip->output,
                ip->usable ? "" : ": ", ip->usable ? "" : ip->why);
        if (!ip->usable || taken)
            continue;
        uint64_t phys = c->aperture + ip->surf, len = (uint64_t)ip->stride * ip->height;
        uint64_t lo = phys, hi = phys + len;     /* (the firmware display's memory, for the takeover) */
        uint64_t need = (uint64_t)((ip->native_w * 4 + 63) & ~63U) * ip->native_h;
        if (need > len && intel_grow_fb(c, ip, len, need)) {
            phys = c->aperture + ip->surf;
            len = PAGE_ALIGN_UP(need);
        }
        char desc[48];                           /* (the firmware's framebuffer, or a display of its own) */
        snprintf(desc, sizeof(desc), "Intel %s, pipe %c", model->name, 'A' + ip->pipe);
        struct display_mode m = { ip->width, ip->height, ip->stride, 0, ip->width == ip->native_w &&
                                                                          ip->height == ip->native_h };
        c->shown = ip;
        c->surf_size = len;
        if (display_takeover(lo, hi, &intel_ops, c, desc, &m, phys, len))
            taken = true;
        if (c->note[0]) {
            kprintf("%s", c->note);
            c->note[0] = 0;
        }
    }
    if (taken)
        pci_claim(&pd, "intel-gen12");
}

#endif

#ifndef INTEL_HOST_TEST
DDI_DRIVER("intel_gen12", DDI_PHASE_DISPLAY, "Intel integrated graphics (Gen9 to Gen12 display engines)");
DDI_ALIAS("pciclass,0300");
DDI_ALIAS("pciclass,0380");

int _init(void)
{
    intel_probe();
    return 0;
}
#endif
