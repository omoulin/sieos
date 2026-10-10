/*
 * siastub - TEST ONLY: a stand-in for the assistant server, so the
 * desktop's assistant panel can be tested without a model. It serves the
 * port "sia" (SIA_* in mk/proto.h) with canned answers sent one word at a
 * time, 30 ms apart. Started by hand (`siastub &`) by tools/guitest.py; if
 * the real sia already runs, it says so and ends.
 *
 * Answers: a question with "slowly" in it gets a long answer (to test
 * Stop); any other gets a short one with two actions, a file to open and a
 * project to go to.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#define NSESS 8
static const char *shortans = "Here is your note, and the demo project.\n[fly:note.txt]\n[project:demo]\n";
static struct { int used, stopped; const char *ans; int pos; } sess[NSESS];

static int has(const char *s, size_t n, const char *w)
{
    size_t k = strlen(w);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(s + i, w, k)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char longans[2400], q[SIA_MAX];
    for (size_t n = 0; n + 12 < sizeof longans; ) n += strlcpy(longans + n, "word after word ", sizeof longans - n);
    long port = port_create("sia");
    if (port < 0) { printf("siastub: sia is already running (%ld)\n", port); return 0; }
    printf("siastub: ready\n");
    for (;;) {
        msg_t m = { .rbuf = q, .rlen = sizeof q };
        long from = ipc_recv(port, &m);
        if (from <= 0) continue;
        long s = (long)m.w[1] - 1;
        int ok = s >= 0 && s < NSESS && sess[s].used;
        switch (m.w[0]) {
        case SIA_OPEN: {
            int i = 0;
            while (i < NSESS && sess[i].used) i++;
            if (i == NSESS) { reply_val(from, -EBUSY); break; }
            sess[i].used = 1;
            reply_val(from, i + 1);
            break;
        }
        case SIA_ASK:
            if (!ok) { reply_val(from, -ESRCH); break; }
            sess[s].ans = has(q, m.rlen, "slowly") ? longans : shortans;
            sess[s].pos = sess[s].stopped = 0;
            reply_val(from, 0);
            break;
        case SIA_NEXT: {
            if (!ok) { reply_val(from, -ESRCH); break; }
            const char *a = sess[s].ans + sess[s].pos;
            if (sess[s].stopped || !*a) { reply_val(from, 0); break; }
            sys_sleep(30000000);
            size_t n = 0;
            while (a[n] && a[n] != ' ' && a[n] != '\n') n++;
            if (a[n]) n++;                               /* the word and what follows it */
            sess[s].pos += n;
            msg_t r = { .w = { n }, .sbuf = a, .slen = n };
            ipc_reply(from, &r);
            break;
        }
        case SIA_STOP:  if (ok) sess[s].stopped = 1; reply_val(from, ok ? 0 : -ESRCH); break;
        case SIA_CLOSE: if (ok) sess[s].used = 0; reply_val(from, ok ? 0 : -ESRCH); break;
        case SIA_INFO: {
            sia_info_t in = { SIA_LOCAL, 1, 0, 2048, 0, 3300, "test stub" };
            for (int i = 0; i < NSESS; i++) in.sessions += sess[i].used;
            msg_t r = { .w = { 0 }, .sbuf = &in, .slen = sizeof in };
            ipc_reply(from, &r);
            break;
        }
        default: reply_val(from, -ENOSYS);
        }
    }
}
