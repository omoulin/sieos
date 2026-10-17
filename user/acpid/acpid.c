/*
 * acpid - The ACPI server (x86-64, docs/power.md): what the firmware's
 * tables say about power, and the events of the machine's ACPI hardware.
 *
 *  - Sleep types: \_S5 and \_S3 (AML packages) go to the kernel (SYS_POWER,
 *    POWER_SET), which powers off (S5) or suspends (S3) by writing them to
 *    the PM1 control registers. Before either, the firmware is told (_PTS),
 *    and after waking (_WAK). `poweroff`, `reboot`, `suspend` and the
 *    desktop come here (ACPI_POWER, through power() in the library).
 *  - Events come through one interrupt, the SCI (the FADT's line): the fixed
 *    power button (PM1 status), and general-purpose events (GPE0 status
 *    bits: the methods \_GPE._Lxx and _Exx). One of those is the embedded
 *    controller's: when it has news (SCI_EVT) we ask which (query, 0x84) and
 *    run its method _Qxx. The methods say what changed with Notify(): the
 *    battery, the charger, the lid, the power button.
 *  - The embedded controller's space (fields of an EmbeddedControl region)
 *    is read and written through its two ports (its _CRS: data 0x62,
 *    command 0x66), commands 0x80 and 0x81; its _REG tells the AML so.
 *  - The battery: _STA (present), _BIX or _BIF (capacities), _BST (state,
 *    rate, remaining, voltage); the charger: _PSR; the lid: _LID. Read at
 *    start, after each Notify and every 30 s; clients (the Deskbar,
 *    `battery`) ask the port "acpi": ACPI_STATUS, or ACPI_WAIT for the
 *    next change.
 *  - The power button powers off, as `poweroff` does (files committed
 *    first): the fixed one, the PNP0C0C device's, or Intel's HID event
 *    filter's (INT33D5: Notify 0xCE) and virtual buttons' (INT33D6: 0xC0),
 *    which "Modern Standby" laptops use; their other events (a 2-in-1's
 *    tablet and laptop modes) are logged. The lid only reports: the
 *    desktop decides (screen off, sleep).
 *  - Machines without \_S3 ("Modern Standby" only: the FADT's LOW_POWER_S0
 *    flag; Ice Lake laptops often) sleep in s2idle, which SIEOS does not do
 *    yet: `suspend` then says so at once, before anything is touched.
 *  - Waking from S3: the power button, the GPEs of the lid, power button
 *    and controller that can wake (their _PRW; armed by _DSW or _PSW), and
 *    for `suspend N` the CMOS clock's alarm (PM1's RTC_EN). Nothing else.
 * Every step is logged (the serial console, the log viewer): a laptop's log
 * says what was found and what happened.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "aml.h"
#include "pci.h"

enum { TICK = 0x7e10 };                         /* our timer thread's message */
static long port;
static int self;
static const uint8_t *fadt;
static uint16_t pm1a, pm1b, pm1len, gpe0, gpelen, ec_data, ec_sc;   /* ports; gpelen: status bytes */
static uint8_t gpe_en[16], gpe_wk[16];          /* the GPEs we enabled; those that wake us */
static uint16_t hits[128];                      /* each one's events since the last tick */
static int ec = -1, ec_gpe = -1, bat = -1, ac = -1, lid = -1, pwrb = -1, sci = -1, ihid = -1, vbtn = -1, sbtn_dev = -1;
static int s3, bif = 1, button, dirty;          /* ACPI_S3 if it can sleep; read the battery's capacities again; the power button
                                                   was pressed; read all again */
static int wdev[3], nwdev;                      /* the devices that wake us (their _PRW) */
static uint32_t wakes;
static long awake;                              /* (when we last woke: sleep asked before then is not done) */
static uint32_t unit_mwh = 1, design, full;
static acpi_status_t st;
static long waiters[8];
static int nwait;

static struct { char b[199]; size_t n; } line;  /* (what SYS_DEBUG takes) */
static void line_put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    memcpy(line.b, "acpid: ", 7); line.n = 7;
    vformat(line_put, 0, fmt, ap);
    sys_debug(line.b, line.n);
    va_end(ap);
}

