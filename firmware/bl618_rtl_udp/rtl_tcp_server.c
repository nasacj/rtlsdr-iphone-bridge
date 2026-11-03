// SPDX-License-Identifier: GPL-2.0-or-later
//
// rtl_tcp over Wi-Fi: the network half of the bridge. The session rules are the Teensy's
// (../teensy41_rtl_tcp/src/bridge.cpp); what differs is the transport underneath and the
// buffer in between.
//
// Two tasks. **copy** moves each filled USB staging buffer into the PSRAM ring as soon as
// it lands, so the USB side always has somewhere to put the next transfer. **serve** owns
// librtlsdr and the socket: it opens the dongle, accepts the client, applies its
// commands, and sends whatever the ring holds as fast as Wi-Fi takes it.
//
// **How a session ends**, every way it can:
//
// | what happened                         | how the board sees it              | action                          |
// |---------------------------------------|------------------------------------|---------------------------------|
// | the app disconnects                   | recv returns 0                     | stop streaming, close           |
// | the app or phone vanishes mid-stream  | nothing is sent for 10 s           | close, stop streaming           |
// | an rtl_udp client stops heartbeating  | no heartbeat for 10 s              | close, stop streaming           |
// | a second connection arrives           | accept while a session runs        | the new one replaces the old    |
// | the dongle is unplugged               | USB disconnect                     | close, close librtlsdr          |
// | a client connects with no dongle      | accept with no device open         | wait 3 s for test mode, see below |
//
// **With no dongle, test mode gets a counter made here instead.** The RTL2832U's test
// mode replaces samples with an incrementing byte, which is what makes loss countable;
// with nothing on the USB port, a client that turns test mode on gets the same counter
// generated on the board at the requested rate, through the same ring and the same send
// loop. So tools/rtl_tcp_counter.py measures Wi-Fi, lwIP and everything of ours with the
// USB half taken out, and then again with it in. Any other client sends something else
// first — a real app turns test mode *off*, because it sends every setting — and is
// closed as before, so nothing ever demodulates a counter it did not ask for.
//
// A new connection replaces the old one rather than being refused, because the case that
// produces two is a client reconnecting after its first connection died without telling
// the board. `rtl_tcp` serves one client, and so does this — across both ports.
//
// **rtl_udp, on port 1235**: the same commands, the samples in UDP datagrams. Measured on
// this board, UDP carried 29 Mbit/s where TCP carried 13 over the same link: TCP keeps
// every segment in lwIP's heap until it is acknowledged, so its rate is that heap over the
// round trip, and the heap is 80 KiB. A datagram is freed as soon as the radio has sent it.
// The wire format, which the client implements and this follows:
//
//   control, TCP 1235  server → client once, 20 bytes:
//                        "RTLU" | tuner u32 | gain count u32 | session u32 | version u32 (1)
//                      client → server: rtl_tcp's 5-byte commands, plus 0x40 START_UDP whose
//                        parameter is the client's UDP port; nothing streams before it; and,
//                        since version 2, 0x41 SET_FORMAT: 1 for magnitudes, 0 for I/Q
//   data, UDP 1235 →   20-byte header, then I/Q, even, at most 1452 bytes (one Ethernet
//     client port        frame): "RU" | version u8 | flags u8 | session u32 | sequence u32 |
//                        index of the first sample u64, all big-endian. Flags bit 0 set:
//                        the payload is one magnitude byte a sample, up to 1452 of them
//
// **Magnitudes are for ADS-B**, which decodes the envelope and throws the phase away: one
// byte a sample instead of two, so 2 MSPS needs 16 Mbit/s where I/Q needs 32 and this
// board in ordinary placement carries about 20–25. The byte, which a client must decode
// the same way: r = sqrt((2I−255)² + (2Q−255)²), sent as 255 × sqrt(r / 360.6),
// rounded — square-root companded, because linear lost frames on real air and this did
// not. It is a lookup on |2I−255| and |2Q−255| (128 values each) in a 16 KiB table.
//   heartbeat, client  8 bytes every 100 ms: "RU" | version u8 | 0x80 | session u32;
//     → UDP 1235         none for 10 s ends the session
//
// A sequence gap is the network losing datagrams; an index jump with consecutive sequences
// is this board dropping samples because Wi-Fi fell behind — the ring says where (see
// sample_ring.h), so the client fills its zeros in the right place. The board never slows
// down for a client: a radio cannot be paused, so the loss is reported instead.

#include "rtl_tcp_server.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include <lwip/inet.h>
#include <lwip/pbuf.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>

#include "bflb_mtimer.h"
#include "bflb_sec_trng.h"
#include "bridge_config.h"
#include "mm.h"
#include "rtl-sdr.h"
#include "rtl_usb.h"
#include "sample_ring.h"
#include "wifi_link.h"

