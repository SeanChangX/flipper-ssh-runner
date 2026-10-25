/* Payload codec tests: round trips, truncation, trailing bytes, bounds.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "proto/proto_msg.h"
#include "tt.h"

/* Every strict prefix of a valid payload must be rejected, and so must one extra byte. */
#define CHECK_PREFIXES(dec_fn, type, buf, n)     \
    do {                                         \
        type tmp_;                               \
        for(size_t k_ = 0; k_ < (n); k_++)       \
            TT_CHECK(!dec_fn((buf), k_, &tmp_)); \
        uint8_t ext_[PROTO_MAX_PAYLOAD + 1];     \
        memcpy(ext_, (buf), (n));                \
        ext_[(n)] = 0xAA;                        \
        TT_CHECK(!dec_fn(ext_, (n) + 1, &tmp_)); \
    } while(0)

static uint8_t buf[PROTO_MAX_PAYLOAD];

static void test_hello(void) {
    ProtoHello a = {.proto_version = PROTO_VERSION, .max_baud = 921600};
    strcpy(a.version, "ssh_runner 0.1");
    size_t n = proto_enc_hello(&a, buf, sizeof(buf));
    TT_CHECK(n > 0);
    ProtoHello b;
    TT_CHECK(proto_dec_hello(buf, n, &b));
    TT_EQ_INT(b.max_baud, 921600);
    TT_EQ_STR(b.version, "ssh_runner 0.1");
    CHECK_PREFIXES(proto_dec_hello, ProtoHello, buf, n);

    ProtoHelloResp c = {.proto_version = 0, .max_baud = 921600, .baud = 460800};
    strcpy(c.version, "fw 0.1.0");
    n = proto_enc_hello_resp(&c, buf, sizeof(buf));
    ProtoHelloResp d;
    TT_CHECK(proto_dec_hello_resp(buf, n, &d));
    TT_EQ_INT(d.baud, 460800);
    TT_EQ_STR(d.version, "fw 0.1.0");
    CHECK_PREFIXES(proto_dec_hello_resp, ProtoHelloResp, buf, n);
}

static void test_ssh_connect(void) {
    ProtoSshConnect a = {
        .port = 2222,
        .transport = ProtoTransportLan,
        .auth = ProtoAuthKey,
        .policy = ProtoPolicyStrict,
        .flags = PROTO_CONN_REMEMBER};
    strcpy(a.profile, "nas");
    strcpy(a.user, "user");
    strcpy(a.host, "192.168.1.10");
    size_t n = proto_enc_ssh_connect(&a, buf, sizeof(buf));
    ProtoSshConnect kb;
    strcpy(a.key_id, "SHA256:abc");
    n = proto_enc_ssh_connect(&a, buf, sizeof(buf));
    TT_CHECK(proto_dec_ssh_connect(buf, n, &kb));
    TT_EQ_STR(kb.key_id, "SHA256:abc");
    a.key_id[0] = '\0';
    n = proto_enc_ssh_connect(&a, buf, sizeof(buf));
    TT_CHECK(n > 0);
    ProtoSshConnect b;
    TT_CHECK(proto_dec_ssh_connect(buf, n, &b));
    TT_EQ_STR(b.profile, "nas");
    TT_EQ_STR(b.user, "user");
    TT_EQ_STR(b.host, "192.168.1.10");
    TT_EQ_INT(b.port, 2222);
    TT_EQ_INT(b.transport, ProtoTransportLan);
    TT_EQ_INT(b.auth, ProtoAuthKey);
    TT_EQ_INT(b.policy, ProtoPolicyStrict);
    TT_EQ_INT(b.flags, PROTO_CONN_REMEMBER);
    TT_EQ_STR(b.key_id, "");
    CHECK_PREFIXES(proto_dec_ssh_connect, ProtoSshConnect, buf, n);

    /* Out-of-range enum and port 0 are rejected. */
    a.policy = 7;
    n = proto_enc_ssh_connect(&a, buf, sizeof(buf));
    TT_CHECK(!proto_dec_ssh_connect(buf, n, &b));
    a.policy = ProtoPolicyOff;
    a.port = 0;
    n = proto_enc_ssh_connect(&a, buf, sizeof(buf));
    TT_CHECK(!proto_dec_ssh_connect(buf, n, &b));
}

