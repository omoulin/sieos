/*
 * skin.c - The Facet desktop's skins: their colours, and which one is in
 * use (facet/skin.h).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "facet/skin.h"
#include "facet/settings.h"

static const struct fct_skin skins[FCT_NSKINS] = {
    {
        .id = FCT_SKIN_STRATA, .name = "strata", .title = "Strata",
        .blurb = "SIEOS's own: graphite surfaces, warm light text, an amber accent, the spine dock.",
        .light = false,
        .face = RGB(0x28, 0x2B, 0x30), .face_light = RGB(0x3A, 0x3E, 0x45), .face_shadow = RGB(0x8D, 0x88, 0x80),
        .face_dark = RGB(0x0E, 0x0F, 0x11),
        .text = RGB(0xE4, 0xE0, 0xD8), .dim = RGB(0x8D, 0x88, 0x80), .accent = RGB(0xD9, 0xA1, 0x5F),
        .blue = RGB(0x8F, 0xB4, 0xDC), .title_a1 = RGB(0x8F, 0xB4, 0xDC), .title_a2 = RGB(0x5F, 0x84, 0xAD),
        .desk_top = RGB(0x1D, 0x20, 0x24), .desk_bot = RGB(0x10, 0x11, 0x13),
        .content = RGB(0x1B, 0x1D, 0x21), .content_alt = RGB(0x21, 0x24, 0x28), .line = RGB(0x33, 0x37, 0x3D),
        .select = RGB(0x3A, 0x44, 0x52), .menu = RGB(0x26, 0x28, 0x2E), .menu_hot = RGB(0x3A, 0x44, 0x52),
        .titlebar = RGB(0x27, 0x2A, 0x2F), .titlebar_i = RGB(0x21, 0x23, 0x27),
        .title_text = RGB(0xE4, 0xE0, 0xD8), .title_text_i = RGB(0x8D, 0x88, 0x80),
        .spine = RGB(0x28, 0x2B, 0x30), .strip = RGB(0x0C, 0x0D, 0x0F),
        .good = RGB(0x7F, 0xD3, 0x9A), .bad = RGB(0xC9, 0x6A, 0x5A),
    },
    {
        .id = FCT_SKIN_BEOS, .name = "beos", .title = "BeOS style",
        .blurb = "Yellow title tabs, light bevelled panels, the Deskbar at the top right, a blue desktop.",
        .light = true,
        .face = RGB(0xD8, 0xD8, 0xD8), .face_light = RGB(0xFF, 0xFF, 0xFF), .face_shadow = RGB(0x98, 0x98, 0x98),
        .face_dark = RGB(0x60, 0x60, 0x60),
        .text = RGB(0x00, 0x00, 0x00), .dim = RGB(0x5E, 0x5E, 0x5E), .accent = RGB(0x33, 0x66, 0x99),
        .blue = RGB(0x33, 0x66, 0x99), .title_a1 = RGB(0x66, 0x99, 0xCC), .title_a2 = RGB(0x33, 0x66, 0x99),
        .desk_top = RGB(0x33, 0x66, 0x98), .desk_bot = RGB(0x33, 0x66, 0x98),
        .content = RGB(0xFF, 0xFF, 0xFF), .content_alt = RGB(0xEE, 0xEE, 0xEE), .line = RGB(0xB4, 0xB4, 0xB4),
        .select = RGB(0xC2, 0xD6, 0xF0), .menu = RGB(0xE8, 0xE8, 0xE8), .menu_hot = RGB(0xB8, 0xC8, 0xE0),
        .titlebar = RGB(0xFF, 0xCB, 0x00), .titlebar_i = RGB(0xE0, 0xE0, 0xE0),
        .title_text = RGB(0x00, 0x00, 0x00), .title_text_i = RGB(0x50, 0x50, 0x50),
        .spine = RGB(0xE8, 0xE8, 0xE8), .strip = RGB(0xF4, 0xF4, 0xF4),
        .good = RGB(0x2E, 0x9E, 0x3A), .bad = RGB(0xCC, 0x33, 0x22),
    },
    {
        .id = FCT_SKIN_IRIX, .name = "irix", .title = "IRIX style",
        .blurb = "Steel-blue bevelled frames, the Toolchest at the top left, an indigo desktop, a red pointer.",
        .light = true,
        .face = RGB(0xB3, 0xB8, 0xC1), .face_light = RGB(0xE6, 0xE9, 0xEE), .face_shadow = RGB(0x6C, 0x71, 0x7B),
        .face_dark = RGB(0x33, 0x37, 0x40),
        .text = RGB(0x00, 0x00, 0x00), .dim = RGB(0x44, 0x48, 0x52), .accent = RGB(0xA8, 0x2E, 0x2E),
        .blue = RGB(0x5C, 0x78, 0xA8), .title_a1 = RGB(0x8A, 0xA2, 0xC8), .title_a2 = RGB(0x5C, 0x78, 0xA8),
        .desk_top = RGB(0x5A, 0x63, 0x92), .desk_bot = RGB(0x33, 0x3A, 0x63),
        .content = RGB(0xCA, 0xCE, 0xD5), .content_alt = RGB(0xBF, 0xC4, 0xCC), .line = RGB(0x8C, 0x91, 0x9A),
        .select = RGB(0x9C, 0xB2, 0xD2), .menu = RGB(0xB3, 0xB8, 0xC1), .menu_hot = RGB(0x7B, 0x95, 0xBC),
        .titlebar = RGB(0x6E, 0x8B, 0xB0), .titlebar_i = RGB(0x9C, 0xA1, 0xA9),
        .title_text = RGB(0xFF, 0xFF, 0xFF), .title_text_i = RGB(0x2A, 0x2D, 0x33),
        .spine = RGB(0xB3, 0xB8, 0xC1), .strip = RGB(0xC6, 0xCA, 0xD2),
        .good = RGB(0x2F, 0x8A, 0x3C), .bad = RGB(0xB0, 0x2A, 0x22),
    },
};

const struct fct_skin *fct_skin = &skins[FCT_SKIN_BEOS];      /* the default */