#define STALL_MS 10000
#define REOPEN_MS 2000
#define STATUS_MS 5000
#define DEFAULT_RATE 2048000        // rtl_tcp's own defaults, applied when the dongle opens
#define DEFAULT_FREQUENCY 100000000
#define SEND_CHUNK (16u * 1024u)    // one send() at most; lwIP copies it into its own buffer
#define SYNTH_WAIT_MS 3000          // how long a client with no dongle has to ask for test mode
#define SYNTH_CHUNK (8u * 1024u)    // one push at most: 1.7 ms at 2.4 MSPS
#define UDP_HEADER 20
#define UDP_PAYLOAD 1452            // 1500 - IPv4 20 - UDP 8 - header 20; even
#define UDP_START_UDP 0x40
#define UDP_SET_FORMAT 0x41
#define UDP_VERSION 2               // the greeting's: 2 takes SET_FORMAT
#define UDP_FLAG_MAGNITUDE 0x01
// Silence that ends an rtl_udp session: 10 s, as rtl_tcp's stall rule. It was 2 s, and a
// 2.4 MSPS stream with nothing lost was ended after 33 s for want of heartbeats — the
// board's receive side loses packets while it transmits flat out, and twenty in a row is
// not rare then. The heartbeat only has to show the client is still there.
#define UDP_HEARTBEAT_MS 10000
#define UDP_BURST 32                // datagrams per pass: 3.3 a millisecond at 2.4 MSPS

static rtlsdr_dev_t *g_dev;
static volatile bool g_session;  // read by the copy task: samples go to the ring only then
static volatile bool g_synth;    // read by the synth task: the counter goes to the ring only then
static volatile uint32_t g_synth_rate = DEFAULT_RATE;
static TickType_t g_synth_deadline;

static void blog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void blog(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("[%8lu] %s\r\n", (unsigned long)xTaskGetTickCount(), line);
}

// --- copy: USB staging → PSRAM ring -------------------------------------------------

static void copy_task(void *param)
{
    (void)param;
    for (;;) {
        const uint8_t *data;
        uint32_t length, gap;
        int i = rtl_usb_take(&data, &length, &gap, 100);
        if (i < 0) {
            continue;
        }
        if (g_session && gap > 0) {
            sample_ring_skip(gap);  // what USB lost, placed before this buffer
        }
        if (g_session && length > 0) {
            sample_ring_push(data, length);  // drops and counts when Wi-Fi is too far behind
        }
        rtl_usb_release(i);
    }
}

// --- synth: the test-mode counter, when there is no dongle ------------------------------
//
// Paced against the tick: the bytes owed are what the rate has earned since the start, so
// a late wake-up catches up rather than slowing the stream. A block the ring cannot take
// is dropped and counted, and the counter moves on regardless — what the dongle's would
// do — so the client sees the gap as the break it would be.

static uint8_t g_ramp[SYNTH_CHUNK + 256];

static void synth_task(void *param)
{
    (void)param;
    for (uint32_t i = 0; i < sizeof g_ramp; i++) {
        g_ramp[i] = (uint8_t)i;
    }
    for (;;) {
        if (!g_synth) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t rate = 0;
        TickType_t start = 0;
        uint64_t made = 0;
        uint8_t next = 0;
        while (g_synth) {
            if (g_synth_rate != rate) {  // a new rate starts the pacing again
                rate = g_synth_rate;
                start = xTaskGetTickCount();
                made = 0;
            }
            uint64_t owed = (uint64_t)(xTaskGetTickCount() - start) * rate * 2 / configTICK_RATE_HZ;
            while (made + 2 <= owed && g_synth) {
                uint64_t want = owed - made;
                uint32_t n = want > SYNTH_CHUNK ? SYNTH_CHUNK : (uint32_t)want & ~1u;  // whole I/Q pairs
                sample_ring_push(&g_ramp[next], n);
                next = (uint8_t)(next + n);
                made += n;
            }
            vTaskDelay(1);
        }
    }
}

// --- the dongle ------------------------------------------------------------------------

static TickType_t g_next_open;

static void open_dongle(void)
{
    int r = rtlsdr_open(&g_dev, 0);
    if (r < 0) {
        g_dev = NULL;
        blog("dongle %04x:%04x: rtlsdr_open failed (%d), retrying", rtl_usb_vid(), rtl_usb_pid(), r);
        g_next_open = xTaskGetTickCount() + pdMS_TO_TICKS(REOPEN_MS);
        return;
    }
    rtlsdr_set_sample_rate(g_dev, DEFAULT_RATE);
    rtlsdr_set_center_freq(g_dev, DEFAULT_FREQUENCY);
    rtlsdr_set_tuner_gain_mode(g_dev, 0);
    rtlsdr_reset_buffer(g_dev);
    blog("dongle ready: %04x:%04x, tuner %d, %d gains", rtl_usb_vid(), rtl_usb_pid(),
         (int)rtlsdr_get_tuner_type(g_dev), rtlsdr_get_tuner_gains(g_dev, NULL));
}

