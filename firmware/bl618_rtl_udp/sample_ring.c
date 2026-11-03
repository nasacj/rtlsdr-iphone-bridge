// SPDX-License-Identifier: GPL-2.0-or-later

#include "sample_ring.h"

#include <string.h>

// NOLOAD in PSRAM: three megabytes of zeros in the image would be three megabytes to
// flash for nothing. Only on the board; the host tests build this file too.
#if defined(__riscv)
#define RING_PLACEMENT __attribute__((section(".psram_noinit"), aligned(64)))
#else
#define RING_PLACEMENT __attribute__((aligned(64)))
#endif

// **A guard in front of the ring, because Wi-Fi zeroes one fixed word of PSRAM.**
// While Wi-Fi runs, the aligned 32-bit word at 0xa8201300 is written with zero every one to
// four minutes, idle or not: in the counter test that was 4 bytes of samples gone about
// every ten minutes, silently, since the stream around them was intact. Measured on
// 2026-11-03, step by step: the address stayed the same through five memory layouts; it is
// not in the image as a constant; a hardware trigger on it, proven by a self-test, never
// fired, so no CPU store from a task wrote it; the word did not change during any
// interrupt handler; it happened with no RTL-SDR plugged in; and it stopped entirely with
// Wi-Fi not started. So the writer is very likely the Wi-Fi hardware's DMA, inside the
// prebuilt library, and not fixable here. What is fixable is what lies at that address:
// the guard, filled with a pattern and checked (sample_ring_guard_check), so the write
// costs nothing and is still reported. One variable holds guard then ring, so their order
// cannot change; CMakeLists.txt sizes the guard to cover the address.
#define SAMPLE_RING_WIFI_WORD 0xa8201300u
#ifndef SAMPLE_RING_GUARD
#define SAMPLE_RING_GUARD (16u * 1024u)
#endif
#define GUARD_PATTERN 0xA5u
RING_PLACEMENT static struct {
    uint8_t guard[SAMPLE_RING_GUARD];
    uint8_t ring[SAMPLE_RING_BYTES];
} g_memory;
#define g_ring g_memory.ring

// Positions run over [0, 2 * size), so full and empty are told apart without a spare
// byte. **Not free-running counters taken modulo the size**: 3 MiB does not divide 2^32,
// so a counter that wrapped — after about fifteen minutes at 4.8 MB/s — would jump to the
// wrong place in the buffer. Each is written by one side only.
#define SPAN (2u * SAMPLE_RING_BYTES)

static volatile uint32_t g_head;  // producer
static volatile uint32_t g_tail;  // consumer
static volatile uint32_t g_dropped;
static volatile uint32_t g_high_water;

// Gaps. A queue of its own, single producer and single consumer like the ring. Its
// positions are ring positions, which cannot be lapped while an event is waiting: the
// consumer cannot pass one, and the ring holds less than a span.
#ifndef SAMPLE_RING_GAPS
#define SAMPLE_RING_GAPS 256u
#endif
#define GAPS SAMPLE_RING_GAPS
struct gap {
    uint32_t at;       // ring position of the first byte after the gap
    uint32_t samples;  // I/Q pairs missing there
};
static struct gap g_gaps[GAPS];
static volatile uint32_t g_gap_head;  // producer
static volatile uint32_t g_gap_tail;  // consumer
static uint32_t g_pending_gap;        // producer: dropped since the last block that fitted

static inline void barrier(void) { __sync_synchronize(); }
static inline uint32_t used_between(uint32_t head, uint32_t tail) { return (head + SPAN - tail) % SPAN; }

