/* Link transport. SPDX-License-Identifier: GPL-3.0-only */
#include "link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "proto/proto_link.h"
#include "sdkconfig.h"

#if CONFIG_FES_LINK_UART
#include "driver/uart.h"
#define LINK_UART UART_NUM_1
#else
#include "driver/usb_serial_jtag.h"
#endif

static const char* TAG = "link";

#define BAUD_SWITCH_DELAY_MS 20
#define BAUD_FALLBACK_MS     1000
#define PEER_IDLE_MS         2000 /* PROTOCOL 4.3: Flipper keepalive is 0.7 s */

static struct {
    ProtoLink link;
    SemaphoreHandle_t lock; /* recursive: handlers reply from inside rx */
    LinkRequestHandler handler;
    void* ctx;
    uint32_t baud;
    uint32_t pending_baud;
    int64_t switch_at_us;
    int64_t switched_at_us;
    bool confirmed; /* a valid frame arrived at the current rate */
    bool peer;
    int64_t last_rx_us; /* last valid frame from the Flipper */
    uint32_t rx_bytes;
    uint32_t rx_frames;
    int64_t mute_until_us; /* wiring test: ignore the Flipper until then */
    void (*on_peer_lost)(void);
} s;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void transport_write(void* ctx, const uint8_t* data, size_t len) {
    (void)ctx;
#if CONFIG_FES_LINK_UART
    uart_write_bytes(LINK_UART, data, len);
#else
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(50));
#endif
}

/* Whatever has arrived, waiting only for the first byte: uart_read_bytes() returns early only
 * once `len` bytes are in, so a short frame (an ACK) sat out the whole timeout, and the
 * Flipper's next stream frame waited for it (one in flight): ~10 ms per 511-byte frame. */
static int transport_read(uint8_t* buf, size_t len, uint32_t timeout_ms) {
#if CONFIG_FES_LINK_UART
    int n = uart_read_bytes(LINK_UART, buf, 1, pdMS_TO_TICKS(timeout_ms));
    if(n <= 0) return n;
    size_t more = 0;
    uart_get_buffered_data_len(LINK_UART, &more);
    if(more > len - 1) more = len - 1;
    if(more) {
        int m = uart_read_bytes(LINK_UART, buf + 1, more, 0);
        if(m > 0) n += m;
    }
    return n;
#else
    return usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(timeout_ms));
#endif
}

static void set_baud(uint32_t baud) {
#if CONFIG_FES_LINK_UART
    uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(100));
    uart_set_baudrate(LINK_UART, baud);
#endif
    s.baud = baud;
    s.switched_at_us = esp_timer_get_time();
    s.confirmed = false;
    proto_link_reset_peer(&s.link);
    ESP_LOGI(TAG, "baud %lu", (unsigned long)baud);
}

static void on_frame(void* ctx, const ProtoFrame* f) {
    (void)ctx;
    s.confirmed = true;
    s.peer = true;
    s.last_rx_us = esp_timer_get_time();
    s.rx_frames++;
    if(f->type == PROTO_T_ECHO) {
        /* link test (PROTOCOL.md): answered here, never reaches the app */
        proto_link_reply(&s.link, PROTO_T_ECHO_RESP, f->payload, f->len);
        return;
    }
    LinkRequest* req = malloc(sizeof(LinkRequest));
    if(!req) {
        link_reply_error(ProtoErrInternal, "out of memory");
        return;
    }
    uint16_t err = link_decode_request(f->type, f->payload, f->len, req);
    if(err != ProtoErrNone) {
        link_reply_error(err, err == ProtoErrUnknownType ? "unknown type" : "bad payload");
    } else if(s.handler) {
        s.handler(req, s.ctx);
    }
    memset(req, 0, sizeof(*req)); /* may hold a password */
    free(req);
}

static void on_reply(void* ctx, const ProtoFrame* f, uint8_t req) {
    (void)ctx;
    (void)f;
    (void)req;
    s.confirmed = true;
    s.peer = true;
    s.last_rx_us = esp_timer_get_time();
}

static void on_timeout(void* ctx, uint8_t req, uint16_t seq) {
    (void)ctx;
    (void)seq;
    ESP_LOGW(TAG, "no ACK for %s; Flipper gone?", proto_type_name(req));
    s.peer = false;
    if(s.on_peer_lost) s.on_peer_lost(); /* its SSH session has no owner now */
}

void link_reply(uint8_t type, const uint8_t* payload, size_t len) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    proto_link_reply(&s.link, type, payload, (uint16_t)len);
    xSemaphoreGiveRecursive(s.lock);
}

void link_reply_ack(void) {
    link_reply(PROTO_T_ACK, NULL, 0);
}

void link_reply_error(uint16_t code, const char* msg) {
    ProtoError e = {.code = code};
    snprintf(e.msg, sizeof(e.msg), "%s", msg ? msg : "");
    uint8_t buf[80];
    size_t n = proto_enc_error(&e, buf, sizeof(buf));
    link_reply(PROTO_T_ERROR, buf, n);
}