/* ---- The embedded controller: status (OBF: a byte for us, IBF: it has not
 * taken ours yet, SCI_EVT: news) and data ports. */
enum { OBF = 1, IBF = 2, SCI_EVT = 0x20 };
static int ec_wait(int mask, int want)
{
    for (long t = sys_clock() + 50000000; (inb(ec_sc) & mask) != want; )
        if (sys_clock() > t) { log_line("embedded controller: no answer (status %x)\n", inb(ec_sc)); return -1; }
    return 0;
}
static int ec_out(uint16_t p, int v) { if (ec_wait(IBF, 0)) return -1; outb(p, (uint8_t)v); return 0; }
static int ec_in(void) { return ec_wait(OBF, OBF) ? -1 : inb(ec_data); }
static int ec_rw(int a, int v, int write)       /* a byte of its space -> it, or -1 */
{
    if (ec_out(ec_sc, write ? 0x81 : 0x80) || ec_out(ec_data, a)) return -1;
    return write ? ec_out(ec_data, v) : ec_in();
}

/* ---- The AML's regions: memory (pages mapped when first used), I/O ports,
 * PCI configuration (SYS_PCI), the embedded controller. */
int aml_io(int space, uint64_t a, int w, uint64_t *v, int write)
{
    static struct { uint64_t pa; volatile uint8_t *va; } m[64];
    static int n;
    if (space == 0) {
        int i = 0;
        while (i < n && m[i].pa != (a & ~4095ULL)) i++;
        if (i == n) {
            volatile uint8_t *va = map_phys(a & ~4095ULL, 4096);
            if ((long)va < 0) { log_line("memory at %lx: no access\n", (long)a); return -1; }
            i = n < 64 ? n++ : (int)(a >> 12 & 63);  /* (full: one is replaced) */
            m[i].pa = a & ~4095ULL; m[i].va = va;
        }
        volatile uint8_t *p = m[i].va + (a & 4095);
        if (write) { if (w == 1) *p = (uint8_t)*v; else if (w == 2) *(volatile uint16_t *)p = (uint16_t)*v;
                     else if (w == 4) *(volatile uint32_t *)p = (uint32_t)*v; else *(volatile uint64_t *)p = *v; }
        else *v = w == 1 ? *p : w == 2 ? *(volatile uint16_t *)p : w == 4 ? *(volatile uint32_t *)p : *(volatile uint64_t *)p;
        return 0;
    }
    if (space == 1) {
        uint16_t p = (uint16_t)a;
        if (w == 8) { uint64_t lo = *v, hi = *v >> 32; if (aml_io(1, a, 4, &lo, write) || aml_io(1, a + 4, 4, &hi, write)) return -1; *v = lo | hi << 32; return 0; }
        if (write) { if (w == 1) outb(p, (uint8_t)*v); else if (w == 2) outw(p, (uint16_t)*v); else outl(p, (uint32_t)*v); }
        else *v = w == 1 ? inb(p) : w == 2 ? inw(p) : inl(p);
        return 0;
    }
    if (space == 2 && w <= 4 && (a & 3) + w <= 4) {     /* (dword accesses: the others in it written back) */
        uint32_t d = pci_read((uint32_t)a & ~3u), sh = (a & 3) * 8, mk = w == 4 ? ~0u : ((1u << 8 * w) - 1) << sh;
        if (write) pci_write((uint32_t)a & ~3u, (d & ~mk) | ((uint32_t)*v << sh & mk));
        else *v = (d & mk) >> sh;
        return 0;
    }
    if (space == 3 && ec >= 0 && w == 1) {
        int r = ec_rw((int)a, (int)*v, write);
        if (!write) *v = (uint64_t)r;
        return r < 0;
    }
    return -1;
}

