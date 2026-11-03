// SPDX-License-Identifier: GPL-2.0-or-later

#include "rtl_usb.h"

#include <string.h>
#include <unistd.h>

#include "libusb.h"  // ../common/librtlsdr, an Arduino library (--library)

USBHost myusb;
RtlUsb rtlusb(myusb);

// The sample buffers live in OCRAM (DMAMEM), which the CPU caches. The USB controller
// writes them by DMA behind the cache's back, so every buffer is invalidated before it is
// handed to the controller and again before the CPU reads it.
DMAMEM static uint8_t blocks[RtlUsb::kBlocks][RtlUsb::kBlockSize] __attribute__((aligned(32)));

void RtlUsb::init() {
  contribute_Pipes(pipes_, sizeof(pipes_) / sizeof(Pipe_t));
  contribute_Transfers(transfers_, sizeof(transfers_) / sizeof(Transfer_t));
  contribute_String_Buffers(strbufs_, sizeof(strbufs_) / sizeof(strbuf_t));
  driver_ready_for_device(this);
}

bool RtlUsb::ready() const {
  return claimed_ && !lost_ && device != nullptr && !USBHost::enumeration_busy;
}

bool RtlUsb::take_lost() {
  if (!lost_) return false;
  __disable_irq();
  lost_ = false;
  ring_.reset_all();  // the controller freed the queued transfers with the device
  __enable_irq();
  return true;
}

// Interface 0, vendor-specific, with a bulk-IN endpoint: what every RTL2832U stick
// presents. Whether it is a *known* stick is librtlsdr's question, answered in
// rtlsdr_open from the VID/PID; claiming here only reserves the hardware.
bool RtlUsb::claim(Device_t *dev, int type, const uint8_t *d, uint32_t len) {
  if (type != 1 || len < 9 || d[0] != 9 || d[1] != 4) return false;
  if (d[2] != 0 || d[5] != 0xFF) return false;

  uint8_t endpoint = 0;
  uint16_t max_packet = 0;
  const uint8_t *p = d + 9;
  const uint8_t *end = d + len;
  while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
    if (p[1] == 4) break;  // the next interface: this one had no bulk IN
    if (p[1] == 5 && p[0] >= 7 && (p[2] & 0x80) && (p[3] & 0x03) == 2) {
      endpoint = p[2];
      max_packet = p[4] | (p[5] << 8);
      break;
    }
    p += p[0];
  }
  if (endpoint == 0) return false;

  Pipe_t *pipe = new_Pipe(dev, 2, endpoint & 0x0F, 1, max_packet);
  if (pipe == nullptr) return false;
  pipe->callback_function = bulk_done;
  pipe->error_callback_function = bulk_error;

  control_pipe_ = dev->control_pipe;
  control_pipe_->error_callback_function = control_error;

  vid_ = dev->idVendor;
  pid_ = dev->idProduct;
  streaming_ = false;
  control_state_ = Idle;
  ring_.reset_all();
  bulk_pipe_ = pipe;
  lost_ = false;
  claimed_ = true;
  return true;
}

// In the USB interrupt. USBHost_t36 frees the pipes and their queued transfers itself.
void RtlUsb::disconnect() {
  streaming_ = false;
  bulk_pipe_ = nullptr;
  control_pipe_ = nullptr;
  if (control_state_ == Pending) control_state_ = Failed;
  claimed_ = false;
  lost_ = true;
}

// A control transfer this driver queued has completed.
void RtlUsb::control(const Transfer_t *transfer) {
  if (transfer->driver == this && control_state_ == Pending) control_state_ = Done;
}

// Every error on this device's control pipe arrives here. When this driver has a transfer
// outstanding the error is ours — a STALL on an I2C address with nothing behind it, as
// librtlsdr's tuner probe expects. Anything else goes where USBHost_t36 sent it before.
// The failing qTD is not always the one carrying `driver` (only the last qTD of a control
// transfer does), so ownership is judged by our own state and the pipe, not by that field.
void RtlUsb::control_error(const Transfer_t *transfer) {
  if (rtlusb.control_state_ == Pending && transfer->pipe == rtlusb.control_pipe_) {
    rtlusb.control_state_ = Failed;
    return;
  }
  enumeration_error(transfer);
}

