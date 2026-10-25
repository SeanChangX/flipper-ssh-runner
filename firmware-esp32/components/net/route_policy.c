/* SPDX-License-Identifier: GPL-3.0-only */
#include "route_policy.h"

static bool on_link(uint32_t target, const RouteIfaceInfo* i) {
    return i->up && i->mask && (target & i->mask) == (i->ip & i->mask);
}

RouteIface
    route_pick_auto(uint32_t target, const RouteIfaceInfo* lan, const RouteIfaceInfo* wifi) {
    if(target) {
        /* both subnets may hold it (the same subnet on both): the wired one, like the rest */
        if(on_link(target, lan)) return RouteLan;
        if(on_link(target, wifi)) return RouteWifi;
    }
    if(lan->up && lan->gw) return RouteLan;
    if(wifi->up && wifi->gw) return RouteWifi;
    if(lan->up) return RouteLan;
    if(wifi->up) return RouteWifi;
    return RouteNone;
}
