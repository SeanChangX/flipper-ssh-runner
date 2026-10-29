/* The status LED task: samples net and SSH state, runs statusled_sm, drives the LED.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "statusled.h"

#include "bootinfo.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "proto/proto_msg.h"
#include "ssh_client.h"
#include "statusled_drv.h"

static const char* TAG = "statusled";

#define TICK_MS      20
#define TEST_STEP_MS 3000u

static volatile StatusLedMode mode;
static volatile LedPattern shown;
static volatile uint32_t test_start;
static volatile bool testing;

static const LedPattern test_order[] =
    {LED_PAT_BOOT, LED_PAT_LINKING, LED_PAT_LINK_UP, LED_PAT_SSH, LED_PAT_ERROR};
#define TEST_STEPS (sizeof(test_order) / sizeof(test_order[0]))

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sample(LedInputs* in) {
    ProtoNetStatus st;
    net_get_status(&st);
    NetActivity act;
    net_get_activity(&act);
    uint8_t ssh = ssh_client_state();
    in->joining = act.joining;
    in->background_linking = act.rejoining ||
                             ((st.lan.flags & PROTO_NET_LINK) && !(st.lan.flags & PROTO_NET_IP));
    in->links_up = (uint8_t)(!!(st.lan.flags & PROTO_NET_IP) + !!(st.wifi.flags & PROTO_NET_IP));
    in->ssh_active = ssh != ProtoSshIdle && ssh != ProtoSshClosed && ssh != ProtoSshFailed;
    in->ssh_failed = ssh == ProtoSshFailed;
    in->join_failures = act.join_failures;
}

/* Lamp test: true while it runs, with *lit for this tick. */
static bool test_tick(uint32_t now, bool* lit) {
    if(!testing) return false;
    uint32_t t = now - test_start;
    uint32_t step = t / TEST_STEP_MS;
    if(step >= TEST_STEPS) {
        testing = false;
        return false;
    }
    LedPattern p = test_order[step];
    shown = p;
    t %= TEST_STEP_MS;
    *lit = ledsm_lit(p, p == LED_PAT_ERROR ? t % (2 * LED_ERROR_MS) : t);
    return true;
}

static void led_task(void* arg) {
    (void)arg;
    LedSm sm;
    const BootStats* bs = bootinfo_get();
    ledsm_init(&sm, now_ms(), bootstats_is_fault(bs->last));
    bool on = false;
    statusled_driver.set(false);
    TickType_t wake = xTaskGetTickCount();
    while(true) {
        uint32_t now = now_ms();
        LedInputs in;
        sample(&in);
        bool lit = ledsm_step(&sm, now, &in);
        if(!test_tick(now, &lit)) {
            shown = ledsm_pattern(&sm);
            if(mode == STATUSLED_OFF) lit = false;
        }
        if(lit != on) {
            on = lit;
            statusled_driver.set(on);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
    }
}

void statusled_start(void) {
    nvs_handle_t h;
    uint8_t m = STATUSLED_AUTO;
    if(nvs_open("statusled", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mode", &m);
        nvs_close(h);
    }
    mode = m == STATUSLED_OFF ? STATUSLED_OFF : STATUSLED_AUTO;
    esp_err_t err = statusled_driver.init();
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "%s driver: %s, no status LED", statusled_driver.name, esp_err_to_name(err));
        return;
    }
    if(xTaskCreate(led_task, "statusled", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGW(TAG, "no task, no status LED");
    ESP_LOGI(TAG, "%s, %s", statusled_driver.name, mode == STATUSLED_OFF ? "off" : "auto");
}

esp_err_t statusled_set_mode(StatusLedMode m) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("statusled", NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_set_u8(h, "mode", (uint8_t)m);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if(err == ESP_OK) mode = m;
    return err;
}

StatusLedMode statusled_mode(void) {
    return mode;
}

LedPattern statusled_pattern(void) {
    return shown;
}

const char* statusled_driver_name(void) {
    return statusled_driver.name;
}

void statusled_test(void) {
    test_start = now_ms();
    testing = true;
}