/* A Notify() from the AML: what changed. */
static void notify(int dev, int64_t v)
{
    if (dev < 0) return;
    if (dev == bat) { if (v == 0x81) bif = 1; }
    else if ((dev == pwrb && v == 0x80) || (dev == ihid && v == 0xCE) || (dev == vbtn && v == 0xC0) || (dev == sbtn_dev && v == 0xC6)) button = 1;
    else if (dev == ihid || dev == vbtn) {
        if (v != 0xCF && v != 0xC1) log_line("Intel %s: event %x%s\n", dev == ihid ? "HID" : "buttons", (int)v, v == 0xCC ? ": tablet mode" : v == 0xCD ? ": laptop mode" : "");
        return;
    } else if (dev != ac && dev != lid) return;
    dirty = 1;
}

/* ---- The status: battery, charger, lid. Changes wake the waiters. */
static uint32_t known(int64_t x) { return x < 0 || x >= 0x7FFFFFFF ? 0 : (uint32_t)x; }    /* (0xFFFFFFFF: unknown) */
static void refresh(void)
{
    acpi_status_t s = st;
    int64_t v[20];
    int n;
    dirty = 0;
    s.flags = s3;
    if (ac >= 0 && aml_eval(ac, "_PSR", 0, 0, v, 1) == 1 && v[0]) s.flags |= ACPI_AC;
    if (lid >= 0) s.flags |= ACPI_LID | (aml_eval(lid, "_LID", 0, 0, v, 1) == 1 && !v[0] ? ACPI_LID_CLOSED : 0);
    if (bat >= 0 && (aml_eval(bat, "_STA", 0, 0, v, 1) != 1 || v[0] & 0x10)) {
        if (bif && (n = aml_eval(bat, "_BIX", 0, 0, v, 4)) == 4) unit_mwh = !v[1], design = known(v[2]), full = known(v[3]), bif = 0;
        else if (bif && (n = aml_eval(bat, "_BIF", 0, 0, v, 3)) == 3) unit_mwh = !v[0], design = known(v[1]), full = known(v[2]), bif = 0;
        if (aml_eval(bat, "_BST", 0, 0, v, 4) == 4) {
            s.flags |= ACPI_BAT | (v[0] & 2 ? ACPI_CHARGING : 0) | (unit_mwh ? ACPI_MWH : 0);
            s.rate = (int32_t)known(v[1]); s.now = known(v[2]); s.mv = known(v[3]);
            s.full = full ? full : design; s.design = design;
            s.percent = s.full ? (int32_t)(s.now >= s.full ? 100 : (uint64_t)s.now * 100 / s.full) : -1;
            s.minutes = !s.rate ? -1 : v[0] & 2 ? (int32_t)((uint64_t)(s.full > s.now ? s.full - s.now : 0) * 60 / s.rate)
                                 : v[0] & 1 ? (int32_t)((uint64_t)s.now * 60 / s.rate) : -1;
        }
    }
    if (!(s.flags & ACPI_BAT)) s.percent = s.minutes = -1, s.rate = 0, s.now = 0;
    s.wakes = wakes;
    int big = s.flags != st.flags || s.percent != st.percent || s.wakes != st.wakes;
    if (!big && s.minutes == st.minutes && s.rate == st.rate && s.now == st.now && s.mv == st.mv) return;
    if (big && s.flags & ACPI_BAT)
        log_line("battery %d%%%s: %u of %u m%sh (design %u), %d m%s, %u mV, %d min\n", s.percent, s.flags & ACPI_CHARGING ? ", charging" : "",
                 s.now, s.full, unit_mwh ? "W" : "A", s.design, s.rate, unit_mwh ? "W" : "A", s.mv, s.minutes);
    if (big) log_line("AC %s, battery %s, lid %s\n", ac < 0 ? "none" : s.flags & ACPI_AC ? "on" : "off", s.flags & ACPI_BAT ? "in" : "none",
                      !(s.flags & ACPI_LID) ? "none" : s.flags & ACPI_LID_CLOSED ? "closed" : "open");
    s.seq++;
    st = s;
    for (int i = 0; i < nwait; i++) { msg_t r = { .w = { 0 }, .sbuf = &st, .slen = sizeof st }; ipc_reply(waiters[i], &r); }
    nwait = 0;
}

