#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Puts a BL616/BL618 into boot2's download mode over the serial port alone, with no boot pin.

    firmware/tools/bl618-isp-handshake.py /dev/cu.usbmodemXXXX     then press reset ONCE

Why it exists: the ROM enters download mode only when IO2 is high at reset, and on a plain
Ai-M61-32S that pin is a test point on the module's back, out of reach once it is soldered
down. But boot2 -- Ai-Thinker's factory one and the SDK's `boot2_*_isp_release` alike --
listens on the UART for a few milliseconds after every reset, and answers a burst of 0x55 at
2 Mbaud with `Boot2 ISP Ready` and `OK`. It then waits for the flasher's commands.

BLFlashCommand has this mode too (`boot2_isp_mode = 1`), but it waits about seven seconds
for the reset and then gives up, and a second press of the button resets the chip out of
the mode again. So this sends the bursts for as long as it is told to, stops at the first
`OK`, and `flash-bl618.sh --isp` runs the flasher straight after.

The burst is what the flasher sends: 6 ms of 0x55, a short listen, again. Sparser than that
misses the window more often than it hits it.

Needs a USB-serial chip that really does 2 Mbaud (CH340C, CH343, FT232 do).
"""
import os
import select
import struct
import sys
import termios
import time

BAUD = 2000000


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                           # iflag: raw
    attrs[1] = 0                                           # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                           # lflag
    if sys.platform == "darwin":
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        import fcntl
        fcntl.ioctl(fd, 0x80085402, struct.pack("L", BAUD))    # IOSSIOSPEED: any baud rate
    else:
        attrs[4] = attrs[5] = termios.B2000000
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: bl618-isp-handshake.py PORT [SECONDS]")
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    fd = open_port(sys.argv[1])
    burst = b"\x55" * (BAUD // 10 * 6 // 1000)             # 6 ms on the wire
    seen = b""
    print("sending the handshake for %.0f s: press reset once, and only once" % seconds, flush=True)
    end = time.time() + seconds
    while time.time() < end:
        try:
            os.write(fd, burst)
        except BlockingIOError:
            pass
        if select.select([fd], [], [], 0.004)[0]:
            try:
                seen = (seen + os.read(fd, 4096))[-65536:]
            except BlockingIOError:
                pass
        if b"OK" in seen:
            print("boot2 answered: in download mode")
            return 0
    print("no answer in %.0f s (%s)" % (seconds, "the port was silent: was reset pressed?" if not seen
                                        else "the chip restarted but boot2 did not answer: press reset again"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