static void test_string_limits(void) {
    /* A string longer than the receiving field must be rejected, not truncated. */
    ProtoWriter w;
    pw_init(&w, buf, sizeof(buf));
    char longuser[60];
    memset(longuser, 'u', sizeof(longuser) - 1);
    longuser[sizeof(longuser) - 1] = 0;
    pw_str(&w, "p");
    pw_str(&w, longuser); /* > PROTO_STR_USER */
    pw_str(&w, "h");
    pw_u16(&w, 22);
    pw_u8(&w, 0);
    pw_u8(&w, 0);
    pw_u8(&w, 0);
    pw_u8(&w, 0);
    size_t n = pw_finish(&w);
    ProtoSshConnect b;
    TT_CHECK(!proto_dec_ssh_connect(buf, n, &b));

    /* Embedded NUL is rejected. */
    pw_init(&w, buf, sizeof(buf));
    pw_u8(&w, 3);
    pw_bytes(&w, "a\0b", 3);
    ProtoProfileRef pr;
    TT_CHECK(!proto_dec_profile_ref(buf, pw_finish(&w), &pr));

    /* Writer overflow is reported. */
    uint8_t small[4];
    pw_init(&w, small, sizeof(small));
    pw_str(&w, "too long");
    TT_EQ_INT(pw_finish(&w), 0);
}

static void test_hostkey_prompt(void) {
    ProtoHostkeyPrompt a = {.kind = ProtoHostkeyMismatch, .trusted_total = 5, .count = 4};
    strcpy(a.key_type, "ssh-ed25519");
    strcpy(a.fp, "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU");
    for(int i = 0; i < 4; i++) {
        snprintf(a.trusted[i].fp, sizeof(a.trusted[i].fp), "SHA256:%043d", i);
        snprintf(a.trusted[i].label, sizeof(a.trusted[i].label), "robot-%c", 'a' + i);
    }
    size_t n = proto_enc_hostkey_prompt(&a, buf, sizeof(buf));
    TT_CHECK(n > 0 && n <= PROTO_MAX_PAYLOAD);
    ProtoHostkeyPrompt b;
    TT_CHECK(proto_dec_hostkey_prompt(buf, n, &b));
    TT_EQ_INT(b.kind, ProtoHostkeyMismatch);
    TT_EQ_INT(b.trusted_total, 5);
    TT_EQ_INT(b.count, 4);
    TT_EQ_STR(b.trusted[3].label, "robot-d");
    TT_EQ_STR(b.fp, a.fp);
    CHECK_PREFIXES(proto_dec_hostkey_prompt, ProtoHostkeyPrompt, buf, n);

    a.count = PROTO_MAX_TRUSTED_FP + 1;
    TT_EQ_INT(proto_enc_hostkey_prompt(&a, buf, sizeof(buf)), 0);
}

static void test_scan_result_fits(void) {
    ProtoScanResult a = {.total = 20, .first = 8, .count = PROTO_MAX_SCAN_ENTRIES, .last = 0};
    for(int i = 0; i < PROTO_MAX_SCAN_ENTRIES; i++) {
        memset(a.entries[i].ssid, 'A' + i, PROTO_STR_SSID);
        a.entries[i].ssid[PROTO_STR_SSID] = 0;
        a.entries[i].rssi = (int8_t)(-40 - i);
        a.entries[i].auth = (uint8_t)(i & 1);
        a.entries[i].channel = (uint8_t)(1 + i);
    }
    size_t n = proto_enc_scan_result(&a, buf, sizeof(buf));
    TT_CHECK(n > 0 && n <= PROTO_MAX_PAYLOAD);
    ProtoScanResult b;
    TT_CHECK(proto_dec_scan_result(buf, n, &b));
    TT_EQ_INT(b.entries[7].rssi, -47);
    TT_EQ_INT(b.entries[7].channel, 8);
    TT_EQ_INT(b.first, 8);
    CHECK_PREFIXES(proto_dec_scan_result, ProtoScanResult, buf, n);
}

