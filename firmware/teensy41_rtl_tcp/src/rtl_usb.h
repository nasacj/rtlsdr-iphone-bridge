// SPDX-License-Identifier: GPL-2.0-or-later
//
// The RTL2832U on the Teensy 4.1's USB host port, as a USBHost_t36 driver.
//
// Two jobs. **Control transfers for librtlsdr**, made synchronous: every register and I2C
// access goes through libusb_control_transfer, which the shim in rtl_usb.cpp turns into a
// queued control transfer and a wait. **Streaming the bulk-IN endpoint** with several
// 16 KiB transfers queued back to back, re-queued from the completion interrupt, so the
// host controller never runs out of somewhere to put samples (see block_ring.h).
//
// Two USBHost_t36 behaviours this has to work around, both read out of its source:
//
// * A control transfer that STALLs is reported to the control pipe's error callback, which
//   USBHost_t36 sets to enumeration_error — that treats it as an enumeration failure and
//   re-sends an enumeration request, and the driver never hears of it. librtlsdr's tuner
//   probe STALLs on every I2C address with no tuner behind it, so this happens on every
//   open. The driver installs its own error callback and passes anything that is not its
//   own transfer on to enumeration_error.
// * An error on a bulk pipe removes *every* transfer queued on it. The driver's bulk error
//   callback therefore treats all queued buffers as gone and queues fresh ones.

#pragma once
#include <Arduino.h>
#include <USBHost_t36.h>

#include "block_ring.h"

class RtlUsb : public USBDriver {
public:
  static constexpr size_t kBlockSize = 16384;  // one qTD, librtlsdr's own default buffer size
  static constexpr size_t kBlocks = 20;        // 320 KiB of DMAMEM
  static constexpr size_t kQueued = 6;         // transfers kept with the controller at once

  struct Stats {
    uint32_t filled;
    uint32_t dropped;
    uint32_t starved;
    uint32_t bulk_errors;
    uint32_t queue_failures;
    uint64_t bytes_in;
  };

  explicit RtlUsb(USBHost &) { init(); }

  // True once a dongle is claimed and enumeration has finished, so control transfers may
  // be made. Main loop only.
  bool ready() const;
  // True once after the dongle was unplugged; the caller then closes librtlsdr.
  bool take_lost();

  // Synchronous control transfer. Returns bytes transferred, or a negative LIBUSB_ERROR_*.
  // Never from an interrupt or a USBHost_t36 callback.
  int control_sync(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                   uint8_t *data, uint16_t length, uint32_t timeout_ms);

  // Bulk streaming. `start` discards any filled blocks and queues transfers; `stop` stops
  // re-queueing, and what is already queued drains into buffers nobody will send.
  void start_streaming();
  void stop_streaming();
  bool streaming() const { return streaming_; }

  // The oldest filled block, now owned by the caller until `release_block`. Returns the
  // block index, or -1 when nothing is waiting.
  int take_block(const uint8_t **data, uint32_t *length);
  void release_block(int index);

  Stats stats();
  uint16_t vid() const { return vid_; }
  uint16_t pid() const { return pid_; }

protected:
  bool claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) override;
  void control(const Transfer_t *transfer) override;
  void disconnect() override;

private:
  void init();
  void queue_more();  // interrupts off, or in the USB interrupt
  static void bulk_done(const Transfer_t *transfer);
  static void bulk_error(const Transfer_t *transfer);
  static void control_error(const Transfer_t *transfer);

  enum ControlState : uint8_t { Idle, Pending, Done, Failed };

  Pipe_t pipes_[2] __attribute__((aligned(32)));
  Transfer_t transfers_[kQueued + 12] __attribute__((aligned(32)));
  strbuf_t strbufs_[1];

  Pipe_t *volatile bulk_pipe_ = nullptr;
  Pipe_t *control_pipe_ = nullptr;
  volatile bool claimed_ = false;
  volatile bool lost_ = false;
  volatile bool streaming_ = false;
  volatile uint8_t control_state_ = Idle;
  volatile uint32_t bulk_errors_ = 0;
  volatile uint32_t queue_failures_ = 0;
  uint16_t vid_ = 0, pid_ = 0;

  setup_t setup_;
  uint8_t control_buf_[256] __attribute__((aligned(32)));  // DTCM: not cached, DMA-safe

  BlockRing<kBlocks> ring_;
};

extern USBHost myusb;
extern RtlUsb rtlusb;
