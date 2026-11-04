/* The web page and its WebSocket over Ethernet, and the page's commands,
 * which the USB serial port takes as well. GPL-3.0-or-later.
 *
 * The messages and commands are gate/src/web.c's (see its web.h) except
 * for the network: no Wi-Fi settings; "set ip" and "set gw" set the
 * Ethernet address, and the "net" message carries it (net.cpp).
 *
 * One small HTTP server on port 80: GET / sends the page (gate/web's
 * index.html, gzipped by embed_page.py), GET /ws upgrades to the page's
 * WebSocket, GET /api to the timer API's (gate/src/gate_api.h,
 * docs/timer-api.md). Nothing waits: the page goes out as fast as its
 * connection takes it, a page message that doesn't fit a client's send
 * buffer is dropped for that client (timer API messages wait their turn),
 * and a client with no room for 5 s is closed: gone or not reading. (The
 * WROOM gives it a second; TCP takes longer than that to resend a lost
 * packet, which Wi-Fi clients see now and then.) */
#include <Arduino.h>
#include <QNEthernet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "index_html.h"
#include "teensy_gate.h"
#include "version.h"
#include "ws.h"

using namespace qindesign::network;

#define LEVELS_EVERY_MS 33
#define STATUS_EVERY_MS 1000
#define MAX_CLIENTS 6      /* page loads and WebSockets at once; lwIP has 8 TCP connections */
#define REQUEST_BYTES 1536 /* request line and headers */
#define REQUEST_MS 5000    /* to send them */
#define SEND_MS 5000       /* a client with no room for what it is due this long is closed */
#define IN_BYTES 1024      /* frames from a client */
#define COMMAND_BYTES 256  /* the page's longest command, as on the WROOM */
#define MESSAGE_BYTES 768  /* the longest message to a client */
#define API_BURST 32       /* timer API messages per client per UI tick, at most */

static const char *const BOARD[] = {"top", "bottom", "gate"};

typedef enum { C_FREE, C_REQUEST, C_RESPONSE, C_WS } client_state_t;

typedef struct {
    EthernetClient conn;
    client_state_t state;
    uint32_t since_ms;   /* C_REQUEST: when it connected */
    bool full;           /* a send found no room, and nothing has gone in since */
    uint32_t full_ms;    /* when it found none */
    char req[REQUEST_BYTES];
    size_t req_len;
    const uint8_t *body; /* C_RESPONSE: what is still to go */
    size_t body_len;
    uint8_t in[IN_BYTES]; /* C_WS: frames from the client */
    size_t in_len;
    bool api;             /* C_WS: the timer API (/api), not the page (/ws) */
    gate_api_client_t api_client;
} client_t;

static EthernetServer server(80);
static client_t clients[MAX_CLIENTS];

/* Only while a terminal has the port open, and only what fits, so the
 * console never holds up loop(). */
void console_printf(const char *fmt, ...) {
    if (!Serial.dtr()) return;
    char buf[MESSAGE_BYTES];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    if (Serial.availableForWrite() >= n) Serial.write(buf, (size_t)n);
}

/* graceful: the response is out, close once it has gone. Otherwise the
 * client is gone, stalled or not speaking the protocol: reset. */
static void close_client(client_t *c, bool graceful) {
    if (graceful) {
        uint8_t junk[64]; /* unread input would turn the close into a reset */
        for (int i = 0; i < 16 && c->conn.read(junk, sizeof(junk)) > 0; i++) {
        }
        c->conn.close();
    } else {
        c->conn.abort();
    }
    c->state = C_FREE;
}

static void blocked(client_t *c, bool sent) {
    if (sent) {
        c->full = false;
    } else if (!c->full) {
        c->full = true;
        c->full_ms = millis();
    }
}

/* Something to send, and no room for it this long: gone or not reading. */
static bool stalled(const client_t *c) { return c->full && millis() - c->full_ms > SEND_MS; }

/* All of it or nothing: part of a WebSocket frame would garble the stream,
 * so what doesn't fit the send buffer is dropped for this client. lwIP
 * takes a write whole or not at all (none: out of TCP segments, as good as
 * no room). */
static bool put(client_t *c, const uint8_t *data, size_t len) {
    size_t n = c->conn.availableForWrite() >= (int)len ? c->conn.write(data, len) : 0;
    if (n && n != len) {
        close_client(c, false);
        return false;
    }
    blocked(c, n);
    return n;
}

static bool ws_send(client_t *c, uint8_t opcode, const void *payload, size_t len) {
    uint8_t frame[WS_HEADER_MAX + MESSAGE_BYTES];
    if (len > MESSAGE_BYTES) return false;
    size_t h = ws_header(frame, opcode, len);
    memcpy(frame + h, payload, len);
    return put(c, frame, h + len);
}

