// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wi-Fi: join the configured network (the iPhone's Personal Hotspot), or, failing that,
// open the board's own. The start-up sequence — RF parameters, lwIP, the firmware task,
// the event filter — is the SDK's examples/wifi/sta/wifi_tcp/main.c.
//
// **Power saving off once connected.** A station in power-save sleeps between beacons and
// lets the access point buffer its traffic, which costs throughput and adds latency in
// bursts — exactly the stalls the sample ring is there to absorb. The board is powered
// from a USB supply, not a coin cell, so there is nothing to save.
//
// **mDNS on whichever network is up**, so the app can be given `rtlsdr.local` and never
// needs the address the phone's hotspot happened to hand out. The responder is lwIP's,
// built by the SDK (CONFIG_MDNS); the settings are in config/lwipopts_user.h.

#include "wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include <lwip/apps/mdns.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

#include "async_event.h"
#include "rfparam_adapter.h"
#include "wifi_mgmr.h"
#include "wifi_mgmr_ext.h"

#include "ap_credentials.h"
#include "bridge_config.h"

extern void wifi_task_create(void);
extern int fhost_init(void);
extern int wifi_mgmr_task_start(void);

static volatile bool g_manager_ready;
static volatile bool g_sta_up;
static volatile bool g_ap_up;
static char g_address[16] = "-";
static char g_ap_ssid[33];
static char g_ap_key[64];

// The reentrant spelling: inet_ntoa's one static buffer is shared with the serve task's
// log line for each new client.
static bool remember_sta_address(void)
{
    uint32_t addr = 0, mask = 0, gw = 0, dns = 0;
    if (wifi_sta_ip4_addr_get(&addr, &mask, &gw, &dns) != 0 || addr == 0) {
        return false;
    }
    ip4_addr_t a = { .addr = addr };
    ip4addr_ntoa_r(&a, g_address, sizeof g_address);
    return true;
}

// --- mDNS ------------------------------------------------------------------------------

#define MDNS_TTL 120
static struct netif *g_mdns_netifs[2];
static char g_mdns_name[MDNS_LABEL_MAXLEN + 1] = BRIDGE_MDNS_NAME;

// Another device already answers to the name — normally the other bridge board. Take the
// next free one rather than going quiet, and say so: the app's setting is then wrong.
static void mdns_name_result(struct netif *netif, u8_t result)
{
    if (result == MDNS_PROBING_SUCCESSFUL) {
        printf("[mdns] %s.local on %c%c\r\n", g_mdns_name, netif->name[0], netif->name[1]);
        return;
    }
    static int suffix = 1;
    snprintf(g_mdns_name, sizeof g_mdns_name, "%s-%d", BRIDGE_MDNS_NAME, ++suffix);
    printf("[mdns] %s.local is taken, using %s.local\r\n", BRIDGE_MDNS_NAME, g_mdns_name);
    mdns_resp_rename_netif(netif, g_mdns_name);
}

// In the TCP/IP thread: announce on every interface that has an address and is not yet
// announced. Called on each GOT_IP and AP start, so a later network is picked up too.
static void mdns_publish(void *arg)
{
    (void)arg;
    static bool started;
    if (!started) {
        mdns_resp_init();
        mdns_resp_register_name_result_cb(mdns_name_result);
        started = true;
    }
    struct netif *n;
    NETIF_FOREACH(n) {
        if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n)) || (n->flags & NETIF_FLAG_ETHARP) == 0) {
            continue;  // down, no address yet, or the loopback
        }
        int free_slot = -1;
        bool known = false;
        for (int i = 0; i < 2; i++) {
            known |= g_mdns_netifs[i] == n;
            if (g_mdns_netifs[i] == NULL && free_slot < 0) {
                free_slot = i;
            }
        }
        if (known || free_slot < 0) {
            continue;
        }
        n->flags |= NETIF_FLAG_IGMP;
        if (mdns_resp_add_netif(n, g_mdns_name, MDNS_TTL) != ERR_OK) {
            printf("[mdns] could not start on %c%c\r\n", n->name[0], n->name[1]);
            continue;
        }
        mdns_resp_add_service(n, g_mdns_name, "_rtl_tcp", DNSSD_PROTO_TCP, BRIDGE_RTL_TCP_PORT, MDNS_TTL,
                              NULL, NULL);
        g_mdns_netifs[free_slot] = n;
    }
}

