// SPDX-License-Identifier: GPL-2.0-or-later
// Desktop tests for the parts of the firmware that are plain logic: the rtl_tcp parser,
// the block ring between USB and TCP, the DHCP server, and the BL618's sample ring with
// its gap queue. Build and run with `make` here.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../teensy41_rtl_tcp/src/block_ring.h"
#include "../teensy41_rtl_tcp/src/dhcp_server.h"
#include "../teensy41_rtl_tcp/src/rtltcp_proto.h"
#include "../bl618_rtl_udp/sample_ring.h"  // built with SAMPLE_RING_BYTES=4096, see Makefile

static int failures = 0;
#define CHECK(cond)                                                               \
  do {                                                                            \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } \
  } while (0)

// --- rtl_tcp -----------------------------------------------------------------

static void test_header() {
  uint8_t h[rtltcp::kHeaderSize];
  rtltcp::make_header(h, 6, 29);  // R828D, 29 gains
  const uint8_t want[] = {'R', 'T', 'L', '0', 0, 0, 0, 6, 0, 0, 0, 29};
  CHECK(std::memcmp(h, want, sizeof want) == 0);
}

// Commands arrive as a byte stream: split anywhere, or several at once.
static void test_parser_split_and_batched() {
  rtltcp::Parser p;
  rtltcp::Command c{};
  // 0x01 freq 100.1 MHz = 0x05F7_67A0, delivered one byte at a time.
  const uint8_t freq[] = {0x01, 0x05, 0xF7, 0x67, 0xA0};
  for (int i = 0; i < 4; i++) CHECK(!p.feed(freq[i], c));
  CHECK(p.feed(freq[4], c));
  CHECK(c.opcode == rtltcp::SetFrequency && c.param == 100100000u);

  // two commands in one read, the second split across the end of it
  const uint8_t batch[] = {0x02, 0x00, 0x0F, 0xA0, 0x00, 0x0e, 0x00};
  std::vector<rtltcp::Command> got;
  for (uint8_t b : batch) if (p.feed(b, c)) got.push_back(c);
  CHECK(got.size() == 1 && got[0].opcode == rtltcp::SetSampleRate && got[0].param == 1024000u);
  CHECK(p.pending() == 2);
  const uint8_t rest[] = {0x00, 0x00, 0x01};
  for (uint8_t b : rest) if (p.feed(b, c)) got.push_back(c);
  CHECK(got.size() == 2 && got[1].opcode == rtltcp::SetBiasTee && got[1].param == 1);
}

static void test_gain_by_index_clamps() {
  const int gains[] = {0, 9, 14, 27};
  CHECK(rtltcp::gain_for_index(gains, 4, 2) == 14);
  CHECK(rtltcp::gain_for_index(gains, 4, 99) == 27);  // clamped, as rtl_tcp.c does
  CHECK(rtltcp::gain_for_index(nullptr, 0, 1) == 0);
}

// --- block ring --------------------------------------------------------------

// The USB side always gets a buffer while the network is stuck, by dropping the oldest
// filled block, and samples keep their order.
static void test_ring_never_starves_usb() {
  BlockRing<4> r;
  int q[4];
  for (int i = 0; i < 4; i++) q[i] = r.next_to_queue();
  CHECK(r.queued() == 4);
  for (int i = 0; i < 4; i++) r.completed(q[i], 16384);
  CHECK(r.count(BlockRing<4>::Filled) == 4);
  // the network has taken nothing; the USB side asks again and must still get one
  int stolen = r.next_to_queue();
  CHECK(stolen == q[0]);                 // the oldest goes first
  CHECK(r.stats().dropped == 1);
  CHECK(r.oldest_filled() == q[1]);      // the stream continues with the next block
}

static void test_ring_never_steals_what_is_being_sent() {
  BlockRing<2> r;
  int a = r.next_to_queue(), b = r.next_to_queue();
  r.completed(a, 100);
  r.completed(b, 100);
  r.begin_sending(a);
  int got = r.next_to_queue();
  CHECK(got == b);                       // a is half sent; b is the one to drop
  CHECK(r.next_to_queue() == -1);        // nothing left: counted, never corrupting a
  CHECK(r.stats().starved == 1);
  r.finish_sending(a);
  CHECK(r.state(a) == BlockRing<2>::Free);
}

static void test_ring_new_session_starts_clean() {
  BlockRing<3> r;
  int a = r.next_to_queue(), b = r.next_to_queue();
  r.completed(a, 10);
  r.discard_filled();
  CHECK(r.state(a) == BlockRing<3>::Free);    // last session's samples are gone
  CHECK(r.state(b) == BlockRing<3>::Queued);  // the controller still owns b
  CHECK(r.stats().filled == 0);
}

