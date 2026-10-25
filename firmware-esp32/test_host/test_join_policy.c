/* WiFi join decisions (components/net/join_policy.c). SPDX-License-Identifier: GPL-3.0-only
 *
 * On the board, `wifi join` reported failure at once when the previous connect attempt
 * (the one started at boot) was still running: esp_wifi_connect() returned
 * ESP_ERR_WIFI_CONN, and the disconnect that ends the old attempt counted as a failed try. */
#include "join_policy.h"

#include <stdio.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if(!(c)) {                                                       \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while(0)

int main(void) {
    /* connect while the old attempt still runs: not a failure */
    CHECK(!join_connect_failed(false, true));
    CHECK(join_connect_failed(false, false));
    CHECK(!join_connect_failed(true, false));

    /* the old attempt ends (local leave): reconnect, no try used */
    uint8_t tries = 0;
    CHECK(join_on_disconnect(JOIN_REASON_LOCAL_LEAVE, &tries, 3) == JoinConnect);
    CHECK(tries == 0);
    /* real failures: three tries, then fail */
    CHECK(join_on_disconnect(15 /* 4-way handshake timeout */, &tries, 3) == JoinConnect);
    CHECK(join_on_disconnect(201 /* no AP found */, &tries, 3) == JoinConnect);
    CHECK(join_on_disconnect(15, &tries, 3) == JoinFail);
    CHECK(tries == 3);
    /* a local leave never ends a join, even after failed tries */
    tries = 2;
    CHECK(join_on_disconnect(JOIN_REASON_LOCAL_LEAVE, &tries, 3) == JoinConnect);
    CHECK(tries == 2);

    /* what a failed join says: WPA2 and WPA3 refusals, a missing network, the rest */
    CHECK(join_result_for(15) == JOIN_RESULT_WRONG_PASSWORD);
    CHECK(join_result_for(202) == JOIN_RESULT_WRONG_PASSWORD);
    CHECK(join_result_for(201) == JOIN_RESULT_NOT_FOUND);
    CHECK(join_result_for(212) == JOIN_RESULT_FAILED); /* found, but too weak */
    CHECK(join_result_for(200) == JOIN_RESULT_FAILED); /* beacon timeout */
    /* over the tries: a refused password is not hidden by a later timeout */
    uint8_t r = join_result_merge(0, join_result_for(202));
    r = join_result_merge(r, join_result_for(200));
    CHECK(r == JOIN_RESULT_WRONG_PASSWORD);
    CHECK(join_result_merge(join_result_for(200), join_result_for(201)) == JOIN_RESULT_NOT_FOUND);

    /* rejoining: 1, 2, 4, 8, 16 s, then every 30 s, never more */
    CHECK(rejoin_delay_ms(0) == 1000);
    CHECK(rejoin_delay_ms(1) == 2000);
    CHECK(rejoin_delay_ms(4) == 16000);
    CHECK(rejoin_delay_ms(5) == REJOIN_MAX_MS);
    CHECK(rejoin_delay_ms(255) == REJOIN_MAX_MS);

    printf("%s\n", failures ? "FAIL" : "PASS test_join_policy");
    return failures ? 1 : 0;
}