/* To every page. */
static void broadcast(const char *text, size_t len) {
    if (!len) return;
    for (client_t &c : clients)
        if (c.state == C_WS && !c.api) ws_send(&c, WS_TEXT, text, len);
}

/* to: one client, or NULL for all. */
static void send_config(client_t *to) {
    char buf[512];
    size_t n = gate_json_config(buf, sizeof(buf));
    if (to && n) ws_send(to, WS_TEXT, buf, n);
    else if (!to) broadcast(buf, n);
}

static void send_net(client_t *to) {
    char buf[256];
    size_t n = net_json(buf, sizeof(buf));
    if (to && n) ws_send(to, WS_TEXT, buf, n);
    else if (!to) broadcast(buf, n);
}

static void apply(const settings_t *s) {
    settings_t old;
    settings_get(&old);
    settings_set(s);
    gate_settings_changed(&old, s);
    send_config(NULL);
}

static void command(char *line) {
    settings_t s;
    settings_get(&s);
    char key[16];
    int pos = 0;
    long v = 0;
    if (sscanf(line, "set %15s %n", key, &pos) == 1 && pos) {
        const char *val = line + pos;
        if (!strcmp(key, "ip") || !strcmp(key, "gw")) {
            if (net_set(key, val)) send_net(NULL);
            else gate_line(C5_BOARDS, "ERR gate: not an address");
            return;
        }
        v = strtol(val, NULL, 10);
        if (!strcmp(key, "gain") && v >= 0 && v <= 83) s.gain = (uint8_t)v;
        else if (!strcmp(key, "enter") && v >= 1 && v <= 254) s.enter_at = (uint8_t)v;
        else if (!strcmp(key, "exit") && v >= 1 && v <= 254) s.exit_at = (uint8_t)v;
        else if (!strcmp(key, "delta") && v >= 0 && v <= 254) s.delta_max = (uint8_t)v;
        else if (!strcmp(key, "offset") && v >= -254 && v <= 254) s.offset = (int16_t)v;
        else if (!strcmp(key, "pair") && v >= 1 && v <= 1000) s.pair_window_ms = (uint16_t)v;
        else if (!strcmp(key, "wait") && v >= 50 && v <= 10000) s.pair_wait_ms = (uint16_t)v;
        else if (!strcmp(key, "minlap") && v >= 0 && v <= 600000) s.min_lap_ms = (uint32_t)v;
        else if (!strcmp(key, "channels") && *val) snprintf(s.channels, sizeof(s.channels), "%s", val);
        else {
            gate_line(C5_BOARDS, "ERR gate: bad setting");
            return;
        }
        if (s.exit_at > s.enter_at) s.exit_at = s.enter_at;
        apply(&s);
    } else if (!strcmp(line, "save")) {
        bool ok = settings_save() && net_save();
        gate_line(C5_BOARDS, ok ? "OK gate: saved" : "ERR gate: save failed");
    } else if (!strcmp(line, "defaults")) {
        settings_t d; /* the Ethernet address stays: the way in */
        settings_defaults(&d);
        apply(&d);
    } else if (sscanf(line, "cal %ld", &v) == 1 && v >= 1 && v <= 50) {
        gate_calibrate((unsigned)v);
    } else if (sscanf(line, "baud %ld %n", &v, &pos) == 1 && pos && v >= 0 && v < C5_BOARDS) {
        c5_link_set_baud((unsigned)v, atoi(line + pos)); /* 0: back to the link's own */
    } else if (!strcmp(line, "clear")) {
        gate_clear();
    } else if (!strncmp(line, "c5 ", 3)) {
        char who[8];
        if (sscanf(line + 3, "%7s %n", who, &pos) == 1 && pos) {
            const char *cmd = line + 3 + pos;
            for (unsigned b = 0; b < C5_BOARDS; b++)
                if (!strcmp(who, "all") || (unsigned)atoi(who) == b) c5_link_send(b, cmd);
        }
    } else if (!strcmp(line, "net")) {
        net_print();
    } else {
        gate_line(C5_BOARDS, "ERR gate: unknown command");
    }
}

/* An HTTP response; its body, if any, goes out from web_poll. */
static void respond(client_t *c, const char *status, const char *headers, const uint8_t *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Length: %u\r\nConnection: close\r\n%s\r\n", status,
                     (unsigned)len, headers);
    c->body = body;
    c->body_len = len;
    c->state = C_RESPONSE;
    if (!put(c, (const uint8_t *)head, (size_t)n)) close_client(c, false);
}

/* A request header's value without the spaces around it, or NULL. Names
 * are case-insensitive. */
