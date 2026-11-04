/* The Teensy build's own parts, and gate/src's API for its C++ files.
 * GPL-3.0-or-later.
 *
 * On the WROOM the receiver links, the web server and the UI are tasks.
 * The Teensy runs them all from loop(), so each has a poll function. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
/* gate/src and Widscan58's frames.h are C, and frames.h checks its layouts
 * with C11's _Static_assert, which C++ spells static_assert. (newlib's
 * sys/cdefs.h maps it already, once anything has included it.) */
#ifndef _Static_assert
#define _Static_assert static_assert
#endif
extern "C" {
#endif

#include "c5_frame.h"
#include "c5_link.h"
#include "gate.h"
#include "gate_api.h"
#include "gate_fusion.h"
#include "settings.h"

/* Read the receivers' ports and run their setups (c5_link.cpp). */
void c5_link_poll(void);

/* Ethernet (net.cpp). */
/* Load the saved address settings, name the gate and bring Ethernet up. */
void net_start(void);
/* Switch to a new address when due; report link and address changes on
 * the console. */
void net_poll(void);
/* "ip": a.b.c.d[/bits] (/24 if left out), or blank or "dhcp" for DHCP.
 * "gw": a.b.c.d, or blank for none. In use half a second later (not
 * saved); false if it isn't an address. */
bool net_set(const char *key, const char *value);
/* Save the address settings in use. */
bool net_save(void);
/* The page's "net" message: link, address, name, settings. */
size_t net_json(char *buf, size_t cap);
/* The gate's name on the network, racegate-XXXX. */
const char *net_name(void);
/* One line on the console: the gate's name, address and link. */
void net_print(void);

/* The web page, its WebSocket, and the USB serial console (web.cpp). */
void web_start(void);
void web_poll(void);
/* Text on the USB serial console, while a terminal has the port open. */
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
