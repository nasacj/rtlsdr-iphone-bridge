// SPDX-License-Identifier: GPL-2.0-or-later
//
// The libusb calls librtlsdr makes, on the BL618: the same shim as the Teensy's (the end
// of ../teensy41_rtl_tcp/src/rtl_usb.cpp) against rtl_usb.c instead of USBHost_t36, and the same
// header, ../common/librtlsdr/libusb.h, so librtlsdr is built unmodified from the
// one vendored copy.
//
// What is real: device lookup, descriptors, synchronous control transfers (every register
// and I2C access librtlsdr makes) and string descriptors. librtlsdr's own asynchronous
// reader is not used — the firmware streams the bulk endpoint itself — so those calls
// exist only to link and return LIBUSB_ERROR_NOT_SUPPORTED.

#include <stdlib.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"
#include "bflb_mtimer.h"

#include "libusb.h"
#include "rtl_usb.h"

static int fake_context, fake_device, fake_handle;
static libusb_device *device_list[2] = { (libusb_device *)&fake_device, NULL };
static libusb_device *empty_list[1] = { NULL };
#define CONTROL_TIMEOUT_MS 300

int libusb_init(libusb_context **ctx)
{
    if (ctx) {
        *ctx = (libusb_context *)&fake_context;
    }
    return LIBUSB_SUCCESS;
}

void libusb_exit(libusb_context *ctx) { (void)ctx; }

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    (void)ctx;
    if (!rtl_usb_ready()) {
        *list = empty_list;
        return 0;
    }
    *list = device_list;
    return 1;
}

void libusb_free_device_list(libusb_device **list, int unref) { (void)list; (void)unref; }

int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    (void)dev;
    uint8_t d[18];
    int r = rtl_usb_control(0x80, 6, 0x0100, 0, d, sizeof d, CONTROL_TIMEOUT_MS);
    if (r < 0) {
        return r;
    }
    desc->bLength = d[0];
    desc->bDescriptorType = d[1];
    desc->bcdUSB = (uint16_t)(d[2] | (d[3] << 8));
    desc->bDeviceClass = d[4];
    desc->bDeviceSubClass = d[5];
    desc->bDeviceProtocol = d[6];
    desc->bMaxPacketSize0 = d[7];
    desc->idVendor = (uint16_t)(d[8] | (d[9] << 8));
    desc->idProduct = (uint16_t)(d[10] | (d[11] << 8));
    desc->bcdDevice = (uint16_t)(d[12] | (d[13] << 8));
    desc->iManufacturer = d[14];
    desc->iProduct = d[15];
    desc->iSerialNumber = d[16];
    desc->bNumConfigurations = d[17];
    return LIBUSB_SUCCESS;
}

int libusb_open(libusb_device *dev, libusb_device_handle **handle)
{
    (void)dev;
    if (!rtl_usb_ready()) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    *handle = (libusb_device_handle *)&fake_handle;
    return LIBUSB_SUCCESS;
}

void libusb_close(libusb_device_handle *handle) { (void)handle; }
libusb_device *libusb_get_device(libusb_device_handle *handle) { (void)handle; return device_list[0]; }
int libusb_kernel_driver_active(libusb_device_handle *handle, int i) { (void)handle; (void)i; return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *handle, int i) { (void)handle; (void)i; return LIBUSB_SUCCESS; }
int libusb_attach_kernel_driver(libusb_device_handle *handle, int i) { (void)handle; (void)i; return LIBUSB_SUCCESS; }
int libusb_claim_interface(libusb_device_handle *handle, int i) { (void)handle; (void)i; return LIBUSB_SUCCESS; }
int libusb_release_interface(libusb_device_handle *handle, int i) { (void)handle; (void)i; return LIBUSB_SUCCESS; }
int libusb_reset_device(libusb_device_handle *handle) { (void)handle; return LIBUSB_ERROR_NOT_SUPPORTED; }

int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index, unsigned char *data, uint16_t length,
                            unsigned int timeout)
{
    (void)handle;
    return rtl_usb_control(request_type, request, value, index, data, length,
                           timeout ? timeout : CONTROL_TIMEOUT_MS);
}

int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
                         int length, int *actual, unsigned int timeout)
{
    (void)handle; (void)endpoint; (void)data; (void)length; (void)timeout;
    if (actual) {
        *actual = 0;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;  // the firmware streams the endpoint itself
}

// A string descriptor in US English, UTF-16LE folded to ASCII. librtlsdr reads the
// manufacturer and product strings to tell an RTL-SDR Blog V4 from other R828D sticks.
int libusb_get_string_descriptor_ascii(libusb_device_handle *handle, uint8_t desc_index,
                                       unsigned char *data, int length)
{
    (void)handle;
    if (desc_index == 0 || length <= 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    uint8_t raw[255];
    int r = rtl_usb_control(0x80, 6, (uint16_t)(0x0300 | desc_index), 0x0409, raw, sizeof raw,
                            CONTROL_TIMEOUT_MS);
    if (r < 0) {
        return r;
    }
    int n = raw[0] < r ? raw[0] : r;
    if (n < 2 || raw[1] != 3) {
        return LIBUSB_ERROR_IO;
    }
    int out = 0;
    for (int i = 2; i + 1 < n && out < length - 1; i += 2) {
        data[out++] = raw[i + 1] ? '?' : raw[i];
    }
    data[out] = 0;
    return out;
}

// librtlsdr's asynchronous reader is not used; these exist so it links.
struct libusb_transfer *libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    return (struct libusb_transfer *)calloc(1, sizeof(struct libusb_transfer));
}
void libusb_free_transfer(struct libusb_transfer *transfer) { free(transfer); }
int libusb_submit_transfer(struct libusb_transfer *transfer) { (void)transfer; return LIBUSB_ERROR_NOT_SUPPORTED; }
int libusb_cancel_transfer(struct libusb_transfer *transfer) { (void)transfer; return LIBUSB_ERROR_NOT_SUPPORTED; }
int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
    (void)ctx; (void)tv; (void)completed;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
unsigned char *libusb_dev_mem_alloc(libusb_device_handle *handle, size_t length)
{
    (void)handle; (void)length;
    return NULL;
}
int libusb_dev_mem_free(libusb_device_handle *handle, unsigned char *buffer, size_t length)
{
    (void)handle; (void)buffer; (void)length;
    return LIBUSB_SUCCESS;
}

// librtlsdr sleeps between some tuner steps (5 ms at most). A task delay where the
// scheduler can give the time away, a busy wait for anything shorter than a tick.
__attribute__((weak)) int usleep(useconds_t us)
{
    if (us >= 1000 * portTICK_PERIOD_MS) {
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    } else {
        bflb_mtimer_delay_us(us);
    }
    return 0;
}
