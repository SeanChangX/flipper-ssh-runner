/* libssh client task. SPDX-License-Identifier: GPL-3.0-only */
#include "ssh_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hostkey_policy.h"
#include "keystore.h"
#include "libssh/libssh.h"
#include "net.h"
#include "proto/keyfmt.h"
#include "sdkconfig.h"

static const char* TAG = "ssh";

#define DECISION_TIMEOUT_MS (120 * 1000)
#define PASSWORD_TIMEOUT_MS (120 * 1000)
#define CONNECT_TIMEOUT_S   10
#define MAX_PASSWORD_TRIES  3
#define READ_SLICE_MS       50

typedef enum {
    CmdConnect,
    CmdExec,
    CmdScript,
    CmdDisconnect,
    CmdKeygen,
    CmdLegacyExport,
    CmdLock, /* drop unlocked keys from RAM: `id`, or all when empty */
} CmdType;

typedef struct {
    CmdType type;
    char id[PROTO_STR_KEYID + 1];
} Cmd;

/* Unlocked keys, like ssh-agent: an encrypted key stays parsed in RAM after its first
 * unlock until it is forgotten, renamed, deleted, locked or the board restarts, so only
 * the first connect after boot pays the bcrypt KDF (~11 s). Owned by the client task.
 * SECURITY.md "Unlocked keys". */
#define UNLOCKED_MAX 4
typedef struct {
    char id[PROTO_STR_KEYID + 1]; /* the id the passphrase is remembered under */
    char fp[KEYFMT_FP_SIZE]; /* the key's fingerprint */
    ssh_key key;
} Unlocked;

typedef struct {
    bool is_decision;
    ProtoHostkeyDecision decision;
    char password[PROTO_STR_PASSWORD + 1];
    bool remember;
} Answer;

static struct {
    QueueHandle_t cmds;
    QueueHandle_t answers;
    TaskHandle_t task;
    atomic_uint state;
    atomic_uint credit;
    atomic_bool cancel;
    atomic_bool stop;
    SshSink sink;
    ProtoSshConnect req;
    uint8_t iface;
    char command[PROTO_STR_COMMAND + 1];
    char interp[PROTO_STR_INTERP + 1];
    const uint8_t* script;
    size_t script_len;
    ssh_session session;
    SshStats stats;
    const char* key; /* staged key file (owned by the caller until key_release) */
    size_t key_len;
    ProtoKeyGen gen;
    SshSink key_sink; /* for KEY_GEN / legacy export */
    Unlocked unlocked[UNLOCKED_MAX];
} c;

/* ---------------- events ---------------- */

static void emit_state(uint8_t st, uint8_t reason, const char* msg) {
    atomic_store(&c.state, st);
    ProtoSshStateEvt e = {.state = st, .reason = reason, .iface = c.iface};
    e.stored_secret = keystore_secret_exists(c.req.profile) ? 1 : 0;
    snprintf(e.msg, sizeof(e.msg), "%s", msg ? msg : "");
    if(c.sink.state) c.sink.state(c.sink.ctx, &e);
}

static void track_heap(void) {
    uint32_t free_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if(free_now < c.stats.heap_min_internal) c.stats.heap_min_internal = free_now;
}

/* ---------------- public API (any task) ---------------- */

uint8_t ssh_client_state(void) {
    return (uint8_t)atomic_load(&c.state);
}

static bool idle(void) {
    uint8_t st = ssh_client_state();
    return st == ProtoSshIdle || st == ProtoSshFailed || st == ProtoSshClosed;
}

