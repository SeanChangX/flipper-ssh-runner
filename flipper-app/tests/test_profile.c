/* Profiles, targets, recent list, saved commands, config files. SPDX-License-Identifier: GPL-3.0-only */
#include "ffkv.h"
#include "profile.h"
#include "cstr.h"
#include "tt.h"

#include <stdio.h>

static void test_target_parse(void) {
    SshTarget t;
    TT_EQ_INT(target_parse("user@192.168.1.10", &t), TargetOk);
    TT_EQ_STR(t.user, "user");
    TT_EQ_STR(t.host, "192.168.1.10");
    TT_EQ_INT(t.port, 22);

    TT_EQ_INT(target_parse("  root@nas.lan:2222 ", &t), TargetOk);
    TT_EQ_STR(t.host, "nas.lan");
    TT_EQ_INT(t.port, 2222);

    TT_EQ_INT(target_parse("pi@[fe80::1%eth0]:22", &t), TargetOk);
    TT_EQ_STR(t.host, "fe80::1%eth0");
    TT_EQ_INT(target_parse("pi@[2001:db8::2]", &t), TargetOk);
    TT_EQ_STR(t.host, "2001:db8::2");

    TT_EQ_INT(target_parse("", &t), TargetErrEmpty);
    TT_EQ_INT(target_parse("192.168.1.10", &t), TargetErrNoUser);
    TT_EQ_INT(target_parse("@host", &t), TargetErrBadUser);
    TT_EQ_INT(target_parse("a b@host", &t), TargetErrBadUser);
    TT_EQ_INT(target_parse("u@", &t), TargetErrBadHost);
    TT_EQ_INT(target_parse("u@ho st", &t), TargetErrBadHost);
    TT_EQ_INT(target_parse("u@fe80::1", &t), TargetErrBadHost); /* needs [] */
    TT_EQ_INT(target_parse("u@[fe80::1", &t), TargetErrBadHost);
    TT_EQ_INT(target_parse("u@host:0", &t), TargetErrBadPort);
    TT_EQ_INT(target_parse("u@host:65536", &t), TargetErrBadPort);
    TT_EQ_INT(target_parse("u@host:", &t), TargetErrBadPort);
    TT_EQ_INT(target_parse("u@host:22x", &t), TargetErrBadPort);
    TT_EQ_INT(target_parse("u@host:-1", &t), TargetErrBadPort);
    /* last '@' wins: user part cannot contain '@' anyway */
    TT_EQ_INT(target_parse("a@b@host", &t), TargetErrBadUser);
    TT_CHECK(target_error_str(TargetErrBadPort)[0] != '\0');
}

static void test_target_format(void) {
    SshTarget t;
    char buf[128];
    target_parse("user@192.168.1.10", &t);
    target_format(&t, buf, sizeof(buf));
    TT_EQ_STR(buf, "user@192.168.1.10");
    target_parse("user@nas:2222", &t);
    target_format(&t, buf, sizeof(buf));
    TT_EQ_STR(buf, "user@nas:2222");
    target_parse("pi@[fe80::1]:23", &t);
    target_format(&t, buf, sizeof(buf));
    TT_EQ_STR(buf, "pi@[fe80::1]:23");
    /* format -> parse round trip */
    SshTarget u;
    TT_EQ_INT(target_parse(buf, &u), TargetOk);
    TT_CHECK(target_equal(&t, &u));
    target_id(&t, buf, sizeof(buf));
    TT_EQ_STR(buf, "[fe80::1]:23");
}

static void test_alias(void) {
    TT_CHECK(alias_valid("nas"));
    TT_CHECK(alias_valid("robot-a"));
    TT_CHECK(alias_valid("db_01.prod"));
    TT_CHECK(!alias_valid(""));
    TT_CHECK(!alias_valid("-x"));
    TT_CHECK(!alias_valid("a/b"));
    TT_CHECK(!alias_valid("a b"));
    TT_CHECK(!alias_valid("trailing."));
    TT_CHECK(!alias_valid("abcdefghijklmnopqrstu")); /* 21 > max */
    char a[32];
    alias_suggest("nas.lan", a, sizeof(a));
    TT_EQ_STR(a, "nas");
    alias_suggest("192.168.1.10", a, sizeof(a));
    TT_EQ_STR(a, "host-1-10");
    alias_suggest("fe80::1", a, sizeof(a));
    TT_CHECK(alias_valid(a));
    alias_suggest("...", a, sizeof(a));
    TT_EQ_STR(a, "host");
}

