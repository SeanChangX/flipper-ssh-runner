/*
 * Persistent secrets and settings in NVS (plaintext until "Protect secrets"):
 * pinned host keys per profile, remembered passwords and key passphrases, WiFi
 * credentials, per-interface IP settings and the host-key decision audit log, plus
 * the v0 device key until it has been migrated. Private keys themselves live on the
 * Flipper's SD card; none crosses the UART except the one-time legacy export.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hostkey_policy.h"
#include "proto/proto_msg.h"

esp_err_t keystore_init(void);

/* ---- legacy device key: the single NVS key of v0, exported once to keys/ ---- */
/* OpenSSH-format private key, malloc'd; the caller wipes and frees it. NULL if none. */
char* keystore_legacy_get(void);
bool keystore_legacy_exists(void);
/* Deletes it only if `crc32` matches the exported file (ESP_ERR_INVALID_ARG otherwise). */
esp_err_t keystore_legacy_delete(uint32_t crc32);
/* Console only: create a v0-style key to test the migration. */
esp_err_t keystore_legacy_generate(void);

/* ---- key passphrases, remembered by key id ("decrypt once") ---- */
bool keystore_pass_get(const char* id, char* pass, size_t size);
esp_err_t keystore_pass_set(const char* id, const char* pass);
esp_err_t keystore_pass_forget(const char* id);

/* ---- pinned host keys, per profile ---- */
void keystore_kh_load(const char* profile, KnownHosts* kh);
esp_err_t keystore_kh_save(const KnownHosts* kh);
esp_err_t keystore_kh_forget(const char* profile);

/* ---- remembered passwords (opt-in per profile) ---- */
bool keystore_secret_exists(const char* profile);
bool keystore_secret_get(const char* profile, char* password, size_t size);
esp_err_t keystore_secret_set(const char* profile, const char* password);
esp_err_t keystore_secret_forget(const char* profile);

/* ---- network ---- */
bool keystore_wifi_get(char* ssid, size_t ssid_size, char* pass, size_t pass_size);
esp_err_t keystore_wifi_set(const char* ssid, const char* pass);
esp_err_t keystore_wifi_forget(void);
bool keystore_netcfg_get(uint8_t iface, ProtoNetCfg* cfg);
esp_err_t keystore_netcfg_set(const ProtoNetCfg* cfg);

/* ---- "Protect secrets" (Tier 1) ---- */
typedef void (*KeystoreSecProgress)(
    uint8_t phase,
    uint8_t result,
    uint8_t migrated,
    const char* msg,
    void* ctx);
void keystore_sec_status(ProtoSecStatus* out);
/* Burns the HMAC eFuse key (unless one exists) and copies the keystore into the encrypted
 * partition. ESP_ERR_INVALID_ARG, with nothing touched, unless proto_sec_word_ok(word).
 * On success the plaintext copy is erased at the next boot: the caller restarts the board. */
esp_err_t keystore_sec_enable(const char* word, KeystoreSecProgress cb, void* ctx);
/* Entries in the keystore now (console `sec`, tests). */
unsigned keystore_sec_count(void);
/* Entries left in the plaintext partition (0 once protected: the copy is erased). */
unsigned keystore_sec_plain_count(void);
/* Test instrumentation: how often the burn function was entered since boot. */
unsigned keystore_sec_burn_calls(void);
/* Virtual-eFuse test builds only (no-op otherwise): restart at a migration point 1-4. */
void keystore_sec_test_crash_at(int point);
/* Virtual-eFuse test builds only: make the key derivation fail after the burn (as a broken
 * HMAC peripheral would); the plaintext must survive and the board keep working. */
void keystore_sec_test_fail_derive(bool fail);

/* ---- audit log of accepted host keys (every decision except cancel) ---- */
void keystore_audit(const char* profile, const char* fp, uint8_t decision);
uint8_t keystore_audit_count(const char* profile);
/* Print the log to stdout (console `audit` command). */
void keystore_audit_dump(void);