// **`wifi_event` only records what happened; `link_task` does the work.** The SDK calls it
// from the timer task with the scheduler suspended (platform_bouffalo_sdk.c,
// `async_event_handler`: "Prevents blocking operations in the Timer context"), and FreeRTOS
// asserts on any wait made while suspended. The first boot on real hardware died exactly
// so: "network up" printed, then `tcpip_callback` waited for lwIP's mailbox and
// `xQueueGenericSend` asserted. `wifi_mgmr_sta_ps_exit` and `wifi_sta_ip4_addr_get` were
// in the same handler and are binary-only, so whether they wait cannot be read — the rule
// is therefore structural rather than call by call: here, flag stores and nothing else.
// `wifi_mgmr_task_start` is the one exception, made exactly where the SDK's own examples
// make it (examples/wifi/sta/wifi_tcp/main.c).
//
// Events of one kind arriving faster than link_task's 50 ms poll merge into one, which
// costs a duplicate log line at most: each handler reads the current state, not a count.
#define LINK_EV_GOT_IP     (1u << 0)
#define LINK_EV_DISCONNECT (1u << 1)
#define LINK_EV_AP_STARTED (1u << 2)
#define LINK_EV_AP_STA_ADD (1u << 3)
#define LINK_EV_AP_STA_DEL (1u << 4)
static volatile uint32_t g_link_events;
// Phones on the board's own network, counted in the event itself: two joins inside one of
// link_task's polls would be one bit in g_link_events.
static volatile int32_t g_ap_stations;

static void post_link_event(uint32_t bit) { __atomic_fetch_or(&g_link_events, bit, __ATOMIC_SEQ_CST); }

static void wifi_event(async_input_event_t ev, void *priv)
{
    (void)priv;
    switch (ev->code) {
        case CODE_WIFI_ON_INIT_DONE:
            wifi_mgmr_task_start();
            break;
        case CODE_WIFI_ON_MGMR_DONE:
            g_manager_ready = true;
            break;
        case CODE_WIFI_ON_GOT_IP:
            post_link_event(LINK_EV_GOT_IP);
            break;
        case CODE_WIFI_ON_DISCONNECT:
            post_link_event(LINK_EV_DISCONNECT);
            break;
        case CODE_WIFI_ON_AP_STARTED:
            post_link_event(LINK_EV_AP_STARTED);
            break;
        case CODE_WIFI_ON_AP_STA_ADD:
            __atomic_fetch_add(&g_ap_stations, 1, __ATOMIC_SEQ_CST);
            post_link_event(LINK_EV_AP_STA_ADD);
            break;
        case CODE_WIFI_ON_AP_STA_DEL:
            __atomic_fetch_sub(&g_ap_stations, 1, __ATOMIC_SEQ_CST);
            post_link_event(LINK_EV_AP_STA_DEL);
            break;
        default:
            break;
    }
}

// In link_task, where waiting is allowed.
static void handle_link_events(void)
{
    uint32_t ev = __atomic_exchange_n(&g_link_events, 0, __ATOMIC_SEQ_CST);
    if (ev & LINK_EV_DISCONNECT) {
        if (g_sta_up) {
            printf("[wifi] disconnected from %s\r\n", BRIDGE_STA_SSID);
        }
        g_sta_up = false;
    }
    // A disconnect and an address in one poll could have come in either order, so the
    // interface is asked whether it has an address now rather than the order guessed.
    if ((ev & LINK_EV_GOT_IP) && remember_sta_address()) {
        g_sta_up = true;
        wifi_mgmr_sta_ps_exit();
        printf("[wifi] got ip %s: rtl_tcp on %s:%d\r\n", g_address, g_address, BRIDGE_RTL_TCP_PORT);
        tcpip_callback(mdns_publish, NULL);
    }
    if (ev & LINK_EV_AP_STARTED) {
        snprintf(g_address, sizeof g_address, "%s", BRIDGE_AP_ADDRESS);
        g_ap_up = true;
        // The password on every start: it is this board's own, and the serial log is
        // where it is read from (ap_credentials.c).
        printf("[wifi] network \"%s\" up, password %s: rtl_tcp on %s:%d (%s.local)\r\n", g_ap_ssid,
               g_ap_key, BRIDGE_AP_ADDRESS, BRIDGE_RTL_TCP_PORT, BRIDGE_MDNS_NAME);
        tcpip_callback(mdns_publish, NULL);
    }
    if (ev & LINK_EV_AP_STA_ADD) {
        printf("[wifi] a phone joined \"%s\"\r\n", g_ap_ssid);
    }
    if (ev & LINK_EV_AP_STA_DEL) {
        printf("[wifi] a phone left \"%s\"\r\n", g_ap_ssid);
    }
}

