/* Boot counters in NVS (see bootinfo.h). SPDX-License-Identifier: GPL-3.0-only */
#include "bootinfo.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

static const char* TAG = "bootinfo";
static BootStats stats;

_Static_assert(
    (int)ESP_RST_BROWNOUT == (int)BOOT_RST_BROWNOUT &&
        (int)ESP_RST_CPU_LOCKUP == (int)BOOT_RST_CPU_LOCKUP,
    "BOOT_RST_* must follow esp_reset_reason_t");

void bootinfo_init(void) {
    nvs_handle_t h;
    /* the default, plain nvs partition (the keystore has initialised it) */
    if(nvs_open("bootinfo", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no NVS: boots are not counted");
        bootstats_count(&stats, esp_reset_reason());
        return;
    }
    size_t n = sizeof(stats);
    if(nvs_get_blob(h, "stats", &stats, &n) != ESP_OK || n != sizeof(stats))
        memset(&stats, 0, sizeof(stats));
    bootstats_count(&stats, esp_reset_reason());
    if(nvs_set_blob(h, "stats", &stats, sizeof(stats)) != ESP_OK || nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "boot count not saved");
    nvs_close(h);
    ESP_LOGI(
        TAG, "boot %lu, reset: %s", (unsigned long)stats.boots, bootstats_reason_name(stats.last));
}

const BootStats* bootinfo_get(void) {
    return &stats;
}