/* ---- Events: the SCI's sources. */
static uint16_t f16(int off) { return off + 2 <= (int)*(uint32_t *)(fadt + 4) ? *(uint16_t *)(fadt + off) : 0; }
static uint32_t f32(int off) { return off + 4 <= (int)*(uint32_t *)(fadt + 4) ? *(uint32_t *)(fadt + off) : 0; }
static uint16_t blk(int off, int xoff) { return f32(off) ? (uint16_t)f32(off) : fadt[xoff] == 1 ? (uint16_t)f32(xoff + 4) : 0; }
static void gpe_set(void)                       /* our enables (0: all off, for sleeping) */
{
    for (int i = 0; i < gpelen; i++) outb(gpe0 + gpelen + i, gpe_en[i]);
}
static void ec_events(void)                     /* the controller's news: its queries' methods */
{
    for (int k = 0; k < 64 && inb(ec_sc) & SCI_EVT; k++) {
        int q;
        if (ec_out(ec_sc, 0x84) || (q = ec_in()) <= 0) return;
        char m[5] = { '_', 'Q', "0123456789ABCDEF"[q >> 4], "0123456789ABCDEF"[q & 15], 0 };
        int64_t r;
        int has = aml_find(ec, m) >= 0;
        log_line("embedded controller: query %02x%s\n", q, has ? "" : " (no method)");
        if (has && aml_eval(ec, m, 0, 0, &r, 1) < 0) log_line("%s failed\n", m);
    }
}
static void events(void)
{
    uint16_t s = pm1a ? inw(pm1a) : 0;
    if (pm1b) s |= inw(pm1b);
    if (s & 0x100) {                            /* the fixed power button */
        if (pm1a) outw(pm1a, 0x100);
        if (pm1b) outw(pm1b, 0x100);
        log_line("power button\n");
        button = 1;
    }
    for (int i = 0; i < gpelen; i++) {
        uint8_t on = inb(gpe0 + i) & gpe_en[i];
        for (int b = 0; on; b++, on >>= 1) {
            if (!(on & 1)) continue;
            int g = i * 8 + b;
            char m[12] = "\\_GPE._L00";
            m[8] = "0123456789ABCDEF"[g >> 4]; m[9] = "0123456789ABCDEF"[g & 15];
            int k = aml_find(0, m), edge = k < 0;
            if (edge) m[7] = 'E', k = aml_find(0, m);
            if (edge || g == ec_gpe) outb(gpe0 + i, (uint8_t)(1 << b));   /* edge: cleared before, level: after */
            int64_t r;
            if (g == ec_gpe) ec_events();
            int bad = k >= 0 && aml_eval(k, 0, 0, 0, &r, 1) < 0;
            if (bad || ++hits[g] > 3000) {                /* (a storm: 100 a second for 30 s) */
                log_line("GPE %02x: %s %s: GPE disabled\n", g, m, bad ? "failed" : "keeps coming");
                gpe_en[i] &= (uint8_t)~(1 << b);
                gpe_set();
            }
            if (!edge && g != ec_gpe) outb(gpe0 + i, (uint8_t)(1 << b));
        }
    }
}

/* ---- The fixed hardware armed (at start, after waking): ACPI mode (the
 * firmware asked through its SMI port if not on), status bits cleared, the
 * fixed power button (unless it is a device) and our GPEs enabled, the
 * controller's pending queries run. */
static uint16_t cnt, smi;
static void arm(void)
{
    if (cnt && !(inw(cnt) & 1) && smi && fadt[52]) {
        outb(smi, fadt[52]);
        for (long e = sys_clock() + 1000000000; !(inw(cnt) & 1) && sys_clock() < e; ) sys_sleep(1000000);
        log_line("ACPI mode %s\n", inw(cnt) & 1 ? "on" : "did not come");
    }
    for (int k = 0; k < 2; k++) {
        uint16_t e = k ? pm1b : pm1a;
        if (!e) continue;
        outw(e, 0x8500);                            /* WAK_STS, RTC_STS, PWRBTN_STS */
        outw(e + pm1len, f32(112) & 1 << 4 ? 0 : 0x100);
    }
    for (int i = 0; i < gpelen; i++) outb(gpe0 + i, 0xFF);
    gpe_set();
    if (ec >= 0) ec_events();                       /* (news it had before: no new GPE edge for them) */
}