static void test_net(void) {
    ProtoNetCfg a = {
        .iface = ProtoIfaceWifi,
        .mode = ProtoIpStatic,
        .ip = 0xC0A80132,
        .mask = 0xFFFFFF00,
        .gw = 0xC0A80101,
        .dns = 0x01010101};
    size_t n = proto_enc_net_cfg(&a, buf, sizeof(buf));
    ProtoNetCfg b;
    TT_CHECK(proto_dec_net_cfg(buf, n, &b));
    TT_EQ_INT(b.ip, 0xC0A80132);
    TT_EQ_INT(b.dns, 0x01010101);
    CHECK_PREFIXES(proto_dec_net_cfg, ProtoNetCfg, buf, n);

    ProtoNetStatus s = {0};
    s.lan.flags = PROTO_NET_PRESENT | PROTO_NET_LINK | PROTO_NET_IP;
    s.lan.ip = 0xC0A80132;
    s.wifi.flags = PROTO_NET_PRESENT;
    s.wifi_rssi = -61;
    strcpy(s.wifi_ssid, "Lab-WiFi");
    n = proto_enc_net_status(&s, buf, sizeof(buf));
    ProtoNetStatus t;
    TT_CHECK(proto_dec_net_status(buf, n, &t));
    TT_EQ_INT(t.lan.flags, 7);
    TT_EQ_INT(t.wifi_rssi, -61);
    TT_EQ_STR(t.wifi_ssid, "Lab-WiFi");
    CHECK_PREFIXES(proto_dec_net_status, ProtoNetStatus, buf, n);
}

static void test_exec_stream_done(void) {
    const char* cmd = "sudo systemctl restart nginx";
    ProtoExecReq a = {.len = (uint16_t)strlen(cmd), .command = cmd};
    size_t n = proto_enc_exec_req(&a, buf, sizeof(buf));
    TT_EQ_INT(n, strlen(cmd));
    ProtoExecReq b;
    TT_CHECK(proto_dec_exec_req(buf, n, &b));
    TT_EQ_INT(b.len, strlen(cmd));
    TT_CHECK(memcmp(b.command, cmd, b.len) == 0);
    TT_CHECK(!proto_dec_exec_req(buf, 0, &b)); /* empty command */

    uint8_t data[PROTO_STREAM_CHUNK];
    for(size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)i;
    ProtoStreamData s = {.stream = ProtoStreamStderr, .len = sizeof(data), .data = data};
    n = proto_enc_stream_data(&s, buf, sizeof(buf));
    TT_EQ_INT(n, PROTO_MAX_PAYLOAD);
    ProtoStreamData t;
    TT_CHECK(proto_dec_stream_data(buf, n, &t));
    TT_EQ_INT(t.stream, ProtoStreamStderr);
    TT_EQ_INT(t.len, sizeof(data));
    TT_CHECK(memcmp(t.data, data, sizeof(data)) == 0);
    s.len = PROTO_STREAM_CHUNK + 1;
    TT_EQ_INT(proto_enc_stream_data(&s, buf, sizeof(buf)), 0);

    ProtoExecDone d = {.exit_code = -3, .flags = PROTO_DONE_SIGNAL, .duration_ms = 123456};
    n = proto_enc_exec_done(&d, buf, sizeof(buf));
    ProtoExecDone e;
    TT_CHECK(proto_dec_exec_done(buf, n, &e));
    TT_EQ_INT(e.exit_code, -3);
    TT_EQ_INT(e.duration_ms, 123456);
    CHECK_PREFIXES(proto_dec_exec_done, ProtoExecDone, buf, n);
}

static void test_script(void) {
    ProtoScriptBegin a = {.total = 20 * 1024};
    strcpy(a.interp, "bash");
    size_t n = proto_enc_script_begin(&a, buf, sizeof(buf));
    ProtoScriptBegin b;
    TT_CHECK(proto_dec_script_begin(buf, n, &b));
    TT_EQ_STR(b.interp, "bash");
    a.total = PROTO_SCRIPT_MAX + 1;
    n = proto_enc_script_begin(&a, buf, sizeof(buf));
    TT_CHECK(!proto_dec_script_begin(buf, n, &b));

    uint8_t data[PROTO_SCRIPT_CHUNK];
    memset(data, 'x', sizeof(data));
    ProtoScriptChunk c = {.offset = 4096, .len = sizeof(data), .data = data};
    n = proto_enc_script_chunk(&c, buf, sizeof(buf));
    TT_EQ_INT(n, PROTO_MAX_PAYLOAD);
    ProtoScriptChunk d;
    TT_CHECK(proto_dec_script_chunk(buf, n, &d));
    TT_EQ_INT(d.offset, 4096);
    TT_EQ_INT(d.len, sizeof(data));

    TT_EQ_INT(proto_crc32((const uint8_t*)"123456789", 9, 0), 0xCBF43926u);
    /* Incremental CRC equals one-shot CRC. */
    uint32_t inc = proto_crc32((const uint8_t*)"12345", 5, 0);
    inc = proto_crc32((const uint8_t*)"6789", 4, inc);
    TT_EQ_INT(inc, 0xCBF43926u);
}

