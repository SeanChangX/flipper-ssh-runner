/* Status LED state machine. SPDX-License-Identifier: GPL-3.0-only */
#include <string.h>

#include "statusled_sm.h"
#include "tt.h"

static LedInputs none(void) {
    LedInputs in;
    memset(&in, 0, sizeof(in));
    return in;
}

/* Run from `from` to `to` (exclusive) in 10 ms ticks; returns the lit ticks. */
static unsigned run(LedSm* sm, uint32_t from, uint32_t to, const LedInputs* in) {
    unsigned lit = 0;
    for(uint32_t t = from; t != to; t += 10)
        if(ledsm_step(sm, t, in)) lit++;
    return lit;
}

static void test_boot_flash_then_dark(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    TT_CHECK(ledsm_step(&sm, 0, &in));
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_BOOT);
    TT_EQ_INT(run(&sm, 10, LED_BOOT_MS, &in), 14); /* 150 ms on in all */
    TT_CHECK(!ledsm_step(&sm, LED_BOOT_MS, &in));
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_DARK);
    TT_EQ_INT(run(&sm, LED_BOOT_MS + 10, 10000, &in), 0);
}

static void test_crash_recovered_bursts_after_boot(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, true);
    run(&sm, 0, LED_BOOT_MS, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_BOOT);
    TT_EQ_INT(run(&sm, LED_BOOT_MS, LED_BOOT_MS + LED_ERROR_MS, &in), LED_ERROR_FLASHES * 6);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_ERROR);
    ledsm_step(&sm, LED_BOOT_MS + LED_ERROR_MS, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_DARK);
}

static void test_linking_slow_blink_then_link_up(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    run(&sm, 0, 1000, &in);
    in.background_linking = true; /* cable in, waiting for DHCP */
    TT_EQ_INT(run(&sm, 1000, 4000, &in), 30); /* 100 ms of every second */
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_LINKING);
    in.links_up = 1;
    in.background_linking = false;
    TT_EQ_INT(run(&sm, 4000, 5000, &in), 100); /* on for the whole second */
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_LINK_UP);
    TT_EQ_INT(run(&sm, 5000, 8000, &in), 0); /* then dark */
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_DARK);
}

static void test_background_rejoin_hidden_while_a_link_is_up(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    in.links_up = 1;
    run(&sm, 0, 3000, &in);
    in.background_linking = true; /* saved WiFi out of range, LAN fine */
    TT_EQ_INT(run(&sm, 3000, 6000, &in), 0);
    in.joining = true; /* a join someone asked for always shows */
    TT_EQ_INT(run(&sm, 6000, 9000, &in), 30);
}

static void test_ssh_heartbeat(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    in.links_up = 1;
    run(&sm, 0, 3000, &in);
    in.ssh_active = true;
    TT_EQ_INT(run(&sm, 3000, 7000, &in), 24); /* two 60 ms flashes per 2 s */
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_SSH);
    TT_CHECK(
        ledsm_lit(LED_PAT_SSH, 0) && !ledsm_lit(LED_PAT_SSH, 100) && ledsm_lit(LED_PAT_SSH, 200));
    TT_CHECK(!ledsm_lit(LED_PAT_SSH, 1000));
    in.ssh_active = false;
    ledsm_step(&sm, 7000, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_DARK);
}

static void test_errors_burst(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    in.links_up = 2;
    run(&sm, 0, 3000, &in);
    in.links_up = 1; /* a link lost */
    TT_EQ_INT(run(&sm, 3000, 3000 + LED_ERROR_MS, &in), LED_ERROR_FLASHES * 6);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_ERROR);
    TT_EQ_INT(run(&sm, 3000 + LED_ERROR_MS, 6000, &in), 0);

    in.join_failures = 1; /* a WiFi join failed */
    ledsm_step(&sm, 6000, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_ERROR);
    run(&sm, 6010, 7000, &in);

    in.ssh_active = true;
    run(&sm, 7000, 8000, &in);
    in.ssh_active = false;
    in.ssh_failed = true; /* a session failed: one burst, not one per tick */
    TT_EQ_INT(run(&sm, 8000, 8000 + LED_ERROR_MS, &in), LED_ERROR_FLASHES * 6);
    TT_EQ_INT(run(&sm, 8000 + LED_ERROR_MS, 12000, &in), 0);
}

static void test_rejoin_drop_is_no_error(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    in.links_up = 2;
    run(&sm, 0, 3000, &in);
    in.joining = true; /* `wifi rejoin`: the WiFi address goes while it joins */
    TT_EQ_INT(run(&sm, 3000, 3100, &in), 10);
    in.links_up = 1;
    /* the linking blink goes on (flashes at +1 s and +2 s), no error burst */
    TT_EQ_INT(run(&sm, 3100, 6000, &in), 20);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_LINKING);
    in.joining = false;
    in.links_up = 2;
    ledsm_step(&sm, 6000, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_LINK_UP);
}

static void test_error_wins_and_restarts(void) {
    LedSm sm;
    LedInputs in = none();
    ledsm_init(&sm, 0, false);
    in.ssh_active = true;
    run(&sm, 0, 1000, &in);
    ledsm_error(&sm, 1000);
    TT_CHECK(ledsm_step(&sm, 1000, &in));
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_ERROR);
    run(&sm, 1010, 1300, &in);
    ledsm_error(&sm, 1300); /* a second error mid-burst starts a full burst */
    TT_EQ_INT(run(&sm, 1300, 1300 + LED_ERROR_MS, &in), LED_ERROR_FLASHES * 6);
    ledsm_step(&sm, 1300 + LED_ERROR_MS, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_SSH);
}

static void test_clock_wrap(void) {
    LedSm sm;
    LedInputs in = none();
    uint32_t t0 = 0xFFFFFF00u; /* 49.7 days of uptime: the ms clock wraps mid-boot */
    ledsm_init(&sm, t0, false);
    TT_CHECK(ledsm_step(&sm, t0, &in));
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_BOOT);
    ledsm_step(&sm, t0 + LED_BOOT_MS, &in);
    TT_EQ_INT(ledsm_pattern(&sm), LED_PAT_DARK);
}

static void test_low_duty(void) {
    /* the repeating patterns stay at or below 10 % */
    const LedPattern pats[] = {LED_PAT_LINKING, LED_PAT_SSH};
    for(unsigned i = 0; i < 2; i++) {
        unsigned lit = 0;
        for(uint32_t t = 0; t < 60000; t++)
            if(ledsm_lit(pats[i], t)) lit++;
        TT_CHECK(lit <= 6000);
    }
    TT_CHECK(!ledsm_lit(LED_PAT_DARK, 0));
    TT_CHECK(!ledsm_lit(LED_PAT_ERROR, LED_ERROR_MS));
    TT_EQ_STR(ledsm_pattern_name(LED_PAT_SSH), "ssh");
    TT_EQ_STR(ledsm_pattern_name(LED_PAT_COUNT), "unknown");
}

int main(void) {
    TT_RUN(test_boot_flash_then_dark);
    TT_RUN(test_crash_recovered_bursts_after_boot);
    TT_RUN(test_linking_slow_blink_then_link_up);
    TT_RUN(test_background_rejoin_hidden_while_a_link_is_up);
    TT_RUN(test_ssh_heartbeat);
    TT_RUN(test_errors_burst);
    TT_RUN(test_rejoin_drop_is_no_error);
    TT_RUN(test_error_wins_and_restarts);
    TT_RUN(test_clock_wrap);
    TT_RUN(test_low_duty);
    TT_END();
}