/* ---- PCI: what the sleep loses. Each function's header (BARs, command,
 * bridges' buses and windows, interrupt line) and its PCI Express control
 * registers are kept, then written back where they changed on waking (the
 * bridges first, as found; the command last), before the drivers start
 * again; the host and ISA bridges' whole configuration (the chipset's own:
 * QEMU's ICH9 loses its ACPI ports; a real firmware restores them itself). */
#define NPCI 64
static struct { uint32_t at, cap, h[64], x[4]; } pcs[NPCI];
static int npcs;
static const uint8_t pcie[4] = { 8, 0x10, 0x28, 0x30 };   /* PCI Express: device, link control (and their 2s) */
static void pci_keep(void)
{
    pci_t p;
    for (npcs = 0; npcs < NPCI && !pci_scan(&p, 0, 0, 0, npcs); npcs++) {
        pcs[npcs].at = p.at;
        pcs[npcs].cap = 0;
        for (int r = 0; r < 64; r++) pcs[npcs].h[r] = pci_cfg(&p, 4 * r);
        for (int c = pcs[npcs].h[13] & 0xFC, k = 0; c && k < 48; c = pci_cfg(&p, c) >> 8 & 0xFC, k++)
            if ((pci_cfg(&p, c) & 0xFF) == 0x10) { pcs[npcs].cap = (uint32_t)c; break; }
        for (int r = 0; r < 4 && pcs[npcs].cap; r++) pcs[npcs].x[r] = pci_cfg(&p, pcs[npcs].cap + pcie[r]);
    }
    log_line("PCI: %d functions kept, %d interrupt links\n", npcs, aml_links(0));
}
static void pci_back(void)
{
    int n = 0;
    aml_links(1);
    for (int i = 0; i < npcs; i++) {
        pci_t p = { .at = pcs[i].at };
        if (pci_cfg(&p, 0) != pcs[i].h[0]) continue;     /* (gone, or behind a bridge not back yet) */
        for (int r = 0; r < 4 && pcs[i].cap; r++) pci_cfgw(&p, pcs[i].cap + pcie[r], pcs[i].x[r] & 0xFFFF);
        for (int r = (pcs[i].h[2] >> 24 == 6 && (pcs[i].h[2] >> 16 & 0xFF) <= 1) ? 63 : 15; r >= 1; r--)   /* (host, ISA bridges: all) */
            if (pci_cfg(&p, 4 * r) != pcs[i].h[r]) pci_cfgw(&p, 4 * r, r == 1 ? pcs[i].h[1] & 0xFFFF : pcs[i].h[r]), n++;
    }
    log_line("PCI: %d registers written back\n", n);
}

/* ---- Power: the firmware told before (_PTS) and after (_WAK). Waking up,
 * init starts the device drivers anew (the devices lost their state).
 * Asleep, only the wake GPEs are on (status cleared), the power button,
 * and with secs the CMOS clock's alarm (< 1 hour: minute and second, any
 * hour), RTC_EN; awake, the alarm off (arm(): the PM1 enables). */
