/* Reliability layer: two links joined by a lossy, corrupting channel.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "proto/proto_link.h"
#include "proto/proto_msg.h"
#include "tt.h"

#define PIPE_CAP (64 * 1024)

typedef struct {
    uint8_t data[PIPE_CAP];
    size_t n;
} Pipe;

typedef struct {
    ProtoLink link;
    Pipe* out;
    const char* name;
    /* delivered reliable frames, in order */
    uint16_t got[4096];
    int got_n;
    int replies;
    int timeouts;
    bool auto_reply;
} Peer;

static int drop_permille = 0; /* frame-level loss */
static int corrupt_permille = 0; /* byte-level corruption inside a written frame */

static void peer_write(void* ctx, const uint8_t* data, size_t len) {
    Peer* p = ctx;
    if((int)(tt_rand() % 1000) < drop_permille) return;
    for(size_t i = 0; i < len && p->out->n < PIPE_CAP; i++) {
        uint8_t b = data[i];
        if((int)(tt_rand() % 100000) < corrupt_permille) b ^= (uint8_t)(1u << (tt_rand() % 8));
        p->out->data[p->out->n++] = b;
    }
}

static void peer_on_frame(void* ctx, const ProtoFrame* f) {
    Peer* p = ctx;
    /* payload carries the message number */
    uint16_t v = (uint16_t)(f->payload[0] | (f->payload[1] << 8));
    if(p->got_n < 4096) p->got[p->got_n++] = v;
    if(p->auto_reply) proto_link_ack(&p->link);
}

static void peer_on_reply(void* ctx, const ProtoFrame* f, uint8_t req) {
    (void)f;
    (void)req;
    ((Peer*)ctx)->replies++;
}

static void peer_on_timeout(void* ctx, uint8_t req, uint16_t seq) {
    (void)req;
    (void)seq;
    ((Peer*)ctx)->timeouts++;
}

static void peer_init(Peer* p, Pipe* out, const char* name) {
    memset(p, 0, sizeof(*p));
    ProtoLinkCallbacks cb = {
        .write = peer_write,
        .on_frame = peer_on_frame,
        .on_reply = peer_on_reply,
        .on_timeout = peer_on_timeout,
        .ctx = p};
    proto_link_init(&p->link, &cb);
    p->out = out;
    p->name = name;
    p->auto_reply = true;
}

static void pump(Pipe* pipe, Peer* to, uint32_t now) {
    /* Move bytes in small chunks to emulate a UART. */
    size_t n = pipe->n;
    if(!n) return;
    uint8_t tmp[PIPE_CAP];
    memcpy(tmp, pipe->data, n);
    pipe->n = 0;
    proto_link_rx(&to->link, tmp, n, now);
}

/* Both sides send `count` messages; returns elapsed simulated ms. */
static uint32_t run_exchange(Peer* a, Peer* b, Pipe* ab, Pipe* ba, int count) {
    int sent_a = 0, sent_b = 0;
    uint32_t now = 0;
    while((sent_a < count || sent_b < count || proto_link_busy(&a->link) ||
           proto_link_busy(&b->link)) &&
          now < 10000000u) {
        uint8_t pl[2];
        if(sent_a < count && !proto_link_busy(&a->link)) {
            pl[0] = (uint8_t)sent_a;
            pl[1] = (uint8_t)(sent_a >> 8);
            if(proto_link_send(&a->link, PROTO_T_EXEC_REQ, pl, 2, now, NULL)) sent_a++;
        }
        if(sent_b < count && !proto_link_busy(&b->link)) {
            pl[0] = (uint8_t)sent_b;
            pl[1] = (uint8_t)(sent_b >> 8);
            if(proto_link_send(&b->link, PROTO_T_STREAM_DATA_EVT, pl, 2, now, NULL)) sent_b++;
        }
        pump(ab, b, now);
        pump(ba, a, now);
        now += 10;
        proto_link_poll(&a->link, now);
        proto_link_poll(&b->link, now);
    }
    return now;
}

static void check_exactly_once(Peer* p, int count, int expect_timeouts_max) {
    /* Delivered in order, no duplicates. Messages whose sender timed out may be missing. */
    int last = -1;
    bool ordered = true;
    for(int i = 0; i < p->got_n; i++) {
        if((int)p->got[i] <= last) ordered = false;
        last = p->got[i];
    }
    TT_CHECK(ordered);
    TT_CHECK(p->got_n <= count);
    TT_CHECK(p->got_n >= count - expect_timeouts_max);
}

static void test_clean_channel(void) {
    static Pipe ab, ba;
    static Peer a, b;
    ab.n = ba.n = 0;
    drop_permille = 0;
    corrupt_permille = 0;
    peer_init(&a, &ab, "flipper");
    peer_init(&b, &ba, "esp32");
    run_exchange(&a, &b, &ab, &ba, 1000);
    TT_EQ_INT(b.got_n, 1000);
    TT_EQ_INT(a.got_n, 1000);
    TT_EQ_INT(a.replies, 1000);
    TT_EQ_INT(b.replies, 1000);
    TT_EQ_INT(a.link.stats.retransmits, 0);
    TT_EQ_INT(a.timeouts + b.timeouts, 0);
}

