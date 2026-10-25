/* Word wrap and truncation (core/wrap.c). SPDX-License-Identifier: GPL-3.0-only */
#include "wrap.h"
#include "tt.h"

#include <stdio.h>
#include <string.h>

/* A proportional test font, roughly FontSecondary: narrow punctuation, wide m/w. */
static uint8_t gw_prop(void* ctx, char c) {
    (void)ctx;
    switch(c) {
    case ' ':
        return 4;
    case 'i':
    case 'l':
    case '.':
    case ',':
    case ':':
    case '!':
    case '|':
        return 2;
    case 'm':
    case 'w':
    case 'M':
    case 'W':
        return 8;
    default:
        return 6;
    }
}

/* Monospace, like FontKeyboard. */
static uint8_t gw_mono(void* ctx, char c) {
    (void)ctx;
    (void)c;
    return 6;
}

#define MAXL 64
static WrapLine L[MAXL];
static char lines[MAXL][256];

static size_t lay(const char* t, uint16_t px, WrapGlyphWidth gw) {
    size_t n = wrap_text(t, strlen(t), px, gw, NULL, L, MAXL);
    for(size_t i = 0; i < n && i < MAXL; i++) {
        memcpy(lines[i], t + L[i].off, L[i].len);
        lines[i][L[i].len] = 0;
    }
    return n;
}

static void test_words(void) {
    /* 60 px = 10 mono chars */
    size_t n = lay("the quick brown fox jumps", 60, gw_mono);
    TT_EQ_INT(n, 3);
    TT_EQ_STR(lines[0], "the quick");
    TT_EQ_STR(lines[1], "brown fox");
    TT_EQ_STR(lines[2], "jumps");
}

static void test_uname_never_splits_words(void) {
    const char* t =
        "Linux server 6.8.0-45-generic #45-Ubuntu SMP PREEMPT_DYNAMIC x86_64 GNU/Linux";
    size_t n = lay(t, 120, gw_prop);
    TT_CHECK(n >= 3);
    for(size_t i = 0; i < n; i++) {
        TT_CHECK(wrap_width(lines[i], strlen(lines[i]), gw_prop, NULL) <= 120);
        /* every line ends at a space or after a break character, never mid-word */
        const char* end = t + L[i].off + L[i].len;
        char last = lines[i][L[i].len - 1];
        TT_CHECK(
            *end == 0 || *end == ' ' || last == '-' || last == '/' || last == ',' || last == ':' ||
            last == '=');
    }
    /* "Ubuntu" and "PREEMPT_DYNAMIC" each stay whole */
    bool ubuntu = false, dyn = false;
    for(size_t i = 0; i < n; i++) {
        if(strstr(lines[i], "Ubuntu")) ubuntu = true;
        if(strstr(lines[i], "PREEMPT_DYNAMIC")) dyn = true;
    }
    TT_CHECK(ubuntu && dyn);
}

static void test_break_after_punct(void) {
    size_t n = lay("/var/log/nginx/access.log", 60, gw_mono);
    TT_CHECK(n >= 2);
    TT_EQ_STR(lines[0], "/var/log/"); /* breaks after '/', keeps it above */
    n = lay("key=value,other=thing", 60, gw_mono);
    TT_EQ_STR(lines[0], "key=value,");
}

static void test_long_token_hard_break_no_widow(void) {
    /* 12 chars, 10 per line: a plain split would leave "kl" (2 chars) alone */
    size_t n = lay("abcdefghijkl", 60, gw_mono);
    TT_EQ_INT(n, 2);
    TT_EQ_STR(lines[0], "abcdefghi");
    TT_EQ_STR(lines[1], "jkl");
    /* 43-char base64 fingerprint in 20-char mono lines */
    n = lay("SHA256:yzHRNt8aQ+0NTacFMFcf09olctuJ47g0c2uHb/a3Nau", 120, gw_mono);
    for(size_t i = 0; i < n; i++)
        TT_CHECK(strlen(lines[i]) >= 3 && strlen(lines[i]) <= 20);
    TT_EQ_STR(lines[0], "SHA256:"); /* the prefix stays whole on its own */
}

static void test_newlines_and_blank_lines(void) {
    size_t n = lay("a\n\nb\n", 60, gw_mono);
    TT_EQ_INT(n, 3);
    TT_EQ_STR(lines[0], "a");
    TT_EQ_STR(lines[1], "");
    TT_EQ_STR(lines[2], "b");
    n = lay("x\r\ny", 60, gw_mono);
    TT_EQ_INT(n, 2);
    TT_EQ_STR(lines[0], "x");
    TT_EQ_STR(lines[1], "y");
    TT_EQ_INT(lay("", 60, gw_mono), 0);
}

/* "exit code should be / 3" on the device: a paragraph never ends with a lone 1-2 char word */
static void test_no_widow_word(void) {
    /* 60 px = 10 mono chars: "abcd efgh" fits, then "x" alone would follow */
    size_t n = lay("abcd efgh x", 60, gw_mono);
    TT_EQ_INT(n, 2);
    TT_EQ_STR(lines[0], "abcd");
    TT_EQ_STR(lines[1], "efgh x");
    n = lay("exit code should be 3\nnext", 120, gw_mono); /* 20 chars per line */
    TT_EQ_STR(lines[0], "exit code should");
    TT_EQ_STR(lines[1], "be 3");
    TT_EQ_STR(lines[2], "next");
    /* a longer last word is fine where it is */
    n = lay("abcd efgh xyz", 60, gw_mono);
    TT_EQ_STR(lines[0], "abcd efgh");
    TT_EQ_STR(lines[1], "xyz");
}