static int cmos(int r, int v) { outb(0x70, (uint8_t)r); if (v >= 0) outb(0x71, (uint8_t)v); return inb(0x71); }
static void alarm(int secs)
{
    int b = cmos(11, -1), bin = b & 4;
    if (secs) {
        while (cmos(10, -1) & 0x80) ;               /* (no update under way: 244 us before one) */
        int s = cmos(0, -1), m = cmos(2, -1);
        if (!bin) s = (s >> 4) * 10 + (s & 15), m = (m >> 4) * 10 + (m & 15);
        int t = (m * 60 + s + secs) % 3600;
        s = t % 60; m = t / 60;
        cmos(1, bin ? s : s / 10 << 4 | s % 10); cmos(3, bin ? m : m / 10 << 4 | m % 10); cmos(5, 0xFF);
        for (int k = 0; k < 2; k++) { uint16_t e = k ? pm1b : pm1a; if (e) outw(e + pm1len, inw(e + pm1len) | 0x400); }
    }
    cmos(11, secs ? b | 0x20 : b & ~0x20);          /* AIE */
    cmos(12, -1);                                   /* (its flags cleared) */
}
static void tell(const char *m, int s)
{
    int64_t a = s, r;
    if (aml_find(0, m) >= 0) log_line("%s(%d)%s\n", m + 1, s, aml_eval(0, m, 1, &a, &r, 1) < 0 ? ": failed" : "");
}
static long power_do(int how)
{
    static const char *what[] = { "power off", "restart", "suspend" };
    int secs = how >> 8;
    how &= 255;
    if (how == POWER_SLEEP && sys_clock() < awake) { log_line("suspend: just woke: not again\n"); return 0; }
    if (how == POWER_SLEEP && !s3) { log_line("suspend: this machine has no S3 (only s2idle, not done yet)\n"); return -ENOSYS; }
    log_line("%s\n", what[how]);
    if (how == POWER_RESTART) return sys_power(how, 0);
    for (int i = 0; how == POWER_SLEEP && i < nwdev; i++) aml_wake(wdev[i], 3, 1);
    tell("\\_PTS", how == POWER_SLEEP ? 3 : 5);
    if (how == POWER_OFF) return sys_power(how, 0);
    pci_keep();
    for (int i = 0; i < gpelen; i++) outb(gpe0 + gpelen + i, gpe_wk[i]), outb(gpe0 + i, 0xFF);
    if (secs) alarm(secs);
    long r = sys_power(POWER_SLEEP, 0);
    int s = (pm1a ? inw(pm1a) : 0) | (pm1b ? inw(pm1b) : 0), g = -1;
    for (int i = 0; i < 8 * gpelen; i++) if (inb(gpe0 + i / 8) & gpe_wk[i / 8] & 1 << i % 8) g = i;
    if (r) log_line("suspend failed (%ld)\n", r);
    else log_line(g < 0 ? "awake (PM1 status %x)\n" : "awake (PM1 status %x, GPE %02x)\n", s, g);
    if (!r) pci_back();
    arm();
    tell("\\_WAK", 3);
    for (int i = 0; i < nwdev; i++) aml_wake(wdev[i], 3, 0);
    alarm(0);
    wakes += !r;
    bif = dirty = 1;
    msg_t m = { .w = { INIT_RESUME } };
    long ip = port_find("init");
    if (!r && ip > 0) ipc_call(ip, &m);
    awake = sys_clock() + 1000000000;
    return r;
}

static void ticker(void *arg)
{
    (void)arg;
    for (;;) { sys_sleep(30000000000L); msg_t m = { .w = { TICK } }; ipc_call(port, &m); }
}

/* ---- A Surface's buttons (MSHW0040, Surface Pro 5 to 7, Book 2): power,
 * volume up, volume down on three GPIO pads (its _CRS's GpioInts, in that
 * order, active low; Linux: soc_button_array). Their level (DW0's RX state,
 * before any inversion) is read every 50 ms: the power button powers off,
 * the volume buttons go to the audio server (held: again every 150 ms
 * after 450 ms). */
