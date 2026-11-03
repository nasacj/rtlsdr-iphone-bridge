# librtlsdr, vendored

From [rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog) at
`aed0ea19f3a273370a13c9009b96313c75d54c7b` (2026-03-22): `src/librtlsdr.c`, the five tuner
drivers, and the headers they need. GPL-2.0-or-later, see `COPYING`. This fork rather than
osmocom's because it drives the RTL-SDR Blog V4's R828D correctly.

Changes from upstream, all mechanical, none to register or tuner logic:

- `#include <libusb.h>` → `#include "libusb.h"`, so the build picks up `./libusb.h`, the interface
  each board implements: on USBHost_t36 in `../../teensy41_rtl_tcp/src/rtl_usb.cpp`, on
  CherryUSB in `../../bl618_rtl_udp/libusb_shim.c`.
- `#include <rtl-sdr_export.h>`, `<reg_field.h>`, `<tuner_*.h>`, `<rtlsdr_i2c.h>` → quoted,
  because the Arduino build does not put this directory on the system include path.

`tools/fetch-deps.sh` clones the same commit into `firmware/deps/rtl-sdr-blog` so the two can be
diffed: `diff -r firmware/deps/rtl-sdr-blog/src firmware/common/librtlsdr`.