static void close_dongle(void)
{
    if (g_dev) {
        rtlsdr_close(g_dev);  // its control transfers fail fast: the device is gone
    }
    g_dev = NULL;
}

// --- the protocol ----------------------------------------------------------------------
//
// rtl_tcp's wire format, unchanged so that any client works — the C spelling of
// ../teensy41_rtl_tcp/src/rtltcp_proto.h:
//
//   server → client, once: "RTL0", tuner type (u32 BE), tuner gain count (u32 BE)
//   server → client, then: raw unsigned 8-bit I/Q, interleaved, forever
//   client → server:       5-byte commands, 1 byte opcode + u32 BE parameter

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void end_session(const char *why);
static uint64_t g_session_bytes;

// A session with no dongle: the sample rate paces the counter, test mode on starts it,
// and anything else means a client that wanted a radio.
static void apply_without_dongle(uint8_t opcode, uint32_t param)
{
    if (opcode == 0x02) {
        g_synth_rate = param;
        blog("no dongle: counter rate %lu S/s", (unsigned long)param);
    } else if (opcode == 0x07 && param != 0) {
        if (!g_synth) {
            sample_ring_clear();
            g_session_bytes = 0;
            g_synth = true;
            blog("no dongle: sending a synthetic test-mode counter at %lu S/s",
                 (unsigned long)g_synth_rate);
        }
    } else if (g_synth) {
        blog("no dongle: command 0x%02x %lu ignored", opcode, (unsigned long)param);
    } else {
        end_session("no dongle");
    }
}

static void start_udp(uint32_t port);

static void set_udp_format(uint32_t format);

static void apply(uint8_t opcode, uint32_t param)
{
    if (opcode == UDP_START_UDP) {  // the transport's, not the radio's: with or without a dongle
        start_udp(param);
        return;
    }
    if (opcode == UDP_SET_FORMAT) {
        set_udp_format(param);
        return;
    }
    if (g_dev == NULL || g_synth) {
        apply_without_dongle(opcode, param);
        return;
    }
    switch (opcode) {
        case 0x01: rtlsdr_set_center_freq(g_dev, param); break;
        case 0x02: rtlsdr_set_sample_rate(g_dev, param); break;
        case 0x03: rtlsdr_set_tuner_gain_mode(g_dev, (int)param); break;
        case 0x04: rtlsdr_set_tuner_gain(g_dev, (int)param); break;  // tenths of a dB
        case 0x05: rtlsdr_set_freq_correction(g_dev, (int)param); break;
        case 0x06: rtlsdr_set_tuner_if_gain(g_dev, (int)(param >> 16), (short)(param & 0xffff)); break;
        case 0x07: rtlsdr_set_testmode(g_dev, (int)param); break;
        case 0x08: rtlsdr_set_agc_mode(g_dev, (int)param); break;
        case 0x09: rtlsdr_set_direct_sampling(g_dev, (int)param); break;
        case 0x0a: rtlsdr_set_offset_tuning(g_dev, (int)param); break;
        case 0x0b: rtlsdr_set_xtal_freq(g_dev, param, 0); break;
        case 0x0c: rtlsdr_set_xtal_freq(g_dev, 0, param); break;
        case 0x0d: {  // gain by index: the tuner's table, clamped, as rtl_tcp does
            int gains[64];
            int n = rtlsdr_get_tuner_gains(g_dev, NULL);
            if (n > 0 && n <= 64) {
                rtlsdr_get_tuner_gains(g_dev, gains);
                rtlsdr_set_tuner_gain(g_dev, gains[param < (uint32_t)n ? param : (uint32_t)n - 1]);
            }
            break;
        }
        case 0x0e: rtlsdr_set_bias_tee(g_dev, (int)param); break;
        default: blog("unknown command 0x%02x %lu ignored", opcode, (unsigned long)param); return;
    }
    blog("cmd 0x%02x %lu", opcode, (unsigned long)param);
}

// --- the session -----------------------------------------------------------------------

static int g_client = -1;
static uint8_t g_cmd[5];
static size_t g_cmd_have;
static TickType_t g_last_progress;
static uint32_t g_usb_drops_at_start, g_ring_drops_at_start;
static struct sockaddr_in g_client_addr;

// rtl_udp: whether this session is one, where its datagrams go, and their numbering.
//
// **lwIP's raw UDP API, not a socket**, as the SDK's own iperf sends: allocate a pbuf in
// lwIP's heap, copy the ring straight into it, and udp_sendto under the core lock. The
// socket version measured 18.7 Mbit/s where iperf's raw sender reached 31.4 on the same
// link a minute later — sendto copied every datagram twice (ring to a staging buffer, then
// into a netbuf) and went through the socket layer for each one. The pbuf is in the Wi-Fi
// RAM, which the radio's DMA reads, and is freed by lwIP once it has been sent.
//
// Most of what is left of each send is the Wi-Fi driver's own code, which is why the
// firmware runs from PSRAM (defconfig, PSRAM_COPY_CODE): from flash, udp_sendto was ~420 us
// a datagram; from PSRAM ~100, and the board went from 1.4 to 2.3 MSPS.
static struct udp_pcb *g_udp_pcb;
static volatile bool g_udp;
static bool g_udp_streaming;
static bool g_udp_magnitude;   // SET_FORMAT 1: one magnitude byte a sample

