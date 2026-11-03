// SPDX-License-Identifier: GPL-2.0-or-later
//
// The I/Q waiting for Wi-Fi, in PSRAM.
//
// **Why it exists at all.** Ethernet on the Teensy delivers at a steady rate, so a few
// hundred kilobytes between USB and TCP were enough. Wi-Fi does not: a retransmission, a
// beacon from a busy neighbour or the phone's own power saving holds the link for tens to
// hundreds of milliseconds. At 2.4 MSPS that is 4.8 MB a second arriving regardless, and
// the BL618's 532 KiB of SRAM is shared with lwIP and the Wi-Fi stack. The Ai-M61 module's
// 4 MiB of PSRAM is what turns a stall into a delay rather than a gap. 3 MiB by default;
// 1.75 MiB in the shipped build (CMakeLists.txt), behind a 1 MiB guard, where the code runs
// from PSRAM too — 0.38 s at 2.4 MSPS and 0.9 s at 1.024.
//
// **Whole blocks or nothing.** A block that does not fit is dropped entire and counted, so
// the stream stays an exact copy of what the dongle sent minus counted gaps — and, since
// every block is a whole number of I/Q pairs, I and Q never swap. `rtl_tcp` has no flow
// control and a stalled reader must not corrupt what it eventually gets.
//
// **And where each gap fell.** rtl_udp numbers every sample it sends, so a gap has to land
// in the numbering exactly where it happened in the stream, or the client fills zeros in
// the wrong place and the time base slips anyway. The producer therefore queues "N samples
// missing before ring position P" when its next block does fit, and the consumer, peeking,
// is told the gap as it reaches P and is never handed a run that crosses one. rtl_tcp has
// nowhere to say any of this and simply has the events skipped for it.
//
// One producer (the copy task, or the synthetic counter) and one consumer (the TCP task),
// so the indices need no lock: each is written by one side only, with a barrier between
// the data and the index that publishes it.

#ifndef SAMPLE_RING_H
#define SAMPLE_RING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SAMPLE_RING_BYTES
#define SAMPLE_RING_BYTES (3u * 1024u * 1024u)
#endif

// Producer. Returns false, and counts a drop, when `length` bytes do not fit.
bool sample_ring_push(const uint8_t *data, uint32_t length);

// Producer: `length` bytes the source lost before the next block, so the gap lands there
// exactly as a dropped block's does. Not counted as a drop: the ring did not drop them.
void sample_ring_skip(uint32_t length);

// Consumer: the longest contiguous run of waiting bytes that does not cross a gap, then
// how many were sent. When the next byte is the first after a gap, `*gap_samples` (if
// given) is how many I/Q pairs went missing there, once; otherwise 0.
uint32_t sample_ring_peek(const uint8_t **data, uint32_t *gap_samples);
void sample_ring_consume(uint32_t length);

// Consumer, at the start of a session: forget everything waiting, gaps included.
void sample_ring_clear(void);

// The guard in front of the ring (sample_ring.c says why): fill once at start, then check.
// The check returns how many bytes no longer hold the pattern, the first one's offset and
// value, and restores them, so each call reports only what was written since the last.
void sample_ring_guard_fill(void);
uint32_t sample_ring_guard_check(uint32_t *first, uint8_t *value);
// Where the word Wi-Fi zeroes falls: 1 inside the guard (as it should), -1 inside the ring
// (samples there will be corrupted), 0 outside both.
int sample_ring_wifi_word_placement(void);

uint32_t sample_ring_used(void);
uint32_t sample_ring_dropped_blocks(void);
uint32_t sample_ring_high_water(void);

#ifdef __cplusplus
}
#endif

#endif
