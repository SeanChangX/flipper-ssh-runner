/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "ssh_client.h"

/* "fes 0.2.0": the firmware's short name and PROJECT_VER (firmware-esp32/CMakeLists.txt) */
const char* app_fw_version(void);

/* Flipper link front-end (app.c) */
void app_link_start(void);
void app_net_status_changed(void* ctx);

/* The staged key file, shared by the link and the console. It lives in one
 * static internal-RAM buffer that is zeroed after use, after an error, and 30 s after
 * KEY_BLOB_END if no connect takes it. Return values are ProtoErrCode. */
uint16_t app_key_begin(uint32_t total);
uint16_t app_key_chunk(const ProtoScriptChunk* chunk);
uint16_t app_key_end(const ProtoScriptEnd* end);
void app_key_wipe(void);
/* Debug check: bytes of the staging buffer that are not zero (0 after every use). */
size_t app_key_nonzero(void);
/* ssh_client_connect() with the staged key for auth = key; wipes it otherwise. */
esp_err_t app_ssh_connect(const ProtoSshConnect* req, const SshSink* sink, uint32_t credit);

/* USB console front-end (console.c) */
void console_start(void);
/* Sink that prints SSH events to the console. */
extern const SshSink console_sink;