int RtlUsb::control_sync(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                         uint8_t *data, uint16_t length, uint32_t timeout_ms) {
  if (!claimed_ || lost_ || device == nullptr) return LIBUSB_ERROR_NO_DEVICE;
  if (length > sizeof(control_buf_)) return LIBUSB_ERROR_INVALID_PARAM;
  const bool in = (request_type & 0x80) != 0;
  if (!in && length) memcpy(control_buf_, data, length);

  mk_setup(setup_, request_type, request, value, index, length);
  control_state_ = Pending;
  if (!queue_Control_Transfer(device, &setup_, length ? control_buf_ : nullptr, this)) {
    control_state_ = Idle;
    return LIBUSB_ERROR_NO_MEM;
  }
  const uint32_t start = millis();
  while (control_state_ == Pending) {
    if (lost_) { control_state_ = Idle; return LIBUSB_ERROR_NO_DEVICE; }
    // A late completion after a timeout would be credited to the next transfer. A
    // control transfer to this chip takes well under a millisecond, so a timeout means the
    // device is gone or wedged, and the caller's next step is to close it.
    if (millis() - start > timeout_ms) { control_state_ = Idle; return LIBUSB_ERROR_TIMEOUT; }
  }
  const uint8_t outcome = control_state_;
  control_state_ = Idle;
  if (outcome == Failed) return lost_ ? LIBUSB_ERROR_NO_DEVICE : LIBUSB_ERROR_PIPE;
  if (in && length) memcpy(data, control_buf_, length);
  return length;
}

void RtlUsb::queue_more() {
  Pipe_t *pipe = bulk_pipe_;
  if (!streaming_ || pipe == nullptr) return;
  while (ring_.queued() < kQueued) {
    const int i = ring_.next_to_queue();
    if (i < 0) break;
    arm_dcache_delete(blocks[i], kBlockSize);
    if (!queue_Data_Transfer(pipe, blocks[i], kBlockSize, this)) {
      ring_.abandon(i);
      queue_failures_++;
      break;
    }
  }
}

// In the USB interrupt: a transfer filled, so record it and put another in its place
// before anything else happens.
void RtlUsb::bulk_done(const Transfer_t *transfer) {
  const uint8_t *buffer = static_cast<const uint8_t *>(transfer->buffer);
  const int i = (int)((buffer - &blocks[0][0]) / RtlUsb::kBlockSize);
  const uint32_t got = transfer->length - ((transfer->qtd.token >> 16) & 0x7FFF);
  rtlusb.ring_.completed(i, got);
  rtlusb.queue_more();
}

// In the USB interrupt: USBHost_t36 has removed every transfer queued on the bulk pipe.
void RtlUsb::bulk_error(const Transfer_t *) {
  rtlusb.bulk_errors_++;
  for (size_t i = 0; i < kBlocks; i++) rtlusb.ring_.abandon((int)i);
  rtlusb.queue_more();
}

void RtlUsb::start_streaming() {
  __disable_irq();
  ring_.discard_filled();
  streaming_ = true;
  queue_more();
  __enable_irq();
}

void RtlUsb::stop_streaming() {
  streaming_ = false;
}

int RtlUsb::take_block(const uint8_t **data, uint32_t *length) {
  __disable_irq();
  const int i = ring_.oldest_filled();
  uint32_t n = 0;
  if (i >= 0) {
    ring_.begin_sending(i);
    n = ring_.length(i);
  }
  __enable_irq();
  if (i < 0) return -1;
  arm_dcache_delete(blocks[i], (n + 31) & ~31u);
  *data = blocks[i];
  *length = n;
  return i;
}

void RtlUsb::release_block(int index) {
  __disable_irq();
  ring_.finish_sending(index);
  queue_more();  // if the USB side ever ran short, top it up now
  __enable_irq();
}

RtlUsb::Stats RtlUsb::stats() {
  __disable_irq();
  const auto &s = ring_.stats();
  Stats out{s.filled, s.dropped, s.starved, bulk_errors_, queue_failures_, s.bytes_in};
  __enable_irq();
  return out;
}

// --- the libusb shim librtlsdr links against -----------------------------------------

namespace {
int fake_context, fake_device, fake_handle;
libusb_device *device_list[2] = {reinterpret_cast<libusb_device *>(&fake_device), nullptr};
libusb_device *empty_list[1] = {nullptr};
constexpr uint32_t kControlTimeoutMs = 300;
}  // namespace

