/* Framing tests: CRC, COBS, round trips, fuzzing, CRC errors and resync.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "proto/proto.h"
#include "tt.h"

static void test_crc16_vector(void) {
    const char* s = "123456789";
    TT_EQ_INT(proto_crc16((const uint8_t*)s, 9, PROTO_CRC16_INIT), 0x29B1);
    TT_EQ_INT(proto_crc16(NULL, 0, PROTO_CRC16_INIT), 0xFFFF);
}

static void cobs_vec(const uint8_t* in, size_t n, const uint8_t* want, size_t want_n) {
    uint8_t enc[600], dec[600];
    size_t e = proto_cobs_encode(in, n, enc, sizeof(enc));
    TT_EQ_INT(e, want_n);
    TT_CHECK(memcmp(enc, want, want_n) == 0);
    for(size_t i = 0; i < e; i++)
        TT_CHECK(enc[i] != 0);
    size_t d = proto_cobs_decode(enc, e, dec, sizeof(dec));
    TT_EQ_INT(d, n);
    TT_CHECK(n == 0 || memcmp(dec, in, n) == 0);
}

static void test_cobs_vectors(void) {
    /* Examples from the COBS paper / Wikipedia. */
    cobs_vec((const uint8_t[]){0x00}, 1, (const uint8_t[]){0x01, 0x01}, 2);
    cobs_vec((const uint8_t[]){0x00, 0x00}, 2, (const uint8_t[]){0x01, 0x01, 0x01}, 3);
    cobs_vec(
        (const uint8_t[]){0x11, 0x22, 0x00, 0x33},
        4,
        (const uint8_t[]){0x03, 0x11, 0x22, 0x02, 0x33},
        5);
    cobs_vec(
        (const uint8_t[]){0x11, 0x00, 0x00, 0x00},
        4,
        (const uint8_t[]){0x02, 0x11, 0x01, 0x01, 0x01},
        5);

    /* 254 non-zero bytes -> FF + 254 bytes + 01 */
    uint8_t in[255], want[257];
    for(int i = 0; i < 254; i++)
        in[i] = (uint8_t)(i + 1);
    want[0] = 0xFF;
    memcpy(&want[1], in, 254);
    want[255] = 0x01;
    cobs_vec(in, 254, want, 256);

    /* 255 non-zero bytes -> FF + 254 bytes + 02 + last byte */
    in[254] = 0xFF;
    want[255] = 0x02;
    want[256] = 0xFF;
    cobs_vec(in, 255, want, 257);
}

static void test_cobs_rejects_garbage(void) {
    uint8_t out[16];
    /* Zero inside encoded data is illegal. */
    TT_EQ_INT(proto_cobs_decode((const uint8_t[]){0x03, 0x11, 0x00}, 3, out, sizeof(out)), 0);
    /* Code byte runs past the end. */
    TT_EQ_INT(proto_cobs_decode((const uint8_t[]){0x05, 0x11}, 2, out, sizeof(out)), 0);
    /* Output too small. */
    TT_EQ_INT(proto_cobs_decode((const uint8_t[]){0x04, 1, 2, 3}, 4, out, 2), 0);
    /* Encode output too small. */
    TT_EQ_INT(proto_cobs_encode((const uint8_t[]){1, 2, 3}, 3, out, 3), 0);
}

static int feed(ProtoDecoder* d, const uint8_t* wire, size_t n, ProtoFrame* f, int* errors) {
    int frames = 0;
    for(size_t i = 0; i < n; i++) {
        ProtoDecResult r = proto_decoder_push(d, wire[i], f);
        if(r == ProtoDecFrame)
            frames++;
        else if(r != ProtoDecNone && errors)
            (*errors)++;
    }
    return frames;
}

