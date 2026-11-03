#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Counts lost bytes on an rtl_tcp link, end to end, with no radio involved.

    firmware/tools/rtl_tcp_counter.py 192.168.7.1 --rate 2.4M --seconds 60
    firmware/tools/rtl_tcp_counter.py rtlsdr.local --rate 1.024M

It connects to any rtl_tcp server (a board here, or rtl_tcp on a computer), sets the
sample rate and turns on the RTL2832U's test mode, in which the chip sends an
incrementing 8-bit counter instead of samples. Every byte must be the previous one
plus one, so anything lost between the dongle's ADC and this script is countable:
USB, the board's buffer, Wi-Fi or Ethernet, TCP.

Two numbers decide it, and both must be right:

  breaks         places where the counter jumped. Each is at least one lost byte. Must be 0.
  delivered rate bytes per second / 2, against the requested rate. A link that runs well
                 under it is broken even with no breaks, because a whole block dropped
                 cleanly can be a multiple of 256 bytes and leave the counter looking intact.

The bridge firmware with no dongle plugged in answers with tuner type 0 and generates the
same counter itself, so the same command first checks Wi-Fi and the board's network side
alone, and then, with the dongle in, the whole path.

Test mode is turned off again on exit, because the dongle keeps its settings between
clients and rtl_tcp resets nothing: left on, the next client demodulates a counter.

With --udp it speaks rtl_udp instead (port 1235; the firmware's rtl_tcp_server.c has the
format), and the check gets stricter, because every datagram carries the index of its
first sample: the counter's value at index i is known, so each datagram's *content* is
checked against its own position, and loss is counted exactly rather than modulo 256 —
datagrams the network lost (a sequence gap) apart from samples the board dropped itself
(an index jump with no sequence gap).

    firmware/tools/rtl_tcp_counter.py 192.168.7.1 --udp --rate 2.4M

With --udp --magnitude it asks for one magnitude byte a sample instead of I/Q (rtl_udp
version 2, SET_FORMAT 1). The counter is still a function of position: sample i is the
pair (c, c+1) with c = base + 2i, so its magnitude byte is known too, and every byte the
board computed is checked against the same formula computed here.

    firmware/tools/rtl_tcp_counter.py 192.168.7.1 --udp --magnitude --rate 2.0M

