// SPDX-License-Identifier: GPL-2.0-or-later
//
// See rtl_usb.h for what this does and why there is one transfer in flight.

#include "rtl_usb.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "bflb_core.h"
#include "usbh_core.h"

#include "libusb.h"

// --- staging buffers ------------------------------------------------------------------
//
// The controller writes these by DMA, so they sit in the non-cacheable alias of SRAM, as
// CherryUSB's own buffers do; the copy task reads them straight out and nothing has to be
// invalidated. Each is in exactly one state:
//
//   free ──submit──▶ queued ──done──▶ filled ──copy task takes it──▶ taken ──released──▶ free
//                      ▲                 │
//                      └─ stolen (drop) ─┘
//
// **USB never waits for the copy task**: the RTL2832U's FIFO lasts well under a millisecond
// at 2.4 MSPS, so if nothing is free the oldest filled buffer is reused and counted.

enum stage_state { STAGE_FREE, STAGE_QUEUED, STAGE_FILLED, STAGE_TAKEN };

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t g_stage[RTL_USB_STAGES][RTL_USB_XFER_SIZE];
static volatile uint8_t g_state[RTL_USB_STAGES];
static volatile uint32_t g_length[RTL_USB_STAGES];
static volatile uint32_t g_order[RTL_USB_STAGES];
// **Bytes lost before each filled buffer, and before whatever fills next** — a filled buffer
// reused before the copy task took it. The loss travels with the next buffer to fill, which
// is where in the stream it happened, so rtl_udp numbers everything after it correctly.
static volatile uint32_t g_gap[RTL_USB_STAGES];
static uint32_t g_gap_pending;
static uint32_t g_next_order = 1;
static volatile int g_queued = -1;  // the buffer the URB in flight fills, or -1

static rtl_usb_stats_t g_stats;
static SemaphoreHandle_t g_filled_sem;
// **Held across every control transfer, and taken by rtl_disconnect before it returns.**
// CherryUSB deletes `hport->mutex` on unplug (usbh_hubport_release), straight after calling
// the class driver's disconnect — so a transfer that had checked the device was there and
// then reached for that mutex would take a deleted one. Unplugging while the app retunes
// is an ordinary thing to do. Holding this mutex until disconnect has seen `g_claimed`
// cleared closes the gap: release cannot reach the delete while a transfer is between its
// check and its last use of the hport.
static SemaphoreHandle_t g_ctrl_mutex;

// --- the device -----------------------------------------------------------------------

static struct usbh_hubport *volatile g_hport;
static struct usb_endpoint_descriptor *g_bulk_in;
static struct usbh_urb g_bulk_urb;
static volatile bool g_claimed;
static volatile bool g_lost;
static volatile bool g_streaming;
static uint16_t g_vid, g_pid;

// ep0 for librtlsdr: the setup packet and data go through DMA too.
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static struct usb_setup_packet g_setup;
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t g_ctrl_buf[256];

static size_t enter(void) { return usb_osal_enter_critical_section(); }
static void leave(size_t flags) { usb_osal_leave_critical_section(flags); }

static int oldest_filled(void)
{
    int best = -1;
    for (int i = 0; i < RTL_USB_STAGES; i++) {
        if (g_state[i] == STAGE_FILLED && (best < 0 || g_order[i] < g_order[best])) {
            best = i;
        }
    }
    return best;
}

// A buffer for the next transfer: a free one, or else the oldest filled one, dropped.
// Inside a critical section.
static int pick_stage(void)
{
    for (int i = 0; i < RTL_USB_STAGES; i++) {
        if (g_state[i] == STAGE_FREE) {
            return i;
        }
    }
    int oldest = oldest_filled();
    if (oldest >= 0) {
        g_stats.dropped++;
        // Its samples and the gap in front of it pass to the next buffer in the stream:
        // the oldest still filled after it, or, with none, whatever fills next.
        uint32_t lost = g_gap[oldest] + g_length[oldest];
        g_stats.bytes_lost += g_length[oldest];
        g_state[oldest] = STAGE_FREE;
        int next = oldest_filled();
        if (next >= 0) {
            g_gap[next] += lost;
        } else {
            g_gap_pending += lost;
        }
    }
    return oldest;
}

static void bulk_complete(void *arg, int nbytes);

