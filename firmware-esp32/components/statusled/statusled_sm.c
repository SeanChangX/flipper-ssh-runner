/* Status LED patterns (pure, host-tested; see statusled_sm.h). SPDX-License-Identifier: GPL-3.0-only */
#include "statusled_sm.h"

#include <string.h>

/* Wrap-safe "a is before b" on a millisecond clock. */
static bool before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

void ledsm_init(LedSm* sm, uint32_t now_ms, bool crash_recovered) {
    memset(sm, 0, sizeof(*sm));
    sm->pat = LED_PAT_BOOT;
    sm->pat_start = now_ms;
    sm->boot_until = now_ms + LED_BOOT_MS;
    sm->link_up_until = now_ms;
    sm->error_from = sm->error_until = now_ms;
    if(crash_recovered) {
        sm->error_from = sm->boot_until;
        sm->error_until = sm->boot_until + LED_ERROR_MS;
    }
}

void ledsm_error(LedSm* sm, uint32_t now_ms) {
    sm->error_from = now_ms;
    sm->error_until = now_ms + LED_ERROR_MS;
}

static void edges(LedSm* sm, uint32_t now, const LedInputs* in) {
    if(!sm->have_prev) return;
    const LedInputs* p = &sm->prev;
    if(in->links_up > p->links_up) sm->link_up_until = now + LED_LINK_UP_MS;
    /* a join someone asked for drops the old WiFi address on purpose: not a lost link */
    bool lost = in->links_up < p->links_up && !in->joining && !p->joining;
    if(lost || in->join_failures != p->join_failures || (in->ssh_failed && !p->ssh_failed))
        ledsm_error(sm, now);
}

static LedPattern pick(const LedSm* sm, uint32_t now, const LedInputs* in) {
    if(!before(now, sm->error_from) && before(now, sm->error_until)) return LED_PAT_ERROR;
    if(before(now, sm->boot_until)) return LED_PAT_BOOT;
    if(before(now, sm->link_up_until)) return LED_PAT_LINK_UP;
    if(in->ssh_active) return LED_PAT_SSH;
    if(in->joining || (in->background_linking && in->links_up == 0)) return LED_PAT_LINKING;
    return LED_PAT_DARK;
}

bool ledsm_step(LedSm* sm, uint32_t now_ms, const LedInputs* in) {
    edges(sm, now_ms, in);
    sm->prev = *in;
    sm->have_prev = true;
    LedPattern pat = pick(sm, now_ms, in);
    if(pat != sm->pat) {
        sm->pat = pat;
        sm->pat_start = now_ms;
    }
    /* an error burst counts from its own start, so a restarted burst starts over */
    uint32_t start = pat == LED_PAT_ERROR ? sm->error_from : sm->pat_start;
    return ledsm_lit(pat, now_ms - start);
}

LedPattern ledsm_pattern(const LedSm* sm) {
    return sm->pat;
}

bool ledsm_lit(LedPattern pat, uint32_t t) {
    switch(pat) {
    case LED_PAT_BOOT:
        return t < 150;
    case LED_PAT_LINKING:
        return t % 1000 < 100;
    case LED_PAT_LINK_UP:
        return t < LED_LINK_UP_MS;
    case LED_PAT_SSH: {
        uint32_t p = t % 2000;
        return p < 60 || (p >= 180 && p < 240);
    }
    case LED_PAT_ERROR:
        return t < LED_ERROR_MS && t % LED_ERROR_STEP_MS < 60;
    default:
        return false;
    }
}

const char* ledsm_pattern_name(LedPattern pat) {
    static const char* const names[LED_PAT_COUNT] = {
        "dark", "boot", "linking", "link up", "ssh", "error"};
    return (unsigned)pat < LED_PAT_COUNT ? names[pat] : "unknown";
}
