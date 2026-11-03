// SPDX-License-Identifier: GPL-2.0-or-later
//
// Settings that differ between users. The Wi-Fi credentials go in wifi_secrets.h, which is
// not committed — copy wifi_secrets.h.example and fill it in.

#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#if defined(__has_include)
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#endif

// **Station mode**: join a network, normally the iPhone's Personal Hotspot. The phone
// hands out the address, so the app's host is whatever the serial log prints at
// `got ip` (on a Personal Hotspot usually 172.20.10.2).
#ifndef BRIDGE_STA_SSID
#define BRIDGE_STA_SSID ""
#endif
#ifndef BRIDGE_STA_KEY
#define BRIDGE_STA_KEY ""
#endif
// How long to try before giving up on the network and opening our own.
#ifndef BRIDGE_STA_TIMEOUT_MS
#define BRIDGE_STA_TIMEOUT_MS 20000
#endif

// **Access-point mode**: the board's own network, when there is no SSID above or it
// cannot be joined. The phone joins it and connects to 192.168.7.1 — the same address as
// the Teensy's wired routes, so the app's setting is the same whichever board is in use.
// iOS keeps using mobile data while it is on a network with no internet.
//
// The name gets the chip ID's last two bytes ("SDR-Bridge-3F2A"). The password has **no
// default here on purpose** — this file is public — and is drawn at random on first boot
// (ap_credentials.c) unless wifi_secrets.h sets BRIDGE_AP_KEY (8 to 63 characters).
#ifndef BRIDGE_AP_SSID
#define BRIDGE_AP_SSID "SDR-Bridge"
#endif
#ifndef BRIDGE_AP_CHANNEL
#define BRIDGE_AP_CHANNEL 6
#endif
#define BRIDGE_AP_ADDRESS "192.168.7.1"

#define BRIDGE_RTL_TCP_PORT 1234
// rtl_udp beside it: control over TCP, samples in UDP datagrams. See rtl_tcp_server.c.
#define BRIDGE_RTL_UDP_PORT 1235

// mDNS: the app can use `rtlsdr.local` instead of an address, on either network. The
// Teensy board answers to the same name, so the app's setting does not depend on which
// board is in use; if both are on one network at once, the later one takes rtlsdr-2 and
// the serial log says so.
#ifndef BRIDGE_MDNS_NAME
#define BRIDGE_MDNS_NAME "rtlsdr"
#endif

#endif
