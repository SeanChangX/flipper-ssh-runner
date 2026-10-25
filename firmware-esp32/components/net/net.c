/* LAN + WiFi networking. SPDX-License-Identifier: GPL-3.0-only */
#include "net.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "join_policy.h"
#include "ping_stats.h"
#include "route_policy.h"
#include "keystore.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

static const char* TAG = "net";

#define JOIN_RETRIES 3
_Static_assert(
    JOIN_REASON_LOCAL_LEAVE == WIFI_REASON_ASSOC_LEAVE,
    "join_policy.h: local leave reason");
_Static_assert(
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT == 15 && WIFI_REASON_AUTH_FAIL == 202 &&
        WIFI_REASON_HANDSHAKE_TIMEOUT == 204 && WIFI_REASON_NO_AP_FOUND == 201 &&
        WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY == 210 &&
        WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD == 211 && WIFI_REASON_MIC_FAILURE == 14 &&
        WIFI_REASON_802_1X_AUTH_FAILED == 23,
    "join_policy.c: reason values");
_Static_assert(
    JOIN_RESULT_WRONG_PASSWORD == ProtoJoinWrongPassword &&
        JOIN_RESULT_NOT_FOUND == ProtoJoinNotFound && JOIN_RESULT_FAILED == ProtoJoinFailed,
    "join_policy.h: result values");

static struct {
    NetStatusCallback on_status;
    void* ctx;
    esp_netif_t* eth_netif;
    esp_netif_t* sta_netif;
    esp_eth_handle_t eth;
    bool eth_present;
    bool eth_link;
    bool wifi_link;
    bool joining;
    uint8_t join_tries;
    uint8_t join_result; /* JOIN_RESULT_* over this join's tries, 0 = none yet */
    bool join_result_pending; /* finished: app.c sends WIFI_JOIN_RESULT before the status */
    bool join_start_pending; /* the driver was busy leaving: start on its STA_DISCONNECTED */
    bool join_config_set; /* the driver took this join's network: only its connection counts */
    bool boot_connect; /* the saved network, once the station has started (STA_START) */
    ProtoWifiJoinResult join_last;
    char join_ssid[PROTO_STR_SSID + 1];
    char join_pass[PROTO_STR_WIFI_PASS + 1];
    bool scanning;
    bool auto_rejoin; /* the saved network is the station's config: keep rejoining it */
    uint8_t rejoin_attempt;
    esp_timer_handle_t rejoin_timer;
    NetScanCallback scan_cb;
    void* scan_ctx;
    esp_ping_handle_t ping;
    NetPingCallback ping_cb;
    void* ping_ctx;
    uint8_t ping_iface;
    PingStats ping_stats;
    SemaphoreHandle_t lock;
} s;

static void notify(void) {
    if(s.on_status) s.on_status(s.ctx);
}

static uint32_t ip_to_u32(esp_ip4_addr_t a) {
    /* esp_ip4_addr_t is network order in memory; the wire format wants a.b.c.d as MSB..LSB */
    return ntohl(a.addr);
}

static esp_ip4_addr_t u32_to_ip(uint32_t v) {
    esp_ip4_addr_t a = {.addr = htonl(v)};
    return a;
}

static void
    fill_iface(esp_netif_t* n, bool present, bool link, uint8_t iface, ProtoIfaceStatus* st) {
    memset(st, 0, sizeof(*st));
    if(!present || !n) return;
    st->flags = PROTO_NET_PRESENT | (link ? PROTO_NET_LINK : 0);
    ProtoNetCfg cfg;
    st->mode = keystore_netcfg_get(iface, &cfg) ? cfg.mode : ProtoIpDhcp;
    esp_netif_ip_info_t ip;
    if(link && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr) {
        st->flags |= PROTO_NET_IP;
        st->ip = ip_to_u32(ip.ip);
        st->mask = ip_to_u32(ip.netmask);
        st->gw = ip_to_u32(ip.gw);
    }
}

