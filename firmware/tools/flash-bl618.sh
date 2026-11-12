#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Flashes the BL618 firmware from the Mac, where the board is plugged in, using the build on
# the Linux host (there is no x86 macOS compiler, but Bouffalo's flasher does run here).
#
#   firmware/tools/flash-bl618.sh /dev/tty.usbserial-XXXX
#   firmware/tools/flash-bl618.sh --isp /dev/cu.usbmodemXXXX     no boot pin: see below
#
# Before running it: build (tools/build-all.sh), then put the board in download mode —
# hold the boot button (IO2), press and release reset (EN), release boot. The kit has no
# auto-download circuit, so this is by hand every time; press reset again afterwards to run.
#
# What it does: copies the build output and the flash_prog_cfg.ini the SDK generated from
# the partition table (boot2 at 0x0, partition table at 0xE000, firmware at 0x10000) from
# the build host, then runs BLFlashCommand-macos on them. `tools/fetch-deps.sh bl618` on the
# Mac puts the flasher in firmware/deps/bflb_flash_cube.
#
# **--isp is for a board whose boot pin cannot be reached**, which is every plain Ai-M61-32S
# soldered to a board: its IO2 is a test point on the module's back. boot2 answers a
# handshake on the UART just after reset instead (tools/bl618-isp-handshake.py), so the
# whole procedure is: run this, press reset once when asked, and press it again at the end.
# It needs a boot2 already in the flash -- the factory AT firmware has one, and so does
# anything this script has written -- and a serial chip good for 2 Mbaud.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
isp=0
if [ "${1:-}" = "--isp" ]; then isp=1; shift; fi
# The Linux build host: $BL618_HOST, or the one line in tools/build-host (not committed).
host=${BL618_HOST:-$(cat "$here/tools/build-host" 2>/dev/null || true)}
[ -n "$host" ] || { echo "no build host: set BL618_HOST, or put its ssh name in firmware/tools/build-host"; exit 1; }
remote=${BL618_REMOTE:-Projects/rtlsdr-iphone-bridge}
port=${1:?usage: flash-bl618.sh /dev/tty.usbserial-XXXX}
# The Ai-M61-32S-Kit's USB-serial chip is a CH340C (its schematic, v1.1), and the SDK's
# README gives CH340 at 500 kbit/s at most for flashing. A board with a CP2102 or FT232 can
# take BL618_BAUD=2000000.
baud=${BL618_BAUD:-500000}
[ "$isp" = 1 ] && baud=2000000     # boot2 talks at 2 Mbaud whatever the chip on the other end
flasher="$here/deps/bflb_flash_cube/BLFlashCommand-macos"
[ -x "$flasher" ] || { echo "no flasher at $flasher: run tools/fetch-deps.sh bl618"; exit 1; }

out="$here/build/bl618"
rm -rf "$out"
mkdir -p "$out"
rsync -a -e "ssh -o BatchMode=yes -o LogLevel=ERROR" \
  "$host:$remote/firmware/bl618_rtl_udp/build/flash_prog_cfg.ini" \
  "$host:$remote/firmware/bl618_rtl_udp/build/build_out" "$out/"
ls -l "$out/build_out/"*.bin | awk '{print $5, $NF}'

# The ini names its files relative to the build directory.
cd "$out"
if [ "$isp" = 1 ]; then
  sed -i.bak 's/^boot2_isp_mode = 0/boot2_isp_mode = 1/' flash_prog_cfg.ini
  python3 "$here/tools/bl618-isp-handshake.py" "$port" || exit 1
fi
"$flasher" --interface=uart --baudrate="$baud" --port="$port" --chipname=bl616 \
  --config="$out/flash_prog_cfg.ini"