static void wait_ms_handling_events(uint32_t ms)
{
    handle_link_events();
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void start_ap(void)
{
    ap_credentials(g_ap_ssid, g_ap_key);
    wifi_mgmr_ap_params_t ap;
    memset(&ap, 0, sizeof ap);
    ap.ssid = g_ap_ssid;
    ap.key = g_ap_key;
    ap.akm = (char *)"WPA2";
    ap.channel = BRIDGE_AP_CHANNEL;
    ap.use_ipcfg = true;
    ap.use_dhcpd = true;
    ap.start = 2;  // the phone gets 192.168.7.2 onwards
    ap.limit = 8;
    ap.ap_ipaddr = inet_addr(BRIDGE_AP_ADDRESS);
    ap.ap_mask = inet_addr("255.255.255.0");
    ap.bcn_mode = 2;  // always beacon: an AP that waits for a probe is easy to miss in Settings
    if (wifi_mgmr_ap_start(&ap) != 0) {
        printf("[wifi] could not start \"%s\"\r\n", g_ap_ssid);
    }
}

static void link_task(void *param)
{
    (void)param;
    while (!g_manager_ready) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (BRIDGE_STA_SSID[0] != '\0') {
        printf("[wifi] joining \"%s\"\r\n", BRIDGE_STA_SSID);
        wifi_sta_connect(BRIDGE_STA_SSID, BRIDGE_STA_KEY, NULL, NULL, 0, 0, 0, 1);
        TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(BRIDGE_STA_TIMEOUT_MS);
        while (!g_sta_up && (int32_t)(until - xTaskGetTickCount()) > 0) {
            wait_ms_handling_events(50);
        }
        if (g_sta_up) {
            // The manager reconnects on its own after a drop.
            wifi_mgmr_sta_autoconnect_enable();
        } else {
            printf("[wifi] \"%s\" not joined in %d s, opening the board's own network instead\r\n",
                   BRIDGE_STA_SSID, BRIDGE_STA_TIMEOUT_MS / 1000);
            wifi_sta_disconnect();
            start_ap();
        }
    } else {
        start_ap();
    }
    // Stays for the life of the board: every later event — a reconnect's new address, a
    // phone joining — is handled here, never in wifi_event.
    for (;;) {
        wait_ms_handling_events(50);
    }
}

static void firmware_task(void *param)
{
    (void)param;
    async_register_event_filter(EV_WIFI, wifi_event, NULL);
    wifi_task_create();
    fhost_init();
    vTaskDelete(NULL);
}

int wifi_link_start(void)
{
    if (rfparam_init(0, NULL, 0) != 0) {
        printf("[wifi] RF init failed\r\n");
        return -1;
    }
    tcpip_init(NULL, NULL);
    xTaskCreate(firmware_task, "wifi_fw", 1024, NULL, 10, NULL);
    xTaskCreate(link_task, "wifi_link", 1024, NULL, 9, NULL);
    return 0;
}

bool wifi_link_up(void) { return g_sta_up || g_ap_up; }
int wifi_link_ap_stations(void)
{
    int32_t n = g_ap_stations;
    return n > 0 ? (int)n : 0;
}
const char *wifi_link_address(void) { return g_address; }