Python 3 standard library only.
"""

import argparse
import math
import socket
import struct
import sys
import threading
import time

SET_SAMPLE_RATE = 0x02
SET_TEST_MODE = 0x07
START_UDP = 0x40
SET_FORMAT = 0x41
FLAG_MAGNITUDE = 0x01


def magnitude_code(i, q):
    """The rtl_udp magnitude byte: sqrt((2I-255)^2 + (2Q-255)^2), square-root companded."""
    r = math.sqrt((2 * i - 255) ** 2 + (2 * q - 255) ** 2)
    return min(255, int(255 * math.sqrt(r / math.sqrt(2 * 255 * 255)) + 0.5))


# Sample i of the counter is (c, c+1) with c = base + 2i, so the expected bytes of a run
# starting at counter value s are the magnitudes of every second pair from s: one lane for
# even s and one for odd, each 128 long and repeated past the longest datagram.
_PAIR = [magnitude_code(c, (c + 1) & 0xFF) for c in range(256)]
_LANES = [bytes(_PAIR[(parity + 2 * m) & 0xFF] for m in range(128)) * 14 for parity in (0, 1)]


def expected_magnitudes(start, count):
    return _LANES[start & 1][start >> 1:(start >> 1) + count]

# The RTL2832U accepts these and nothing else; rtl_tcp takes any other value and then
# sends no samples at all.
VALID_RATES = ((225_001, 300_000), (900_000, 3_200_000))


def parse_rate(text):
    text = text.strip().lower()
    scale = 1
    if text.endswith("m"):
        scale, text = 1_000_000, text[:-1]
    elif text.endswith("k"):
        scale, text = 1_000, text[:-1]
    return int(round(float(text) * scale))


def command(sock, code, value):
    sock.sendall(struct.pack(">BI", code, value))


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("server closed the connection")
        data += chunk
    return data


def describe_wrong(payload, want, magnitude, base, index, at, recent):
    """Where in a datagram the bytes stop being the counter, and what they are instead.

    A wrong datagram is one of three things, and they look different here: a header whose
    index is off (every byte wrong by one constant shift), a region overwritten with data
    from elsewhere in the stream (a run of bytes that is still a ramp, at another phase),
    or bytes that are no counter at all."""
    bad = [k for k in range(min(len(payload), len(want))) if payload[k] != want[k]]
    lines = [f"  wrong datagram at {at:.1f} s: index {index}, {len(payload)} bytes, "
             f"{len(bad)} differ, from byte {bad[0]} to {bad[-1]}"
             + ("" if len(payload) == len(want) else f" (expected {len(want)} bytes)")
             # Where the bad byte sat in each buffer it passed through: a fault that repeats
             # at one offset names its buffer. Stream bytes are what the RTL-SDR sent, gaps
             # included, so with no USB loss this is also the offset in a 32 KiB transfer.
             + ("" if magnitude else f"; stream byte {2 * index + bad[0]}, "
                f"{(2 * index + bad[0]) % 32768} into a 32 KiB transfer")]
    if not magnitude:
        # In I/Q each byte names its own stream position modulo 256, so a wrong one says how
        # far from where it belongs it came from. Group the bad bytes by that shift.
        runs = []
        for k in bad:
            shift = (payload[k] - base - (2 * index + k)) & 0xFF
            if runs and runs[-1][1] == k - 1 and runs[-1][2] == shift:
                runs[-1][1] = k
            else:
                runs.append([k, k, shift])
        for first, last, shift in runs[:6]:
            lines.append(f"    bytes {first}-{last}: shifted {shift} (mod 256)")
        if len(runs) > 6:
            lines.append(f"    … {len(runs) - 6} more runs: not a counter at another phase")
    lines.append(f"    first bytes {payload[:16].hex(' ')}  expected {want[:16].hex(' ')}")
    lines.extend("    before: " + r for r in recent[:-1])
    return "\n".join(lines)


def run_udp(args, rate):
    """rtl_udp: control on TCP, the counter in datagrams, a heartbeat to keep them coming."""
    sock = socket.create_connection((args.host, args.port), timeout=10)
    magic, tuner, gains, session, version = struct.unpack(">4sIIII", recv_exact(sock, 20))
    if magic != b"RTLU":
        sys.exit(f"not an rtl_udp server: greeting {magic!r}")
    server = sock.getpeername()
    print(f"connected to {args.host}:{args.port} (rtl_udp v{version}), session {session:08x}, "
          f"tuner type {tuner}, {gains} gain steps")
    no_dongle = tuner == 0 and gains == 0
    if no_dongle:
        print("no dongle on the board: the counter comes from the board itself")

    data = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    data.bind(("", 0))
    data.settimeout(0.5)
    command(sock, SET_SAMPLE_RATE, rate)
    command(sock, SET_TEST_MODE, 1)
    if args.magnitude:
        if version < 2:
            sys.exit(f"the server speaks rtl_udp v{version}, which has no magnitude format")
        command(sock, SET_FORMAT, 1)
    command(sock, START_UDP, data.getsockname()[1])

    running = {"on": True}
    beat = b"RU\x01\x80" + struct.pack(">I", session)

    def heartbeat():
        while running["on"]:
            try:
                data.sendto(beat, (server[0], args.port))
            except OSError:
                pass
            time.sleep(0.1)
    threading.Thread(target=heartbeat, daemon=True).start()

    ramp = bytes(range(256)) * 8
    next_seq = next_index = base = None
    received = datagrams = lost = net_samples = board_samples = late = wrong = 0
    samples_in = format_wrong = 0
    started = last_print = last_packet = time.monotonic()
    stopped_at = None
    settle = started + 0.5   # a dongle's last real samples may precede its counter
    # A wrong datagram is rare and says nothing by its count alone, so the few before it
    # are kept: whether it follows a gap, a loss or a late arrival is most of the diagnosis.
    recent = []
    reports = []
    try:
        while time.monotonic() - started < args.seconds:
            try:
                packet = data.recv(2048)
            except socket.timeout:
                # A stream that stops is not a slow stream: say so instead of averaging the
                # silence into the rate. The board ends a session it hears no heartbeat for.
                if datagrams and time.monotonic() - last_packet > 3:
                    stopped_at = last_packet - started
                    print(f"\n  the stream stopped at {stopped_at:.1f} s", flush=True)
                    break
                continue
            now = last_packet = time.monotonic()
            if len(packet) < 20 or packet[:3] != b"RU\x01":
                continue
            pkt_session, seq, index = struct.unpack(">IIQ", packet[4:20])
            if pkt_session != session:
                continue
            payload = packet[20:]
            # Each datagram says what it carries; a server that ignored the request is a
            # failure here, not something to decode around.
            magnitude = bool(packet[3] & FLAG_MAGNITUDE)
            if magnitude != args.magnitude:
                format_wrong += 1
                continue
            samples = len(payload) if magnitude else len(payload) // 2
            samples_in += samples
            if next_seq is None:
                next_seq, next_index = seq, index
            if index < next_index:
                late += 1
                recent = (recent + [f"seq {seq} index {index} late"])[-4:]
                continue
            gap = index - next_index
            missing = (seq - next_seq) & 0xFFFFFFFF
            if missing and missing < 1 << 31:
                lost += missing
                net_samples += gap
            else:
                board_samples += gap
            recent = (recent + [f"seq {seq} index {index} +{samples}"
                                + (f" after {missing} lost" if missing and missing < 1 << 31 else "")
                                + (f" after a {gap}-sample gap" if gap else "")])[-4:]
            next_seq, next_index = (seq + 1) & 0xFFFFFFFF, index + samples
            datagrams += 1
            received += len(payload)
            # The counter is a function of position: byte 2i of the stream holds base + 2i.
            if now >= settle and payload:
                if base is None:
                    if magnitude:
                        # Not invertible byte by byte, so take the one base whose run of
                        # magnitudes is this datagram.
                        fits = [b for b in range(256)
                                if expected_magnitudes((b + 2 * index) & 0xFF, len(payload)) == payload]
                        if len(fits) == 1:
                            base = fits[0]
                    else:
                        base = (payload[0] - 2 * index) & 0xFF
                if base is None:
                    wrong += 1
                else:
                    start = (base + 2 * index) & 0xFF
                    want = (expected_magnitudes(start, len(payload)) if magnitude
                            else ramp[start:start + len(payload)])
                    if payload != want:
                        wrong += 1
                        if len(reports) < 5:
                            reports.append(describe_wrong(payload, want, magnitude, base, index,
                                                          now - started, recent))
            if now - last_print >= 1:
                last_print = now
                elapsed = now - started
                print(f"\r  {elapsed:5.0f} s  {samples_in / elapsed / 1e6:6.3f} MS/s  "
                      f"{received * 8 / elapsed / 1e6:6.2f} Mbit/s  datagrams {datagrams}  "
                      f"lost {lost}  board dropped {board_samples}  wrong {wrong}",
                      end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        running["on"] = False
        elapsed = time.monotonic() - started
        try:
            command(sock, SET_TEST_MODE, 0)
        except OSError:
            pass
        sock.close()

    delivered = samples_in / elapsed if elapsed > 0 else 0
    sent = datagrams + lost
    print(f"\n\n{datagrams} datagrams, {received} bytes in {elapsed:.1f} s: delivered "
          f"{delivered / 1e6:.3f} MS/s of {rate / 1e6:.3f} requested ({delivered / rate * 100:.1f}%)"
          f"{', as magnitudes' if args.magnitude else ''}")
    print(f"  lost on the network: {lost} datagrams ({lost / max(sent, 1) * 100:.3f}%), "
          f"{net_samples} samples")
    print(f"  dropped by the board: {board_samples} samples  late: {late}  "
          f"content wrong: {wrong} datagrams")
    for report in reports:
        print(report)
    if format_wrong:
        print(f"FAIL: {format_wrong} datagrams in the other format than the one asked for")
        return 1
    if datagrams == 0:
        print("FAIL: nothing arrived")
        return 1
    if stopped_at is not None:
        print(f"FAIL: the board stopped sending at {stopped_at:.1f} s (its serial log says why)")
        return 1
    if wrong:
        print("FAIL: datagrams whose bytes are not the counter at their own position")
        return 1
    if lost or board_samples:
        print("FAIL: samples lost" + (" — the board could not send as fast as the rate"
                                      if board_samples else " on the network"))
        return 1
    print("PASS: every sample, in place, at the full rate" if delivered >= rate * 0.98
          else "FAIL: nothing lost, but delivered under the requested rate")
    return 0 if delivered >= rate * 0.98 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("host", help="board address, e.g. 192.168.7.1 or rtlsdr.local")
    ap.add_argument("--port", type=int, default=None, help="1234, or 1235 with --udp")
    ap.add_argument("--udp", action="store_true", help="rtl_udp instead of rtl_tcp")
    ap.add_argument("--magnitude", action="store_true",
                    help="with --udp: ask for magnitude bytes (rtl_udp v2) and check each one")
    ap.add_argument("--rate", default="2.4M", help="sample rate, e.g. 1.024M, 2.0M, 2.4M")
    ap.add_argument("--seconds", type=float, default=30)
    args = ap.parse_args()
    if args.port is None:
        args.port = 1235 if args.udp else 1234

    rate = parse_rate(args.rate)
    if not any(lo <= rate <= hi for lo, hi in VALID_RATES):
        sys.exit(f"{rate} S/s is not a rate the RTL2832U accepts "
                 "(225001-300000 or 900000-3200000)")
    if args.magnitude and not args.udp:
        sys.exit("--magnitude is an rtl_udp format: add --udp")
    if args.udp:
        return run_udp(args, rate)

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    magic, tuner, gains = struct.unpack(">4sII", recv_exact(sock, 12))
    if magic != b"RTL0":
        sys.exit(f"not an rtl_tcp server: header {magic!r}")
    print(f"connected to {args.host}:{args.port}, tuner type {tuner}, {gains} gain steps")
    # The bridge firmware with nothing on its USB port answers tuner 0 and, asked for test
    # mode, generates the counter itself: the same check with the USB half left out.
    no_dongle = tuner == 0 and gains == 0
    if no_dongle:
        print("no dongle on the board: the counter comes from the board itself, "
              "so this measures Wi-Fi and the board's network side only")

    command(sock, SET_SAMPLE_RATE, rate)
    command(sock, SET_TEST_MODE, 1)

    # Real samples already in flight arrive behind the command; let them pass before
    # counting, or the switch-over reads as a break.
    settle_until = time.monotonic() + 1.0
    while time.monotonic() < settle_until:
        sock.recv(1 << 16)

    expected = None
    total = breaks = lost = 0
    started = last_print = time.monotonic()
    buf = bytearray(1 << 18)
    view = memoryview(buf)
    ramp = bytes(range(256)) * (len(buf) // 256 + 2)
    try:
        while True:
            now = time.monotonic()
            if now - started >= args.seconds:
                break
            try:
                n = sock.recv_into(view)
            except OSError as error:
                print(f"\nconnection lost: {error}")
                break
            if n == 0:
                print("\nserver closed the connection")
                break
            chunk = view[:n]
            if expected is None:
                expected = chunk[0]
            # Fast path: an unbroken chunk is exactly the counter run starting at `expected`.
            if chunk == ramp[expected:expected + n]:
                expected = (expected + n) & 0xFF
            else:
                for value in chunk:
                    if value != expected:
                        breaks += 1
                        lost += (value - expected) & 0xFF
                    expected = (value + 1) & 0xFF
            total += n
            if now - last_print >= 1:
                last_print = now
                elapsed = now - started
                print(f"\r  {elapsed:5.0f} s  {total / elapsed / 2 / 1e6:6.3f} MS/s  "
                      f"{total * 8 / elapsed / 1e6:6.2f} Mbit/s  breaks {breaks}", end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        elapsed = time.monotonic() - started
        try:
            command(sock, SET_TEST_MODE, 0)
        except OSError:
            pass
        sock.close()

    delivered = total / elapsed / 2 if elapsed > 0 else 0
    print(f"\n\n{total} bytes in {elapsed:.1f} s: delivered {delivered / 1e6:.3f} MS/s "
          f"of {rate / 1e6:.3f} requested ({delivered / rate * 100:.1f}%)")
    if total == 0:
        print("FAIL: nothing arrived. " + ("The board did not start its counter: is it running "
              "this repository's firmware?" if no_dongle else "Is a dongle plugged into the board?"))
        return 1
    scope = " (board's own counter, no USB)" if no_dongle else ""
    if breaks:
        print(f"FAIL: {breaks} break(s), at least {lost} bytes lost "
              f"({lost / total * 100:.4f}%)")
        return 1
    if delivered < rate * 0.98:
        print("FAIL: counter unbroken, but the link delivers under the requested rate")
        return 1
    print(f"PASS: counter unbroken at the full rate{scope}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
