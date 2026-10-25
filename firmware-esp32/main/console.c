/*
 * USB console for bring-up and testing: drives SSH on the ESP32 without the Flipper,
 * through the same net/ssh/keystore code paths as the Flipper link.
 *
 *   status | heap | wifi scan | wifi join <ssid> [pass] | netcfg lan|wifi dhcp
 *   netcfg lan|wifi static <ip> <mask> <gw> [dns] | ping <host> [n] [auto|lan|wifi]
 *   ssh <user@host[:port]> [-t auto|lan|wifi] [-a password|key] [-p prompt|strict|off]
 *       [-n profile] [-r] [-k key_id] [-P] [-F]
 *         (-r: remember the password; -a key uses the staged key file, -k names its
 *          remembered passphrase, -P asks for its public key line)
 *   trust cancel|once|add|replace [label] | pw <password> | pp <passphrase> [-n]
 *   exec <command ...> | script <bytes> [exit_code] [sh|bash] | cancel | bye
 *   cycles <n> <command ...>
 *   keyblob begin <bytes> | keyblob hex <hex> | keyblob end [keep] | keyblob check | keyblob drop
 *   keygen ed25519|ecdsa [passphrase] | legacy create|export|delete <crc32> | forgetpp <key_id>
 *   keys | keys lock [key_id] | restart
 *   hostkeys <profile> | forget <profile> | audit | stats
 *
 * `keyblob` stages a key file like the Flipper does; `keyblob end keep` also keeps
 * a copy in the console so `cycles` can restage it. Test keys only: this console
 * prints generated private keys.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app.h"
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bootinfo.h"
#include "keystore.h"
#include "lwip/inet.h"
#include "net.h"
#include "sdkconfig.h"

#if CONFIG_FES_CONSOLE

static ProtoSshConnect last_conn;
static SemaphoreHandle_t done_sem;
static ProtoExecDone last_done;
static uint8_t* script_buf;
static SemaphoreHandle_t key_sem;
static char* kept_key; /* `keyblob end keep`: console copy for `cycles` */
static size_t kept_len;
static size_t hex_off; /* next offset of `keyblob hex` */
static uint32_t hex_crc;

static const char* const state_names[] = {
    "idle",
    "connecting",
    "hostkey?",
    "password?",
    "authenticating",
    "ready",
    "running",
    "failed",
    "closed",
    "passphrase?"};

/* ---------------- sink ---------------- */

static void c_state(void* ctx, const ProtoSshStateEvt* st) {
    (void)ctx;
    printf(
        "[ssh] %s%s%s%s\n",
        st->state < 10 ? state_names[st->state] : "?",
        st->msg[0] ? ": " : "",
        st->msg,
        st->stored_secret ? " (password remembered)" : "");
    if(st->state == ProtoSshPassphraseWait) printf("      answer with: pp <passphrase> [-n]\n");
    if(st->state == ProtoSshHostkeyWait)
        printf("      answer with: trust cancel|once|add|replace\n");
    if(st->state == ProtoSshPasswordWait) printf("      answer with: pw <password>\n");
}

static void c_prompt(void* ctx, const ProtoHostkeyPrompt* p) {
    (void)ctx;
    printf(
        "[ssh] %s host key %s %s\n",
        p->kind == ProtoHostkeyUnknown ? "UNKNOWN" : "DIFFERENT",
        p->key_type,
        p->fp);
    for(uint8_t i = 0; i < p->count; i++)
        printf("      trusted: %s %s\n", p->trusted[i].fp, p->trusted[i].label);
}

static void c_stream(void* ctx, uint8_t stream, const uint8_t* data, size_t len) {
    (void)ctx;
    FILE* f = stream == ProtoStreamStderr ? stderr : stdout;
    fwrite(data, 1, len, f);
    fflush(f);
}

static void c_done(void* ctx, const ProtoExecDone* d) {
    (void)ctx;
    last_done = *d;
    printf(
        "\n[ssh] %s %ld after %lu ms\n",
        d->flags & PROTO_DONE_SIGNAL    ? "signal" :
        d->flags & PROTO_DONE_CANCELLED ? "cancelled, exit" :
        d->flags & PROTO_DONE_LOST      ? "lost, exit" :
                                          "exit",
        (long)d->exit_code,
        (unsigned long)d->duration_ms);
    xSemaphoreGive(done_sem);
}

