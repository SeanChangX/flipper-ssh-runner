/* SPDX-License-Identifier: GPL-3.0-only */
#include "hostkey_policy.h"

#include <stdio.h>
#include <string.h>

void kh_init(KnownHosts* kh, const char* profile) {
    memset(kh, 0, sizeof(*kh));
    snprintf(kh->profile, sizeof(kh->profile), "%s", profile ? profile : "");
}

bool kh_contains(const KnownHosts* kh, const char* fp) {
    for(uint8_t i = 0; i < kh->count; i++)
        if(strcmp(kh->keys[i].fp, fp) == 0) return true;
    return false;
}

KhVerdict kh_check(const KnownHosts* kh, uint8_t policy, const char* fp) {
    if(policy == ProtoPolicyOff) return KhAccept;
    if(kh_contains(kh, fp)) return KhAccept;
    if(kh->count == 0) return KhPromptUnknown; /* strict also asks once */
    return policy == ProtoPolicyStrict ? KhReject : KhPromptMismatch;
}

static void push(KnownHosts* kh, const char* fp, const char* label) {
    if(kh->count >= KH_MAX_KEYS) {
        memmove(&kh->keys[0], &kh->keys[1], sizeof(KhKey) * (KH_MAX_KEYS - 1));
        kh->count--;
    }
    KhKey* k = &kh->keys[kh->count++];
    memset(k, 0, sizeof(*k));
    snprintf(k->fp, sizeof(k->fp), "%s", fp);
    snprintf(k->label, sizeof(k->label), "%s", label ? label : "");
}

bool kh_apply(KnownHosts* kh, uint8_t decision, const char* fp, const char* label, KhStore* store) {
    *store = KhStoreNone;
    switch(decision) {
    case ProtoDecisionOnce:
        return true;
    case ProtoDecisionAdd:
        if(!kh_contains(kh, fp)) {
            push(kh, fp, label);
            *store = KhStoreSave;
        }
        return true;
    case ProtoDecisionReplace:
        kh->count = 0;
        memset(kh->keys, 0, sizeof(kh->keys));
        push(kh, fp, label);
        *store = KhStoreSave;
        return true;
    case ProtoDecisionCancel:
    default:
        return false;
    }
}

bool kh_delete(KnownHosts* kh, const char* fp) {
    for(uint8_t i = 0; i < kh->count; i++) {
        if(strcmp(kh->keys[i].fp, fp) == 0) {
            memmove(&kh->keys[i], &kh->keys[i + 1], sizeof(KhKey) * (size_t)(kh->count - i - 1));
            kh->count--;
            memset(&kh->keys[kh->count], 0, sizeof(KhKey));
            return true;
        }
    }
    return false;
}

bool kh_may_authenticate(KhVerdict verdict, bool decided, uint8_t decision) {
    switch(verdict) {
    case KhAccept:
        return true;
    case KhReject:
        return false;
    case KhPromptUnknown:
    case KhPromptMismatch:
        return decided && decision != ProtoDecisionCancel;
    }
    return false;
}
