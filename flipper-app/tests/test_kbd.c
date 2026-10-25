/* Keyboard layout (views/kbd_layout.c): on screen, no overlaps, aligned columns, every
 * character a host or a shell command needs, and nearest-key Up/Down.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "kbd_layout.h"
#include "tt.h"

#include <string.h>

#define FIELD_BOTTOM 20 /* the input field ends here; keys start below it */

static bool overlap(KbdRect a, KbdRect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void test_keys_on_screen(void) {
    for(uint8_t r = 0; r < KBD_ROWS; r++) {
        uint8_t n;
        const KbdKey* k = kbd_row(r, &n);
        TT_CHECK(n <= KBD_MAXKEYS);
        for(uint8_t i = 0; i < n; i++) {
            KbdRect q = kbd_key_rect(&k[i]);
            /* the stock module keeps 1 px clear at the screen edges */
            TT_CHECK(q.x >= 1);
            TT_CHECK(q.x + q.w <= KBD_SCREEN_W - 1);
            TT_CHECK(q.y > FIELD_BOTTOM);
            TT_CHECK(q.y + q.h <= KBD_SCREEN_H);
        }
    }
}

static void test_no_overlaps(void) {
    KbdRect all[KBD_ROWS * KBD_MAXKEYS];
    size_t total = 0;
    for(uint8_t r = 0; r < KBD_ROWS; r++) {
        uint8_t n;
        const KbdKey* k = kbd_row(r, &n);
        for(uint8_t i = 0; i < n; i++)
            all[total++] = kbd_key_rect(&k[i]);
    }
    for(size_t i = 0; i < total; i++)
        for(size_t j = i + 1; j < total; j++)
            TT_CHECK(!overlap(all[i], all[j]));
}

/* No ragged digit columns: glyph keys in the same column share one x. */
static void test_columns_aligned(void) {
    for(uint8_t r = 1; r < KBD_ROWS; r++) {
        uint8_t n0, n;
        const KbdKey* top = kbd_row(0, &n0);
        const KbdKey* k = kbd_row(r, &n);
        for(uint8_t i = 0; i < n; i++) {
            if(k[i].key == KBD_BS || k[i].key == KBD_ENTER) continue;
            bool found = false;
            for(uint8_t j = 0; j < n0; j++)
                found |= top[j].x == k[i].x;
            TT_CHECK(found);
        }
    }
}

static bool typeable(char c) {
    for(uint8_t r = 0; r < KBD_ROWS; r++) {
        uint8_t n;
        const KbdKey* k = kbd_row(r, &n);
        for(uint8_t i = 0; i < n; i++)
            if(k[i].key == c || kbd_shift(k[i].key) == c) return true;
    }
    return false;
}

static void test_everything_typeable(void) {
    const char* need =
        "abcdefghijklmnopqrstuvwxyzABCXYZ0123456789@.-:/_ |>~$\"'*;&<=+!?()[]{}#%^,\\`";
    for(const char* p = need; *p; p++) {
        if(!typeable(*p)) printf("  not typeable: '%c'\n", *p);
        TT_CHECK(typeable(*p));
    }
}

static void test_shift(void) {
    TT_CHECK(kbd_shift('a') == 'A');
    TT_CHECK(kbd_shift('_') == ' ');
    TT_CHECK(kbd_shift(';') == ':');
    TT_CHECK(kbd_shift('7') == '7');
}

static void test_nearest(void) {
    /* straight down a column: '1' (row 1) -> '4' (row 2) -> '7' (row 3) */
    uint8_t n;
    const KbdKey* r1 = kbd_row(1, &n);
    uint8_t c1 = 0;
    while(r1[c1].key != '1')
        c1++;
    uint8_t c2 = kbd_nearest(1, c1, 2);
    TT_CHECK(kbd_row(2, &n)[c2].key == '4');
    uint8_t c3 = kbd_nearest(2, c2, 3);
    TT_CHECK(kbd_row(3, &n)[c3].key == '7');
}

int main(void) {
    TT_RUN(test_keys_on_screen);
    TT_RUN(test_no_overlaps);
    TT_RUN(test_columns_aligned);
    TT_RUN(test_everything_typeable);
    TT_RUN(test_shift);
    TT_RUN(test_nearest);
    TT_END();
}
