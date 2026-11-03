// SPDX-License-Identifier: AGPL-3.0-or-later
//
// rtl_tcp over the Teensy 4.1's Ethernet port: the network half of the bridge.
//
// Everything here runs from loop(). The USB interrupt only fills sample buffers
// (rtl_usb.cpp); QNEthernet is not interrupt-safe and is never touched from one.
//
// **How a session ends**, every way it can, and what the board does:
//
// | what happened                         | how the board sees it              | action                          |
// |---------------------------------------|------------------------------------|---------------------------------|
// | the app disconnects                   | FIN: `connected()` goes false      | stop streaming, close           |
// | the app or phone vanishes mid-stream  | nothing is acknowledged for 10 s   | abort (RST), stop streaming     |
// | the cable or adapter is unplugged     | PHY link down                      | abort at once, no retransmits   |
// | a second connection arrives           | `accept()` while a session runs    | the new one replaces the old    |
// | the dongle is unplugged               | USB disconnect                     | abort, close librtlsdr          |
// | a client connects with no dongle      | accept with no device open         | close it immediately            |
//
// A new connection replaces the old one rather than being refused, because the case that
// produces two is a client reconnecting after its first connection died without telling
// the board — refusing would lock it out until the stall timer fired. `rtl_tcp` serves one
// client, and so does this.
//
// The dongle keeps its settings between sessions, as with `rtl_tcp`; a client that
// sends every setting on connect does not depend on what the previous client left.

#include <Arduino.h>
#include <QNEthernet.h>

#include "dhcp_server.h"
#include "rtl-sdr.h"  // ../common/librtlsdr, an Arduino library (--library)
#include "rtl_usb.h"
#include "rtltcp_proto.h"

using namespace qindesign::network;

namespace {

// 192.168.7.1, the same as the BL618 board's own hotspot, so the app's settings never change.
const IPAddress kAddress(192, 168, 7, 1);
const IPAddress kNetmask(255, 255, 255, 0);
const IPAddress kGateway(0, 0, 0, 0);
constexpr uint16_t kRtlTcpPort = 1234;
constexpr uint32_t kStallMs = 10000;
constexpr uint32_t kReopenMs = 2000;
constexpr uint32_t kStatusMs = 5000;
constexpr int kLed = 13;

// rtl_tcp's own defaults, applied when the dongle opens.
constexpr uint32_t kDefaultRate = 2048000;
constexpr uint32_t kDefaultFrequency = 100000000;

EthernetServer server(kRtlTcpPort);
EthernetClient client;
EthernetUDP dhcp_udp;
dhcp::Server dhcpd({{192, 168, 7, 1}, {255, 255, 255, 0}, 2, 8});

rtlsdr_dev_t *dev = nullptr;
uint32_t next_open_at = 0;

bool session = false;
rtltcp::Parser parser;
int block = -1;              // block being sent, or -1
const uint8_t *block_data = nullptr;
uint32_t block_length = 0;
uint32_t block_offset = 0;
uint32_t last_progress = 0;
uint64_t session_bytes = 0;
uint32_t session_drops_at_start = 0;

volatile bool link_up = false;
volatile bool link_changed = false;
uint32_t next_status_at = 0;

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  Serial.printf("[%8lu] %s\r\n", (unsigned long)millis(), line);
}

// --- the dongle ----------------------------------------------------------------------

void open_dongle() {
  const int r = rtlsdr_open(&dev, 0);
  if (r < 0) {
    dev = nullptr;
    log("dongle %04x:%04x: rtlsdr_open failed (%d), retrying", rtlusb.vid(), rtlusb.pid(), r);
    next_open_at = millis() + kReopenMs;
    return;
  }
  rtlsdr_set_sample_rate(dev, kDefaultRate);
  rtlsdr_set_center_freq(dev, kDefaultFrequency);
  rtlsdr_set_tuner_gain_mode(dev, 0);
  rtlsdr_reset_buffer(dev);
  log("dongle ready: %04x:%04x, tuner %d, %d gains", rtlusb.vid(), rtlusb.pid(),
      (int)rtlsdr_get_tuner_type(dev), rtlsdr_get_tuner_gains(dev, nullptr));
  digitalWrite(kLed, HIGH);
}

void close_dongle() {
  if (dev) rtlsdr_close(dev);  // its control transfers fail fast: the device is gone
  dev = nullptr;
  digitalWrite(kLed, LOW);
}

