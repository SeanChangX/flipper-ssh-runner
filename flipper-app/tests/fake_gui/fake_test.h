/* Test-side control of the fake SDK (fake.h): drive the app, read back what was drawn.
 * SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "fake.h"

typedef enum {
    OpText,
    OpGlyph,
    OpBox,
    OpFrame,
    OpLine,
    OpIcon
} OpKind;

/* Who drew it: our code, or a stock element (whose layout is the firmware's, not ours). */
typedef enum {
    RoleOwn, /* app or our views */
    RoleButton, /* elements_button_* */
    RoleScrollbar, /* elements_scrollbar* */
    RoleStock, /* inside a stock module's own drawing */
} OpRole;

typedef struct {
    OpKind kind;
    OpRole role;
    Color color;
    Font font;
    int16_t x, y, w, h; /* ink box: x, y = top left; for text the glyph boxes' union */
    int16_t bx, by; /* text: the draw position (left, baseline) */
    char text[100];
    bool hidden; /* fully covered by a later white box */
    bool clipped; /* text partly covered by a later white box */
} Op;

#define FAKE_MAX_OPS 1500 /* a dimmed placeholder is drawn dot by dot */
typedef struct {
    Op op[FAKE_MAX_OPS];
    int n;
    uint32_t view_id;
    char fit_cut[8][64]; /* strings elements_string_fit_width shortened */
    int n_fit_cut;
    bool bad_button_font; /* a stock button was drawn with a font other than FontSecondary */
    bool space_dots; /* a stock "..." cut right after a space */
    char value_clipped[48]; /* a VariableItemList value wider than the 37 px the firmware shows */
} Frame;

/* Run `script` from inside view_dispatcher_run (the app's main loop). */
void fake_set_script(void (*script)(void));
int fake_app_main(void); /* calls the app's entry point */

void fake_press(InputKey key);
void fake_long(InputKey key);
void fake_advance(uint32_t ms); /* time passes: timers fire, events are dispatched */
bool fake_running(void); /* view_dispatcher_run has not been stopped */
const Frame* fake_render(void); /* draw the current view now */
uint32_t fake_current_view(void);
uint32_t fake_current_scene(void); /* top of the scene stack, or UINT32_MAX */
uint32_t fake_scene_visits(uint32_t scene_id);
void fake_fs_write(const char* path, const char* data);
void fake_fs_mkdir(const char* path);
bool fake_fs_exists(const char* path);
/* the file's text (a static buffer, the next call overwrites it), NULL if missing */
const char* fake_fs_read(const char* path);
void fake_fs_remove(const char* path);
/* Width of a string in a font, as canvas_string_width measures it. */
uint16_t fake_text_width(Font font, const char* s);

/* System heap the app sees at entry (default: plenty); the last stock DialogMessage shown. */
void fake_set_heap(size_t free_heap, size_t max_block);
const char* fake_last_dialog(void); /* "header|text", or "" */
