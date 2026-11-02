/* Terminal width of UTF-8 text (pure, host-tested). SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stddef.h>

/* Columns `s` takes on a terminal: East Asian wide and fullwidth characters (CJK, Hangul,
 * fullwidth forms, most emoji) take two, everything else one. Malformed or cut UTF-8 counts
 * one column per byte and never reads past the terminating NUL. */
size_t text_width(const char* s);