// The magnitude of a raw pair, by |2I−255| and |2Q−255|: each is odd, 1…255, so (|d|−1)/2
// indexes 128 rows. Built once at start; PSRAM, since on-chip RAM has 20 KiB to spare.
__attribute__((section(".psram_noinit"), aligned(64))) static uint8_t g_magnitude[128 * 128];
static uint8_t g_magnitude_index[256];  // raw byte → row

static void build_magnitude_table(void)
{
    for (int b = 0; b < 256; b++) {
        int d = 2 * b - 255;
        g_magnitude_index[b] = (uint8_t)(((d < 0 ? -d : d) - 1) / 2);
    }
    const float full_scale = 360.62445f;  // sqrt(2 × 255²): both axes at a rail
    for (int a = 0; a < 128; a++) {
        for (int b = 0; b < 128; b++) {
            float di = (float)(2 * a + 1), dq = (float)(2 * b + 1);
            float code = 255.0f * sqrtf(sqrtf(di * di + dq * dq) / full_scale) + 0.5f;
            g_magnitude[a * 128 + b] = code >= 255.0f ? 255 : (uint8_t)code;
        }
    }
}
static struct sockaddr_in g_udp_dest;
static volatile uint32_t g_udp_session;
static uint32_t g_udp_sequence;
static uint64_t g_udp_index;
static uint64_t g_udp_gap_samples;
static uint32_t g_udp_refused;  // sends lwIP had no room for; retried, not lost
static volatile TickType_t g_udp_heard;  // written by lwIP's thread, in the receive callback
static volatile uint32_t g_stat_heartbeats;  // per status interval: 10 a second when all arrive

// Where the sending time goes, per status interval: measured, because the first guess
// about a throughput limit on this board has been wrong more often than right.
static uint32_t g_stat_datagrams, g_stat_passes, g_stat_refused;
static uint64_t g_stat_send_us, g_stat_pass_us;
static uint64_t g_stat_alloc_us, g_stat_copy_us, g_stat_sendto_us;  // the send, in three parts


// Sessions that stopped while samples were flowing for a reason other than the client's
// own: the RTL-SDR pulled, no heartbeat, a send that failed. For the status lamp.
static volatile uint32_t g_interruptions;

static void end_session(const char *why)
{
    if (g_client < 0) {
        return;
    }
    if (g_session && strcmp(why, "closed by the client") != 0
        && strcmp(why, "replaced by a new connection") != 0) {
        g_interruptions++;
    }
    g_session = false;
    g_synth = false;
    rtl_usb_stop_streaming();
    closesocket(g_client);
    g_client = -1;
    rtl_usb_stats_t s = rtl_usb_stats();
    blog("session ended: %s; %llu bytes sent, %lu USB blocks dropped, %lu ring blocks dropped", why,
         (unsigned long long)g_session_bytes, (unsigned long)(s.dropped - g_usb_drops_at_start),
         (unsigned long)(sample_ring_dropped_blocks() - g_ring_drops_at_start));
    if (g_udp) {
        blog("rtl_udp: %lu datagrams, %llu samples reported missing, %lu sends retried",
             (unsigned long)g_udp_sequence, (unsigned long long)g_udp_gap_samples,
             (unsigned long)g_udp_refused);
    }
    g_udp = false;
    g_udp_streaming = false;
}