void net_get_status(ProtoNetStatus* out) {
    memset(out, 0, sizeof(*out));
    fill_iface(s.eth_netif, s.eth_present, s.eth_link, ProtoIfaceLan, &out->lan);
    fill_iface(s.sta_netif, s.sta_netif != NULL, s.wifi_link, ProtoIfaceWifi, &out->wifi);
    if(s.wifi_link) {
        wifi_ap_record_t ap;
        if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->wifi_rssi = ap.rssi;
            snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%s", (const char*)ap.ssid);
        }
    }
}

static esp_err_t apply_ip(esp_netif_t* n, const ProtoNetCfg* cfg) {
    if(!n) return ESP_ERR_INVALID_STATE;
    if(cfg->mode == ProtoIpDhcp) {
        esp_err_t err = esp_netif_dhcpc_start(n);
        return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
    }
    esp_netif_dhcpc_stop(n);
    esp_netif_ip_info_t ip = {
        .ip = u32_to_ip(cfg->ip), .netmask = u32_to_ip(cfg->mask), .gw = u32_to_ip(cfg->gw)};
    esp_err_t err = esp_netif_set_ip_info(n, &ip);
    if(err == ESP_OK && cfg->dns) {
        esp_netif_dns_info_t dns = {
            .ip.u_addr.ip4 = u32_to_ip(cfg->dns), .ip.type = ESP_IPADDR_TYPE_V4};
        err = esp_netif_set_dns_info(n, ESP_NETIF_DNS_MAIN, &dns);
    }
    return err;
}

esp_err_t net_set_cfg(const ProtoNetCfg* cfg) {
    esp_netif_t* n = cfg->iface == ProtoIfaceLan ? s.eth_netif : s.sta_netif;
    esp_err_t err = apply_ip(n, cfg);
    if(err == ESP_OK) err = keystore_netcfg_set(cfg);
    ESP_LOGI(
        TAG,
        "%s: %s config %s",
        cfg->iface == ProtoIfaceLan ? "LAN" : "WiFi",
        cfg->mode == ProtoIpDhcp ? "DHCP" : "static",
        esp_err_to_name(err));
    notify();
    return err;
}

/* ---------------- rejoining the saved network ---------------- */

/* The saved network as the station's config; false if none is saved. */
static bool use_saved_network(void) {
    char ssid[PROTO_STR_SSID + 1], pass[PROTO_STR_WIFI_PASS + 1];
    if(!keystore_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass))) return false;
    wifi_config_t wc = {0};
    snprintf((char*)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char*)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    memset(pass, 0, sizeof(pass));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    return err == ESP_OK;
}

/* A failed or lost connection: try again later (join_policy.c rejoin_delay_ms). A single
 * try at boot left the board offline for good when that one failed. */
static void rejoin_schedule(void) {
    if(!s.auto_rejoin || s.joining || !s.rejoin_timer) return;
    uint32_t ms = rejoin_delay_ms(s.rejoin_attempt);
    if(s.rejoin_attempt < UINT8_MAX) s.rejoin_attempt++;
    esp_timer_stop(s.rejoin_timer);
    esp_timer_start_once(s.rejoin_timer, (uint64_t)ms * 1000u);
    ESP_LOGI(TAG, "WiFi rejoin in %lu s", (unsigned long)(ms / 1000));
}

static void rejoin_stop(void) {
    if(s.rejoin_timer) esp_timer_stop(s.rejoin_timer);
    s.rejoin_attempt = 0;
}

static void rejoin_fire(void* arg) {
    (void)arg;
    if(!s.auto_rejoin || s.joining || s.wifi_link) return;
    if(s.scanning) {
        /* a connect attempt would fail the scan: after it */
        esp_timer_start_once(s.rejoin_timer, 1000u * 1000u);
        return;
    }
    if(esp_wifi_connect() != ESP_OK) rejoin_schedule();
}

/* ---------------- events ---------------- */

static void join_start(void);

