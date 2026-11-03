// SPDX-License-Identifier: Apache-2.0
//
// Derived from CherryUSB's configuration template as shipped in Bouffalo's SDK
// (Apache-2.0, Copyright (c) 2022, sakumisu).
//
// CherryUSB settings for this firmware: the SDK's template, then what differs. The shape is
// the SDK's examples/cherryusb/cherryusb_cli/config_file/usb_config.h.

#ifndef BL618_RTL_TCP_USB_CONFIG_H
#define BL618_RTL_TCP_USB_CONFIG_H

#include "cherryusb_config_template.h"

// High speed: 480 Mbit/s to the dongle. The template leaves it to the port.
#undef CONFIG_USB_HS
#define CONFIG_USB_HS

// One RTL2832U and nothing else: no hubs are expected, and two interfaces at most.
#undef CONFIG_USBHOST_MAX_RHPORTS
#define CONFIG_USBHOST_MAX_RHPORTS 1

// qTDs: a 32 KiB bulk URB is two (16 KiB each), a control transfer three. The template's
// 30 is enough; the margin is for a hub, should anybody put one in between.
#undef CONFIG_USB_EHCI_QTD_NUM
#define CONFIG_USB_EHCI_QTD_NUM 40

#endif
