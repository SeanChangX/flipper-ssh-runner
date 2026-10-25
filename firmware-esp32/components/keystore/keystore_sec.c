/*
 * "Protect secrets" (Tier 1): an HMAC key burnt into one eFuse key block derives the
 * NVS encryption keys; the keystore moves from plaintext NVS into the encrypted partition
 * fes_sec, then the plaintext partition is erased.
 *
 * Nothing here burns on its own: the only burn is in sec_burn(), reached only from
 * keystore_sec_enable() after the typed word passed proto_sec_word_ok(). The image never
 * enables CONFIG_NVS_ENCRYPTION, so no IDF path generates (burns) keys at boot.
 *
 * Order (each step is idempotent, so a power loss at any point resumes safely):
 *   (a) burn, skipped when an HMAC_UP block exists;
 *   (b) erase fes_sec, secure init (clears a partial earlier attempt);
 *   (c) copy every keystore entry, re-read and compare;
 *   (d) marker sec/migrated = 1 in fes_sec;
 *   (e) erase the plaintext partition, at the next boot (before WiFi opens NVS).
 * Boot: key and no marker -> redo from (b); marker and plaintext entries left -> redo (e).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "keystore.h"
#include "keystore_i.h"

#include <stdlib.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"
#if CONFIG_EFUSE_VIRTUAL
#include "mbedtls/md.h"
#endif

static const char* TAG = "sec";

#define SEC_NS     "sec"
#define SEC_MARKER "migrated"
#define COPY_MAX   64 /* keystore entries: hosts, passwords, passphrases, WiFi, netcfg, audit */

static bool s_protected; /* the keystore lives in KS_SEC_PART */
static unsigned s_burn_calls; /* test instrumentation: every entry into sec_burn() */
#if CONFIG_EFUSE_VIRTUAL
static int s_crash_at = -1; /* virtual-eFuse tests only: simulated power loss */
static bool s_fail_derive; /* virtual-eFuse tests only: the HMAC derivation fails after the burn */
#endif

/* Every failure before the marker leaves the plaintext keystore as it was (nothing is moved
 * or erased before the copy is verified), so it is said plainly. */
#define SEC_FAILED_UNCHANGED "Protection failed; your data is unchanged."

const char* keystore_part(void) {
    return s_protected ? KS_SEC_PART : KS_PLAIN_PART;
}

unsigned keystore_sec_burn_calls(void) {
    return s_burn_calls;
}

static void crash_point(int point) {
#if CONFIG_EFUSE_VIRTUAL
    if(s_crash_at == point) {
        ESP_LOGW(TAG, "test: power loss at point %d", point);
        esp_restart();
    }
#else
    (void)point;
#endif
}

void keystore_sec_test_crash_at(int point) {
#if CONFIG_EFUSE_VIRTUAL
    s_crash_at = point;
#else
    (void)point;
#endif
}

void keystore_sec_test_fail_derive(bool fail) {
#if CONFIG_EFUSE_VIRTUAL
    s_fail_derive = fail;
#else
    (void)fail;
#endif
}

/* ---------------- eFuse ---------------- */

static bool key_block(esp_efuse_block_t* blk) {
    return esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, blk);
}

/* The one place that burns. 32 random bytes; esp_efuse_write_key() writes key, read- and
 * write-protection and purpose in one batch commit. */
static esp_err_t sec_burn(esp_efuse_block_t* out) {
    s_burn_calls++;
    esp_efuse_block_t blk = esp_efuse_find_unused_key_block();
    if(blk == EFUSE_BLK_KEY_MAX) return ESP_ERR_NOT_FOUND;
    uint8_t key[32];
    esp_fill_random(key, sizeof(key));
    esp_err_t err = esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, key, sizeof(key));
    memset(key, 0, sizeof(key));
    if(err == ESP_OK) *out = blk;
    return err;
}

/* ---------------- encrypted partition ---------------- */

#if CONFIG_EFUSE_VIRTUAL
/* The HMAC peripheral reads real eFuse key blocks only, so a virtual-eFuse test build derives
 * the XTS keys in software from the emulated block, with IDF's seeds (nvs_sec_provider.c:
 * HMAC-SHA256 over 8 words of 0xAEBE5A5A / 0xCEDEA5A5). The normal image uses the peripheral;
 * that one step can only be exercised by a real burn. */
