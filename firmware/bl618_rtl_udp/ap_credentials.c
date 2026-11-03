// SPDX-License-Identifier: GPL-2.0-or-later
//
// The board's own network: a name and a password that belong to this board.
//
// **Why not a fixed password.** rtl_tcp has no authentication at all — anyone who can
// reach port 1234 can retune the dongle and read every sample — so the Wi-Fi password is
// the only lock there is, and a password written in an open-source repository is no lock.
// **Why not one computed from the chip ID either**, which is the obvious next idea: the ID
// is in the MAC address the board broadcasts in every beacon, and the computation would
// be in the same public repository.
//
// So the password is drawn from the chip's true random number generator on first boot
// and kept in the PSM partition (lfs_kv), and the serial log prints it at every start.
// `bridge_ap_newkey` at the serial shell forgets it, and the next boot draws another.
// A BRIDGE_AP_KEY in wifi_secrets.h overrides all of this.
//
// The name carries the last two bytes of the chip ID, so two boards side by side are two
// networks in Settings rather than one name with two different passwords.

#include "ap_credentials.h"

#include <stdio.h>
#include <string.h>

#include "bflb_efuse.h"
#include "bflb_mtd.h"
#include "bflb_sec_trng.h"
#include "lfs_kv.h"
#include "shell.h"

#include "bridge_config.h"

#define KV_KEY "bridge.ap_key"
#define KEY_LENGTH 12
// No 0/o, 1/l/i: the password is read off a serial log and typed on a phone.
static const char kAlphabet[] = "23456789abcdefghjkmnpqrstuvwxyz";

static bool g_kv_ready;

static bool valid_key(const char *key, size_t length)
{
    if (length < 8 || length > 63) {  // WPA2's limits
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (key[i] < 0x20 || key[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

static void kv_init_once(void)
{
    if (!g_kv_ready) {
        bflb_mtd_init();
        g_kv_ready = lfs_kv_init() == LFS_KV_OK;
        if (!g_kv_ready) {
            printf("[wifi] PSM storage unavailable: the password will not survive a reboot\r\n");
        }
    }
}

static void generate(char key[KEY_LENGTH + 1])
{
    uint8_t random[KEY_LENGTH];
    bflb_trng_readlen(random, sizeof random);
    for (size_t i = 0; i < KEY_LENGTH; i++) {
        // 31 symbols: the modulo bias is under one part in eight, irrelevant at 59 bits.
        key[i] = kAlphabet[random[i] % (sizeof kAlphabet - 1)];
    }
    key[KEY_LENGTH] = '\0';
}

void ap_credentials(char ssid[33], char key[64])
{
    uint8_t id[8] = { 0 };
    bflb_efuse_get_chipid(id);
    snprintf(ssid, 33, "%s-%02X%02X", BRIDGE_AP_SSID, id[1], id[0]);

#ifdef BRIDGE_AP_KEY
    snprintf(key, 64, "%s", BRIDGE_AP_KEY);
    return;
#else
    kv_init_once();
    if (g_kv_ready) {
        size_t saved = 0;
        memset(key, 0, 64);
        lfs_kv_get_blob(KV_KEY, key, 63, &saved);
        if (saved > 0 && saved < 64 && valid_key(key, strlen(key))) {
            return;
        }
    }
    generate(key);
    if (g_kv_ready && lfs_kv_set_blob(KV_KEY, key, strlen(key) + 1) != LFS_KV_OK) {
        printf("[wifi] could not save the password: a new one will be drawn next boot\r\n");
    }
#endif
}

// Shell: forget the stored password; the next boot draws another. For a password that
// has been shared further than intended.
static int cmd_ap_newkey(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef BRIDGE_AP_KEY
    printf("BRIDGE_AP_KEY is set in wifi_secrets.h; change it there\r\n");
#else
    kv_init_once();
    if (g_kv_ready && lfs_kv_del(KV_KEY) == LFS_KV_OK) {
        printf("password forgotten: press EN to restart with a new one\r\n");
    } else {
        printf("nothing stored, or storage unavailable\r\n");
    }
#endif
    return 0;
}
SHELL_CMD_EXPORT_ALIAS(cmd_ap_newkey, bridge_ap_newkey, forget the Wi-Fi password and draw a new one at the next boot);
