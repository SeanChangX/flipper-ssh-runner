/* Boot counting by reset reason. SPDX-License-Identifier: GPL-3.0-only */
#include <string.h>

#include "bootinfo.h"
#include "tt.h"

static void test_counts(void) {
    BootStats s;
    memset(&s, 0, sizeof(s));
    bootstats_count(&s, BOOT_RST_POWERON);
    bootstats_count(&s, BOOT_RST_SW);
    bootstats_count(&s, BOOT_RST_BROWNOUT);
    bootstats_count(&s, BOOT_RST_PWR_GLITCH); /* a dip too */
    bootstats_count(&s, BOOT_RST_TASK_WDT);
    bootstats_count(&s, BOOT_RST_INT_WDT);
    bootstats_count(&s, BOOT_RST_PANIC);
    bootstats_count(&s, BOOT_RST_USB);
    TT_EQ_INT(s.boots, 8);
    TT_EQ_INT(s.poweron, 1);
    TT_EQ_INT(s.brownout, 2);
    TT_EQ_INT(s.watchdog, 2);
    TT_EQ_INT(s.crash, 1);
    TT_EQ_INT(s.other, 2);
    TT_EQ_INT(s.last, BOOT_RST_USB);
    TT_EQ_INT(s.poweron + s.brownout + s.watchdog + s.crash + s.other, s.boots);
}

static void test_names(void) {
    TT_EQ_STR(bootstats_reason_name(BOOT_RST_BROWNOUT), "brownout");
    TT_EQ_STR(bootstats_reason_name(BOOT_RST_POWERON), "power-on");
    TT_EQ_STR(bootstats_reason_name(BOOT_RST_CPU_LOCKUP), "CPU lockup");
    TT_EQ_STR(bootstats_reason_name(99), "unknown");
    TT_EQ_STR(bootstats_reason_name(-1), "unknown");
    BootStats s;
    memset(&s, 0, sizeof(s));
    bootstats_count(&s, 300); /* out of range: counted, named unknown */
    TT_EQ_INT(s.other, 1);
    TT_EQ_INT(s.last, BOOT_RST_UNKNOWN);
}

static void test_faults(void) {
    TT_CHECK(bootstats_is_fault(BOOT_RST_BROWNOUT));
    TT_CHECK(bootstats_is_fault(BOOT_RST_PWR_GLITCH));
    TT_CHECK(bootstats_is_fault(BOOT_RST_TASK_WDT));
    TT_CHECK(bootstats_is_fault(BOOT_RST_PANIC));
    TT_CHECK(!bootstats_is_fault(BOOT_RST_POWERON));
    TT_CHECK(!bootstats_is_fault(BOOT_RST_SW));
    TT_CHECK(!bootstats_is_fault(BOOT_RST_USB));
    TT_CHECK(!bootstats_is_fault(-1));
}

int main(void) {
    TT_RUN(test_counts);
    TT_RUN(test_names);
    TT_RUN(test_faults);
    TT_END();
}
