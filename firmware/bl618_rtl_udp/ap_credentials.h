// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef AP_CREDENTIALS_H
#define AP_CREDENTIALS_H

// The board's own network name ("SDR-Bridge-3F2A") and its WPA2 password, drawn at random
// on first boot and kept in flash unless wifi_secrets.h sets BRIDGE_AP_KEY. Task context.
void ap_credentials(char ssid[33], char key[64]);

#endif
