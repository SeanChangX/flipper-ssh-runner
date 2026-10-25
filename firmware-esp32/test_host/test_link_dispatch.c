/* ESP32 link: request validation, baud choice, script reassembly. SPDX-License-Identifier: GPL-3.0-only */
#include "link_dispatch.h"
#include "tt.h"

#include <string.h>

static uint8_t buf[PROTO_MAX_PAYLOAD];

static void test_decode_requests(void) {
    LinkRequest r;
    ProtoSshConnect c = {.port = 22, .transport = ProtoTransportLan, .auth = ProtoAuthPassword};
    strcpy(c.profile, "nas");
    strcpy(c.user, "user");
    strcpy(c.host, "192.168.1.10");
    size_t n = proto_enc_ssh_connect(&c, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_SSH_CONNECT, buf, n, &r), ProtoErrNone);
    TT_EQ_STR(r.conn.host, "192.168.1.10");
    /* empty profile is refused */
    c.profile[0] = 0;
    n = proto_enc_ssh_connect(&c, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_SSH_CONNECT, buf, n, &r), ProtoErrBadPayload);

    const char* cmd = "uname -a";
    ProtoExecReq e = {.len = 8, .command = cmd};
    n = proto_enc_exec_req(&e, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_EXEC_REQ, buf, n, &r), ProtoErrNone);
    TT_EQ_STR(r.command, "uname -a");

    /* empty-payload requests must really be empty */
    TT_EQ_INT(link_decode_request(PROTO_T_CANCEL, NULL, 0, &r), ProtoErrNone);
    TT_EQ_INT(link_decode_request(PROTO_T_CANCEL, buf, 1, &r), ProtoErrBadPayload);

    /* unknown and reserved types */
    TT_EQ_INT(link_decode_request(0x7A, NULL, 0, &r), ProtoErrUnknownType);
    TT_EQ_INT(link_decode_request(PROTO_T_SHELL_OPEN, NULL, 0, &r), ProtoErrUnknownType);

    /* script interpreter allow-list */
    ProtoScriptBegin b = {.total = 10};
    strcpy(b.interp, "python3");
    n = proto_enc_script_begin(&b, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_SCRIPT_BEGIN, buf, n, &r), ProtoErrBadPayload);
    strcpy(b.interp, "bash");
    n = proto_enc_script_begin(&b, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_SCRIPT_BEGIN, buf, n, &r), ProtoErrNone);

    ProtoKeyGen g = {.key_type = ProtoKeyGenEcdsaP256};
    n = proto_enc_key_gen(&g, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_KEY_GEN, buf, n, &r), ProtoErrNone);
    g.key_type = 2; /* no RSA generation */
    n = proto_enc_key_gen(&g, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_KEY_GEN, buf, n, &r), ProtoErrBadPayload);

    /* Keys: KEY_GET_PUB (v0) is gone; forget takes a kind; legacy export is empty */
    TT_EQ_INT(link_decode_request(0x41, NULL, 0, &r), ProtoErrUnknownType);
    ProtoSecretForget f = {.kind = ProtoForgetPassphrase};
    strcpy(f.id, "SHA256:abc");
    n = proto_enc_secret_forget(&f, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_SECRET_FORGET, buf, n, &r), ProtoErrNone);
    TT_EQ_INT(r.forget.kind, ProtoForgetPassphrase);
    TT_EQ_INT(link_decode_request(PROTO_T_KEY_LEGACY_EXPORT, NULL, 0, &r), ProtoErrNone);
    ProtoKeyBlobBegin kb = {.total = PROTO_KEY_MAX + 1};
    n = proto_enc_key_blob_begin(&kb, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_KEY_BLOB_BEGIN, buf, n, &r), ProtoErrBadPayload);

    /* Security: SEC_ENABLE reaches the app only with tier 1 and the typed word */
    TT_EQ_INT(link_decode_request(PROTO_T_SEC_GET_STATUS, NULL, 0, &r), ProtoErrNone);
    static const char* const good[] = {"confirm", "CONFIRM", " Confirm\t"};
    for(size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        ProtoSecEnable se = {.tier = ProtoSecTierProtect};
        strcpy(se.word, good[i]);
        n = proto_enc_sec_enable(&se, buf, sizeof(buf));
        TT_EQ_INT(link_decode_request(PROTO_T_SEC_ENABLE, buf, n, &r), ProtoErrNone);
    }
    static const char* const bad[] = {
        "", "   ", "confirm.", "con firm", "confirmx", "conf", "yes"};
    for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ProtoSecEnable se = {.tier = ProtoSecTierProtect};
        strcpy(se.word, bad[i]);
        n = proto_enc_sec_enable(&se, buf, sizeof(buf));
        TT_EQ_INT(link_decode_request(PROTO_T_SEC_ENABLE, buf, n, &r), ProtoErrBadPayload);
    }
    ProtoSecEnable lock = {
        .tier = ProtoSecTierLock, .word = "confirm"}; /* Tier 2 is not offered */
    n = proto_enc_sec_enable(&lock, buf, sizeof(buf));
    TT_EQ_INT(link_decode_request(PROTO_T_SEC_ENABLE, buf, n, &r), ProtoErrBadPayload);
}