static void test_misc_codecs(void) {
    ProtoHostkeyDecision a = {.decision = ProtoDecisionAdd};
    strcpy(a.label, "robot-b");
    size_t n = proto_enc_hostkey_decision(&a, buf, sizeof(buf));
    ProtoHostkeyDecision b;
    TT_CHECK(proto_dec_hostkey_decision(buf, n, &b));
    TT_EQ_INT(b.decision, ProtoDecisionAdd);
    a.decision = 9;
    n = proto_enc_hostkey_decision(&a, buf, sizeof(buf));
    TT_CHECK(!proto_dec_hostkey_decision(buf, n, &b));

    ProtoAuthPasswordMsg p = {.remember = 1};
    strcpy(p.password, "hunter2");
    n = proto_enc_auth_password(&p, buf, sizeof(buf));
    ProtoAuthPasswordMsg q;
    TT_CHECK(proto_dec_auth_password(buf, n, &q));
    TT_EQ_STR(q.password, "hunter2");
    TT_EQ_INT(q.remember, 1);

    ProtoSshStateEvt pw = {.state = ProtoSshPassphraseWait, .reason = ProtoReasonBadPassphrase};
    n = proto_enc_ssh_state(&pw, buf, sizeof(buf));
    ProtoSshStateEvt px;
    TT_CHECK(proto_dec_ssh_state(buf, n, &px));
    TT_EQ_INT(px.state, ProtoSshPassphraseWait);
    TT_EQ_INT(px.reason, ProtoReasonBadPassphrase);
}

