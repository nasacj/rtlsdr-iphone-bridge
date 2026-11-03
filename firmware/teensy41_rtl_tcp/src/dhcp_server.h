// SPDX-License-Identifier: GPL-2.0-or-later
//
// The smallest DHCP server that gets an iPhone's USB Ethernet adapter an address.
//
// The phone asks for an address the moment the cable is plugged in, and with no answer it
// falls back to 169.254.x.x after several seconds — where, with Wi-Fi also up, routing to
// the board is not reliable. So the board answers, with 192.168.7.2–9 out of 192.168.7.0/24
// and itself as the server.
//
// **No router and no DNS option, deliberately.** Without a router the phone treats the
// adapter as a local network only and keeps using Wi-Fi or cellular for the internet; with
// one, it may try to send its internet traffic to a board that cannot forward it.
//
// Only DISCOVER → OFFER and REQUEST → ACK/NAK, plus RELEASE. Everything is answered by
// broadcast, which a client without an address can always receive.

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace dhcp {

constexpr uint16_t kServerPort = 67;
constexpr uint16_t kClientPort = 68;
constexpr uint32_t kLeaseSeconds = 24 * 3600;

enum MessageType : uint8_t { Discover = 1, Offer = 2, Request = 3, Decline = 4, Ack = 5, Nak = 6, Release = 7, Inform = 8 };

struct Config {
  uint8_t server[4];   // our own address, e.g. 192.168.7.1
  uint8_t netmask[4];  // 255.255.255.0
  uint8_t first_host;  // last octet of the first address handed out
  uint8_t pool_size;   // how many addresses
};

class Server {
public:
  static constexpr size_t kMaxPool = 16;

  explicit Server(const Config &c) : config_(c) {
    if (config_.pool_size > kMaxPool) config_.pool_size = kMaxPool;
    memset(leases_, 0, sizeof(leases_));
  }

  // Handle one request datagram. Returns the reply length written to `reply`, or 0 when
  // there is nothing to send. `reply` needs at least 300 bytes (a minimal BOOTP frame).
  size_t handle(const uint8_t *req, size_t len, uint8_t *reply, size_t cap) {
    if (len < 240 || cap < 300) return 0;
    if (req[0] != 1 || req[1] != 1 || req[2] != 6) return 0;  // BOOTREQUEST, Ethernet, 6-byte MAC
    if (!(req[236] == 0x63 && req[237] == 0x82 && req[238] == 0x53 && req[239] == 0x63)) return 0;

    uint8_t type = 0;
    const uint8_t *requested = nullptr;
    const uint8_t *server_id = nullptr;
    for (size_t i = 240; i < len;) {
      uint8_t code = req[i];
      if (code == 0) { i++; continue; }      // pad
      if (code == 255) break;                 // end
      if (i + 1 >= len) break;
      uint8_t olen = req[i + 1];
      if (i + 2 + olen > len) break;
      const uint8_t *v = req + i + 2;
      if (code == 53 && olen == 1) type = v[0];
      if (code == 50 && olen == 4) requested = v;
      if (code == 54 && olen == 4) server_id = v;
      i += 2 + olen;
    }
    const uint8_t *mac = req + 28;

    switch (type) {
      case Discover: {
        int slot = lease_for(mac, true);
        if (slot < 0) return 0;  // pool exhausted: stay silent rather than lie
        return build(req, reply, Offer, slot);
      }
      case Request: {
        // A REQUEST naming another server is that client choosing someone else.
        if (server_id && memcmp(server_id, config_.server, 4) != 0) return 0;
        int slot = lease_for(mac, true);
        if (slot < 0) return build(req, reply, Nak, -1);
        uint8_t ours[4];
        address_of(slot, ours);
        // INIT-REBOOT / renew put the address in option 50 or in ciaddr.
        const uint8_t *asked = requested ? requested : (memcmp(req + 12, "\0\0\0\0", 4) ? req + 12 : nullptr);
        if (asked && memcmp(asked, ours, 4) != 0) return build(req, reply, Nak, -1);
        return build(req, reply, Ack, slot);
      }
      case Release: {
        int slot = lease_for(mac, false);
        if (slot >= 0) leases_[slot].used = false;
        return 0;
      }
      default:
        return 0;
    }
  }

  void address_of(int slot, uint8_t out[4]) const {
    memcpy(out, config_.server, 3);
    out[3] = (uint8_t)(config_.first_host + slot);
  }

private:
  struct Lease { bool used; uint8_t mac[6]; };

  // The slot this MAC already holds, or a new one when `allocate`. The same MAC always
  // gets the same address back, so a phone that replugs keeps its address.
  int lease_for(const uint8_t *mac, bool allocate) {
    for (int i = 0; i < config_.pool_size; i++) {
      if (leases_[i].used && memcmp(leases_[i].mac, mac, 6) == 0) return i;
    }
    if (!allocate) return -1;
    for (int i = 0; i < config_.pool_size; i++) {
      if (!leases_[i].used) { leases_[i].used = true; memcpy(leases_[i].mac, mac, 6); return i; }
    }
    return -1;
  }

  size_t build(const uint8_t *req, uint8_t *out, uint8_t type, int slot) {
    memset(out, 0, 300);
    out[0] = 2;               // BOOTREPLY
    out[1] = 1; out[2] = 6;   // Ethernet, 6-byte address
    memcpy(out + 4, req + 4, 4);    // xid
    out[10] = 0x80;                  // broadcast flag: the client has no address yet
    if (slot >= 0) address_of(slot, out + 16);   // yiaddr
    memcpy(out + 20, config_.server, 4);         // siaddr
    memcpy(out + 28, req + 28, 16);              // chaddr
    out[236] = 0x63; out[237] = 0x82; out[238] = 0x53; out[239] = 0x63;
    size_t i = 240;
    out[i++] = 53; out[i++] = 1; out[i++] = type;
    out[i++] = 54; out[i++] = 4; memcpy(out + i, config_.server, 4); i += 4;
    if (type != Nak) {
      out[i++] = 51; out[i++] = 4; put32(out + i, kLeaseSeconds); i += 4;
      out[i++] = 58; out[i++] = 4; put32(out + i, kLeaseSeconds / 2); i += 4;
      out[i++] = 59; out[i++] = 4; put32(out + i, kLeaseSeconds / 8 * 7); i += 4;
      out[i++] = 1;  out[i++] = 4; memcpy(out + i, config_.netmask, 4); i += 4;
    }
    out[i++] = 255;
    return i < 300 ? 300 : i;  // BOOTP minimum frame, which some clients insist on
  }

  static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

  Config config_;
  Lease leases_[kMaxPool];
};

}  // namespace dhcp
