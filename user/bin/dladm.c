/*
 * dladm - data links: the Wi-Fi device (the Solaris command's subcommands).
 *   dladm show-wifi          the Wi-Fi link's state
 *   dladm scan-wifi          scan, then list the networks heard (ESSID, BSSID, security, strength, channel)
 *   dladm show-link          the links (Ethernet interfaces and the Wi-Fi device)
 *   dladm connect-wifi [-e essid] [-k key | -k -] [-i bssid] [-q]
 *                            join a network (scanning first if it was not heard); the key is the WPA2
 *                            passphrase (-k - reads it from standard input) and is kept, with the ESSID,
 *                            in /etc/wifi.conf (root only).  Without -e: the strongest network kept there
 *                            (at boot, from /etc/rc).  -q: nothing printed.
 *   dladm disconnect-wifi    leave the network
 *   dladm show-linkprop [-p powermode] [iwx0]
 *   dladm set-linkprop -p powermode=off|fast|max [iwx0]
 *                            the Wi-Fi power save (fast: the default), kept in /etc/dladm/linkprop.conf
 * Installed set-user-ID root (joining a network is root's).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include "sieos/sysinfo.h"
#include <sys/stat.h>
#include <sys/wait.h>

#define WLINK "iwx0"

static const char *state_name(int s)
{
    static const char *n[] = { "none", "down", "disconnected", "scanning", "connecting", "connected" };
    return s >= 0 && s <= 5 ? n[s] : "?";
}

static const char *sec_name(unsigned s)
{
    if (s & SIEOS_WIFI_SEC_RSN)
        return s & SIEOS_WIFI_SEC_SAE ? (s & SIEOS_WIFI_SEC_PSK ? "wpa2/3" : "wpa3")
             : s & SIEOS_WIFI_SEC_8021X && !(s & SIEOS_WIFI_SEC_PSK) ? "wpa2-eap" : "wpa2";
    if (s & SIEOS_WIFI_SEC_WPA)
        return "wpa";
    if (s & SIEOS_WIFI_SEC_WEP)
        return "wep";
    return "none";
}

static const char *strength(int rssi)
{
    return rssi >= -55 ? "excellent" : rssi >= -67 ? "very good" : rssi >= -75 ? "good" : rssi >= -83 ? "weak"
                                                                                                     : "very weak";
}

static int status(struct sieos_wifi_status *st)
{
    if (wifi(SIEOS_WIFI_OP_STATUS, st, sizeof(*st)) < 0 || st->ws_state == SIEOS_WIFI_NONE) {
        fprintf(stderr, "dladm: no Wi-Fi device\n");
        return -1;
    }
    return 0;
}

static int show_wifi(void)
{
    struct sieos_wifi_status st;
    if (status(&st) < 0)
        return 1;
    static const char *const modes[] = { "--", "a/g", "n", "ac" };
    char mode[24] = "--";
    if (st.ws_mode > 0 && st.ws_mode <= 3)
        snprintf(mode, sizeof(mode), "%s %dMHz %dx%d", modes[st.ws_mode], st.ws_width, st.ws_streams, st.ws_streams);
    printf("%-8s %-13s %-20s %-16s %s\n", "LINK", "STATUS", "ESSID", "MODE", "INFO");
    printf("%-8s %-13s %-20s %-16s %s\n", WLINK, state_name(st.ws_state), st.ws_ssid[0] ? st.ws_ssid : "--", mode,
           st.ws_info);
    return 0;
}

#define PROPCONF "/etc/dladm/linkprop.conf"
static const char *const powermodes[] = { "off", "fast", "max" };

/* The power mode kept (-1: none). */
static int saved_powermode(void)
{
    FILE *f = fopen(PROPCONF, "r");
    char line[128];
    int m = -1;
    while (f && fgets(line, sizeof(line), f)) {
        char *p = strstr(line, WLINK " powermode=");
        if (!p)
            continue;
        p += strlen(WLINK " powermode=");
        for (int i = 0; i < 3; i++)
            if (!strncmp(p, powermodes[i], strlen(powermodes[i])))
                m = i;
    }
    if (f)
        fclose(f);
    return m;
}

