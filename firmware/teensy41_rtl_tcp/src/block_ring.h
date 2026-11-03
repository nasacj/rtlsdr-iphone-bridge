// SPDX-License-Identifier: GPL-2.0-or-later
//
// The I/Q buffers between the USB host and the TCP connection.
//
// **USB must never wait for the network.** The RTL2832U's own FIFO holds well under a
// millisecond at 2.4 MSPS, so a bulk-IN endpoint left without a queued transfer loses
// samples inside the dongle, where nothing can count them. So the USB side always gets a
// buffer: a free one if there is one, otherwise the oldest filled one that the network has
// not started sending — and that block is dropped and counted. `rtl_tcp` has no flow
// control, and dropping whole blocks keeps the stream an exact copy of what the dongle
// sent, minus counted gaps, which is what the client's test-mode counter check can see.
//
// Each buffer is in exactly one state:
//
//   free ──queue──▶ queued ──usb done──▶ filled ──network takes it──▶ sending ──sent──▶ free
//                     ▲                    │
//                     └──── stolen (drop) ─┘
//
// The interrupt-side calls are `completed` and `next_to_queue`; the network side is
// `oldest_filled`, `begin_sending`, `finish_sending` and the session resets. On the
// Teensy the network side holds interrupts off around each call (the owner does that);
// the class itself is plain logic so that it can be tested on a desktop.

#pragma once
#include <stddef.h>
#include <stdint.h>

template <size_t N>
class BlockRing {
public:
  enum State : uint8_t { Free, Queued, Filled, Sending };

  struct Stats {
    uint32_t filled = 0;    // blocks the USB side completed
    uint32_t dropped = 0;   // filled blocks reused before the network took them
    uint32_t starved = 0;   // times the USB side found nothing at all to queue
    uint64_t bytes_in = 0;  // bytes the dongle delivered
  };

  BlockRing() { reset_all(); }

  // Everything back to free, including queued buffers. Only safe when no USB transfer
  // can still complete into them: at start-up, and after the device is gone.
  void reset_all() {
    for (size_t i = 0; i < N; i++) { state_[i] = Free; length_[i] = 0; order_[i] = 0; }
    next_order_ = 1;
    stats_ = Stats();
  }

  // A new session must not be handed the previous session's samples. Queued buffers stay
  // queued, because their transfers are still owned by the USB controller.
  void discard_filled() {
    for (size_t i = 0; i < N; i++) {
      if (state_[i] == Filled || state_[i] == Sending) { state_[i] = Free; length_[i] = 0; }
    }
    stats_ = Stats();
  }

  // A buffer for the USB side to queue, or -1. Prefers a free buffer; failing that takes
  // the oldest filled one and counts the drop. Never takes a buffer being sent.
  int next_to_queue() {
    for (size_t i = 0; i < N; i++) {
      if (state_[i] == Free) { state_[i] = Queued; return (int)i; }
    }
    int oldest = oldest_filled();
    if (oldest >= 0) {
      state_[oldest] = Queued;
      length_[oldest] = 0;
      stats_.dropped++;
      return oldest;
    }
    stats_.starved++;
    return -1;
  }

  // The USB side finished a transfer into buffer i with `length` bytes.
  void completed(int i, uint32_t length) {
    if (i < 0 || (size_t)i >= N || state_[i] != Queued) return;
    state_[i] = Filled;
    length_[i] = length;
    order_[i] = next_order_++;
    stats_.filled++;
    stats_.bytes_in += length;
  }

  // A transfer that will never complete into buffer i (stream stopped, device gone).
  void abandon(int i) {
    if (i >= 0 && (size_t)i < N && state_[i] == Queued) state_[i] = Free;
  }

  // The filled buffer that arrived first, or -1. Order is by completion, which is the
  // order the dongle produced the samples in.
  int oldest_filled() const {
    int best = -1;
    for (size_t i = 0; i < N; i++) {
      if (state_[i] == Filled && (best < 0 || order_[i] < order_[best])) best = (int)i;
    }
    return best;
  }

  void begin_sending(int i) { if (i >= 0 && (size_t)i < N && state_[i] == Filled) state_[i] = Sending; }
  void finish_sending(int i) { if (i >= 0 && (size_t)i < N && state_[i] == Sending) { state_[i] = Free; length_[i] = 0; } }

  // How many buffers the USB controller currently owns.
  size_t queued() const { size_t n = 0; for (size_t i = 0; i < N; i++) n += state_[i] == Queued; return n; }
  size_t count(State s) const { size_t n = 0; for (size_t i = 0; i < N; i++) n += state_[i] == s; return n; }

  State state(int i) const { return state_[i]; }
  uint32_t length(int i) const { return length_[i]; }
  const Stats &stats() const { return stats_; }

private:
  State state_[N];
  uint32_t length_[N];
  uint32_t order_[N];
  uint32_t next_order_;
  Stats stats_;
};
