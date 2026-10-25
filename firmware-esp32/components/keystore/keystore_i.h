/* Keystore internals shared by keystore.c and keystore_sec.c. SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "nvs.h"
#include "sdkconfig.h"

#define KS_NS "fes" /* every keystore entry lives in this namespace */

/* Plaintext keystore partition. The virtual-eFuse test build keeps its own copy, so
 * the migration tests never touch the board's real secrets (WiFi credentials included). */
#if CONFIG_EFUSE_VIRTUAL
#define KS_PLAIN_PART "nvs_test"
#else
#define KS_PLAIN_PART NVS_DEFAULT_PART_NAME
#endif
#define KS_SEC_PART "fes_sec" /* encrypted keystore after "Protect secrets" */

/* Partition the keystore uses now: KS_PLAIN_PART, or KS_SEC_PART once protected. */
const char* keystore_part(void);

/* Boot: picks the partition, and resumes an interrupted migration after a power loss. */
esp_err_t keystore_sec_boot(void);
