# rtlsdr-iphone-bridge

A board that bridges an RTL-SDR to an iPhone. USB host on one side; on the other, a
network link to the phone, which connects to `rtl_tcp` at `192.168.7.1:1234`. Two boards
implement it: **W**, Wi-Fi from a BL618 (`firmware/bl618_rtl_udp/`, the one README.md is
about, and the only one run on hardware), and **E**, a Teensy 4.1's Ethernet port to a
USB-C Ethernet adapter on the phone (`firmware/teensy41_rtl_tcp/`, builds, never run).
The client is an existing iPhone `rtl_tcp` app, which must not need any change for the board.

**Ideas not built are not documented in the repository.** A third route, **U** (the Teensy
as a USB CDC-NCM device on the phone's own port), and a custom PCB were planned and never
started; their write-ups (docs/routes.md, docs/bench-setup.md, docs/teensy-plan.md) were
removed on 2026-11-03 and are in git history before that date. README.md and README.zh-CN.md
are for someone building a W board; each firmware's own README holds its details.

## Settled decisions

- **Route B1: NXP i.MX RT1062**, chosen 2026-10-16 over an Allwinner T113-S3 Linux bridge
  (B2). Both USB ports must be high-speed; that rules out ESP32-P4, STM32H7 (external
  ULPI PHY), RP2040/RP2350 and single-OTG Linux boards.
- **Route E before route U** (2026-10-19). E puts a standard USB-C Ethernet adapter on the
  phone, so the riskiest part of U — whether iOS accepts our own NCM device — stays out of
  the way. E caps at 100 Mbit/s (DP83825I), enough for RTL-SDR, not for HackRF-class rates.
  Every board uses `192.168.7.1`, so the app's settings never change.
- **The protocol is `rtl_tcp`, unchanged.** The 12-byte `RTL0` header, raw unsigned 8-bit I/Q,
  and 5-byte commands. Anything new goes on a different port, never into this stream.
- **GPL is allowed here and only here.** Both firmwares vendor librtlsdr (GPL-2.0-or-later):
  the BL618 firmware is **GPL-2.0-or-later** (LICENSE at the root), and the Teensy firmware,
  which also links QNEthernet (AGPL-3.0-or-later), is **AGPL-3.0-or-later**. None of it may ever be copied into the phone app, whose rule is that GPL stays off the
  phone.
- **Stack (2026-10-22): Teensyduino + USBHost_t36 + QNEthernet**, built with `arduino-cli`
  from the command line (firmware/README.md). The user develops in VS Code, not an IDE. This
  replaced "TinyUSB + MCUXpresso SDK" after reading TinyUSB's EHCI: **it allows one transfer
  per endpoint** (`usbh_edpt_claim`) and completes it from `tuh_task` in the main loop, so the
  bulk-IN endpoint idles between every 16 KiB. The RTL2832U's FIFO lasts well under a
  millisecond at 2.4 MSPS — librtlsdr keeps 15 transfers in flight on a PC for this reason —
  so that design loses samples. USBHost_t36 chains qTDs on one QH and completes in the
  interrupt, where the next one is queued. Do not move back without solving that.
- **Two USBHost_t36 behaviours the driver works around, read out of its source.** A STALL on
  the control pipe goes to `enumeration_error`, which re-sends enumeration and never tells the
  driver — and librtlsdr's tuner probe STALLs on every absent tuner address. And an error on a
  bulk pipe removes every transfer queued on it. See `rtl_usb.h`.
- **QNEthernet's `TCP_SND_BUF` is 4 × MSS (5.8 KB) and cannot be overridden**, which caps TCP at
  window ÷ RTT — under 4.8 MB/s once the round trip passes about a millisecond. A patch in
  `firmware/patches/` makes it 32 KB; `tools/fetch-deps.sh` applies it to a pinned tag.

- **Route W (2026-10-27): Wi-Fi from a BL618, no phone-side hardware at all.** An Ai-Thinker
  Ai-M61-32S (BL618: USB 2.0 HS OTG, 2.4 GHz Wi-Fi 6, 4 MiB PSRAM) is USB host and Wi-Fi
  station or AP at once. The ESP32-P4 and single-OTG exclusions above were about the *USB*
  routes, where both ports must be high-speed; here the phone side is radio. Firmware is
  `firmware/bl618_rtl_udp/` on the Bouffalo SDK (pinned in `tools/fetch-deps.sh`) with
  CherryUSB, sharing `common/librtlsdr/` and its `libusb.h` with the Teensy build —
  one copy, two shims. **Builds and links (2026-10-27) on an x86_64 CentOS
  Stream 9 build host (its ssh name in the untracked `firmware/tools/build-host`), with XuanTie GCC v2.6.1. Never run.** v2.6.1 is required, not chosen: the SDK's
  prebuilt Wi-Fi library is GCC 10.2 LTO bytecode and v2.10.2 (GCC 10.4) will not link it —
  measured in an Ubuntu 24.04 container, where our own sources compiled and the link failed. Two things only hardware answers: whether
  2.4 GHz single-stream Wi-Fi sustains 32–38 Mbit/s, and whether the gap below loses samples.
- **Both firmwares share `firmware/common/librtlsdr/` (upstream, unmodified) and its `libusb.h`
  (the interface each board implements), and that is deliberate — copying it per board would
  leave one copy without the next upstream fix.** The cost is that a change there can break
  either board, so **`firmware/tools/build-all.sh` builds both** (Teensy here, BL618 on
  the build host) plus the unit tests, and is run before any such change is called done. Verified
  2026-10-27 when `libusb.h` moved from `<sys/time.h>` to `<sys/_timeval.h>`: the Teensy hex
  came out byte-identical, and the script fails on a deliberate error in either.
  **Layout (2026-10-28): `firmware/teensy41_rtl_tcp/`, `firmware/bl618_rtl_udp/`,
  `firmware/common/librtlsdr/`.** Arduino compiles only the sketch's own folder, so the Teensy
  build takes librtlsdr as a flat library (`--library common/librtlsdr`) and includes
  `"rtl-sdr.h"`/`"libusb.h"` by name. That move changed the hex only in placement: every
  symbol and every loaded section kept its size, checked against a build of the old tree.
- **CherryUSB allows one transfer per endpoint in practice, the same trap as TinyUSB in a new
  form.** Its EHCI port gives every URB its own QH and the data toggle lives in the QH, so two
  URBs on one endpoint keep two toggles and the controller silently discards alternate
  packets. One URB can be large (it is split into 16 KiB qTDs the controller runs back to
  back), so the BL618 driver keeps one 32 KiB URB in flight and re-submits from the
  completion interrupt; the hand-over is the gap. If the counter test shows loss, the answer
  is a resident QH with a ring of qTDs re-armed in the interrupt, as USBHost_t36 and Linux do.
- **The board's own network has a per-board random password, never one in the source.**
  `rtl_tcp` has no authentication, so the WPA2 key is the only lock, and this repository is to
  be public. A key derived from the chip ID is no better: the ID is in the MAC every beacon
  carries. `ap_credentials.c` draws 12 characters from the TRNG on first boot, keeps them in
  the PSM partition through lfs_kv, and the serial log prints them at every start;
  `BRIDGE_AP_KEY` in wifi_secrets.h overrides, `bridge_ap_newkey` at the shell forgets.
- **Wi-Fi needs a deep buffer where Ethernet did not**: 1.75 MiB of the PSRAM behind a 1 MiB guard
  over 0xa8201300, which Wi-Fi hardware zeroes every few minutes (bouffalo_sdk#272, see
  firmware/bl618_rtl_udp/README.md); whole blocks
  dropped and counted when it fills, so a stall is a delay rather than a gap or a swapped I/Q.
- **The BL618 runs its code from PSRAM** (`PSRAM_COPY_CODE`), and that is what made 2 MSPS
  fit. From flash, through a 32 KiB instruction cache the Wi-Fi transmit path does not fit
  in, `udp_sendto` cost ~420 us of CPU a datagram and the board peaked near 1.4 MSPS with the
  CPU saturated; from PSRAM it is ~100 us. Measured 2026-10-30, after two wrong guesses — the
  lwIP heap (real, but worth 15%) and airtime without aggregation (the CPU was at 93%).
  Per-task CPU shares are in the status line; read them before guessing the next limit.
- **The BL618 serves rtl_udp on 1235 beside rtl_tcp on 1234.** Format at the top of
  `rtl_tcp_server.c`; the ring's gap queue is what puts each loss in the right place, and a
  full queue drops the next block rather than announcing a gap late.

## Measurement rules

- **The instrument is the counter.** RTL2832U test mode emits an incrementing 8-bit counter,
  so every lost byte is countable end to end. `firmware/tools/rtl_tcp_counter.py` checks it, breaks and delivered rate both. Never judge
  throughput by whether audio sounds right.
- **Read the delivered rate, not the requested one.** A session running well under nominal
  is broken, not lossy.
- **`rtl_tcp` has no flow control.** When the phone stalls, drop whole blocks, count them,
  and expose the count. Never corrupt the stream.
- **Do not disturb an `rtl_tcp` already running on port 1234 on the development machine.**
  Use another port for any mock.

## This repository is public

Write every tracked file, and every commit message, as public: no personal details, hostnames,
network names, or anything from `wifi_secrets.h`. The build host's name lives in the untracked
`firmware/tools/build-host` (or `BL618_HOST`), and the docs say "an iPhone `rtl_tcp` app"
rather than naming one. Each borrowed piece keeps its notice: librtlsdr (PROVENANCE.md and
COPYING), the SDK-derived files in `firmware/bl618_rtl_udp/config/` (CherryUSB Apache-2.0,
lwIP BSD, FreeRTOS MIT), and the QNEthernet patch. Licences: GPL-2.0-or-later for the BL618
firmware (LICENSE at the root), AGPL-3.0-or-later for the Teensy one
(`firmware/teensy41_rtl_tcp/LICENSE`).

## Working agreements

- Look up how others solved a problem before designing from scratch, and read a reference
  whole before reading parts of it. Candidates: USBHost_t36 and QNEthernet (in
  `docs/reference/`), TinyUSB examples, librtlsdr,
  `esp-rtl-sdr` (AGPL: read only, never copy).
- Commit and push only when asked.
