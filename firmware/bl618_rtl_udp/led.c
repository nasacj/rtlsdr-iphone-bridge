// SPDX-License-Identifier: GPL-2.0-or-later

#include "led.h"

#include <math.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_pwm_v2.h"

#include "rtl_tcp_server.h"
#include "rtl_usb.h"
#include "sample_ring.h"
#include "wifi_link.h"

// Ai-M61-32S-Kit specification v1.1.2: red IO12, green IO14, blue IO15, lit when high.
// The schematic (v1.1) has two more the specification's table leaves out, LED4 "C" on IO29
// and LED5 "W" on IO27, each through 470 ohms to ground.
#define LED_RED GPIO_PIN_12
#define LED_GREEN GPIO_PIN_14
#define LED_BLUE GPIO_PIN_15
#define LED_C GPIO_PIN_29
#define LED_W GPIO_PIN_27

// **Breathing is the PWM, and only two lamps can have it.** PWM0's pins pair up as channel
// 0..3 positive/negative, IO24..31 and again from IO8, so blue (IO15) is channel 3 negative
// and C (IO29) channel 2 negative — but red (IO12) is channel 2 positive and green (IO14)
// channel 3 positive, and the two outputs of a channel share its thresholds. So red and
// green stay plain GPIO, and a pin is switched to the PWM only while it breathes: on, off
// and flashing are GPIO for every lamp. That also means a negative output that turns out to
// be the complement of what is set here costs a breath out of phase, not a lamp that will
// not go dark.
#define PWM_CH_BLUE PWM_CH3
#define PWM_CH_C PWM_CH2
#define PWM_PERIOD 1000          // 80 MHz / 80 / 1000: 1 kHz, far above flicker

#define LED_TICK_MS 20           // smooth enough to breathe
#define LED_FAST_MS 100          // fast flash: 5 a second
#define LED_SLOW_MS 500          // slow flash: once a second
#define LED_BREATH_MS 3000       // one breath
#define LED_FAULT_HOLD_MS 2000   // a lost block or a USB error keeps its pattern this long
#define LED_STOP_HOLD_MS 3000    // and a stream that stopped by itself, this long

enum mode { MODE_OFF, MODE_ON, MODE_BREATHE };

static struct bflb_device_s *g_gpio;
static struct bflb_device_s *g_pwm;

typedef struct {
    uint8_t pin;
    int8_t channel;   // PWM channel, or -1 for a lamp that cannot breathe
    enum mode mode;
    bool configured;
} lamp_t;

static lamp_t g_red = { LED_RED, -1 };
static lamp_t g_green = { LED_GREEN, -1 };
static lamp_t g_blue = { LED_BLUE, PWM_CH_BLUE };
static lamp_t g_w = { LED_W, -1 };
static lamp_t g_c = { LED_C, PWM_CH_C };

// The pin function changes only when the mode does: re-initialising a pin every tick would
// glitch it.
static void set(lamp_t *lamp, enum mode mode)
{
    if (mode == MODE_BREATHE && lamp->channel < 0) {
        mode = MODE_ON;
    }
    bool was_pwm = lamp->configured && lamp->mode == MODE_BREATHE;
    if (mode == MODE_BREATHE) {
        if (!was_pwm) {
            bflb_gpio_init(g_gpio, lamp->pin,
                           GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_0);
        }
    } else {
        if (was_pwm || !lamp->configured) {
            bflb_gpio_init(g_gpio, lamp->pin, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_0);
        }
        if (mode == MODE_ON) {
            bflb_gpio_set(g_gpio, lamp->pin);
        } else {
            bflb_gpio_reset(g_gpio, lamp->pin);
        }
    }
    lamp->mode = mode;
    lamp->configured = true;
}

static void flash(lamp_t *lamp, bool on) { set(lamp, on ? MODE_ON : MODE_OFF); }

// A breath: a raised cosine, squared because the eye is closer to logarithmic than linear —
// unsquared, the lamp seems to sit near full brightness for most of the cycle.
static void breathe(TickType_t now)
{
    float t = (float)((now * portTICK_PERIOD_MS) % LED_BREATH_MS) / LED_BREATH_MS;
    float level = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * t);
    uint16_t high = (uint16_t)(level * level * (PWM_PERIOD - 1)) + 1;
    bflb_pwm_v2_channel_set_threshold(g_pwm, PWM_CH_BLUE, 0, high);
    bflb_pwm_v2_channel_set_threshold(g_pwm, PWM_CH_C, 0, high);
}

// USB faults that cost something: a transfer that failed with nothing in it, and a transfer
// the controller would not take. Not the ones that failed after arriving whole, which are
// delivered (rtl_usb.c) — about one a second while streaming, and a lamp flashing for those
// would say "fault" all the time about something that loses nothing.
static uint32_t usb_errors(const rtl_usb_stats_t *s)
{
    return s->errors_empty + s->submit_failures;
}

// A counter that moved holds a fault for `hold_ms`.
typedef struct {
    uint32_t seen;
    TickType_t until;
} fault_t;

static bool fault(fault_t *f, uint32_t count, TickType_t now, uint32_t hold_ms)
{
    if (count != f->seen) {
        f->seen = count;
        f->until = now + pdMS_TO_TICKS(hold_ms);
    }
    return (int32_t)(f->until - now) > 0;
}