static void test_roundtrip_fuzz(void) {
    tt_seed(1);
    ProtoDecoder d;
    proto_decoder_init(&d);
    uint8_t payload[PROTO_MAX_PAYLOAD];
    uint8_t wire[PROTO_MAX_WIRE];
    for(int iter = 0; iter < 20000; iter++) {
        uint16_t len = (uint16_t)(tt_rand() % (PROTO_MAX_PAYLOAD + 1));
        /* Bias some payloads towards zeros and 0xFF runs to stress COBS. */
        int mode = (int)(tt_rand() % 4);
        for(uint16_t i = 0; i < len; i++) {
            uint32_t r = tt_rand();
            payload[i] = mode == 0 ? 0 :
                         mode == 1 ? 0xFF :
                         mode == 2 ? (uint8_t)(r % 3) :
                                     (uint8_t)r;
        }
        uint8_t type = (uint8_t)tt_rand();
        uint16_t seq = (uint16_t)tt_rand();
        size_t n = proto_frame_encode(type, seq, payload, len, wire, sizeof(wire));
        TT_CHECK(n > 0 && n <= PROTO_MAX_WIRE);
        TT_CHECK(wire[0] == 0 && wire[n - 1] == 0);
        for(size_t i = 1; i + 1 < n; i++)
            if(wire[i] == 0) {
                TT_CHECK(!"zero inside frame");
                break;
            }
        ProtoFrame f;
        int frames = feed(&d, wire, n, &f, NULL);
        TT_EQ_INT(frames, 1);
        if(frames == 1) {
            TT_EQ_INT(f.type, type);
            TT_EQ_INT(f.seq, seq);
            TT_EQ_INT(f.len, len);
            TT_CHECK(len == 0 || memcmp(f.payload, payload, len) == 0);
        }
    }
    TT_EQ_INT(d.stats.frames, 20000);
    TT_EQ_INT(d.stats.err_crc + d.stats.err_cobs + d.stats.err_len + d.stats.err_overflow, 0);
}

static void test_oversize_payload_rejected(void) {
    uint8_t payload[PROTO_MAX_PAYLOAD + 1] = {0};
    uint8_t wire[PROTO_MAX_WIRE + 8];
    TT_EQ_INT(proto_frame_encode(1, 1, payload, PROTO_MAX_PAYLOAD + 1, wire, sizeof(wire)), 0);
    TT_EQ_INT(proto_frame_encode(1, 1, payload, 10, wire, 5), 0);
}

/* Flip one bit anywhere inside a frame: it must never decode as a valid frame,
 * and the following good frame must decode (resync). */
static void test_bitflip_then_resync(void) {
    tt_seed(2);
    uint8_t payload[64], wire[PROTO_MAX_WIRE], good[PROTO_MAX_WIRE];
    int undetected = 0;
    for(int iter = 0; iter < 5000; iter++) {
        ProtoDecoder d;
        proto_decoder_init(&d);
        uint16_t len = (uint16_t)(tt_rand() % sizeof(payload));
        for(uint16_t i = 0; i < len; i++)
            payload[i] = (uint8_t)tt_rand();
        size_t n = proto_frame_encode(0x30, 7, payload, len, wire, sizeof(wire));
        size_t pos = 1 + tt_rand() % (n - 2);
        wire[pos] ^= (uint8_t)(1u << (tt_rand() % 8));
        ProtoFrame f;
        int errs = 0;
        int frames = feed(&d, wire, n, &f, &errs);
        if(frames) undetected++;
        size_t g = proto_frame_encode(0x31, 8, (const uint8_t*)"ok", 2, good, sizeof(good));
        frames = feed(&d, good, g, &f, NULL);
        TT_EQ_INT(frames, 1);
        if(frames == 1) {
            TT_EQ_INT(f.type, 0x31);
            TT_EQ_INT(f.seq, 8);
        }
    }
    /* A single bit flip is always caught by CRC-16, or splits the frame at a new zero
     * byte, which COBS/length checks reject. */
    TT_EQ_INT(undetected, 0);
}

static void test_crc_error_counted(void) {
    ProtoDecoder d;
    proto_decoder_init(&d);
    uint8_t wire[PROTO_MAX_WIRE];
    size_t n = proto_frame_encode(0x21, 3, (const uint8_t*)"hello", 5, wire, sizeof(wire));
    /* Corrupt a payload byte without creating a zero: XOR 0x01 on a byte that isn't 0x01. */
    size_t pos = n - 4;
    wire[pos] = (uint8_t)(wire[pos] == 0x02 ? 0x03 : (wire[pos] ^ 0x80));
    if(wire[pos] == 0) wire[pos] = 0x55;
    ProtoFrame f;
    int errs = 0;
    TT_EQ_INT(feed(&d, wire, n, &f, &errs), 0);
    TT_EQ_INT(errs, 1);
    TT_CHECK(d.stats.err_crc + d.stats.err_cobs + d.stats.err_len == 1);
}