static volatile uint32_t *sbtn[3];
static void surface_buttons(void *arg)
{
    (void)arg;
    long audio = 0;
    int was = 0, held = 0;
    for (;;) {
        sys_sleep(50000000);
        int now = 0;
        for (int i = 0; i < 3; i++) if (sbtn[i] && !(*sbtn[i] & 2)) now |= 1 << i;
        if (now & ~was & 1) { msg_t m = { .w = { TICK, 1 } }; ipc_call(port, &m); }
        held = now & 6 && (now & 6) == (was & 6) ? held + 1 : 0;
        if ((now & 6 & ~was) || (held >= 9 && held % 3 == 0)) {
            log_line("volume %s\n", now & 2 ? "up" : "down");
            msg_t m = { .w = { AUDIO_KEY, now & 2 ? MK_VOLUP : MK_VOLDOWN } };
            call_named(&audio, "audio", &m, 1);
        }
        was = now;
    }
}
static void surface_buttons_start(int dev)
{
    aml_hid_t r;
    aml_gpio_t g;
    static const char *what[3] = { "power", "volume up", "volume down" };
    if (aml_res(dev, "_CRS", &r) || r.npins < 1) { log_line("Surface buttons: no GPIO pins (notifications only)\n"); return; }
    for (int i = 0; i < 3 && i < r.npins; i++) {
        volatile uint8_t *cm;
        if (aml_gpio(r.pins[i], &g) || (long)(cm = map_phys(g.base, 8192)) < 0) { log_line("Surface buttons: %s: pin %d unknown\n", what[i], r.pins[i]); continue; }
        sbtn[i] = (volatile uint32_t *)(cm + g.pad);
        log_line("Surface buttons: %s: pin %d, group %d pad %d, DW0 %08x (%s)\n", what[i], r.pins[i], g.group, g.bit, *sbtn[i], *sbtn[i] & 2 ? "up" : "pressed");
    }
}

