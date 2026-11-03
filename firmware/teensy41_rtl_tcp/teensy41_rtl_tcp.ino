// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RTL-SDR → Teensy 4.1 → Ethernet → iPhone, serving rtl_tcp at 192.168.7.1:1234.
// The code is in src/: rtl_usb.cpp is the USB host side, bridge.cpp the network side.

void bridge_setup();
void bridge_loop();

void setup() { bridge_setup(); }
void loop() { bridge_loop(); }
