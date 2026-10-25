/*
 * Hardware-independent half of the ESP32 link layer: request decoding and
 * validation, baud negotiation, and script reassembly. Unit-tested on the host.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/proto_msg.h"

typedef struct {
    uint8_t type;
    union {
        ProtoHello hello;
        ProtoFlowCredit credit;
        ProtoNetCfg net_cfg;
        ProtoWifiJoin join;
        ProtoPingReq ping;
        ProtoSshConnect conn;
        ProtoHostkeyDecision decision;
        ProtoProfileRef ref;
        ProtoHostkeyDelete hk_delete;
        ProtoAuthPasswordMsg password;
        ProtoScriptBegin script_begin;
        ProtoScriptChunk script_chunk;
        ProtoScriptEnd script_end;
        ProtoKeyGen key_gen;
        ProtoKeyBlobBegin key_begin;
        ProtoKeyPassphrase passphrase;
        ProtoSecretForget forget;
        ProtoKeyLegacyDelete legacy_delete;
        ProtoSecEnable sec_enable;
    };
    /* EXEC_REQ: NUL-terminated copy of the command */
    char command[PROTO_STR_COMMAND + 1];
} LinkRequest;

/* Decode a reliable F->E frame. Returns ProtoErrNone, ProtoErrUnknownType or
 * ProtoErrBadPayload. Payload pointers inside `out` (script chunk data) point
 * into `payload`. */
uint16_t link_decode_request(uint8_t type, const uint8_t* payload, size_t len, LinkRequest* out);

/* HELLO: pick min(ours, theirs) from the supported rates. */
uint32_t link_choose_baud(uint32_t peer_max, uint32_t our_max);

/* Script reassembly into a caller-provided buffer (PSRAM on the ESP32). */
typedef struct {
    uint8_t* buf;
    size_t cap;
    uint32_t total;
    uint32_t received;
    uint32_t crc;
    bool active;
    char interp[PROTO_STR_INTERP + 1];
} ScriptAsm;

void script_asm_init(ScriptAsm* s, uint8_t* buf, size_t cap);
uint16_t script_asm_begin(ScriptAsm* s, const ProtoScriptBegin* b);
uint16_t script_asm_chunk(ScriptAsm* s, const ProtoScriptChunk* c);
/* On success the script is complete and verified; s->active stays true until reset. */
uint16_t script_asm_end(ScriptAsm* s, const ProtoScriptEnd* e);
void script_asm_reset(ScriptAsm* s);
/* The same reassembly for a key file (KEY_BLOB_*): no interpreter, up to s->cap bytes. */
uint16_t blob_asm_begin(ScriptAsm* s, uint32_t total);
/* Zero the whole buffer (a key was in it) and reset. */
void script_asm_wipe(ScriptAsm* s);
/* Interpreter allow-list check; also used to build "exec <interp> -s". */
bool script_interp_allowed(const char* interp);