static void start_session(int sock, const struct sockaddr_in *from, bool udp)
{
    if (g_client >= 0) {
        end_session("replaced by a new connection");
    }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);

    // With no dongle the header says tuner 0 (unknown) and no gains, which is true.
    // rtl_udp's adds a session number, which every datagram and heartbeat then carries.
    uint8_t header[20] = { 'R', 'T', 'L', '0' };
    uint32_t session = 0;
    put_be32(&header[4], g_dev ? (uint32_t)rtlsdr_get_tuner_type(g_dev) : 0);
    put_be32(&header[8], g_dev ? (uint32_t)rtlsdr_get_tuner_gains(g_dev, NULL) : 0);
    size_t header_length = 12;
    if (udp) {
        bflb_trng_readlen((uint8_t *)&session, sizeof session);
        header[3] = 'U';
        put_be32(&header[12], session);
        put_be32(&header[16], UDP_VERSION);
        header_length = 20;
    }
    if (send(sock, header, header_length, 0) != (int)header_length) {
        blog("connection lost while sending the header");
        closesocket(sock);
        return;
    }
    // Non-blocking from here: the loop interleaves commands and samples, and a full send
    // buffer must not hold up a command.
    lwip_fcntl(sock, F_SETFL, lwip_fcntl(sock, F_GETFL, 0) | O_NONBLOCK);

    g_client = sock;
    g_client_addr = *from;
    g_cmd_have = 0;
    g_udp = udp;
    g_udp_streaming = false;
    g_udp_magnitude = false;
    g_udp_session = session;
    g_udp_heard = xTaskGetTickCount();  // the client has this long to say START_UDP
    sample_ring_clear();
    g_session_bytes = 0;
    g_usb_drops_at_start = rtl_usb_stats().dropped;
    g_ring_drops_at_start = sample_ring_dropped_blocks();
    g_last_progress = xTaskGetTickCount();
    if (g_dev == NULL) {
        g_synth_rate = DEFAULT_RATE;
        g_synth_deadline = g_last_progress + pdMS_TO_TICKS(SYNTH_WAIT_MS);
        blog("%s session from %s with no dongle: waiting for test mode", udp ? "rtl_udp" : "rtl_tcp",
             inet_ntoa(from->sin_addr));
        return;
    }
    if (udp) {
        // Nothing streams until the client has a socket to receive it: START_UDP.
        blog("rtl_udp session %08lx from %s", (unsigned long)session, inet_ntoa(from->sin_addr));
        return;
    }
    rtlsdr_reset_buffer(g_dev);  // clear the dongle's FIFO, so the stream starts fresh
    g_session = true;
    rtl_usb_start_streaming();
    blog("session from %s", inet_ntoa(from->sin_addr));
}

// START_UDP: the datagrams go to the control connection's address at the port named, and
// the numbering starts at zero with the stream.
static void start_udp(uint32_t port)
{
    if (!g_udp || port == 0 || port > 0xffff) {
        blog("START_UDP %lu ignored", (unsigned long)port);
        return;
    }
    g_udp_dest = g_client_addr;
    g_udp_dest.sin_port = htons((uint16_t)port);
    g_udp_sequence = 0;
    g_udp_index = 0;
    g_udp_gap_samples = 0;
    g_udp_refused = 0;
    g_udp_heard = xTaskGetTickCount();
    sample_ring_clear();
    g_udp_streaming = true;
    if (g_dev) {
        rtlsdr_reset_buffer(g_dev);
        g_session = true;
        rtl_usb_start_streaming();
    }
    blog("rtl_udp: streaming to %s:%lu", inet_ntoa(g_udp_dest.sin_addr), (unsigned long)port);
}

// SET_FORMAT: before START_UDP, so every datagram of the stream is in one format; each
// says which in its flags anyway, so a client never has to trust that this was honoured.
static void set_udp_format(uint32_t format)
{
    if (!g_udp || format > 1) {
        blog("SET_FORMAT %lu ignored", (unsigned long)format);
        return;
    }
    g_udp_magnitude = format == 1;
    blog("rtl_udp: %s", g_udp_magnitude ? "magnitudes, one byte a sample" : "I/Q");
}