// --- DHCP ------------------------------------------------------------------------

static std::vector<uint8_t> dhcp_request(uint8_t type, const uint8_t mac[6], const uint8_t *requested = nullptr,
                                         const uint8_t *server = nullptr) {
  std::vector<uint8_t> p(300, 0);
  p[0] = 1; p[1] = 1; p[2] = 6;
  p[4] = 0xDE; p[5] = 0xAD; p[6] = 0xBE; p[7] = 0xEF;
  std::memcpy(&p[28], mac, 6);
  p[236] = 0x63; p[237] = 0x82; p[238] = 0x53; p[239] = 0x63;
  size_t i = 240;
  p[i++] = 53; p[i++] = 1; p[i++] = type;
  if (requested) { p[i++] = 50; p[i++] = 4; std::memcpy(&p[i], requested, 4); i += 4; }
  if (server) { p[i++] = 54; p[i++] = 4; std::memcpy(&p[i], server, 4); i += 4; }
  p[i++] = 255;
  return p;
}

static bool has_option(const uint8_t *r, size_t len, uint8_t code) {
  for (size_t i = 240; i < len && r[i] != 255;) {
    if (r[i] == 0) { i++; continue; }
    if (r[i] == code) return true;
    i += 2 + r[i + 1];
  }
  return false;
}

static void test_dhcp_offer_then_ack() {
  dhcp::Server s({{192, 168, 7, 1}, {255, 255, 255, 0}, 2, 8});
  const uint8_t mac[6] = {0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02};
  uint8_t reply[600];

  auto d = dhcp_request(dhcp::Discover, mac);
  size_t n = s.handle(d.data(), d.size(), reply, sizeof reply);
  CHECK(n >= 300);
  CHECK(reply[0] == 2 && reply[242] == dhcp::Offer);
  const uint8_t offered[4] = {192, 168, 7, 2};
  CHECK(std::memcmp(reply + 16, offered, 4) == 0);
  CHECK(std::memcmp(reply + 4, d.data() + 4, 4) == 0);   // same transaction id
  CHECK(has_option(reply, n, 1));                         // netmask
  CHECK(!has_option(reply, n, 3));                        // no router: not an internet path
  CHECK(!has_option(reply, n, 6));                        // no DNS

  const uint8_t us[4] = {192, 168, 7, 1};
  auto r = dhcp_request(dhcp::Request, mac, offered, us);
  n = s.handle(r.data(), r.size(), reply, sizeof reply);
  CHECK(n >= 300 && reply[242] == dhcp::Ack);
  CHECK(std::memcmp(reply + 16, offered, 4) == 0);
}

static void test_dhcp_same_phone_same_address() {
  dhcp::Server s({{192, 168, 7, 1}, {255, 255, 255, 0}, 2, 8});
  const uint8_t a[6] = {1, 2, 3, 4, 5, 6}, b[6] = {1, 2, 3, 4, 5, 7};
  uint8_t reply[600];
  auto da = dhcp_request(dhcp::Discover, a);
  s.handle(da.data(), da.size(), reply, sizeof reply);
  auto db = dhcp_request(dhcp::Discover, b);
  s.handle(db.data(), db.size(), reply, sizeof reply);
  CHECK(reply[19] == 3);  // second phone gets .3
  s.handle(da.data(), da.size(), reply, sizeof reply);
  CHECK(reply[19] == 2);  // first phone, replugged, gets its .2 back
}

static void test_dhcp_naks_a_foreign_address_and_ignores_other_servers() {
  dhcp::Server s({{192, 168, 7, 1}, {255, 255, 255, 0}, 2, 8});
  const uint8_t mac[6] = {9, 9, 9, 9, 9, 9};
  uint8_t reply[600];
  const uint8_t wrong[4] = {10, 0, 0, 5};
  auto r = dhcp_request(dhcp::Request, mac, wrong);
  size_t n = s.handle(r.data(), r.size(), reply, sizeof reply);
  CHECK(n >= 300 && reply[242] == dhcp::Nak);   // a phone remembering another network
  const uint8_t other[4] = {192, 168, 1, 1};
  const uint8_t addr[4] = {192, 168, 7, 2};
  auto o = dhcp_request(dhcp::Request, mac, addr, other);
  CHECK(s.handle(o.data(), o.size(), reply, sizeof reply) == 0);
  CHECK(s.handle(reply, 10, reply, sizeof reply) == 0);  // runt: ignored
}

// --- BL618 sample ring: where gaps fall ------------------------------------------

