/*
 * SSH client task (libssh): connect, host-key check, password or
 * key auth, exec of one-liners and scripts streamed to `<interp> -s`.
 * One session at a time. Events go to the sink given at connect time, so the same
 * code serves the Flipper link and the USB console.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "proto/proto_msg.h"

typedef struct {
    void (*state)(void* ctx, const ProtoSshStateEvt* st);
    void (*hostkey_prompt)(void* ctx, const ProtoHostkeyPrompt* p);
    /* Never more bytes than the credit granted (ssh_client_credit). */
    void (*stream)(void* ctx, uint8_t stream, const uint8_t* data, size_t len);
    void (*done)(void* ctx, const ProtoExecDone* d);
    /* The staged key file given to ssh_client_connect() is no longer needed:
     * called exactly once per key connect, right after parsing or on failure. */
    void (*key_release)(void* ctx);
    /* A generated or exported private key file (KEY_GEN, KEY_LEGACY_EXPORT). */
    bool (*key_blob)(void* ctx, const uint8_t* data, size_t len);
    /* Its public key line and fingerprint (status 0), or a failure: status is a
     * ProtoReason and `line` the message. Also sent after parsing a key that came
     * with PROTO_CONN_WANT_PUB. */
    void (*key_pub)(void* ctx, uint8_t status, const char* fp, const char* line);
    void* ctx;
} SshSink;

#define SSH_CREDIT_UNLIMITED UINT32_MAX
/* libssh signs only with these: never ssh-rsa (SHA-1). */
#define SSH_PUBKEY_ALGOS \
    "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256"

esp_err_t ssh_client_init(void);
/* ESP_ERR_INVALID_STATE if a session exists. The sink stays bound to the session.
 * With auth = key, `key` is the staged key file (NUL-terminated, `key_len` bytes); the
 * client reads it until sink->key_release(), and never copies it. NULL = none staged. */
esp_err_t ssh_client_connect(
    const ProtoSshConnect* req,
    const SshSink* sink,
    uint32_t initial_credit,
    const char* key,
    size_t key_len);
esp_err_t ssh_client_hostkey_decision(const ProtoHostkeyDecision* d);
esp_err_t ssh_client_password(const char* password, bool remember);
/* Answer to passphrase_wait. */
esp_err_t ssh_client_passphrase(const char* passphrase, bool remember);
/* Generate a key on the client task; results go to sink->key_blob and ->key_pub. */
esp_err_t ssh_client_keygen(const ProtoKeyGen* req, const SshSink* sink);
/* Drop an unlocked key (by key id or fingerprint) from RAM, or all with NULL / "". */
esp_err_t ssh_client_lock(const char* id);
size_t ssh_client_unlocked_count(void);
/* Export the v0 NVS key the same way (ESP_ERR_NOT_FOUND if there is none). */
esp_err_t ssh_client_legacy_export(const SshSink* sink);
esp_err_t ssh_client_exec(const char* command);
/* `script` must stay valid until the done event; the client does not copy it. */
esp_err_t ssh_client_script(const char* interp, const uint8_t* script, size_t len);
void ssh_client_credit(uint32_t bytes);
void ssh_client_cancel(void);
void ssh_client_disconnect(void);
uint8_t ssh_client_state(void); /* ProtoSshState */

/* Handshake time and heap use, measured to compare SSH libraries. */
typedef struct {
    uint32_t handshake_ms; /* TCP connect + key exchange of the last session */
    uint32_t heap_min_internal; /* low-water mark of internal heap during it */
} SshStats;
void ssh_client_stats(SshStats* out);
