// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef LED_H
#define LED_H

// The kit's three lamps as status lights, so the bridge can be carried around on a power bank
// with no serial console and still say what it is doing. **Each lamp answers one question**,
// and the patterns mean the same thing on all three: slow flash is waiting, steady is ready,
// breathing is working, fast flash is a fault.
//
// W, the yellow lamp (IO27) — is the RTL-SDR all right?
//
// | slow flash | no RTL-SDR plugged in                                              |
// | steady     | RTL-SDR open                                                       |
// | fast flash | plugged in but librtlsdr cannot open it, or a USB transfer failed  |
// |            | with nothing delivered (not one that failed after arriving whole:   |
// |            | those are delivered, see rtl_usb.c)                                 |
//
// C, the white lamp (IO29) — how far has the phone got?
//
// | off        | Wi-Fi not up yet                                                   |
// | slow flash | Wi-Fi up, no phone: nobody on the board's own network, or (joined  |
// |            | to someone else's network, where the board cannot see who else is |
// |            | on it) no client connected                                         |
// | steady     | a phone on the board's network, or a client connected, not playing |
// | breathing  | playing                                                            |
//
// The RGB lamp — is the stream healthy? Dark when nothing is playing and nothing is wrong.
//
// | red, steady            | not up yet: Wi-Fi starting, or failed to start                 |
// | blue, breathing        | playing, nothing lost                                          |
// | red and blue, fast     | the board lost samples on the USB side: it could not keep up   |
// | purple and blue, fast  | the board lost samples on the Wi-Fi side: the network is slow  |
// | red, steady, 3 s       | a stream stopped by itself: RTL-SDR pulled, no heartbeat, a    |
// |                        | send that failed                                               |
//
// A fault holds its pattern for a few seconds after the last occurrence, so one lost block
// is still visible to somebody glancing at the board.

// Lights red at once. Before the scheduler starts.
void led_init(void);

#endif