static const char *header(const char *req, const char *name, size_t *len) {
    size_t name_len = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (strncasecmp(p, name, name_len) || p[name_len] != ':') continue;
        const char *v = p + name_len + 1, *e = strstr(v, "\r\n");
        while (*v == ' ' || *v == '\t') v++;
        while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *len = (size_t)(e - v);
        return v;
    }
    return NULL;
}

/* The WebSocket handshake, then for the page its settings and network, for
 * a timer API client the hello. */
static void upgrade(client_t *c, bool api) {
    size_t key_len = 0, up_len = 0;
    const char *key = header(c->req, "Sec-WebSocket-Key", &key_len);
    const char *up = header(c->req, "Upgrade", &up_len);
    if (!key || !up || up_len != 9 || strncasecmp(up, "websocket", 9)) {
        respond(c, "400 Bad Request", "", NULL, 0);
        return;
    }
    char accept[29], head[160];
    ws_accept(key, key_len, accept);
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    c->state = C_WS;
    c->api = api;
    c->in_len = 0;
    c->conn.setNoDelay(true); /* messages go out as they are written, not Nagle-delayed */
    if (!put(c, (const uint8_t *)head, (size_t)n)) {
        close_client(c, false);
        return;
    }
    if (api) {
        char buf[MESSAGE_BYTES];
        size_t len = gate_api_hello(&c->api_client, buf, sizeof(buf));
        if (len) ws_send(c, WS_TEXT, buf, len);
    } else {
        send_config(c);
        send_net(c);
    }
}

static void request(client_t *c) {
    char method[8], path[96];
    if (sscanf(c->req, "%7s %95s", method, path) != 2) {
        respond(c, "400 Bad Request", "", NULL, 0);
        return;
    }
    char *query = strchr(path, '?');
    if (query) *query = 0;
    if (strcmp(method, "GET"))
        respond(c, "405 Method Not Allowed", "Allow: GET\r\n", NULL, 0);
    else if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
        respond(c, "200 OK", "Content-Type: text/html\r\nContent-Encoding: gzip\r\nCache-Control: no-cache\r\n",
                index_html_gz, sizeof(index_html_gz));
    else if (!strcmp(path, "/ws") || !strcmp(path, "/api"))
        upgrade(c, path[1] == 'a');
    else
        respond(c, "404 Not Found", "", NULL, 0);
}

static void poll_request(client_t *c) {
    int n = c->conn.read((uint8_t *)c->req + c->req_len, sizeof(c->req) - 1 - c->req_len);
    if (n > 0) {
        c->req_len += (size_t)n;
        c->req[c->req_len] = 0;
        if (strstr(c->req, "\r\n\r\n")) {
            request(c);
            return;
        }
        if (c->req_len == sizeof(c->req) - 1) {
            respond(c, "431 Request Header Fields Too Large", "", NULL, 0);
            return;
        }
    }
    if (!c->conn.connected() || millis() - c->since_ms > REQUEST_MS) close_client(c, false);
}

static void poll_response(client_t *c) {
    if (c->body_len) {
        int room = c->conn.availableForWrite();
        size_t n = room > 0 ? c->conn.write(c->body, (size_t)room < c->body_len ? (size_t)room : c->body_len) : 0;
        c->body += n;
        c->body_len -= n;
        blocked(c, n);
    }
    if (!c->body_len) close_client(c, true);
    else if (!c->conn.connected() || stalled(c)) close_client(c, false);
}

/* A request to the timer API, and its reply. */
static void api_request(client_t *c, const char *json) {
    char reply[MESSAGE_BYTES];
    size_t n = gate_api_request(&c->api_client, json, reply, sizeof(reply));
    if (n) ws_send(c, WS_TEXT, reply, n);
}

static void ws_frame(client_t *c, const ws_frame_t *f) {
    if (f->opcode == WS_TEXT && f->fin && f->len && (c->api || f->len < COMMAND_BYTES)) {
        char text[IN_BYTES];
        memcpy(text, f->payload, f->len);
        text[f->len] = 0;
        if (c->api) api_request(c, text);
        else command(text);
    } else if (f->opcode == WS_PING) {
        ws_send(c, WS_PONG, f->payload, f->len);
    } else if (f->opcode == WS_CLOSE) {
        ws_send(c, WS_CLOSE, f->payload, f->len < 2 ? f->len : 2); /* its status code back */
        close_client(c, true);
    }
}