static int dev(const char *hid, const char *what)    /* the first device present with this id, or -1 */
{
    int k;
    char p[48];
    if (aml_devs(hid, &k, 1) < 1) { log_line("%s: none (%s)\n", what, hid); return -1; }
    aml_path(k, p, sizeof p);
    log_line("%s: %s\n", what, p);
    return k;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    mk_info_t in;
    sys_info(&in);
    self = in.self_pid;
    long n = sys_acpi(0, 0);
    uint8_t *t = n > 0 ? malloc(n) : 0;
    if (!t || sys_acpi(t, n) != n || memcmp(t, "FACP", 4)) { log_line("no ACPI tables\n"); return 0; }
    fadt = t;
    log_line("ACPI tables: %ld bytes, %ld named objects\n", n, aml_load(t, n));

    /* Sleep types, for the kernel. */
    for (int s = 3; s <= 5; s += 2) {
        char p[5] = { '\\', '_', 'S', (char)('0' + s), 0 };
        int64_t v[2] = { 0, 0 };
        int k = aml_eval(0, p, 0, 0, v, 2);
        if (k < 1) { log_line("%s: none: no S%d\n", p, s); continue; }
        log_line("%s: sleep type %ld/%ld\n", p, (long)v[0], (long)v[1]);
        if (!sys_power(POWER_SET, s | (v[0] & 7) << 8 | (k > 1 ? v[1] & 7 : 0) << 16) && s == 3) s3 = ACPI_S3;
    }

    /* The fixed hardware. */
    sci = f16(46);
    pm1a = blk(56, 148); pm1b = blk(60, 160); pm1len = fadt[88] / 2;
    gpe0 = blk(80, 220); gpelen = gpe0 ? fadt[92] / 2 : 0;
    if (gpelen > 16) gpelen = 16;
    cnt = blk(64, 172); smi = (uint16_t)f32(48);
    log_line("FADT: SCI %d, PM1 events %x/%x, control %x, GPE0 %x (%d), SMI %x, flags %x\n", sci, pm1a, pm1b, cnt, gpe0, 8 * gpelen, smi, f32(112));
    if (f32(112) & 1 << 21) log_line("FADT: low-power S0 idle (Modern Standby)%s\n", s3 ? "; S3 too: suspend uses S3" : ", no S3: suspend is not available (s2idle: not done yet)");
    arm();                                          /* (no GPE yet) */

    /* The devices. */
    aml_notify = notify;
    ec = dev("PNP0C09", "embedded controller");
    if (ec >= 0) {
        aml_hid_t r;
        int64_t g, a[2] = { 3, 1 }, x;
        if (!aml_res(ec, "_CRS", &r) && r.io[0] > 0 && r.io[1] > 0) ec_data = (uint16_t)r.io[0], ec_sc = (uint16_t)r.io[1];
        if (!ec_data) { log_line("embedded controller: no ports\n"); ec = -1; }
        else {
            if (aml_eval(ec, "_GPE", 0, 0, &g, 1) == 1 && g < 8 * gpelen) ec_gpe = (int)g, gpe_en[g / 8] |= (uint8_t)(1 << g % 8);
            log_line("embedded controller: ports %x/%x, status %x, GPE %d\n", ec_data, ec_sc, inb(ec_sc), ec_gpe);
            if (aml_find(ec, "_REG") >= 0) aml_eval(ec, "_REG", 2, a, &x, 1);
        }
    }
    bat = dev("PNP0C0A", "battery");
    ac = dev("ACPI0003", "AC adapter");
    lid = dev("PNP0C0D", "lid");
    pwrb = dev("PNP0C0C", "power button");
    int64_t one = 1, x;
    if ((ihid = dev("INT33D5", "Intel HID events")) >= 0)    /* (on: its events, and the power button's) */
        aml_eval(ihid, "HDSM", 1, &one, &x, 1), aml_eval(ihid, "BTNL", 0, 0, &x, 1);
    if ((vbtn = dev("INT33D6", "Intel virtual buttons")) >= 0) aml_eval(vbtn, "VBDL", 0, 0, &x, 1);
    if ((sbtn_dev = dev("MSHW0040", "Surface buttons")) >= 0) surface_buttons_start(sbtn_dev);
    for (int i = 0, d[3] = { lid, pwrb, ec }, g; i < 3; i++)       /* what wakes us from S3 */
        if (d[i] >= 0 && (g = aml_wake(d[i], 3, -1)) >= 0 && g < 8 * gpelen) {
            gpe_wk[g / 8] |= (uint8_t)(1 << g % 8); wdev[nwdev++] = d[i];
            log_line("%s: wakes from S3: GPE %02x\n", i == 0 ? "lid" : i == 1 ? "power button" : "embedded controller", g);
        }
    for (int g = 0; g < 8 * gpelen; g++) {          /* the GPEs with methods */
        char m[12] = "\\_GPE._L00";
        m[8] = "0123456789ABCDEF"[g >> 4]; m[9] = "0123456789ABCDEF"[g & 15];
        if (aml_find(0, m) < 0 && (m[7] = 'E', aml_find(0, m) < 0)) continue;
        gpe_en[g / 8] |= (uint8_t)(1 << g % 8);
        log_line("GPE %02x: %s\n", g, m + 6);
    }
    port = port_create("acpi");
    if (sci > 0 && irq_bind(sci, port) < 0) { log_line("SCI %d: cannot have it\n", sci); sci = -1; }
    arm();
    refresh();
    thread_start(ticker, 0, 4096);
    if (sbtn[0] || sbtn[1] || sbtn[2]) thread_start(surface_buttons, 0, 4096);

    static acpi_status_t out;
    for (;;) {
        msg_t m = { 0 };
        long from = ipc_recv(port, &m);
        if (from == 0) { events(); if (sci > 0) irq_ack(sci); }
        else if (from > 0) {
            msg_t r = { .w = { 0 } };
            switch (m.w[0]) {
            case ACPI_POWER:
                r.w[0] = m.uid ? -EPERM : (m.w[1] & 255) > POWER_SLEEP || m.w[1] >> 8 >= 3600 ? -EINVAL : power_do((int)m.w[1]);
                break;
            case ACPI_WAIT:
                if (m.w[1] == st.seq && nwait < 8) { waiters[nwait++] = from; from = 0; break; }
                /* fall through */
            case ACPI_STATUS: out = st; r.sbuf = &out; r.slen = sizeof out; break;
            case TICK:                              /* (w[1] 1: a Surface's power button) */
                if (m.pid != self) r.w[0] = -EPERM;
                else if (m.w[1] == 1) log_line("power button (Surface)\n"), button = 1;
                else dirty = 1, memset(hits, 0, sizeof hits);
                break;
            default: r.w[0] = -ENOSYS;
            }
            if (from) ipc_reply(from, &r);
        }
        if (button) { button = 0; log_line("power button: power off\n"); fs_sync(); power_do(POWER_OFF); }
        if (dirty) refresh();
    }
}
