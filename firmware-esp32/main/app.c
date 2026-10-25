/*
 * Flipper link front-end: maps requests (PROTOCOL.md §5) onto net / ssh / keystore
 * and turns their events into link events.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "keystore.h"
#include "link.h"
#include "net.h"

static const char* TAG = "app";

const char* app_fw_version(void) {
    static char v[40];
    if(!v[0]) snprintf(v, sizeof(v), "fes %s", esp_app_get_description()->version);
    return v;
}

#define EVT_TIMEOUT_MS 1000

static ScriptAsm script;
static uint8_t* script_buf; /* 64 KiB, PSRAM */

/* ---------------- staged key file ---------------- */

#define KEY_STAGE_TIMEOUT_MS (30 * 1000)

/* Static, so internal RAM (never PSRAM), and the same place every time: the debug
 * console can check it reads back as zeros. +1 for the NUL libssh needs. */
static uint8_t key_buf[PROTO_KEY_MAX + 1];
static ScriptAsm key_asm;
static bool key_ready; /* KEY_BLOB_END verified */
static bool key_lent; /* the SSH client reads it until key_release */
static SemaphoreHandle_t key_mu;
static TimerHandle_t key_timer;

void app_key_wipe(void) {
    xSemaphoreTake(key_mu, portMAX_DELAY);
    script_asm_wipe(&key_asm);
    key_buf[PROTO_KEY_MAX] = 0;
    key_ready = false;
    key_lent = false;
    xTimerStop(key_timer, 0);
    xSemaphoreGive(key_mu);
}

/* Work for the status worker task, as notification bits (the timer service task's 2 KB
 * stack must not log, lock or wipe: it overflows doing so). */
#define NOTE_NET_STATUS  (1u << 0)
#define NOTE_KEY_TIMEOUT (1u << 1)
static TaskHandle_t status_task;

static void key_timeout(TimerHandle_t t) {
    (void)t;
    if(status_task) xTaskNotify(status_task, NOTE_KEY_TIMEOUT, eSetBits);
}

/* On the worker: wipe the staged key unless the SSH client holds it, or a new key file
 * restarted the timer after it fired. */
static void key_timeout_work(void) {
    if(xTimerIsTimerActive(key_timer)) return;
    xSemaphoreTake(key_mu, portMAX_DELAY);
    bool lent = key_lent;
    xSemaphoreGive(key_mu);
    if(!lent) {
        ESP_LOGW(TAG, "staged key not used within 30 s: wiped");
        app_key_wipe();
    }
}

uint16_t app_key_begin(uint32_t total) {
    xSemaphoreTake(key_mu, portMAX_DELAY);
    bool lent = key_lent;
    xSemaphoreGive(key_mu);
    if(lent) return ProtoErrBusy;
    app_key_wipe();
    xSemaphoreTake(key_mu, portMAX_DELAY);
    uint16_t e = blob_asm_begin(&key_asm, total);
    xSemaphoreGive(key_mu);
    return e;
}

uint16_t app_key_chunk(const ProtoScriptChunk* chunk) {
    xSemaphoreTake(key_mu, portMAX_DELAY);
    uint16_t e = key_lent ? ProtoErrBusy : script_asm_chunk(&key_asm, chunk);
    xSemaphoreGive(key_mu);
    if(e && e != ProtoErrBusy) app_key_wipe();
    return e;
}

uint16_t app_key_end(const ProtoScriptEnd* end) {
    xSemaphoreTake(key_mu, portMAX_DELAY);
    uint16_t e = key_lent ? ProtoErrBusy : script_asm_end(&key_asm, end);
    if(!e) {
        key_ready = true;
        xTimerReset(key_timer, 0);
    }
    xSemaphoreGive(key_mu);
    if(e && e != ProtoErrBusy) app_key_wipe();
    return e;
}

size_t app_key_nonzero(void) {
    xSemaphoreTake(key_mu, portMAX_DELAY);
    size_t n = 0;
    for(size_t i = 0; i < sizeof(key_buf); i++)
        n += key_buf[i] != 0;
    xSemaphoreGive(key_mu);
    return n;
}

