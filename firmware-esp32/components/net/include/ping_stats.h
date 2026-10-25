/* Ping summary maths, without ESP-IDF (host-tested in test_host/test_ping_stats.c).
 * SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <proto/proto_msg.h>

#include <stdint.h>

/* min/avg/max come from the same reply samples, so avg <= max always holds, and a run cut
 * short (the cable pulled mid-ping) still reports replies out of the count asked. */
typedef struct {
    uint8_t asked;
    uint8_t replies;
    uint32_t sum_ms, min_ms, max_ms;
} PingStats;

void ping_stats_start(PingStats* p, uint8_t asked);
void ping_stats_reply(PingStats* p, uint32_t ms);
/* `sent` is what the stack says it sent: more than asked never shows less than asked. */
void ping_stats_result(const PingStats* p, uint32_t sent, uint8_t iface, ProtoPingResp* out);