static void test_profile_roundtrip(void) {
    HostProfile p;
    profile_init(&p);
    TT_EQ_INT(p.policy, ProtoPolicyPrompt);
    TT_EQ_INT(p.transport, ProtoTransportAuto);
    TT_CHECK(!p.remember);
    strcpy(p.alias, "nas");
    target_parse("user@192.168.1.10:2222", &p.target);
    p.auth = ProfileAuthDefault;
    p.remember = true;
    p.transport = ProtoTransportWifi;
    p.policy = ProtoPolicyStrict;
    TT_CHECK(profile_add_cmd(&p, "uptime"));
    TT_CHECK(profile_add_cmd(&p, "df -h | grep /srv"));
    TT_CHECK(!profile_add_cmd(&p, "bad\ncmd"));
    TT_CHECK(!profile_add_cmd(&p, ""));

    char text[2048];
    TT_CHECK(profile_to_text(&p, text, sizeof(text)));
    TT_CHECK(strstr(text, "Filetype: SSH Runner Host\nVersion: 1\n") == text);
    TT_CHECK(strstr(text, "Hostkey_policy: strict\n") != NULL);
    TT_CHECK(strstr(text, "Cmd: df -h | grep /srv\n") != NULL);

    HostProfile q;
    TT_CHECK(profile_from_text(text, &q));
    TT_EQ_STR(q.alias, "nas");
    TT_EQ_STR(q.target.user, "user");
    TT_EQ_INT(q.target.port, 2222);
    TT_EQ_INT(q.auth, ProfileAuthDefault);
    TT_CHECK(q.remember);
    TT_EQ_INT(q.transport, ProtoTransportWifi);
    TT_EQ_INT(q.policy, ProtoPolicyStrict);
    TT_EQ_INT(q.cmd_count, 2);
    TT_EQ_STR(q.cmds[1], "df -h | grep /srv");

    char id[64];
    profile_id(&q, id, sizeof(id));
    TT_EQ_STR(id, "nas");
    q.alias[0] = 0;
    profile_id(&q, id, sizeof(id));
    TT_EQ_STR(id, "192.168.1.10:2222");

    TT_CHECK(profile_remove_cmd(&p, 0));
    TT_EQ_INT(p.cmd_count, 1);
    TT_EQ_STR(p.cmds[0], "df -h | grep /srv");
    TT_CHECK(!profile_remove_cmd(&p, 5));
    for(int i = 0; i < 20; i++)
        (void)profile_add_cmd(&p, "x");
    TT_EQ_INT(p.cmd_count, PROFILE_MAX_CMDS);

    /* Too small output buffer is reported, not silently truncated. */
    char tiny[40];
    TT_CHECK(!profile_to_text(&p, tiny, sizeof(tiny)));
}