void link_reply_hello(const ProtoHello* hello, const char* fw_version) {
    ProtoHelloResp r = {
        .proto_version = PROTO_VERSION,
        .max_baud = CONFIG_FES_LINK_MAX_BAUD,
        .baud = link_choose_baud(hello->max_baud, CONFIG_FES_LINK_MAX_BAUD),
    };
#if !CONFIG_FES_LINK_UART
    r.baud = s.baud; /* USB-Serial-JTAG has no baud rate */
#endif
    snprintf(r.version, sizeof(r.version), "%s", fw_version);
    uint8_t buf[64];
    size_t n = proto_enc_hello_resp(&r, buf, sizeof(buf));
    link_reply(PROTO_T_HELLO_RESP, buf, n);
    if(r.baud != s.baud && hello->proto_version == PROTO_VERSION) {
        s.pending_baud = r.baud;
        s.switch_at_us = esp_timer_get_time() + BAUD_SWITCH_DELAY_MS * 1000;
    }
}

bool link_send_event(uint8_t type, const uint8_t* payload, size_t len, uint32_t timeout_ms) {
    if(!s.peer) return false;
    uint32_t end = now_ms() + timeout_ms;
    while(true) {
        xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
        bool sent = false;
        if(!proto_link_busy(&s.link) && !s.pending_baud)
            sent = proto_link_send(&s.link, type, payload, (uint16_t)len, now_ms(), NULL);
        xSemaphoreGiveRecursive(s.lock);
        if(sent) return true;
        if(!s.peer || (int32_t)(now_ms() - end) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool link_peer_present(void) {
    return s.peer;
}

void link_send_idle_bytes(size_t n) {
    /* Wiring test: frame delimiters only, which every decoder skips. */
    static const uint8_t zeros[64] = {0};
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    while(n) {
        size_t k = n < sizeof(zeros) ? n : sizeof(zeros);
        transport_write(NULL, zeros, k);
        n -= k;
    }
    xSemaphoreGiveRecursive(s.lock);
}

void link_set_peer_lost_cb(void (*cb)(void)) {
    s.on_peer_lost = cb;
}

void link_mute(uint32_t ms) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    s.mute_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
    xSemaphoreGiveRecursive(s.lock);
}

void link_get_debug(LinkDebug* out) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    out->baud = s.baud;
    out->peer = s.peer;
    out->rx_bytes = s.rx_bytes;
    out->rx_frames = s.rx_frames;
    out->stats = s.link.stats;
    xSemaphoreGiveRecursive(s.lock);
}

static void link_task(void* arg) {
    (void)arg;
    uint8_t buf[256];
    while(true) {
        int n = transport_read(buf, sizeof(buf), 10);
        xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
        if(n > 0 && esp_timer_get_time() < s.mute_until_us) n = 0; /* wiring test: deaf */
        if(n > 0) {
            s.rx_bytes += (uint32_t)n;
            proto_link_rx(&s.link, buf, (size_t)n, now_ms());
        }
        proto_link_poll(&s.link, now_ms());
        int64_t t = esp_timer_get_time();
        if(s.pending_baud && t >= s.switch_at_us) {
            uint32_t b = s.pending_baud;
            s.pending_baud = 0;
            set_baud(b);
        }
        /* PROTOCOL §4.1: no valid frame within 1 s of switching -> back to 115200 */
        if(s.baud != PROTO_BAUD_INITIAL && !s.confirmed &&
           t - s.switched_at_us > BAUD_FALLBACK_MS * 1000)
            set_baud(PROTO_BAUD_INITIAL);
        /* PROTOCOL 4.3: silent for PEER_IDLE_MS (the app exited, or the wires came off):
         * the Flipper is gone; listen at 115200 for the next HELLO */
        if(t - s.last_rx_us > PEER_IDLE_MS * 1000) {
            /* Driven by silence alone: on_timeout() may already have cleared s.peer, and
             * the board then stayed at 921600, deaf to the next app's HELLO (hardware). */
            if(s.peer) {
                s.peer = false;
                proto_link_reset_peer(&s.link);
                ESP_LOGI(TAG, "Flipper silent for %d ms: peer gone", PEER_IDLE_MS);
                if(s.on_peer_lost) s.on_peer_lost();
            }
            if(s.baud != PROTO_BAUD_INITIAL) {
                ESP_LOGI(TAG, "no Flipper: back to %d baud", PROTO_BAUD_INITIAL);
                set_baud(PROTO_BAUD_INITIAL);
            }
        }
        xSemaphoreGiveRecursive(s.lock);
    }
}

esp_err_t link_init(LinkRequestHandler handler, void* ctx) {
    s.handler = handler;
    s.ctx = ctx;
    s.lock = xSemaphoreCreateRecursiveMutex();
    s.baud = PROTO_BAUD_INITIAL;
    ProtoLinkCallbacks cb = {
        .write = transport_write,
        .on_frame = on_frame,
        .on_reply = on_reply,
        .on_timeout = on_timeout};
    proto_link_init(&s.link, &cb);
#if CONFIG_FES_LINK_UART
    uart_config_t uc = {
        .baud_rate = PROTO_BAUD_INITIAL,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 4096, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(
        LINK_UART,
        CONFIG_FES_UART_TX_GPIO,
        CONFIG_FES_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE));
#else
    usb_serial_jtag_driver_config_t jc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&jc));
#endif
    BaseType_t ok = xTaskCreatePinnedToCore(link_task, "link", 6144, NULL, 6, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
