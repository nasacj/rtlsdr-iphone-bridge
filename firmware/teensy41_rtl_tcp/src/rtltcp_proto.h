// SPDX-License-Identifier: GPL-2.0-or-later
//
// The rtl_tcp wire protocol, unchanged from rtl_tcp.c so that any client works.
//
//   server → client, once: "RTL0", tuner type (u32 BE), tuner gain count (u32 BE)
//   server → client, then: raw unsigned 8-bit I/Q, interleaved, forever
//   client → server:       5-byte commands, 1 byte opcode + u32 BE parameter
//
// TCP delivers a byte stream, not messages, so a command can arrive split across reads,
// or several in one. The parser keeps the partial command between calls.

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rtltcp {

enum Opcode : uint8_t {
  SetFrequency = 0x01,
  SetSampleRate = 0x02,
  SetGainMode = 0x03,
  SetGain = 0x04,              // tenths of a dB
  SetFrequencyCorrection = 0x05,
  SetIfGain = 0x06,            // stage << 16 | (int16) gain
  SetTestMode = 0x07,
  SetAgcMode = 0x08,
  SetDirectSampling = 0x09,
  SetOffsetTuning = 0x0a,
  SetRtlXtal = 0x0b,
  SetTunerXtal = 0x0c,
  SetGainByIndex = 0x0d,
  SetBiasTee = 0x0e,
};

struct Command {
  uint8_t opcode;
  uint32_t param;
};

constexpr size_t kHeaderSize = 12;

// The 12-byte greeting the client reads before any samples.
inline void make_header(uint8_t out[kHeaderSize], uint32_t tuner_type, uint32_t gain_count) {
  out[0] = 'R'; out[1] = 'T'; out[2] = 'L'; out[3] = '0';
  for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(tuner_type >> (24 - 8 * i));
  for (int i = 0; i < 4; i++) out[8 + i] = (uint8_t)(gain_count >> (24 - 8 * i));
}

class Parser {
public:
  // Feed one received byte; returns true and fills `out` when it completes a command.
  bool feed(uint8_t byte, Command &out) {
    buf_[have_++] = byte;
    if (have_ < 5) return false;
    have_ = 0;
    out.opcode = buf_[0];
    out.param = ((uint32_t)buf_[1] << 24) | ((uint32_t)buf_[2] << 16) |
                ((uint32_t)buf_[3] << 8) | (uint32_t)buf_[4];
    return true;
  }
  void reset() { have_ = 0; }
  size_t pending() const { return have_; }

private:
  uint8_t buf_[5] = {0};
  size_t have_ = 0;
};

// What a gain index means: the tuner's gain table, clamped, as rtl_tcp's
// set_gain_by_index does.
inline int gain_for_index(const int *gains, int count, uint32_t index) {
  if (count <= 0 || gains == nullptr) return 0;
  if (index >= (uint32_t)count) index = (uint32_t)count - 1;
  return gains[index];
}

}  // namespace rtltcp
