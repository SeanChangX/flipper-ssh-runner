/* Minimal unit-test helpers shared by the host test suites. SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tt_failures = 0;
static int tt_checks = 0;

#define TT_CHECK(cond)                                                               \
    do {                                                                             \
        tt_checks++;                                                                 \
        if(!(cond)) {                                                                \
            tt_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while(0)

#define TT_EQ_INT(a, b)                                           \
    do {                                                          \
        long long tt_a_ = (long long)(a), tt_b_ = (long long)(b); \
        tt_checks++;                                              \
        if(tt_a_ != tt_b_) {                                      \
            tt_failures++;                                        \
            fprintf(                                              \
                stderr,                                           \
                "%s:%d: %s == %s failed: %lld != %lld\n",         \
                __FILE__,                                         \
                __LINE__,                                         \
                #a,                                               \
                #b,                                               \
                tt_a_,                                            \
                tt_b_);                                           \
        }                                                         \
    } while(0)

#define TT_EQ_STR(a, b)                                       \
    do {                                                      \
        const char *tt_a_ = (a), *tt_b_ = (b);                \
        tt_checks++;                                          \
        if(!tt_a_ || !tt_b_ || strcmp(tt_a_, tt_b_) != 0) {   \
            tt_failures++;                                    \
            fprintf(                                          \
                stderr,                                       \
                "%s:%d: %s == %s failed: \"%s\" != \"%s\"\n", \
                __FILE__,                                     \
                __LINE__,                                     \
                #a,                                           \
                #b,                                           \
                tt_a_ ? tt_a_ : "(null)",                     \
                tt_b_ ? tt_b_ : "(null)");                    \
        }                                                     \
    } while(0)

#define TT_RUN(fn)                                                           \
    do {                                                                     \
        int tt_before_ = tt_failures;                                        \
        fn();                                                                \
        printf("%s %s\n", tt_failures == tt_before_ ? "PASS" : "FAIL", #fn); \
    } while(0)

#define TT_END()                                                    \
    do {                                                            \
        printf("%d checks, %d failures\n", tt_checks, tt_failures); \
        return tt_failures ? 1 : 0;                                 \
    } while(0)

/* Deterministic xorshift PRNG so fuzz runs are reproducible. */
static uint32_t tt_rng_state = 0x12345678u;
static inline void tt_seed(uint32_t s) {
    tt_rng_state = s ? s : 1u;
}
static inline uint32_t tt_rand(void) {
    uint32_t x = tt_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return tt_rng_state = x;
}
