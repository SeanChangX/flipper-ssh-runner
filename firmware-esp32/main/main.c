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

void app_main(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(
        TAG,
        "%s on %s: %d core(s), rev %d, protocol v%d, psram %u KB",
        app_fw_version(),
        CONFIG_FES_BOARD_NAME,
        chip.cores,
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