static esp_err_t derive_keys(esp_efuse_block_t blk, nvs_sec_cfg_t* keys) {
    if(s_fail_derive) return ESP_FAIL; /* as the peripheral would on real silicon */
    uint8_t key[32];
    esp_err_t err = esp_efuse_read_block(blk, key, 0, sizeof(key) * 8);
    uint32_t eseed[8], tseed[8];
    for(int i = 0; i < 8; i++) {
        eseed[i] = 0xAEBE5A5Au;
        tseed[i] = 0xCEDEA5A5u;
    }
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(err == ESP_OK &&
       (mbedtls_md_hmac(md, key, sizeof(key), (const uint8_t*)eseed, sizeof(eseed), keys->eky) ||
        mbedtls_md_hmac(md, key, sizeof(key), (const uint8_t*)tseed, sizeof(tseed), keys->tky)))
        err = ESP_FAIL;
    memset(key, 0, sizeof(key));
    return err;
}
#else
static esp_err_t derive_keys(esp_efuse_block_t blk, nvs_sec_cfg_t* keys) {
    nvs_sec_config_hmac_t cfg = {.hmac_key_id = (hmac_key_id_t)(blk - EFUSE_BLK_KEY0)};
    nvs_sec_scheme_t* scheme = NULL;
    esp_err_t err = nvs_sec_provider_register_hmac(&cfg, &scheme);
    if(err != ESP_OK) return err;
    err = nvs_flash_read_security_cfg_v2(scheme, keys); /* derives; never generates (burns) */
    nvs_sec_provider_deregister(scheme);
    return err;
}
#endif

static esp_err_t sec_open_part(esp_efuse_block_t blk, bool erase_first) {
    nvs_sec_cfg_t keys;
    esp_err_t err = derive_keys(blk, &keys);
    if(err != ESP_OK) {
        memset(&keys, 0, sizeof(keys));
        return err;
    }
    nvs_flash_deinit_partition(KS_SEC_PART);
    if(erase_first) err = nvs_flash_erase_partition(KS_SEC_PART);
    if(err == ESP_OK) err = nvs_flash_secure_init_partition(KS_SEC_PART, &keys);
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase_partition(KS_SEC_PART);
        if(err == ESP_OK) err = nvs_flash_secure_init_partition(KS_SEC_PART, &keys);
    }
    memset(&keys, 0, sizeof(keys));
    return err;
}

