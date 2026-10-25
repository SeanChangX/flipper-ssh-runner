/* Ping summary maths (components/net/ping_stats.c). SPDX-License-Identifier: GPL-3.0-only
 *
 * On the board, a ping cut short by a pulled cable printed "2/2 replies, min/avg/max 1/501/1
 * ms": the average was the session's whole duration (with the lost request's timeout) over
 * the replies, and the count the requests actually sent rather than the ones asked. */
#include "ping_stats.h"

#include <stdio.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if(!(c)) {                                                       \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while(0)

int main(void) {
    PingStats p;
    ProtoPingResp r;

    /* a full run */
    ping_stats_start(&p, 3);
    ping_stats_reply(&p, 1);
    ping_stats_reply(&p, 5);
    ping_stats_reply(&p, 3);
    ping_stats_result(&p, 3, ProtoIfaceLan, &r);
    CHECK(r.sent == 3 && r.received == 3);
    CHECK(r.min_ms == 1 && r.avg_ms == 3 && r.max_ms == 5);
    CHECK(r.iface == ProtoIfaceLan);

    /* the cable pulled after two replies: the stack sent 2 of 3 */
    ping_stats_start(&p, 3);
    ping_stats_reply(&p, 1);
    ping_stats_reply(&p, 1);
    ping_stats_result(&p, 2, ProtoIfaceLan, &r);
    CHECK(r.sent == 3 && r.received == 2);
    CHECK(r.min_ms == 1 && r.avg_ms == 1 && r.max_ms == 1);
    CHECK(r.avg_ms <= r.max_ms && r.min_ms <= r.avg_ms);

    /* one reply lost (timed out) in the middle */
    ping_stats_start(&p, 4);
    ping_stats_reply(&p, 2);
    ping_stats_reply(&p, 4);
    ping_stats_reply(&p, 3);
    ping_stats_result(&p, 4, ProtoIfaceWifi, &r);
    CHECK(r.sent == 4 && r.received == 3);
    CHECK(r.min_ms == 2 && r.avg_ms == 3 && r.max_ms == 4);

    /* no reply at all: no times */
    ping_stats_start(&p, 4);
    ping_stats_result(&p, 1, ProtoIfaceWifi, &r);
    CHECK(r.sent == 4 && r.received == 0);
    CHECK(r.min_ms == 0 && r.avg_ms == 0 && r.max_ms == 0);

    /* rounding, and a slow reply beyond 16 bits clamps */
    ping_stats_start(&p, 2);
    ping_stats_reply(&p, 1);
    ping_stats_reply(&p, 2);
    ping_stats_result(&p, 2, ProtoIfaceLan, &r);
    CHECK(r.avg_ms == 2); /* 1.5 rounds up */
    ping_stats_start(&p, 1);
    ping_stats_reply(&p, 70000);
    ping_stats_result(&p, 1, ProtoIfaceLan, &r);
    CHECK(r.max_ms == UINT16_MAX && r.avg_ms == UINT16_MAX);

    if(failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("ping_stats: all checks passed\n");
    return 0;
}