static void test_profile_parse_robustness(void) {
    HostProfile p;
    char t1[] = "Filetype: SSH Runner Host\r\nVersion: 1\r\n# c\r\nAlias: nas\r\nUser: user\r\n"
                "Host: 10.0.0.2\r\nAuth: bogus\r\nTransport: lan\r\n";
    TT_CHECK(profile_from_text(t1, &p)); /* CRLF ok, unknown enum -> default */
    TT_EQ_INT(p.auth, ProtoAuthPassword);
    TT_EQ_INT(p.transport, ProtoTransportLan);
    TT_EQ_INT(p.target.port, 22);
    char t2[] = "Filetype: Something Else\nVersion: 1\nAlias: nas\nUser: w\nHost: h\n";
    TT_CHECK(!profile_from_text(t2, &p));
    char t3[] = "Filetype: SSH Runner Host\nVersion: 9\nAlias: nas\nUser: w\nHost: h\n";
    TT_CHECK(!profile_from_text(t3, &p)); /* future version */
    char t4[] = "Filetype: SSH Runner Host\nVersion: 1\nAlias: ../x\nUser: w\nHost: h\n";
    TT_CHECK(!profile_from_text(t4, &p)); /* path traversal alias */
    char t5[] = "Filetype: SSH Runner Host\nVersion: 1\nAlias: a\nUser: w\nHost: h\nPort: 99999\n";
    TT_CHECK(!profile_from_text(t5, &p));
    char t6[] = "";
    TT_CHECK(!profile_from_text(t6, &p));
    char t7[] = "Filetype: SSH Runner Host\nVersion: 1\nAlias: v6\nUser: w\nHost: fe80::1\n";
    TT_CHECK(profile_from_text(t7, &p)); /* value with colons */
    TT_EQ_STR(p.target.host, "fe80::1");
}

static void test_ffkv(void) {
    char t[] = "Filetype: X\nVersion: 1\n  Key :  spaced value  \nnocolon\nK: a\nK: b\n:empty\n";
    Ffkv kv;
    ffkv_parse(&kv, t);
    TT_EQ_STR(ffkv_get(&kv, "Key"), "spaced value");
    TT_EQ_INT(ffkv_count(&kv, "K"), 2);
    TT_EQ_STR(ffkv_get_nth(&kv, "K", 1), "b");
    TT_CHECK(ffkv_get_nth(&kv, "K", 2) == NULL);
    TT_CHECK(ffkv_check_header(&kv, "X", 1));
    TT_CHECK(!ffkv_check_header(&kv, "Y", 1));

    char buf[64];
    FfkvWriter w;
    ffkv_writer_init(&w, buf, sizeof(buf));
    ffkv_write(&w, "A", "line\nbreak");
    TT_CHECK(w.err);

    /* more pairs than the table holds */
    char big[4096] = "";
    for(int i = 0; i < FFKV_MAX_PAIRS + 5; i++)
        strcat(big, "Cmd: x\n");
    ffkv_parse(&kv, big);
    TT_EQ_INT(kv.count, FFKV_MAX_PAIRS);
    TT_CHECK(kv.overflow);
}

static RecentEntry entry(const char* target, const char* alias, uint8_t transport) {
    RecentEntry e;
    memset(&e, 0, sizeof(e));
    target_parse(target, &e.target);
    strcpy(e.alias, alias);
    e.transport = transport;
    return e;
}

