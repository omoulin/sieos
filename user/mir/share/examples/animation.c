/*
 * animation.c - MiR's example of a window application with its own loop:
 * for animations and games (the view loop's tick comes only 4 times a
 * second).  A ball bounces; Space pauses it, Escape quits.
 *
 *   cc -O2 -o animation animation.c -lfacet -lm
 */
#include <facet/facet.h>
#include <stdbool.h>
#include <time.h>

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

int main(void)
{
    fct_display *d = fct_open();
    if (!d)
        return 1;
    struct fct_window_attr a = { "Animation", FCT_POS_AUTO, FCT_POS_AUTO, 400, 300, 200, 150, 0 };
    fct_window *w = fct_window_create(d, &a);
    if (!w)
        return 1;
    float x = 50, y = 50, vx = 180, vy = 140;     /* pixels, pixels per second */
    bool paused = false, running = true;
    long last = now_ms();
    while (running) {
        struct fct_event ev;
        int r = fct_next_event(d, &ev, 16);       /* wait at most 16 ms: about 60 frames a second */
        while (r > 0) {                           /* that event and every other one waiting */
            if (ev.type == FCT_CLOSE)
                running = false;
            else if (ev.type == FCT_KEY && ev.key.value && ev.key.ascii == 27)
                running = false;
            else if (ev.type == FCT_KEY && ev.key.value && ev.key.ascii == ' ')
                paused = !paused;
            r = fct_next_event(d, &ev, 0);
        }
        if (r < 0)                                /* the desktop is gone */
            break;
        long t = now_ms();
        float dt = (t - last) / 1000.0f;
        last = t;
        struct surface *s = fct_window_surface(w);   /* (after a resize it is a new one: ask each frame) */
        int W = s->w, H = s->h;
        if (!paused) {
            x += vx * dt;
            y += vy * dt;
            if (x < 12 || x > W - 12)
                vx = -vx, x = x < 12 ? 12 : W - 12;
            if (y < 12 || y > H - 12)
                vy = -vy, y = y < 12 ? 12 : H - 12;
        }
        gfx_fill(s, 0, 0, W, H, C_CONTENT);
        gfx_ellipse_aa(s, x, y, 12, 12, C_ACCENT, C_BLUE);
        gfx_text(s, 8, 8, paused ? "Paused (Space)" : "Space pauses, Escape quits", C_DIM);
        fct_window_damage(w, rect_make(0, 0, W, H));   /* show the frame */
    }
    fct_window_destroy(w);
    fct_disconnect(d);
    return 0;
}
