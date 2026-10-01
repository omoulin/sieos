/*
 * vision.c - Does the model see images?  It is shown a picture of a
 * four-digit number and asked to read it.  And images in a conversation:
 * a tool (MiR's screenshot) attaches one to the model's next turn.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "internal.h"

/* The digits, 5x7 */
static const unsigned char digit_rows[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

#define SCALE 10
#define IMG_W (40 + 4 * 7 * SCALE)
#define IMG_H (40 + 7 * SCALE)

static void test_image(const char *number, struct sbuf *png)
{
    static uint32_t px[IMG_W * IMG_H];
    for (int i = 0; i < IMG_W * IMG_H; i++)
        px[i] = 0xFFFFFF;
    for (int d = 0; d < 4; d++) {
        const unsigned char *rows = digit_rows[number[d] - '0'];
        for (int y = 0; y < 7 * SCALE; y++)
            for (int x = 0; x < 5 * SCALE; x++)
                if (rows[y / SCALE] & (0x10 >> (x / SCALE)))
                    px[(20 + y) * IMG_W + 20 + d * 7 * SCALE + x] = 0x1A2A6C;
    }
    sia_png(px, IMG_W, IMG_H, IMG_W, png);
}

static void image_content(struct sbuf *b, const char *text, const void *png, size_t len)
{
    sb_puts(b, "[{\"type\":\"text\",\"text\":");
    sb_json_str(b, text);
    sb_puts(b, "},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,");
    sb_base64(b, png, len);
    sb_puts(b, "\"}}]");
}

int sia_vision_test(const struct sia_config *c, char *err, size_t errlen)
{
    struct model_cfg mc;
    memset(&mc, 0, sizeof(mc));
    snprintf(mc.endpoint, sizeof(mc.endpoint), "%s", c->endpoint);
    snprintf(mc.model, sizeof(mc.model), "%s", c->model);
    snprintf(mc.api_key, sizeof(mc.api_key), "%s", c->api_key);
    struct model m;
    bool ok = model_init(&m, &mc, err, errlen);
    memset(&mc, 0, sizeof(mc));
    if (!ok)
        return -1;
    char number[5];
    unsigned r = (unsigned)(uptime_ms() * 2654435761u ^ (unsigned)getpid());
    for (int i = 0; i < 4; i++, r /= 10)
        number[i] = (char)('1' + r % 9);                 /* (no zero: never a leading one to drop) */
    number[4] = 0;
    struct sbuf png, msgs;
    sb_init(&png);
    sb_init(&msgs);
    test_image(number, &png);
    sb_puts(&msgs, "[{\"role\":\"user\",\"content\":");
    image_content(&msgs, "What number is written in this image? Reply with the digits only.", png.s, png.len);
    sb_puts(&msgs, "}]");
    sb_free(&png);
    char e[400] = "";
    struct json *reply = model_chat(&m, msgs.s, NULL, e, sizeof(e));
    sb_free(&msgs);
    memset(&m.cfg, 0, sizeof(m.cfg));
    if (!reply) {
        /* a model without vision refuses the request (HTTP 400); anything else: no answer */
        bool refused = !strncmp(e, "HTTP 400", 8) || strstr(e, "image") || strstr(e, "vision");
        snprintf(err, errlen, "%s%s", refused ? "the model refused the image: " : "", e);
        return refused ? 0 : -1;
    }
    const char *text = json_get_str(reply, "content");
    char digits[16];
    int n = 0;
    for (const char *t = text ? text : ""; *t && n < 15; t++)
        if (*t >= '0' && *t <= '9')
            digits[n++] = *t;
    digits[n] = 0;
    int result = strstr(digits, number) != NULL;
    if (!result)
        snprintf(err, errlen, "shown %s, the model read \"%.60s\"", number, text ? text : "");
    json_free(reply);
    return result;
}

/* ---------------- images in a conversation ---------------- */

bool sia_session_vision(const struct sia_session *s) { return s->vision > 0; }

bool sia_attach_image(struct sia_session *s, const char *caption, const void *png, size_t len)
{
    if (s->vision <= 0)
        return false;
    sb_free(&s->img);
    sb_init(&s->img);
    sb_puts(&s->img, "{\"role\":\"user\",\"content\":");
    image_content(&s->img, caption, png, len);
    sb_puts(&s->img, "}");
    snprintf(s->img_caption, sizeof(s->img_caption), "%s", caption);
    return true;
}

void sia_notify(struct sia_session *s, const char *name, const char *value)
{
    if (s->io.event)
        s->io.event(s->io.ctx, name, value);
}