static void test_recent(void) {
    RecentList r;
    recent_init(&r);
    RecentEntry a = entry("user@192.168.1.10", "", 0);
    RecentEntry b = entry("root@10.0.0.2:2222", "", 1);
    RecentEntry c = entry("user@192.168.1.10", "nas", 2);
    recent_push(&r, &a);
    recent_push(&r, &b);
    recent_push(&r, &c);
    TT_EQ_INT(r.count, 3); /* aliased entry is distinct from the unsaved one */
    recent_push(&r, &a); /* moves to front, no duplicate */
    TT_EQ_INT(r.count, 3);
    TT_EQ_STR(r.items[0].target.host, "192.168.1.10");
    TT_EQ_STR(r.items[0].alias, "");
    TT_EQ_STR(r.items[1].alias, "nas");

    for(int i = 0; i < 15; i++) {
        char t[32];
        snprintf(t, sizeof(t), "u@10.1.1.%d", i);
        RecentEntry e = entry(t, "", 0);
        recent_push(&r, &e);
    }
    TT_EQ_INT(r.count, RECENT_MAX);
    TT_EQ_STR(r.items[0].target.host, "10.1.1.14");
    TT_EQ_STR(r.items[RECENT_MAX - 1].target.host, "10.1.1.5");

    char text[2048];
    TT_CHECK(recent_to_text(&r, text, sizeof(text)));
    RecentList r2;
    TT_CHECK(recent_from_text(text, &r2));
    TT_EQ_INT(r2.count, RECENT_MAX);
    TT_EQ_STR(r2.items[3].target.host, "10.1.1.11");

    recent_init(&r);
    recent_push(&r, &c);
    recent_push(&r, &b);
    TT_CHECK(recent_to_text(&r, text, sizeof(text)));
    TT_CHECK(strstr(text, "Conn: lan root@10.0.0.2:2222\n") != NULL);
    TT_CHECK(strstr(text, "Conn: wifi user@192.168.1.10 nas\n") != NULL);
    TT_CHECK(recent_from_text(text, &r2));
    TT_EQ_INT(r2.count, 2);
    TT_EQ_STR(r2.items[1].alias, "nas");
    TT_EQ_INT(r2.items[1].transport, 2);
    char label[64];
    recent_label(&r2.items[1], label, sizeof(label));
    TT_EQ_STR(label, "nas");
    recent_label(&r2.items[0], label, sizeof(label));
    TT_EQ_STR(label, "root@10.0.0.2:2222");

    recent_forget_alias(&r2, "nas");
    TT_EQ_STR(r2.items[1].alias, "");
    TT_CHECK(recent_remove(&r2, 0));
    TT_EQ_INT(r2.count, 1);
    TT_CHECK(!recent_remove(&r2, 3));

    /* Garbage lines are skipped, not fatal. */
    char bad[] =
        "Filetype: SSH Runner Recent\nVersion: 1\nConn: lan\nConn: nope u@h\nConn: auto u@h ../x\n"
        "Conn: auto ok@h\n";
    TT_CHECK(recent_from_text(bad, &r2));
    TT_EQ_INT(r2.count, 1);
    TT_EQ_STR(r2.items[0].target.user, "ok");
}

static void test_commands(void) {
    CommandList c;
    commands_init(&c);
    TT_CHECK(commands_add(&c, "uptime"));
    TT_CHECK(commands_add(&c, "sudo systemctl restart nginx"));
    char text[4096];
    TT_CHECK(commands_to_text(&c, text, sizeof(text)));
    CommandList d;
    TT_CHECK(commands_from_text(text, &d));
    TT_EQ_INT(d.count, 2);
    TT_EQ_STR(d.cmds[1], "sudo systemctl restart nginx");
    TT_CHECK(commands_remove(&d, 0));
    TT_EQ_STR(d.cmds[0], "sudo systemctl restart nginx");
    for(int i = 0; i < 40; i++)
        (void)commands_add(&d, "x");
    TT_EQ_INT(d.count, GLOBAL_MAX_CMDS);
}

/* Appends "Cmd: c<i>" to the text in buf; the test fails rather than cut it short. */
static void add_cmd(char* buf, size_t size, int i) {
    size_t used = strlen(buf);
    int r = snprintf(buf + used, size - used, "Cmd: c%d\n", i);
    TT_CHECK(r > 0 && (size_t)r < size - used);
}

/* never cut or dropped: a host file with a command the app cannot run is refused (the list
 * shows it as unreadable, and a save cannot lose it); commands.conf counts what cannot run */
