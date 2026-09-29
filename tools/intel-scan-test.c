/*
 * intel-scan-test.c - host test of the Intel display driver's register scan
 * (kernel/intel_gen12.c, built with INTEL_HOST_TEST) on simulated registers:
 * a GOP-like setup (pipe A linear XRGB8888 on eDP, 1920x1200), a second
 * pipe on a Type-C DP port, and the cases that must be refused; then the
 * mode setting's register writes (plane, pipe source, the scaler window).
 */
#define INTEL_HOST_TEST
#include "../kernel/intel_gen12.c"
#include <stdlib.h>

static uint32_t regs[0x80000 / 4];
static void apply(const struct intel_reg *r, int n) { for (int i = 0; i < n; i++) regs[r[i].reg / 4] = r[i].val; }
static uint32_t rd(void *ctx, uint32_t reg) { (void)ctx; return regs[reg / 4]; }
static void wr(uint32_t reg, uint32_t v) { regs[reg / 4] = v; }

static void pipe(int p, uint32_t w, uint32_t h, uint32_t fmt, uint32_t tiling, uint32_t surf, int port, int mode)
{
    uint32_t o = p * PIPE_STEP, stride = (w * 4 + 63) / 64;
    wr(TRANSCONF + o, 1U << 31);
    wr(PLANE_CTL + o, 1U << 31 | fmt << 24 | tiling << 10);
    wr(PLANE_SIZE + o, (h - 1) << 16 | (w - 1));
    wr(PLANE_STRIDE + o, stride);
    wr(PLANE_SURF + o, surf);
    wr(TRANS_DDI_FUNC_CTL + o, 1U << 31 | (uint32_t)(port + 1) << 27 | (uint32_t)mode << 24);
    wr(TRANS_HTOTAL + o, (w + 160 - 1) << 16 | (w - 1));      /* (blanking in the upper half) */
    wr(TRANS_VTOTAL + o, (h + 35 - 1) << 16 | (h - 1));
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    struct intel_pipe out[NPIPES];
    /* Arrow Lake-P laptop: eDP on DDI A, 1920x1200 at GGTT 0; a dock on TC1 (DP MST) at 3840x2160 */
    memset(regs, 0, sizeof(regs));
    pipe(0, 1920, 1200, 4, 0, 0x000000, 0, 2);
    pipe(1, 3840, 2160, 4, 0, 0x1000000, 3, 3);
    int n = intel_scan(rd, NULL, 14, 256UL << 20, out, NPIPES);
    for (int i = 0; i < n; i++)
        printf("pipe %c %ux%u stride %u surf %#llx -> %s %s%s\n", 'A' + out[i].pipe, out[i].width, out[i].height,
               out[i].stride, (unsigned long long)out[i].surf, out[i].output, out[i].usable ? "usable" : "no: ",
               out[i].why);
    CHECK(n == 2);
    CHECK(out[0].usable && out[0].width == 1920 && out[0].height == 1200 && out[0].stride == 7680);
    CHECK(!strcmp(out[0].output, "DDI A (DP)"));
    CHECK(out[1].usable && !strcmp(out[1].output, "DDI TC1 (DP MST)"));
    /* refused: X-tiled, 10-bit, outside the aperture */
    memset(regs, 0, sizeof(regs));
    pipe(0, 1920, 1080, 4, 1, 0, 0, 2);
    pipe(1, 1920, 1080, 10, 0, 0, 1, 0);
    pipe(2, 1920, 1080, 4, 0, 0xFF00000, 3, 2);            /* ends past 256 MiB */
    n = intel_scan(rd, NULL, 12, 256UL << 20, out, NPIPES);
    CHECK(n == 3 && !out[0].usable && !out[1].usable && !out[2].usable);
    for (int i = 0; i < n; i++)
        printf("pipe %c -> %s: %s\n", 'A' + out[i].pipe, out[i].output, out[i].why);
    /* Ice Lake (display 11): the DDI in 30:28, the port itself; eDP on A, DP on TC2 (port D) */
    memset(regs, 0, sizeof(regs));
    pipe(0, 1920, 1080, 4, 0, 0, 0, 2);
    pipe(1, 2560, 1440, 4, 0, 0x800000, 0, 2);
    wr(TRANS_DDI_FUNC_CTL, 1U << 31 | 0U << 28 | 2U << 24);
    wr(TRANS_DDI_FUNC_CTL + PIPE_STEP, 1U << 31 | 3U << 28 | 2U << 24);
    n = intel_scan(rd, NULL, 11, 256UL << 20, out, NPIPES);
    for (int i = 0; i < n; i++)
        printf("display 11: pipe %c -> %s %s\n", 'A' + out[i].pipe, out[i].output, out[i].usable ? "usable" : out[i].why);
    CHECK(n == 2 && out[0].usable && !strcmp(out[0].output, "DDI A (DP)"));
    CHECK(out[1].usable && !strcmp(out[1].output, "DDI TC2 (DP)"));
    /* nothing running; a pipe reading all ones (not there) */
    memset(regs, 0, sizeof(regs));
    CHECK(intel_scan(rd, NULL, 12, 256UL << 20, out, NPIPES) == 0);
    memset(regs, 0xFF, sizeof(regs));
    CHECK(intel_scan(rd, NULL, 12, 256UL << 20, out, NPIPES) == 0);

    /* mode setting on the laptop's eDP (1920x1200, the firmware's mode) */
    memset(regs, 0, sizeof(regs));
    pipe(0, 1920, 1200, 4, 0, 0x200000, 0, 2);
    n = intel_scan(rd, NULL, 14, 256UL << 20, out, NPIPES);
    struct intel_pipe *ip = &out[0];
    uint64_t fbsize = (uint64_t)ip->stride * ip->height;
    CHECK(n == 1 && ip->native_w == 1920 && ip->native_h == 1200 && !ip->scaled);
    CHECK(intel_mode_ok(ip, fbsize, 1024, 768) && intel_mode_ok(ip, fbsize, 1920, 1200));
    CHECK(!intel_mode_ok(ip, fbsize, 2560, 1600) && !intel_mode_ok(ip, fbsize, 1920, 1440) &&
          !intel_mode_ok(ip, fbsize, 320, 200));
    struct intel_reg r[INTEL_MODE_REGS];
    int k = intel_mode_regs(ip, 1280, 800, r);                 /* the same shape: the whole panel */
    CHECK(k <= INTEL_MODE_REGS && r[k - 1].reg == PLANE_SURF && r[k - 1].val == 0x200000);
    apply(r, k);
    CHECK(regs[PIPE_SRC / 4] == (1279U << 16 | 799) && regs[PLANE_SIZE / 4] == (799U << 16 | 1279));
    CHECK(regs[PLANE_STRIDE / 4] == 1280 * 4 / 64);
    CHECK(regs[PS_CTRL / 4] == PS_EN && regs[PS_WIN_POS / 4] == 0 && regs[PS_WIN_SZ / 4] == (1920U << 16 | 1200));
    apply(r, intel_mode_regs(ip, 1024, 768, r));               /* 4:3: bars left and right */
    CHECK(regs[PS_WIN_SZ / 4] == (1600U << 16 | 1200) && regs[PS_WIN_POS / 4] == (160U << 16 | 0));
    apply(r, intel_mode_regs(ip, 1920, 1080, r));              /* 16:9: bars above and below */
    CHECK(regs[PS_WIN_SZ / 4] == (1920U << 16 | 1080) && regs[PS_WIN_POS / 4] == 60);
    CHECK(regs[PLANE_STRIDE / 4] == 1920 * 4 / 64 && regs[PIPE_SRC / 4] == (1919U << 16 | 1079));
    apply(r, intel_mode_regs(ip, 1366, 768, r));               /* the stride rounds up to 64 bytes */
    CHECK(regs[PLANE_STRIDE / 4] == (1366 * 4 + 63) / 64);
    apply(r, intel_mode_regs(ip, 1920, 1200, r));              /* native again: the scaler off */
    CHECK(regs[PS_CTRL / 4] == 0 && regs[PS_WIN_SZ / 4] == 0 && regs[PIPE_SRC / 4] == (1919U << 16 | 1199));
    printf("mode 1920x1200 -> 1024x768: window %ux%u at %u,%u\n", 1600, 1200, 160, 0);
    /* pipe B, and the firmware's own scaled mode (1024x768 on a 2560x1600 panel) is seen */
    memset(regs, 0, sizeof(regs));
    pipe(1, 1024, 768, 4, 0, 0, 0, 2);
    wr(TRANS_HTOTAL + PIPE_STEP, 2559);
    wr(TRANS_VTOTAL + PIPE_STEP, 1599);
    wr(PS_CTRL + PS_STEP, PS_EN);
    n = intel_scan(rd, NULL, 12, 256UL << 20, out, NPIPES);
    CHECK(n == 1 && out[0].pipe == 1 && out[0].scaled && out[0].native_w == 2560 && out[0].native_h == 1600);
    k = intel_mode_regs(&out[0], 800, 600, r);
    CHECK(r[0].reg == PIPE_SRC + PIPE_STEP && r[5].reg == PS_CTRL + PS_STEP);
    printf("intel-scan-test: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
