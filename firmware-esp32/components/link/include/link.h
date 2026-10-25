/*
 * Flipper link: proto/ framing + reliability over UART1 (the Flipper) or the
 * USB-Serial-JTAG port (HIL harness), selected in Kconfig.
 *
 * Requests are decoded and handed to the handler on the link task; the handler must
 * answer each one with exactly one link_reply*() call, quickly (long work goes to
 * other tasks). Events from any task use link_send_event(), which waits for the
 * previous event's ACK (stop-and-wait, PROTOCOL.md §3).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include "proto/proto_link.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "link_dispatch.h"

typedef void (*LinkRequestHandler)(const LinkRequest* req, void* ctx);

esp_err_t link_init(LinkRequestHandler handler, void* ctx);
void link_reply_ack(void);
void link_reply_error(uint16_t code, const char* msg);
void link_reply(uint8_t type, const uint8_t* payload, size_t len);
/* Answer a HELLO: replies HELLO_RESP and switches baud rate afterwards. */
void link_reply_hello(const ProtoHello* hello, const char* fw_version);
/* false if the Flipper is absent or did not ACK in time. */
bool link_send_event(uint8_t type, const uint8_t* payload, size_t len, uint32_t timeout_ms);
bool link_peer_present(void);

typedef struct {
    uint32_t baud;
    bool peer;
    uint32_t rx_bytes; /* raw bytes from the transport */
    uint32_t rx_frames; /* valid requests (CRC ok) */
    ProtoLinkStats stats;
} LinkDebug;
void link_get_debug(LinkDebug* out);
/* Wiring test: send n frame delimiters (0x00) to the Flipper. */
void link_send_idle_bytes(size_t n);
/* Wiring test: ignore everything from the Flipper for ms, as if the wires were pulled. */
void link_mute(uint32_t ms);
/* Called from the link task when the Flipper went silent (app gone, wires off). */
void link_set_peer_lost_cb(void (*cb)(void));