/* A staged key that no session took is wiped when its app goes away or cancels. */
static void wipe_unused_key(void) {
    xSemaphoreTake(key_mu, portMAX_DELAY);
    bool lent = key_lent;
    xSemaphoreGive(key_mu);
    if(!lent) app_key_wipe();
}

static void key_release(void* ctx) {
    (void)ctx;
    app_key_wipe();
}

esp_err_t app_ssh_connect(const ProtoSshConnect* req, const SshSink* sink, uint32_t credit) {
    const char* key = NULL;
    size_t len = 0;
    if(req->auth == ProtoAuthKey) {
        xSemaphoreTake(key_mu, portMAX_DELAY);
        if(key_ready && !key_lent) {
            key_lent = true;
            xTimerStop(key_timer, 0);
            len = key_asm.total;
            key_buf[len] = 0;
            key = (const char*)key_buf;
        }
        xSemaphoreGive(key_mu);
    } else {
        app_key_wipe(); /* a key staged for nothing */
    }
    SshSink s = *sink;
    s.key_release = key_release;
    esp_err_t err = ssh_client_connect(req, &s, credit, key, len);
    if(err != ESP_OK && key) app_key_wipe();
    return err;
}

/* ---------------- events to the Flipper ---------------- */

static void send_evt(uint8_t type, const uint8_t* buf, size_t n) {
    if(n == 0 && type != PROTO_T_NET_STATUS_EVT) {
        ESP_LOGE(TAG, "encode failed for %s", proto_type_name(type));
        return;
    }
    if(!link_send_event(type, buf, n, EVT_TIMEOUT_MS))
        ESP_LOGW(TAG, "%s not delivered", proto_type_name(type));
}

static void link_state(void* ctx, const ProtoSshStateEvt* st) {
    (void)ctx;
    uint8_t buf[96];
    send_evt(PROTO_T_SSH_STATE_EVT, buf, proto_enc_ssh_state(st, buf, sizeof(buf)));
}

static void link_prompt(void* ctx, const ProtoHostkeyPrompt* p) {
    (void)ctx;
    uint8_t buf[PROTO_MAX_PAYLOAD];
    send_evt(PROTO_T_HOSTKEY_PROMPT_EVT, buf, proto_enc_hostkey_prompt(p, buf, sizeof(buf)));
}

static void link_stream(void* ctx, uint8_t stream, const uint8_t* data, size_t len) {
    (void)ctx;
    uint8_t buf[PROTO_MAX_PAYLOAD];
    while(len) {
        size_t k = len > PROTO_STREAM_CHUNK ? PROTO_STREAM_CHUNK : len;
        ProtoStreamData d = {.stream = stream, .len = (uint16_t)k, .data = data};
        send_evt(PROTO_T_STREAM_DATA_EVT, buf, proto_enc_stream_data(&d, buf, sizeof(buf)));
        data += k;
        len -= k;
    }
}

static void link_done(void* ctx, const ProtoExecDone* d) {
    (void)ctx;
    script_asm_reset(&script);
    uint8_t buf[16];
    send_evt(PROTO_T_EXEC_DONE_EVT, buf, proto_enc_exec_done(d, buf, sizeof(buf)));
}

