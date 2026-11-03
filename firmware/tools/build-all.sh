#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds both firmwares, because they share librtlsdr and its libusb.h: any change to
# common/librtlsdr/ can break either board, and a build of one says nothing about
# the other. Run it before calling such a change done.
#
#   1. Teensy 4.1 (route E): arduino-cli, here on the Mac
#   2. the desktop unit tests (block ring, rtl_tcp parser, DHCP)
#   3. BL618 (route W): copied to a Linux x86_64 machine and built there with the Bouffalo
#      SDK — there is no x86 macOS toolchain. Only warnings in our own files fail it; the
#      SDK's and upstream librtlsdr's are theirs.
#
# The Mac copy is the source of truth: step 3 copies it over the remote one (deps/ and
# build output are left alone; nothing is deleted on the remote side).
#
#   firmware/tools/build-all.sh
#   BL618_HOST=my-linux-box firmware/tools/build-all.sh
#
# One-time setup: `tools/fetch-deps.sh` here, `tools/fetch-deps.sh bl618` on the Linux host.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$here/.." && pwd)
# The Linux build host: $BL618_HOST, or the one line in tools/build-host (not committed).
host=${BL618_HOST:-$(cat "$here/tools/build-host" 2>/dev/null || true)}
[ -n "$host" ] || { echo "no build host: set BL618_HOST, or put its ssh name in firmware/tools/build-host"; exit 1; }
remote=${BL618_REMOTE:-Projects/rtlsdr-iphone-bridge}
ssh_opts="-o BatchMode=yes -o LogLevel=ERROR"

step() { printf '\n== %s\n' "$*"; }

step "Teensy 4.1"
if ! out=$(arduino-cli compile --fqbn teensy:avr:teensy41 --libraries "$here/deps/libraries" \
      --library "$here/common/librtlsdr" --build-path "$here/build/teensy41" "$here/teensy41_rtl_tcp" 2>&1); then
  echo "$out" | grep -E 'error:' | head -20
  echo "Teensy build failed"; exit 1
fi
echo "$out" | grep -E 'FLASH|RAM[12]'

step "desktop unit tests"
make -s -C "$here/tests" test

step "BL618 on $host"
# --delete so a renamed or removed file does not live on there: the build host is a mirror of
# this tree. Excluded paths are kept, which protects its toolchains and SDK.
#
# wifi_secrets.h goes too, deliberately: it is compiled in, so the copy here is the one
# that has to be built, and the README says to create it here. Excluding it once meant an
# edit on the Mac never reached the firmware and the board quietly fell back to its own
# network. It is gitignored, so it travels to your build host and nowhere else.
rsync -a --delete -e "ssh $ssh_opts" --exclude 'firmware/deps/' --exclude 'build/' --exclude '.DS_Store' \
  --exclude 'docs/reference/' --exclude 'firmware/bl618_rtl_udp/build.log' \
  "$repo/" "$host:$remote/"
ssh $ssh_opts "$host" "set -e
  F=\$HOME/$remote/firmware
  export PATH=\$F/deps/xuantie-gcc-2.6.1/bin:\$PATH
  cd \$F/bl618_rtl_udp
  rm -rf build
  if ! make -j\$(nproc) > build.log 2>&1; then
    grep -E 'error:' build.log | sed 's/\x1b\[[0-9;]*[mK]//g' | head -20
    echo 'BL618 build failed (build.log on $host)'; exit 1
  fi
  ours=\$(grep 'warning:' build.log | grep -c 'bl618_rtl_udp/' || true)
  ls -l build/build_out/bl618_rtl_udp_bl616.bin | awk '{print \"bin\", \$5, \"bytes\"}'
  echo \"warnings in our files: \$ours\"
  [ \"\$ours\" -eq 0 ]"

printf '\nboth firmwares build\n'