/* Keys: key file staging, passphrase answer, public key pieces, forget, legacy delete. */
static void test_keys(void) {
    ProtoKeyBlobBegin bb = {.total = PROTO_KEY_MAX}, bc;
    size_t n = proto_enc_key_blob_begin(&bb, buf, sizeof(buf));
    TT_CHECK(proto_dec_key_blob_begin(buf, n, &bc));
    TT_EQ_INT(bc.total, PROTO_KEY_MAX);
    CHECK_PREFIXES(proto_dec_key_blob_begin, ProtoKeyBlobBegin, buf, n);
    bb.total = PROTO_KEY_MAX + 1; /* too big for the ESP32's staging buffer */
    n = proto_enc_key_blob_begin(&bb, buf, sizeof(buf));
    TT_CHECK(!proto_dec_key_blob_begin(buf, n, &bc));
    bb.total = 0;
    n = proto_enc_key_blob_begin(&bb, buf, sizeof(buf));
    TT_CHECK(!proto_dec_key_blob_begin(buf, n, &bc));

    ProtoKeyGen g = {.key_type = ProtoKeyGenEcdsaP256}, h;
    strcpy(g.passphrase, "correct horse");
    n = proto_enc_key_gen(&g, buf, sizeof(buf));
    TT_CHECK(proto_dec_key_gen(buf, n, &h));
    TT_EQ_INT(h.key_type, ProtoKeyGenEcdsaP256);
    TT_EQ_STR(h.passphrase, "correct horse");
    CHECK_PREFIXES(proto_dec_key_gen, ProtoKeyGen, buf, n);
    g.key_type = 2; /* no RSA generation */
    n = proto_enc_key_gen(&g, buf, sizeof(buf));
    TT_CHECK(!proto_dec_key_gen(buf, n, &h));

    ProtoKeyPassphrase kp = {.remember = 1}, kq;
    strcpy(kp.passphrase, "s3cret");
    n = proto_enc_key_passphrase(&kp, buf, sizeof(buf));
    TT_CHECK(proto_dec_key_passphrase(buf, n, &kq));
    TT_EQ_STR(kq.passphrase, "s3cret");
    TT_EQ_INT(kq.remember, 1);
    CHECK_PREFIXES(proto_dec_key_passphrase, ProtoKeyPassphrase, buf, n);

    /* a public key line in two pieces */
    static const char line[] =
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl ssh_runner";
    ProtoKeyPub
        k = {.total = sizeof(line) - 1, .offset = 0, .len = 20, .data = (const uint8_t*)line},
        l;
    strcpy(k.fp, "SHA256:abc");
    n = proto_enc_key_pub(&k, buf, sizeof(buf));
    TT_CHECK(proto_dec_key_pub(buf, n, &l));
    TT_EQ_STR(l.fp, "SHA256:abc");
    TT_EQ_INT(l.total, sizeof(line) - 1);
    TT_EQ_INT(l.len, 20);
    TT_CHECK(memcmp(l.data, line, 20) == 0);
    k.offset = 20;
    k.len = (uint16_t)(sizeof(line) - 1 - 20);
    k.data = (const uint8_t*)line + 20;
    n = proto_enc_key_pub(&k, buf, sizeof(buf));
    TT_CHECK(proto_dec_key_pub(buf, n, &l));
    TT_EQ_INT(l.offset + l.len, l.total);
    uint8_t ext[PROTO_MAX_PAYLOAD];
    memcpy(ext, buf, n);
    ext[n] = 'x'; /* runs past total */
    TT_CHECK(!proto_dec_key_pub(ext, n + 1, &l));
    static uint8_t big[PROTO_PUB_MAX + 1];
    k.data = big;
    k.len = PROTO_PUB_CHUNK + 1; /* never more than one frame */
    k.total = PROTO_PUB_MAX;
    k.offset = 0;
    TT_EQ_INT(proto_enc_key_pub(&k, buf, sizeof(buf)), 0);
    k.len = PROTO_PUB_CHUNK;
    TT_CHECK(proto_enc_key_pub(&k, buf, sizeof(buf)) > 0);
    k.total = PROTO_PUB_MAX + 1;
    TT_EQ_INT(proto_enc_key_pub(&k, buf, sizeof(buf)), 0);

    ProtoSecretForget f = {.kind = ProtoForgetPassphrase}, e;
    strcpy(f.id, "SHA256:Zm9vYmFyYmF6cXV4cXV1eGZvb2JhcmJhenF1eHF1dXhmb28");
    n = proto_enc_secret_forget(&f, buf, sizeof(buf));
    TT_CHECK(proto_dec_secret_forget(buf, n, &e));
    TT_EQ_INT(e.kind, ProtoForgetPassphrase);
    TT_EQ_STR(e.id, f.id);
    CHECK_PREFIXES(proto_dec_secret_forget, ProtoSecretForget, buf, n);
    f.kind = ProtoForgetUnlocked; /* lock: RAM only */
    n = proto_enc_secret_forget(&f, buf, sizeof(buf));
    TT_CHECK(proto_dec_secret_forget(buf, n, &e));
    f.kind = 3;
    n = proto_enc_secret_forget(&f, buf, sizeof(buf));
    TT_CHECK(!proto_dec_secret_forget(buf, n, &e));
    f.kind = ProtoForgetPassphrase;
    f.id[0] = '\0';
    n = proto_enc_secret_forget(&f, buf, sizeof(buf));
    TT_CHECK(!proto_dec_secret_forget(buf, n, &e));

    ProtoKeyLegacyDelete d = {.crc32 = 0xCBF43926u}, d2;
    n = proto_enc_key_legacy_delete(&d, buf, sizeof(buf));
    TT_CHECK(proto_dec_key_legacy_delete(buf, n, &d2));
    TT_EQ_INT(d2.crc32, 0xCBF43926u);
    CHECK_PREFIXES(proto_dec_key_legacy_delete, ProtoKeyLegacyDelete, buf, n);

    TT_EQ_STR(proto_type_name(PROTO_T_KEY_BLOB_CHUNK), "KEY_BLOB_CHUNK");
    TT_EQ_STR(proto_type_name(0x41), "?"); /* KEY_GET_PUB is gone */
}