static void poll_ws(client_t *c) {
    int n = c->conn.read(c->in + c->in_len, sizeof(c->in) - c->in_len);
    if (n > 0) c->in_len += (size_t)n;
    while (c->state == C_WS) {
        ws_frame_t f;
        long used = ws_parse(c->in, c->in_len, sizeof(c->in) - WS_HEADER_MAX - 4, &f);
        if (used == 0) break;
        if (used < 0) {
            close_client(c, false);
            return;
        }
        ws_frame(c, &f);
        c->in_len -= (size_t)used;
        memmove(c->in, c->in + used, c->in_len);
    }
    if (c->state == C_WS && (!c->conn.connected() || stalled(c))) close_client(c, false);
}

/* A timer API client's messages that are due: detections, levels, status,
 * as many as its connection takes now. */
static void api_poll(client_t *c, uint32_t now) {
    char buf[MESSAGE_BYTES];
    for (int i = 0; i < API_BURST && c->state == C_WS; i++) {
        gate_api_client_t before = c->api_client;
        size_t n = gate_api_next(&c->api_client, now, buf, sizeof(buf));
        if (!n) break;
        if (!ws_send(c, WS_TEXT, buf, n)) {
            c->api_client = before; /* again next tick */
            break;
        }
    }
}

static void accept_client(void) {
    EthernetClient conn = server.accept();
    if (!conn) return;
    for (client_t &c : clients)
        if (c.state == C_FREE) {
            c.conn = conn;
            c.state = C_REQUEST;
            c.since_ms = millis();
            c.full = false;
            c.req_len = 0;
            c.req[0] = 0;
            c.api = false;
            return;
        }
    conn.abort(); /* all taken */
}

/* An event on the console: reply lines as text, the rest as JSON. */
static void console_event(const char *json) {
    unsigned b;
    int pos = 0;
    if (sscanf(json, "{\"t\":\"line\",\"b\":%u,\"s\":\"%n", &b, &pos) == 1 && pos) {
        char text[MESSAGE_BYTES];
        size_t k = 0;
        for (const char *p = json + pos; *p && strcmp(p, "\"}") && k + 1 < sizeof(text); p++) {
            if (*p == '\\' && p[1]) p++;
            text[k++] = *p;
        }
        text[k] = 0;
        console_printf("[%s] %s\n", BOARD[b < 3 ? b : 2], text);
    } else {
        console_printf("%s\n", json);
    }
}

/* What the WROOM's UI task does: levels ~30 times a second, events as they
 * come, status, config and network once a second. Runs with no page open
 * too: it also reports singles and saves a finished calibration. Then the
 * timer API clients' messages. */
static void ui_poll(void) {
    static uint32_t levels_ms, status_ms;
    static char buf[MESSAGE_BYTES];
    uint32_t now = millis();
    if (now - levels_ms < LEVELS_EVERY_MS) return;
    levels_ms = now;
    broadcast(buf, gate_json_levels(buf, sizeof(buf)));
    size_t n;
    while ((n = gate_json_event(buf, sizeof(buf))) > 0) {
        broadcast(buf, n);
        console_event(buf);
    }
    if (now - status_ms >= STATUS_EVERY_MS) {
        status_ms = now;
        broadcast(buf, gate_json_status(buf, sizeof(buf)));
        send_config(NULL); /* a calibration changes the offset */
        send_net(NULL);
    }
    for (client_t &c : clients)
        if (c.state == C_WS && c.api) api_poll(&c, now);
    for (client_t &c : clients)
        if (c.state == C_WS) c.conn.flush();
}

/* The USB serial port: commands a line at a time. When a terminal opens
 * it, where the gate is and what it takes. */
static void console_poll(void) {
    static uint32_t closed_ms;
    static bool greeted;
    static char line[COMMAND_BYTES];
    static size_t len;
    if (!Serial.dtr()) {
        closed_ms = millis();
        greeted = false;
    } else if (!greeted && millis() - closed_ms >= 100) {
        greeted = true;
        console_printf("racetimer gate controller, Teensy 4.1\n");
        net_print();
        console_printf("Commands, as on the web page: set <key> <value> (ip, gw, gain, enter, exit, delta, offset, "
                       "pair, wait, minlap, channels), save, defaults, cal <n>, clear, c5 <0|1|all> <command>, "
                       "baud <0|1> <rate>, net\n");
    }
    int c;
    while ((c = Serial.read()) >= 0) {
        if (c == '\r' || c == '\n') {
            if (len) {
                line[len] = 0;
                len = 0;
                command(line);
            }
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
}

void web_start(void) {
    gate_api_init(net_name(), "teensy41", GATE_VERSION);
    server.begin();
}

void web_poll(void) {
    accept_client();
    for (client_t &c : clients) {
        if (c.state == C_REQUEST) poll_request(&c);
        else if (c.state == C_RESPONSE) poll_response(&c);
        else if (c.state == C_WS) poll_ws(&c);
    }
    ui_poll();
    console_poll();
}
