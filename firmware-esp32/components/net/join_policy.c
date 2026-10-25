/* WiFi join decisions (see join_policy.h). SPDX-License-Identifier: GPL-3.0-only */
#include "join_policy.h"

JoinStep join_on_disconnect(uint8_t reason, uint8_t* tries, uint8_t max_tries) {
    if(reason == JOIN_REASON_LOCAL_LEAVE) return JoinConnect;
    return ++(*tries) < max_tries ? JoinConnect : JoinFail;
}

bool join_connect_failed(bool ok, bool still_connecting) {
    return !ok && !still_connecting;
}

uint8_t join_result_for(uint8_t reason) {
    switch(reason) {
    case 14: /* MIC_FAILURE */
    case 15: /* 4WAY_HANDSHAKE_TIMEOUT: WPA2, the key does not match */
    case 23: /* 802_1X_AUTH_FAILED */
    case 202: /* AUTH_FAIL: WPA3 SAE refused */
    case 204: /* HANDSHAKE_TIMEOUT */
        return JOIN_RESULT_WRONG_PASSWORD;
    case 201: /* NO_AP_FOUND */
    case 210: /* NO_AP_FOUND_W_COMPATIBLE_SECURITY */
    case 211: /* NO_AP_FOUND_IN_AUTHMODE_THRESHOLD */
        return JOIN_RESULT_NOT_FOUND;
    default:
        return JOIN_RESULT_FAILED;
    }
}

uint8_t join_result_merge(uint8_t so_far, uint8_t this_try) {
    /* lower is clearer: wrong password (1) < not found (2) < failed (3); 0 = none yet */
    if(!so_far) return this_try;
    return this_try < so_far ? this_try : so_far;
}

uint32_t rejoin_delay_ms(uint8_t attempt) {
    if(attempt >= 5) return REJOIN_MAX_MS;
    uint32_t ms = 1000u << attempt;
    return ms < REJOIN_MAX_MS ? ms : REJOIN_MAX_MS;
}
