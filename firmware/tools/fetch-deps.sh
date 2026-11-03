#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetches the pinned firmware dependencies into firmware/deps (not committed) and applies
# our lwIP settings to QNEthernet. Run once, and again after changing a pin.
#
#   fetch-deps.sh          the Teensy's (QNEthernet, rtl-sdr-blog)
#   fetch-deps.sh bl618    those plus the BL618's. On Linux x86_64, the build host: the
#                          Bouffalo SDK (2 GB) and XuanTie GCC. On a Mac, where the board is
#                          plugged in: only the SDK's flasher, plus newlib headers for
#                          tools/check-bl618.sh.
#
#   QNEthernet  AGPL-3.0-or-later  Ethernet + lwIP for the Teensy 4.1
#   rtl-sdr-blog                   librtlsdr reference copy (the build uses the vendored
#                                  copy in common/librtlsdr; this is for diffing)
#   bouffalo_sdk Apache-2.0        the BL618 board's SDK: FreeRTOS, lwIP, Wi-Fi, CherryUSB
#   xuantie-gcc  GPL (the compiler) Bouffalo's T-HEAD RISC-V GCC for the BL618, Linux x86_64
#   newlib       BSD-style         headers only, for tools/check-bl618.sh on a Mac
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
deps="$here/deps"
mkdir -p "$deps/libraries"

QNETHERNET_TAG=v0.37.0
RTLSDR_BLOG_COMMIT=aed0ea19f3a273370a13c9009b96313c75d54c7b
BOUFFALO_SDK_COMMIT=5fce0c8e895f22e247d5071318aa8d01cec2cb40  # 2026-10-27, SDK 2.3.37
NEWLIB=newlib-4.4.0.20231231
# v2.6.1, not the newer v2.10.2 beside it, and not by preference: the SDK's prebuilt Wi-Fi
# library (components/wireless/macsw/lib/libmacsw_bl616.a) is LTO bytecode from GCC 10.2,
# and v2.10.2's GCC 10.4 refuses to link it ("LTO version 9.0 instead of the expected
# 9.4"). Measured 2026-10-27 in an Ubuntu 24.04 container — our own sources compile with
# v2.10.2, the link does not. v2.10.2 also needs glibc 2.36, which CentOS Stream 9 lacks.
XUANTIE_GCC=xuantie-gcc-riscv-v2.6.1-linux-x86_64

q="$deps/libraries/QNEthernet"
if [ ! -d "$q" ]; then
  git clone -q --branch "$QNETHERNET_TAG" --depth 1 https://github.com/ssilverman/QNEthernet.git "$q"
fi
(cd "$q" && git checkout -q "$QNETHERNET_TAG" && git checkout -q -- src/lwipopts.h && \
  git apply "$here/patches/qnethernet-lwipopts.patch")
echo "QNEthernet $QNETHERNET_TAG, lwipopts patched"

r="$deps/rtl-sdr-blog"
if [ ! -d "$r" ]; then git clone -q https://github.com/rtlsdrblog/rtl-sdr-blog.git "$r"; fi
(cd "$r" && git fetch -q --depth 1 origin "$RTLSDR_BLOG_COMMIT" 2>/dev/null || true; git checkout -q "$RTLSDR_BLOG_COMMIT")
echo "rtl-sdr-blog $RTLSDR_BLOG_COMMIT"

[ "${1:-}" = bl618 ] || exit 0

if [ "$(uname -s)" = Darwin ]; then
  # A Mac flashes but does not build (there is no x86 macOS compiler): only the flasher,
  # from the same SDK commit, without the other 2 GB. tools/flash-bl618.sh runs it.
  f="$deps/bflb_flash_cube"
  if [ ! -x "$f/BLFlashCommand-macos" ]; then
    tmp="$deps/.bouffalo_sdk_flasher"
    rm -rf "$tmp"
    git clone -q --filter=blob:none --no-checkout https://github.com/bouffalolab/bouffalo_sdk.git "$tmp"
    (cd "$tmp" && git sparse-checkout set --no-cone /tools/bflb_tools/bouffalo_flash_cube/ && \
      git checkout -q "$BOUFFALO_SDK_COMMIT")
    src="$tmp/tools/bflb_tools/bouffalo_flash_cube"
    mkdir -p "$f"
    cp -R "$src/BLFlashCommand-macos" "$src/chips" "$src/utils" "$src/version_info.txt" "$f/"
    rm -rf "$tmp"
  fi
  echo "bflb flasher (macOS) from bouffalo_sdk $BOUFFALO_SDK_COMMIT"
else
  b="$deps/bouffalo_sdk"
  if [ ! -d "$b" ]; then git clone -q https://github.com/bouffalolab/bouffalo_sdk.git "$b"; fi
  (cd "$b" && git fetch -q --depth 1 origin "$BOUFFALO_SDK_COMMIT" 2>/dev/null || true; git checkout -q "$BOUFFALO_SDK_COMMIT")
  echo "bouffalo_sdk $BOUFFALO_SDK_COMMIT"
fi

n="$deps/newlib"
if [ ! -d "$n" ]; then
  curl -sSL "https://sourceware.org/pub/newlib/$NEWLIB.tar.gz" | tar xz -C "$deps" "$NEWLIB/newlib/libc/include"
  mv "$deps/$NEWLIB" "$n"
fi
echo "newlib headers ($NEWLIB)"

if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ]; then
  t="$deps/xuantie-gcc-2.6.1"
  if [ ! -d "$t" ]; then
    mkdir -p "$t.tmp"
    curl -sSL "https://github.com/bouffalolab/toolchain/releases/download/default/$XUANTIE_GCC.tar.xz" | tar xJ -C "$t.tmp"
    mv "$t.tmp"/* "$t" && rmdir "$t.tmp"
  fi
  echo "xuantie-gcc: $("$t/bin/riscv64-unknown-elf-gcc" --version | head -1)"
fi
