/* Ping summary maths (see ping_stats.h). SPDX-License-Identifier: GPL-3.0-only */
#include "ping_stats.h"

#include <string.h>

void ping_stats_start(PingStats* p, uint8_t asked) {
    memset(p, 0, sizeof(*p));
    p->asked = asked;
}

void ping_stats_reply(PingStats* p, uint32_t ms) {
    if(p->replies == UINT8_MAX) return;
    if(!p->replies || ms < p->min_ms) p->min_ms = ms;
    if(ms > p->max_ms) p->max_ms = ms;
    p->sum_ms += ms;
    p->replies++;
}

static uint16_t ms16(uint32_t ms) {
    return ms > UINT16_MAX ? UINT16_MAX : (uint16_t)ms;
}

void ping_stats_result(const PingStats* p, uint32_t sent, uint8_t iface, ProtoPingResp* out) {
    memset(out, 0, sizeof(*out));
    uint32_t n = sent > p->asked ? sent : p->asked;
    if(n < p->replies) n = p->replies;
    out->sent = (uint8_t)(n > UINT8_MAX ? UINT8_MAX : n);
    out->received = p->replies;
    if(p->replies) {
        out->min_ms = ms16(p->min_ms);
        out->avg_ms = ms16((p->sum_ms + p->replies / 2u) / p->replies);
        out->max_ms = ms16(p->max_ms);
    }
    out->iface = iface;
}