/* KEY_BLOB staging into a buffer that is wiped afterwards. */
static void test_key_blob(void) {
    static uint8_t store[PROTO_KEY_MAX + 1];
    ScriptAsm s;
    script_asm_init(&s, store, PROTO_KEY_MAX);
    TT_EQ_INT(blob_asm_begin(&s, PROTO_KEY_MAX + 1), ProtoErrTooLarge);
    const char* key =
        "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjE=\n-----END OPENSSH PRIVATE KEY-----\n";
    uint32_t total = (uint32_t)strlen(key);
    TT_EQ_INT(blob_asm_begin(&s, total), ProtoErrNone);
    ProtoScriptChunk c = {.offset = 0, .len = 40, .data = (const uint8_t*)key};
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrNone);
    c = (ProtoScriptChunk){
        .offset = 40, .len = (uint16_t)(total - 40), .data = (const uint8_t*)key + 40};
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrNone);
    ProtoScriptEnd e = {.crc32 = proto_crc32((const uint8_t*)key, total, 0)};
    TT_EQ_INT(script_asm_end(&s, &e), ProtoErrNone);
    TT_CHECK(memcmp(store, key, total) == 0);
    script_asm_wipe(&s);
    size_t nonzero = 0;
    for(size_t i = 0; i < sizeof(store); i++)
        nonzero += store[i] != 0;
    TT_EQ_INT(nonzero, 0);
    TT_CHECK(!s.active);
    /* a bad CRC leaves nothing usable */
    TT_EQ_INT(blob_asm_begin(&s, 4), ProtoErrNone);
    c = (ProtoScriptChunk){.offset = 0, .len = 4, .data = (const uint8_t*)"abcd"};
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrNone);
    e.crc32 = 1;
    TT_EQ_INT(script_asm_end(&s, &e), ProtoErrBadPayload);
    TT_CHECK(!s.active);
}

static void test_decode_fuzz(void) {
    tt_seed(9);
    LinkRequest r;
    for(int i = 0; i < 50000; i++) {
        size_t n = tt_rand() % 64;
        for(size_t k = 0; k < n; k++)
            buf[k] = (uint8_t)tt_rand();
        uint8_t type = (uint8_t)(tt_rand() & 0x7F);
        uint16_t err = link_decode_request(type, buf, n, &r);
        TT_CHECK(err == ProtoErrNone || err == ProtoErrBadPayload || err == ProtoErrUnknownType);
    }
}

static void test_baud(void) {
    TT_EQ_INT(link_choose_baud(921600, 921600), 921600);
    TT_EQ_INT(link_choose_baud(500000, 921600), 460800);
    TT_EQ_INT(link_choose_baud(921600, 230400), 230400);
    TT_EQ_INT(link_choose_baud(9600, 921600), 115200);
}

static void test_script_asm(void) {
    static uint8_t store[PROTO_SCRIPT_MAX];
    ScriptAsm s;
    script_asm_init(&s, store, sizeof(store));
    const char* text = "#!/bin/bash\necho hi\nexit 3\n";
    uint32_t total = (uint32_t)strlen(text);
    ProtoScriptBegin b = {.total = total};
    strcpy(b.interp, "bash");
    TT_EQ_INT(script_asm_begin(&s, &b), ProtoErrNone);
    /* out-of-order chunk refused */
    ProtoScriptChunk c = {.offset = 5, .len = 3, .data = (const uint8_t*)text + 5};
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrBadPayload);
    c.offset = 0;
    c.len = 10;
    c.data = (const uint8_t*)text;
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrNone);
    c.offset = 10;
    c.len = (uint16_t)(total - 10);
    c.data = (const uint8_t*)text + 10;
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrNone);
    ProtoScriptEnd e = {.crc32 = proto_crc32((const uint8_t*)text, total, 0)};
    TT_EQ_INT(script_asm_end(&s, &e), ProtoErrNone);
    TT_CHECK(memcmp(store, text, total) == 0);

    /* bad CRC resets */
    script_asm_reset(&s);
    TT_EQ_INT(script_asm_begin(&s, &b), ProtoErrNone);
    c.offset = 0;
    c.len = (uint16_t)total;
    c.data = (const uint8_t*)text;
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrNone);
    e.crc32 ^= 1;
    TT_EQ_INT(script_asm_end(&s, &e), ProtoErrBadPayload);
    TT_CHECK(!s.active);

    /* overflow and size limits */
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrState);
    b.total = PROTO_SCRIPT_MAX + 1;
    TT_EQ_INT(script_asm_begin(&s, &b), ProtoErrTooLarge);
    b.total = 4;
    TT_EQ_INT(script_asm_begin(&s, &b), ProtoErrNone);
    c.len = 5;
    TT_EQ_INT(script_asm_chunk(&s, &c), ProtoErrTooLarge);
}

int main(void) {
    TT_RUN(test_decode_requests);
    TT_RUN(test_decode_fuzz);
    TT_RUN(test_baud);
    TT_RUN(test_script_asm);
    TT_RUN(test_key_blob);
    TT_END();
}