/* E->F key file: KEY_BLOB_BEGIN, CHUNKs, END, each one an acknowledged event. */
static bool link_key_blob(void* ctx, const uint8_t* data, size_t len) {
    (void)ctx;
    uint8_t buf[PROTO_MAX_PAYLOAD];
    ProtoKeyBlobBegin b = {.total = (uint32_t)len};
    size_t n = proto_enc_key_blob_begin(&b, buf, sizeof(buf));
    if(!n || !link_send_event(PROTO_T_KEY_BLOB_BEGIN, buf, n, EVT_TIMEOUT_MS)) return false;
    bool ok = true;
    for(size_t off = 0; ok && off < len; off += PROTO_SCRIPT_CHUNK) {
        size_t k = len - off > PROTO_SCRIPT_CHUNK ? PROTO_SCRIPT_CHUNK : len - off;
        ProtoScriptChunk ch = {.offset = (uint32_t)off, .len = (uint16_t)k, .data = data + off};
        n = proto_enc_script_chunk(&ch, buf, sizeof(buf));
        ok = n && link_send_event(PROTO_T_KEY_BLOB_CHUNK, buf, n, EVT_TIMEOUT_MS);
    }
    ProtoScriptEnd e = {.crc32 = proto_crc32(data, len, 0)};
    n = proto_enc_script_end(&e, buf, sizeof(buf));
    ok = ok && n && link_send_event(PROTO_T_KEY_BLOB_END, buf, n, EVT_TIMEOUT_MS);
    memset(buf, 0, sizeof(buf));
    return ok;
}

static void link_key_pub(void* ctx, uint8_t status, const char* fp, const char* line) {
    (void)ctx;
    uint8_t buf[PROTO_MAX_PAYLOAD];
    size_t total = strlen(line);
    if(total > PROTO_PUB_MAX) total = PROTO_PUB_MAX;
    size_t off = 0;
    do {
        size_t k = total - off > PROTO_PUB_CHUNK ? PROTO_PUB_CHUNK : total - off;
        ProtoKeyPub p = {
            .status = status,
            .total = (uint16_t)total,
            .offset = (uint16_t)off,
            .len = (uint16_t)k,
            .data = (const uint8_t*)line + off};
        snprintf(p.fp, sizeof(p.fp), "%s", fp);
        send_evt(PROTO_T_KEY_PUB_EVT, buf, proto_enc_key_pub(&p, buf, sizeof(buf)));
        off += k;
    } while(off < total);
}

static const SshSink link_sink = {
    .state = link_state,
    .hostkey_prompt = link_prompt,
    .stream = link_stream,
    .done = link_done,
    .key_blob = link_key_blob,
    .key_pub = link_key_pub};

/* Status changes can be reported from the link task itself (e.g. NET_SET_CFG), which
 * must not wait for its own ACK; a worker task sends them instead. It also does the staged
 * key's timeout work (NOTE_KEY_TIMEOUT). */
static void status_worker(void* arg) {
    (void)arg;
    while(true) {
        uint32_t note = 0;
        xTaskNotifyWait(0, UINT32_MAX, &note, portMAX_DELAY);
        if(note & NOTE_KEY_TIMEOUT) key_timeout_work();
        if(!(note & NOTE_NET_STATUS) || !link_peer_present()) continue;
        ProtoWifiJoinResult jr;
        if(net_take_join_result(&jr)) {
            uint8_t jb[8];
            send_evt(
                PROTO_T_WIFI_JOIN_RESULT, jb, proto_enc_wifi_join_result(&jr, jb, sizeof(jb)));
        }
        ProtoNetStatus st;
        net_get_status(&st);
        uint8_t buf[64];
        send_evt(PROTO_T_NET_STATUS_EVT, buf, proto_enc_net_status(&st, buf, sizeof(buf)));
    }
}

void app_net_status_changed(void* ctx) {
    (void)ctx;
    if(status_task) xTaskNotify(status_task, NOTE_NET_STATUS, eSetBits);
}

static void scan_done(const ProtoScanResult* r, void* ctx) {
    (void)ctx;
    uint8_t buf[PROTO_MAX_PAYLOAD];
    send_evt(PROTO_T_WIFI_SCAN_RESULT, buf, proto_enc_scan_result(r, buf, sizeof(buf)));
}

static void ping_done(const ProtoPingResp* r, void* ctx) {
    (void)ctx;
    uint8_t buf[16];
    send_evt(PROTO_T_PING_RESP, buf, proto_enc_ping_resp(r, buf, sizeof(buf)));
}

/* ---------------- "Protect secrets" ---------------- */

static volatile bool sec_running; /* while it runs, nothing else may touch the keystore */
static char sec_word[PROTO_STR_SEC_WORD + 1];