static bool c_key_blob(void* ctx, const uint8_t* data, size_t len) {
    (void)ctx;
    printf(
        "[key] file %u bytes crc 0x%08lx\n",
        (unsigned)len,
        (unsigned long)proto_crc32(data, len, 0));
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    return true;
}

static void c_key_pub(void* ctx, uint8_t status, const char* fp, const char* line) {
    (void)ctx;
    if(status)
        printf("[key] failed (%u): %s\n", status, line);
    else
        printf("[key] pub %s\n[key] fp %s\n", line, fp);
    xSemaphoreGive(key_sem);
}

const SshSink console_sink = {
    .state = c_state,
    .hostkey_prompt = c_prompt,
    .stream = c_stream,
    .done = c_done,
    .key_blob = c_key_blob,
    .key_pub = c_key_pub};

/* ---------------- helpers ---------------- */

static const char* ip_str(uint32_t ip, char* buf) {
    snprintf(
        buf,
        16,
        "%lu.%lu.%lu.%lu",
        (unsigned long)(ip >> 24),
        (unsigned long)((ip >> 16) & 255),
        (unsigned long)((ip >> 8) & 255),
        (unsigned long)(ip & 255));
    return buf;
}

static bool parse_ip(const char* s, uint32_t* out) {
    struct in_addr a;
    if(!inet_aton(s, &a)) return false;
    *out = ntohl(a.s_addr);
    return true;
}

static uint8_t parse_transport(const char* s) {
    return !strcmp(s, "lan")  ? ProtoTransportLan :
           !strcmp(s, "wifi") ? ProtoTransportWifi :
                                ProtoTransportAuto;
}

static void join_args(int argc, char** argv, int from, char* out, size_t size) {
    out[0] = '\0';
    for(int i = from; i < argc; i++) {
        strlcat(out, argv[i], size);
        if(i + 1 < argc) strlcat(out, " ", size);
    }
}

