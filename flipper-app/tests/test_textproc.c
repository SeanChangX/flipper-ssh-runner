/* ANSI stripping, output buffer, formatting helpers. SPDX-License-Identifier: GPL-3.0-only */
#include "textproc.h"
#include "tt.h"

#include <proto/proto_msg.h>

static const char* strip_all(const char* in) {
    static char out[4096];
    AnsiStripper a;
    ansi_init(&a);
    size_t n = ansi_strip(&a, (const uint8_t*)in, strlen(in), out);
    out[n] = 0;
    return out;
}

/* Feed one byte at a time: results must equal the one-shot result. */
static const char* strip_bytewise(const char* in) {
    static char out[4096];
    AnsiStripper a;
    ansi_init(&a);
    size_t o = 0;
    for(size_t i = 0; in[i]; i++)
        o += ansi_strip(&a, (const uint8_t*)&in[i], 1, &out[o]);
    out[o] = 0;
    return out;
}

static void check_strip(const char* in, const char* want) {
    TT_EQ_STR(strip_all(in), want);
    TT_EQ_STR(strip_bytewise(in), want);
}

static void test_ansi(void) {
    check_strip("plain", "plain");
    check_strip("\x1b[01;34mbackups\x1b[0m  notes", "backups  notes");
    check_strip("a\r\nb\r\n", "a\nb\n");
    check_strip("50%\r100%\n", "50%\n100%\n");
    check_strip(
        "\x1b]0;title\x07"
        "after",
        "after"); /* OSC with BEL */
    check_strip("\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\", "link"); /* OSC 8 hyperlink */
    check_strip("\x1b(Bok", "ok"); /* charset designation */
    check_strip(
        "\x1b"
        "7saved\x1b"
        "8",
        "saved"); /* two-byte ESC sequences */
    check_strip("\x1b[?25l\x1b[2K\x1b[1Gline", "line");
    check_strip("tab\there", "tab  here");
    check_strip("bell\x07 bs\x08 del\x7f", "bell bs del");
    check_strip("caf\xc3\xa9 \xe2\x9c\x93 \xf0\x9f\x98\x80!", "caf? ? ?!");
    check_strip("\x80\xff", "??"); /* stray bytes */
    check_strip("\xc3", "?"); /* truncated UTF-8 at end */
    check_strip(
        "\xc3"
        "A",
        "?A"); /* malformed continuation */
    check_strip("\x1b[31", ""); /* unterminated CSI swallowed */

    /* Worst-case expansion stays within 2n. */
    char tabs[200];
    memset(tabs, '\t', sizeof(tabs) - 1);
    tabs[199] = 0;
    TT_EQ_INT(strlen(strip_all(tabs)), 2 * 199);

    /* CR split across calls followed by LF */
    AnsiStripper a;
    ansi_init(&a);
    char out[16];
    size_t n = ansi_strip(&a, (const uint8_t*)"x\r", 2, out);
    n += ansi_strip(&a, (const uint8_t*)"\ny", 2, out + n);
    out[n] = 0;
    TT_EQ_STR(out, "x\ny");
}

static void test_ansi_fuzz(void) {
    tt_seed(77);
    uint8_t in[512];
    char out[2 * 512 + 1];
    AnsiStripper a;
    ansi_init(&a);
    for(int iter = 0; iter < 5000; iter++) {
        size_t n = tt_rand() % sizeof(in);
        for(size_t i = 0; i < n; i++)
            in[i] = (uint8_t)tt_rand();
        size_t m = ansi_strip(&a, in, n, out);
        TT_CHECK(m <= 2 * n + 1);
        for(size_t i = 0; i < m; i++)
            if(!((out[i] >= 0x20 && out[i] < 0x7F) || out[i] == '\n')) {
                TT_CHECK(!"non-printable byte in output");
                break;
            }
    }
}

static void test_outbuf_small(void) {
    static OutBuf o;
    outbuf_reset(&o);
    outbuf_append(&o, "hello\n", 6);
    outbuf_append(&o, "world\n", 6);
    TT_CHECK(!outbuf_truncated(&o));
    char dst[64];
    TT_EQ_INT(outbuf_render(&o, dst, sizeof(dst)), 12);
    TT_EQ_STR(dst, "hello\nworld\n");
}

static void test_outbuf_truncation(void) {
    static OutBuf o;
    static char dst[OUTBUF_HEAD + OUTBUF_TAIL + 64];
    outbuf_reset(&o);
    char line[16];
    size_t total = 0;
    for(int i = 1; i <= 5000; i++) {
        int n = snprintf(line, sizeof(line), "%d\n", i);
        outbuf_append(&o, line, (size_t)n);
        total += (size_t)n;
    }
    TT_EQ_INT(o.total, total);
    TT_CHECK(outbuf_truncated(&o));
    TT_EQ_INT(outbuf_dropped(&o), total - OUTBUF_HEAD - OUTBUF_TAIL);
    size_t n = outbuf_render(&o, dst, sizeof(dst));
    TT_CHECK(n < outbuf_render_size(&o));
    TT_CHECK(strncmp(dst, "1\n2\n3\n", 6) == 0);
    TT_CHECK(strstr(dst, " skipped ...]\n") != NULL);
    /* ends with the last line, and the tail starts at a line boundary */
    TT_CHECK(n > 5 && strcmp(dst + n - 5, "5000\n") == 0);
    const char* after = strstr(dst, "skipped ...]\n") + strlen("skipped ...]\n");
    int first_tail = atoi(after);
    TT_CHECK(first_tail > 0);
    char expect[16];
    snprintf(expect, sizeof(expect), "%d\n", first_tail);
    TT_CHECK(strncmp(after, expect, strlen(expect)) == 0);
    /* everything after the first tail number is consecutive */
    int prev = first_tail;
    const char* p = strchr(after, '\n') + 1;
    bool consecutive = true;
    while(*p) {
        int v = atoi(p);
        if(v != prev + 1) consecutive = false;
        prev = v;
        p = strchr(p, '\n') + 1;
    }
    TT_CHECK(consecutive);
    TT_EQ_INT(prev, 5000);

    /* one huge append */
    outbuf_reset(&o);
    static char big[20000];
    memset(big, 'a', sizeof(big));
    big[sizeof(big) - 1] = 'z';
    outbuf_append(&o, big, sizeof(big));
    TT_EQ_INT(o.tail_len, OUTBUF_TAIL);
    outbuf_render(&o, dst, sizeof(dst));
    TT_CHECK(dst[strlen(dst) - 1] == 'z');

    /* render into a too-small buffer stays NUL-terminated */
    char small[10];
    TT_EQ_INT(outbuf_render(&o, small, sizeof(small)), 9);
    TT_EQ_INT(small[9], 0);
}

/* The parts the Output screen shows in place: one run until truncated, then head, marker
 * and tail, always the same text as outbuf_render; random appends against a plain model
 * of "first HEAD bytes + last TAIL bytes". */
static void test_outbuf_parts(void) {
    static OutBuf o;
    static char all[60000], dst[OUTBUF_HEAD + OUTBUF_TAIL + 64],
        joined[OUTBUF_HEAD + OUTBUF_TAIL + 64];
    OutbufParts p;

    outbuf_reset(&o);
    outbuf_append(&o, "abc", 3);
    outbuf_parts(&o, &p);
    TT_EQ_INT(p.head_len, 3);
    TT_EQ_INT(p.marker_len, 0);
    TT_EQ_INT(p.tail_len, 0);
    /* past the head but not truncated: still one run, head and tail adjacent */
    static char fill[OUTBUF_HEAD + 100];
    memset(fill, 'x', sizeof(fill));
    outbuf_reset(&o);
    outbuf_append(&o, fill, sizeof(fill));
    outbuf_parts(&o, &p);
    TT_EQ_INT(p.head_len, sizeof(fill));
    TT_EQ_INT(p.tail_len, 0);
    TT_CHECK(p.head == o.buf);

    uint32_t seed = 12345;
    for(int round = 0; round < 200; round++) {
        outbuf_reset(&o);
        size_t total = 0;
        int appends = 1 + round % 40;
        for(int k = 0; k < appends && total < sizeof(all) - 3000; k++) {
            seed = seed * 1103515245u + 12345u;
            size_t len = (seed >> 8) % (round % 3 ? 300 : 2900);
            for(size_t i = 0; i < len; i++) {
                seed = seed * 1103515245u + 12345u;
                all[total + i] = (seed >> 16) % 9 ? (char)('a' + (seed >> 20) % 26) : '\n';
            }
            outbuf_append(&o, all + total, len);
            total += len;
        }
        TT_EQ_INT(o.total, total);
        size_t head = total < OUTBUF_HEAD ? total : OUTBUF_HEAD;
        size_t tail = total - head < OUTBUF_TAIL ? total - head : OUTBUF_TAIL;
        TT_EQ_INT(o.head_len, head);
        TT_EQ_INT(o.tail_len, tail);
        TT_CHECK(!memcmp(o.buf, all, head));
        TT_CHECK(!memcmp(o.buf + OUTBUF_HEAD, all + total - tail, tail));

        outbuf_parts(&o, &p);
        size_t n = 0;
        memcpy(joined + n, p.head, p.head_len);
        n += p.head_len;
        memcpy(joined + n, p.marker, p.marker_len);
        n += p.marker_len;
        memcpy(joined + n, p.tail, p.tail_len);
        n += p.tail_len;
        TT_EQ_INT(outbuf_render(&o, dst, sizeof(dst)), n);
        TT_CHECK(!memcmp(dst, joined, n));
        if(outbuf_truncated(&o)) {
            TT_CHECK(p.marker_len > 0);
            /* the tail starts after a line break (or is the whole tail when it has none) */
            TT_CHECK(p.tail == o.buf + OUTBUF_HEAD || p.tail[-1] == '\n');
        } else {
            TT_CHECK(!memcmp(p.head, all, total));
            TT_EQ_INT(p.head_len, total);
        }
    }
}

/* SSIDs and other names on the Flipper's ASCII fonts: one '?' per character. */
static void test_text_ascii(void) {
    char b[32];
    TT_EQ_STR(text_ascii("Lab-WiFi_60", b, sizeof(b), '?'), "Lab-WiFi_60");
    TT_EQ_STR(
        text_ascii("\xe8\xb1\xaa\xe6\x9d\xb0\xe5\x8a\x9e\xe5\x85\xac", b, sizeof(b), '?'),
        "????"); /* 4 CJK */
    TT_EQ_STR(text_ascii("Caf\xc3\xa9 5G", b, sizeof(b), '?'), "Caf? 5G");
    TT_EQ_STR(text_ascii("a\tb\x01", b, sizeof(b), '?'), "a?b?"); /* control bytes */
    TT_EQ_STR(text_ascii("\xf0\x9f\x98\x80!", b, sizeof(b), '?'), "?!"); /* 4-byte emoji */
    TT_EQ_STR(text_ascii("\x80x", b, sizeof(b), '?'), "?x"); /* a stray continuation byte */
    TT_EQ_STR(text_ascii("abcdef", b, 4, '?'), "abc"); /* truncated, NUL-terminated */
    TT_EQ_STR(text_ascii(NULL, b, sizeof(b), '?'), "");
    TT_EQ_STR(text_ascii("\xe8\xb1\xaa!", b, sizeof(b), TEXT_TOFU), "\x7f!"); /* the box marker */
}

static void test_format(void) {
    char b[48];
    format_duration(b, sizeof(b), 850);
    TT_EQ_STR(b, "850 ms");
    format_duration(b, sizeof(b), 1240);
    TT_EQ_STR(b, "1.2 s");
    format_duration(b, sizeof(b), 125000);
    TT_EQ_STR(b, "2 m 05 s");
    format_size(b, sizeof(b), 900);
    TT_EQ_STR(b, "900 B");
    format_size(b, sizeof(b), 1536);
    TT_EQ_STR(b, "1.5 KB");
    format_exit_line(b, sizeof(b), 0, 0, 1240);
    TT_EQ_STR(b, "exit 0, 1.2 s");
    format_exit_line(b, sizeof(b), 9, PROTO_DONE_SIGNAL, 10);
    TT_EQ_STR(b, "signal 9, 10 ms");
    format_exit_line(b, sizeof(b), 130, PROTO_DONE_CANCELLED, 10);
    TT_EQ_STR(b, "cancelled, 10 ms");
    format_exit_line(b, sizeof(b), -1, PROTO_DONE_LOST, 10);
    TT_EQ_STR(b, "connection lost, 10 ms");
}

static void test_ip4(void) {
    char b[20];
    ip4_format(0xC0A8010A, b, sizeof(b));
    TT_EQ_STR(b, "192.168.1.10");
    uint32_t ip = 0;
    TT_CHECK(ip4_parse("10.0.0.254", &ip));
    TT_EQ_INT(ip, 0x0A0000FE);
    TT_CHECK(ip4_parse("255.255.255.0", &ip));
    TT_EQ_INT(ip, 0xFFFFFF00u);
    TT_CHECK(!ip4_parse("256.1.1.1", &ip));
    TT_CHECK(!ip4_parse("1.1.1", &ip));
    TT_CHECK(!ip4_parse("1.1.1.1.", &ip));
    TT_CHECK(!ip4_parse("1..1.1", &ip));
    TT_CHECK(!ip4_parse("0001.1.1.1", &ip));
    TT_CHECK(!ip4_parse("", &ip));
    TT_CHECK(!ip4_parse("a.b.c.d", &ip));
}

static void test_key_type_short(void) {
    TT_EQ_STR(key_type_short("ssh-ed25519"), "ED25519");
    TT_EQ_STR(key_type_short("ecdsa-sha2-nistp256"), "ECDSA P-256");
    TT_EQ_STR(key_type_short("ecdsa-sha2-nistp521"), "ECDSA P-521");
    TT_EQ_STR(key_type_short("rsa-sha2-512"), "RSA");
    TT_EQ_STR(key_type_short("ssh-rsa"), "RSA");
    TT_EQ_STR(key_type_short(""), "Unknown");
}

static void test_fp_wrap(void) {
    char out[128];
    fp_wrap("SHA256:abcdefghij", 7, out, sizeof(out));
    TT_EQ_STR(out, "SHA256:\nabcdefg\nhij");
    fp_wrap("abc", 10, out, sizeof(out));
    TT_EQ_STR(out, "abc");
}

static void test_technical(void) {
    const char* yes[] = {
        "user@192.168.1.10",
        "192.168.1.10",
        "$ uname -a",
        "# backup.sh (bash)",
        "logs/nas/",
        "user@host[:port]",
        "host:22",
        "SHA256:abc",
        "61%",
        "~/x",
        "a|b",
        "ESP32 fw 0.1.0",
        "v1.2"};
    const char* no[] = {
        "Quick Connect",
        "Host key changed",
        "Exit: 0",
        "Output saved",
        "Lab-WiFi",
        "Done.",
        "Add a user name",
        "nas",
        "Retry password for nas",
        "Back to stop",
        "Version 2",
        ""};
    for(size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        TT_CHECK(text_is_technical(yes[i]));
    for(size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        TT_CHECK(!text_is_technical(no[i]));
    TT_CHECK(!text_is_technical(NULL));
    /* body text: one '.' between digits is a number, two make an address or version */
    TT_CHECK(text_is_technical("1.2 s"));
    TT_CHECK(text_is_technical_body("870 KiB/s")); /* the slash */
    TT_CHECK(!text_is_technical_body("Exit 0, 1.2 s"));
    TT_CHECK(!text_is_technical_body("1.2 KB, runs with bash"));
    TT_CHECK(text_is_technical_body("ESP32 fw 0.1.0"));
    TT_CHECK(text_is_technical_body("Address 192.168.1.50"));
    TT_CHECK(text_is_technical_body("Missing user@"));
    TT_CHECK(!text_is_technical_body("Add a user name"));
}

int main(void) {
    TT_RUN(test_ansi);
    TT_RUN(test_ansi_fuzz);
    TT_RUN(test_outbuf_small);
    TT_RUN(test_outbuf_truncation);
    TT_RUN(test_outbuf_parts);
    TT_RUN(test_text_ascii);
    TT_RUN(test_format);
    TT_RUN(test_ip4);
    TT_RUN(test_fp_wrap);
    TT_RUN(test_key_type_short);
    TT_RUN(test_technical);
    TT_END();
}