static void
    sec_progress(uint8_t phase, uint8_t result, uint8_t migrated, const char* msg, void* ctx) {
    (void)ctx;
    ProtoSecState st = {.phase = phase, .result = result, .migrated = migrated};
    strncpy(st.msg, msg ? msg : "", sizeof(st.msg) - 1);
    uint8_t buf[PROTO_MAX_PAYLOAD];
    send_evt(PROTO_T_SEC_STATE_EVT, buf, proto_enc_sec_state(&st, buf, sizeof(buf)));
}

static void sec_worker(void* arg) {
    (void)arg;
    esp_err_t err = keystore_sec_enable(sec_word, sec_progress, NULL);
    memset(sec_word, 0, sizeof(sec_word));
    if(err == ESP_OK) {
        ProtoSecStatus s;
        keystore_sec_status(&s);
        if(s.tier == ProtoSecTierNone) {
            /* the plaintext copy is erased at boot, before WiFi opens NVS */
            vTaskDelay(pdMS_TO_TICKS(500)); /* let the last event leave */
            esp_restart();
        }
    }
    sec_running = false;
    vTaskDelete(NULL);
}

/* Requests that may run while the migration does: none of them touches the keystore. */
static bool sec_allowed(uint8_t type) {
    return type == PROTO_T_HELLO || type == PROTO_T_FLOW_CREDIT ||
           type == PROTO_T_NET_GET_STATUS || type == PROTO_T_SEC_GET_STATUS;
}

/* ---------------- requests from the Flipper ---------------- */

static void reply_err(esp_err_t err, const char* what) {
    if(err == ESP_OK) {
        link_reply_ack();
        return;
    }
    uint16_t code = err == ESP_ERR_INVALID_STATE ? ProtoErrState :
                    err == ESP_ERR_NOT_FOUND     ? ProtoErrNotFound :
                    err == ESP_ERR_NO_MEM        ? ProtoErrBusy :
                                                   ProtoErrInternal;
    link_reply_error(code, what);
}

/* The app that owned a session is gone: close it, or the next app's SSH_CONNECT is "busy". */
static void on_peer_lost(void) {
    uint8_t st = ssh_client_state();
    if(st != ProtoSshIdle && st != ProtoSshClosed && st != ProtoSshFailed) {
        ESP_LOGW(TAG, "Flipper gone: closing its SSH session");
        ssh_client_disconnect();
    }
    wipe_unused_key();
}

