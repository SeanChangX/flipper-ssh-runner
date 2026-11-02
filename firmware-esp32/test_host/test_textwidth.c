/* Terminal width of UTF-8 text (wifi scan alignment). SPDX-License-Identifier: GPL-3.0-only */
#include "textwidth.h"
#include "tt.h"

static void test_ascii(void) {
    TT_EQ_INT(text_width(""), 0);
    TT_EQ_INT(text_width("homelab"), 7);
}

static void test_wide(void) {
    TT_EQ_INT(text_width("\xe4\xb8\xad\xe6\x96\x87"), 4); /* 2 CJK ideographs */
    TT_EQ_INT(
        text_width("\xed\x95\x9c"
                   "a"),
        3); /* Hangul + ASCII */
    TT_EQ_INT(text_width("\xef\xbc\xa1"), 2); /* fullwidth A */
    TT_EQ_INT(text_width("\xf0\x9f\x93\xb6"), 2); /* an emoji */
    TT_EQ_INT(text_width("caf\xc3\xa9"), 4); /* e acute: narrow */
}

static void test_cut_and_malformed(void) {
    TT_EQ_INT(text_width("ab\xe4\xb8"), 4); /* a CJK character cut short: 2 bytes, 2 columns */
    TT_EQ_INT(text_width("\x80x"), 2); /* a stray continuation byte */
    TT_EQ_INT(text_width("\xff"), 1);
}

int main(void) {
    TT_RUN(test_ascii);
    TT_RUN(test_wide);
    TT_RUN(test_cut_and_malformed);
    TT_END();
}
