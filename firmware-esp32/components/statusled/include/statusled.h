/*
 * The board's status LED: one LED that says boot / linking / link up / SSH session / error
 * (patterns in statusled_sm.h). The light itself is a driver behind StatusLedDriver: the
 * prototype's WS2812 on GPIO21, or rev 0.1's single green LED (D201) on a plain GPIO; the
 * Kconfig board choice picks one.
 * The RJ45 LEDs are the W5500's own and not touched here.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "statusled_sm.h"

typedef struct {
    const char* name;
    esp_err_t (*init)(void);
    void (*set)(bool on);
} StatusLedDriver;

typedef enum {
    STATUSLED_AUTO = 0, /* the default */
    STATUSLED_OFF = 1, /* dark for good (`led off`, kept in NVS) */
} StatusLedMode;

/* After net and SSH are up: reads the mode from NVS and starts the LED task. */
void statusled_start(void);
esp_err_t statusled_set_mode(StatusLedMode mode);
StatusLedMode statusled_mode(void);
LedPattern statusled_pattern(void);
const char* statusled_driver_name(void);
/* Lamp test: each pattern for 3 s in turn (shows also with `led off`). */
void statusled_test(void);