static void join_finished(bool ok) {
    if(!s.joining) return;
    s.joining = false;
    s.join_last.result = ok ? ProtoJoinOk : s.join_result ? s.join_result : ProtoJoinFailed;
    s.join_result_pending = true;
    if(ok) keystore_wifi_set(s.join_ssid, s.join_pass);
    memset(s.join_pass, 0, sizeof(s.join_pass));
    ESP_LOGI(
        TAG,
        "WiFi join %s: %s (result %u, last reason %u)",
        s.join_ssid,
        ok ? "ok" : "failed",
        s.join_last.result,
        s.join_last.reason);
    if(ok) {
        s.auto_rejoin = true; /* the new network is the saved one now */
        rejoin_stop();
    } else {
        /* back to the saved network, if there is one, and keep trying it */
        s.auto_rejoin = use_saved_network();
        rejoin_schedule();
    }
    notify(); /* the single "join finished" status event (PROTOCOL.md) */
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if(base == ETH_EVENT) {
        if(id == ETHERNET_EVENT_CONNECTED) {
            s.eth_link = true;
            ESP_LOGI(TAG, "LAN link up");
        } else if(id == ETHERNET_EVENT_DISCONNECTED) {
            s.eth_link = false;
            ESP_LOGI(TAG, "LAN link down");
        } else {
            return;
        }
        notify();
    } else if(base == WIFI_EVENT) {
        if(id == WIFI_EVENT_STA_START) {
            if(s.boot_connect && !s.joining) {
                s.boot_connect = false;
                esp_wifi_connect(); /* a failure is retried (rejoin_schedule) */
            }
        } else if(id == WIFI_EVENT_STA_DISCONNECTED) {
            bool was = s.wifi_link;
            s.wifi_link = false;
            if(s.joining && s.join_start_pending) {
                join_start(); /* the leave the driver was busy with has ended: not a try */
            } else if(s.joining) {
                const wifi_event_sta_disconnected_t* d = data;
                uint8_t reason = d ? d->reason : 0;
                if(reason != JOIN_REASON_LOCAL_LEAVE) {
                    ESP_LOGI(TAG, "WiFi join try failed (reason %u)", reason);
                    s.join_result = join_result_merge(s.join_result, join_result_for(reason));
                    s.join_last.reason = reason;
                }
                /* ending the previous attempt is not a failed try (join_policy.c) */
                if(join_on_disconnect(reason, &s.join_tries, JOIN_RETRIES) == JoinConnect) {
                    esp_wifi_connect();
                } else {
                    join_finished(false);
                }
            } else {
                const wifi_event_sta_disconnected_t* d = data;
                if(was) {
                    ESP_LOGI(TAG, "WiFi disconnected (reason %u)", d ? d->reason : 0);
                    notify();
                } else {
                    ESP_LOGI(TAG, "WiFi connect failed (reason %u)", d ? d->reason : 0);
                }
                rejoin_schedule();
            }
        } else if(id == WIFI_EVENT_STA_CONNECTED) {
            s.wifi_link = true;
        } else if(id == WIFI_EVENT_SCAN_DONE) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            wifi_ap_record_t* recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
            uint16_t got = n;
            if(recs) esp_wifi_scan_get_ap_records(&got, recs);
            ProtoScanResult r;
            memset(&r, 0, sizeof(r));
            r.total = (uint8_t)(got > 255 ? 255 : got);
            for(uint16_t i = 0; i <= got; i += PROTO_MAX_SCAN_ENTRIES) {
                r.first = (uint8_t)i;
                r.count = 0;
                for(uint16_t k = i; k < got && r.count < PROTO_MAX_SCAN_ENTRIES; k++) {
                    ProtoScanEntry* e = &r.entries[r.count++];
                    snprintf(e->ssid, sizeof(e->ssid), "%s", (const char*)recs[k].ssid);
                    e->rssi = recs[k].rssi;
                    e->auth = recs[k].authmode != WIFI_AUTH_OPEN;
                    e->channel = recs[k].primary;
                }
                r.last = (i + PROTO_MAX_SCAN_ENTRIES >= got);
                if(s.scan_cb) s.scan_cb(&r, s.scan_ctx);
                if(r.last) break;
            }
            free(recs);
            s.scanning = false;
            if(!s.wifi_link) rejoin_schedule(); /* paused for the scan */
        }
    } else if(base == IP_EVENT) {
        if(id == IP_EVENT_ETH_GOT_IP) {
            ip_event_got_ip_t* e = data;
            ESP_LOGI(TAG, "LAN IP " IPSTR, IP2STR(&e->ip_info.ip));
            notify();
        } else if(id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* e = data;
            ESP_LOGI(TAG, "WiFi IP " IPSTR, IP2STR(&e->ip_info.ip));
            s.wifi_link = true;
            rejoin_stop();
            if(s.joining && s.join_config_set) {
                join_finished(true);
            } else if(s.joining) {
                /* the previous network's connection finished before the driver took this
                 * join's: not this join (it would save the new password unchecked). Leave
                 * it; its disconnect starts the join. */
                ESP_LOGI(TAG, "WiFi up on the previous network during a join: leaving it");
                s.join_start_pending = true;
                esp_wifi_disconnect();
            } else {
                notify();
            }
        } else if(id == IP_EVENT_ETH_LOST_IP || id == IP_EVENT_STA_LOST_IP) {
            notify();
        }
    }
}