extern "C" {

int libusb_init(libusb_context **ctx) {
  if (ctx) *ctx = reinterpret_cast<libusb_context *>(&fake_context);
  return LIBUSB_SUCCESS;
}

void libusb_exit(libusb_context *) {}

ssize_t libusb_get_device_list(libusb_context *, libusb_device ***list) {
  if (!rtlusb.ready()) { *list = empty_list; return 0; }
  *list = device_list;
  return 1;
}

void libusb_free_device_list(libusb_device **, int) {}

int libusb_get_device_descriptor(libusb_device *, struct libusb_device_descriptor *desc) {
  uint8_t d[18];
  const int r = rtlusb.control_sync(0x80, 6, 0x0100, 0, d, sizeof d, kControlTimeoutMs);
  if (r < 0) return r;
  desc->bLength = d[0];
  desc->bDescriptorType = d[1];
  desc->bcdUSB = d[2] | (d[3] << 8);
  desc->bDeviceClass = d[4];
  desc->bDeviceSubClass = d[5];
  desc->bDeviceProtocol = d[6];
  desc->bMaxPacketSize0 = d[7];
  desc->idVendor = d[8] | (d[9] << 8);
  desc->idProduct = d[10] | (d[11] << 8);
  desc->bcdDevice = d[12] | (d[13] << 8);
  desc->iManufacturer = d[14];
  desc->iProduct = d[15];
  desc->iSerialNumber = d[16];
  desc->bNumConfigurations = d[17];
  return LIBUSB_SUCCESS;
}

int libusb_open(libusb_device *, libusb_device_handle **handle) {
  if (!rtlusb.ready()) return LIBUSB_ERROR_NO_DEVICE;
  *handle = reinterpret_cast<libusb_device_handle *>(&fake_handle);
  return LIBUSB_SUCCESS;
}

void libusb_close(libusb_device_handle *) {}
libusb_device *libusb_get_device(libusb_device_handle *) { return device_list[0]; }
int libusb_kernel_driver_active(libusb_device_handle *, int) { return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *, int) { return LIBUSB_SUCCESS; }
int libusb_attach_kernel_driver(libusb_device_handle *, int) { return LIBUSB_SUCCESS; }
int libusb_claim_interface(libusb_device_handle *, int) { return LIBUSB_SUCCESS; }
int libusb_release_interface(libusb_device_handle *, int) { return LIBUSB_SUCCESS; }
int libusb_reset_device(libusb_device_handle *) { return LIBUSB_ERROR_NOT_SUPPORTED; }

int libusb_control_transfer(libusb_device_handle *, uint8_t request_type, uint8_t request, uint16_t value,
                            uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout) {
  return rtlusb.control_sync(request_type, request, value, index, data, length,
                             timeout ? timeout : kControlTimeoutMs);
}

int libusb_bulk_transfer(libusb_device_handle *, unsigned char, unsigned char *, int, int *actual, unsigned int) {
  if (actual) *actual = 0;
  return LIBUSB_ERROR_NOT_SUPPORTED;  // the firmware streams the endpoint itself
}

// A string descriptor in US English, UTF-16LE folded to ASCII. librtlsdr reads the
// manufacturer and product strings to tell an RTL-SDR Blog V4 from other R828D sticks.
int libusb_get_string_descriptor_ascii(libusb_device_handle *, uint8_t desc_index, unsigned char *data,
                                       int length) {
  if (desc_index == 0 || length <= 0) return LIBUSB_ERROR_INVALID_PARAM;
  uint8_t raw[255];
  const int r = rtlusb.control_sync(0x80, 6, 0x0300 | desc_index, 0x0409, raw, sizeof raw, kControlTimeoutMs);
  if (r < 0) return r;
  const int n = raw[0] < r ? raw[0] : r;
  if (n < 2 || raw[1] != 3) return LIBUSB_ERROR_IO;
  int out = 0;
  for (int i = 2; i + 1 < n && out < length - 1; i += 2) data[out++] = raw[i + 1] ? '?' : raw[i];
  data[out] = 0;
  return out;
}

// librtlsdr's asynchronous reader is not used; these exist so it links.
struct libusb_transfer *libusb_alloc_transfer(int) {
  return static_cast<struct libusb_transfer *>(calloc(1, sizeof(struct libusb_transfer)));
}
void libusb_free_transfer(struct libusb_transfer *transfer) { free(transfer); }
int libusb_submit_transfer(struct libusb_transfer *) { return LIBUSB_ERROR_NOT_SUPPORTED; }
int libusb_cancel_transfer(struct libusb_transfer *) { return LIBUSB_ERROR_NOT_SUPPORTED; }
int libusb_handle_events_timeout_completed(libusb_context *, struct timeval *, int *) {
  return LIBUSB_ERROR_NOT_SUPPORTED;
}
unsigned char *libusb_dev_mem_alloc(libusb_device_handle *, size_t) { return nullptr; }
int libusb_dev_mem_free(libusb_device_handle *, unsigned char *, size_t) { return LIBUSB_SUCCESS; }

// librtlsdr sleeps between some tuner steps.
__attribute__((weak)) int usleep(useconds_t us) {
  delayMicroseconds(us);
  return 0;
}

}  // extern "C"
