/*
 * The status LED's state machine (pure, host-tested): which pattern shows, and whether the
 * single LED is lit at a given millisecond. Patterns follow the usual single-LED device
 * convention and stay at a low duty cycle, so the LED costs next to nothing on the Flipper's
 * battery:
 *
 *   boot      one 150 ms flash at start
 *   linking   a 100 ms flash every second (10 %) while a link is coming up
 *   link up   on for 1 s when a link comes up, then dark (dark = all is well)
 *   ssh       heartbeat while a session is open: two 60 ms flashes every 2 s (6 %)
 *   error     a burst of 5 fast flashes: WiFi join failed, a link lost, an SSH session
 *             failed, or the last reset was a crash, watchdog or brownout
 *
 * Priority: error > boot > link up > ssh > linking > dark.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_PAT_DARK = 0,
    LED_PAT_BOOT,
    LED_PAT_LINKING,
    LED_PAT_LINK_UP,
    LED_PAT_SSH,
    LED_PAT_ERROR,
    LED_PAT_COUNT,
} LedPattern;

#define LED_BOOT_MS       400u /* 150 ms on, then dark until the next pattern */
#define LED_LINK_UP_MS    1000u
#define LED_ERROR_FLASHES 5u
#define LED_ERROR_STEP_MS 150u /* 60 ms on, 90 ms off */
#define LED_ERROR_MS      (LED_ERROR_FLASHES * LED_ERROR_STEP_MS)

/* What the board is doing, sampled every tick. */
typedef struct {
    bool joining; /* a WiFi join someone asked for */
    bool background_linking; /* rejoining the saved WiFi, or LAN cable in and no address yet */
    uint8_t links_up; /* interfaces with an IPv4 address */
    bool ssh_active; /* a session between connecting and closed */
    bool ssh_failed;
    uint32_t join_failures; /* failed WiFi joins since boot */
} LedInputs;

typedef struct {
    LedPattern pat;
    uint32_t pat_start;
    uint32_t boot_until, link_up_until, error_from, error_until;
    LedInputs prev;
    bool have_prev;
} LedSm;

/* crash_recovered: the last reset was a crash, watchdog or brownout (an error burst follows
 * the boot flash). */
void ledsm_init(LedSm* sm, uint32_t now_ms, bool crash_recovered);
/* An error burst from now (also restarts one already running). */
void ledsm_error(LedSm* sm, uint32_t now_ms);
/* One tick: picks the pattern for these inputs; returns whether the LED is lit now. */
bool ledsm_step(LedSm* sm, uint32_t now_ms, const LedInputs* in);
LedPattern ledsm_pattern(const LedSm* sm);
/* Whether `pat` is lit `t_ms` after it started (also drives the console's `led test`). */
bool ledsm_lit(LedPattern pat, uint32_t t_ms);
const char* ledsm_pattern_name(LedPattern pat);