/* ---------------- W5500 ---------------- */

static esp_err_t lan_init(void) {
#if CONFIG_FES_LAN_ENABLE
    /* Pins are Kconfig options; the defaults are the Waveshare ESP32-S3-ETH's (HARDWARE.md),
     * which the custom board keeps. */
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_FES_W5500_MOSI_GPIO,
        .miso_io_num = CONFIG_FES_W5500_MISO_GPIO,
        .sclk_io_num = CONFIG_FES_W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(CONFIG_FES_W5500_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if(err != ESP_OK) return err;
#if CONFIG_FES_W5500_SPI_SOFT_EDGES
    /* Softer SCLK/MOSI edges (Kconfig help): set after spi_bus_initialize, which configures
     * these pins and would otherwise leave the default strength. Not fatal if it fails. */
    if(gpio_set_drive_capability(CONFIG_FES_W5500_SCLK_GPIO, GPIO_DRIVE_CAP_1) != ESP_OK ||
       gpio_set_drive_capability(CONFIG_FES_W5500_MOSI_GPIO, GPIO_DRIVE_CAP_1) != ESP_OK)
        ESP_LOGW(TAG, "SPI drive strength not lowered");
#endif
    err = gpio_install_isr_service(0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = CONFIG_FES_W5500_SPI_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_FES_W5500_CS_GPIO,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(CONFIG_FES_W5500_SPI_HOST, &dev);
    w5500.int_gpio_num = CONFIG_FES_W5500_INT_GPIO;
    w5500.poll_period_ms = CONFIG_FES_W5500_INT_GPIO < 0 ? 10 : 0;
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = CONFIG_FES_W5500_RST_GPIO;
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_cfg);
    if(!mac || !phy) return ESP_FAIL;
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&cfg, &s.eth);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "W5500 not found (%s): LAN disabled", esp_err_to_name(err));
        return err;
    }
    /* The W5500 has no factory MAC; use the chip's Ethernet MAC. */
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    esp_eth_ioctl(s.eth, ETH_CMD_S_MAC_ADDR, mac_addr);

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s.eth_netif = esp_netif_new(&ncfg);
    esp_netif_attach(s.eth_netif, esp_eth_new_netif_glue(s.eth));
    ProtoNetCfg ip;
    if(keystore_netcfg_get(ProtoIfaceLan, &ip)) apply_ip(s.eth_netif, &ip);
    s.eth_present = true;
    return esp_eth_start(s.eth);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ---------------- WiFi ---------------- */