// Puts the next transfer with the controller. Inside a critical section; from the
// completion interrupt or from a task.
static void submit_next(void)
{
    if (!g_streaming || g_lost || g_queued >= 0 || g_hport == NULL || g_bulk_in == NULL) {
        return;
    }
    int i = pick_stage();
    if (i < 0) {
        return;  // every buffer is with the copy task: it is not running at all
    }
    g_state[i] = STAGE_QUEUED;
    g_length[i] = 0;
    g_queued = i;
    // The URB is reused, so its data toggle carries over from the last transfer, as
    // the endpoint's own toggle does.
    usbh_bulk_urb_fill(&g_bulk_urb, g_hport, g_bulk_in, g_stage[i], RTL_USB_XFER_SIZE, 0,
                       bulk_complete, NULL);
    int ret = usbh_submit_urb(&g_bulk_urb);
    if (ret < 0) {
        g_state[i] = STAGE_FREE;
        g_queued = -1;
        g_stats.submit_failures++;
    }
}

// From the USB interrupt: the transfer in flight finished. Record it and put the next
// one with the controller before anything else.
static void bulk_complete(void *arg, int nbytes)
{
    (void)arg;
    size_t flags = enter();
    int i = g_queued;
    g_queued = -1;
    bool filled = false;
    if (i >= 0) {
        uint32_t got = g_bulk_urb.actual_length;
        if (nbytes >= 0) {
            filled = true;
            g_state[i] = STAGE_FILLED;
            g_length[i] = (uint32_t)nbytes;
            g_order[i] = g_next_order++;
            g_gap[i] = g_gap_pending;
            g_gap_pending = 0;
            g_stats.filled++;
            g_stats.bytes_in += (uint32_t)nbytes;
        } else {
            g_stats.bulk_errors++;
            int code = -nbytes;
            g_stats.bulk_error_codes[code < 16 ? code : 0]++;
            // **A failed transfer is delivered, not discarded, and that is a measurement.**
            // On this board about one 32 KiB transfer in a hundred ends in -USB_ERR_IO
            // (the EHCI's transaction or data-buffer error, which CherryUSB does not tell
            // apart), and the first versions threw each away: 1.4% of every stream,
            // joined up silently. Counted by how much had arrived, every one measured had
            // arrived **whole** — 12 of 12 in a minute at 1.024 MSPS, the RTL-SDR's test
            // counter checked byte for byte at the far end, nothing wrong. The packets in
            // actual_length were acknowledged, so the RTL-SDR has moved past them; one that
            // failed was not, so it is sent again. So nothing is lost and the stream
            // continues with the next transfer, provided its data toggle is where the
            // RTL-SDR's is: the driver leaves the URB's toggle as it was at submit when a
            // transfer fails, so it is advanced here by the packets that did arrive.
            // Smaller transfers fail far less (16 KiB: 0.13 errors a MB against 0.43),
            // which fits a fault in chaining qTDs — one holds 20 KiB — rather than the
            // wiring, which would fail by the byte.
            if (g_streaming && nbytes != -USB_ERR_SHUTDOWN && got > 0) {
                g_state[i] = STAGE_FILLED;
                g_length[i] = got;
                g_order[i] = g_next_order++;
                g_gap[i] = g_gap_pending;
                g_gap_pending = 0;
                g_stats.filled++;
                g_stats.bytes_in += got;
                filled = true;
                if (got >= RTL_USB_XFER_SIZE) {
                    g_stats.errors_full++;
                } else {
                    g_stats.errors_partial++;
                }
                uint32_t mps = g_bulk_in->wMaxPacketSize & 0x7ff;
                uint32_t packets = (got + mps - 1) / mps;
                if (packets & 1) {
                    g_bulk_urb.data_toggle = !g_bulk_urb.data_toggle;
                }
            } else {
                // Nothing arrived, so nothing was acknowledged and the RTL-SDR sends it
                // again: no gap. Never seen yet; counted so that it would be.
                g_state[i] = STAGE_FREE;
                if (g_streaming && nbytes != -USB_ERR_SHUTDOWN) {
                    g_stats.errors_empty++;
                }
            }
        }
    }
    submit_next();
    leave(flags);

    if (filled) {
        if (xPortIsInsideInterrupt()) {
            BaseType_t woken = pdFALSE;
            xSemaphoreGiveFromISR(g_filled_sem, &woken);
            portYIELD_FROM_ISR(woken);
        } else {
            xSemaphoreGive(g_filled_sem);
        }
    }
}

// --- the class driver -----------------------------------------------------------------
//
// Interface 0, vendor-specific, with a bulk-IN endpoint: what every RTL2832U stick
// presents. Whether it is a *known* stick is librtlsdr's question, answered in
// rtlsdr_open from the VID/PID; claiming here only reserves the hardware.