static bool marker_set(void) {
    nvs_handle_t h;
    if(nvs_open_from_partition(KS_SEC_PART, SEC_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool ok = nvs_get_u8(h, SEC_MARKER, &v) == ESP_OK && v == 1;
    nvs_close(h);
    return ok;
}

static esp_err_t marker_write(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KS_SEC_PART, SEC_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_set_u8(h, SEC_MARKER, 1);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------------- copy ---------------- */

static unsigned ns_count(const char* part) {
    unsigned n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(part, KS_NS, NVS_TYPE_ANY, &it);
    while(res == ESP_OK) {
        n++;
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

unsigned keystore_sec_count(void) {
    return ns_count(keystore_part());
}

unsigned keystore_sec_plain_count(void) {
    return ns_count(KS_PLAIN_PART);
}

/* Reads one entry (blob or string) into a malloc'd buffer; the caller wipes and frees. */
static void* entry_read(nvs_handle_t h, const nvs_entry_info_t* e, size_t* len) {
    size_t n = 0;
    esp_err_t err = e->type == NVS_TYPE_STR ? nvs_get_str(h, e->key, NULL, &n) :
                                              nvs_get_blob(h, e->key, NULL, &n);
    if(err != ESP_OK || !n) return NULL;
    void* buf = malloc(n);
    if(!buf) return NULL;
    err = e->type == NVS_TYPE_STR ? nvs_get_str(h, e->key, buf, &n) :
                                    nvs_get_blob(h, e->key, buf, &n);
    if(err != ESP_OK) {
        memset(buf, 0, n);
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static void wipe_free(void* p, size_t n) {
    if(!p) return;
    memset(p, 0, n);
    free(p);
}

/* (c): every entry of the keystore namespace, then re-read and compare. */
static esp_err_t copy_all(uint8_t* copied) {
    nvs_entry_info_t* list = calloc(COPY_MAX, sizeof(nvs_entry_info_t));
    if(!list) return ESP_ERR_NO_MEM;
    unsigned count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(KS_PLAIN_PART, KS_NS, NVS_TYPE_ANY, &it);
    while(res == ESP_OK && count < COPY_MAX) {
        nvs_entry_info(it, &list[count++]);
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    esp_err_t err = res == ESP_ERR_NVS_NOT_FOUND || res == ESP_OK ? ESP_OK : res;
    if(err == ESP_OK && count == COPY_MAX) err = ESP_ERR_INVALID_SIZE;

    nvs_handle_t src = 0, dst = 0;
    if(err == ESP_OK) err = nvs_open_from_partition(KS_PLAIN_PART, KS_NS, NVS_READONLY, &src);
    if(err == ESP_OK) err = nvs_open_from_partition(KS_SEC_PART, KS_NS, NVS_READWRITE, &dst);
    for(unsigned i = 0; err == ESP_OK && i < count; i++) {
        const nvs_entry_info_t* e = &list[i];
        if(e->type != NVS_TYPE_STR && e->type != NVS_TYPE_BLOB) {
            ESP_LOGW(TAG, "entry %s: type %d not copied", e->key, e->type);
            err = ESP_ERR_NOT_SUPPORTED; /* the keystore stores only these two */
            break;
        }
        size_t n = 0;
        void* v = entry_read(src, e, &n);
        if(!v) {
            err = ESP_FAIL;
            break;
        }
        err = e->type == NVS_TYPE_STR ? nvs_set_str(dst, e->key, v) :
                                        nvs_set_blob(dst, e->key, v, n);
        wipe_free(v, n);
        if(err == ESP_OK) *copied = (uint8_t)(i + 1);
        if(i + 1 == count / 2) crash_point(3); /* half copied, nothing committed */
    }
    if(err == ESP_OK) err = nvs_commit(dst);
    for(unsigned i = 0; err == ESP_OK && i < count; i++) {
        size_t a = 0, b = 0;
        void* x = entry_read(src, &list[i], &a);
        void* y = entry_read(dst, &list[i], &b);
        if(!x || !y || a != b || memcmp(x, y, a) != 0) err = ESP_ERR_INVALID_CRC;
        wipe_free(x, a);
        wipe_free(y, b);
    }
    if(src) nvs_close(src);
    if(dst) nvs_close(dst);
    memset(list, 0, COPY_MAX * sizeof(nvs_entry_info_t));
    free(list);
    return err;
}

/* (e): erasing the sectors (nvs_erase_key() only marks entries and leaves the bytes). */
static esp_err_t erase_plain(void) {
    nvs_flash_deinit_partition(KS_PLAIN_PART);
    esp_err_t err = nvs_flash_erase_partition(KS_PLAIN_PART);
    if(err != ESP_OK) return err;
#if CONFIG_EFUSE_VIRTUAL
    return nvs_flash_init_partition(KS_PLAIN_PART);
#else
    return nvs_flash_init(); /* the default partition keeps system data (PHY calibration) */
#endif
}

/* The derived keys really encrypt: 32 random bytes written to fes_sec read back equal, and
 * the raw partition does not contain them in clear. Runs before anything is copied, so a
 * wrong derivation (the one step only a real burn exercises) cannot cost data. */
static esp_err_t self_test(void) {
    uint8_t probe[32], back[32];
    esp_fill_random(probe, sizeof(probe));
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KS_SEC_PART, SEC_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    size_t n = sizeof(back);
    err = nvs_set_blob(h, "selftest", probe, sizeof(probe));
    if(err == ESP_OK) err = nvs_commit(h);
    if(err == ESP_OK) err = nvs_get_blob(h, "selftest", back, &n);
    if(err == ESP_OK && (n != sizeof(probe) || memcmp(probe, back, n) != 0))
        err = ESP_ERR_INVALID_CRC;
    nvs_erase_key(h, "selftest");
    nvs_commit(h);
    nvs_close(h);
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, KS_SEC_PART);
    uint8_t* chunk = err == ESP_OK && part ? malloc(4096 + sizeof(probe)) : NULL;
    if(err == ESP_OK && !chunk) err = part ? ESP_ERR_NO_MEM : ESP_ERR_NOT_FOUND;
    for(size_t off = 0; err == ESP_OK && off < part->size; off += 4096) {
        size_t len = part->size - off < 4096 + sizeof(probe) ? part->size - off :
                                                               4096 + sizeof(probe);
        err = esp_partition_read_raw(part, off, chunk, len);
        for(size_t i = 0; err == ESP_OK && i + sizeof(probe) <= len; i++)
            if(memcmp(chunk + i, probe, sizeof(probe)) == 0)
                err = ESP_ERR_INVALID_STATE; /* stored in clear */
    }
    free(chunk);
    memset(probe, 0, sizeof(probe));
    memset(back, 0, sizeof(back));
    if(err != ESP_OK) ESP_LOGE(TAG, "encryption self-test failed (%s)", esp_err_to_name(err));
    return err;
}

/* (b) to (d) */
static esp_err_t migrate(esp_efuse_block_t blk, uint8_t* copied) {
    esp_err_t err = sec_open_part(blk, true);
    if(err == ESP_OK) err = self_test();
    if(err != ESP_OK) return err;
    crash_point(2);
    err = copy_all(copied);
    if(err != ESP_OK) return err;
    err = marker_write();
    crash_point(4);
    return err;
}

/* ---------------- boot ---------------- */

esp_err_t keystore_sec_boot(void) {
    s_protected = false;
    esp_efuse_block_t blk;
    if(!key_block(&blk)) return ESP_OK; /* tier 0: plaintext, nothing to do */
    esp_err_t err = sec_open_part(blk, false);
    if(err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "encrypted keystore unreadable (%s); staying on the plaintext one",
            esp_err_to_name(err));
        return ESP_OK;
    }
    if(!marker_set()) {
        ESP_LOGW(TAG, "migration was interrupted: copying again");
        uint8_t copied = 0;
        err = migrate(blk, &copied);
        if(err != ESP_OK) {
            ESP_LOGE(
                TAG, "migration failed (%s); secrets stay in plaintext", esp_err_to_name(err));
            return ESP_OK;
        }
    }
    if(ns_count(KS_PLAIN_PART)) {
        ESP_LOGI(TAG, "erasing the plaintext copy");
        err = erase_plain();
        if(err != ESP_OK) ESP_LOGE(TAG, "erase failed (%s)", esp_err_to_name(err));
    }
    s_protected = true;
    ESP_LOGI(TAG, "secrets protected (eFuse key block %d)", (int)(blk - EFUSE_BLK_KEY0));
    return ESP_OK;
}

/* ---------------- status and the opt-in ---------------- */

void keystore_sec_status(ProtoSecStatus* out) {
    memset(out, 0, sizeof(*out));
    esp_efuse_block_t blk;
    bool have = key_block(&blk);
    out->tier = have && s_protected ? ProtoSecTierProtect : ProtoSecTierNone;
    out->key_block = have ? (uint8_t)(blk - EFUSE_BLK_KEY0) : PROTO_SEC_NO_BLOCK;
    out->free_blocks = (uint8_t)esp_efuse_count_unused_key_blocks();
#if CONFIG_EFUSE_VIRTUAL
    out->efuse_virtual = 1;
#endif
}

esp_err_t keystore_sec_enable(const char* word, KeystoreSecProgress cb, void* ctx) {
    /* checked here again: no caller can reach the burn without the typed word */
    if(!proto_sec_word_ok(word)) return ESP_ERR_INVALID_ARG;
    if(s_protected) {
        cb(ProtoSecPhaseDone, ProtoSecOk, 0, "Already protected", ctx);
        return ESP_OK;
    }
    esp_efuse_block_t blk;
    esp_err_t err = ESP_OK;
    if(!key_block(&blk)) {
        cb(ProtoSecPhaseBurn, ProtoSecRunning, 0, "", ctx);
        err = sec_burn(&blk);
        if(err != ESP_OK) {
            /* write_key() commits in one batch: a failure here burnt nothing usable */
            cb(ProtoSecPhaseBurn,
               ProtoSecFailed,
               0,
               err == ESP_ERR_NOT_FOUND ? "No free eFuse key block. Nothing changed." :
                                          "eFuse write failed. Secrets unchanged.",
               ctx);
            return err;
        }
        crash_point(1);
    }
    uint8_t copied = 0;
    cb(ProtoSecPhaseMigrate, ProtoSecRunning, 0, "", ctx);
    err = migrate(blk, &copied);
    if(err != ESP_OK) {
        cb(ProtoSecPhaseMigrate, ProtoSecFailed, copied, SEC_FAILED_UNCHANGED, ctx);
        return err;
    }
    /* (e) runs at the next boot, before WiFi opens NVS */
    cb(ProtoSecPhaseErase, ProtoSecRunning, copied, "Restarting the board", ctx);
    return ESP_OK;
}