/* "add-/on" and "SD-/card" were split on the device (03 About) */
static void test_short_hyphen_parts_stay_whole(void) {
    size_t n = lay("an ESP32 add-on board", 60, gw_mono); /* 10 chars per line */
    for(size_t i = 0; i < n; i++) {
        TT_CHECK(strcmp(lines[i], "add-") != 0);
        TT_CHECK(strcmp(lines[i], "on board") != 0);
    }
    bool whole = false;
    for(size_t i = 0; i < n; i++)
        whole |= strstr(lines[i], "add-on") != NULL;
    TT_CHECK(whole);
    n = lay("an add-on, over LAN", 60, gw_mono); /* About on the device: "add-/on," */
    for(size_t i = 0; i < n; i++)
        TT_CHECK(strcmp(lines[i], "an add-") != 0 && strcmp(lines[i], "add-") != 0);
    n = lay("long-hyphenated-word", 60, gw_mono);
    TT_EQ_STR(lines[0], "long-"); /* parts of 4+ characters may still break */
}

static void test_continuation_skips_spaces(void) {
    size_t n = lay("aaaaaaaa    bbbb", 60, gw_mono);
    TT_EQ_INT(n, 2);
    TT_EQ_STR(lines[0], "aaaaaaaa");
    TT_EQ_STR(lines[1], "bbbb");
    /* leading spaces of a real line (after '\n') are kept: indentation */
    n = lay("x\n  y", 60, gw_mono);
    TT_EQ_STR(lines[1], "  y");
}

static void test_count_without_buffer(void) {
    const char* t = "one two three four five six seven eight nine ten";
    size_t n = wrap_text(t, strlen(t), 30, gw_mono, NULL, NULL, 0);
    TT_EQ_INT(n, lay(t, 30, gw_mono));
}

/* A view keeps only every Nth line start: wrapping again from any recorded line start
 * must reproduce exactly the same following lines. */
static void test_resume_from_any_line(void) {
    const char* t =
        "Linux server 6.8.0-45-generic #45-Ubuntu SMP\n\n  indented line here and more words\n"
        "abcdefghijklmnopqrstuvwxyz0123456789 /usr/local/share/very/long/path,a=b\n";
    size_t len = strlen(t);
    size_t n = wrap_text(t, len, 50, gw_prop, NULL, L, MAXL);
    TT_CHECK(n > 8 && n < MAXL);
    for(size_t k = 0; k < n; k++) {
        WrapLine r[4];
        size_t next = 0;
        size_t got = wrap_next(t + L[k].off, len - L[k].off, 50, gw_prop, NULL, r, 3, &next);
        TT_CHECK(got >= 1);
        for(size_t j = 0; j < got && k + j < n; j++) {
            TT_EQ_INT(L[k].off + r[j].off, L[k + j].off);
            TT_EQ_INT(r[j].len, L[k + j].len);
        }
        if(k + got < n) TT_EQ_INT(L[k].off + next, L[k + got].off);
    }
    /* chunked walk over the whole text gives the same total */
    size_t pos = 0, total = 0;
    for(int guard = 0; guard < 100 && pos < len; guard++) {
        size_t next = 0;
        size_t got = wrap_next(t + pos, len - pos, 50, gw_prop, NULL, NULL, 4, &next);
        total += got;
        if(got < 4) break;
        pos += next;
    }
    TT_EQ_INT(total, n);
}

static void test_fit(void) {
    char out[32];
    TT_CHECK(!wrap_fit("short", 60, gw_mono, NULL, out, sizeof(out)));
    TT_EQ_STR(out, "short");
    TT_CHECK(wrap_fit("user@a-very-long-hostname.example", 60, gw_mono, NULL, out, sizeof(out)));
    TT_CHECK(wrap_width(out, strlen(out), gw_mono, NULL) <= 60);
    TT_CHECK(strstr(out, "...") != NULL);
    /* no space right before the dots */
    TT_CHECK(wrap_fit("abc defghijklmnop", 42, gw_mono, NULL, out, sizeof(out)));
    TT_EQ_STR(out, "abc...");
}

/* 35-output-saved: a file name never splits; too long, it loses its middle, not its end */
static void test_fit_middle(void) {
    char out[40];
    TT_CHECK(!wrap_fit_middle("261021-061344.log", 120, gw_mono, NULL, out, sizeof(out)));
    TT_EQ_STR(out, "261021-061344.log"); /* 17 chars fit 20 */
    TT_CHECK(wrap_fit_middle("nas-20261021-061344.log", 120, gw_mono, NULL, out, sizeof(out)));
    TT_EQ_INT(strlen(out), 20);
    TT_CHECK(strncmp(out, "nas-20", 6) == 0); /* start and end balanced around the dots */
    TT_CHECK(strstr(out, "...") != NULL);
    TT_CHECK(strcmp(out + strlen(out) - 4, ".log") == 0);
    /* no extension: still start and end around the dots */
    TT_CHECK(wrap_fit_middle("abcdefghijklmnopqrstuvwxyz", 60, gw_mono, NULL, out, sizeof(out)));
    TT_CHECK(strlen(out) == 10 && strstr(out, "...") && out[0] == 'a' && out[9] == 'z');
}

int main(void) {
    TT_RUN(test_words);
    TT_RUN(test_uname_never_splits_words);
    TT_RUN(test_break_after_punct);
    TT_RUN(test_long_token_hard_break_no_widow);
    TT_RUN(test_newlines_and_blank_lines);
    TT_RUN(test_no_widow_word);
    TT_RUN(test_short_hyphen_parts_stay_whole);
    TT_RUN(test_continuation_skips_spaces);
    TT_RUN(test_count_without_buffer);
    TT_RUN(test_resume_from_any_line);
    TT_RUN(test_fit);
    TT_RUN(test_fit_middle);
    TT_END();
}
