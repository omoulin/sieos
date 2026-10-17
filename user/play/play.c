/*
 * play - Sound, one program under three names (docs/audio.md):
 *   play FILE.wav...          plays WAV files: 48 kHz, 16-bit, stereo; a
 *                             headset's pause holds it, next and previous
 *                             skip, stop ends it (audiod's AUDIO_KEY)
 *   rec FILE.wav [SECONDS]    records that long (5 s if not said) from the
 *                             microphone or line in, in that format
 *   vol [0-100|mute]          shows or sets the volume (a number unmutes)
 *   vol out [N]               lists the outputs (speakers, monitors with
 *                             sound), or plays to output N
 * through the audio server (port "audio", /bin/audiod; started when first
 * needed). Ctrl-C stops play and rec.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#define RATE 48000
static long port;
static uint8_t buf[64 << 10];

static long call(msg_t *m)
{
    long e = call_named(&port, "audio", m, 1);
    return e < 0 ? e : (long)m->w[0];
}
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static int fail(const char *me, long e)
{
    printf("%s: %s (%ld)\n", me, e == -ESRCH ? "no sound card" : "error", e);
    return 1;
}

static int play(const char *path)                /* -> 0 played, 1 failed, or the key that ended it (MK_...) */
{
    long f = fs_open(path, FS_RDONLY, 0), n = f < 0 ? f : fs_read(f, 0, buf, 4096);
    if (n < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { printf("play: %s: not a WAV file\n", path); return 1; }
    uint64_t off = 12, len = 0;
    int ok = 0;
    for (; off + 8 <= (uint64_t)n; off += 8 + (le32(buf + off + 4) + 1) / 2 * 2) {   /* the chunks */
        const uint8_t *c = buf + off;
        if (!memcmp(c, "fmt ", 4) && off + 24 <= (uint64_t)n)
            ok = (c[8] == 1 || c[8] == 0xFE) && c[10] == 2 && le32(c + 12) == RATE && c[22] == 16;
        if (!memcmp(c, "data", 4)) { len = le32(c + 4); off += 8; break; }
    }
    if (!ok || !len) { printf("play: %s: only 48 kHz, 16-bit, stereo PCM\n", path); return 1; }
    while (len) {
        n = fs_read(f, off, buf, len < sizeof buf ? len : sizeof buf);
        if (n <= 0) break;
        off += n; len -= n;
        for (long at = 0; at + 4 <= n; ) {       /* (the server takes what fits, the rest goes again) */
            msg_t m = { .w = { AUDIO_WRITE }, .sbuf = buf + at, .slen = (uint64_t)(n - at) & ~3ull };
            long r = call(&m);
            if (r < 0) return fail("play", r);
            if (m.w[1]) { fs_close(f); return (int)m.w[1]; }
            at += r;
        }
    }
    fs_close(f);
    return 0;
}

static int rec(const char *path, long secs)
{
    uint32_t len = (uint32_t)(secs > 0 && secs < 3600 ? secs : 5) * RATE * 4;
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0....\0\xee\x02\0\x04\0\x10\0data";
    put32(h + 4, 36 + len); put32(h + 24, RATE); put32(h + 40, len);   /* (bytes a second: 192000) */
    long f = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f < 0 || fs_write(f, 0, h, 44) != 44) { printf("rec: cannot write %s\n", path); return 1; }
    for (uint32_t got = 0; got < len; ) {
        msg_t m = { .w = { AUDIO_READ, len - got < sizeof buf ? len - got : sizeof buf }, .rbuf = buf, .rlen = sizeof buf };
        long r = call(&m);
        if (r < 0) return fail("rec", r);
        if (fs_write(f, 44 + got, buf, r) != r) { printf("rec: cannot write %s\n", path); return 1; }
        got += r;
    }
    fs_close(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *me = argc ? argv[0] : "play";
    if (!strcmp(me, "vol") && argc > 1 && !strcmp(argv[1], "out")) {   /* the outputs; one chosen */
        char *p = (char *)buf, *n;
        msg_t m = { .w = { AUDIO_OUT, argc > 2 ? (uint64_t)strnum(argv[2]) : AUDIO_ASK }, .rbuf = buf, .rlen = 256 };
        long v = call(&m);
        if (v < 0) return fail("vol", v);
        buf[m.rlen] = 0;
        for (int i = 0; (n = strchr(p, '\n')); i++, p = n + 1) { *n = 0; printf("%c %d %s\n", i == v ? '*' : ' ', i, p); }
        return 0;
    }
    if (!strcmp(me, "vol")) {
        msg_t m = { .w = { AUDIO_VOL, AUDIO_ASK } };
        long v = call(&m);
        if (argc > 1 && v >= 0) {
            m = (msg_t){ .w = { AUDIO_VOL, !strcmp(argv[1], "mute") ? (v & 0xFF) | AUDIO_MUTE : strnum(argv[1]) } };
            v = call(&m);
        }
        if (v < 0) return fail("vol", v);
        printf("volume %ld%%%s\n", v & 0xFF, v & AUDIO_MUTE ? ", muted" : "");
        return 0;
    }
    if (argc < 2) { printf("usage: play FILE.wav... | rec FILE.wav [SECONDS] | vol [0-100|mute|out [N]]\n"); return 1; }
    if (!strcmp(me, "rec")) return rec(argv[1], argc > 2 ? strnum(argv[2]) : 5);
    for (int i = 1, k; i < argc; i += k == MK_PREV ? -(i > 1) : 1)   /* (previous: the one before, or again) */
        if ((k = play(argv[i])) == 1 || k == MK_STOP) return k == 1;
    return 0;
}