static int show_linkprop(void)
{
    struct sieos_wifi_status st;
    if (status(&st) < 0)
        return 1;
    printf("%-8s %-12s %-6s %-8s %-8s %s\n", "LINK", "PROPERTY", "PERM", "VALUE", "DEFAULT", "POSSIBLE");
    printf("%-8s %-12s %-6s %-8s %-8s %s\n", WLINK, "powermode", "rw",
           st.ws_power >= 0 && st.ws_power <= 2 ? powermodes[st.ws_power] : "?", "fast", "off,fast,max");
    return 0;
}

static int set_linkprop(int argc, char **argv)
{
    const char *prop = NULL;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            prop = argv[++i];
    int m = -1;
    for (int i = 0; prop && i < 3; i++)
        if (!strncmp(prop, "powermode=", 10) && !strcmp(prop + 10, powermodes[i]))
            m = i;
    if (m < 0) {
        fprintf(stderr, "usage: dladm set-linkprop -p powermode=off|fast|max [%s]\n", WLINK);
        return 2;
    }
    if (wifi(SIEOS_WIFI_OP_POWER, NULL, m) < 0) {
        perror("dladm: set-linkprop");
        return 1;
    }
    mkdir("/etc/dladm", 0755);
    FILE *f = fopen(PROPCONF, "w");
    if (f) {
        fprintf(f, "# link properties set by dladm set-linkprop\n%s powermode=%s\n", WLINK, powermodes[m]);
        fclose(f);
    }
    return 0;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const struct sieos_wifi_bss *)b)->wb_rssi - ((const struct sieos_wifi_bss *)a)->wb_rssi;
}

static int scan_wifi(void)
{
    struct sieos_wifi_status st;
    if (status(&st) < 0)
        return 1;
    if (st.ws_state == SIEOS_WIFI_DOWN) {
        fprintf(stderr, "dladm: %s is down: %s\n", WLINK, st.ws_info);
        return 1;
    }
    unsigned before = st.ws_scans;
    if (wifi(SIEOS_WIFI_OP_SCAN, NULL, 0) < 0 && errno != EBUSY) {
        perror("dladm: scan-wifi");
        return 1;
    }
    for (int t = 0; t < 150; t++) {              /* up to 15 s */
        usleep(100000);
        if (wifi(SIEOS_WIFI_OP_STATUS, &st, sizeof(st)) < 0 || st.ws_scans != before)
            break;
    }
    static struct sieos_wifi_bss b[64];
    long n = wifi(SIEOS_WIFI_OP_RESULTS, b, 64);
    if (n < 0) {
        perror("dladm: scan-wifi");
        return 1;
    }
    qsort(b, n, sizeof(b[0]), cmp_rssi);
    printf("%-8s %-24s %-17s %-8s %-10s %s\n", "LINK", "ESSID", "BSSID", "SEC", "STRENGTH", "CHANNEL");
    for (long i = 0; i < n; i++)
        printf("%-8s %-24s %02x:%02x:%02x:%02x:%02x:%02x %-8s %-10s %d\n", WLINK,
               b[i].wb_ssid[0] ? b[i].wb_ssid : "(hidden)", b[i].wb_bssid[0], b[i].wb_bssid[1], b[i].wb_bssid[2],
               b[i].wb_bssid[3], b[i].wb_bssid[4], b[i].wb_bssid[5], sec_name(b[i].wb_sec), strength(b[i].wb_rssi),
               b[i].wb_channel);
    return 0;
}

#define WCONF "/etc/wifi.conf"
#define NKNOWN 16

static struct { char essid[34], key[66]; } known[NKNOWN];
static int nknown;
static bool quiet;

static void load_known(void)
{
    FILE *f = fopen(WCONF, "r");
    char line[128];
    nknown = 0;
    while (f && fgets(line, sizeof(line), f) && nknown < NKNOWN) {
        line[strcspn(line, "\n")] = 0;
        char *tab = strchr(line, '\t');
        if (line[0] == '#' || !tab)
            continue;
        *tab = 0;
        snprintf(known[nknown].essid, sizeof(known[0].essid), "%s", line);
        snprintf(known[nknown].key, sizeof(known[0].key), "%s", tab + 1);
        nknown++;
    }
    if (f)
        fclose(f);
}