static void led_task(void *unused)
{
    (void)unused;
    TickType_t now = xTaskGetTickCount();
    rtl_usb_stats_t s = rtl_usb_stats();
    // Primed with the counts at start, so nothing from before this task reads as new.
    // Everything USB lost, whether a buffer reused before it was taken or a failed
    // transfer: both are samples the stream does not have.
    fault_t usb_drop = { (uint32_t)s.bytes_lost, now };
    fault_t ring_drop = { sample_ring_dropped_blocks(), now };
    fault_t usb_error = { usb_errors(&s), now };
    fault_t stopped = { rtl_tcp_server_interruptions(), now };
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
        now = xTaskGetTickCount();
        uint32_t ms = now * portTICK_PERIOD_MS;
        bool fast = (ms / LED_FAST_MS) & 1;
        bool slow = (ms / LED_SLOW_MS) & 1;
        s = rtl_usb_stats();
        bool usb_dropping = fault(&usb_drop, (uint32_t)s.bytes_lost, now, LED_FAULT_HOLD_MS);
        bool ring_dropping = fault(&ring_drop, sample_ring_dropped_blocks(), now, LED_FAULT_HOLD_MS);
        bool usb_failing = fault(&usb_error, usb_errors(&s), now, LED_FAULT_HOLD_MS);
        bool stopped_by_itself = fault(&stopped, rtl_tcp_server_interruptions(), now, LED_STOP_HOLD_MS);
        bool wifi = wifi_link_up();
        bool streaming = rtl_tcp_server_streaming();

        // W: the RTL-SDR. Claimed by USB but not opened by librtlsdr is the open failing: it is
        // retried every few seconds, and the moment between claim and open is too short to see.
        if (usb_failing || (rtl_usb_ready() && !rtl_tcp_server_dongle_open())) {
            flash(&g_w, fast);
        } else if (!rtl_usb_ready()) {
            flash(&g_w, slow);
        } else {
            set(&g_w, MODE_ON);
        }

        // C: Wi-Fi, then the phone.
        if (!wifi) {
            set(&g_c, MODE_OFF);
        } else if (streaming) {
            set(&g_c, MODE_BREATHE);
        } else if (rtl_tcp_server_client() || wifi_link_ap_stations() > 0) {
            set(&g_c, MODE_ON);
        } else {
            flash(&g_c, slow);
        }

        // RGB: the stream's health. Faults first, so a lost block is never hidden by the
        // breath that says all is well.
        if (!wifi || stopped_by_itself) {
            set(&g_red, MODE_ON);
            set(&g_blue, MODE_OFF);
        } else if (streaming && usb_dropping) {
            flash(&g_red, fast);
            flash(&g_blue, !fast);
        } else if (streaming && ring_dropping) {
            flash(&g_red, fast);
            set(&g_blue, MODE_ON);
        } else if (streaming) {
            set(&g_red, MODE_OFF);
            set(&g_blue, MODE_BREATHE);
        } else {
            set(&g_red, MODE_OFF);
            set(&g_blue, MODE_OFF);
        }

        if (g_blue.mode == MODE_BREATHE || g_c.mode == MODE_BREATHE) {
            breathe(now);
        }
    }
}

static void pwm_init(void)
{
    g_pwm = bflb_device_get_by_name("pwm_v2_0");
    struct bflb_pwm_v2_config_s cfg = {
        .clk_source = BFLB_SYSTEM_PBCLK,
        .clk_div = 80,
        .period = PWM_PERIOD,
    };
    // Paired outputs, so the negative pins (IO15, IO29) carry their channels at all.
    bflb_pwm_v2_feature_control(g_pwm, PWM_CMD_IO_SEL, PWM_IO_SEL_DIFF_END);
    bflb_pwm_v2_init(g_pwm, &cfg);
    struct bflb_pwm_v2_channel_config_s ch = {
        .positive_polarity = PWM_POLARITY_ACTIVE_HIGH,
        .negative_polarity = PWM_POLARITY_ACTIVE_HIGH,
        .positive_stop_state = PWM_STATE_INACTIVE,
        .negative_stop_state = PWM_STATE_INACTIVE,
        .positive_brake_state = PWM_STATE_INACTIVE,
        .negative_brake_state = PWM_STATE_INACTIVE,
        .dead_time = 0,
    };
    bflb_pwm_v2_channel_init(g_pwm, PWM_CH_BLUE, &ch);
    bflb_pwm_v2_channel_init(g_pwm, PWM_CH_C, &ch);
    bflb_pwm_v2_channel_set_threshold(g_pwm, PWM_CH_BLUE, 0, 1);
    bflb_pwm_v2_channel_set_threshold(g_pwm, PWM_CH_C, 0, 1);
    bflb_pwm_v2_channel_negative_start(g_pwm, PWM_CH_BLUE);
    bflb_pwm_v2_channel_negative_start(g_pwm, PWM_CH_C);
    bflb_pwm_v2_start(g_pwm);
}

void led_init(void)
{
    g_gpio = bflb_device_get_by_name("gpio");
    pwm_init();
    // Green is not used: the RGB lamp says only "healthy" or which fault.
    set(&g_green, MODE_OFF);
    set(&g_blue, MODE_OFF);
    set(&g_w, MODE_OFF);
    set(&g_c, MODE_OFF);
    set(&g_red, MODE_ON);
    xTaskCreate(led_task, "led", 512, NULL, 3, NULL);
}
