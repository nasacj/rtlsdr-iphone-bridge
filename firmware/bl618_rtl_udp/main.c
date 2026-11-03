// SPDX-License-Identifier: GPL-2.0-or-later
//
// rtl_tcp over Wi-Fi on an Ai-Thinker Ai-M61-32S (BL618): an RTL-SDR on the module's
// USB host port, an rtl_tcp client app on the far side of the Wi-Fi. See README.md.

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bflb_uart.h"

#include "led.h"
#include "rtl_tcp_server.h"
#include "rtl_usb.h"
#include "wifi_link.h"

extern void shell_init_with_task(struct bflb_device_s *shell);

int main(void)
{
    board_init();
    // The SDK's shell on UART0, so the Wi-Fi manager's own commands (wifi_sta_connect,
    // wifi_scan, ...) are there for diagnosis alongside this firmware's log.
    shell_init_with_task(bflb_device_get_by_name("uart0"));

    printf("\r\nbl618_rtl_udp: RTL-SDR over Wi-Fi\r\n");
    led_init();

    if (wifi_link_start() != 0) {
        printf("Wi-Fi did not start; halting\r\n");
        for (;;) {
        }
    }
    rtl_usb_init();
    rtl_tcp_server_start();

    vTaskStartScheduler();
    for (;;) {
    }
}