const struct fct_skin *fct_skin_at(int i)
{
    return i >= 0 && i < FCT_NSKINS ? &skins[i] : NULL;
}

const struct fct_skin *fct_skin_named(const char *name)
{
    for (int i = 0; name && i < FCT_NSKINS; i++)
        if (!strcmp(skins[i].name, name))
            return &skins[i];
    return NULL;
}

void fct_skin_use(const struct fct_skin *s)
{
    if (s)
        fct_skin = s;
}

static const struct fct_skin *from_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    buf[n] = 0;
    buf[strcspn(buf, " \t\r\n")] = 0;
    return fct_skin_named(buf);
}

const struct fct_skin *fct_skin_load(void)
{
    const struct fct_skin *s = fct_skin_named(getenv("FACET_SKIN"));
    char v[32];
    if (!s && fct_setting_get("skin", v, sizeof(v)))
        s = fct_skin_named(v);
    const char *home = getenv("HOME");
    if (!s && home) {                            /* (milestone 34 kept it in ~/.facet/skin) */
        char path[256];
        snprintf(path, sizeof(path), "%s/.facet/skin", home);
        s = from_file(path);
    }
    fct_skin_use(s ? s : &skins[FCT_SKIN_BEOS]);
    return fct_skin;
}

const struct fct_skin *fct_skin_load_system(void)
{
    const struct fct_skin *s = fct_skin_named(getenv("FACET_SKIN"));
    char v[32];
    if (!s && fct_setting_get_system("skin", v, sizeof(v)))
        s = fct_skin_named(v);
    fct_skin_use(s ? s : &skins[FCT_SKIN_BEOS]);
    return fct_skin;
}

bool fct_skin_save(const struct fct_skin *s)
{
    return s && fct_setting_set("skin", s->name);
}
