/*
 * SSH Runner firmware (ESP32-S3): WiFi + W5500 LAN, libssh client, keystore,
 * Flipper UART link and a USB bring-up console.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>

#include "app.h"
#include "bootinfo.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "keystore.h"
#include "net.h"
#include "proto/proto.h"
#include "sdkconfig.h"
#include "ssh_client.h"
#include "statusled.h"

static const char* TAG = "main";

/* The WiFi driver's own tags: about 20 INFO lines at boot and on every join (init, PHY,
 * block-ack sessions). Kept to warnings and errors; net logs what matters (the address). */
static const char* const QUIET_TAGS[] =
    {"wifi", "wifi_init", "pp", "net80211", "phy_init", "esp_netif_handlers"};

void app_main(void) {
    statusled_init();
    for(size_t i = 0; i < sizeof(QUIET_TAGS) / sizeof(QUIET_TAGS[0]); i++)
        esp_log_level_set(QUIET_TAGS[i], ESP_LOG_WARN);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(
        TAG,
        "%s on %s: %d core(s) at %d MHz, rev %d, protocol v%d, psram %u KB",
        app_fw_version(),
        CONFIG_FES_BOARD_NAME,
        chip.cores,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        chip.revision,
        PROTO_VERSION,
        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_ERROR_CHECK(keystore_init());
    bootinfo_init(); /* after NVS is up: count this boot and its reset reason */
    ESP_ERROR_CHECK(net_init(app_net_status_changed, NULL));
    ESP_ERROR_CHECK(ssh_client_init());
    statusled_start();
    app_link_start();
    console_start();
}