// --- the session -----------------------------------------------------------------------

void end_session(const char *why, bool abort_connection) {
  if (!session) return;
  rtlusb.stop_streaming();
  if (block >= 0) {
    rtlusb.release_block(block);
    block = -1;
  }
  if (abort_connection) client.abort(); else client.stop();
  session = false;
  const RtlUsb::Stats s = rtlusb.stats();
  log("session ended: %s; %llu bytes sent, %lu blocks dropped", why,
      (unsigned long long)session_bytes, (unsigned long)(s.dropped - session_drops_at_start));
}

void start_session(EthernetClient incoming) {
  if (session) end_session("replaced by a new connection", true);
  if (dev == nullptr) {
    log("connection from %u.%u.%u.%u refused: no dongle", incoming.remoteIP()[0], incoming.remoteIP()[1],
        incoming.remoteIP()[2], incoming.remoteIP()[3]);
    incoming.stop();
    return;
  }
  client = incoming;
  client.setNoDelay(true);

  uint8_t header[rtltcp::kHeaderSize];
  rtltcp::make_header(header, (uint32_t)rtlsdr_get_tuner_type(dev),
                      (uint32_t)rtlsdr_get_tuner_gains(dev, nullptr));
  if (client.writeFully(header, sizeof header) != sizeof header) {
    client.abort();
    log("connection lost while sending the header");
    return;
  }
  parser.reset();
  rtlsdr_reset_buffer(dev);  // clear the dongle's FIFO, so the stream starts fresh
  rtlusb.start_streaming();
  session = true;
  session_bytes = 0;
  session_drops_at_start = rtlusb.stats().dropped;
  last_progress = millis();
  log("session from %u.%u.%u.%u", client.remoteIP()[0], client.remoteIP()[1], client.remoteIP()[2],
      client.remoteIP()[3]);
}

void apply(const rtltcp::Command &c) {
  switch (c.opcode) {
    case rtltcp::SetFrequency: rtlsdr_set_center_freq(dev, c.param); break;
    case rtltcp::SetSampleRate: rtlsdr_set_sample_rate(dev, c.param); break;
    case rtltcp::SetGainMode: rtlsdr_set_tuner_gain_mode(dev, (int)c.param); break;
    case rtltcp::SetGain: rtlsdr_set_tuner_gain(dev, (int)c.param); break;
    case rtltcp::SetFrequencyCorrection: rtlsdr_set_freq_correction(dev, (int)c.param); break;
    case rtltcp::SetIfGain: rtlsdr_set_tuner_if_gain(dev, (int)(c.param >> 16), (short)(c.param & 0xffff)); break;
    case rtltcp::SetTestMode: rtlsdr_set_testmode(dev, (int)c.param); break;
    case rtltcp::SetAgcMode: rtlsdr_set_agc_mode(dev, (int)c.param); break;
    case rtltcp::SetDirectSampling: rtlsdr_set_direct_sampling(dev, (int)c.param); break;
    case rtltcp::SetOffsetTuning: rtlsdr_set_offset_tuning(dev, (int)c.param); break;
    case rtltcp::SetRtlXtal: rtlsdr_set_xtal_freq(dev, c.param, 0); break;
    case rtltcp::SetTunerXtal: rtlsdr_set_xtal_freq(dev, 0, c.param); break;
    case rtltcp::SetGainByIndex: {
      int gains[64];
      const int n = rtlsdr_get_tuner_gains(dev, nullptr);
      if (n > 0 && n <= 64) {
        rtlsdr_get_tuner_gains(dev, gains);
        rtlsdr_set_tuner_gain(dev, rtltcp::gain_for_index(gains, n, c.param));
      }
      break;
    }
    case rtltcp::SetBiasTee: rtlsdr_set_bias_tee(dev, (int)c.param); break;
    default: log("unknown command 0x%02x %lu ignored", c.opcode, (unsigned long)c.param); return;
  }
  log("cmd 0x%02x %lu", c.opcode, (unsigned long)c.param);
}