// Every datagram arriving on port 1235, in lwIP's own thread: a heartbeat for the current
// session moves the deadline, anything else is freed at once, so a client that has gone
// and keeps beating for a while costs no heap.
static void heartbeat_received(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                               u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;
    uint8_t in[8];
    if (p->tot_len == sizeof in && pbuf_copy_partial(p, in, sizeof in, 0) == sizeof in && in[0] == 'R' &&
        in[1] == 'U' && in[2] == 1 && in[3] == 0x80 && g_udp &&
        (((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 8) | in[7]) == g_udp_session) {
        g_udp_heard = xTaskGetTickCount();
        g_stat_heartbeats++;
    }
    pbuf_free(p);
}

// Whole datagrams from the ring, each starting where its samples belong. A run shorter
// than a datagram is sent only when something lies beyond it — a gap, or the end of the
// storage — and otherwise waits for the next USB block, so the stream is not cut into
// slivers at every block boundary.
static void service_udp(void)
{
    if ((int32_t)(xTaskGetTickCount() - g_udp_heard) > (int32_t)pdMS_TO_TICKS(UDP_HEARTBEAT_MS)) {
        end_session(g_udp_streaming ? "no heartbeat for 10 s" : "no START_UDP");
        return;
    }
    if (!g_udp_streaming) {
        return;
    }
    uint64_t pass_start = bflb_mtimer_get_time_us();
    g_stat_passes++;
    ip_addr_t to;
    ip_addr_set_ip4_u32(&to, g_udp_dest.sin_addr.s_addr);
    u16_t to_port = ntohs(g_udp_dest.sin_port);
    for (int k = 0; k < UDP_BURST; k++) {
        const uint8_t *data;
        uint32_t gap = 0;
        uint32_t run = sample_ring_peek(&data, &gap);
        g_udp_index += gap;  // the missing samples come before whatever is sent next
        g_udp_gap_samples += gap;
        // `length` is ring bytes taken, `out` the payload sent: the same for I/Q, half for
        // magnitudes, which fill a datagram from twice as many ring bytes.
        uint32_t most = g_udp_magnitude ? 2 * UDP_PAYLOAD : UDP_PAYLOAD;
        uint32_t length = (run < most ? run : most) & ~1u;
        if (length == 0 || (length < most && sample_ring_used() <= run)) {
            break;
        }
        uint32_t out = g_udp_magnitude ? length / 2 : length;
        uint64_t send_start = bflb_mtimer_get_time_us();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, UDP_HEADER + out, PBUF_RAM);
        uint64_t allocated = bflb_mtimer_get_time_us();
        g_stat_alloc_us += allocated - send_start;
        if (p == NULL) {
            // lwIP's heap is full of datagrams the radio has not sent yet. The samples stay
            // in the ring; if it fills, the loss is the ring's and is reported as a gap.
            g_udp_refused++;
            g_stat_refused++;
            break;
        }
        uint8_t *h = p->payload;
        h[0] = 'R';
        h[1] = 'U';
        h[2] = 1;
        h[3] = g_udp_magnitude ? UDP_FLAG_MAGNITUDE : 0;
        put_be32(&h[4], g_udp_session);
        put_be32(&h[8], g_udp_sequence);
        put_be32(&h[12], (uint32_t)(g_udp_index >> 32));
        put_be32(&h[16], (uint32_t)g_udp_index);
        if (g_udp_magnitude) {
            uint8_t *m = h + UDP_HEADER;
            for (uint32_t n = 0; n < out; n++) {
                m[n] = g_magnitude[g_magnitude_index[data[2 * n]] * 128 + g_magnitude_index[data[2 * n + 1]]];
            }
        } else {
            memcpy(h + UDP_HEADER, data, length);
        }
        uint64_t copied = bflb_mtimer_get_time_us();
        g_stat_copy_us += copied - allocated;
        LOCK_TCPIP_CORE();
        err_t err = udp_sendto(g_udp_pcb, p, &to, to_port);
        UNLOCK_TCPIP_CORE();
        pbuf_free(p);
        uint64_t sent = bflb_mtimer_get_time_us();
        g_stat_sendto_us += sent - copied;

        g_stat_send_us += sent - send_start;
        if (err != ERR_OK) {
            g_udp_refused++;
            g_stat_refused++;
            break;
        }
        sample_ring_consume(length);
        g_udp_sequence++;
        g_udp_index += length / 2;
        g_session_bytes += out;
        g_stat_datagrams++;
    }
    g_stat_pass_us += bflb_mtimer_get_time_us() - pass_start;
}

static void service_session(void)
{
    // Commands. Each can be several control transfers; USB keeps streaming meanwhile, so
    // only the network waits.
    uint8_t in[64];
    for (;;) {
        int n = recv(g_client, in, sizeof in, 0);
        if (n == 0) {
            end_session("closed by the client");
            return;
        }
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break;
            }
            end_session("receive error");
            return;
        }
        // TCP is a byte stream: a command can arrive split across reads, or several in one.
        for (int i = 0; i < n; i++) {
            g_cmd[g_cmd_have++] = in[i];
            if (g_cmd_have == 5) {
                g_cmd_have = 0;
                apply(g_cmd[0], ((uint32_t)g_cmd[1] << 24) | ((uint32_t)g_cmd[2] << 16) |
                                    ((uint32_t)g_cmd[3] << 8) | g_cmd[4]);
                if (g_client < 0) {
                    return;  // the command ended the session
                }
            }
        }
    }
    if (g_dev == NULL && !g_synth && (int32_t)(xTaskGetTickCount() - g_synth_deadline) >= 0) {
        end_session("no dongle");
        return;
    }
    if (g_udp) {
        service_udp();
        return;
    }

    // Samples: whatever the TCP send buffer takes, oldest first.
    bool pending = false;
    for (;;) {
        const uint8_t *data;
        uint32_t run = sample_ring_peek(&data, NULL);  // rtl_tcp has no way to say "gap"
        if (run == 0) {
            break;
        }
        pending = true;
        int n = send(g_client, data, run < SEND_CHUNK ? run : SEND_CHUNK, 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break;
            }
            end_session("send error");
            return;
        }
        if (n == 0) {
            break;
        }
        sample_ring_consume((uint32_t)n);
        g_session_bytes += (uint32_t)n;
        g_last_progress = xTaskGetTickCount();
        pending = false;
    }
    if (!pending) {
        g_last_progress = xTaskGetTickCount();
    }

    // A peer that stopped reading: TCP would keep retransmitting for minutes.
    if (xTaskGetTickCount() - g_last_progress > pdMS_TO_TICKS(STALL_MS)) {
        end_session("no progress for 10 s");
    }
}