static void test_command_limits(void) {
    static char text[4096];
    char long_cmd[PROFILE_CMD_MAX + 2];
    memset(long_cmd, 'x', PROFILE_CMD_MAX + 1);
    long_cmd[PROFILE_CMD_MAX + 1] = 0;
    const char* head = "Filetype: SSH Runner Host\nVersion: 1\nAlias: nas\nUser: u\nHost: h\n";
    HostProfile p;
    snprintf(text, sizeof(text), "%sCmd: ok\nCmd: \nCmd: %.127s\n", head, long_cmd);
    TT_CHECK(profile_from_text(text, &p)); /* 127 is fine, an empty line holds nothing */
    TT_EQ_INT(p.cmd_count, 2);
    TT_EQ_STR(profile_error(), "");
    snprintf(text, sizeof(text), "%sCmd: ok\nCmd: %s\n", head, long_cmd);
    TT_CHECK(!profile_from_text(text, &p));
    TT_EQ_STR(profile_error(), "A command over 127 chars");
    snprintf(text, sizeof(text), "%s", head);
    for(int i = 0; i < PROFILE_MAX_CMDS + 1; i++)
        add_cmd(text, sizeof(text), i);
    TT_CHECK(!profile_from_text(text, &p));
    TT_EQ_STR(profile_error(), "More than 8 commands");

    snprintf(text, sizeof(text), "Filetype: SSH Runner Commands\nVersion: 1\nCmd: %s\n", long_cmd);
    for(int i = 0; i < GLOBAL_MAX_CMDS + 1; i++)
        add_cmd(text, sizeof(text), i);
    CommandList c;
    TT_CHECK(commands_from_text(text, &c));
    TT_EQ_INT(c.count, GLOBAL_MAX_CMDS);
    TT_EQ_INT(c.bad, 2); /* the long one and the 17th */
    TT_EQ_STR(c.cmds[0], "c0");
}

/* Restore Examples appends what is missing and nothing else */
static void test_commands_missing(void) {
    static const char ex_text[] = "Filetype: SSH Runner Commands\nVersion: 1\n# examples\n"
                                  "Cmd: up=  echo;uptime\nCmd: df -h\nCmd: date\n";
    char ex[256], out[512];
    unsigned added, no_room;
    snprintf(ex, sizeof(ex), "%s", ex_text);
    commands_missing("", ex, out, sizeof(out), &added, &no_room);
    TT_EQ_INT(added, 3);
    TT_EQ_STR(out, "Cmd: up=  echo;uptime\nCmd: df -h\nCmd: date\n");
    /* the user's file: one example kept (CRLF, spaces), one edited, their own command */
    const char* cur =
        "Filetype: SSH Runner Commands\r\nVersion: 1\r\nCmd:   df -h  \r\nCmd: date -u\r\nCmd: mine\r\n";
    snprintf(ex, sizeof(ex), "%s", ex_text);
    commands_missing(cur, ex, out, sizeof(out), &added, &no_room);
    TT_EQ_INT(added, 2);
    TT_EQ_INT(no_room, 0);
    TT_EQ_STR(out, "Cmd: up=  echo;uptime\nCmd: date\n");
    TT_EQ_INT(commands_text_count(cur), 3);
    /* 15 there: one more fits, the other is reported */
    static char full[2048];
    snprintf(full, sizeof(full), "Filetype: SSH Runner Commands\nVersion: 1\n");
    for(int i = 0; i < GLOBAL_MAX_CMDS - 1; i++)
        add_cmd(full, sizeof(full), i);
    snprintf(ex, sizeof(ex), "%s", ex_text);
    commands_missing(full, ex, out, sizeof(out), &added, &no_room);
    TT_EQ_INT(added, 1);
    TT_EQ_INT(no_room, 2);
    TT_EQ_STR(out, "Cmd: up=  echo;uptime\n");
}

static void test_script_helpers(void) {
    char i[16];
    script_interpreter("#!/bin/bash", i, sizeof(i));
    TT_EQ_STR(i, "bash");
    script_interpreter("#!/usr/bin/env bash\n", i, sizeof(i));
    TT_EQ_STR(i, "bash");
    script_interpreter("#! /bin/sh -e", i, sizeof(i));
    TT_EQ_STR(i, "sh");
    script_interpreter("#!/usr/bin/python3", i, sizeof(i));
    TT_EQ_STR(i, "sh"); /* not in the allow-list */
    script_interpreter("echo hi", i, sizeof(i));
    TT_EQ_STR(i, "sh");
    script_interpreter(NULL, i, sizeof(i));
    TT_EQ_STR(i, "sh");
    TT_CHECK(script_name_valid("backup.sh"));
    TT_CHECK(!script_name_valid("backup.py"));
    TT_CHECK(!script_name_valid("../x.sh"));
    TT_CHECK(!script_name_valid(".hidden.sh"));
    TT_CHECK(!script_name_valid(".sh"));
}