// Everything the consumer will hand on: peeks consumed whole, gaps summed.
struct Drained { std::vector<uint8_t> bytes; std::vector<std::pair<size_t, uint32_t>> gaps; };
static Drained drain_ring() {
  Drained d;
  for (;;) {
    const uint8_t *p;
    uint32_t gap = 0;
    uint32_t n = sample_ring_peek(&p, &gap);
    if (gap) d.gaps.push_back({d.bytes.size(), gap});
    if (n == 0) break;
    d.bytes.insert(d.bytes.end(), p, p + n);
    sample_ring_consume(n);
  }
  return d;
}

static std::vector<uint8_t> block(uint8_t start, size_t n) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(start + i);
  return b;
}

static void test_sample_ring_clean_stream_has_no_gaps() {
  sample_ring_clear();
  CHECK(sample_ring_push(block(0, 1000).data(), 1000));
  CHECK(sample_ring_push(block(0, 1000).data(), 1000));
  Drained d = drain_ring();
  CHECK(d.bytes.size() == 2000);
  CHECK(d.gaps.empty());
}

// A block that does not fit is announced at the position of the next one that does, and
// the consumer is never handed a run across it — so a datagram cut from a run starts
// exactly where the missing samples belong.
static void test_sample_ring_gap_lands_where_the_drop_was() {
  sample_ring_clear();
  CHECK(sample_ring_push(block(0, 3000).data(), 3000));
  CHECK(!sample_ring_push(block(0, 2000).data(), 2000));   // 4096-byte ring: no room
  CHECK(!sample_ring_push(block(0, 1200).data(), 1200));   // and again: they add up
  // Drained whole: the ring may hand it over in two runs if it wraps the storage.
  Drained before = drain_ring();
  CHECK(before.bytes.size() == 3000 && before.gaps.empty());
  CHECK(sample_ring_push(block(7, 500).data(), 500));      // fits now; carries the gap
  Drained d = drain_ring();
  CHECK(d.bytes.size() == 500);
  CHECK(d.gaps.size() == 1);
  CHECK(d.gaps[0].first == 0 && d.gaps[0].second == (2000 + 1200) / 2);
  CHECK(d.bytes[0] == 7);
}

// What USB lost before a block is a gap in front of that block, exactly like a dropped one —
// a failed transfer used to be joined up silently, so every sample after it was numbered
// early — and it is not the ring's drop, so the ring's own count stays where it was.
static void test_sample_ring_usb_loss_is_a_gap_where_it_happened() {
  sample_ring_clear();
  uint32_t drops = sample_ring_dropped_blocks();
  CHECK(sample_ring_push(block(0, 1000).data(), 1000));
  sample_ring_skip(32768);                                  // one failed 32 KiB transfer
  CHECK(sample_ring_push(block(9, 600).data(), 600));
  Drained d = drain_ring();
  CHECK(d.bytes.size() == 1600);
  CHECK(d.gaps.size() == 1);
  CHECK(d.gaps[0].first == 1000 && d.gaps[0].second == 32768 / 2);
  CHECK(d.bytes[1000] == 9);
  CHECK(sample_ring_dropped_blocks() == drops);
}

// The ring wraps: the run stops at the end of storage, and a gap past the wrap still
// arrives at its own position, not early.
static void test_sample_ring_gap_after_wrap() {
  sample_ring_clear();
  for (int i = 0; i < 3; i++) {   // walk the positions round the end of the storage
    CHECK(sample_ring_push(block(0, 1500).data(), 1500));
    drain_ring();
  }
  CHECK(sample_ring_push(block(0, 1000).data(), 1000));
  CHECK(!sample_ring_push(block(0, 4000).data(), 4000));
  CHECK(sample_ring_push(block(42, 1000).data(), 1000));
  Drained d = drain_ring();
  CHECK(d.bytes.size() == 2000);
  CHECK(d.gaps.size() == 1);
  CHECK(d.gaps.size() == 1 && d.gaps[0].first == 1000 && d.gaps[0].second == 2000);
  CHECK(d.bytes[1000] == 42);
}

static void test_sample_ring_clear_forgets_gaps_and_tcp_skips_them() {
  sample_ring_clear();
  CHECK(sample_ring_push(block(0, 4000).data(), 4000));
  CHECK(!sample_ring_push(block(0, 1000).data(), 1000));
  drain_ring();
  CHECK(sample_ring_push(block(0, 100).data(), 100));      // gap queued before these
  sample_ring_clear();
  CHECK(sample_ring_push(block(0, 100).data(), 100));
  CHECK(drain_ring().gaps.empty());
  // The rtl_tcp path asks for no gaps and must still move past them.
  CHECK(!sample_ring_push(block(0, 5000).data(), 5000));
  CHECK(sample_ring_push(block(0, 100).data(), 100));
  const uint8_t *p;
  uint32_t n = sample_ring_peek(&p, nullptr);
  CHECK(n == 100);
  sample_ring_consume(n);
  CHECK(sample_ring_used() == 0);
}

