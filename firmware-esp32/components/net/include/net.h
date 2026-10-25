/*
 * Networking: LAN (W5500 over SPI, esp_eth MACRAW + lwIP) and WiFi STA, each with
 * DHCP or static IPv4, plus scan/join, ping and transport selection.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "proto/proto_msg.h"

/* Called from the network event task on every link / address change, and exactly
 * once when a WiFi join attempt finishes (success or failure). */
typedef void (*NetStatusCallback)(void* ctx);
typedef void (*NetScanCallback)(const ProtoScanResult* batch, void* ctx);
typedef void (*NetPingCallback)(const ProtoPingResp* resp, void* ctx);

esp_err_t net_init(NetStatusCallback on_status, void* ctx);
void net_get_status(ProtoNetStatus* out);
/* Apply and persist DHCP/static settings for one interface. */
esp_err_t net_set_cfg(const ProtoNetCfg* cfg);
esp_err_t net_wifi_scan(NetScanCallback cb, void* ctx);
/* Asynchronous; credentials are persisted on success only. */
esp_err_t net_wifi_join(const char* ssid, const char* pass);
/* Once per finished join: how it ended (true), else false. Read before the status it
 * comes with, so WIFI_JOIN_RESULT goes out ahead of that NET_STATUS_EVT. */
bool net_take_join_result(ProtoWifiJoinResult* out);
/* Disconnect and erase the stored WiFi credentials (the only copy: WiFi storage is RAM). */
/* Join the saved network again with its saved password; the password never leaves the
 * board (console `wifi rejoin`). ESP_ERR_NOT_FOUND if nothing is saved. */
esp_err_t net_wifi_rejoin(void);
esp_err_t net_wifi_forget(void);
esp_err_t net_ping(const ProtoPingReq* req, NetPingCallback cb, void* ctx);
void net_ping_cancel(void);

/* Transport selection for a connection to `host` (IPv4 address or name): `lan`/`wifi` that
 * interface, `auto` as a routing table does (route_policy.h): the interface whose subnet holds
 * the host, else LAN, else WiFi. On success fills the interface used and its IPv4 address as
 * text (to bind the socket to). */
bool net_pick(uint8_t transport, const char* host, uint8_t* iface, char* src_ip, size_t src_ip_size);
