/*
 * Host-key policy. Pure C, unit-tested on the host.
 *
 *  - Keys are pinned per profile (alias, or "host:port" for Quick Connect),
 *    like OpenSSH HostKeyAlias, never per IP.
 *  - A profile may trust several keys (same-IP fleets).
 *  - Policy per profile: prompt (default), strict, off.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/proto_msg.h"

#define KH_MAX_KEYS PROTO_MAX_HOSTKEYS

typedef struct {
    char fp[PROTO_STR_FP + 1];
    char label[PROTO_STR_LABEL + 1];
} KhKey;

typedef struct {
    char profile[PROTO_STR_PROFILE + 1];
    uint8_t count;
    KhKey keys[KH_MAX_KEYS];
} KnownHosts;

typedef enum {
    KhAccept, /* trusted (or policy off): continue silently */
    KhPromptUnknown, /* ask: trust & save / cancel */
    KhPromptMismatch, /* ask: connect once / trust & add / replace all / cancel */
    KhReject, /* strict + key not trusted: refuse, no prompt */
} KhVerdict;

typedef enum {
    KhStoreNone, /* nothing to persist */
    KhStoreSave, /* `kh` changed: persist it */
} KhStore;

void kh_init(KnownHosts* kh, const char* profile);
bool kh_contains(const KnownHosts* kh, const char* fp);

/* What to do when the server presents `fp`. */
KhVerdict kh_check(const KnownHosts* kh, uint8_t policy, const char* fp);

/* Apply the user's decision to a prompt. Returns whether the connection may proceed;
 * *store tells the caller to persist `kh`. "once" never stores. When the set is full,
 * "add" drops the oldest key. */
bool kh_apply(KnownHosts* kh, uint8_t decision, const char* fp, const char* label, KhStore* store);

bool kh_delete(KnownHosts* kh, const char* fp);

/* The password may be sent only after the host-key step has said yes. Encoded here so
 * the rule has one home and a unit test: true when `verdict` + `decision` allow auth. */
bool kh_may_authenticate(KhVerdict verdict, bool decided, uint8_t decision);