void service_session() {
  if (!client.connected()) {
    end_session("closed by the client", false);
    return;
  }

  // Commands. Each can be several control transfers; the USB interrupt keeps streaming
  // meanwhile, so only the network waits.
  rtltcp::Command c;
  for (int n = client.available(); n > 0 && session; n--) {
    const int b = client.read();
    if (b < 0) break;
    if (parser.feed((uint8_t)b, c)) apply(c);
  }

  // Samples: whatever the TCP send buffer will take, block by block in arrival order.
  bool pending = false;
  while (session) {
    if (block < 0) {
      block = rtlusb.take_block(&block_data, &block_length);
      block_offset = 0;
      if (block < 0) break;
    }
    pending = true;
    const int space = client.availableForWrite();
    if (space <= 0) break;
    const uint32_t want = min((uint32_t)space, block_length - block_offset);
    const size_t wrote = client.write(block_data + block_offset, want);
    if (wrote == 0) break;
    block_offset += wrote;
    session_bytes += wrote;
    last_progress = millis();
    if (block_offset >= block_length) {
      rtlusb.release_block(block);
      block = -1;
      pending = false;
    }
  }
  if (!pending) last_progress = millis();

  // A peer that stopped reading: TCP would keep retransmitting for minutes.
  if (session && millis() - last_progress > kStallMs) end_session("no progress for 10 s", true);
}

// --- DHCP ------------------------------------------------------------------------------

void service_dhcp() {
  const int size = dhcp_udp.parsePacket();
  if (size <= 0) return;
  static uint8_t request[600];
  static uint8_t reply[600];
  const int got = dhcp_udp.read(request, min(size, (int)sizeof request));
  if (got <= 0) return;
  const size_t n = dhcpd.handle(request, (size_t)got, reply, sizeof reply);
  if (n == 0) return;
  dhcp_udp.beginPacket(IPAddress(255, 255, 255, 255), dhcp::kClientPort);
  dhcp_udp.write(reply, n);
  dhcp_udp.endPacket();
  log("dhcp: %s 192.168.7.%u to %02x:%02x:%02x:%02x:%02x:%02x",
      reply[242] == dhcp::Offer ? "offer" : reply[242] == dhcp::Ack ? "ack" : "nak", reply[19],
      request[28], request[29], request[30], request[31], request[32], request[33]);
}

void print_status() {
  const RtlUsb::Stats s = rtlusb.stats();
  log("link %s, dongle %s, %s; usb %llu B in, %lu blocks, %lu dropped, %lu starved, %lu bulk errors",
      link_up ? "up" : "down", dev ? "open" : (rtlusb.ready() ? "claimed" : "absent"),
      session ? "streaming" : "idle", (unsigned long long)s.bytes_in, (unsigned long)s.filled,
      (unsigned long)s.dropped, (unsigned long)s.starved, (unsigned long)s.bulk_errors);
}

}  // namespace

void bridge_setup() {
  Serial.begin(115200);
  const uint32_t wait_until = millis() + 1500;
  while (!Serial && millis() < wait_until) {}
  pinMode(kLed, OUTPUT);
  digitalWrite(kLed, LOW);
  log("teensy41_rtl_tcp: rtl_tcp on %u.%u.%u.%u:%u", kAddress[0], kAddress[1], kAddress[2], kAddress[3], kRtlTcpPort);

  myusb.begin();  // also switches on the USB host port's 5 V (GPIO_EMC_40 on the Teensy 4.1)

  Ethernet.onLinkState([](bool up) {
    link_up = up;
    link_changed = true;
  });
  if (!Ethernet.begin(kAddress, kNetmask, kGateway)) log("Ethernet failed to start (no PHY? an NE board?)");
  server.begin();
  dhcp_udp.begin(dhcp::kServerPort);
  // The same name as the BL618 board's, so the app can say rtlsdr.local whichever board is
  // in use.
  MDNS.begin("rtlsdr");
  MDNS.addService("_rtl_tcp", "_tcp", kRtlTcpPort);
}

void bridge_loop() {
  myusb.Task();

  if (rtlusb.take_lost()) {
    log("dongle unplugged");
    end_session("dongle unplugged", true);
    close_dongle();
  }
  if (dev == nullptr && rtlusb.ready() && (int32_t)(millis() - next_open_at) >= 0) open_dongle();

  if (link_changed) {
    link_changed = false;
    log("link %s", link_up ? "up" : "down");
    // The peer is unreachable now: retransmitting into a dead cable only delays the next
    // session, and the phone on the far end will connect afresh anyway.
    if (!link_up) end_session("link down", true);
  }

  service_dhcp();

  EthernetClient incoming = server.accept();
  if (incoming) start_session(incoming);
  if (session) service_session();

  if ((int32_t)(millis() - next_status_at) >= 0) {
    next_status_at = millis() + kStatusMs;
    print_status();
  }
}