// Each task's share of the CPU since the last status line. Interrupt time lands on the
// task it interrupted, which is good enough to tell working from waiting.
static void print_cpu(void)
{
    enum { MAX_TASKS = 24 };
    static TaskStatus_t now[MAX_TASKS];
    static struct {
        TaskHandle_t task;
        uint32_t runtime;
    } before[MAX_TASKS];
    static UBaseType_t before_count;

    UBaseType_t n = uxTaskGetSystemState(now, MAX_TASKS, NULL);
    uint32_t delta[MAX_TASKS];
    uint64_t total = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t previous = now[i].ulRunTimeCounter;  // a task new since last time counts from here
        for (UBaseType_t j = 0; j < before_count; j++) {
            if (before[j].task == now[i].xHandle) {
                previous = before[j].runtime;
                break;
            }
        }
        delta[i] = now[i].ulRunTimeCounter - previous;
        total += delta[i];
    }
    if (before_count != 0 && total != 0) {
        char line[256];
        int at = snprintf(line, sizeof line, "cpu:");
        for (UBaseType_t i = 0; i < n && at < (int)sizeof line - 24; i++) {
            uint32_t permille = (uint32_t)(delta[i] * 1000ull / total);
            if (permille >= 10) {
                at += snprintf(line + at, sizeof line - at, " %s %lu.%lu%%", now[i].pcTaskName,
                               (unsigned long)(permille / 10), (unsigned long)(permille % 10));
            }
        }
        printf("[%8lu] %s\r\n", (unsigned long)xTaskGetTickCount(), line);  // longer than blog's
    }
    for (UBaseType_t i = 0; i < n; i++) {
        before[i].task = now[i].xHandle;
        before[i].runtime = now[i].ulRunTimeCounter;
    }
    before_count = n;
}

// Guard checks that found the Wi-Fi word written (sample_ring.c), and how many status lines
// since the last check: the guard is 1 MiB, so it is read once a minute, not every 5 s.
static uint32_t g_guard_writes, g_guard_wait;

static void print_status(void)
{
    print_cpu();
    rtl_usb_stats_t s = rtl_usb_stats();
    // Free memory per heap: both crashes on the first day of real hardware were memory, and
    // neither number was anywhere to be seen.
    // The error numbers that occurred, `code×count`: which one it is decides the repair.
    char codes[64] = "";
    for (int c = 0, at = 0; c < 16; c++) {
        if (s.bulk_error_codes[c]) {
            at += snprintf(codes + at, sizeof codes - at, " %d×%lu", c, (unsigned long)s.bulk_error_codes[c]);
            if (at >= (int)sizeof codes) {
                break;
            }
        }
    }
    blog("usb transfers failed: %lu after arriving whole, %lu partly, %lu empty", (unsigned long)s.errors_full,
         (unsigned long)s.errors_partial, (unsigned long)s.errors_empty);
    if (++g_guard_wait >= 60000 / STATUS_MS) {
        g_guard_wait = 0;
        uint32_t at = 0;
        uint8_t value = 0;
        uint32_t changed = sample_ring_guard_check(&at, &value);
        if (changed) {
            g_guard_writes++;
            blog("ring guard written: %lu bytes, first at +0x%lx = 0x%02x (%lu times so far)", (unsigned long)changed,
                 (unsigned long)at, value, (unsigned long)g_guard_writes);
        }
    }
    blog("wifi %s %s, dongle %s, %s; usb %llu B in, %lu blocks, %lu dropped, %lu bulk errors%s, %llu B lost; "
         "ring %lu KiB (peak %lu), %lu dropped; free KiB ocram %u wram %u psram %u",
         wifi_link_up() ? "up" : "down", wifi_link_address(),
         g_dev ? "open" : (rtl_usb_ready() ? "claimed" : "absent"),
         g_client < 0 ? "idle" : (g_synth ? (g_udp ? "synthetic counter, udp" : "synthetic counter")
                                          : (g_udp ? "streaming, udp" : "streaming")),
         (unsigned long long)s.bytes_in, (unsigned long)s.filled, (unsigned long)s.dropped,
         (unsigned long)s.bulk_errors, codes, (unsigned long long)s.bytes_lost,
         (unsigned long)(sample_ring_used() / 1024),
         (unsigned long)(sample_ring_high_water() / 1024), (unsigned long)sample_ring_dropped_blocks(),
         (unsigned)(kfree_size(MM_HEAP_OCRAM_0) / 1024), (unsigned)(kfree_size(MM_HEAP_WRAM_0) / 1024),
         (unsigned)(kfree_size(MM_HEAP_PSRAM_0) / 1024));
    if (g_stat_passes) {
        uint32_t n = g_stat_datagrams ? g_stat_datagrams : 1;
        blog("rtl_udp last %ds: %lu datagrams/s, %lu passes/s, %lu us a send (alloc %lu, copy %lu, "
             "sendto %lu), %lu us a pass, %lu refused, %lu heartbeats",
             STATUS_MS / 1000, (unsigned long)(g_stat_datagrams * 1000 / STATUS_MS),
             (unsigned long)(g_stat_passes * 1000 / STATUS_MS), (unsigned long)(g_stat_send_us / n),
             (unsigned long)(g_stat_alloc_us / n), (unsigned long)(g_stat_copy_us / n),
             (unsigned long)(g_stat_sendto_us / n), (unsigned long)(g_stat_pass_us / g_stat_passes),
             (unsigned long)g_stat_refused, (unsigned long)g_stat_heartbeats);
    }
    g_stat_heartbeats = 0;
    g_stat_datagrams = g_stat_passes = g_stat_refused = 0;
    g_stat_send_us = g_stat_pass_us = 0;
    g_stat_alloc_us = g_stat_copy_us = g_stat_sendto_us = 0;
}

