/* NVS-backed keystore. SPDX-License-Identifier: GPL-3.0-only */
#include "keystore.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "libssh/libssh.h"
#include "keystore_i.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "keystore";

#define AUDIT_MAX 16

typedef struct {
    uint32_t time; /* seconds since boot if no RTC/SNTP */
    uint8_t decision;
    char profile[PROTO_STR_PROFILE + 1];
    char fp[PROTO_STR_FP + 1];
} AuditEntry;

typedef struct {
    uint8_t next;
    uint8_t count;
    AuditEntry e[AUDIT_MAX];
} AuditLog;

typedef struct {
    char profile[PROTO_STR_PROFILE + 1];
    char password[PROTO_STR_PASSWORD + 1];
} SecretBlob;

typedef struct {
    char ssid[PROTO_STR_SSID + 1];
    char pass[PROTO_STR_WIFI_PASS + 1];
} WifiBlob;

/* NVS keys are at most 15 characters: "<prefix><8 hex of FNV-1a(profile)>". The blob
 * stores the full profile name, so a hash collision reads as "nothing stored". */
static void profile_key(char out[16], char prefix, const char* profile) {
    uint32_t h = 2166136261u;
    for(const char* c = profile; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    snprintf(out, 16, "%c%08lx", prefix, (unsigned long)h);
}

static esp_err_t blob_get(const char* key, void* out, size_t size) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(keystore_part(), KS_NS, NVS_READONLY, &h);
    if(err != ESP_OK) return err;
    size_t n = size;
    err = nvs_get_blob(h, key, out, &n);
    nvs_close(h);
    if(err == ESP_OK && n != size) err = ESP_ERR_INVALID_SIZE;
    return err;
}

static esp_err_t blob_set(const char* key, const void* data, size_t size) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(keystore_part(), KS_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, size);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t key_erase(const char* key) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(keystore_part(), KS_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t keystore_init(void) {
    /* The image never enables CONFIG_NVS_ENCRYPTION: nvs_flash_init() is plain and never
     * generates (burns) keys. Encryption comes only with "Protect secrets". */
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) return err;
#if CONFIG_EFUSE_VIRTUAL
    ESP_LOGW(TAG, "virtual eFuses: test keystore in " KS_PLAIN_PART ", nothing real is burnt");
    err = nvs_flash_init_partition(KS_PLAIN_PART);
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(KS_PLAIN_PART);
        err = nvs_flash_init_partition(KS_PLAIN_PART);
    }
    if(err != ESP_OK) return err;
#endif
    return keystore_sec_boot();
}

/* ---------------- legacy device key (v0, migrated to the SD card) ---------------- */