// Sustained overflow, the way a board behind slow Wi-Fi runs: the producer offers a chunk
// every step whether or not there is room, the consumer takes less than that, and every
// chunk is a ramp continuing from the last one offered — kept or dropped — exactly like
// the synthetic counter. Then every byte handed on must sit at its own position once the
// gaps are counted in, and the gaps must add up to what was dropped.
static void test_sample_ring_gaps_stay_exact_under_sustained_overflow() {
  sample_ring_clear();
  uint64_t offered = 0;            // bytes the producer has generated, kept or not
  uint64_t dropped_samples = 0;
  uint64_t position = 0;           // stream position of the next byte the consumer sees
  uint64_t reported = 0;
  int misplaced = 0;
  for (int step = 0; step < 200000; step++) {
    // Producer: chunks of varying size, as the counter's pacing gives — small enough that
    // far more gaps are waiting in the ring at once than the gap queue has places, which
    // is the board's case: 3 MiB of ring against 8 KiB counter blocks. This is the test
    // that caught gaps being announced late once the queue filled.
    uint32_t n = 2 + (uint32_t)(step * 37 % 24) * 2;
    std::vector<uint8_t> chunk(n);
    for (uint32_t i = 0; i < n; i++) chunk[i] = (uint8_t)(offered + i);
    if (!sample_ring_push(chunk.data(), n)) dropped_samples += n / 2;
    offered += n;
    // Consumer: under one chunk a step, in datagram-sized pieces.
    for (int k = 0; k < 1; k++) {
      const uint8_t *p;
      uint32_t gap = 0;
      uint32_t run = sample_ring_peek(&p, &gap);
      reported += gap;
      position += 2ull * gap;
      uint32_t len = (run < 16 ? run : 16) & ~1u;
      if (len == 0) break;
      for (uint32_t i = 0; i < len; i++)
        if (p[i] != (uint8_t)(position + i)) { misplaced++; break; }
      sample_ring_consume(len);
      position += len;
    }
  }
  // Drain what is left, so every announced gap has been reached.
  for (;;) {
    const uint8_t *p;
    uint32_t gap = 0;
    uint32_t run = sample_ring_peek(&p, &gap);
    reported += gap;
    position += 2ull * gap;
    if (run == 0) break;
    for (uint32_t i = 0; i < run; i++)
      if (p[i] != (uint8_t)(position + i)) { misplaced++; break; }
    sample_ring_consume(run);
    position += run;
  }
  CHECK(dropped_samples > 0);
  CHECK(misplaced == 0);
  if (misplaced) std::printf("  %d runs misplaced\n", misplaced);
  // A gap still pending in the producer (no block has fitted since) is not yet announced.
  CHECK(reported <= dropped_samples);
  CHECK(position == offered - 2 * (dropped_samples - reported));
  std::printf("  overflow: %llu samples dropped, %llu reported\n",
              (unsigned long long)dropped_samples, (unsigned long long)reported);
}

// The guard in front of the ring catches writes from outside it, so the ring's own traffic
// must never touch it — wrapping, overflowing and clearing included — or every report from
// the board would be a false alarm.
static void test_sample_ring_never_writes_its_guard() {
  sample_ring_guard_fill();
  sample_ring_clear();
  for (int round = 0; round < 50; round++) {
    sample_ring_push(block((uint8_t)round, 1500).data(), 1500);   // wraps and overflows a 4096-byte ring
    drain_ring();
  }
  sample_ring_clear();
  uint32_t at = 0;
  uint8_t value = 0;
  CHECK(sample_ring_guard_check(&at, &value) == 0);
}

int main() {
  test_header();
  test_parser_split_and_batched();
  test_gain_by_index_clamps();
  test_ring_never_starves_usb();
  test_ring_never_steals_what_is_being_sent();
  test_ring_new_session_starts_clean();
  test_dhcp_offer_then_ack();
  test_dhcp_same_phone_same_address();
  test_dhcp_naks_a_foreign_address_and_ignores_other_servers();
  test_sample_ring_clean_stream_has_no_gaps();
  test_sample_ring_gap_lands_where_the_drop_was();
  test_sample_ring_gap_after_wrap();
  test_sample_ring_usb_loss_is_a_gap_where_it_happened();
  test_sample_ring_clear_forgets_gaps_and_tcp_skips_them();
  test_sample_ring_gaps_stay_exact_under_sustained_overflow();
  test_sample_ring_never_writes_its_guard();
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("all host tests passed\n");
  return 0;
}
