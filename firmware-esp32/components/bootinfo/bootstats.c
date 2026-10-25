/* Counting boots by reset reason (pure, host-tested). SPDX-License-Identifier: GPL-3.0-only */
#include "bootinfo.h"

void bootstats_count(BootStats* s, int reason) {
    s->boots++;
    switch(reason) {
    case BOOT_RST_POWERON:
        s->poweron++;
        break;
    case BOOT_RST_BROWNOUT:
    case BOOT_RST_PWR_GLITCH:
        s->brownout++;
        break;
    case BOOT_RST_PANIC:
    case BOOT_RST_CPU_LOCKUP:
        s->crash++;
        break;
    case BOOT_RST_INT_WDT:
    case BOOT_RST_TASK_WDT:
    case BOOT_RST_WDT:
        s->watchdog++;
        break;
    default:
        s->other++;
        break;
    }
    s->last = (uint8_t)(reason >= 0 && reason <= 255 ? reason : BOOT_RST_UNKNOWN);
}

const char* bootstats_reason_name(int reason) {
    static const char* const names[] = {
        "unknown",
        "power-on",
        "external pin",
        "software restart",
        "panic",
        "interrupt watchdog",
        "task watchdog",
        "watchdog",
        "deep sleep",
        "brownout",
        "SDIO",
        "USB",
        "JTAG",
        "eFuse error",
        "power glitch",
        "CPU lockup",
    };
    if(reason < 0 || reason >= (int)(sizeof(names) / sizeof(names[0]))) return "unknown";
    return names[reason];
}