static void on_request(const LinkRequest* r, void* ctx) {
    (void)ctx;
    uint8_t buf[PROTO_MAX_PAYLOAD];
    if(sec_running && !sec_allowed(r->type)) {
        link_reply_error(ProtoErrBusy, "protecting secrets");
        return;
    }
    switch(r->type) {
    case PROTO_T_HELLO:
        ESP_LOGI(TAG, "HELLO from %s (proto v%u)", r->hello.version, r->hello.proto_version);
        link_reply_hello(&r->hello, app_fw_version());
        {
            /* A HELLO means a new app instance (or one that already gave the link up): a
             * session left over from before has no owner and would make the next
             * SSH_CONNECT fail with "busy" (found on hardware after an app exit). */
            uint8_t st = ssh_client_state();
            if(st != ProtoSshIdle && st != ProtoSshClosed && st != ProtoSshFailed) {
                ESP_LOGW(TAG, "closing the SSH session of the previous app instance");
                ssh_client_disconnect();
            }
            wipe_unused_key(); /* a key the previous app staged */
        }
        break;
    case PROTO_T_FLOW_CREDIT:
        ssh_client_credit(r->credit.bytes);
        link_reply_ack();
        break;
    case PROTO_T_CANCEL:
        ssh_client_cancel();
        net_ping_cancel();
        script_asm_reset(&script);
        wipe_unused_key();
        link_reply_ack();
        break;
    case PROTO_T_NET_SET_CFG:
        reply_err(net_set_cfg(&r->net_cfg), "cannot apply network settings");
        break;
    case PROTO_T_NET_GET_STATUS: {
        ProtoNetStatus st;
        net_get_status(&st);
        link_reply(PROTO_T_NET_STATUS, buf, proto_enc_net_status(&st, buf, sizeof(buf)));
        break;
    }
    case PROTO_T_WIFI_SCAN:
        reply_err(net_wifi_scan(scan_done, NULL), "scan busy");
        break;
    case PROTO_T_WIFI_JOIN:
        reply_err(net_wifi_join(r->join.ssid, r->join.password), "join busy");
        break;
    case PROTO_T_PING_REQ:
        reply_err(net_ping(&r->ping, ping_done, NULL), "ping not possible");
        break;
    case PROTO_T_SSH_CONNECT:
        /* the Flipper grants credit itself with FLOW_CREDIT */
        reply_err(app_ssh_connect(&r->conn, &link_sink, 0), "session busy");
        break;
    case PROTO_T_HOSTKEY_DECISION:
        reply_err(ssh_client_hostkey_decision(&r->decision), "no prompt pending");
        break;
    case PROTO_T_HOSTKEY_LIST: {
        KnownHosts* kh = calloc(1, sizeof(KnownHosts));
        ProtoHostkeyList l = {0};
        if(kh) {
            keystore_kh_load(r->ref.profile, kh);
            l.count = kh->count < PROTO_MAX_HOSTKEYS ? kh->count : PROTO_MAX_HOSTKEYS;
            for(uint8_t i = 0; i < l.count; i++) {
                memcpy(l.keys[i].fp, kh->keys[i].fp, sizeof(l.keys[i].fp));
                memcpy(l.keys[i].label, kh->keys[i].label, sizeof(l.keys[i].label));
            }
            free(kh);
        }
        l.policy_log_count = keystore_audit_count(r->ref.profile);
        link_reply(PROTO_T_HOSTKEY_LIST_RESP, buf, proto_enc_hostkey_list(&l, buf, sizeof(buf)));
        break;
    }
    case PROTO_T_HOSTKEY_DELETE: {
        KnownHosts* kh = calloc(1, sizeof(KnownHosts));
        esp_err_t err = ESP_ERR_NO_MEM;
        if(kh) {
            keystore_kh_load(r->hk_delete.profile, kh);
            err = kh_delete(kh, r->hk_delete.fp) ? keystore_kh_save(kh) : ESP_ERR_NOT_FOUND;
            free(kh);
        }
        reply_err(err, "key not found");
        break;
    }
    case PROTO_T_KEY_PASSPHRASE:
        reply_err(
            ssh_client_passphrase(r->passphrase.passphrase, r->passphrase.remember),
            "no passphrase wanted");
        break;
    case PROTO_T_KEY_BLOB_BEGIN: {
        uint16_t e = app_key_begin(r->key_begin.total);
        e ? link_reply_error(e, "key refused") : link_reply_ack();
        break;
    }
    case PROTO_T_KEY_BLOB_CHUNK: {
        uint16_t e = app_key_chunk(&r->script_chunk);
        e ? link_reply_error(e, "key chunk refused") : link_reply_ack();
        break;
    }
    case PROTO_T_KEY_BLOB_END: {
        uint16_t e = app_key_end(&r->script_end);
        e ? link_reply_error(e, "key CRC mismatch") : link_reply_ack();
        break;
    }
    case PROTO_T_AUTH_PASSWORD:
        reply_err(
            ssh_client_password(r->password.password, r->password.remember), "no password wanted");
        break;
    case PROTO_T_SSH_DISCONNECT:
        ssh_client_disconnect();
        link_reply_ack();
        break;
    case PROTO_T_EXEC_REQ:
        reply_err(ssh_client_exec(r->command), "not ready");
        break;
    case PROTO_T_SCRIPT_BEGIN:
        if(ssh_client_state() != ProtoSshReady)
            link_reply_error(ProtoErrState, "not ready");
        else if(!script_buf)
            link_reply_error(ProtoErrInternal, "no script buffer");
        else {
            uint16_t e = script_asm_begin(&script, &r->script_begin);
            e ? link_reply_error(e, "script refused") : link_reply_ack();
        }
        break;
    case PROTO_T_SCRIPT_CHUNK: {
        uint16_t e = script_asm_chunk(&script, &r->script_chunk);
        e ? link_reply_error(e, "chunk refused") : link_reply_ack();
        break;
    }
    case PROTO_T_SCRIPT_END: {
        uint16_t e = script_asm_end(&script, &r->script_end);
        if(e) {
            link_reply_error(e, "script CRC mismatch");
        } else {
            reply_err(ssh_client_script(script.interp, script.buf, script.total), "not ready");
        }
        break;
    }
    case PROTO_T_KEY_GEN:
        reply_err(ssh_client_keygen(&r->key_gen, &link_sink), "session busy");
        break;
    case PROTO_T_KEY_LEGACY_EXPORT:
        reply_err(ssh_client_legacy_export(&link_sink), "no legacy key");
        break;
    case PROTO_T_KEY_LEGACY_DELETE: {
        esp_err_t err = keystore_legacy_delete(r->legacy_delete.crc32);
        if(err == ESP_ERR_INVALID_ARG)
            link_reply_error(ProtoErrBadPayload, "CRC does not match the key");
        else
            reply_err(err, "no legacy key");
        break;
    }
    case PROTO_T_SECRET_FORGET:
        if(r->forget.kind == ProtoForgetPassphrase || r->forget.kind == ProtoForgetUnlocked) {
            if(r->forget.kind == ProtoForgetPassphrase) keystore_pass_forget(r->forget.id);
            ssh_client_lock(r->forget.id);
        } else {
            /* deleting a profile on the Flipper forgets everything kept for it */
            keystore_secret_forget(r->forget.id);
            keystore_kh_forget(r->forget.id);
        }
        link_reply_ack();
        break;
    case PROTO_T_SEC_GET_STATUS: {
        ProtoSecStatus st;
        keystore_sec_status(&st);
        link_reply(PROTO_T_SEC_STATUS, buf, proto_enc_sec_status(&st, buf, sizeof(buf)));
        break;
    }
    case PROTO_T_SEC_ENABLE: {
        /* the word was checked by link_decode_request() (ERROR bad payload otherwise) and is
         * checked again in keystore_sec_enable() before the burn */
        uint8_t st = ssh_client_state();
        if(st != ProtoSshIdle && st != ProtoSshClosed && st != ProtoSshFailed) {
            link_reply_error(ProtoErrState, "close the SSH session first");
            break;
        }
        memcpy(sec_word, r->sec_enable.word, sizeof(sec_word));
        sec_running = true;
        if(xTaskCreate(sec_worker, "sec", 6144, NULL, 5, NULL) != pdPASS) {
            sec_running = false;
            memset(sec_word, 0, sizeof(sec_word));
            link_reply_error(ProtoErrInternal, "no memory");
            break;
        }
        link_reply_ack();
        break;
    }
    default:
        link_reply_error(ProtoErrUnknownType, "unknown type");
        break;
    }
}

void app_link_start(void) {
    key_mu = xSemaphoreCreateMutex();
    key_timer =
        xTimerCreate("keystage", pdMS_TO_TICKS(KEY_STAGE_TIMEOUT_MS), pdFALSE, NULL, key_timeout);
    script_asm_init(&key_asm, key_buf, PROTO_KEY_MAX);
    script_buf = heap_caps_malloc(PROTO_SCRIPT_MAX, MALLOC_CAP_SPIRAM);
    if(!script_buf) script_buf = malloc(PROTO_SCRIPT_MAX);
    script_asm_init(&script, script_buf, script_buf ? PROTO_SCRIPT_MAX : 0);
    xTaskCreate(status_worker, "netstat", 4096, NULL, 4, &status_task);
    ESP_ERROR_CHECK(link_init(on_request, NULL));
    link_set_peer_lost_cb(on_peer_lost);
}
