// SPDX-License-Identifier: GPL-2.0-or-later
//
// The RTL2832U on the BL618's USB host port, as a CherryUSB class driver.
//
// Two jobs, the same two as the Teensy's rtl_usb.cpp. **Control transfers for librtlsdr**,
// synchronous: every register and I2C access goes through libusb_control_transfer, which
// libusb_shim.c turns into rtl_usb_control. **Streaming the bulk-IN endpoint** into a few
// staging buffers in non-cacheable SRAM, re-submitted from the completion interrupt.
//
// **One transfer in flight, never two.** CherryUSB's EHCI port gives every URB its own
// queue head, and the data toggle lives in the queue head — so two URBs on one endpoint
// would each keep a toggle of their own, and the controller would discard every other
// packet as a retransmission without reporting anything. What CherryUSB does do is split
// one large URB into a chain of 16 KiB qTDs that the controller runs back to back, so the
// only gap is the hand-over between URBs: one per transfer, at kXferSize per transfer. If
// the counter test shows loss there, the fix is a streaming queue head with a ring of
// qTDs, as USBHost_t36 and Linux do — see README.md.
//
// A STALL needs no workaround here, unlike USBHost_t36: CherryUSB returns it to the
// caller of the control transfer (-USB_ERR_STALL). librtlsdr's tuner probe expects one
// for every I2C address with no tuner behind it. This driver submits on ep0 itself rather
// than through usbh_control_transfer, which retries a failed transfer three times — three
// STALLs per absent tuner, each logged as a warning.

#ifndef RTL_USB_H
#define RTL_USB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One URB. 32 KiB is 6.8 ms of samples at 2.4 MSPS: the hand-over gap comes once per
// transfer, so bigger is fewer gaps, and the staging buffers come out of the same 320 KiB
// of SRAM as everything else.
#define RTL_USB_XFER_SIZE (32u * 1024u)
// Staging buffers: one in flight, the rest waiting for the copy task. 96 KiB of the
// 320 KiB of SRAM the application has (Wi-Fi keeps 160 KiB of its own), beside FreeRTOS's
// 100 KiB heap and lwIP's 64 KiB — the linker map is what says whether this fits.
#define RTL_USB_STAGES 3

typedef struct {
    uint32_t filled;          // transfers completed
    uint32_t dropped;         // filled buffers reused before the copy task took them
    uint32_t bulk_errors;     // transfers that ended in an error
    // The same, by CherryUSB's error number (usb_errno.h: 9 babble, 12 I/O, 13 shutdown,
    // 14 timeout …); index 0 holds any number past the table.
    uint32_t bulk_error_codes[16];
    uint64_t bytes_lost;      // what the dropped and failed transfers would have held
    // Failed transfers by how much had arrived: all of it, some, none. The first two are
    // delivered (rtl_usb.c, bulk_complete); only the third is a fault.
    uint32_t errors_full, errors_partial, errors_empty;
    uint32_t submit_failures; // URBs the controller would not take
    uint64_t bytes_in;        // bytes the dongle delivered
} rtl_usb_stats_t;

// Registers nothing (the class driver is found through its section); starts CherryUSB's
// host stack on the BL618's USB controller.
void rtl_usb_init(void);

// True once a dongle is claimed, so control transfers may be made. Task context only.
bool rtl_usb_ready(void);
// True once after the dongle was unplugged; the caller then closes librtlsdr.
bool rtl_usb_take_lost(void);
uint16_t rtl_usb_vid(void);
uint16_t rtl_usb_pid(void);

// Synchronous control transfer. Returns bytes transferred, or a negative LIBUSB_ERROR_*.
// Never from an interrupt.
int rtl_usb_control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                    uint8_t *data, uint16_t length, uint32_t timeout_ms);

// Bulk streaming. `start` discards whatever is waiting and keeps a transfer in flight;
// `stop` stops re-submitting, and the transfer already in flight completes into a buffer
// nobody reads.
void rtl_usb_start_streaming(void);
void rtl_usb_stop_streaming(void);

// The oldest filled buffer, owned by the caller until rtl_usb_release. Waits up to
// `wait_ms` for one. Returns the buffer index, or -1.
// `*gap_bytes` is how many bytes the dongle sent before this buffer that never reached a
// buffer: transfers that failed, and filled buffers reused before they were taken. Its
// place in the stream is exactly before `*data`, which is what lets rtl_udp number the
// samples after it correctly.
int rtl_usb_take(const uint8_t **data, uint32_t *length, uint32_t *gap_bytes, uint32_t wait_ms);
void rtl_usb_release(int index);

rtl_usb_stats_t rtl_usb_stats(void);

#ifdef __cplusplus
}
#endif

#endif