static int rtl_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_interface_altsetting *alt = &hport->config.intf[intf].altsetting[0];
    struct usb_endpoint_descriptor *in = NULL;

    for (uint8_t e = 0; e < alt->intf_desc.bNumEndpoints && e < CONFIG_USBHOST_MAX_ENDPOINTS; e++) {
        struct usb_endpoint_descriptor *d = &alt->ep[e].ep_desc;
        if ((d->bEndpointAddress & 0x80) && USB_GET_ENDPOINT_TYPE(d->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
            in = d;
            break;
        }
    }
    if (in == NULL || g_claimed) {
        return -USB_ERR_NODEV;
    }

    size_t flags = enter();
    for (int i = 0; i < RTL_USB_STAGES; i++) {
        g_state[i] = STAGE_FREE;
        g_length[i] = 0;
    }
    g_queued = -1;
    memset(&g_stats, 0, sizeof g_stats);
    memset(&g_bulk_urb, 0, sizeof g_bulk_urb);  // data toggle 0 after SET_CONFIGURATION
    g_bulk_in = in;
    g_hport = hport;
    g_vid = hport->device_desc.idVendor;
    g_pid = hport->device_desc.idProduct;
    g_streaming = false;
    g_lost = false;
    g_claimed = true;
    leave(flags);

    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN, "/dev/rtlsdr");
    hport->config.intf[intf].priv = &g_bulk_urb;
    printf("[usb] RTL2832U %04x:%04x claimed, bulk in 0x%02x, %u-byte packets\r\n", g_vid, g_pid,
           in->bEndpointAddress, (unsigned)USB_GET_MAXPACKETSIZE(in->wMaxPacketSize));
    return 0;
}

static int rtl_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    // Waits out a control transfer in progress. Not for long: release has already marked
    // the port disconnected and killed ep0, so the transfer is failing as this waits.
    xSemaphoreTake(g_ctrl_mutex, portMAX_DELAY);
    g_streaming = false;
    if (g_queued >= 0) {
        usbh_kill_urb(&g_bulk_urb);
    }
    size_t flags = enter();
    g_claimed = false;
    g_lost = true;
    g_hport = NULL;
    g_bulk_in = NULL;
    g_queued = -1;
    leave(flags);
    xSemaphoreGive(g_ctrl_mutex);
    xSemaphoreGive(g_filled_sem);  // wake the copy task so it notices
    printf("[usb] RTL2832U disconnected\r\n");
    return 0;
}

static const struct usbh_class_driver rtl_class_driver = {
    .driver_name = "rtlsdr",
    .connect = rtl_connect,
    .disconnect = rtl_disconnect,
};

CLASS_INFO_DEFINE const struct usbh_class_info rtl_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_NUM,
    .bInterfaceClass = 0xff,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .bInterfaceNumber = 0,
    .id_table = NULL,
    .class_driver = &rtl_class_driver,
};

// --- the public calls -----------------------------------------------------------------

void rtl_usb_init(void)
{
    g_filled_sem = xSemaphoreCreateCounting(64, 0);
    g_ctrl_mutex = xSemaphoreCreateMutex();
    struct bflb_device_s *usb = bflb_device_get_by_name(BFLB_NAME_USB_V2);
    if (usb == NULL) {
        printf("[usb] no USB controller\r\n");
        return;
    }
    usbh_initialize(0, usb->reg_base, NULL);
}

// `g_hport` read once: rtl_disconnect can clear it between two reads.
bool rtl_usb_ready(void)
{
    struct usbh_hubport *hport = g_hport;
    return g_claimed && !g_lost && hport != NULL && hport->connected;
}

bool rtl_usb_take_lost(void)
{
    if (!g_lost) {
        return false;
    }
    size_t flags = enter();
    g_lost = false;
    for (int i = 0; i < RTL_USB_STAGES; i++) {
        g_state[i] = STAGE_FREE;
    }
    leave(flags);
    return true;
}

uint16_t rtl_usb_vid(void) { return g_vid; }
uint16_t rtl_usb_pid(void) { return g_pid; }