/* The v0 firmware kept one Ed25519 key as an OpenSSH-format string under "ed25519". */
char* keystore_legacy_get(void) {
    nvs_handle_t h;
    if(nvs_open_from_partition(keystore_part(), KS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t n = 0;
    char* pem = NULL;
    if(nvs_get_str(h, "ed25519", NULL, &n) == ESP_OK && n > 1) {
        pem = malloc(n);
        if(pem && nvs_get_str(h, "ed25519", pem, &n) != ESP_OK) {
            memset(pem, 0, n);
            free(pem);
            pem = NULL;
        }
    }
    nvs_close(h);
    return pem;
}

bool keystore_legacy_exists(void) {
    nvs_handle_t h;
    if(nvs_open_from_partition(keystore_part(), KS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0;
    bool ok = nvs_get_str(h, "ed25519", NULL, &n) == ESP_OK && n > 1;
    nvs_close(h);
    return ok;
}

esp_err_t keystore_legacy_delete(uint32_t crc32) {
    char* pem = keystore_legacy_get();
    if(!pem) return ESP_ERR_NOT_FOUND;
    size_t n = strlen(pem);
    bool same = proto_crc32((const uint8_t*)pem, n, 0) == crc32;
    memset(pem, 0, n);
    free(pem);
    if(!same) return ESP_ERR_INVALID_ARG; /* the Flipper does not hold this exact key */
    ESP_LOGI(TAG, "legacy key deleted after export");
    return key_erase("ed25519");
}

esp_err_t keystore_legacy_generate(void) {
    ssh_key key = NULL;
    if(ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) return ESP_FAIL;
    char* b64 = NULL;
    esp_err_t err = ESP_FAIL;
    if(ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &b64) == SSH_OK) {
        nvs_handle_t h;
        err = nvs_open_from_partition(keystore_part(), KS_NS, NVS_READWRITE, &h);
        if(err == ESP_OK) {
            err = nvs_set_str(h, "ed25519", b64);
            if(err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        memset(b64, 0, strlen(b64));
        ssh_string_free_char(b64);
    }
    ssh_key_free(key);
    return err;
}

/* ---------------- key passphrases ("decrypt once", by key id) ---------------- */

typedef struct {
    char id[PROTO_STR_KEYID + 1];
    char pass[PROTO_STR_PASSPHRASE + 1];
} PassBlob;

bool keystore_pass_get(const char* id, char* pass, size_t size) {
    char key[16];
    profile_key(key, 'p', id);
    PassBlob b;
    bool ok = blob_get(key, &b, sizeof(b)) == ESP_OK && strcmp(b.id, id) == 0;
    if(ok && pass) snprintf(pass, size, "%s", b.pass);
    memset(&b, 0, sizeof(b));
    return ok;
}

esp_err_t keystore_pass_set(const char* id, const char* pass) {
    char key[16];
    profile_key(key, 'p', id);
    PassBlob b;
    memset(&b, 0, sizeof(b));
    snprintf(b.id, sizeof(b.id), "%s", id);
    snprintf(b.pass, sizeof(b.pass), "%s", pass);
    esp_err_t err = blob_set(key, &b, sizeof(b));
    memset(&b, 0, sizeof(b));
    return err;
}

esp_err_t keystore_pass_forget(const char* id) {
    char key[16];
    profile_key(key, 'p', id);
    return key_erase(key);
}

/* ---------------- known hosts ---------------- */

void keystore_kh_load(const char* profile, KnownHosts* kh) {
    char key[16];
    profile_key(key, 'k', profile);
    if(blob_get(key, kh, sizeof(*kh)) != ESP_OK || strcmp(kh->profile, profile) != 0)
        kh_init(kh, profile);
}

esp_err_t keystore_kh_save(const KnownHosts* kh) {
    char key[16];
    profile_key(key, 'k', kh->profile);
    return blob_set(key, kh, sizeof(*kh));
}

esp_err_t keystore_kh_forget(const char* profile) {
    char key[16];
    profile_key(key, 'k', profile);
    return key_erase(key);
}

/* ---------------- secrets ---------------- */

bool keystore_secret_get(const char* profile, char* password, size_t size) {
    char key[16];
    profile_key(key, 's', profile);
    SecretBlob b;
    bool ok = blob_get(key, &b, sizeof(b)) == ESP_OK && strcmp(b.profile, profile) == 0;
    if(ok && password) snprintf(password, size, "%s", b.password);
    memset(&b, 0, sizeof(b));
    return ok;
}

bool keystore_secret_exists(const char* profile) {
    return keystore_secret_get(profile, NULL, 0);
}

esp_err_t keystore_secret_set(const char* profile, const char* password) {
    char key[16];
    profile_key(key, 's', profile);
    SecretBlob b;
    memset(&b, 0, sizeof(b));
    snprintf(b.profile, sizeof(b.profile), "%s", profile);
    snprintf(b.password, sizeof(b.password), "%s", password);
    esp_err_t err = blob_set(key, &b, sizeof(b));
    memset(&b, 0, sizeof(b));
    return err;
}

esp_err_t keystore_secret_forget(const char* profile) {
    char key[16];
    profile_key(key, 's', profile);
    return key_erase(key);
}

/* ---------------- network ---------------- */

bool keystore_wifi_get(char* ssid, size_t ssid_size, char* pass, size_t pass_size) {
    WifiBlob b;
    if(blob_get("wifi", &b, sizeof(b)) != ESP_OK || !b.ssid[0]) return false;
    snprintf(ssid, ssid_size, "%s", b.ssid);
    snprintf(pass, pass_size, "%s", b.pass);
    memset(&b, 0, sizeof(b));
    return true;
}

esp_err_t keystore_wifi_set(const char* ssid, const char* pass) {
    WifiBlob b;
    memset(&b, 0, sizeof(b));
    snprintf(b.ssid, sizeof(b.ssid), "%s", ssid);
    snprintf(b.pass, sizeof(b.pass), "%s", pass);
    esp_err_t err = blob_set("wifi", &b, sizeof(b));
    memset(&b, 0, sizeof(b));
    return err;
}

esp_err_t keystore_wifi_forget(void) {
    esp_err_t err = key_erase("wifi");
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

bool keystore_netcfg_get(uint8_t iface, ProtoNetCfg* cfg) {
    return blob_get(iface == ProtoIfaceLan ? "net_lan" : "net_wifi", cfg, sizeof(*cfg)) == ESP_OK;
}

esp_err_t keystore_netcfg_set(const ProtoNetCfg* cfg) {
    return blob_set(cfg->iface == ProtoIfaceLan ? "net_lan" : "net_wifi", cfg, sizeof(*cfg));
}

/* ---------------- audit ---------------- */

static void audit_load(AuditLog* log) {
    if(blob_get("audit", log, sizeof(*log)) != ESP_OK) memset(log, 0, sizeof(*log));
}

void keystore_audit(const char* profile, const char* fp, uint8_t decision) {
    if(decision == ProtoDecisionCancel) return;
    static const char* const names[] = {"cancel", "once", "add", "replace"};
    ESP_LOGW(
        TAG, "host key accepted (%s): %s %s", decision < 4 ? names[decision] : "?", profile, fp);
    AuditLog* log = calloc(1, sizeof(AuditLog));
    if(!log) return;
    audit_load(log);
    AuditEntry* e = &log->e[log->next];
    memset(e, 0, sizeof(*e));
    e->time = (uint32_t)time(NULL);
    e->decision = decision;
    snprintf(e->profile, sizeof(e->profile), "%s", profile);
    snprintf(e->fp, sizeof(e->fp), "%s", fp);
    log->next = (uint8_t)((log->next + 1) % AUDIT_MAX);
    if(log->count < AUDIT_MAX) log->count++;
    blob_set("audit", log, sizeof(*log));
    free(log);
}

uint8_t keystore_audit_count(const char* profile) {
    AuditLog* log = calloc(1, sizeof(AuditLog));
    if(!log) return 0;
    audit_load(log);
    uint8_t n = 0;
    for(uint8_t i = 0; i < log->count; i++)
        if(strcmp(log->e[i].profile, profile) == 0) n++;
    free(log);
    return n;
}

void keystore_audit_dump(void) {
    AuditLog* log = calloc(1, sizeof(AuditLog));
    if(!log) return;
    audit_load(log);
    static const char* const names[] = {"cancel", "once", "add", "replace"};
    for(uint8_t i = 0; i < log->count; i++) {
        const AuditEntry* e = &log->e[(log->next + AUDIT_MAX - log->count + i) % AUDIT_MAX];
        printf(
            "%10lu  %-8s %-20s %s\n",
            (unsigned long)e->time,
            e->decision < 4 ? names[e->decision] : "?",
            e->profile,
            e->fp);
    }
    free(log);
}