static void test_cstr(void) {
    TT_CHECK(cstr_has_any("a\nb", "\r\n"));
    TT_CHECK(!cstr_has_any("abc", "\r\n"));
    TT_CHECK(!cstr_has_any("", "x"));

    char line[] = "  lan  user@h:22 nas ";
    char* save = NULL;
    TT_EQ_STR(cstr_next_word(line, &save), "lan");
    TT_EQ_STR(cstr_next_word(NULL, &save), "user@h:22");
    TT_EQ_STR(cstr_next_word(NULL, &save), "nas");
    TT_CHECK(cstr_next_word(NULL, &save) == NULL);
    TT_CHECK(cstr_next_word(NULL, &save) == NULL);

    char names[5][8] = {"nas", "Beta", "alpha", "robot", "Nas2"};
    cstr_sort(&names[0][0], 5, sizeof(names[0]));
    TT_EQ_STR(names[0], "alpha");
    TT_EQ_STR(names[1], "Beta");
    TT_EQ_STR(names[2], "nas");
    TT_EQ_STR(names[3], "Nas2");
    TT_EQ_STR(names[4], "robot");
    cstr_sort(&names[0][0], 0, sizeof(names[0]));

    TT_EQ_INT(cstr_to_ulong("3000", 200000), 3000);
    TT_EQ_INT(cstr_to_ulong(" 12x", 99), 12);
    TT_EQ_INT(cstr_to_ulong("99999999999", 200000), 200000);
    TT_EQ_INT(cstr_to_ulong("abc", 10), 0);
}

static void test_set_cmd(void) {
    HostProfile p;
    profile_init(&p);
    TT_CHECK(profile_add_cmd(&p, "uptime"));
    TT_CHECK(profile_add_cmd(&p, "df -h"));
    TT_CHECK(profile_set_cmd(&p, 0, "uptime -p"));
    TT_EQ_STR(p.cmds[0], "uptime -p");
    TT_EQ_STR(p.cmds[1], "df -h");
    TT_CHECK(!profile_set_cmd(&p, 2, "ls"));
    TT_CHECK(!profile_set_cmd(&p, 0, ""));
    TT_EQ_INT(p.cmd_count, 2);
}

/* The golden files are the canonical text: the app reads each and writes it back unchanged.
 * cli/internal/appdata checks the same files, so the PC tool and the app agree. */
static char* slurp(const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", GOLDEN_DIR, name);
    FILE* f = fopen(path, "rb");
    TT_CHECK(f != NULL);
    if(!f) return NULL;
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void test_golden_files(void) {
    static const char* const hosts[] = {
        "host_key.conf", "host_password.conf", "host_default.conf"};
    static char copy_in[4096], out[4096];
    for(size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        char* text = slurp(hosts[i]);
        if(!text) continue;
        snprintf(copy_in, sizeof(copy_in), "%s", text);
        HostProfile p;
        TT_CHECK(profile_from_text(copy_in, &p));
        TT_CHECK(profile_to_text(&p, out, sizeof(out)));
        TT_EQ_STR(out, text);
    }
    char* text = slurp("commands.conf");
    if(text) {
        snprintf(copy_in, sizeof(copy_in), "%s", text);
        static CommandList c;
        TT_CHECK(commands_from_text(copy_in, &c));
        TT_CHECK(commands_to_text(&c, out, sizeof(out)));
        TT_EQ_STR(out, text);
    }
}

int main(void) {
    TT_RUN(test_cstr);
    TT_RUN(test_golden_files);
    TT_RUN(test_target_parse);
    TT_RUN(test_target_format);
    TT_RUN(test_alias);
    TT_RUN(test_profile_roundtrip);
    TT_RUN(test_profile_parse_robustness);
    TT_RUN(test_ffkv);
    TT_RUN(test_recent);
    TT_RUN(test_commands);
    TT_RUN(test_command_limits);
    TT_RUN(test_commands_missing);
    TT_RUN(test_script_helpers);
    TT_RUN(test_set_cmd);
    TT_END();
}
