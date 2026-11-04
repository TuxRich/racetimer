/* Ethernet: the gate's address (DHCP, or fixed in the settings), its name
 * on the network and the page's "net" message. GPL-3.0-or-later.
 *
 * The name is racegate-XXXX from the MAC address, as the WROOM names its
 * access point. It is the DHCP hostname, so the router lists it, and an
 * mDNS name: http://racegate-XXXX.local/. */
#include <Arduino.h>
#include <QNEthernet.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "nvs.h"
#include "teensy_gate.h"

using namespace qindesign::network;

#define NVS_NS "net"
#define NVS_KEY "cfg"
#define VERSION 1          /* bump when net_settings_t changes: older blobs are ignored */
#define APPLY_DELAY_MS 500 /* a new address waits this long, so the page hears of it first */

typedef struct {
    uint8_t ip[4];  /* 0.0.0.0: DHCP */
    uint8_t prefix; /* subnet mask bits */
    uint8_t gw[4];  /* 0.0.0.0: none */
} net_settings_t;

typedef struct {
    uint32_t version;
    net_settings_t n;
} blob_t;

static net_settings_t cur = {{0, 0, 0, 0}, 24, {0, 0, 0, 0}};
static char name[16];
static bool apply_due, changed;
static uint32_t apply_at;

static bool zero(const uint8_t a[4]) { return !(a[0] | a[1] | a[2] | a[3]); }

static IPAddress ip_of(const uint8_t a[4]) { return IPAddress(a[0], a[1], a[2], a[3]); }

/* Restarts the interface: connections to the old address end. */
static void apply(void) {
    if (zero(cur.ip)) {
        Ethernet.begin();
        return;
    }
    uint32_t m = 0xffffffffu << (32 - cur.prefix);
    IPAddress mask((uint8_t)(m >> 24), (uint8_t)(m >> 16), (uint8_t)(m >> 8), (uint8_t)m);
    Ethernet.begin(ip_of(cur.ip), mask, ip_of(cur.gw));
}

void net_start(void) {
    blob_t b;
    size_t len = sizeof(b);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &b, &len) == ESP_OK && len == sizeof(b) && b.version == VERSION &&
            b.n.prefix >= 1 && b.n.prefix <= 30)
            cur = b.n;
        nvs_close(h);
    }
    uint8_t mac[6];
    Ethernet.macAddress(mac);
    snprintf(name, sizeof(name), "racegate-%02X%02X", mac[4], mac[5]);
    Ethernet.setHostname(name);
    Ethernet.onLinkState([](bool) { changed = true; });
    Ethernet.onAddressChanged([] { changed = true; });
    apply();
    MDNS.begin(name);
    MDNS.addService("_http", "_tcp", 80);
}

void net_poll(void) {
    if (apply_due && (int32_t)(millis() - apply_at) >= 0) {
        apply_due = false;
        apply();
        changed = true;
    }
    if (changed) {
        changed = false;
        net_print();
    }
}

/* a.b.c.d at the start of s; *end: the characters it took. */
static bool parse_ip(const char *s, uint8_t out[4], int *end) {
    unsigned a[4];
    int n = 0;
    if (sscanf(s, "%3u.%3u.%3u.%3u%n", &a[0], &a[1], &a[2], &a[3], &n) != 4 || !n) return false;
    for (int i = 0; i < 4; i++) {
        if (a[i] > 255) return false;
        out[i] = (uint8_t)a[i];
    }
    *end = n;
    return true;
}

bool net_set(const char *key, const char *value) {
    char v[40];
    snprintf(v, sizeof(v), "%s", value);
    for (size_t n = strlen(v); n && v[n - 1] == ' '; n--) v[n - 1] = 0;
    net_settings_t n = cur;
    uint8_t a[4];
    int end = 0;
    if (!strcmp(key, "ip")) {
        if (!v[0] || !strcasecmp(v, "dhcp")) {
            memset(n.ip, 0, sizeof(n.ip));
        } else {
            unsigned bits = 24;
            int k = 0;
            if (!parse_ip(v, a, &end) || zero(a) || a[0] == 127 || a[0] >= 224) return false;
            if (v[end] == '/') {
                if (sscanf(v + end + 1, "%2u%n", &bits, &k) != 1 || v[end + 1 + k] || bits < 1 || bits > 30)
                    return false;
            } else if (v[end]) {
                return false;
            }
            memcpy(n.ip, a, sizeof(n.ip));
            n.prefix = (uint8_t)bits;
        }
    } else if (!strcmp(key, "gw")) {
        if (!v[0] || !strcasecmp(v, "none")) memset(n.gw, 0, sizeof(n.gw));
        else if (parse_ip(v, a, &end) && !v[end]) memcpy(n.gw, a, sizeof(n.gw));
        else return false;
    } else {
        return false;
    }
    if (!memcmp(&n, &cur, sizeof(n))) return true;
    cur = n;
    apply_due = true;
    apply_at = millis() + APPLY_DELAY_MS;
    return true;
}

const char *net_name(void) { return name; }

bool net_save(void) {
    blob_t b = {VERSION, cur};
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, NVS_KEY, &b, sizeof(b)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

size_t net_json(char *buf, size_t cap) {
    IPAddress ip = Ethernet.localIP();
    bool link = Ethernet.linkState();
    char sip[20] = "", sgw[16] = "";
    if (!zero(cur.ip))
        snprintf(sip, sizeof(sip), "%u.%u.%u.%u/%u", cur.ip[0], cur.ip[1], cur.ip[2], cur.ip[3], cur.prefix);
    if (!zero(cur.gw)) snprintf(sgw, sizeof(sgw), "%u.%u.%u.%u", cur.gw[0], cur.gw[1], cur.gw[2], cur.gw[3]);
    int n = snprintf(buf, cap,
                     "{\"t\":\"net\",\"if\":\"eth\",\"st\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"name\":\"%s\",\"dhcp\":%d,"
                     "\"mbps\":%d,\"sip\":\"%s\",\"sgw\":\"%s\"}",
                     !link ? "nolink" : (uint32_t)ip ? "up" : "dhcp", ip[0], ip[1], ip[2], ip[3], name,
                     Ethernet.isDHCPActive(), link ? Ethernet.linkInfo().speed : 0, sip, sgw);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

void net_print(void) {
    IPAddress ip = Ethernet.localIP();
    if (!Ethernet)
        console_printf("%s: no Ethernet (is this a Teensy 4.1 without the Ethernet chip?)\n", name);
    else if (!Ethernet.linkState())
        console_printf("%s: no Ethernet link (cable, switch?)\n", name);
    else if (!(uint32_t)ip)
        console_printf("%s: link up, waiting for an address from DHCP\n", name);
    else
        console_printf("%s: http://%u.%u.%u.%u/ or http://%s.local/ (%s, %d Mbit/s)\n", name, ip[0], ip[1], ip[2],
                       ip[3], name, Ethernet.isDHCPActive() ? "DHCP" : "fixed address", Ethernet.linkInfo().speed);
}