static esp_err_t wifi_init(void) {
    s.sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if(err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* credentials live in our keystore */
    esp_wifi_set_mode(WIFI_MODE_STA);
    const esp_timer_create_args_t ta = {.callback = rejoin_fire, .name = "wifi_rejoin"};
    esp_timer_create(&ta, &s.rejoin_timer);
    /* the saved network is set before the start and joined on STA_START, as ESP-IDF's
     * examples do. After a power cycle the first try times out in auth (reason 2) on a PMF
     * network that still holds the old association (802.11w SA Query); the 1 s rejoin gets in.
     * esp_restart() leaves the network first, so a software restart joins at once. */
    if(use_saved_network()) {
        s.auto_rejoin = true;
        s.boot_connect = true;
    }
    err = esp_wifi_start();
    if(err != ESP_OK) return err;
    ProtoNetCfg ip;
    if(keystore_netcfg_get(ProtoIfaceWifi, &ip)) apply_ip(s.sta_netif, &ip);
    return ESP_OK;
}

esp_err_t net_wifi_scan(NetScanCallback cb, void* ctx) {
    if(s.scanning || s.joining) return ESP_ERR_INVALID_STATE;
    if(!s.wifi_link && s.rejoin_timer) {
        /* a rejoin attempt in progress would fail the scan: stop it, resume after */
        esp_timer_stop(s.rejoin_timer);
        esp_wifi_disconnect();
    }
    s.scan_cb = cb;
    s.scan_ctx = ctx;
    s.scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if(err != ESP_OK) s.scanning = false;
    return err;
}

/* Sets the join's network and starts it. While the driver is still leaving the previous
 * network (or connecting to it) it refuses the config with ESP_ERR_WIFI_STATE; that leave
 * ends in STA_DISCONNECTED, which calls this again. Failing the join there made it fail at
 * once, with no try, about every other time. */
static void join_start(void) {
    wifi_config_t wc = {0};
    snprintf((char*)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", s.join_ssid);
    snprintf((char*)wc.sta.password, sizeof(wc.sta.password), "%s", s.join_pass);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    if(err == ESP_ERR_WIFI_STATE) {
        s.join_start_pending = true;
        ESP_LOGI(TAG, "WiFi busy leaving: join starts on its disconnect");
        return;
    }
    s.join_start_pending = false;
    s.join_config_set = err == ESP_OK;
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi join config: %s", esp_err_to_name(err));
        join_finished(false);
        return;
    }
    /* ESP_ERR_WIFI_CONN: the previous attempt is still running; its STA_DISCONNECTED
     * (local leave, from the disconnect before) starts this one */
    esp_err_t c = esp_wifi_connect();
    if(join_connect_failed(c == ESP_OK, c == ESP_ERR_WIFI_CONN)) {
        ESP_LOGW(TAG, "WiFi join connect: %s", esp_err_to_name(c));
        join_finished(false);
    }
}

esp_err_t net_wifi_join(const char* ssid, const char* pass) {
    if(s.joining || s.scanning) return ESP_ERR_INVALID_STATE;
    snprintf(s.join_ssid, sizeof(s.join_ssid), "%s", ssid);
    snprintf(s.join_pass, sizeof(s.join_pass), "%s", pass ? pass : "");
    s.joining = true;
    s.join_tries = 0;
    s.join_result = 0;
    s.join_last.reason = 0;
    s.join_result_pending = false; /* one not sent yet (no Flipper then) is stale now */
    s.join_start_pending = false;
    s.join_config_set = false;
    rejoin_stop();
    esp_wifi_disconnect();
    join_start();
    return ESP_OK;
}

bool net_take_join_result(ProtoWifiJoinResult* out) {
    if(!s.join_result_pending) return false;
    s.join_result_pending = false;
    *out = s.join_last;
    return true;
}

esp_err_t net_wifi_rejoin(void) {
    char ssid[PROTO_STR_SSID + 1], pass[PROTO_STR_WIFI_PASS + 1];
    if(!keystore_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass))) return ESP_ERR_NOT_FOUND;
    esp_err_t err = net_wifi_join(ssid, pass);
    memset(pass, 0, sizeof(pass));
    return err;
}

esp_err_t net_wifi_forget(void) {
    if(s.joining || s.scanning) return ESP_ERR_INVALID_STATE;
    s.auto_rejoin = false;
    rejoin_stop();
    esp_wifi_disconnect();
    memset(s.join_ssid, 0, sizeof(s.join_ssid));
    memset(s.join_pass, 0, sizeof(s.join_pass));
    return keystore_wifi_forget();
}

/* ---------------- transport selection ---------------- */

static bool iface_info(esp_netif_t* n, bool link, RouteIfaceInfo* out) {
    esp_netif_ip_info_t ip;
    memset(out, 0, sizeof(*out));
    if(!n || !link || esp_netif_get_ip_info(n, &ip) != ESP_OK || !ip.ip.addr) return false;
    /* all three in network byte order: the subnet test only needs one order throughout */
    *out =
        (RouteIfaceInfo){.up = true, .ip = ip.ip.addr, .mask = ip.netmask.addr, .gw = ip.gw.addr};
    return true;
}

