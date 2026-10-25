/* Host-key policy semantics, on the host.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "hostkey_policy.h"
#include "tt.h"

static const char* FP_A = "SHA256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char* FP_B = "SHA256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char* FP_C = "SHA256:ccccccccccccccccccccccccccccccccccccccccccc";

/* Two different targets at the same IP, `prompt`: offers to continue on the
 * mismatch; Trust & add stores both; later connections to either are silent. */
static void test_prompt_same_ip_fleet(void) {
    KnownHosts kh;
    kh_init(&kh, "robot");
    KhStore st;
    TT_EQ_INT(kh_check(&kh, ProtoPolicyPrompt, FP_A), KhPromptUnknown);
    TT_CHECK(kh_apply(&kh, ProtoDecisionAdd, FP_A, "a", &st));
    TT_EQ_INT(st, KhStoreSave);
    TT_EQ_INT(kh_check(&kh, ProtoPolicyPrompt, FP_A), KhAccept);
    TT_EQ_INT(kh_check(&kh, ProtoPolicyPrompt, FP_B), KhPromptMismatch);
    TT_CHECK(kh_apply(&kh, ProtoDecisionAdd, FP_B, "b", &st));
    TT_EQ_INT(kh.count, 2);
    TT_EQ_INT(kh_check(&kh, ProtoPolicyPrompt, FP_A), KhAccept);
    TT_EQ_INT(kh_check(&kh, ProtoPolicyPrompt, FP_B), KhAccept);
}

/* `strict` refuses the second target. */
static void test_strict(void) {
    KnownHosts kh;
    kh_init(&kh, "nas");
    KhStore st;
    TT_EQ_INT(kh_check(&kh, ProtoPolicyStrict, FP_A), KhPromptUnknown); /* asks once */
    TT_CHECK(kh_apply(&kh, ProtoDecisionAdd, FP_A, "", &st));
    TT_EQ_INT(kh_check(&kh, ProtoPolicyStrict, FP_A), KhAccept);
    TT_EQ_INT(kh_check(&kh, ProtoPolicyStrict, FP_B), KhReject);
    TT_CHECK(!kh_may_authenticate(KhReject, false, 0));
    /* the only way out is deleting the key */
    TT_CHECK(kh_delete(&kh, FP_A));
    TT_EQ_INT(kh_check(&kh, ProtoPolicyStrict, FP_B), KhPromptUnknown);
}

/* `off` never prompts. */
static void test_off(void) {
    KnownHosts kh;
    kh_init(&kh, "lab");
    TT_EQ_INT(kh_check(&kh, ProtoPolicyOff, FP_A), KhAccept);
    TT_CHECK(kh_apply(&kh, ProtoDecisionAdd, FP_A, "", &(KhStore){0}));
    TT_EQ_INT(kh_check(&kh, ProtoPolicyOff, FP_B), KhAccept);
}

static void test_once_and_replace(void) {
    KnownHosts kh;
    kh_init(&kh, "x");
    KhStore st;
    kh_apply(&kh, ProtoDecisionAdd, FP_A, "", &st);
    TT_CHECK(kh_apply(&kh, ProtoDecisionOnce, FP_B, "", &st));
    TT_EQ_INT(st, KhStoreNone);
    TT_EQ_INT(kh.count, 1); /* once never stores */
    TT_CHECK(kh_apply(&kh, ProtoDecisionReplace, FP_C, "new", &st));
    TT_EQ_INT(st, KhStoreSave);
    TT_EQ_INT(kh.count, 1);
    TT_EQ_STR(kh.keys[0].fp, FP_C);
    TT_EQ_STR(kh.keys[0].label, "new");
    TT_CHECK(!kh_apply(&kh, ProtoDecisionCancel, FP_A, "", &st));
    TT_EQ_INT(st, KhStoreNone);
    TT_CHECK(!kh_apply(&kh, 99, FP_A, "", &st)); /* unknown decision = cancel */
    /* adding a key already present stores nothing */
    TT_CHECK(kh_apply(&kh, ProtoDecisionAdd, FP_C, "", &st));
    TT_EQ_INT(st, KhStoreNone);
}

static void test_full_set_drops_oldest(void) {
    KnownHosts kh;
    kh_init(&kh, "fleet");
    KhStore st;
    char fp[64];
    for(int i = 0; i < KH_MAX_KEYS + 2; i++) {
        snprintf(fp, sizeof(fp), "SHA256:%043d", i);
        kh_apply(&kh, ProtoDecisionAdd, fp, "", &st);
    }
    TT_EQ_INT(kh.count, KH_MAX_KEYS);
    snprintf(fp, sizeof(fp), "SHA256:%043d", 0);
    TT_CHECK(!kh_contains(&kh, fp));
    snprintf(fp, sizeof(fp), "SHA256:%043d", KH_MAX_KEYS + 1);
    TT_CHECK(kh_contains(&kh, fp));
    TT_CHECK(!kh_delete(&kh, "SHA256:nope"));
}

/* The password is never sent before the host-key decision. */
static void test_password_after_decision(void) {
    TT_CHECK(kh_may_authenticate(KhAccept, false, 0));
    TT_CHECK(!kh_may_authenticate(KhPromptUnknown, false, ProtoDecisionAdd)); /* undecided */
    TT_CHECK(!kh_may_authenticate(KhPromptMismatch, true, ProtoDecisionCancel));
    TT_CHECK(kh_may_authenticate(KhPromptMismatch, true, ProtoDecisionOnce));
    TT_CHECK(kh_may_authenticate(KhPromptUnknown, true, ProtoDecisionAdd));
}

static void test_profiles_are_independent(void) {
    /* robot-a and robot-b point at the same IP but never conflict */
    KnownHosts a, b;
    KhStore st;
    kh_init(&a, "robot-a");
    kh_init(&b, "robot-b");
    kh_apply(&a, ProtoDecisionAdd, FP_A, "", &st);
    kh_apply(&b, ProtoDecisionAdd, FP_B, "", &st);
    TT_EQ_INT(kh_check(&a, ProtoPolicyStrict, FP_A), KhAccept);
    TT_EQ_INT(kh_check(&b, ProtoPolicyStrict, FP_B), KhAccept);
    TT_EQ_INT(kh_check(&a, ProtoPolicyStrict, FP_B), KhReject);
}

int main(void) {
    TT_RUN(test_prompt_same_ip_fleet);
    TT_RUN(test_strict);
    TT_RUN(test_off);
    TT_RUN(test_once_and_replace);
    TT_RUN(test_full_set_drops_oldest);
    TT_RUN(test_password_after_decision);
    TT_RUN(test_profiles_are_independent);
    TT_END();
}
