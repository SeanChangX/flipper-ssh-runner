/* Status LED drivers: WS2812 over RMT, or a plain GPIO. SPDX-License-Identifier: GPL-3.0-only */
#include "statusled_drv.h"

#include "sdkconfig.h"

/* The board choice sets the LED (Kconfig); stop the build if that ever drifts. */
#if CONFIG_FES_STATUS_LED && CONFIG_FES_BOARD_WAVESHARE_S3_ETH && \
    !(CONFIG_FES_STATUS_LED_WS2812 && CONFIG_FES_STATUS_LED_GPIO == 21)
#error "Waveshare ESP32-S3-ETH: the status LED is the WS2812 on GPIO21"
#endif
#if CONFIG_FES_STATUS_LED && CONFIG_FES_BOARD_SSH_RUNNER_REV0_1 &&          \
    !(CONFIG_FES_STATUS_LED_GPIO_PIN && CONFIG_FES_STATUS_LED_GPIO == 21 && \
      !CONFIG_FES_STATUS_LED_ACTIVE_LOW)
#error "SSH Runner rev 0.1: the status LED is D201 on GPIO21, active high"
#endif

#if CONFIG_FES_STATUS_LED_WS2812
#include "driver/rmt_tx.h"

static rmt_channel_handle_t ws_chan;
static rmt_encoder_handle_t ws_enc;

static esp_err_t ws_init(void) {
    /* 10 MHz: 0.1 us ticks; WS2812 bits are 0.3/0.9 us (0) and 0.9/0.3 us (1) */
    rmt_tx_channel_config_t cfg = {
        .gpio_num = CONFIG_FES_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&cfg, &ws_chan);
    if(err != ESP_OK) return err;
    rmt_bytes_encoder_config_t enc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc, &ws_enc);
    if(err != ESP_OK) return err;
    return rmt_enable(ws_chan);
}

static void ws_set(bool on) {
    /* red only, dim: one plain light, like rev 0.1's single LED */
    uint8_t grb[3] = {0, on ? CONFIG_FES_STATUS_LED_LEVEL : 0, 0};
    rmt_transmit_config_t tx = {.loop_count = 0};
    if(rmt_transmit(ws_chan, ws_enc, grb, sizeof(grb), &tx) == ESP_OK)
        rmt_tx_wait_all_done(ws_chan, 10);
}

const StatusLedDriver statusled_driver = {.name = "ws2812", .init = ws_init, .set = ws_set};

#elif CONFIG_FES_STATUS_LED_GPIO_PIN
#include "driver/gpio.h"

#if CONFIG_FES_STATUS_LED_ACTIVE_LOW
#define LED_ON_LEVEL 0
#else
#define LED_ON_LEVEL 1
#endif

static void gpio_led_set(bool on) {
    gpio_set_level(CONFIG_FES_STATUS_LED_GPIO, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

static esp_err_t gpio_led_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_FES_STATUS_LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&io);
    if(err == ESP_OK) gpio_led_set(false);
    return err;
}

const StatusLedDriver statusled_driver = {
    .name = "gpio",
    .init = gpio_led_init,
    .set = gpio_led_set};

#else
static esp_err_t none_init(void) {
    return ESP_OK;
}
static void none_set(bool on) {
    (void)on;
}
const StatusLedDriver statusled_driver = {.name = "none", .init = none_init, .set = none_set};
#endif