bool sample_ring_push(const uint8_t *data, uint32_t length)
{
    uint32_t head = g_head;
    uint32_t used = used_between(head, g_tail);
    // **A block that would follow a gap the queue has no place for is dropped too.** The
    // first version kept such a gap pending and announced it with a later block, on the
    // reasoning that the total stays right and only its position coarsens — and position
    // is the whole point: every byte between where the gap happened and where it was
    // announced reached the client numbered too early. On the board, the counter with
    // 8 KiB blocks behind slow Wi-Fi had half its datagrams wrong. Dropping more keeps
    // every gap exact; the queue frees a place as the consumer reaches the oldest.
    bool gap_queue_full = g_gap_head - g_gap_tail >= GAPS;
    if (length > SAMPLE_RING_BYTES - used || (g_pending_gap != 0 && gap_queue_full)) {
        g_dropped++;
        g_pending_gap += length / 2;
        return false;
    }
    // Announce the gap at the position this block starts.
    if (g_pending_gap != 0) {
        g_gaps[g_gap_head % GAPS] = (struct gap){ head, g_pending_gap };
        barrier();
        g_gap_head++;
        g_pending_gap = 0;
    }
    uint32_t at = head % SAMPLE_RING_BYTES;
    uint32_t first = SAMPLE_RING_BYTES - at;
    if (first > length) {
        first = length;
    }
    memcpy(&g_ring[at], data, first);
    memcpy(&g_ring[0], data + first, length - first);
    barrier();
    g_head = (head + length) % SPAN;
    if (used + length > g_high_water) {
        g_high_water = used + length;
    }
    return true;
}

void sample_ring_skip(uint32_t length) { g_pending_gap += length / 2; }

uint32_t sample_ring_peek(const uint8_t **data, uint32_t *gap_samples)
{
    uint32_t tail = g_tail;
    uint32_t used = used_between(g_head, tail);
    barrier();
    uint32_t gap = 0;
    uint32_t limit = used;
    while (g_gap_tail != g_gap_head) {
        struct gap next = g_gaps[g_gap_tail % GAPS];
        uint32_t distance = (next.at + SPAN - tail) % SPAN;
        if (distance == 0) {
            gap += next.samples;
            g_gap_tail++;
            continue;
        }
        if (distance < limit) {
            limit = distance;
        }
        break;
    }
    if (gap_samples) {
        *gap_samples = gap;
    }
    uint32_t at = tail % SAMPLE_RING_BYTES;
    uint32_t run = SAMPLE_RING_BYTES - at;
    *data = &g_ring[at];
    return limit < run ? limit : run;
}

void sample_ring_consume(uint32_t length)
{
    barrier();
    g_tail = (g_tail + length) % SPAN;
}

void sample_ring_clear(void)
{
    barrier();
    g_tail = g_head;
    // Gaps already announced lie in what was just discarded. One the producer announces
    // after this belongs to data after it and is kept.
    g_gap_tail = g_gap_head;
}

void sample_ring_guard_fill(void) { memset(g_memory.guard, GUARD_PATTERN, sizeof g_memory.guard); }

int sample_ring_wifi_word_placement(void)
{
    uintptr_t word = SAMPLE_RING_WIFI_WORD;
    uintptr_t guard = (uintptr_t)g_memory.guard, ring = (uintptr_t)g_memory.ring;
    if (word >= guard && word + 4 <= guard + sizeof g_memory.guard) {
        return 1;
    }
    if (word + 4 > ring && word < ring + sizeof g_memory.ring) {
        return -1;
    }
    return 0;
}

uint32_t sample_ring_guard_check(uint32_t *first, uint8_t *value)
{
    uint32_t changed = 0;
    for (uint32_t i = 0; i < sizeof g_memory.guard; i++) {
        if (g_memory.guard[i] != GUARD_PATTERN) {
            if (changed == 0) {
                *first = i;
                *value = g_memory.guard[i];
            }
            changed++;
            g_memory.guard[i] = GUARD_PATTERN;  // so the next check reports only new writes
        }
    }
    return changed;
}

uint32_t sample_ring_used(void) { return used_between(g_head, g_tail); }
uint32_t sample_ring_dropped_blocks(void) { return g_dropped; }
uint32_t sample_ring_high_water(void) { return g_high_water; }