static int to_libusb(int err)
{
    switch (-err) {
        case USB_ERR_STALL: return LIBUSB_ERROR_PIPE;
        case USB_ERR_TIMEOUT: return LIBUSB_ERROR_TIMEOUT;
        case USB_ERR_NODEV:
        case USB_ERR_NOTCONN:
        case USB_ERR_SHUTDOWN: return LIBUSB_ERROR_NO_DEVICE;
        case USB_ERR_NOMEM: return LIBUSB_ERROR_NO_MEM;
        case USB_ERR_INVAL: return LIBUSB_ERROR_INVALID_PARAM;
        case USB_ERR_BUSY: return LIBUSB_ERROR_BUSY;
        default: return LIBUSB_ERROR_IO;
    }
}

int rtl_usb_control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                    uint8_t *data, uint16_t length, uint32_t timeout_ms)
{
    if (length > sizeof g_ctrl_buf) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    xSemaphoreTake(g_ctrl_mutex, portMAX_DELAY);
    struct usbh_hubport *hport = g_hport;
    if (!rtl_usb_ready() || hport == NULL) {
        xSemaphoreGive(g_ctrl_mutex);
        return LIBUSB_ERROR_NO_DEVICE;
    }
    const bool in = (request_type & 0x80) != 0;

    usb_osal_mutex_take(hport->mutex);
    g_setup.bmRequestType = request_type;
    g_setup.bRequest = request;
    g_setup.wValue = value;
    g_setup.wIndex = index;
    g_setup.wLength = length;
    if (!in && length) {
        memcpy(g_ctrl_buf, data, length);
    }
    // ep0 directly, once: see rtl_usb.h on why not usbh_control_transfer.
    struct usbh_urb *urb = &hport->ep0_urb;
    usbh_control_urb_fill(urb, hport, &g_setup, length ? g_ctrl_buf : NULL, length,
                          timeout_ms ? timeout_ms : CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT, NULL, NULL);
    int ret = usbh_submit_urb(urb);
    // **CherryUSB's EHCI counts the 8-byte SETUP stage in actual_length** — it sums every
    // qTD of the transfer — so a 4-byte read reports 12, and its own usbh_control_transfer
    // passes that on. librtlsdr compares the count with what it asked for: r82xx_read logged
    // "i2c rd failed=12 len=4" on every tuner read, the R820T's setup stopped half way, and
    // the counter test still passed because test mode bypasses the tuner. Only the data
    // stage is the caller's, clamped to what was asked for.
    int got = (int)urb->actual_length - (int)sizeof(struct usb_setup_packet);
    if (got < 0) {
        got = 0;
    }
    if (got > length) {
        got = length;
    }
    if (ret >= 0 && in && got > 0) {
        memcpy(data, g_ctrl_buf, (size_t)(got < length ? got : length));
    }
    usb_osal_mutex_give(hport->mutex);
    xSemaphoreGive(g_ctrl_mutex);

    if (ret < 0) {
        return to_libusb(ret);
    }
    return in ? got : length;
}

void rtl_usb_start_streaming(void)
{
    size_t flags = enter();
    for (int i = 0; i < RTL_USB_STAGES; i++) {
        if (g_state[i] == STAGE_FILLED) {
            g_state[i] = STAGE_FREE;  // the previous session's samples are not this one's
        }
    }
    g_gap_pending = 0;  // nor its losses
    g_streaming = true;
    submit_next();  // no-op while the last session's transfer is still in flight
    leave(flags);
}

void rtl_usb_stop_streaming(void) { g_streaming = false; }

int rtl_usb_take(const uint8_t **data, uint32_t *length, uint32_t *gap_bytes, uint32_t wait_ms)
{
    for (;;) {
        size_t flags = enter();
        int i = oldest_filled();
        if (i >= 0) {
            g_state[i] = STAGE_TAKEN;
            *data = g_stage[i];
            *length = g_length[i];
            *gap_bytes = g_gap[i];
            g_gap[i] = 0;
        }
        leave(flags);
        if (i >= 0) {
            return i;
        }
        if (wait_ms == 0 || xSemaphoreTake(g_filled_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            return -1;
        }
        wait_ms = 0;  // one wait, then report what there is
    }
}

void rtl_usb_release(int index)
{
    if (index < 0 || index >= RTL_USB_STAGES) {
        return;
    }
    size_t flags = enter();
    if (g_state[index] == STAGE_TAKEN) {
        g_state[index] = STAGE_FREE;
    }
    submit_next();  // if USB ever ran out of buffers, give it this one now
    leave(flags);
}

rtl_usb_stats_t rtl_usb_stats(void)
{
    size_t flags = enter();
    rtl_usb_stats_t s = g_stats;
    leave(flags);
    return s;
}