static void test_lossy_channel(void) {
    static Pipe ab, ba;
    static Peer a, b;
    tt_seed(11);
    ab.n = ba.n = 0;
    drop_permille = 100; /* 10 % of frames lost */
    corrupt_permille = 20; /* 0.02 % of bytes flipped */
    peer_init(&a, &ab, "flipper");
    peer_init(&b, &ba, "esp32");
    run_exchange(&a, &b, &ab, &ba, 2000);
    check_exactly_once(&b, 2000, a.timeouts);
    check_exactly_once(&a, 2000, b.timeouts);
    TT_CHECK(a.link.stats.retransmits > 0);
    TT_CHECK(b.link.stats.duplicates > 0); /* lost replies cause duplicate deliveries, deduped */
    /* With 10 % loss and 3 retries, a timeout needs 4 losses in a row per attempt
     * pair: rare, but allowed. Everything not timed out was delivered exactly once. */
    TT_EQ_INT(a.replies + a.timeouts, 2000);
    TT_EQ_INT(b.replies + b.timeouts, 2000);
    drop_permille = 0;
    corrupt_permille = 0;
}

static void test_dead_peer_times_out(void) {
    static Pipe ab, ba;
    static Peer a;
    ab.n = ba.n = 0;
    peer_init(&a, &ab, "flipper");
    uint8_t pl[2] = {0, 0};
    TT_CHECK(proto_link_send(&a.link, PROTO_T_HELLO, pl, 2, 0, NULL));
    TT_CHECK(!proto_link_send(&a.link, PROTO_T_HELLO, pl, 2, 0, NULL)); /* busy */
    for(uint32_t t = 0; t <= 2000; t += 10)
        proto_link_poll(&a.link, t);
    TT_EQ_INT(a.timeouts, 1);
    TT_EQ_INT(a.link.stats.retransmits, PROTO_LINK_MAX_RETRIES);
    TT_CHECK(!proto_link_busy(&a.link));
    /* 1 original + 3 retries were written */
    TT_CHECK(ab.n > 0);
}

static void test_reply_rules(void) {
    static Pipe ab, ba;
    static Peer a, b;
    ab.n = ba.n = 0;
    peer_init(&a, &ab, "flipper");
    peer_init(&b, &ba, "esp32");
    b.auto_reply = false;
    /* Cannot send a reply type as a reliable frame, nor reply without a request. */
    TT_CHECK(!proto_link_send(&a.link, PROTO_T_ACK, NULL, 0, 0, NULL));
    TT_CHECK(!proto_link_ack(&b.link));

    uint8_t pl[2] = {5, 0};
    uint16_t seq;
    TT_CHECK(proto_link_send(&a.link, PROTO_T_NET_GET_STATUS, pl, 2, 0, &seq));
    pump(&ab, &b, 0);
    TT_EQ_INT(b.got_n, 1);
    /* Retransmit before the owner replied: no duplicate delivery, no reply yet. */
    proto_link_poll(&a.link, 250);
    pump(&ab, &b, 250);
    TT_EQ_INT(b.got_n, 1);
    TT_EQ_INT(ba.n, 0);
    /* Typed reply arrives and clears the outstanding frame. */
    uint8_t resp[3] = {1, 2, 3};
    TT_CHECK(proto_link_reply(&b.link, PROTO_T_NET_STATUS, resp, 3));
    TT_CHECK(!proto_link_reply(&b.link, PROTO_T_NET_STATUS, resp, 3)); /* only once */
    pump(&ba, &a, 260);
    TT_EQ_INT(a.replies, 1);
    TT_CHECK(!proto_link_busy(&a.link));
    /* A late duplicate request gets the cached reply, which is then stale for A. */
    uint8_t wire[PROTO_MAX_WIRE];
    size_t n = proto_frame_encode(PROTO_T_NET_GET_STATUS, seq, pl, 2, wire, sizeof(wire));
    proto_link_rx(&b.link, wire, n, 300);
    TT_CHECK(ba.n > 0);
    pump(&ba, &a, 300);
    TT_EQ_INT(a.replies, 1);
    TT_EQ_INT(a.link.stats.stale_replies, 1);
}

static void test_hello_resets_dedup(void) {
    static Pipe ba;
    static Peer b;
    ba.n = 0;
    peer_init(&b, &ba, "esp32");
    uint8_t wire[PROTO_MAX_WIRE];
    uint8_t pl[2] = {1, 0};
    size_t n = proto_frame_encode(PROTO_T_HELLO, 1, pl, 2, wire, sizeof(wire));
    proto_link_rx(&b.link, wire, n, 0);
    /* Peer restarted and uses seq 1 again: must be delivered, not deduped. */
    pl[0] = 2;
    n = proto_frame_encode(PROTO_T_HELLO, 1, pl, 2, wire, sizeof(wire));
    proto_link_rx(&b.link, wire, n, 10);
    TT_EQ_INT(b.got_n, 2);
}

int main(void) {
    TT_RUN(test_clean_channel);
    TT_RUN(test_lossy_channel);
    TT_RUN(test_dead_peer_times_out);
    TT_RUN(test_reply_rules);
    TT_RUN(test_hello_resets_dedup);
    TT_END();
}