/* The network and its key kept (replacing its old key), readable by root only. */
static void save_known(const char *essid, const char *key)
{
    load_known();
    int i = 0;
    while (i < nknown && strcmp(known[i].essid, essid))
        i++;
    if (i == nknown) {
        if (nknown == NKNOWN)
            i = --nknown;                        /* (the last one dropped) */
        nknown++;
    }
    snprintf(known[i].essid, sizeof(known[0].essid), "%s", essid);
    snprintf(known[i].key, sizeof(known[0].key), "%s", key);
    int fd = open(WCONF ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    dprintf(fd, "# Wi-Fi networks joined by dladm connect-wifi: ESSID<tab>key\n");
    for (int k = 0; k < nknown; k++)
        dprintf(fd, "%s\t%s\n", known[k].essid, known[k].key);
    fchmod(fd, 0600);
    close(fd);
    rename(WCONF ".new", WCONF);
}

static long scan_now(struct sieos_wifi_bss *b, int max)
{
    struct sieos_wifi_status st;
    if (wifi(SIEOS_WIFI_OP_STATUS, &st, sizeof(st)) < 0)
        return -1;
    unsigned before = st.ws_scans;
    if (wifi(SIEOS_WIFI_OP_SCAN, NULL, 0) < 0 && errno != EBUSY)
        return -1;
    for (int t = 0; t < 150; t++) {
        usleep(100000);
        if (wifi(SIEOS_WIFI_OP_STATUS, &st, sizeof(st)) < 0 || st.ws_scans != before)
            break;
    }
    return wifi(SIEOS_WIFI_OP_RESULTS, b, max);
}

static int parse_mac(const char *s, unsigned char m[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++)
        m[i] = v[i];
    return 0;
}

/* Once DHCP gave the link an address (up to 15 s): the resolver's servers again (ifconfig -r). */
static void resolv_update(int link)
{
    struct netinfo ni;
    for (int t = 0; t < 150 && link >= 0; t++) {
        if (netinfo_if(&ni, link) == 0 && ni.up)
            break;
        usleep(100000);
    }
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            dup2(fd, 1);
        execl("/bin/ifconfig", "ifconfig", "-r", (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static int connect_wifi(int argc, char **argv)
{
    struct sieos_wifi_connect c;
    memset(&c, 0, sizeof(c));
    const char *essid = NULL, *key = NULL;
    char keybuf[80];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc)
            essid = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc)
            key = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            if (parse_mac(argv[++i], c.wc_bssid) < 0) {
                fprintf(stderr, "dladm: bad BSSID %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "-q"))
            quiet = true;
        else if (strcmp(argv[i], WLINK)) {
            fprintf(stderr, "usage: dladm connect-wifi [-e essid] [-k key|-] [-i bssid] [-q] [%s]\n", WLINK);
            return 2;
        }
    }
    if (key && !strcmp(key, "-")) {
        if (!fgets(keybuf, sizeof(keybuf), stdin))
            keybuf[0] = 0;
        keybuf[strcspn(keybuf, "\r\n")] = 0;
        key = keybuf;
    }
    struct sieos_wifi_status st;
    if (status(&st) < 0)
        return 1;
    if (st.ws_state == SIEOS_WIFI_DOWN) {
        fprintf(stderr, "dladm: %s is down: %s\n", WLINK, st.ws_info);
        return 1;
    }
    int pm = saved_powermode();                  /* (the power mode kept, applied at the join) */
    if (pm >= 0)
        wifi(SIEOS_WIFI_OP_POWER, NULL, pm);
    static struct sieos_wifi_bss b[64];
    long n = wifi(SIEOS_WIFI_OP_RESULTS, b, 64);
    load_known();
    if (!essid) {                                /* the strongest network we know */
        if (!nknown) {
            if (!quiet)
                fprintf(stderr, "dladm: no network kept in %s: give -e essid\n", WCONF);
            return 1;
        }
        n = scan_now(b, 64);
        int best = -1, bk = -1;
        for (long i = 0; i < n; i++)
            for (int k = 0; k < nknown; k++)
                if (!strcmp(b[i].wb_ssid, known[k].essid) && (best < 0 || b[i].wb_rssi > b[best].wb_rssi))
                    best = i, bk = k;
        if (best < 0) {
            if (!quiet)
                fprintf(stderr, "dladm: none of the networks kept in %s is in range\n", WCONF);
            return 1;
        }
        essid = known[bk].essid;
        if (!key)
            key = known[bk].key;
    } else {
        bool heard = false;
        for (long i = 0; i < n; i++)
            heard |= !strcmp(b[i].wb_ssid, essid);
        if (!heard) {
            n = scan_now(b, 64);
            for (long i = 0; i < n; i++)
                heard |= !strcmp(b[i].wb_ssid, essid);
        }
        if (!key)
            for (int k = 0; k < nknown; k++)
                if (!strcmp(known[k].essid, essid))
                    key = known[k].key;
    }
    snprintf(c.wc_ssid, sizeof(c.wc_ssid), "%s", essid);
    snprintf(c.wc_key, sizeof(c.wc_key), "%s", key ? key : "");
    if (wifi(SIEOS_WIFI_OP_CONNECT, &c, sizeof(c)) < 0) {
        int e = errno;
        if (!quiet)
            fprintf(stderr, "dladm: %s: %s\n", essid,
                    e == ENOENT ? "no such network in range"
                  : e == ENOTSUP || e == EOPNOTSUPP ? "its security is not supported (WPA2-Personal and open networks are)"
                  : e == EINVAL ? "the key must be a passphrase of 8 to 63 characters (or 64 hex digits)"
                  : e == EPERM ? "joining a network is root's" : strerror(e));
        memset(&c, 0, sizeof(c));
        return 1;
    }
    memset(&c, 0, sizeof(c));
    /* the result: joined, or why not (up to 20 s) */
    for (int t = 0; t < 200; t++) {
        usleep(100000);
        if (wifi(SIEOS_WIFI_OP_STATUS, &st, sizeof(st)) < 0)
            break;
        if (st.ws_state == SIEOS_WIFI_JOINED) {
            if (key && key[0])
                save_known(essid, key);
            else
                save_known(essid, "");
            if (!quiet)
                printf("%s: connected to %s\n", WLINK, essid);
            resolv_update(st.ws_link);
            return 0;
        }
        if (st.ws_state != SIEOS_WIFI_JOINING && t > 2)
            break;
    }
    if (!quiet)
        fprintf(stderr, "dladm: %s: %s\n", essid, st.ws_info);
    return 1;
}

static int disconnect_wifi(void)
{
    if (wifi(SIEOS_WIFI_OP_DISCONNECT, NULL, 0) < 0) {
        perror("dladm: disconnect-wifi");
        return 1;
    }
    return 0;
}

static int show_link(void)
{
    printf("%-8s %-8s %-6s %s\n", "LINK", "CLASS", "STATE", "DRIVER");
    struct netinfo ni;
    for (int i = 0; netinfo_if(&ni, i) == 0; i++)
        if (strcmp(ni.name, WLINK))
            printf("%-8s %-8s %-6s %s\n", ni.name, "phys", ni.up ? "up" : "down", ni.driver);
    struct sieos_wifi_status st;
    if (wifi(SIEOS_WIFI_OP_STATUS, &st, sizeof(st)) == 0 && st.ws_state != SIEOS_WIFI_NONE)
        printf("%-8s %-8s %-6s %s\n", WLINK, "wifi", st.ws_state == SIEOS_WIFI_JOINED ? "up" : "down", "iwlwifi");
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "show-link";
    if (!strcmp(cmd, "show-wifi"))
        return show_wifi();
    if (!strcmp(cmd, "scan-wifi"))
        return scan_wifi();
    if (!strcmp(cmd, "show-link"))
        return show_link();
    if (!strcmp(cmd, "connect-wifi"))
        return connect_wifi(argc, argv);
    if (!strcmp(cmd, "disconnect-wifi"))
        return disconnect_wifi();
    if (!strcmp(cmd, "show-linkprop"))
        return show_linkprop();
    if (!strcmp(cmd, "set-linkprop"))
        return set_linkprop(argc, argv);
    fprintf(stderr, "usage: dladm show-link | show-wifi | scan-wifi | connect-wifi [-e essid] [-k key] | disconnect-wifi\n");
    return 2;
}
