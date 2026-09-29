/*
 * fbset - show and set the displays' modes (/dev/fb0, /dev/fb1, ...)
 *   fbset                  the displays: driver, device, mode
 *   fbset -l [-d N]        the modes of display N (default 0)
 *   fbset [-d N] WxH       change display N to WxH (a mode it lists)
 * The console follows a mode change of the display it draws on.  A display
 * a program has mapped (Facet) can only be changed by that program.
 */
#include "sieos.h"
#include "sieos/sysinfo.h"

static int open_fb(int n, int mode)
{
    char path[16];
    snprintf(path, sizeof(path), "/dev/fb%d", n);
    return open(path, mode);
}

static int list_displays(void)
{
    int found = 0;
    for (int n = 0; n < 8; n++) {
        int fd = open_fb(n, O_RDONLY);
        if (fd < 0)
            continue;
        struct sieos_fb_display di;
        struct sieos_fb_info fi;
        if (ioctl(fd, SIEOS_FBIOGET_DISPLAY, &di) == 0 && ioctl(fd, SIEOS_FBIOGET_INFO, &fi) == 0) {
            printf("fb%d: %ux%u  %s (%s)  %lu KiB%s%s%s\n", n, fi.width, fi.height, di.driver, di.desc, di.vram / 1024,
                   di.flags & SIEOS_FB_SETMODE ? ", modes can be set" : ", fixed mode",
                   di.flags & SIEOS_FB_CONSOLE ? ", console" : "", di.owner ? ", in use" : "");
            found++;
        }
        close(fd);
    }
    if (!found)
        printf("no displays\n");
    return found ? 0 : 1;
}

static int list_modes(int n)
{
    int fd = open_fb(n, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "fbset: /dev/fb%d: %s\n", n, strerror(errno));
        return 1;
    }
    struct sieos_fb_modes m;
    if (ioctl(fd, SIEOS_FBIOGET_MODES, &m) < 0) {
        perror("fbset");
        return 1;
    }
    for (unsigned i = 0; i < m.n; i++) {
        char hz[16] = "";
        if (m.mode[i].refresh)
            snprintf(hz, sizeof(hz), " @ %u Hz", m.mode[i].refresh);
        printf("%c %ux%u%s%s\n", m.mode[i].flags & SIEOS_FB_MODE_CURRENT ? '*' : ' ', m.mode[i].width,
               m.mode[i].height, hz, m.mode[i].flags & SIEOS_FB_MODE_PREFERRED ? "  (preferred)" : "");
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int n = 0, i = 1;
    bool modes = false;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-l"))
            modes = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            n = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: fbset [-l] [-d N] [WxH]\n");
            return 2;
        }
    }
    if (modes)
        return list_modes(n);
    if (i == argc)
        return list_displays();
    unsigned w, h;
    if (sscanf(argv[i], "%ux%u", &w, &h) != 2) {
        fprintf(stderr, "fbset: %s: not WIDTHxHEIGHT\n", argv[i]);
        return 2;
    }
    int fd = open_fb(n, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "fbset: /dev/fb%d: %s\n", n, strerror(errno));
        return 1;
    }
    struct sieos_fb_mode m = { (unsigned short)w, (unsigned short)h, 0, 0 };
    if (ioctl(fd, SIEOS_FBIOSET_MODE, &m) < 0) {
        fprintf(stderr, "fbset: %ux%u on fb%d: %s\n", w, h, n,
                errno == EINVAL ? "not a mode of this display (fbset -l)"
                : errno == ENOTSUP ? "this display's mode is fixed"
                : errno == EBUSY ? "another program has the display" : strerror(errno));
        return 1;
    }
    return 0;
}