/* IPv4 address of `host` (network byte order), 0 if it does not resolve. */
static uint32_t resolve4(const char* host) {
    struct addrinfo hint = {.ai_family = AF_INET};
    struct addrinfo* res = NULL;
    if(!host || getaddrinfo(host, NULL, &hint, &res) != 0 || !res) return 0;
    uint32_t a = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return a;
}

bool net_pick(uint8_t transport, const char* host, uint8_t* iface, char* src_ip, size_t size) {
    RouteIfaceInfo lan, wifi;
    iface_info(s.eth_netif, s.eth_link, &lan);
    iface_info(s.sta_netif, s.wifi_link, &wifi);
    RouteIface r = RouteNone;
    if(transport == ProtoTransportLan)
        r = lan.up ? RouteLan : RouteNone;
    else if(transport == ProtoTransportWifi)
        r = wifi.up ? RouteWifi : RouteNone;
    else
        r = route_pick_auto(lan.up && wifi.up ? resolve4(host) : 0, &lan, &wifi);
    if(r == RouteNone) return false;
    const RouteIfaceInfo* i = r == RouteLan ? &lan : &wifi;
    esp_ip4_addr_t a = {.addr = i->ip};
    snprintf(src_ip, size, IPSTR, IP2STR(&a));
    *iface = r == RouteLan ? ProtoIfaceLan : ProtoIfaceWifi;
    return true;
}

/* ---------------- ping ---------------- */

static void ping_success(esp_ping_handle_t h, void* arg) {
    (void)arg;
    uint32_t t = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    ping_stats_reply(&s.ping_stats, t);
}

static void ping_end(esp_ping_handle_t h, void* arg) {
    (void)arg;
    uint32_t sent = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    /* times from the replies themselves: the session duration also holds lost requests */
    ProtoPingResp r;
    ping_stats_result(&s.ping_stats, sent, s.ping_iface, &r);
    NetPingCallback cb = s.ping_cb;
    void* ctx = s.ping_ctx;
    esp_ping_delete_session(h);
    s.ping = NULL;
    if(cb) cb(&r, ctx);
}

esp_err_t net_ping(const ProtoPingReq* req, NetPingCallback cb, void* ctx) {
    if(s.ping) return ESP_ERR_INVALID_STATE;
    char src[16];
    uint8_t iface;
    if(!net_pick(req->transport, req->host, &iface, src, sizeof(src)))
        return ESP_ERR_INVALID_STATE;
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    struct addrinfo hint = {.ai_family = AF_INET};
    struct addrinfo* res = NULL;
    if(getaddrinfo(req->host, NULL, &hint, &res) != 0 || !res) return ESP_ERR_NOT_FOUND;
    struct in_addr a = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target), &a);
    target.type = IPADDR_TYPE_V4;
    freeaddrinfo(res);

    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr = target;
    pc.count = req->count ? req->count : 4;
    pc.interval_ms = 500;
    pc.interface =
        esp_netif_get_netif_impl_index(iface == ProtoIfaceLan ? s.eth_netif : s.sta_netif);
    esp_ping_callbacks_t cbs = {.on_ping_success = ping_success, .on_ping_end = ping_end};
    s.ping_cb = cb;
    s.ping_ctx = ctx;
    s.ping_iface = iface;
    ping_stats_start(&s.ping_stats, (uint8_t)pc.count);
    esp_err_t err = esp_ping_new_session(&pc, &cbs, &s.ping);
    if(err == ESP_OK) err = esp_ping_start(s.ping);
    return err;
}

void net_ping_cancel(void) {
    if(s.ping) esp_ping_stop(s.ping); /* on_ping_end still fires */
}

/* ---------------- init ---------------- */

esp_err_t net_init(NetStatusCallback on_status, void* ctx) {
    s.on_status = on_status;
    s.ctx = ctx;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    err = lan_init();
    if(err != ESP_OK) ESP_LOGW(TAG, "LAN unavailable: %s", esp_err_to_name(err));
    err = wifi_init();
    if(err != ESP_OK) ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
    return ESP_OK;
}
