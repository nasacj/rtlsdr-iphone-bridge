// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RTL_TCP_SERVER_H
#define RTL_TCP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

// The copy and serve tasks. Before the scheduler starts, after rtl_usb_init and
// wifi_link_start.
void rtl_tcp_server_start(void);

// For the status lamp, from any task: whether librtlsdr has the RTL-SDR open, and whether
// samples are flowing to a client (rtl_tcp from the greeting, rtl_udp from START_UDP).
bool rtl_tcp_server_dongle_open(void);
bool rtl_tcp_server_streaming(void);
// Whether a client is connected at all, streaming or not.
bool rtl_tcp_server_client(void);
// A count of streams that stopped other than by the client's choice; the lamp watches it
// change.
uint32_t rtl_tcp_server_interruptions(void);

#endif
