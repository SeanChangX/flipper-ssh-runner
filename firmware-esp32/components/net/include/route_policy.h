/* Which interface `auto` uses for a target, without ESP-IDF (host-tested in
 * test_host/test_route_policy.c). SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RouteNone,
    RouteLan,
    RouteWifi,
} RouteIface;

/* One interface as the selection sees it; addresses in the same byte order throughout. */
typedef struct {
    bool up; /* link and an address */
    uint32_t ip, mask, gw;
} RouteIfaceInfo;

/* As a host's routing table does: the interface whose subnet holds `target`; otherwise the
 * default route, LAN before WiFi (wired first), an interface with a gateway before one
 * without. `target` 0 (not resolved) skips the subnet match. */
RouteIface route_pick_auto(uint32_t target, const RouteIfaceInfo* lan, const RouteIfaceInfo* wifi);
