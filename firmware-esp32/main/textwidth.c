/* Terminal width of UTF-8 text. SPDX-License-Identifier: GPL-3.0-only */
#include "textwidth.h"

#include <stdbool.h>
#include <stdint.h>

static bool is_wide(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
           (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
           (c >= 0x20000 && c <= 0x3FFFD);
}

size_t text_width(const char* s) {
    size_t w = 0;
    const unsigned char* p = (const unsigned char*)s;
    while(*p) {
        uint32_t c = *p;
        int n = c < 0x80           ? 1 :
                (c & 0xE0) == 0xC0 ? 2 :
                (c & 0xF0) == 0xE0 ? 3 :
                (c & 0xF8) == 0xF0 ? 4 :
                                     1;
        int got = 1;
        if(n > 1) {
            c &= 0xFFu >> (n + 1);
            while(got < n && (p[got] & 0xC0) == 0x80) {
                c = (c << 6) | (p[got] & 0x3F);
                got++;
            }
        }
        if(got < n) { /* cut or malformed: one column per byte */
            w += (size_t)got;
        } else {
            w += is_wide(c) ? 2 : 1;
        }
        p += got;
    }
    return w;
}
