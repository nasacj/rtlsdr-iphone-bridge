// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdbool.h>

// RF, lwIP and the Wi-Fi tasks. Before the scheduler starts.
int wifi_link_start(void);

bool wifi_link_up(void);
// Phones joined to the board's own network; 0 when it has joined someone else's, where
// it cannot see who else is on it.
int wifi_link_ap_stations(void);
// The board's address for the log, "-" until there is one.
const char *wifi_link_address(void);

#endif
