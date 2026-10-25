/* Start-up memory check (core/heapcheck.c). SPDX-License-Identifier: GPL-3.0-only */
#include "heapcheck.h"
#include "tt.h"

static const HeapNeeds n = {.need = 50000, .margin = 8000, .biggest = 6000};

static void test_enough(void) {
    TT_CHECK(heap_start_ok(60000, 20000, &n));
    TT_CHECK(heap_start_ok(58000, 6000, &n)); /* exactly need + margin, block exactly biggest */
}

static void test_short_total(void) {
    TT_CHECK(!heap_start_ok(57999, 20000, &n)); /* one byte under need + margin */
    TT_CHECK(!heap_start_ok(8000, 8000, &n)); /* the crash case: a few KB left */
}

static void test_fragmented(void) {
    TT_CHECK(!heap_start_ok(90000, 5999, &n)); /* plenty in total, no block for the buffer */
}

int main(void) {
    TT_RUN(test_enough);
    TT_RUN(test_short_total);
    TT_RUN(test_fragmented);
    TT_END();
}