static void test_misc_codecs_rest(void) {
    size_t n;

    ProtoSshStateEvt s = {.state = ProtoSshFailed, .reason = ProtoReasonAuthFailed, .iface = 1};
    strcpy(s.msg, "Permission denied");
    n = proto_enc_ssh_state(&s, buf, sizeof(buf));
    ProtoSshStateEvt t;
    TT_CHECK(proto_dec_ssh_state(buf, n, &t));
    TT_EQ_INT(t.reason, ProtoReasonAuthFailed);
    TT_EQ_STR(t.msg, "Permission denied");

    ProtoHostkeyList hl = {.policy_log_count = 2, .count = 2};
    strcpy(hl.keys[0].fp, "SHA256:aaa");
    strcpy(hl.keys[1].fp, "SHA256:bbb");
    strcpy(hl.keys[1].label, "b");
    n = proto_enc_hostkey_list(&hl, buf, sizeof(buf));
    ProtoHostkeyList hm;
    TT_CHECK(proto_dec_hostkey_list(buf, n, &hm));
    TT_EQ_STR(hm.keys[1].label, "b");
    CHECK_PREFIXES(proto_dec_hostkey_list, ProtoHostkeyList, buf, n);

    ProtoWifiJoinResult jr = {.result = ProtoJoinWrongPassword, .reason = 202}, jd;
    n = proto_enc_wifi_join_result(&jr, buf, sizeof(buf));
    TT_CHECK(n == 2 && proto_dec_wifi_join_result(buf, n, &jd));
    TT_CHECK(jd.result == ProtoJoinWrongPassword && jd.reason == 202);
    buf[0] = 9; /* no such result */
    TT_CHECK(!proto_dec_wifi_join_result(buf, n, &jd));
    ProtoPingResp pr = {
        .sent = 4, .received = 3, .min_ms = 1, .avg_ms = 2, .max_ms = 9, .iface = 0};
    n = proto_enc_ping_resp(&pr, buf, sizeof(buf));
    ProtoPingResp ps;
    TT_CHECK(proto_dec_ping_resp(buf, n, &ps));
    TT_EQ_INT(ps.max_ms, 9);

    TT_EQ_STR(proto_type_name(PROTO_T_ACK), "ACK");
    TT_EQ_STR(proto_type_name(0x7E), "?");
}

/* Security: the typed word (shared by both firmwares) and the SEC_* codecs. */
static void test_sec(void) {
    /* accepted: trimmed, any case */
    TT_CHECK(proto_sec_word_ok("confirm"));
    TT_CHECK(proto_sec_word_ok("CONFIRM"));
    TT_CHECK(proto_sec_word_ok("Confirm"));
    TT_CHECK(proto_sec_word_ok("  confirm\t"));
    TT_CHECK(proto_sec_word_ok("\nconfirm\r\n"));
    /* rejected */
    TT_CHECK(!proto_sec_word_ok("confirm."));
    TT_CHECK(!proto_sec_word_ok("con firm"));
    TT_CHECK(!proto_sec_word_ok("confirmx"));
    TT_CHECK(!proto_sec_word_ok("conf"));
    TT_CHECK(!proto_sec_word_ok(""));
    TT_CHECK(!proto_sec_word_ok("   \t "));
    TT_CHECK(!proto_sec_word_ok(NULL));
    TT_CHECK(!proto_sec_word_ok("yes"));
    TT_CHECK(!proto_sec_word_ok("c\xc3\xb6nfirm")); /* not ASCII-folded */

    uint8_t buf[PROTO_MAX_PAYLOAD];
    ProtoSecStatus st = {
        .tier = ProtoSecTierProtect, .key_block = 2, .free_blocks = 5, .efuse_virtual = 1};
    size_t n = proto_enc_sec_status(&st, buf, sizeof(buf));
    ProtoSecStatus st2;
    TT_CHECK(proto_dec_sec_status(buf, n, &st2));
    TT_EQ_INT(st2.tier, ProtoSecTierProtect);
    TT_EQ_INT(st2.key_block, 2);
    TT_EQ_INT(st2.free_blocks, 5);
    TT_EQ_INT(st2.efuse_virtual, 1);
    buf[0] = 3; /* no such tier */
    TT_CHECK(!proto_dec_sec_status(buf, n, &st2));
    CHECK_PREFIXES(proto_dec_sec_status, ProtoSecStatus, buf, n);

    /* any word decodes: the ESP32 itself answers ERROR on a wrong one */
    ProtoSecEnable en = {.tier = ProtoSecTierProtect, .word = " Confirm "};
    n = proto_enc_sec_enable(&en, buf, sizeof(buf));
    ProtoSecEnable en2;
    TT_CHECK(proto_dec_sec_enable(buf, n, &en2));
    TT_EQ_STR(en2.word, " Confirm ");
    TT_CHECK(proto_sec_word_ok(en2.word));
    ProtoSecEnable empty = {.tier = ProtoSecTierProtect};
    n = proto_enc_sec_enable(&empty, buf, sizeof(buf));
    TT_CHECK(proto_dec_sec_enable(buf, n, &en2));
    TT_CHECK(!proto_sec_word_ok(en2.word));

    ProtoSecState ss = {
        .phase = ProtoSecPhaseMigrate, .result = ProtoSecRunning, .migrated = 7, .msg = "copying"};
    n = proto_enc_sec_state(&ss, buf, sizeof(buf));
    ProtoSecState ss2;
    TT_CHECK(proto_dec_sec_state(buf, n, &ss2));
    TT_EQ_INT(ss2.phase, ProtoSecPhaseMigrate);
    TT_EQ_INT(ss2.migrated, 7);
    TT_EQ_STR(ss2.msg, "copying");
    buf[0] = 9; /* no such phase */
    TT_CHECK(!proto_dec_sec_state(buf, n, &ss2));

    TT_EQ_STR(proto_type_name(PROTO_T_SEC_ENABLE), "SEC_ENABLE");
    TT_EQ_INT(PROTO_T_SEC_STATUS, 0xE0);
}

