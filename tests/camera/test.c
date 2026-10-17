/*
 * tests/camera/test.c - camera-test: the USB camera's code (user/usb/uvc.c)
 * and the keyboard light's check (user/usb/kbl.c) on the host, with the
 * Lenovo 83LY's own descriptors (read from Linux's sysfs):
 *   camera.desc   04f2:b83e "Integrated Camera": device + configuration
 *   kbl-if0.rd    048d:c195 ITE 8258, interface 0's report descriptor (the light's report 7)
 *   kbl-if1.rd    its interface 1 (HID LampArray: not ours)
 * Checked: the format, frame, rates and alternate settings found; frames
 * assembled from synthetic packets (frame bit, end of frame, error bit, a
 * lost packet, header-only packets); YUY2 -> RGB values and speed.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static void lock(volatile int *l) { *l = 1; }
static void unlock(volatile int *l) { *l = 0; }
static void uvc_wake(void) {}
#include "../../user/usb/uvc.c"
#include "../../user/usb/kbl.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAILED %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *dir, const char *name, int *n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("camera-test: cannot read %s\n", p); exit(1); }
    static uint8_t b[3][4096];
    static int k;
    *n = (int)fread(b[k], 1, sizeof b[k], f);
    fclose(f);
    return b[k++];
}

/* A frame sent as the camera does: packets of up to `room` image bytes after a 12-byte header. */
static void send_frame(uvc_t *u, const uint8_t *img, uint32_t size, int fid, int room, int eof, int err, int lose)
{
    static uint8_t pk[4096];
    int n = 0;
    for (uint32_t at = 0; at < size; at += room, n++) {
        int len = size - at < (uint32_t)room ? (int)(size - at) : room, last = at + len >= size;
        memset(pk, 0, 12);
        pk[0] = 12; pk[1] = 0x80 | fid | (last && eof ? 2 : 0) | (err && n == 3 ? 0x40 : 0) | 0x0C;
        memcpy(pk + 12, img + at, len);
        if (lose && n == 7) continue;
        uvc_put(u, pk, 12 + len);
        if (n % 5 == 0) { pk[1] = 0x80 | fid; uvc_put(u, pk, 12); }   /* (header-only packets, as between bursts) */
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/camera";
    int n;
    uint8_t *d = load(dir, "camera.desc", &n);
    static uvc_t u;
    CHECK(d[1] == 1 && d[18 + 1] == 2, "not a device + configuration");
    CHECK(!uvc_parse(&u, d + 18, n - 18), "no camera found");
    CHECK(u.vc == 0 && u.vs == 1 && u.ver == 0x150, "interfaces %d %d, UVC %x", u.vc, u.vs, u.ver);
    CHECK(u.fmt == 2 && u.frm == 1 && u.w == 640 && u.h == 480, "format %d frame %d %dx%d", u.fmt, u.frm, u.w, u.h);
    CHECK(u.nival == 2 && u.ival[0] == 333333 && u.ival[1] == 666666, "intervals %d: %u %u", u.nival, u.ival[0], u.ival[1]);
    static const int esit[7] = { 128, 512, 1020, 1536, 2040, 2688, 3060 };
    CHECK(u.nalt == 7, "%d alternate settings", u.nalt);
    for (int i = 0; i < u.nalt && i < 7; i++)
        CHECK(u.alt[i].alt == i + 1 && u.alt[i].esit == esit[i] && d[18 + u.alt[i].ep + 1] == 5, "alt %d: %d bytes", u.alt[i].alt, u.alt[i].esit);
    printf("camera: %dx%d YUY2, format %d frame %d, %d.%d frames/s max, %d alternate settings (up to %d bytes per 125 us)\n",
           u.w, u.h, u.fmt, u.frm, 10000000 / u.ival[0], 100000000 / u.ival[0] % 10, u.nalt, u.alt[u.nalt - 1].esit);

    /* frames from packets */
    u.size = (uint32_t)u.w * u.h * 2;
    for (int i = 0; i < 3; i++) u.buf[i] = malloc(u.size);
    u.fill = 0; u.ready = u.sending = -1;
    uint8_t *img = malloc(u.size);
    for (uint32_t i = 0; i < u.size; i++) img[i] = (uint8_t)(i * 7 + i / 1280);
    send_frame(&u, img, u.size, 0, 3048, 1, 0, 0);
    CHECK(u.frames == 1 && u.ready >= 0 && !memcmp(u.buf[u.ready], img, u.size), "a whole frame: %u frames", u.frames);
    send_frame(&u, img, u.size, 1, 3048, 1, 1, 0);
    CHECK(u.frames == 1, "a frame with the error bit was kept");
    send_frame(&u, img, u.size, 0, 3048, 1, 0, 1);
    CHECK(u.frames == 1, "a frame with a lost packet was kept");
    img[5] ^= 0xFF;
    send_frame(&u, img, u.size, 1, 1020, 0, 0, 0);            /* no end mark: ended by the next frame's bit */
    CHECK(u.frames == 1, "a frame ended before its end");
    u.sending = u.ready == 0 ? 1 : 0;                           /* (one being sent meanwhile: not overwritten) */
    int busy = u.sending;
    send_frame(&u, img, u.size, 0, 3048, 1, 0, 0);
    CHECK(u.frames == 3 && u.ready != busy && u.fill != busy && u.fill != u.ready, "buffers %d %d %d", u.fill, u.ready, u.sending);
    CHECK(!memcmp(u.buf[u.ready], img, u.size), "the frame's bytes");

    /* YUY2 -> RGB */
    static const uint8_t px[] = { 16, 128, 235, 128,   81, 90, 81, 240,   145, 54, 41, 34,   126, 128, 126, 128 };
    static const uint32_t want[] = { 0x000000, 0xFFFFFF, 0xFF0000, 0xFF0000, 0x00FF00, 0x008600, 0x808080, 0x808080 };
    uint32_t o[8];
    uvc_rgb(o, px, 8);
    for (int i = 0; i < 8; i++) {
        int dr = (int)(o[i] >> 16) - (int)(want[i] >> 16), dg = (int)(o[i] >> 8 & 255) - (int)(want[i] >> 8 & 255), db = (int)(o[i] & 255) - (int)(want[i] & 255);
        CHECK(dr * dr + dg * dg + db * db <= 12, "pixel %d: %06x, wanted about %06x", i, o[i], want[i]);
    }
    uint32_t *rgb = malloc(u.size * 2);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 100; i++) uvc_rgb(rgb, u.buf[u.ready], u.w * u.h);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("camera: YUY2 -> RGB, 640x480: %ld us a frame\n", ((t1.tv_sec - t0.tv_sec) * 1000000000L + t1.tv_nsec - t0.tv_nsec) / 100000);

    /* the keyboard light: only interface 0's feature report 7 */
    uint8_t *k0 = load(dir, "kbl-if0.rd", &n);
    CHECK(kbl_match(k0, n), "interface 0: report 7 not found");
    CHECK(!kbl_match(k0, 40), "a cut descriptor matched");
    uint8_t *k1 = load(dir, "kbl-if1.rd", &n);
    CHECK(!kbl_match(k1, n), "interface 1 matched");
    if (fails) { printf("camera-test: %d FAILED\n", fails); return 1; }
    printf("camera-test: ok\n");
    return 0;
}
