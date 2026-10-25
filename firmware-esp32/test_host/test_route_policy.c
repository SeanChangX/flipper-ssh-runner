/* Interface choice for `auto` (components/net/route_policy.c). SPDX-License-Identifier: GPL-3.0-only
 *
 * On the board with LAN and WiFi both up, `auto` always took the LAN: a host on the WiFi
 * subnet (and its gateway) never answered, while `wifi` reached it. */
#include "route_policy.h"

#include <stdio.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if(!(c)) {                                                       \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while(0)

#define IP(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

int main(void) {
    const uint32_t m24 = IP(255, 255, 255, 0);
    RouteIfaceInfo lan = {true, IP(192, 168, 2, 200), m24, IP(192, 168, 2, 2)};
    RouteIfaceInfo wifi = {true, IP(192, 168, 60, 36), m24, IP(192, 168, 60, 1)};
    RouteIfaceInfo down = {false, 0, 0, 0};

    /* the subnet decides */
    CHECK(route_pick_auto(IP(192, 168, 60, 136), &lan, &wifi) == RouteWifi);
    CHECK(route_pick_auto(IP(192, 168, 60, 1), &lan, &wifi) == RouteWifi);
    CHECK(route_pick_auto(IP(192, 168, 2, 10), &lan, &wifi) == RouteLan);
    /* elsewhere: the default route, wired first */
    CHECK(route_pick_auto(IP(1, 1, 1, 1), &lan, &wifi) == RouteLan);
    CHECK(route_pick_auto(0, &lan, &wifi) == RouteLan);
    /* a LAN without a gateway cannot reach the internet: WiFi's gateway can */
    RouteIfaceInfo lan_nogw = lan;
    lan_nogw.gw = 0;
    CHECK(route_pick_auto(IP(1, 1, 1, 1), &lan_nogw, &wifi) == RouteWifi);
    CHECK(route_pick_auto(IP(192, 168, 2, 10), &lan_nogw, &wifi) == RouteLan);
    /* one interface up: that one, whatever the target */
    CHECK(route_pick_auto(IP(192, 168, 60, 136), &lan, &down) == RouteLan);
    CHECK(route_pick_auto(IP(192, 168, 2, 10), &down, &wifi) == RouteWifi);
    CHECK(route_pick_auto(IP(1, 1, 1, 1), &down, &down) == RouteNone);
    /* the same subnet on both: wired */
    RouteIfaceInfo wifi_same = {true, IP(192, 168, 2, 201), m24, IP(192, 168, 2, 2)};
    CHECK(route_pick_auto(IP(192, 168, 2, 10), &lan, &wifi_same) == RouteLan);
    /* an interface that is down never matches its old subnet */
    RouteIfaceInfo wifi_down = wifi;
    wifi_down.up = false;
    CHECK(route_pick_auto(IP(192, 168, 60, 136), &lan, &wifi_down) == RouteLan);

    printf("%s\n", failures ? "FAIL" : "PASS test_route_policy");
    return failures ? 1 : 0;
}