/* Random bytes must never crash any decoder. */
static void test_decoder_fuzz(void) {
    tt_seed(5);
    uint8_t in[PROTO_MAX_PAYLOAD];
    for(int iter = 0; iter < 20000; iter++) {
        size_t n = tt_rand() % 80;
        for(size_t i = 0; i < n; i++)
            in[i] = (uint8_t)(tt_rand() % 6 == 0 ? (tt_rand() % 8) : tt_rand());
        union {
            ProtoHello a;
            ProtoHelloResp b;
            ProtoSshConnect c;
            ProtoHostkeyPrompt d;
            ProtoScanResult e;
            ProtoNetStatus f;
            ProtoKeyPub g;
            ProtoHostkeyList h;
            ProtoStreamData i;
            ProtoExecReq j;
            ProtoKeyGen k;
            ProtoKeyBlobBegin l;
            ProtoKeyPassphrase m;
            ProtoSecretForget o;
            ProtoSecStatus p;
            ProtoSecEnable q;
            ProtoSecState s;
        } u;
        (void)proto_dec_hello(in, n, &u.a);
        (void)proto_dec_hello_resp(in, n, &u.b);
        (void)proto_dec_ssh_connect(in, n, &u.c);
        (void)proto_dec_hostkey_prompt(in, n, &u.d);
        (void)proto_dec_scan_result(in, n, &u.e);
        (void)proto_dec_net_status(in, n, &u.f);
        (void)proto_dec_key_pub(in, n, &u.g);
        (void)proto_dec_hostkey_list(in, n, &u.h);
        (void)proto_dec_stream_data(in, n, &u.i);
        (void)proto_dec_exec_req(in, n, &u.j);
        (void)proto_dec_key_gen(in, n, &u.k);
        (void)proto_dec_key_blob_begin(in, n, &u.l);
        (void)proto_dec_key_passphrase(in, n, &u.m);
        (void)proto_dec_secret_forget(in, n, &u.o);
        (void)proto_dec_sec_status(in, n, &u.p);
        (void)proto_dec_sec_enable(in, n, &u.q);
        (void)proto_dec_sec_state(in, n, &u.s);
    }
    TT_CHECK(1);
}

int main(void) {
    TT_RUN(test_hello);
    TT_RUN(test_ssh_connect);
    TT_RUN(test_string_limits);
    TT_RUN(test_hostkey_prompt);
    TT_RUN(test_scan_result_fits);
    TT_RUN(test_net);
    TT_RUN(test_exec_stream_done);
    TT_RUN(test_script);
    TT_RUN(test_misc_codecs);
    TT_RUN(test_misc_codecs_rest);
    TT_RUN(test_keys);
    TT_RUN(test_sec);
    TT_RUN(test_decoder_fuzz);
    TT_END();
}
