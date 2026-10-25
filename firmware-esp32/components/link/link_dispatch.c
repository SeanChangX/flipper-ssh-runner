/* SPDX-License-Identifier: GPL-3.0-only */
#include "link_dispatch.h"

#include <string.h>

uint16_t link_decode_request(uint8_t type, const uint8_t* p, size_t len, LinkRequest* out) {
    memset(out, 0, sizeof(*out));
    out->type = type;
    bool ok;
    switch(type) {
    case PROTO_T_HELLO:
        ok = proto_dec_hello(p, len, &out->hello);
        break;
    case PROTO_T_FLOW_CREDIT:
        ok = proto_dec_flow_credit(p, len, &out->credit);
        break;
    case PROTO_T_CANCEL:
    case PROTO_T_NET_GET_STATUS:
    case PROTO_T_WIFI_SCAN:
    case PROTO_T_SSH_DISCONNECT:
    case PROTO_T_KEY_LEGACY_EXPORT:
    case PROTO_T_SEC_GET_STATUS:
        ok = len == 0;
        break;
    case PROTO_T_NET_SET_CFG:
        ok = proto_dec_net_cfg(p, len, &out->net_cfg);
        break;
    case PROTO_T_WIFI_JOIN:
        ok = proto_dec_wifi_join(p, len, &out->join) && out->join.ssid[0];
        break;
    case PROTO_T_PING_REQ:
        ok = proto_dec_ping_req(p, len, &out->ping) && out->ping.host[0] && out->ping.count;
        break;
    case PROTO_T_SSH_CONNECT:
        ok = proto_dec_ssh_connect(p, len, &out->conn) && out->conn.profile[0] &&
             out->conn.user[0] && out->conn.host[0];
        break;
    case PROTO_T_HOSTKEY_DECISION:
        ok = proto_dec_hostkey_decision(p, len, &out->decision);
        break;
    case PROTO_T_HOSTKEY_LIST:
        ok = proto_dec_profile_ref(p, len, &out->ref) && out->ref.profile[0];
        break;
    case PROTO_T_SECRET_FORGET:
        ok = proto_dec_secret_forget(p, len, &out->forget);
        break;
    case PROTO_T_SEC_ENABLE:
        /* a wrong or empty word is a bad payload: answered before anything is touched */
        ok = proto_dec_sec_enable(p, len, &out->sec_enable) &&
             out->sec_enable.tier == ProtoSecTierProtect &&
             proto_sec_word_ok(out->sec_enable.word);
        break;
    case PROTO_T_HOSTKEY_DELETE:
        ok = proto_dec_hostkey_delete(p, len, &out->hk_delete) && out->hk_delete.profile[0];
        break;
    case PROTO_T_AUTH_PASSWORD:
        ok = proto_dec_auth_password(p, len, &out->password);
        break;
    case PROTO_T_EXEC_REQ: {
        ProtoExecReq r;
        ok = proto_dec_exec_req(p, len, &r);
        if(ok) {
            memcpy(out->command, r.command, r.len);
            out->command[r.len] = '\0';
        }
        break;
    }
    case PROTO_T_SCRIPT_BEGIN:
        ok = proto_dec_script_begin(p, len, &out->script_begin) &&
             script_interp_allowed(out->script_begin.interp);
        break;
    case PROTO_T_SCRIPT_CHUNK:
    case PROTO_T_KEY_BLOB_CHUNK:
        ok = proto_dec_script_chunk(p, len, &out->script_chunk) &&
             out->script_chunk.len <= PROTO_SCRIPT_CHUNK;
        break;
    case PROTO_T_SCRIPT_END:
    case PROTO_T_KEY_BLOB_END:
        ok = proto_dec_script_end(p, len, &out->script_end);
        break;
    case PROTO_T_KEY_GEN:
        ok = proto_dec_key_gen(p, len, &out->key_gen);
        break;
    case PROTO_T_KEY_BLOB_BEGIN:
        ok = proto_dec_key_blob_begin(p, len, &out->key_begin);
        break;
    case PROTO_T_KEY_PASSPHRASE:
        ok = proto_dec_key_passphrase(p, len, &out->passphrase);
        break;
    case PROTO_T_KEY_LEGACY_DELETE:
        ok = proto_dec_key_legacy_delete(p, len, &out->legacy_delete);
        break;
    default:
        return ProtoErrUnknownType; /* includes the reserved SHELL_* types in v0 */
    }
    return ok ? ProtoErrNone : ProtoErrBadPayload;
}

uint32_t link_choose_baud(uint32_t peer_max, uint32_t our_max) {
    static const uint32_t rates[] = {921600, 460800, 230400, 115200};
    uint32_t limit = peer_max < our_max ? peer_max : our_max;
    for(size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if(rates[i] <= limit) return rates[i];
    return PROTO_BAUD_INITIAL;
}

void script_asm_init(ScriptAsm* s, uint8_t* buf, size_t cap) {
    memset(s, 0, sizeof(*s));
    s->buf = buf;
    s->cap = cap;
}

void script_asm_reset(ScriptAsm* s) {
    s->active = false;
    s->total = s->received = s->crc = 0;
    s->interp[0] = '\0';
}

uint16_t script_asm_begin(ScriptAsm* s, const ProtoScriptBegin* b) {
    if(b->total == 0 || b->total > PROTO_SCRIPT_MAX || b->total > s->cap) return ProtoErrTooLarge;
    if(!script_interp_allowed(b->interp)) return ProtoErrBadPayload;
    s->active = true;
    s->total = b->total;
    s->received = 0;
    s->crc = 0;
    memcpy(s->interp, b->interp, sizeof(s->interp));
    return ProtoErrNone;
}

uint16_t blob_asm_begin(ScriptAsm* s, uint32_t total) {
    if(total == 0 || total > s->cap) return ProtoErrTooLarge;
    s->active = true;
    s->total = total;
    s->received = 0;
    s->crc = 0;
    s->interp[0] = '\0';
    return ProtoErrNone;
}

void script_asm_wipe(ScriptAsm* s) {
    volatile uint8_t* p = s->buf;
    for(size_t i = 0; i < s->cap; i++)
        p[i] = 0;
    script_asm_reset(s);
}

uint16_t script_asm_chunk(ScriptAsm* s, const ProtoScriptChunk* c) {
    if(!s->active) return ProtoErrState;
    if(c->offset != s->received) return ProtoErrBadPayload; /* must be contiguous */
    if((uint64_t)c->offset + c->len > s->total) return ProtoErrTooLarge;
    memcpy(&s->buf[c->offset], c->data, c->len);
    s->received += c->len;
    s->crc = proto_crc32(c->data, c->len, s->crc);
    return ProtoErrNone;
}

uint16_t script_asm_end(ScriptAsm* s, const ProtoScriptEnd* e) {
    if(!s->active) return ProtoErrState;
    if(s->received != s->total || e->crc32 != s->crc) {
        script_asm_reset(s);
        return ProtoErrBadPayload;
    }
    return ProtoErrNone;
}

bool script_interp_allowed(const char* interp) {
    static const char* const ok[] = {"sh", "bash", "ash", "dash", "zsh", "ksh"};
    for(size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if(strcmp(interp, ok[i]) == 0) return true;
    return false;
}