esp_err_t ssh_client_connect(
    const ProtoSshConnect* req,
    const SshSink* sink,
    uint32_t initial_credit,
    const char* key,
    size_t key_len) {
    if(!idle()) return ESP_ERR_INVALID_STATE;
    c.req = *req;
    c.sink = *sink;
    c.key = req->auth == ProtoAuthKey ? key : NULL;
    c.key_len = c.key ? key_len : 0;
    atomic_store(&c.credit, initial_credit);
    atomic_store(&c.cancel, false);
    atomic_store(&c.stop, false);
    xQueueReset(c.answers);
    atomic_store(&c.state, ProtoSshConnecting);
    Cmd cmd = {.type = CmdConnect};
    return xQueueSend(c.cmds, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_hostkey_decision(const ProtoHostkeyDecision* d) {
    if(ssh_client_state() != ProtoSshHostkeyWait) return ESP_ERR_INVALID_STATE;
    Answer a = {.is_decision = true, .decision = *d};
    return xQueueSend(c.answers, &a, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_password(const char* password, bool remember) {
    if(ssh_client_state() != ProtoSshPasswordWait) return ESP_ERR_INVALID_STATE;
    Answer a = {.is_decision = false, .remember = remember};
    snprintf(a.password, sizeof(a.password), "%s", password);
    BaseType_t ok = xQueueSend(c.answers, &a, 0);
    memset(&a, 0, sizeof(a));
    return ok == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_passphrase(const char* passphrase, bool remember) {
    if(ssh_client_state() != ProtoSshPassphraseWait) return ESP_ERR_INVALID_STATE;
    Answer a = {.is_decision = false, .remember = remember};
    snprintf(a.password, sizeof(a.password), "%s", passphrase);
    BaseType_t ok = xQueueSend(c.answers, &a, 0);
    memset(&a, 0, sizeof(a));
    return ok == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_keygen(const ProtoKeyGen* req, const SshSink* sink) {
    if(!idle()) return ESP_ERR_INVALID_STATE;
    c.gen = *req;
    c.key_sink = *sink;
    Cmd cmd = {.type = CmdKeygen};
    return xQueueSend(c.cmds, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_lock(const char* id) {
    Cmd cmd = {.type = CmdLock};
    snprintf(cmd.id, sizeof(cmd.id), "%s", id ? id : "");
    return xQueueSend(c.cmds, &cmd, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_legacy_export(const SshSink* sink) {
    if(!idle()) return ESP_ERR_INVALID_STATE;
    if(!keystore_legacy_exists()) return ESP_ERR_NOT_FOUND;
    c.key_sink = *sink;
    Cmd cmd = {.type = CmdLegacyExport};
    return xQueueSend(c.cmds, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_exec(const char* command) {
    if(ssh_client_state() != ProtoSshReady) return ESP_ERR_INVALID_STATE;
    snprintf(c.command, sizeof(c.command), "%s", command);
    atomic_store(&c.cancel, false);
    atomic_store(&c.state, ProtoSshRunning);
    Cmd cmd = {.type = CmdExec};
    return xQueueSend(c.cmds, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ssh_client_script(const char* interp, const uint8_t* script, size_t len) {
    if(ssh_client_state() != ProtoSshReady) return ESP_ERR_INVALID_STATE;
    snprintf(c.interp, sizeof(c.interp), "%s", interp);
    c.script = script;
    c.script_len = len;
    atomic_store(&c.cancel, false);
    atomic_store(&c.state, ProtoSshRunning);
    Cmd cmd = {.type = CmdScript};
    return xQueueSend(c.cmds, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void ssh_client_credit(uint32_t bytes) {
    unsigned cur = atomic_load(&c.credit);
    if(cur == SSH_CREDIT_UNLIMITED) return;
    uint64_t next = (uint64_t)cur + bytes;
    atomic_store(
        &c.credit, next >= SSH_CREDIT_UNLIMITED ? SSH_CREDIT_UNLIMITED - 1 : (unsigned)next);
}

void ssh_client_cancel(void) {
    atomic_store(&c.cancel, true);
}

void ssh_client_disconnect(void) {
    atomic_store(&c.cancel, true);
    atomic_store(&c.stop, true);
    Answer a = {.is_decision = true, .decision = {.decision = ProtoDecisionCancel}};
    xQueueSend(c.answers, &a, 0); /* unblock a pending prompt */
    Cmd cmd = {.type = CmdDisconnect};
    xQueueSend(c.cmds, &cmd, 0);
}

void ssh_client_stats(SshStats* out) {
    *out = c.stats;
}

/* ---------------- task side ---------------- */

static uint8_t reason_from_error(const char* err) {
    if(!err) return ProtoReasonInternal;
    if(strstr(err, "refused")) return ProtoReasonRefused;
    if(strstr(err, "timed out") || strstr(err, "Timeout") || strstr(err, "timeout"))
        return ProtoReasonTimeout;
    if(strstr(err, "resolve") || strstr(err, "hostname")) return ProtoReasonDns;
    return ProtoReasonProtocol;
}

static void close_session(void) {
    if(c.session) {
        ssh_disconnect(c.session);
        ssh_free(c.session);
        c.session = NULL;
    }
}

static bool wait_answer(Answer* a, uint32_t timeout_ms) {
    return xQueueReceive(c.answers, a, pdMS_TO_TICKS(timeout_ms)) == pdTRUE &&
           !atomic_load(&c.stop);
}

/* Host-key step. Returns true when authentication may proceed. */
static bool check_hostkey(void) {
    ssh_key srv = NULL;
    if(ssh_get_server_publickey(c.session, &srv) != SSH_OK) {
        emit_state(ProtoSshFailed, ProtoReasonProtocol, "No host key");
        return false;
    }
    unsigned char* hash = NULL;
    size_t hlen = 0;
    char fp[PROTO_STR_FP + 1] = "";
    char ktype[PROTO_STR_KEYTYPE + 1] = "";
    if(ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == SSH_OK) {
        char* f = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
        snprintf(fp, sizeof(fp), "%s", f ? f : "");
        ssh_string_free_char(f);
    }
    snprintf(ktype, sizeof(ktype), "%s", ssh_key_type_to_char(ssh_key_type(srv)));
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(srv);
    if(!fp[0]) {
        emit_state(ProtoSshFailed, ProtoReasonProtocol, "Cannot hash host key");
        return false;
    }

    KnownHosts* kh = calloc(1, sizeof(KnownHosts));
    if(!kh) {
        emit_state(ProtoSshFailed, ProtoReasonInternal, "Out of memory");
        return false;
    }
    keystore_kh_load(c.req.profile, kh);
    KhVerdict v = kh_check(kh, c.req.policy, fp);
    bool decided = false;
    uint8_t decision = ProtoDecisionCancel;
    bool ok = false;

    if(v == KhAccept) {
        ok = true;
    } else if(v == KhReject) {
        emit_state(ProtoSshFailed, ProtoReasonHostkeyRejected, "Host key changed (strict policy)");
    } else {
        ProtoHostkeyPrompt p = {
            .kind = v == KhPromptUnknown ? ProtoHostkeyUnknown : ProtoHostkeyMismatch};
        snprintf(p.key_type, sizeof(p.key_type), "%s", ktype);
        snprintf(p.fp, sizeof(p.fp), "%s", fp);
        p.trusted_total = kh->count;
        p.count = kh->count < PROTO_MAX_TRUSTED_FP ? kh->count : PROTO_MAX_TRUSTED_FP;
        for(uint8_t i = 0; i < p.count; i++) {
            snprintf(p.trusted[i].fp, sizeof(p.trusted[i].fp), "%s", kh->keys[i].fp);
            snprintf(p.trusted[i].label, sizeof(p.trusted[i].label), "%s", kh->keys[i].label);
        }
        emit_state(ProtoSshHostkeyWait, ProtoReasonNone, NULL);
        if(c.sink.hostkey_prompt) c.sink.hostkey_prompt(c.sink.ctx, &p);
        Answer a;
        if(wait_answer(&a, DECISION_TIMEOUT_MS) && a.is_decision) {
            decided = true;
            decision = a.decision.decision;
            KhStore store;
            ok = kh_apply(kh, decision, fp, a.decision.label, &store);
            if(store == KhStoreSave) keystore_kh_save(kh);
            keystore_audit(c.req.profile, fp, decision);
        }
        if(!ok) emit_state(ProtoSshFailed, ProtoReasonHostkeyCancelled, "Host key not accepted");
    }
    free(kh);
    /* No password is sent before the host key is accepted, and only after a yes. */
    return ok && kh_may_authenticate(v, decided, decision);
}

/* ---------------- keys ---------------- */

/* The staged key file is no longer needed: the sink that holds it wipes it. */
static void key_release(void) {
    if(!c.key) return;
    c.key = NULL;
    c.key_len = 0;
    if(c.sink.key_release) c.sink.key_release(c.sink.ctx);
}

/* Public key line (with `comment`), fingerprint, type and bits of a parsed key, through
 * the same code the Flipper uses for files (keyfmt). */
typedef struct {
    char fp[KEYFMT_FP_SIZE];
    KeyInfo info;
    char* line; /* malloc'd, PROTO_PUB_MAX + 1 */
} PubInfo;

static bool pub_info(ssh_key key, const char* comment, PubInfo* out) {
    memset(out, 0, sizeof(*out));
    char* b64 = NULL;
    uint8_t* blob = malloc(KEYFMT_SCRATCH);
    out->line = malloc(PROTO_PUB_MAX + 1);
    bool ok = blob && out->line && ssh_pki_export_pubkey_base64(key, &b64) == SSH_OK;
    if(ok) {
        size_t n = keyfmt_b64_decode(b64, strlen(b64), blob, KEYFMT_SCRATCH);
        out->info.fmt = KeyFmtOpenSsh; /* a parsed key: only type and size are checked */
        ok = n < KEYFMT_SCRATCH && keyfmt_blob_info(blob, n, &out->info) &&
             keyfmt_pub_line(blob, n, comment, out->line, PROTO_PUB_MAX + 1) > 0;
        if(ok) keyfmt_fingerprint(blob, n, out->fp);
    }
    ssh_string_free_char(b64);
    free(blob);
    if(!ok) {
        free(out->line);
        out->line = NULL;
    }
    return ok;
}

static bool key_usable(ssh_key key, PubInfo* pi) {
    if(!pub_info(key, NULL, pi)) {
        emit_state(ProtoSshFailed, ProtoReasonBadKey, "Cannot read the public key");
        return false;
    }
    const char* bad = keyfmt_problem(&pi->info);
    if(!bad && pi->info.type != KeyTypeEd25519 && pi->info.type != KeyTypeEcdsa &&
       pi->info.type != KeyTypeRsa)
        bad = "Key type not supported";
    if(bad) {
        emit_state(ProtoSshFailed, ProtoReasonBadKey, bad);
        free(pi->line);
        pi->line = NULL;
        return false;
    }
    return true;
}

static Unlocked* unlocked_find(const char* id, const char* fp) {
    for(int i = 0; i < UNLOCKED_MAX; i++) {
        Unlocked* u = &c.unlocked[i];
        if(!u->key) continue;
        if((id[0] && (!strcmp(u->id, id) || !strcmp(u->fp, id))) || (fp[0] && !strcmp(u->fp, fp)))
            return u;
    }
    return NULL;
}

/* ssh_key_free() zeroises the private key. */
static void unlocked_drop(Unlocked* u) {
    ssh_key_free(u->key);
    memset(u, 0, sizeof(*u));
}

static void do_lock(const char* id) {
    int n = 0;
    for(int i = 0; i < UNLOCKED_MAX; i++)
        if(c.unlocked[i].key &&
           (!id[0] || !strcmp(c.unlocked[i].id, id) || !strcmp(c.unlocked[i].fp, id))) {
            unlocked_drop(&c.unlocked[i]);
            n++;
        }
    ESP_LOGI(TAG, "locked %d unlocked key(s)", n);
}

static void unlocked_add(const char* id, const char* fp, ssh_key key) {
    Unlocked* slot = NULL;
    for(int i = 0; i < UNLOCKED_MAX && !slot; i++)
        if(!c.unlocked[i].key) slot = &c.unlocked[i];
    if(!slot) {
        slot = &c.unlocked[0]; /* the oldest goes */
        unlocked_drop(slot);
        memmove(&c.unlocked[0], &c.unlocked[1], sizeof(Unlocked) * (UNLOCKED_MAX - 1));
        slot = &c.unlocked[UNLOCKED_MAX - 1];
        memset(slot, 0, sizeof(*slot));
    }
    snprintf(slot->id, sizeof(slot->id), "%s", id);
    snprintf(slot->fp, sizeof(slot->fp), "%s", fp);
    slot->key = key;
}

size_t ssh_client_unlocked_count(void) {
    size_t n = 0;
    for(int i = 0; i < UNLOCKED_MAX; i++)
        n += c.unlocked[i].key != NULL;
    return n;
}

/* Parse the staged key file before any TCP connection, asking for its passphrase when
 * it is encrypted and none is remembered; an unlocked key in RAM is used as it is.
 * NULL after a failure event; *cached = the key belongs to the unlocked cache. */
static ssh_key parse_key(bool* cached) {
    *cached = false;
    if(!c.key) {
        emit_state(ProtoSshFailed, ProtoReasonNoKey, "No key file sent");
        return NULL;
    }
    uint8_t* scratch = malloc(KEYFMT_SCRATCH);
    if(!scratch) {
        emit_state(ProtoSshFailed, ProtoReasonInternal, "Out of memory");
        return NULL;
    }
    KeyInfo ki;
    keyfmt_sniff(
        c.key,
        c.key_len < KEYFMT_SNIFF_BYTES ? c.key_len : KEYFMT_SNIFF_BYTES,
        &ki,
        scratch,
        KEYFMT_SCRATCH);
    char file_fp[KEYFMT_FP_SIZE] = ""; /* from an OpenSSH container's public section */
    if(ki.pub) keyfmt_fingerprint(ki.pub, ki.pub_len, file_fp);
    memset(scratch, 0, KEYFMT_SCRATCH);
    free(scratch);
    const char* bad = keyfmt_problem(&ki);
    ESP_LOGI(
        TAG,
        "key file: %u bytes, format %d, type %d, %u bits%s",
        (unsigned)c.key_len,
        ki.fmt,
        ki.type,
        ki.bits,
        ki.encrypted ? ", encrypted" : "");
    if(bad) {
        emit_state(ProtoSshFailed, ProtoReasonBadKey, bad);
        return NULL;
    }

    ssh_key key = NULL;
    if(!ki.encrypted) {
        if(ssh_pki_import_privkey_base64(c.key, NULL, NULL, NULL, &key) != SSH_OK) {
            emit_state(ProtoSshFailed, ProtoReasonBadKey, "Cannot read the key file");
            return NULL;
        }
    } else {
        /* the Flipper names the key; a key without one gets the hash of its file */
        char id[PROTO_STR_KEYID + 1];
        if(c.req.key_id[0]) {
            snprintf(id, sizeof(id), "%s", c.req.key_id);
        } else {
            KeyfmtSha256 s;
            uint8_t d[32];
            char fid[KEYFMT_FP_SIZE + 5];
            keyfmt_sha256_init(&s);
            keyfmt_sha256_update(&s, c.key, c.key_len);
            keyfmt_sha256_final(&s, d);
            keyfmt_file_id(d, fid);
            snprintf(id, sizeof(id), "%s", fid);
        }
        Unlocked* u = unlocked_find(id, file_fp);
        if(u) {
            ESP_LOGI(TAG, "key %s already unlocked", u->fp);
            key_release();
            *cached = true;
            return u->key;
        }
        char pass[PROTO_STR_PASSPHRASE + 1];
        bool stored = keystore_pass_get(id, pass, sizeof(pass));
        uint8_t reason = ProtoReasonNone;
        bool remember = false;
        while(true) {
            if(!stored) {
                emit_state(ProtoSshPassphraseWait, reason, reason ? "Wrong passphrase" : NULL);
                Answer a;
                if(!wait_answer(&a, PASSWORD_TIMEOUT_MS) || a.is_decision) {
                    memset(&a, 0, sizeof(a));
                    memset(pass, 0, sizeof(pass));
                    emit_state(ProtoSshFailed, ProtoReasonAuthFailed, "No passphrase given");
                    return NULL;
                }
                memcpy(pass, a.password, sizeof(pass));
                remember = a.remember;
                memset(&a, 0, sizeof(a));
            }
            /* OpenSSH's bcrypt KDF takes a few seconds on the ESP32 */
            emit_state(ProtoSshConnecting, ProtoReasonNone, "Unlocking key...");
            if(ssh_pki_import_privkey_base64(c.key, pass, NULL, NULL, &key) == SSH_OK) break;
            key = NULL;
            if(stored) keystore_pass_forget(id); /* the key file got a new passphrase */
            stored = false;
            reason = ProtoReasonBadPassphrase;
        }
        PubInfo upi;
        bool have_fp = pub_info(key, NULL, &upi);
        if(have_fp) free(upi.line);
        /* a key without .pub gets one written now: remember it under its real id */
        const char* keep_id = have_fp && (c.req.flags & PROTO_CONN_WANT_PUB) ? upi.fp : id;
        if(!stored && remember) keystore_pass_set(keep_id, pass);
        memset(pass, 0, sizeof(pass));
        if(stored || remember) {
            /* kept unlocked until forgotten, locked or a restart; "don't remember" means
             * asking every time, so such a key is not kept either */
            PubInfo pi;
            if(key_usable(key, &pi)) {
                if((c.req.flags & PROTO_CONN_WANT_PUB) && c.sink.key_pub)
                    c.sink.key_pub(c.sink.ctx, 0, pi.fp, pi.line);
                free(pi.line);
                unlocked_add(keep_id, have_fp ? upi.fp : "", key);
                key_release();
                *cached = true;
                return key;
            }
            ssh_key_free(key);
            return NULL;
        }
    }

    PubInfo pi;
    if(!key_usable(key, &pi)) {
        ssh_key_free(key);
        return NULL;
    }
    if((c.req.flags & PROTO_CONN_WANT_PUB) && c.sink.key_pub)
        c.sink.key_pub(c.sink.ctx, 0, pi.fp, pi.line);
    ESP_LOGI(TAG, "key %s", pi.fp);
    free(pi.line);
    key_release(); /* the file is parsed: wipe it before any network traffic */
    return key;
}

static void key_fail(const SshSink* sink, uint8_t reason, const char* msg) {
    ESP_LOGW(TAG, "key: %s", msg);
    if(sink->key_pub) sink->key_pub(sink->ctx, reason, "", msg);
}

/* Send a private key file and its public line to the sink, then wipe our copy. */
static void key_out(const SshSink* sink, ssh_key key, char* pem, const char* passphrase) {
    PubInfo pi;
    if(!pub_info(key, "ssh_runner", &pi)) {
        key_fail(sink, ProtoReasonInternal, "Cannot export the public key");
        return;
    }
    if(sink->key_blob && !sink->key_blob(sink->ctx, (const uint8_t*)pem, strlen(pem))) {
        key_fail(sink, ProtoReasonInternal, "Key not delivered");
    } else {
        /* decrypt once: a passphrase chosen here is never asked for on this board */
        if(passphrase && passphrase[0]) keystore_pass_set(pi.fp, passphrase);
        if(sink->key_pub) sink->key_pub(sink->ctx, 0, pi.fp, pi.line);
    }
    free(pi.line);
}

static void do_keygen(void) {
    SshSink sink = c.key_sink;
    bool ecdsa = c.gen.key_type == ProtoKeyGenEcdsaP256;
    ssh_key key = NULL;
    int64_t t0 = esp_timer_get_time();
    if(ssh_pki_generate(
           ecdsa ? SSH_KEYTYPE_ECDSA_P256 : SSH_KEYTYPE_ED25519, ecdsa ? 256 : 0, &key) !=
       SSH_OK) {
        memset(&c.gen, 0, sizeof(c.gen));
        key_fail(&sink, ProtoReasonInternal, "Key generation failed");
        return;
    }
    char* pem = NULL;
    /* OpenSSH format for both types (libssh would pick PEM for ECDSA by default with
     * some backends); encrypted with bcrypt + aes256-ctr when a passphrase is given */
    if(ssh_pki_export_privkey_base64_format(
           key,
           c.gen.passphrase[0] ? c.gen.passphrase : NULL,
           NULL,
           NULL,
           &pem,
           SSH_FILE_FORMAT_OPENSSH) != SSH_OK) {
        key_fail(&sink, ProtoReasonInternal, "Key export failed");
    } else {
        ESP_LOGI(
            TAG,
            "generated %s key in %lu ms",
            ecdsa ? "ECDSA P-256" : "Ed25519",
            (unsigned long)((esp_timer_get_time() - t0) / 1000));
        key_out(&sink, key, pem, c.gen.passphrase);
        memset(pem, 0, strlen(pem));
        ssh_string_free_char(pem);
    }
    memset(&c.gen, 0, sizeof(c.gen));
    ssh_key_free(key);
}

static void do_legacy_export(void) {
    SshSink sink = c.key_sink;
    char* pem = keystore_legacy_get();
    if(!pem) {
        key_fail(&sink, ProtoReasonNoKey, "No legacy key");
        return;
    }
    ssh_key key = NULL;
    if(ssh_pki_import_privkey_base64(pem, NULL, NULL, NULL, &key) != SSH_OK) {
        key_fail(&sink, ProtoReasonBadKey, "Legacy key unreadable");
    } else {
        key_out(&sink, key, pem, NULL);
        ssh_key_free(key);
    }
    memset(pem, 0, strlen(pem));
    free(pem);
}

static bool authenticate(ssh_key key) {
    if(c.req.auth == ProtoAuthKey) {
        /* IdentitiesOnly: exactly this key, no agent, no other key, no password */
        emit_state(ProtoSshAuthenticating, ProtoReasonNone, NULL);
        int rc = ssh_userauth_publickey(c.session, NULL, key);
        if(rc == SSH_AUTH_SUCCESS) return true;
        ESP_LOGW(TAG, "key auth: %s", ssh_get_error(c.session));
        if(!(c.req.flags & PROTO_CONN_KEY_FALLBACK) || rc != SSH_AUTH_DENIED) {
            emit_state(ProtoSshFailed, ProtoReasonAuthFailed, "Key not accepted by the host");
            return false;
        }
        /* Quick Connect only (OpenSSH does the same): the default key was refused, ask
         * for the password. Saved hosts stay strict. */
    }

    char pw[PROTO_STR_PASSWORD + 1];
    bool from_store = keystore_secret_get(c.req.profile, pw, sizeof(pw));
    uint8_t reason = ProtoReasonNone;
    for(int attempt = 0; attempt < MAX_PASSWORD_TRIES + (from_store ? 1 : 0); attempt++) {
        bool remember = false;
        if(!from_store) {
            emit_state(
                ProtoSshPasswordWait, reason, reason ? "Permission denied, try again" : NULL);
            Answer a;
            if(!wait_answer(&a, PASSWORD_TIMEOUT_MS) || a.is_decision) {
                memset(&a, 0, sizeof(a));
                emit_state(ProtoSshFailed, ProtoReasonAuthFailed, "No password given");
                return false;
            }
            memcpy(pw, a.password, sizeof(pw));
            remember = a.remember || (c.req.flags & PROTO_CONN_REMEMBER);
            memset(&a, 0, sizeof(a));
        }
        emit_state(
            ProtoSshAuthenticating,
            ProtoReasonNone,
            from_store ? "Using remembered password" : NULL);
        int rc = ssh_userauth_password(c.session, NULL, pw);
        if(rc == SSH_AUTH_SUCCESS) {
            if(remember && !from_store) keystore_secret_set(c.req.profile, pw);
            memset(pw, 0, sizeof(pw));
            return true;
        }
        if(rc == SSH_AUTH_ERROR) break;
        /* a remembered password that stopped working is asked for again */
        from_store = false;
        reason = ProtoReasonAuthFailed;
        memset(pw, 0, sizeof(pw));
    }
    memset(pw, 0, sizeof(pw));
    emit_state(ProtoSshFailed, ProtoReasonAuthFailed, "Too many authentication failures");
    return false;
}

static void do_connect_with(ssh_key key) {
    char src[16];
    if(!net_pick(c.req.transport, c.req.host, &c.iface, src, sizeof(src))) {
        emit_state(
            ProtoSshFailed,
            ProtoReasonNoLink,
            c.req.transport == ProtoTransportLan  ? "LAN has no link or address" :
            c.req.transport == ProtoTransportWifi ? "WiFi is not connected" :
                                                    "No network: LAN and WiFi down");
        return;
    }
    c.session = ssh_new();
    if(!c.session) {
        emit_state(ProtoSshFailed, ProtoReasonInternal, "Out of memory");
        return;
    }
    unsigned int port = c.req.port;
    long timeout = CONNECT_TIMEOUT_S;
    bool no = false;
    int verbosity = CONFIG_FES_SSH_LOG_VERBOSITY;
    ssh_options_set(c.session, SSH_OPTIONS_HOST, c.req.host);
    ssh_options_set(c.session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(c.session, SSH_OPTIONS_USER, c.req.user);
    ssh_options_set(c.session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(c.session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(c.session, SSH_OPTIONS_PROCESS_CONFIG, &no);
    ssh_options_set(c.session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, SSH_PUBKEY_ALGOS);
    /* Host keys are ours to check (per profile); never read files. */
    ssh_options_set(c.session, SSH_OPTIONS_KNOWNHOSTS, "/dev/null");
    ssh_options_set(c.session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/dev/null");
    /* Bind to the chosen interface's address: with LAN and WiFi both up, the routing table
     * alone would pick one. */
    ssh_options_set(c.session, SSH_OPTIONS_BINDADDR, src);
    ESP_LOGI(
        TAG,
        "connecting %s@%s:%u via %s (%s)",
        c.req.user,
        c.req.host,
        port,
        c.iface == ProtoIfaceLan ? "LAN" : "WiFi",
        src);

    int64_t t0 = esp_timer_get_time();
    int rc = ssh_connect(c.session);
    c.stats.handshake_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    track_heap();
    if(rc != SSH_OK) {
        const char* err = ssh_get_error(c.session);
        ESP_LOGW(TAG, "connect failed: %s", err);
        emit_state(ProtoSshFailed, reason_from_error(err), err);
        close_session();
        return;
    }
    if(!check_hostkey() || !authenticate(key)) {
        close_session();
        return;
    }
    track_heap();
    emit_state(ProtoSshReady, ProtoReasonNone, NULL);
}

static void do_connect(void) {
    c.stats.heap_min_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    emit_state(ProtoSshConnecting, ProtoReasonNone, NULL);
    ssh_key key = NULL;
    bool cached = false;
    if(c.req.auth == ProtoAuthKey) key = parse_key(&cached);
    key_release(); /* also on every failure */
    if(c.req.auth != ProtoAuthKey || key) do_connect_with(key);
    if(!cached) ssh_key_free(key); /* libssh zeroises private key material on free */
}

/* Where an exec's time goes, logged when it ends: waiting for the Flipper's credit, for the
 * remote's data, or handing data to the link (UART). */
static struct {
    uint64_t bytes;
    int64_t credit_us, input_us, sink_us;
} prof;

/* Read whatever is available on one stream, within the flow-control credit; never waits. */
static int pump(ssh_channel ch, int is_stderr) {
    uint8_t buf[PROTO_STREAM_CHUNK];
    unsigned credit = atomic_load(&c.credit);
    if(credit == 0) return 0;
    size_t want = credit < sizeof(buf) ? credit : sizeof(buf);
    int n = ssh_channel_read_nonblocking(ch, buf, (uint32_t)want, is_stderr);
    if(n == SSH_EOF) return 0; /* drained; ssh_channel_is_eof() ends the run */
    if(n > 0) {
        if(credit != SSH_CREDIT_UNLIMITED) atomic_fetch_sub(&c.credit, (unsigned)n);
        int64_t t = esp_timer_get_time();
        if(c.sink.stream)
            c.sink.stream(
                c.sink.ctx, is_stderr ? ProtoStreamStderr : ProtoStreamStdout, buf, (size_t)n);
        prof.sink_us += esp_timer_get_time() - t;
        prof.bytes += (uint64_t)n;
    }
    return n;
}

/* Nothing on either stream: wait for the socket (a packet for any stream), one slice at
 * most. A blocking read per stream waited a whole slice on an empty stderr every round,
 * which held stdout to one 511-byte chunk per 50 ms (~9 KiB/s). */
static void wait_input(void) {
    socket_t fd = ssh_get_fd(c.session);
    if(fd == SSH_INVALID_SOCKET) {
        vTaskDelay(pdMS_TO_TICKS(READ_SLICE_MS));
        return;
    }
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = {.tv_sec = 0, .tv_usec = READ_SLICE_MS * 1000};
    select(fd + 1, &rd, NULL, NULL, &tv);
}

static void do_run(bool script) {
    int64_t t0 = esp_timer_get_time();
    ProtoExecDone done = {0};
    memset(&prof, 0, sizeof(prof));
    ssh_channel ch = ssh_channel_new(c.session);
    bool ok = ch && ssh_channel_open_session(ch) == SSH_OK;
    if(ok) {
        if(script) {
            /* `ssh host 'sh -s' < script`: nothing is written to the target's disk */
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "exec %s -s", c.interp);
            ok = ssh_channel_request_exec(ch, cmd) == SSH_OK;
            size_t off = 0;
            while(ok && off < c.script_len && !atomic_load(&c.cancel)) {
                size_t n = c.script_len - off > 1024 ? 1024 : c.script_len - off;
                int w = ssh_channel_write(ch, c.script + off, (uint32_t)n);
                if(w <= 0)
                    ok = false;
                else
                    off += (size_t)w;
            }
            if(ok) ssh_channel_send_eof(ch);
        } else {
            ok = ssh_channel_request_exec(ch, c.command) == SSH_OK;
        }
    }
    if(!ok) {
        done.flags = PROTO_DONE_LOST;
        done.exit_code = -1;
        ESP_LOGW(TAG, "exec failed: %s", ssh_get_error(c.session));
    } else {
        bool cancelled = false;
        while(true) {
            if(atomic_load(&c.cancel) && !cancelled) {
                cancelled = true;
                ssh_channel_request_send_signal(ch, "INT");
                ssh_channel_close(ch);
            }
            int a = pump(ch, 0);
            int b = pump(ch, 1);
            track_heap();
            if(a == SSH_ERROR || b == SSH_ERROR) break;
            if(cancelled) break;
            if(atomic_load(&c.credit) == 0) {
                /* back-pressure: the remote waits until the Flipper grants credit */
                int64_t t = esp_timer_get_time();
                vTaskDelay(pdMS_TO_TICKS(10));
                prof.credit_us += esp_timer_get_time() - t;
                continue;
            }
            if(ssh_channel_is_eof(ch) && a == 0 && b == 0) break;
            if(!ssh_channel_is_open(ch)) break;
            if(a == 0 && b == 0) {
                int64_t t = esp_timer_get_time();
                wait_input();
                prof.input_us += esp_timer_get_time() - t;
            }
        }
        uint32_t code = 0;
        char* sig = NULL;
        int core = 0;
        if(cancelled) {
            done.flags = PROTO_DONE_CANCELLED;
            done.exit_code = 130;
        } else if(ssh_channel_get_exit_state(ch, &code, &sig, &core) == SSH_OK) {
            if(sig) {
                done.flags = PROTO_DONE_SIGNAL;
                done.exit_code = 0;
                ESP_LOGI(TAG, "killed by SIG%s", sig);
                ssh_string_free_char(sig);
            } else {
                done.exit_code = (int32_t)code;
            }
        } else {
            done.flags = PROTO_DONE_LOST;
            done.exit_code = -1;
        }
    }
    if(ch) {
        ssh_channel_close(ch);
        ssh_channel_free(ch);
    }
    done.duration_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(
        TAG,
        "exec: %llu B in %lu ms; waited %lld ms for credit, %lld ms for input, %lld ms in the link",
        (unsigned long long)prof.bytes,
        (unsigned long)done.duration_ms,
        prof.credit_us / 1000,
        prof.input_us / 1000,
        prof.sink_us / 1000);
    c.script = NULL;
    if(c.sink.done) c.sink.done(c.sink.ctx, &done);
    if(ssh_is_connected(c.session)) {
        emit_state(ProtoSshReady, ProtoReasonNone, NULL);
    } else {
        emit_state(ProtoSshClosed, ProtoReasonPeerClosed, "Connection closed by host");
        close_session();
    }
}

static void task(void* arg) {
    (void)arg;
    Cmd cmd;
    while(true) {
        if(xQueueReceive(c.cmds, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* idle check: notice a host that went away while Ready */
            if(c.session && ssh_client_state() == ProtoSshReady && !ssh_is_connected(c.session)) {
                close_session();
                emit_state(ProtoSshClosed, ProtoReasonPeerClosed, "Connection closed by host");
            }
            continue;
        }
        switch(cmd.type) {
        case CmdConnect:
            do_connect();
            break;
        case CmdExec:
        case CmdScript:
            if(c.session) do_run(cmd.type == CmdScript);
            break;
        case CmdDisconnect:
            if(c.session || ssh_client_state() != ProtoSshIdle) {
                close_session();
                emit_state(ProtoSshClosed, ProtoReasonNone, NULL);
            }
            atomic_store(&c.stop, false);
            break;
        case CmdKeygen:
            do_keygen();
            break;
        case CmdLegacyExport:
            do_legacy_export();
            break;
        case CmdLock:
            do_lock(cmd.id);
            break;
        }
    }
}

esp_err_t ssh_client_init(void) {
    if(ssh_init() != SSH_OK) return ESP_FAIL;
    c.cmds = xQueueCreate(4, sizeof(Cmd));
    c.answers = xQueueCreate(1, sizeof(Answer));
    atomic_store(&c.state, ProtoSshIdle);
    /* libssh needs a deep stack; internal RAM, because the task writes NVS (flash
     * operations disable the cache, so the stack must not live in PSRAM). */
    BaseType_t ok =
        xTaskCreatePinnedToCore(task, "ssh", CONFIG_FES_SSH_STACK_SIZE, NULL, 5, &c.task, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