static void test_truncated_frames(void) {
    tt_seed(3);
    uint8_t payload[200], wire[PROTO_MAX_WIRE], good[PROTO_MAX_WIRE];
    for(int iter = 0; iter < 2000; iter++) {
        ProtoDecoder d;
        proto_decoder_init(&d);
        uint16_t len = (uint16_t)(tt_rand() % sizeof(payload));
        for(uint16_t i = 0; i < len; i++)
            payload[i] = (uint8_t)tt_rand();
        size_t n = proto_frame_encode(0x10, 1, payload, len, wire, sizeof(wire));
        size_t cut = 1 + tt_rand() % (n - 2); /* keep the leading 0, drop the tail */
        ProtoFrame f;
        int frames = feed(&d, wire, cut, &f, NULL);
        /* Cable unplugged mid-frame, then the next frame starts with its own 0x00. */
        size_t g = proto_frame_encode(0x11, 2, payload, len, good, sizeof(good));
        frames += feed(&d, good, g, &f, NULL);
        TT_EQ_INT(frames, 1);
        TT_EQ_INT(f.type, 0x11);
        TT_EQ_INT(f.len, len);
    }
}

static void test_overflow_resync(void) {
    ProtoDecoder d;
    proto_decoder_init(&d);
    ProtoFrame f;
    int overflow = 0;
    for(size_t i = 0; i < PROTO_MAX_WIRE * 3; i++)
        if(proto_decoder_push(&d, 0x41, &f) == ProtoDecErrOverflow) overflow++;
    TT_EQ_INT(overflow, 1);
    TT_EQ_INT(proto_decoder_push(&d, 0x00, &f), ProtoDecNone);
    uint8_t wire[PROTO_MAX_WIRE];
    size_t n = proto_frame_encode(0x01, 9, NULL, 0, wire, sizeof(wire));
    TT_EQ_INT(feed(&d, wire, n, &f, NULL), 1);
    TT_EQ_INT(f.seq, 9);
}

static void test_noise_never_crashes(void) {
    tt_seed(4);
    ProtoDecoder d;
    proto_decoder_init(&d);
    ProtoFrame f;
    int valid = 0;
    for(int i = 0; i < 2000000; i++) {
        uint32_t r = tt_rand();
        uint8_t b = (r & 0x700) == 0 ? 0 : (uint8_t)r; /* ~1/8 zeros */
        if(proto_decoder_push(&d, b, &f) == ProtoDecFrame) {
            valid++;
            TT_CHECK(f.len <= PROTO_MAX_PAYLOAD);
        }
    }
    /* Random noise passing CRC-16 + exact length is astronomically rare. */
    TT_CHECK(valid <= 1);
}

static void test_back_to_back_and_garbage_prefix(void) {
    ProtoDecoder d;
    proto_decoder_init(&d);
    uint8_t stream[3 * PROTO_MAX_WIRE + 16];
    size_t n = 0;
    const uint8_t junk[] = {0x13, 0x37, 0xFF, 0xFE};
    memcpy(stream, junk, sizeof(junk));
    n += sizeof(junk);
    for(uint16_t s = 0; s < 3; s++)
        n += proto_frame_encode(0x31, s, (const uint8_t*)"abc", 3, &stream[n], sizeof(stream) - n);
    ProtoFrame f;
    int errs = 0;
    TT_EQ_INT(feed(&d, stream, n, &f, &errs), 3);
    TT_EQ_INT(errs, 1); /* the junk prefix */
}

int main(void) {
    TT_RUN(test_crc16_vector);
    TT_RUN(test_cobs_vectors);
    TT_RUN(test_cobs_rejects_garbage);
    TT_RUN(test_roundtrip_fuzz);
    TT_RUN(test_oversize_payload_rejected);
    TT_RUN(test_bitflip_then_resync);
    TT_RUN(test_crc_error_counted);
    TT_RUN(test_truncated_frames);
    TT_RUN(test_overflow_resync);
    TT_RUN(test_noise_never_crashes);
    TT_RUN(test_back_to_back_and_garbage_prefix);
    TT_END();
}