static bool wait_state(uint8_t want, uint32_t ms) {
    for(uint32_t t = 0; t < ms; t += 50) {
        uint8_t st = ssh_client_state();
        if(st == want) return true;
        if(st == ProtoSshFailed || st == ProtoSshClosed) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

/* ---------------- commands ---------------- */

static int cmd_status(int argc, char** argv) {
    (void)argc;
    (void)argv;
    ProtoNetStatus st;
    net_get_status(&st);
    char a[16], b[16];
    printf(
        "LAN : %s%s%s ip %s gw %s (%s)\n",
        st.lan.flags & PROTO_NET_PRESENT ? "present" : "absent",
        st.lan.flags & PROTO_NET_LINK ? ", link" : ", no link",
        st.lan.flags & PROTO_NET_IP ? ", up" : "",
        ip_str(st.lan.ip, a),
        ip_str(st.lan.gw, b),
        st.lan.mode ? "static" : "dhcp");
    printf(
        "WiFi: %s%s ip %s ssid '%s' rssi %d (%s)\n",
        st.wifi.flags & PROTO_NET_LINK ? "associated" : "not associated",
        st.wifi.flags & PROTO_NET_IP ? ", up" : "",
        ip_str(st.wifi.ip, a),
        st.wifi_ssid,
        st.wifi_rssi,
        st.wifi.mode ? "static" : "dhcp");
    printf("SSH : %s\n", state_names[ssh_client_state() < 9 ? ssh_client_state() : 0]);
    /* power tests: a brownout or a watchdog since the last check shows here */
    const BootStats* bs = bootinfo_get();
    printf(
        "Boot: #%lu, reset %s (power-on %lu, brownout %lu, watchdog %lu, crash %lu, other %lu)\n",
        (unsigned long)bs->boots,
        bootstats_reason_name(bs->last),
        (unsigned long)bs->poweron,
        (unsigned long)bs->brownout,
        (unsigned long)bs->watchdog,
        (unsigned long)bs->crash,
        (unsigned long)bs->other);
    return 0;
}

static int cmd_heap(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf(
        "internal free %u (min %u), psram free %u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static void scan_cb(const ProtoScanResult* r, void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < r->count; i++)
        printf(
            "  %-32s %4d dBm ch %2u %s\n",
            r->entries[i].ssid,
            r->entries[i].rssi,
            r->entries[i].channel,
            r->entries[i].auth ? "secured" : "open");
    if(r->last) printf("%u networks\n", r->total);
}

static int cmd_wifi(int argc, char** argv) {
    if(argc >= 2 && !strcmp(argv[1], "scan"))
        return net_wifi_scan(scan_cb, NULL) == ESP_OK ? 0 : 1;
    if(argc >= 3 && !strcmp(argv[1], "join"))
        return net_wifi_join(argv[2], argc > 3 ? argv[3] : "") == ESP_OK ? 0 : 1;
    if(argc >= 2 && !strcmp(argv[1], "rejoin")) {
        esp_err_t err = net_wifi_rejoin(); /* saved network; the password stays on the board */
        if(err != ESP_OK)
            printf("%s\n", err == ESP_ERR_NOT_FOUND ? "no saved network" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if(argc >= 2 && !strcmp(argv[1], "forget")) {
        esp_err_t err = net_wifi_forget();
        printf("%s\n", err == ESP_OK ? "WiFi credentials erased" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: wifi scan | wifi join <ssid> [password] | wifi rejoin | wifi forget\n");
    return 1;
}

static int cmd_netcfg(int argc, char** argv) {
    ProtoNetCfg c = {0};
    if(argc < 3) goto usage;
    c.iface = !strcmp(argv[1], "wifi") ? ProtoIfaceWifi : ProtoIfaceLan;
    if(!strcmp(argv[2], "dhcp")) {
        c.mode = ProtoIpDhcp;
    } else if(!strcmp(argv[2], "static") && argc >= 6) {
        c.mode = ProtoIpStatic;
        if(!parse_ip(argv[3], &c.ip) || !parse_ip(argv[4], &c.mask) || !parse_ip(argv[5], &c.gw))
            goto usage;
        if(argc > 6 && !parse_ip(argv[6], &c.dns)) goto usage;
    } else {
        goto usage;
    }
    return net_set_cfg(&c) == ESP_OK ? 0 : 1;
usage:
    printf("usage: netcfg lan|wifi dhcp | netcfg lan|wifi static <ip> <mask> <gw> [dns]\n");
    return 1;
}

static void ping_cb(const ProtoPingResp* r, void* ctx) {
    (void)ctx;
    printf(
        "ping: %u/%u replies, min/avg/max %u/%u/%u ms via %s\n",
        r->received,
        r->sent,
        r->min_ms,
        r->avg_ms,
        r->max_ms,
        r->iface == ProtoIfaceLan ? "LAN" : "WiFi");
}

static int cmd_ping(int argc, char** argv) {
    if(argc < 2) {
        printf("usage: ping <host> [count] [auto|lan|wifi]\n");
        return 1;
    }
    ProtoPingReq r = {.count = 4};
    snprintf(r.host, sizeof(r.host), "%s", argv[1]);
    if(argc > 2) {
        /* the wire field is one byte (PROTOCOL.md): refuse rather than wrap (1000 -> 232) */
        int n = atoi(argv[2]);
        if(n < 1 || n > 255) {
            printf("ping: count must be 1-255\n");
            return 1;
        }
        r.count = (uint8_t)n;
    }
    if(argc > 3) r.transport = parse_transport(argv[3]);
    esp_err_t err = net_ping(&r, ping_cb, NULL);
    if(err != ESP_OK) printf("ping: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_ssh(int argc, char** argv) {
    if(argc < 2) {
        printf(
            "usage: ssh <user@host[:port]> [-t auto|lan|wifi] [-a password|key] [-p prompt|strict|off] [-n profile] [-r]\n");
        return 1;
    }
    ProtoSshConnect c = {.port = 22};
    char target[128];
    snprintf(target, sizeof(target), "%s", argv[1]);
    char* at = strrchr(target, '@');
    if(!at) {
        printf("need user@host\n");
        return 1;
    }
    *at = '\0';
    snprintf(c.user, sizeof(c.user), "%s", target);
    char* host = at + 1;
    char* colon = strrchr(host, ':');
    if(colon && !strchr(host, ']')) {
        *colon = '\0';
        c.port = (uint16_t)atoi(colon + 1);
    }
    snprintf(c.host, sizeof(c.host), "%s", host);
    snprintf(c.profile, sizeof(c.profile), "%s:%u", c.host, c.port);
    for(int i = 2; i < argc; i++) {
        if(!strcmp(argv[i], "-t") && i + 1 < argc)
            c.transport = parse_transport(argv[++i]);
        else if(!strcmp(argv[i], "-a") && i + 1 < argc)
            c.auth = !strcmp(argv[++i], "key") ? ProtoAuthKey : ProtoAuthPassword;
        else if(!strcmp(argv[i], "-p") && i + 1 < argc) {
            const char* p = argv[++i];
            c.policy = !strcmp(p, "strict") ? ProtoPolicyStrict :
                       !strcmp(p, "off")    ? ProtoPolicyOff :
                                              ProtoPolicyPrompt;
        } else if(!strcmp(argv[i], "-n") && i + 1 < argc)
            snprintf(c.profile, sizeof(c.profile), "%s", argv[++i]);
        else if(!strcmp(argv[i], "-r"))
            c.flags |= PROTO_CONN_REMEMBER;
        else if(!strcmp(argv[i], "-k") && i + 1 < argc)
            snprintf(c.key_id, sizeof(c.key_id), "%s", argv[++i]);
        else if(!strcmp(argv[i], "-P"))
            c.flags |= PROTO_CONN_WANT_PUB;
        else if(!strcmp(argv[i], "-F"))
            c.flags |= PROTO_CONN_KEY_FALLBACK;
    }
    last_conn = c;
    esp_err_t err = app_ssh_connect(&c, &console_sink, SSH_CREDIT_UNLIMITED);
    if(err != ESP_OK) printf("ssh: %s (use `bye` first)\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_trust(int argc, char** argv) {
    ProtoHostkeyDecision d = {0};
    if(argc < 2) {
        printf("usage: trust cancel|once|add|replace [label]\n");
        return 1;
    }
    d.decision = !strcmp(argv[1], "once")    ? ProtoDecisionOnce :
                 !strcmp(argv[1], "add")     ? ProtoDecisionAdd :
                 !strcmp(argv[1], "replace") ? ProtoDecisionReplace :
                                               ProtoDecisionCancel;
    if(argc > 2) snprintf(d.label, sizeof(d.label), "%s", argv[2]);
    return ssh_client_hostkey_decision(&d) == ESP_OK ? 0 : 1;
}

static int cmd_pw(int argc, char** argv) {
    if(argc < 2) return 1;
    esp_err_t err = ssh_client_password(argv[1], (last_conn.flags & PROTO_CONN_REMEMBER) != 0);
    memset(argv[1], 0, strlen(argv[1]));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_pp(int argc, char** argv) {
    if(argc < 2) return 1;
    bool remember = !(argc > 2 && !strcmp(argv[2], "-n"));
    esp_err_t err = ssh_client_passphrase(argv[1], remember);
    memset(argv[1], 0, strlen(argv[1]));
    return err == ESP_OK ? 0 : 1;
}

static int hexval(char ch) {
    return ch >= '0' && ch <= '9' ? ch - '0' :
           ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
           ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 :
                                    -1;
}

/* Stage `len` bytes in chunks, as KEY_BLOB_* from the Flipper would. */
static uint16_t stage(const uint8_t* data, size_t len) {
    uint16_t e = app_key_begin((uint32_t)len);
    for(size_t off = 0; !e && off < len; off += PROTO_SCRIPT_CHUNK) {
        size_t k = len - off > PROTO_SCRIPT_CHUNK ? PROTO_SCRIPT_CHUNK : len - off;
        ProtoScriptChunk ch = {.offset = (uint32_t)off, .len = (uint16_t)k, .data = data + off};
        e = app_key_chunk(&ch);
    }
    ProtoScriptEnd end = {.crc32 = proto_crc32(data, len, 0)};
    return e ? e : app_key_end(&end);
}

static void drop_kept(void) {
    if(kept_key) {
        memset(kept_key, 0, kept_len);
        free(kept_key);
    }
    kept_key = NULL;
    kept_len = 0;
}

static int cmd_keyblob(int argc, char** argv) {
    if(argc < 2) {
        printf("usage: keyblob begin <bytes> | hex <hex> | end [keep] | check | drop\n");
        return 1;
    }
    uint16_t e = ProtoErrNone;
    if(!strcmp(argv[1], "begin") && argc > 2) {
        e = app_key_begin((uint32_t)strtoul(argv[2], NULL, 10));
        hex_off = 0;
        hex_crc = 0;
        drop_kept();
        if(!e && argc > 2) {
            kept_len = strtoul(argv[2], NULL, 10);
            kept_key = calloc(1, kept_len + 1);
        }
    } else if(!strcmp(argv[1], "hex") && argc > 2) {
        static uint8_t bin[PROTO_SCRIPT_CHUNK];
        size_t n = strlen(argv[2]) / 2;
        if(n > sizeof(bin)) n = sizeof(bin);
        for(size_t i = 0; i < n; i++) {
            int hi = hexval(argv[2][2 * i]), lo = hexval(argv[2][2 * i + 1]);
            if(hi < 0 || lo < 0) return 1;
            bin[i] = (uint8_t)(hi << 4 | lo);
        }
        ProtoScriptChunk ch = {.offset = (uint32_t)hex_off, .len = (uint16_t)n, .data = bin};
        e = app_key_chunk(&ch);
        if(!e && kept_key && hex_off + n <= kept_len) memcpy(kept_key + hex_off, bin, n);
        if(!e) {
            hex_off += n;
            hex_crc = proto_crc32(bin, n, hex_crc);
        }
        memset(bin, 0, sizeof(bin));
        memset(argv[2], 0, strlen(argv[2]));
    } else if(!strcmp(argv[1], "end")) {
        /* keyblob end [crc32] [keep]: the sender's CRC catches a garbled console line */
        bool keep = false;
        ProtoScriptEnd end = {.crc32 = hex_crc};
        for(int i = 2; i < argc; i++) {
            if(!strcmp(argv[i], "keep"))
                keep = true;
            else
                end.crc32 = (uint32_t)strtoul(argv[i], NULL, 0);
        }
        e = app_key_end(&end);
        if(e || !keep) drop_kept();
    } else if(!strcmp(argv[1], "check")) {
        printf("keyblob: %u non-zero bytes in the staging buffer\n", (unsigned)app_key_nonzero());
        return 0;
    } else if(!strcmp(argv[1], "drop")) {
        drop_kept();
        app_key_wipe();
    } else {
        return 1;
    }
    if(e)
        printf("keyblob: error %u\n", e);
    else
        printf("keyblob: ok\n");
    return e ? 1 : 0;
}

static int cmd_keygen(int argc, char** argv) {
    ProtoKeyGen g = {
        .key_type = argc > 1 && !strcmp(argv[1], "ecdsa") ? ProtoKeyGenEcdsaP256 :
                                                            ProtoKeyGenEd25519};
    if(argc > 2) {
        snprintf(g.passphrase, sizeof(g.passphrase), "%s", argv[2]);
        memset(argv[2], 0, strlen(argv[2]));
    }
    xSemaphoreTake(key_sem, 0);
    esp_err_t err = ssh_client_keygen(&g, &console_sink);
    memset(&g, 0, sizeof(g));
    if(err != ESP_OK) {
        printf("keygen: %s\n", esp_err_to_name(err));
        return 1;
    }
    return xSemaphoreTake(key_sem, pdMS_TO_TICKS(60000)) == pdTRUE ? 0 : 1;
}

static int cmd_legacy(int argc, char** argv) {
    if(argc < 2) {
        printf("usage: legacy create|export|delete <crc32>\n");
        return 1;
    }
    if(!strcmp(argv[1], "create")) {
        esp_err_t err = keystore_legacy_generate();
        printf("legacy create: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if(!strcmp(argv[1], "export")) {
        xSemaphoreTake(key_sem, 0);
        esp_err_t err = ssh_client_legacy_export(&console_sink);
        if(err != ESP_OK) {
            printf("legacy export: %s\n", esp_err_to_name(err));
            return 1;
        }
        return xSemaphoreTake(key_sem, pdMS_TO_TICKS(30000)) == pdTRUE ? 0 : 1;
    }
    if(!strcmp(argv[1], "delete") && argc > 2) {
        esp_err_t err = keystore_legacy_delete((uint32_t)strtoul(argv[2], NULL, 0));
        printf("legacy delete: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    return 1;
}

static int cmd_forgetpp(int argc, char** argv) {
    if(argc < 2) return 1;
    keystore_pass_forget(argv[1]);
    ssh_client_lock(argv[1]);
    return 0;
}

/* keys lock [key_id]: drop unlocked keys from RAM; keys: how many are unlocked */
static int cmd_keys(int argc, char** argv) {
    if(argc > 1 && !strcmp(argv[1], "lock")) {
        ssh_client_lock(argc > 2 ? argv[2] : NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("keys: %u unlocked in RAM\n", (unsigned)ssh_client_unlocked_count());
    return 0;
}

/* "Protect secrets" status (read-only, every build). No console path burns an eFuse in
 * the normal image; `sectest` exists only in virtual-eFuse test builds. */
static int cmd_sec(int argc, char** argv) {
    (void)argc;
    (void)argv;
    ProtoSecStatus s;
    keystore_sec_status(&s);
    printf(
        "sec: tier %u, key block %s%u, %u free key blocks, %u keystore entries (%u plaintext), burn calls %u%s\n",
        s.tier,
        s.key_block == PROTO_SEC_NO_BLOCK ? "none " : "",
        s.key_block == PROTO_SEC_NO_BLOCK ? 0 : s.key_block,
        s.free_blocks,
        keystore_sec_count(),
        keystore_sec_plain_count(),
        keystore_sec_burn_calls(),
        s.efuse_virtual ? " (VIRTUAL eFuses)" : "");
    return 0;
}

#if CONFIG_EFUSE_VIRTUAL
static void
    sectest_progress(uint8_t phase, uint8_t result, uint8_t migrated, const char* msg, void* ctx) {
    (void)ctx;
    printf("sectest: phase %u result %u migrated %u %s\n", phase, result, migrated, msg);
}

/* Virtual-eFuse test build only ("Protect secrets" firmware test):
 *   sectest seed <n>      n test passwords and passphrases into the keystore
 *   sectest verify <n>    every one of them readable in the keystore now in use
 *   sectest word <w>      keystore_sec_enable(w) directly: a wrong word must not burn
 *   sectest enable [p]    the opt-in with "confirm", restarting at migration point p (1-4)
 *   sectest enable fail   the opt-in with the key derivation failing after the burn */
static int cmd_sectest(int argc, char** argv) {
    if(argc < 2) return 1;
    unsigned n = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    if(!strcmp(argv[1], "seed") || !strcmp(argv[1], "verify")) {
        bool seed = argv[1][0] == 's';
        unsigned ok = 0;
        for(unsigned i = 0; i < n; i++) {
            char name[24], want[32], got[PROTO_STR_PASSWORD + 1];
            snprintf(name, sizeof(name), "t%u", i);
            snprintf(want, sizeof(want), "pw-%u-test", i);
            if(seed) {
                ok += keystore_secret_set(name, want) == ESP_OK;
                snprintf(name, sizeof(name), "SHA256:test%u", i);
                keystore_pass_set(name, want);
            } else {
                ok += keystore_secret_get(name, got, sizeof(got)) && !strcmp(got, want);
            }
        }
        printf("sectest: %s %u/%u\n", argv[1], ok, n);
        return ok == n ? 0 : 1;
    }
    if(!strcmp(argv[1], "word") && argc > 2) {
        unsigned before = keystore_sec_burn_calls();
        esp_err_t err = keystore_sec_enable(argv[2], sectest_progress, NULL);
        printf(
            "sectest: word -> %s, burn calls +%u\n",
            esp_err_to_name(err),
            keystore_sec_burn_calls() - before);
        return 0;
    }
    if(!strcmp(argv[1], "enable")) {
        bool fail = argc > 2 && !strcmp(argv[2], "fail");
        keystore_sec_test_fail_derive(fail);
        keystore_sec_test_crash_at(argc > 2 && !fail ? atoi(argv[2]) : -1);
        esp_err_t err = keystore_sec_enable("confirm", sectest_progress, NULL);
        printf(
            "sectest: enable -> %s, burn calls %u\n",
            esp_err_to_name(err),
            keystore_sec_burn_calls());
        if(err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart(); /* as the app does: the plaintext copy is erased at boot */
        }
        return err == ESP_OK ? 0 : 1;
    }
    return 1;
}
#endif

static int cmd_restart(int argc, char** argv) {
    (void)argc;
    (void)argv;
    esp_restart();
    return 0;
}

static int cmd_exec(int argc, char** argv) {
    char cmd[PROTO_STR_COMMAND + 1];
    join_args(argc, argv, 1, cmd, sizeof(cmd));
    if(!cmd[0]) return 1;
    xSemaphoreTake(done_sem, 0);
    esp_err_t err = ssh_client_exec(cmd);
    if(err != ESP_OK) printf("exec: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* A generated N-byte bash script with a known exit code, run through `bash -s`. */
static int cmd_script(int argc, char** argv) {
    if(argc < 2) {
        printf("usage: script <bytes> [exit_code] [sh|bash]\n");
        return 1;
    }
    size_t want = (size_t)atoi(argv[1]);
    int code = argc > 2 ? atoi(argv[2]) : 3;
    const char* interp = argc > 3 ? argv[3] : "bash";
    if(want < 64 || want > PROTO_SCRIPT_MAX) {
        printf("size must be 64..%u\n", (unsigned)PROTO_SCRIPT_MAX);
        return 1;
    }
    if(!script_buf) script_buf = heap_caps_malloc(PROTO_SCRIPT_MAX, MALLOC_CAP_SPIRAM);
    if(!script_buf) return 1;
    size_t n = (size_t)snprintf((char*)script_buf, want, "#!/bin/%s\nsum=0\n", interp);
    for(int i = 1; n + 48 < want; i++)
        n += (size_t)snprintf(
            (char*)script_buf + n, want - n, "sum=$((sum+%d)); echo \"line %d sum $sum\"\n", i, i);
    n += (size_t)snprintf((char*)script_buf + n, PROTO_SCRIPT_MAX - n, "exit %d\n", code);
    printf("script: %u bytes, expecting exit %d\n", (unsigned)n, code);
    xSemaphoreTake(done_sem, 0);
    return ssh_client_script(interp, script_buf, n) == ESP_OK ? 0 : 1;
}

static int cmd_cancel(int argc, char** argv) {
    (void)argc;
    (void)argv;
    ssh_client_cancel();
    return 0;
}

static int cmd_bye(int argc, char** argv) {
    (void)argc;
    (void)argv;
    ssh_client_disconnect();
    return 0;
}

/* N connect/exec/disconnect cycles with the last `ssh` parameters (key auth or a
 * remembered password, since nobody types in between), heap printed each time. */
static int cmd_cycles(int argc, char** argv) {
    if(argc < 3 || !last_conn.host[0]) {
        printf("usage: cycles <n> <command ...>   (after one successful `ssh`)\n");
        return 1;
    }
    int n = atoi(argv[1]);
    char cmd[PROTO_STR_COMMAND + 1];
    join_args(argc, argv, 2, cmd, sizeof(cmd));
    uint32_t first = 0;
    for(int i = 0; i < n; i++) {
        if(last_conn.auth == ProtoAuthKey && kept_key &&
           stage((const uint8_t*)kept_key, kept_len)) {
            printf("cycle %d: staging the key failed\n", i);
            return 1;
        }
        if(app_ssh_connect(&last_conn, &console_sink, SSH_CREDIT_UNLIMITED) != ESP_OK ||
           !wait_state(ProtoSshReady, 20000)) {
            printf("cycle %d: connect failed\n", i);
            return 1;
        }
        xSemaphoreTake(done_sem, 0);
        ssh_client_exec(cmd);
        xSemaphoreTake(done_sem, pdMS_TO_TICKS(30000));
        ssh_client_disconnect();
        wait_state(ProtoSshClosed, 5000);
        vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if(i == 0) first = heap;
        printf(
            "cycle %3d: exit %ld, internal heap %lu (delta %ld)\n",
            i + 1,
            (long)last_done.exit_code,
            (unsigned long)heap,
            (long)heap - (long)first);
    }
    return 0;
}

static int cmd_hostkeys(int argc, char** argv) {
    if(argc < 2) return 1;
    KnownHosts kh;
    keystore_kh_load(argv[1], &kh);
    for(uint8_t i = 0; i < kh.count; i++)
        printf("%u: %s %s\n", i, kh.keys[i].fp, kh.keys[i].label);
    if(!kh.count) printf("no trusted keys for %s\n", argv[1]);
    return 0;
}

static int cmd_forget(int argc, char** argv) {
    if(argc < 2) return 1;
    keystore_secret_forget(argv[1]);
    keystore_kh_forget(argv[1]);
    return 0;
}

static int cmd_audit(int argc, char** argv) {
    (void)argc;
    (void)argv;
    keystore_audit_dump();
    return 0;
}

static int cmd_link(int argc, char** argv) {
    (void)argc;
    (void)argv;
    LinkDebug d;
    link_get_debug(&d);
    printf(
        "link: peer %s, baud %lu, rx %lu B, %lu frames; tx %lu frames, %lu retransmits, %lu timeouts, "
        "%lu duplicates\n",
        d.peer ? "yes" : "no",
        (unsigned long)d.baud,
        (unsigned long)d.rx_bytes,
        (unsigned long)d.rx_frames,
        (unsigned long)d.stats.tx_frames,
        (unsigned long)d.stats.retransmits,
        (unsigned long)d.stats.timeouts,
        (unsigned long)d.stats.duplicates);
    if(argc > 1 && strcmp(argv[1], "tx") == 0) {
        size_t n = argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : 64;
        link_send_idle_bytes(n);
        printf("sent %u delimiter bytes to the Flipper\n", (unsigned)n);
    }
    if(argc > 1 && strcmp(argv[1], "mute") == 0) {
        uint32_t sec = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 5;
        link_mute(sec * 1000u);
        printf("ignoring the Flipper for %lu s\n", (unsigned long)sec);
    }
    return 0;
}

static int cmd_stats(int argc, char** argv) {
    (void)argc;
    (void)argv;
    SshStats st;
    ssh_client_stats(&st);
    printf(
        "last handshake %lu ms, internal heap low-water %lu\n",
        (unsigned long)st.handshake_ms,
        (unsigned long)st.heap_min_internal);
    return 0;
}

static void reg(const char* name, const char* help, esp_console_cmd_func_t fn) {
    esp_console_cmd_t c = {.command = name, .help = help, .func = fn};
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void) {
    done_sem = xSemaphoreCreateBinary();
    key_sem = xSemaphoreCreateBinary();
    esp_console_repl_t* repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "fes> ";
    cfg.task_stack_size = 8192;
    cfg.max_cmdline_length = 1100; /* `keyblob hex` lines of 512 bytes */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_FES_LINK_UART
    esp_console_dev_usb_serial_jtag_config_t dev =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev, &cfg, &repl));
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev, &cfg, &repl));
#else
    ESP_LOGW("console", "no console device (link uses USB-Serial-JTAG)");
    return;
#endif
    esp_console_register_help_command();
    reg("status", "network and SSH state", cmd_status);
    reg("heap", "free heap", cmd_heap);
    reg("link", "Flipper link counters; link tx [n] | link mute [s] (wiring tests)", cmd_link);
    reg("wifi", "wifi scan | wifi join <ssid> [password] | wifi rejoin | wifi forget", cmd_wifi);
    reg("netcfg", "netcfg lan|wifi dhcp | static <ip> <mask> <gw> [dns]", cmd_netcfg);
    reg("ping", "ping <host> [count] [auto|lan|wifi]", cmd_ping);
    reg("ssh", "ssh <user@host[:port]> [-t ..] [-a ..] [-p ..] [-n profile] [-r]", cmd_ssh);
    reg("trust", "answer a host-key prompt: cancel|once|add|replace [label]", cmd_trust);
    reg("pw", "answer a password prompt", cmd_pw);
    reg("pp", "answer a passphrase prompt: pp <passphrase> [-n] (-n: do not remember)", cmd_pp);
    reg("keyblob",
        "stage a key file: begin <bytes> | hex <hex> | end [keep] | check | drop",
        cmd_keyblob);
    reg("exec", "run a one-liner", cmd_exec);
    reg("script", "run a generated N-byte script via <interp> -s", cmd_script);
    reg("cancel", "interrupt the running command", cmd_cancel);
    reg("bye", "disconnect", cmd_bye);
    reg("cycles", "N connect/exec/disconnect cycles with heap report", cmd_cycles);
    reg("keygen",
        "keygen ed25519|ecdsa [passphrase]: print a new key file and its public key",
        cmd_keygen);
    reg("legacy", "v0 NVS key: legacy create | export | delete <crc32>", cmd_legacy);
    reg("forgetpp",
        "forget a remembered key passphrase (and its unlocked copy): forgetpp <key_id>",
        cmd_forgetpp);
    reg("keys", "unlocked keys in RAM: keys | keys lock [key_id]", cmd_keys);
    reg("restart", "restart the ESP32", cmd_restart);
    reg("sec", "Protect secrets status (read-only)", cmd_sec);
#if CONFIG_EFUSE_VIRTUAL
    reg("sectest",
        "virtual-eFuse secrets test: seed|verify <n> | word <w> | enable [point]",
        cmd_sectest);
#endif
    reg("hostkeys", "list trusted keys of a profile", cmd_hostkeys);
    reg("forget", "forget password and keys of a profile", cmd_forget);
    reg("audit", "host-key decision log", cmd_audit);
    reg("stats", "handshake time and heap low-water", cmd_stats);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

#else
const SshSink console_sink = {0};
void console_start(void) {
}
#endif