// A non-blocking socket bound to `port` on every interface; a listening one for TCP.
static int open_socket(int type, uint16_t port)
{
    int sock = socket(AF_INET, type, 0);
    if (sock < 0) {
        blog("no socket for port %u", port);
        return -1;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        (type == SOCK_STREAM && listen(sock, 2) < 0)) {
        blog("cannot %s port %u", type == SOCK_STREAM ? "listen on" : "bind", port);
        closesocket(sock);
        return -1;
    }
    lwip_fcntl(sock, F_SETFL, lwip_fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    return sock;
}

static void serve_task(void *param)
{
    (void)param;
    while (!wifi_link_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listener = open_socket(SOCK_STREAM, BRIDGE_RTL_TCP_PORT);
    if (listener < 0) {
        vTaskDelete(NULL);
        return;
    }
    // rtl_udp beside it. A failure here costs only rtl_udp, so rtl_tcp carries on.
    int udp_listener = open_socket(SOCK_STREAM, BRIDGE_RTL_UDP_PORT);
    if (udp_listener >= 0) {
        LOCK_TCPIP_CORE();
        g_udp_pcb = udp_new();
        if (g_udp_pcb && udp_bind(g_udp_pcb, IP_ANY_TYPE, BRIDGE_RTL_UDP_PORT) == ERR_OK) {
            udp_recv(g_udp_pcb, heartbeat_received, NULL);
        } else if (g_udp_pcb) {
            udp_remove(g_udp_pcb);
            g_udp_pcb = NULL;
        }
        UNLOCK_TCPIP_CORE();
    }
    blog("rtl_tcp listening on port %d, rtl_udp on %d%s", BRIDGE_RTL_TCP_PORT, BRIDGE_RTL_UDP_PORT,
         g_udp_pcb ? "" : " (unavailable)");

    TickType_t next_status = xTaskGetTickCount();
    for (;;) {
        if (rtl_usb_take_lost()) {
            blog("dongle unplugged");
            end_session("dongle unplugged");
            close_dongle();
        }
        if (g_dev == NULL && rtl_usb_ready() && (int32_t)(xTaskGetTickCount() - g_next_open) >= 0) {
            open_dongle();
        }

        struct sockaddr_in from;
        socklen_t len = sizeof from;
        int sock = accept(listener, (struct sockaddr *)&from, &len);
        if (sock >= 0) {
            start_session(sock, &from, false);
        }
        if (g_udp_pcb) {
            len = sizeof from;
            sock = accept(udp_listener, (struct sockaddr *)&from, &len);
            if (sock >= 0) {
                start_session(sock, &from, true);
            }
        }

        if (g_client >= 0) {
            service_session();
        }

        if ((int32_t)(xTaskGetTickCount() - next_status) >= 0) {
            next_status = xTaskGetTickCount() + pdMS_TO_TICKS(STATUS_MS);
            print_status();
        }

        // Idle, wait for something to happen; streaming, come straight back for the next
        // stretch of the ring. 1 ms is about 4.8 KB at 2.4 MSPS, well inside lwIP's buffer.
        vTaskDelay(pdMS_TO_TICKS(g_client >= 0 ? 1 : 20));
    }
}

bool rtl_tcp_server_dongle_open(void)
{
    return g_dev != NULL;
}

bool rtl_tcp_server_streaming(void)
{
    return g_session;
}

bool rtl_tcp_server_client(void)
{
    return g_client >= 0;
}

uint32_t rtl_tcp_server_interruptions(void)
{
    return g_interruptions;
}

void rtl_tcp_server_start(void)
{
    build_magnitude_table();
    sample_ring_guard_fill();
    int placement = sample_ring_wifi_word_placement();
    blog("ring guard %s 0xa8201300, the word Wi-Fi zeroes", placement > 0 ? "covers"
                                                          : placement < 0 ? "MISSES: the ring holds"
                                                                          : "does not cover (outside the ring too)");
    // The copy task outranks the server: a full set of staging buffers is lost samples,
    // a full ring is only a delay.
    xTaskCreate(copy_task, "copy", 1024, NULL, 20, NULL);
    xTaskCreate(synth_task, "synth", 512, NULL, 20, NULL);
    xTaskCreate(serve_task, "serve", 4096, NULL, 15, NULL);
}
