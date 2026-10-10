/*
 * racey - A test of servers started on demand (make demand-test).
 *
 *   racey serve NAME LINGER_MS   an on-demand server for port NAME: answers
 *                                each call with its pid; when init asks
 *                                "may you stop?" and it is unused, it says
 *                                yes, then lingers LINGER_MS before ending,
 *                                so calls arrive while it stops (the race)
 *   racey call NAME N MAX_MS     N calls to NAME, a random pause of up to
 *                                MAX_MS between them; every call must be
 *                                answered; prints how many servers answered
 *                                and how many calls waited for a new one
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static int serve(const char *name, long linger_ms)
{
    long port = port_create(name);
    if (port < 0) return 1;
    uint64_t asked = 0, seen = 0;
    int self = 0;
    { mk_ident_t id; if (!sys_ident(0, &id)) self = id.pid; }
    for (;;) {
        msg_t m = { 0 };
        long from = ipc_recv(port, &m);
        if (from <= 0) continue;
        if (m.w[0] == SVC_MAYSTOP) {
            long r = maystop_answer(&m, asked, &seen, 0);
            reply_val(from, r);
            if (!r) { sys_sleep(linger_ms * 1000000); return 0; }  /* calls queue meanwhile */
            continue;
        }
        asked++;
        reply_val(from, self);
    }
}

static int call(const char *name, long n, long max_ms)
{
    long port = 0, fails = 0, waited = 0, servers = 0, last = 0;
    for (long i = 0; i < n; i++) {
        uint32_t r;
        sys_random(&r, sizeof r);
        if (max_ms > 0) sys_sleep((uint64_t)(r % (uint32_t)max_ms) * 1000000);
        msg_t m = { .w = { 1 } };
        int64_t t = sys_clock();
        long e = call_named(&port, name, &m, 0);
        int64_t dt = sys_clock() - t;
        if (e < 0 || (long)m.w[0] <= 0) { fails++; printf("racey: call %ld failed (%ld)\n", i, e < 0 ? e : (long)m.w[0]); continue; }
        if ((long)m.w[0] != last) { servers++; last = (long)m.w[0]; }
        if (dt > 50000000) waited++;      /* over 50 ms: it waited for a server to start */
    }
    printf("racey: %ld calls, %ld failed, %ld servers answered, %ld calls waited for a new one\n", n, fails, servers, waited);
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "serve")) return serve(argv[2], strnum(argv[3]));
    if (argc == 5 && !strcmp(argv[1], "call")) return call(argv[2], strnum(argv[3]), strnum(argv[4]));
    printf("usage: racey serve NAME LINGER_MS | racey call NAME N MAX_MS\n");
    return 2;
}
