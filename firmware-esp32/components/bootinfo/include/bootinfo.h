/*
 * Why the board last started, and how often it has: the reset reason of every boot counted
 * in NVS, for power tests (a brownout or a watchdog during a test shows up afterwards in
 * `status`, also after the board was unplugged).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

/* esp_reset_reason_t's values (ESP-IDF 5.5), so the counting is testable on a host. */
enum {
    BOOT_RST_UNKNOWN = 0,
    BOOT_RST_POWERON,
    BOOT_RST_EXT,
    BOOT_RST_SW,
    BOOT_RST_PANIC,
    BOOT_RST_INT_WDT,
    BOOT_RST_TASK_WDT,
    BOOT_RST_WDT,
    BOOT_RST_DEEPSLEEP,
    BOOT_RST_BROWNOUT,
    BOOT_RST_SDIO,
    BOOT_RST_USB,
    BOOT_RST_JTAG,
    BOOT_RST_EFUSE,
    BOOT_RST_PWR_GLITCH,
    BOOT_RST_CPU_LOCKUP,
};

typedef struct {
    uint32_t boots; /* every start since the counters were first written */
    uint32_t poweron;
    uint32_t brownout; /* brownout or power glitch: the supply dipped */
    uint32_t crash; /* panic, CPU lockup */
    uint32_t watchdog; /* interrupt, task or other watchdog */
    uint32_t other; /* software restart, USB, external, ... */
    uint8_t last; /* this boot's reason, BOOT_RST_* */
} BootStats;

/* Pure: count one boot with its reason. */
void bootstats_count(BootStats* s, int reason);
/* Pure: "brownout", "power-on", ... for BOOT_RST_*. */
const char* bootstats_reason_name(int reason);

/* ESP: read the counters from NVS, count this boot (esp_reset_reason()), write them back.
 * Called once at start, after NVS is up; a failure leaves the counters at zero. */
void bootinfo_init(void);
const BootStats* bootinfo_get(void);
