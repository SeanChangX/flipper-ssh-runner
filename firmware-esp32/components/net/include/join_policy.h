/* WiFi join decisions, without ESP-IDF (host-tested in test_host/test_join_policy.c).
 * SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* WIFI_REASON_ASSOC_LEAVE: the station itself left (our esp_wifi_disconnect()). net.c checks
 * that it matches the ESP-IDF value. */
#define JOIN_REASON_LOCAL_LEAVE 8

typedef enum {
    JoinConnect, /* call esp_wifi_connect() again */
    JoinFail, /* report the join as failed */
} JoinStep;

/* STA_DISCONNECTED while a join runs. Ending the previous attempt (our own disconnect) is
 * not a failed try; any other reason counts one, up to `max_tries`. */
JoinStep join_on_disconnect(uint8_t reason, uint8_t* tries, uint8_t max_tries);

/* esp_wifi_connect() right after the join's disconnect. `still_connecting` is
 * ESP_ERR_WIFI_CONN: the previous attempt has not ended yet, and its STA_DISCONNECTED (local
 * leave) will start the new one, so that is no failure. Returns true if the join fails now. */
bool join_connect_failed(bool ok, bool still_connecting);

/* Rejoining the saved network after a failed or lost connection, as station software
 * usually does: try again after 1, 2, 4, 8, 16 s, then every 30 s, until it is up again or
 * forgotten. `attempt` counts from 0 and is reset on success. */
#define REJOIN_MAX_MS 30000u
uint32_t rejoin_delay_ms(uint8_t attempt);

/* What a failed join tells the user, from the ESP-IDF disconnect reason of a try (values of
 * wifi_reason_t; net.c checks them): the password was refused (WPA2 4-way handshake, WPA3 SAE
 * auth), the network is not there, or anything else (signal, router). A ProtoJoinResult. */
#define JOIN_RESULT_WRONG_PASSWORD 1
#define JOIN_RESULT_NOT_FOUND      2
#define JOIN_RESULT_FAILED         3
uint8_t join_result_for(uint8_t reason);
/* Over the tries of one join: the clearest answer wins (a refused password over not found
 * over anything else), so a timeout on the last try does not hide a refused password. */
uint8_t join_result_merge(uint8_t so_far, uint8_t this_try);
