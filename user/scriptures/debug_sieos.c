/*
 * debug_sieos.c - For make scriptures-test (tools/scripturestest.py): when the
 * data folder holds a file "debug", each frame shown is described on the
 * console's log (the serial port): the app's state, every clickable zone
 * the pointer can reach (found with zone_at, ui.h) and where the first words
 * of the chapter's verses are, in window pixels:
 *
 *   scriptures: frame N WxH tabs=.. cur=.. uri=.. pop=.. dlg=.. notes=.. ...
 *   scriptures: zone ID A x0 y0 x1 y1        (ID: app.h's Z_... number)
 *   scriptures: word ANCHOR K x y            (the middle of its word K, 0..5)
 *   scriptures: end
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sieos_log(const char *s);             /* plat_sieos.c: to the log */

void debug_frame(void)
{
    static int on = -1;
    static unsigned frame;
    if (on < 0) { char *p = xprintf("%s/debug", store_dir()); on = file_exists(p); free(p); }
    if (!on) return;
    char line[600];
    tab_t *t = cur_tab();
    int nn, live = 0;
    notes_lock();
    note_t **all = notes_all(&nn);
    for (int i = 0; i < nn; i++) live += !all[i]->deleted;
    notes_unlock();
    snprintf(line, sizeof line, "scriptures: frame %u %dx%d tabs=%d cur=%d uri=%s scroll=%d pop=%d dlg=%d notes=%d sidebar=%d pages=%d vols=%d dl=%d focus=%s\n",
             ++frame, A.W, A.H, A.ntabs, A.cur, t && t->uri ? t->uri : "-", t ? t->scroll : 0, A.pop, A.dlg, live, A.sidebar,
             A.npages, A.vols ? A.vols->n : 0, A.dl.running, A.focus ? field_str(A.focus) : "-");
    sieos_log(line);
    const zone_t *seen[256];
    int ns = 0;
    for (int y = 1; y < A.H; y += 3)
        for (int x = 1; x < A.W; x += 3) {
            const zone_t *z = zone_at(x, y);
            if (!z || z->id == Z_NONE) continue;
            int k = 0;
            while (k < ns && seen[k] != z) k++;
            if (k < ns || ns == 256) continue;
            seen[ns++] = z;
            snprintf(line, sizeof line, "scriptures: zone %d %d %d %d %d %d\n", z->id, z->a, z->x0, z->y0, z->x1, z->y1);
            sieos_log(line);
        }
    for (int bi = 0, n = 0; t && t->lay && bi < t->lay->doc->n && n < 8; bi++) {
        const block_t *b = &t->lay->doc->b[bi];
        if (!b->anchor) continue;
        for (int w = 0; w < t->lay->bi[bi].nw && w < 6; w++) {   /* its first words */
            int x, y, h, x2, y2, h2;
            layout_pos(t->lay, t->lay->bi[bi].ws[w], &x, &y, &h);
            layout_pos(t->lay, t->lay->bi[bi].we[w], &x2, &y2, &h2);
            snprintf(line, sizeof line, "scriptures: word %s %d %d %d\n", b->anchor, w, A.text_x0 + (y2 == y ? (x + x2) / 2 : x + U(4)),
                     A.read_y0 + U(30) + y + h / 2 - t->scroll);
            sieos_log(line);
        }
        n++;
    }
    sieos_log("scriptures: end\n");
}
