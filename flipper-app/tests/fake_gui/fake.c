/* Host stand-in for the Flipper SDK (see fake.h). SPDX-License-Identifier: GPL-3.0-only
 *
 * Stock geometry (elements_button_*, Submenu, VariableItemList, DialogEx, Popup, Widget,
 * elements_multiline_text_aligned, elements_string_fit_width) follows the firmware's
 * applications/services/gui sources; it is checked against device screenshots by
 * tools/flipper/render_calibrate.py. */
#include "fake_test.h"
#include "font_metrics.h"

#include <assert.h>
#include <math.h>

#define SCREEN_W 128
#define SCREEN_H 64

/* ---------------- basics ---------------- */

void fake_fail(const char* what, const char* file, int line) {
    fprintf(stderr, "FATAL: %s at %s:%d\n", what, file, line);
    abort();
}

void fake_log(char level, const char* tag, const char* fmt, ...) {
    if(!getenv("FAKE_LOG")) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%c] %s: ", level, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

size_t fake_strlcat(char* dst, const char* src, size_t size) {
    size_t d = strnlen(dst, size), s = strlen(src);
    if(d == size) return size + s;
    size_t n = s < size - d - 1 ? s : size - d - 1;
    memcpy(dst + d, src, n);
    dst[d + n] = 0;
    return d + s;
}

struct FuriString {
    char* s;
    size_t cap;
};

static void fs_reserve(FuriString* f, size_t n) {
    if(n + 1 <= f->cap) return;
    f->cap = n + 64;
    f->s = realloc(f->s, f->cap);
}

FuriString* furi_string_alloc(void) {
    FuriString* f = calloc(1, sizeof(FuriString));
    fs_reserve(f, 0);
    f->s[0] = 0;
    return f;
}
FuriString* furi_string_alloc_set(const char* s) {
    FuriString* f = furi_string_alloc();
    furi_string_set(f, s);
    return f;
}
void furi_string_free(FuriString* f) {
    free(f->s);
    free(f);
}
const char* furi_string_get_cstr(const FuriString* f) {
    return f->s;
}
void furi_string_set(FuriString* f, const char* v) {
    size_t n = strlen(v);
    fs_reserve(f, n);
    memmove(f->s, v, n + 1);
}
int furi_string_printf(FuriString* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    fs_reserve(f, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(f->s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return n;
}
void furi_string_cat(FuriString* f, const char* v) {
    size_t a = strlen(f->s), b = strlen(v);
    fs_reserve(f, a + b);
    memcpy(f->s + a, v, b + 1);
}
void furi_string_reset(FuriString* f) {
    f->s[0] = 0;
}
size_t furi_string_size(const FuriString* f) {
    return strlen(f->s);
}
void furi_string_left(FuriString* f, size_t index) {
    if(index < strlen(f->s)) f->s[index] = 0;
}

static uint32_t now_ms = 1000;
FuriThreadId furi_thread_get_current_id(void) {
    return NULL;
}
uint32_t furi_thread_get_stack_space(FuriThreadId thread_id) {
    UNUSED(thread_id);
    return 0; /* no stack to watch on the host */
}

uint32_t furi_get_tick(void) {
    return now_ms;
}
uint32_t furi_kernel_get_tick_frequency(void) {
    return 1000;
}
uint32_t furi_ms_to_ticks(uint32_t ms) {
    return ms;
}

struct FuriTimer {
    FuriTimerCallback cb;
    FuriTimerType type;
    void* ctx;
    uint32_t period, next;
    bool running;
};
#define MAX_TIMERS 8
static FuriTimer* timers[MAX_TIMERS];

FuriTimer* furi_timer_alloc(FuriTimerCallback cb, FuriTimerType type, void* context) {
    FuriTimer* t = calloc(1, sizeof(FuriTimer));
    t->cb = cb;
    t->type = type;
    t->ctx = context;
    for(int i = 0; i < MAX_TIMERS; i++)
        if(!timers[i]) {
            timers[i] = t;
            return t;
        }
    fake_fail("too many timers", __FILE__, __LINE__);
    return NULL;
}
void furi_timer_free(FuriTimer* t) {
    for(int i = 0; i < MAX_TIMERS; i++)
        if(timers[i] == t) timers[i] = NULL;
    free(t);
}
int furi_timer_start(FuriTimer* t, uint32_t ticks) {
    t->period = ticks ? ticks : 1;
    t->next = now_ms + t->period;
    t->running = true;
    return 0;
}
int furi_timer_stop(FuriTimer* t) {
    t->running = false;
    return 0;
}

static int record_dummy;
void* furi_record_open(const char* name) {
    UNUSED(name);
    return &record_dummy;
}
void furi_record_close(const char* name) {
    UNUSED(name);
}

static bool debug_flag = true;
void fake_set_debug(bool on) {
    debug_flag = on;
}
bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) {
    return flag == FuriHalRtcFlagDebug && debug_flag;
}
void furi_hal_rtc_get_datetime(DateTime* dt) {
    *dt = (DateTime){
        .hour = 12, .minute = 34, .second = 56, .day = 21, .month = 10, .year = 2026, .weekday = 3};
}

const NotificationSequence sequence_success = {"success"}, sequence_error = {"error"},
                           sequence_single_vibro = {"vibro"}, sequence_blink_stop = {"blink_stop"},
                           sequence_blink_start_blue = {"blink_blue"};
void notification_message(NotificationApp* app, const NotificationSequence* seq) {
    UNUSED(app);
    UNUSED(seq);
}

/* ---------------- heap, dialogs ---------------- */

static size_t heap_free = 120000, heap_block = 60000;
void fake_set_heap(size_t free_heap, size_t max_block) {
    heap_free = free_heap;
    heap_block = max_block;
}
size_t memmgr_get_free_heap(void) {
    return heap_free;
}
size_t memmgr_heap_get_max_free_block(void) {
    return heap_block;
}
size_t memmgr_get_minimum_free_heap(void) {
    return heap_free;
}

struct DialogMessage {
    char header[48], text[96];
};
static char last_dialog[160];
DialogMessage* dialog_message_alloc(void) {
    return calloc(1, sizeof(DialogMessage));
}
void dialog_message_free(DialogMessage* m) {
    free(m);
}
void dialog_message_set_header(
    DialogMessage* m,
    const char* text,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v) {
    UNUSED(x);
    UNUSED(y);
    UNUSED(h);
    UNUSED(v);
    snprintf(m->header, sizeof(m->header), "%s", text ? text : "");
}
void dialog_message_set_text(
    DialogMessage* m,
    const char* text,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v) {
    UNUSED(x);
    UNUSED(y);
    UNUSED(h);
    UNUSED(v);
    snprintf(m->text, sizeof(m->text), "%s", text ? text : "");
}
void dialog_message_set_buttons(
    DialogMessage* m,
    const char* left,
    const char* center,
    const char* right) {
    UNUSED(m);
    UNUSED(left);
    UNUSED(center);
    UNUSED(right);
}
DialogMessageButton dialog_message_show(DialogsApp* app, const DialogMessage* m) {
    UNUSED(app);
    snprintf(last_dialog, sizeof(last_dialog), "%s|%s", m->header, m->text);
    return DialogMessageButtonCenter;
}
const char* fake_last_dialog(void) {
    return last_dialog;
}

/* ---------------- icons ---------------- */

const Icon I_WifiOn_9x8 = {"WifiOn", 9, 8}, I_Busy0_5x8 = {"Busy0", 5, 8},
           I_Busy1_5x8 = {"Busy1", 5, 8}, I_Busy2_5x8 = {"Busy2", 5, 8},
           I_Busy3_5x8 = {"Busy3", 5, 8}, I_Busy4_5x8 = {"Busy4", 5, 8},
           I_Busy5_5x8 = {"Busy5", 5, 8}, I_Back_8x7 = {"Back", 8, 7},
           I_LanOn_9x8 = {"LanOn", 9, 8}, I_Alert_9x8 = {"Alert", 9, 8},
           I_Lock_7x8 = {"Lock", 7, 8}, I_Key_10x10 = {"Key", 10, 10},
           I_KeySave_24x11 = {"KeySave", 24, 11}, I_KeySaveSelected_24x11 = {"KeySaveSel", 24, 11},
           I_KeyBackspace_16x9 = {"KeyBs", 16, 9},
           I_KeyBackspaceSelected_16x9 = {"KeyBsSel", 16, 9},
           I_Spinner0_13x13 = {"Spinner0", 13, 13}, I_Spinner1_13x13 = {"Spinner1", 13, 13},
           I_Spinner2_13x13 = {"Spinner2", 13, 13}, I_Spinner3_13x13 = {"Spinner3", 13, 13},
           I_Spinner4_13x13 = {"Spinner4", 13, 13}, I_Spinner5_13x13 = {"Spinner5", 13, 13},
           I_Spinner6_13x13 = {"Spinner6", 13, 13}, I_Spinner7_13x13 = {"Spinner7", 13, 13};
/* firmware icons the stock elements draw */
static const Icon I_ButtonLeft_4x7 = {"ButtonLeft", 4, 7},
                  I_ButtonRight_4x7 = {"ButtonRight", 4, 7},
                  I_ButtonCenter_7x7 = {"ButtonCenter", 7, 7}, I_Loading_24 = {"Loading", 24, 24};

uint16_t icon_get_width(const Icon* icon) {
    return icon->width;
}

uint16_t icon_get_height(const Icon* icon) {
    return icon->height;
}

/* ---------------- canvas ---------------- */

struct Canvas {
    Font font;
    Color color;
};
static Canvas canvas_obj;
static Frame frame;
static OpRole role = RoleOwn;

static const FakeFont* font_of(Font f) {
    switch(f) {
    case FontPrimary:
        return &fake_font_primary;
    case FontKeyboard:
        return &fake_font_keyboard;
    case FontBigNumbers:
        return &fake_font_bignumbers;
    default:
        return &fake_font_secondary;
    }
}

static const FakeGlyph* glyph(const FakeFont* f, char c) {
    unsigned u = (unsigned char)c;
    if(u < 32 || u > 126 || !f->g[u - 32].present) return NULL;
    return &f->g[u - 32];
}

/* u8g2_GetStrWidth: advances, but the last glyph counts with its box */
static uint16_t str_width(const FakeFont* f, const char* s) {
    int w = 0, dx = 0;
    const FakeGlyph* last = NULL;
    for(; *s; s++) {
        const FakeGlyph* g = glyph(f, *s);
        if(!g) continue;
        dx = g->dx;
        w += dx;
        last = g;
    }
    if(last && last->w) w = w - dx + last->w + last->x;
    return w < 0 ? 0 : (uint16_t)w;
}

uint16_t fake_text_width(Font font, const char* s) {
    return str_width(font_of(font), s);
}

static const CanvasFontParameters font_params[FontTotalNumber] = {
    [FontPrimary] = {.leading_default = 12, .leading_min = 11, .height = 8, .descender = 2},
    [FontSecondary] = {.leading_default = 11, .leading_min = 9, .height = 7, .descender = 2},
    [FontKeyboard] = {.leading_default = 11, .leading_min = 9, .height = 7, .descender = 2},
    [FontBigNumbers] = {.leading_default = 18, .leading_min = 16, .height = 15, .descender = 0},
};

static Op* add_op(OpKind k, int x, int y, int w, int h) {
    if(frame.n >= FAKE_MAX_OPS) fake_fail("too many draw ops", __FILE__, __LINE__);
    Op* o = &frame.op[frame.n++];
    memset(o, 0, sizeof(*o));
    o->kind = k;
    o->role = role;
    o->color = canvas_obj.color;
    o->font = canvas_obj.font;
    o->x = (int16_t)x;
    o->y = (int16_t)y;
    o->w = (int16_t)w;
    o->h = (int16_t)h;
    return o;
}

static bool inside(const Op* a, int x, int y, int w, int h) {
    return a->x >= x && a->y >= y && a->x + a->w <= x + w && a->y + a->h <= y + h;
}
static bool touches(const Op* a, int x, int y, int w, int h) {
    return a->x < x + w && x < a->x + a->w && a->y < y + h && y < a->y + a->h;
}

void canvas_clear(Canvas* c) {
    UNUSED(c);
    uint32_t v = frame.view_id;
    memset(&frame, 0, sizeof(frame));
    frame.view_id = v;
}
void canvas_set_color(Canvas* c, Color color) {
    c->color = color;
}
void canvas_set_font(Canvas* c, Font font) {
    c->font = font;
}
const CanvasFontParameters* canvas_get_font_params(const Canvas* c, Font font) {
    UNUSED(c);
    return &font_params[font < FontTotalNumber ? font : FontSecondary];
}
uint16_t canvas_string_width(Canvas* c, const char* str) {
    return str_width(font_of(c->font), str);
}
uint8_t canvas_glyph_width(Canvas* c, uint16_t symbol) {
    const FakeGlyph* g = glyph(font_of(c->font), (char)symbol);
    return g ? (uint8_t)g->dx : 0;
}

/* text ink: the union of the glyphs' inked boxes */
static void text_op(OpKind kind, int x, int y, const char* str) {
    const FakeFont* f = font_of(canvas_obj.font);
    int x0 = 1 << 14, y0 = 1 << 14, x1 = -(1 << 14), y1 = -(1 << 14), pen = x;
    for(const char* s = str; *s; s++) {
        const FakeGlyph* g = glyph(f, *s);
        if(!g) continue;
        if(g->ink_w && g->ink_h) {
            int gx = pen + g->ink_x, gy = y - g->ink_top;
            if(gx < x0) x0 = gx;
            if(gy < y0) y0 = gy;
            if(gx + g->ink_w > x1) x1 = gx + g->ink_w;
            if(gy + g->ink_h > y1) y1 = gy + g->ink_h;
        }
        pen += g->dx;
    }
    if(x1 < x0) return; /* only spaces */
    Op* o = add_op(kind, x0, y0, x1 - x0, y1 - y0);
    o->bx = (int16_t)x;
    o->by = (int16_t)y;
    snprintf(o->text, sizeof(o->text), "%s", str);
}

void canvas_draw_str(Canvas* c, int32_t x, int32_t y, const char* str) {
    UNUSED(c);
    text_op(OpText, x, y, str);
}
void canvas_draw_str_aligned(Canvas* c, int32_t x, int32_t y, Align h, Align v, const char* str) {
    uint16_t w = canvas_string_width(c, str);
    if(h == AlignRight) x -= w;
    if(h == AlignCenter) x -= w / 2;
    int asc = font_of(c->font)->ascent;
    if(v == AlignTop) y += asc;
    if(v == AlignCenter) y += asc / 2;
    text_op(OpText, x, y, str);
}
void canvas_draw_glyph(Canvas* c, int32_t x, int32_t y, uint16_t ch) {
    UNUSED(c);
    char s[2] = {(char)ch, 0};
    text_op(OpGlyph, x, y, s);
}

static void box_op(int x, int y, int w, int h) {
    add_op(OpBox, x, y, w, h);
    if(canvas_obj.color != ColorWhite) return;
    /* a white box erases what was drawn under it */
    Op* self = &frame.op[frame.n - 1];
    for(Op* o = frame.op; o < self; o++) {
        if(inside(o, x, y, w, h))
            o->hidden = true;
        else if((o->kind == OpText || o->kind == OpGlyph) && touches(o, x, y, w, h) && !o->hidden)
            o->clipped = true;
    }
}
void canvas_draw_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    UNUSED(c);
    box_op(x, y, (int)w, (int)h);
}
void canvas_draw_rbox(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, size_t r) {
    UNUSED(c);
    UNUSED(r);
    box_op(x, y, (int)w, (int)h);
}
void canvas_draw_frame(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    UNUSED(c);
    add_op(OpFrame, x, y, (int)w, (int)h);
}
void canvas_draw_rframe(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, size_t r) {
    UNUSED(c);
    UNUSED(r);
    add_op(OpFrame, x, y, (int)w, (int)h);
}
void canvas_draw_line(Canvas* c, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    UNUSED(c);
    add_op(OpLine, MIN(x1, x2), MIN(y1, y2), abs(x2 - x1) + 1, abs(y2 - y1) + 1);
}
void canvas_draw_dot(Canvas* c, int32_t x, int32_t y) {
    UNUSED(c);
    add_op(OpBox, x, y, 1, 1);
}
void canvas_draw_icon(Canvas* c, int32_t x, int32_t y, const Icon* icon) {
    UNUSED(c);
    Op* o = add_op(OpIcon, x, y, icon->width, icon->height);
    snprintf(o->text, sizeof(o->text), "%s", icon->name);
}

/* ---------------- elements (firmware gui/elements.c) ---------------- */

void elements_string_fit_width(Canvas* c, FuriString* s, size_t width) {
    uint16_t len_px = canvas_string_width(c, furi_string_get_cstr(s));
    if(len_px > width) {
        if(frame.n_fit_cut < 8)
            snprintf(frame.fit_cut[frame.n_fit_cut++], 64, "%s", furi_string_get_cstr(s));
        width -= canvas_string_width(c, "...");
        do {
            furi_string_left(s, furi_string_size(s) - 1);
            len_px = canvas_string_width(c, furi_string_get_cstr(s));
        } while(len_px > width && furi_string_size(s));
        size_t n = furi_string_size(s);
        if(n && furi_string_get_cstr(s)[n - 1] == ' ') frame.space_dots = true; /* "restart ..." */
        furi_string_cat(s, "...");
    }
}

void elements_scrollbar_pos(
    Canvas* c,
    int32_t x,
    int32_t y,
    size_t height,
    size_t pos,
    size_t total) {
    OpRole r = role;
    role = RoleScrollbar;
    Color col = c->color;
    canvas_set_color(c, ColorWhite);
    box_op(x - 3, y, 3, (int)height);
    canvas_set_color(c, ColorBlack);
    add_op(OpBox, x - 2, y, 1, (int)height); /* the dotted track */
    if(total) {
        float block_h = (float)height / (float)total;
        add_op(OpBox, x - 3, y + (int)(block_h * pos), 3, MAX((int)block_h, 1));
    }
    canvas_set_color(c, col);
    role = r;
}
void elements_scrollbar(Canvas* c, size_t pos, size_t total) {
    elements_scrollbar_pos(c, SCREEN_W, 0, SCREEN_H, pos, total);
}

static void button_check_font(Canvas* c) {
    if(c->font != FontSecondary) frame.bad_button_font = true;
}

void elements_button_left(Canvas* c, const char* str) {
    button_check_font(c);
    OpRole r = role;
    role = RoleButton;
    const int button_height = 12, vertical_offset = 3, horizontal_offset = 3;
    const Icon* icon = &I_ButtonLeft_4x7;
    const int icon_width_with_offset = icon->width + 3,
              icon_v_offset = icon->height + vertical_offset;
    const int button_width =
        canvas_string_width(c, str) + horizontal_offset * 2 + icon_width_with_offset;
    const int x = 0, y = SCREEN_H;
    canvas_set_color(c, ColorBlack);
    box_op(x, y - button_height, button_width + 3, button_height); /* incl. the slanted edge */
    canvas_set_color(c, ColorWhite);
    canvas_draw_icon(c, x + horizontal_offset, y - icon_v_offset, icon);
    canvas_draw_str(c, x + horizontal_offset + icon_width_with_offset, y - vertical_offset, str);
    canvas_set_color(c, ColorBlack);
    role = r;
}

void elements_button_right(Canvas* c, const char* str) {
    button_check_font(c);
    OpRole r = role;
    role = RoleButton;
    const int button_height = 12, vertical_offset = 3, horizontal_offset = 3;
    const Icon* icon = &I_ButtonRight_4x7;
    const int icon_width_with_offset = icon->width + 3,
              icon_v_offset = icon->height + vertical_offset;
    const int button_width =
        canvas_string_width(c, str) + horizontal_offset * 2 + icon_width_with_offset;
    const int x = SCREEN_W, y = SCREEN_H;
    canvas_set_color(c, ColorBlack);
    box_op(x - button_width - 3, y - button_height, button_width + 3, button_height);
    canvas_set_color(c, ColorWhite);
    canvas_draw_str(c, x - button_width + horizontal_offset, y - vertical_offset, str);
    canvas_draw_icon(c, x - horizontal_offset - icon->width, y - icon_v_offset, icon);
    canvas_set_color(c, ColorBlack);
    role = r;
}

void elements_button_center(Canvas* c, const char* str) {
    button_check_font(c);
    OpRole r = role;
    role = RoleButton;
    const int button_height = 12, vertical_offset = 3, horizontal_offset = 1;
    const Icon* icon = &I_ButtonCenter_7x7;
    const int icon_width_with_offset = icon->width + 3,
              icon_v_offset = icon->height + vertical_offset;
    const int button_width =
        canvas_string_width(c, str) + horizontal_offset * 2 + icon_width_with_offset;
    const int x = (SCREEN_W - button_width) / 2, y = SCREEN_H;
    canvas_set_color(c, ColorBlack);
    box_op(x - 3, y - button_height, button_width + 6, button_height);
    canvas_set_color(c, ColorWhite);
    canvas_draw_icon(c, x + horizontal_offset, y - icon_v_offset, icon);
    canvas_draw_str(c, x + horizontal_offset + icon_width_with_offset, y - vertical_offset, str);
    canvas_set_color(c, ColorBlack);
    role = r;
}

static uint8_t current_font_height(Canvas* c) {
    uint8_t h = (uint8_t)font_of(c->font)->max_h;
    if(c->font == FontSecondary) h += 1; /* firmware: haxrcorp4089 gets one extra row */
    return h;
}

static bool multiline_split; /* the stock auto-wrap had to cut a line (mid-word, with "-") */
static size_t max_chars_to_fit(Canvas* c, Align horizontal, const char* text, int x) {
    const char* end = strchr(text, '\n');
    if(!end) end = text + strlen(text);
    size_t text_size = (size_t)(end - text);
    char line[160];
    snprintf(line, sizeof(line), "%.*s", (int)text_size, text);
    uint16_t len_px = canvas_string_width(c, line);
    int px_left = 0;
    if(horizontal == AlignCenter)
        px_left = x > SCREEN_W / 2 ? (SCREEN_W - x) * 2 : x * 2;
    else if(horizontal == AlignLeft)
        px_left = SCREEN_W - x;
    else if(horizontal == AlignRight)
        px_left = x;
    if(len_px > px_left && text_size) {
        multiline_split = true;
        size_t excess =
            (size_t)ceilf((float)(len_px - px_left) / ((float)len_px / (float)text_size));
        excess = MAX(excess, 5);
        return text_size > excess + 1 ? text_size - excess - 1 : 1;
    }
    return text_size;
}

void elements_multiline_text_aligned(
    Canvas* c,
    int32_t x,
    int32_t y,
    Align h,
    Align v,
    const char* text) {
    int lines = 0;
    uint8_t fh = current_font_height(c);
    for(const char* s = text; s[0];) {
        size_t n = max_chars_to_fit(c, h, s, x);
        lines++;
        s += n;
        s += s[0] == '\n' ? 1 : 0;
    }
    if(v == AlignBottom) y -= fh * (lines - 1);
    if(v == AlignCenter) y -= (fh * (lines - 1)) / 2;
    for(const char* s = text; s[0];) {
        size_t n = max_chars_to_fit(c, h, s, x);
        char line[170];
        if(s[n] == '\n' || s[n] == 0)
            snprintf(line, sizeof(line), "%.*s", (int)n, s);
        else
            snprintf(line, sizeof(line), "%.*s-", (int)n, s);
        canvas_draw_str_aligned(c, x, y, h, v, line);
        y += fh;
        if(y > SCREEN_H) break;
        s += n;
        s += s[0] == '\n' ? 1 : 0;
    }
}

void elements_slightly_rounded_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    canvas_draw_rbox(c, x, y, w, h, 1);
}
void elements_slightly_rounded_frame(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    canvas_draw_rframe(c, x, y, w, h, 1);
}

/* firmware gui/elements.c (Unleashed unlshd-094): clip to `width`, scrolled by `scroll` glyphs */
void elements_scrollable_text_line_str(
    Canvas* c,
    uint8_t x,
    uint8_t y,
    uint8_t width,
    const char* string,
    size_t scroll,
    bool ellipsis,
    bool centered) {
    char line[128];
    snprintf(line, sizeof(line), "%s", string);
    size_t len_px = canvas_string_width(c, line);
    if(len_px > width) {
        if(centered) {
            centered = false;
            x -= width / 2;
        }
        if(ellipsis) width -= canvas_string_width(c, "...");
        size_t scroll_size = strlen(line), right_width = 0;
        for(size_t i = scroll_size - 1; i > 0; i--) {
            right_width += canvas_glyph_width(c, (uint8_t)line[i]);
            if(right_width > width) break;
            scroll_size--;
            if(!scroll_size) break;
        }
        if(scroll_size) {
            scroll_size += 3;
            scroll = scroll % scroll_size;
            size_t n = strlen(line);
            if(scroll > n) scroll = n;
            memmove(line, line + scroll, n - scroll + 1);
        }
        while(canvas_string_width(c, line) > width)
            line[strlen(line) - 1] = 0;
        if(ellipsis) strcat(line, "...");
    }
    if(centered)
        canvas_draw_str_aligned(c, x, y, AlignCenter, AlignBottom, line);
    else
        canvas_draw_str(c, x, y, line);
}

/* ---------------- views and the dispatcher ---------------- */

struct View {
    ViewDrawCallback draw;
    ViewInputCallback input;
    void* context;
    void* model;
};
View* view_alloc(void) {
    return calloc(1, sizeof(View));
}
void view_free(View* v) {
    free(v->model);
    free(v);
}
void view_set_context(View* v, void* context) {
    v->context = context;
}
void view_set_draw_callback(View* v, ViewDrawCallback cb) {
    v->draw = cb;
}
void view_set_input_callback(View* v, ViewInputCallback cb) {
    v->input = cb;
}
void view_allocate_model(View* v, ViewModelType type, size_t size) {
    UNUSED(type);
    v->model = calloc(1, size);
}
void* view_get_model(View* v) {
    return v->model;
}
void view_commit_model(View* v, bool update) {
    UNUSED(v);
    UNUSED(update);
}

#define MAX_VIEWS  16
#define QUEUE_SIZE 256
struct ViewDispatcher {
    View* views[MAX_VIEWS];
    uint32_t current;
    void* ctx;
    ViewDispatcherCustomEventCallback custom;
    ViewDispatcherNavigationEventCallback nav;
    uint32_t queue[QUEUE_SIZE];
    unsigned qh, qt;
    bool running;
};
static ViewDispatcher* the_vd;
static void (*the_script)(void);

ViewDispatcher* view_dispatcher_alloc(void) {
    ViewDispatcher* vd = calloc(1, sizeof(ViewDispatcher));
    vd->current = UINT32_MAX;
    the_vd = vd;
    return vd;
}
void view_dispatcher_free(ViewDispatcher* vd) {
    if(the_vd == vd) the_vd = NULL;
    free(vd);
}
void view_dispatcher_set_event_callback_context(ViewDispatcher* vd, void* context) {
    vd->ctx = context;
}
void view_dispatcher_set_custom_event_callback(
    ViewDispatcher* vd,
    ViewDispatcherCustomEventCallback cb) {
    vd->custom = cb;
}
void view_dispatcher_set_navigation_event_callback(
    ViewDispatcher* vd,
    ViewDispatcherNavigationEventCallback cb) {
    vd->nav = cb;
}
void view_dispatcher_add_view(ViewDispatcher* vd, uint32_t id, View* v) {
    furi_check(id < MAX_VIEWS && !vd->views[id]);
    vd->views[id] = v;
}
void view_dispatcher_remove_view(ViewDispatcher* vd, uint32_t id) {
    furi_check(id < MAX_VIEWS && vd->views[id]);
    vd->views[id] = NULL;
}
void view_dispatcher_switch_to_view(ViewDispatcher* vd, uint32_t id) {
    furi_check(id < MAX_VIEWS && vd->views[id]);
    vd->current = id;
}
void view_dispatcher_send_custom_event(ViewDispatcher* vd, uint32_t event) {
    furi_check((vd->qt + 1) % QUEUE_SIZE != vd->qh);
    vd->queue[vd->qt] = event;
    vd->qt = (vd->qt + 1) % QUEUE_SIZE;
}
void view_dispatcher_attach_to_gui(ViewDispatcher* vd, Gui* gui, ViewDispatcherType type) {
    UNUSED(vd);
    UNUSED(gui);
    UNUSED(type);
}
void view_dispatcher_stop(ViewDispatcher* vd) {
    vd->running = false;
}
static void pump(void) {
    ViewDispatcher* vd = the_vd;
    while(vd && vd->running && vd->qh != vd->qt) {
        uint32_t e = vd->queue[vd->qh];
        vd->qh = (vd->qh + 1) % QUEUE_SIZE;
        if(vd->custom) vd->custom(vd->ctx, e);
    }
}
void view_dispatcher_run(ViewDispatcher* vd) {
    vd->running = true;
    pump();
    if(the_script) the_script();
}

void fake_set_script(void (*script)(void)) {
    the_script = script;
}
bool fake_running(void) {
    return the_vd && the_vd->running;
}

static void send_input(InputKey key, InputType type) {
    ViewDispatcher* vd = the_vd;
    if(!fake_running()) fake_fail("input after the app stopped", __FILE__, __LINE__);
    View* v = vd->current < MAX_VIEWS ? vd->views[vd->current] : NULL;
    InputEvent ev = {.sequence = 1, .key = key, .type = type};
    bool consumed = v && v->input ? v->input(&ev, v->context) : false;
    if(!consumed && key == InputKeyBack && type == InputTypeShort && vd->nav) {
        if(!vd->nav(vd->ctx)) view_dispatcher_stop(vd);
    }
    pump();
}
void fake_press(InputKey key) {
    send_input(key, InputTypeShort);
}
void fake_long(InputKey key) {
    send_input(key, InputTypeLong);
}
void fake_advance(uint32_t ms) {
    uint32_t end = now_ms + ms;
    while(now_ms < end && fake_running()) {
        now_ms += 1;
        for(int i = 0; i < MAX_TIMERS; i++) {
            FuriTimer* t = timers[i];
            if(t && t->running && (int32_t)(now_ms - t->next) >= 0) {
                t->next += t->period;
                if(t->type == FuriTimerTypeOnce) t->running = false;
                t->cb(t->ctx);
            }
        }
        pump();
    }
}

const Frame* fake_render(void) {
    ViewDispatcher* vd = the_vd;
    View* v = vd && vd->current < MAX_VIEWS ? vd->views[vd->current] : NULL;
    memset(&frame, 0, sizeof(frame));
    frame.view_id = vd ? vd->current : UINT32_MAX;
    canvas_obj = (Canvas){.font = FontSecondary, .color = ColorBlack};
    role = RoleOwn;
    multiline_split = false;
    if(v && v->draw) v->draw(&canvas_obj, v->model);
    if(multiline_split && frame.n_fit_cut < 8)
        snprintf(frame.fit_cut[frame.n_fit_cut++], 64, "%s", "\x02stock multiline wrap");
    return &frame;
}
uint32_t fake_current_view(void) {
    return the_vd ? the_vd->current : UINT32_MAX;
}

/* ---------------- scene manager ---------------- */

#define MAX_SCENES 64
struct SceneManager {
    const SceneManagerHandlers* h;
    void* ctx;
    uint32_t stack[64];
    int depth;
    uint32_t state[MAX_SCENES];
};
static SceneManager* the_sm;
static uint32_t visits[MAX_SCENES];

SceneManager* scene_manager_alloc(const SceneManagerHandlers* handlers, void* context) {
    SceneManager* sm = calloc(1, sizeof(SceneManager));
    sm->h = handlers;
    sm->ctx = context;
    furi_check(handlers->scene_num <= MAX_SCENES);
    the_sm = sm;
    return sm;
}
void scene_manager_free(SceneManager* sm) {
    if(the_sm == sm) the_sm = NULL;
    free(sm);
}
void scene_manager_set_scene_state(SceneManager* sm, uint32_t id, uint32_t state) {
    sm->state[id] = state;
}
uint32_t scene_manager_get_scene_state(const SceneManager* sm, uint32_t id) {
    return sm->state[id];
}
static void enter(SceneManager* sm, uint32_t id) {
    visits[id]++;
    sm->h->on_enter_handlers[id](sm->ctx);
}
bool scene_manager_handle_custom_event(SceneManager* sm, uint32_t event) {
    if(!sm->depth) return false;
    SceneManagerEvent e = {.type = SceneManagerEventTypeCustom, .event = event};
    return sm->h->on_event_handlers[sm->stack[sm->depth - 1]](sm->ctx, e);
}
bool scene_manager_handle_back_event(SceneManager* sm) {
    if(!sm->depth) return false;
    SceneManagerEvent e = {.type = SceneManagerEventTypeBack, .event = 0};
    bool consumed = sm->h->on_event_handlers[sm->stack[sm->depth - 1]](sm->ctx, e);
    if(!consumed) consumed = scene_manager_previous_scene(sm);
    return consumed;
}
void scene_manager_next_scene(SceneManager* sm, uint32_t id) {
    if(sm->depth) sm->h->on_exit_handlers[sm->stack[sm->depth - 1]](sm->ctx);
    furi_check(sm->depth < 64);
    sm->stack[sm->depth++] = id;
    enter(sm, id);
}
bool scene_manager_previous_scene(SceneManager* sm) {
    if(!sm->depth) return false;
    uint32_t cur = sm->stack[--sm->depth];
    sm->h->on_exit_handlers[cur](sm->ctx);
    if(!sm->depth) return false;
    enter(sm, sm->stack[sm->depth - 1]);
    return true;
}
bool scene_manager_search_and_switch_to_previous_scene(SceneManager* sm, uint32_t id) {
    int i = sm->depth - 1;
    while(i >= 0 && sm->stack[i] != id)
        i--;
    if(i < 0) return false;
    sm->h->on_exit_handlers[sm->stack[sm->depth - 1]](sm->ctx);
    sm->depth = i + 1;
    enter(sm, id);
    return true;
}
uint32_t fake_current_scene(void) {
    return the_sm && the_sm->depth ? the_sm->stack[the_sm->depth - 1] : UINT32_MAX;
}
uint32_t fake_scene_visits(uint32_t id) {
    return id < MAX_SCENES ? visits[id] : 0;
}

/* ---------------- Submenu (firmware gui/modules/submenu.c) ---------------- */

#define SM_ITEMS 40
typedef struct {
    char label[64];
    uint32_t index;
    SubmenuItemCallback cb;
    SubmenuItemCallbackEx cbex;
    void* ctx;
} SmItem;
struct Submenu {
    View* view;
    SmItem items[SM_ITEMS];
    int count, position, window;
    char header[64];
};
static int sm_on_screen(const Submenu* s) {
    return s->header[0] ? 3 : 4;
}
/* Stock lists scroll with one row of lookahead (submenu_process_up/down) and open a
 * selected item one row below the top (submenu_set_selected_item). */
static void list_up(int* pos, int* win, int count, int on_screen) {
    if(*pos > 0) {
        (*pos)--;
        if(*pos - *win < 1 && *win > 0) (*win)--;
    } else {
        *pos = count - 1;
        if(*pos > on_screen - 1) *win = *pos - (on_screen - 1);
    }
}
static void list_down(int* pos, int* win, int count, int on_screen) {
    if(*pos < count - 1) {
        (*pos)++;
        if(*pos - *win > on_screen - 2 && *win <= count - on_screen - 1) (*win)++;
    } else {
        *pos = 0;
        *win = 0;
    }
}
static void list_select(int pos, int* win, int count, int on_screen) {
    *win = pos > 0 ? pos - 1 : 0;
    if(count <= on_screen)
        *win = 0;
    else if(*win > count - on_screen)
        *win = count - on_screen;
}
static void submenu_draw(Canvas* c, void* model) {
    Submenu* s = *(Submenu**)model;
    role = RoleStock;
    const int item_height = 16, item_width = SCREEN_W - 5;
    canvas_clear(c);
    if(s->header[0]) {
        canvas_set_font(c, FontPrimary);
        role = RoleOwn; /* the app chooses this text: no technical tokens */
        canvas_draw_str(c, 4, 11, s->header);
        role = RoleStock;
    }
    canvas_set_font(c, FontSecondary);
    int y_offset = s->header[0] ? 16 : 0;
    for(int i = 0; i < s->count; i++) {
        int pos = i - s->window;
        if(pos < 0 || pos >= sm_on_screen(s)) continue;
        if(i == s->position) {
            canvas_set_color(c, ColorBlack);
            elements_slightly_rounded_box(
                c, 0, y_offset + pos * item_height + 1, item_width, item_height - 2);
            canvas_set_color(c, ColorWhite);
        } else {
            canvas_set_color(c, ColorBlack);
        }
        FuriString* d = furi_string_alloc_set(s->items[i].label);
        elements_string_fit_width(c, d, item_width - 6 * 2);
        canvas_draw_str(
            c, 6, y_offset + pos * item_height + item_height - 4, furi_string_get_cstr(d));
        furi_string_free(d);
    }
    canvas_set_color(c, ColorBlack);
    elements_scrollbar(c, s->position, s->count);
    role = RoleOwn;
}
static bool submenu_input(InputEvent* e, void* context) {
    Submenu* s = context;
    if(e->key == InputKeyBack || !s->count) return false;
    if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
        if(e->key == InputKeyUp) list_up(&s->position, &s->window, s->count, sm_on_screen(s));
        if(e->key == InputKeyDown) list_down(&s->position, &s->window, s->count, sm_on_screen(s));
    }
    if(e->key == InputKeyOk && (e->type == InputTypeShort || e->type == InputTypeLong)) {
        SmItem* it = &s->items[s->position];
        if(it->cbex)
            it->cbex(it->ctx, e->type, it->index);
        else if(it->cb && e->type == InputTypeShort)
            it->cb(it->ctx, it->index);
    }
    return true;
}
Submenu* submenu_alloc(void) {
    Submenu* s = calloc(1, sizeof(Submenu));
    s->view = view_alloc();
    view_allocate_model(s->view, ViewModelTypeLocking, sizeof(Submenu*));
    *(Submenu**)s->view->model = s;
    view_set_context(s->view, s);
    view_set_draw_callback(s->view, submenu_draw);
    view_set_input_callback(s->view, submenu_input);
    return s;
}
void submenu_free(Submenu* s) {
    view_free(s->view);
    free(s);
}
View* submenu_get_view(Submenu* s) {
    return s->view;
}
void submenu_reset(Submenu* s) {
    s->count = s->position = s->window = 0;
    s->header[0] = 0;
}
void submenu_set_header(Submenu* s, const char* header) {
    snprintf(s->header, sizeof(s->header), "%s", header ? header : "");
}
static SmItem* sm_add(Submenu* s, const char* label, uint32_t index, void* ctx) {
    furi_check(s->count < SM_ITEMS);
    SmItem* it = &s->items[s->count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->label, sizeof(it->label), "%s", label);
    it->index = index;
    it->ctx = ctx;
    return it;
}
void submenu_add_item(
    Submenu* s,
    const char* label,
    uint32_t index,
    SubmenuItemCallback cb,
    void* ctx) {
    sm_add(s, label, index, ctx)->cb = cb;
}
void submenu_add_item_ex(
    Submenu* s,
    const char* label,
    uint32_t index,
    SubmenuItemCallbackEx cb,
    void* ctx) {
    sm_add(s, label, index, ctx)->cbex = cb;
}
void submenu_change_item_label(Submenu* s, uint32_t index, const char* label) {
    for(int i = 0; i < s->count; i++)
        if(s->items[i].index == index)
            snprintf(s->items[i].label, sizeof(s->items[i].label), "%s", label);
}
uint32_t submenu_get_selected_item(Submenu* s) {
    return s->position < s->count ? s->items[s->position].index : 0;
}
void submenu_set_selected_item(Submenu* s, uint32_t index) {
    for(int i = 0; i < s->count; i++)
        if(s->items[i].index == index) s->position = i;
    list_select(s->position, &s->window, s->count, sm_on_screen(s));
}

/* ---------------- VariableItemList (firmware gui/modules/variable_item_list.c) ---------------- */

#define VL_ITEMS 24
struct VariableItem {
    char label[96]; /* a FuriString on the device: no limit */
    char value[48];
    uint8_t count, index;
    VariableItemChangeCallback cb;
    void* ctx;
};
struct VariableItemList {
    View* view;
    VariableItem items[VL_ITEMS];
    int count, position, window;
    size_t scroll_counter; /* advanced every 333 ms; the selected row scrolls */
    FuriTimer* scroll_timer;
    VariableItemListEnterCallback enter;
    void* enter_ctx;
};
/* firmware gui/modules/variable_item_list.c, Unleashed unlshd-094 (OFW 1.5.1 draws label
 * and value whole, without scrolling; the app pre-fits values there, scene_common.c) */
static void vl_draw(Canvas* c, void* model) {
    VariableItemList* l = *(VariableItemList**)model;
    role = RoleStock;
    const int item_height = 16, item_width = SCREEN_W - 5;
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    for(int i = 0; i < l->count; i++) {
        int pos = i - l->window;
        if(pos < 0 || pos >= 4) continue;
        VariableItem* it = &l->items[i];
        int item_y = pos * item_height, text_y = item_y + item_height - 4;
        size_t scroll = 0;
        if(i == l->position) {
            canvas_set_color(c, ColorBlack);
            elements_slightly_rounded_box(c, 0, item_y + 1, item_width, item_height - 2);
            canvas_set_color(c, ColorWhite);
            scroll = l->scroll_counter ? l->scroll_counter - 1 : 0;
        } else {
            canvas_set_color(c, ColorBlack);
        }
        uint8_t tx = 73, tw = 66;
        if(it->index == 0 && !it->value[0]) {
            canvas_draw_str(c, 6, text_y, it->label);
        } else {
            if(strlen(it->value) < 4) {
                tx = 80;
                tw = 71;
            }
            elements_scrollable_text_line_str(c, 6, text_y, tw, it->label, scroll, false, false);
        }
        if(it->index > 0) canvas_draw_str(c, tx, text_y, "<");
        elements_scrollable_text_line_str(
            c, (115 + tx) / 2 + 1, text_y, 37, it->value, scroll, false, true);
        if(it->index + 1 < it->count) canvas_draw_str(c, 115, text_y, ">");
    }
    canvas_set_color(c, ColorBlack);
    elements_scrollbar(c, l->position, l->count);
    role = RoleOwn;
}
static void vl_scroll_cb(void* context) {
    VariableItemList* l = context;
    l->scroll_counter++;
}
static bool vl_input(InputEvent* e, void* context) {
    VariableItemList* l = context;
    if(e->key == InputKeyBack || !l->count) return false;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return true;
    VariableItem* it = &l->items[l->position];
    if(e->key != InputKeyOk) l->scroll_counter = 0; /* stock: a move restarts the marquee */
    switch(e->key) {
    case InputKeyUp:
        list_up(&l->position, &l->window, l->count, 4);
        break;
    case InputKeyDown:
        list_down(&l->position, &l->window, l->count, 4);
        break;
    case InputKeyLeft:
        if(it->index > 0) {
            it->index--;
            if(it->cb) it->cb(it);
        }
        break;
    case InputKeyRight:
        if(it->index + 1 < it->count) {
            it->index++;
            if(it->cb) it->cb(it);
        }
        break;
    case InputKeyOk:
        if(e->type == InputTypeShort && l->enter) l->enter(l->enter_ctx, (uint32_t)l->position);
        break;
    default:
        break;
    }
    return true;
}
VariableItemList* variable_item_list_alloc(void) {
    VariableItemList* l = calloc(1, sizeof(VariableItemList));
    l->view = view_alloc();
    view_allocate_model(l->view, ViewModelTypeLocking, sizeof(VariableItemList*));
    *(VariableItemList**)l->view->model = l;
    view_set_context(l->view, l);
    view_set_draw_callback(l->view, vl_draw);
    view_set_input_callback(l->view, vl_input);
    l->scroll_timer = furi_timer_alloc(vl_scroll_cb, FuriTimerTypePeriodic, l);
    furi_timer_start(l->scroll_timer, 333);
    return l;
}
void variable_item_list_free(VariableItemList* l) {
    furi_timer_stop(l->scroll_timer);
    furi_timer_free(l->scroll_timer);
    view_free(l->view);
    free(l);
}
View* variable_item_list_get_view(VariableItemList* l) {
    return l->view;
}
void variable_item_list_reset(VariableItemList* l) {
    /* stock: the items go, position and window stay (a list built next starts where the
     * last one was unless the scene selects an item) */
    l->count = 0;
    l->scroll_counter = 0;
    l->enter = NULL;
}
VariableItem* variable_item_list_add(
    VariableItemList* l,
    const char* label,
    uint8_t values_count,
    VariableItemChangeCallback cb,
    void* context) {
    furi_check(l->count < VL_ITEMS);
    VariableItem* it = &l->items[l->count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->label, sizeof(it->label), "%s", label);
    it->count = values_count;
    it->cb = cb;
    it->ctx = context;
    return it;
}
void variable_item_list_set_enter_callback(
    VariableItemList* l,
    VariableItemListEnterCallback cb,
    void* context) {
    l->enter = cb;
    l->enter_ctx = context;
}
void variable_item_list_set_selected_item(VariableItemList* l, uint8_t index) {
    l->position = index < l->count ? index : 0;
    list_select(l->position, &l->window, l->count, 4);
}
uint8_t variable_item_list_get_selected_item_index(VariableItemList* l) {
    return (uint8_t)l->position;
}
void variable_item_set_current_value_index(VariableItem* it, uint8_t index) {
    it->index = index;
}
void variable_item_set_current_value_text(VariableItem* it, const char* text) {
    snprintf(it->value, sizeof(it->value), "%s", text);
}
uint8_t variable_item_get_current_value_index(VariableItem* it) {
    return it->index;
}
void* variable_item_get_context(VariableItem* it) {
    return it->ctx;
}

/* ---------------- DialogEx, Popup, Widget, Loading ---------------- */

typedef struct {
    char text[160];
    uint8_t x, y;
    Align h, v;
} PlacedText;

struct DialogEx {
    View* view;
    PlacedText header, text;
    char left[24], center[24], right[24];
    DialogExResultCallback cb;
    void* ctx;
};
static void dialog_draw(Canvas* c, void* model) {
    DialogEx* d = *(DialogEx**)model;
    role = RoleStock;
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    if(d->header.text[0]) {
        canvas_set_font(c, FontPrimary);
        role = RoleOwn; /* placed by the app */
        elements_multiline_text_aligned(
            c, d->header.x, d->header.y, d->header.h, d->header.v, d->header.text);
    }
    if(d->text.text[0]) {
        canvas_set_font(c, FontSecondary);
        role = RoleOwn;
        elements_multiline_text_aligned(
            c, d->text.x, d->text.y, d->text.h, d->text.v, d->text.text);
    }
    canvas_set_font(c, FontSecondary);
    if(d->left[0]) elements_button_left(c, d->left);
    if(d->center[0]) elements_button_center(c, d->center);
    if(d->right[0]) elements_button_right(c, d->right);
    role = RoleOwn;
}
static bool dialog_input(InputEvent* e, void* context) {
    DialogEx* d = context;
    if(e->type != InputTypeShort || !d->cb) return false;
    if(e->key == InputKeyLeft && d->left[0])
        d->cb(DialogExResultLeft, d->ctx);
    else if(e->key == InputKeyRight && d->right[0])
        d->cb(DialogExResultRight, d->ctx);
    else if(e->key == InputKeyOk && d->center[0])
        d->cb(DialogExResultCenter, d->ctx);
    else
        return false;
    return true;
}
DialogEx* dialog_ex_alloc(void) {
    DialogEx* d = calloc(1, sizeof(DialogEx));
    d->view = view_alloc();
    view_allocate_model(d->view, ViewModelTypeLocking, sizeof(DialogEx*));
    *(DialogEx**)d->view->model = d;
    view_set_context(d->view, d);
    view_set_draw_callback(d->view, dialog_draw);
    view_set_input_callback(d->view, dialog_input);
    return d;
}
void dialog_ex_free(DialogEx* d) {
    view_free(d->view);
    free(d);
}
View* dialog_ex_get_view(DialogEx* d) {
    return d->view;
}
void dialog_ex_reset(DialogEx* d) {
    View* v = d->view;
    memset(d, 0, sizeof(*d));
    d->view = v;
}
void dialog_ex_set_context(DialogEx* d, void* context) {
    d->ctx = context;
}
void dialog_ex_set_result_callback(DialogEx* d, DialogExResultCallback cb) {
    d->cb = cb;
}
static void place(PlacedText* p, const char* text, uint8_t x, uint8_t y, Align h, Align v) {
    snprintf(p->text, sizeof(p->text), "%s", text ? text : "");
    p->x = x;
    p->y = y;
    p->h = h;
    p->v = v;
}
void dialog_ex_set_header(DialogEx* d, const char* text, uint8_t x, uint8_t y, Align h, Align v) {
    place(&d->header, text, x, y, h, v);
}
void dialog_ex_set_text(DialogEx* d, const char* text, uint8_t x, uint8_t y, Align h, Align v) {
    place(&d->text, text, x, y, h, v);
}
void dialog_ex_set_left_button_text(DialogEx* d, const char* text) {
    snprintf(d->left, sizeof(d->left), "%s", text ? text : "");
}
void dialog_ex_set_center_button_text(DialogEx* d, const char* text) {
    snprintf(d->center, sizeof(d->center), "%s", text ? text : "");
}
void dialog_ex_set_right_button_text(DialogEx* d, const char* text) {
    snprintf(d->right, sizeof(d->right), "%s", text ? text : "");
}

struct Popup {
    View* view;
    PlacedText header, text;
    const Icon* icon;
    uint8_t ix, iy;
};
static void popup_draw(Canvas* c, void* model) {
    Popup* p = *(Popup**)model;
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    if(p->icon) canvas_draw_icon(c, p->ix, p->iy, p->icon);
    if(p->header.text[0]) {
        canvas_set_font(c, FontPrimary);
        elements_multiline_text_aligned(
            c, p->header.x, p->header.y, p->header.h, p->header.v, p->header.text);
    }
    if(p->text.text[0]) {
        canvas_set_font(c, FontSecondary);
        elements_multiline_text_aligned(
            c, p->text.x, p->text.y, p->text.h, p->text.v, p->text.text);
    }
}
static bool popup_input(InputEvent* e, void* context) {
    UNUSED(e);
    UNUSED(context);
    return false; /* no callback is ever set: keys pass to the scene */
}
Popup* popup_alloc(void) {
    Popup* p = calloc(1, sizeof(Popup));
    p->view = view_alloc();
    view_allocate_model(p->view, ViewModelTypeLocking, sizeof(Popup*));
    *(Popup**)p->view->model = p;
    view_set_context(p->view, p);
    view_set_draw_callback(p->view, popup_draw);
    view_set_input_callback(p->view, popup_input);
    return p;
}
void popup_free(Popup* p) {
    view_free(p->view);
    free(p);
}
View* popup_get_view(Popup* p) {
    return p->view;
}
void popup_reset(Popup* p) {
    View* v = p->view;
    memset(p, 0, sizeof(*p));
    p->view = v;
}
void popup_set_header(Popup* p, const char* text, uint8_t x, uint8_t y, Align h, Align v) {
    place(&p->header, text, x, y, h, v);
}
void popup_set_text(Popup* p, const char* text, uint8_t x, uint8_t y, Align h, Align v) {
    place(&p->text, text, x, y, h, v);
}
void popup_set_icon(Popup* p, uint8_t x, uint8_t y, const Icon* icon) {
    p->icon = icon;
    p->ix = x;
    p->iy = y;
}

#define WIDGET_ELEMENTS 12
typedef struct {
    int kind; /* 0 string, 1 multiline, 2 button */
    PlacedText t;
    Font font;
    GuiButtonType button;
    ButtonCallback cb;
    void* ctx;
} WidgetElement;
struct Widget {
    View* view;
    WidgetElement el[WIDGET_ELEMENTS];
    int count;
};
static void widget_draw(Canvas* c, void* model) {
    Widget* w = *(Widget**)model;
    canvas_clear(c);
    for(int i = 0; i < w->count; i++) {
        WidgetElement* e = &w->el[i];
        canvas_set_color(c, ColorBlack);
        if(e->kind == 0) {
            canvas_set_font(c, e->font);
            canvas_draw_str_aligned(c, e->t.x, e->t.y, e->t.h, e->t.v, e->t.text);
        } else if(e->kind == 1) {
            canvas_set_font(c, e->font);
            elements_multiline_text_aligned(c, e->t.x, e->t.y, e->t.h, e->t.v, e->t.text);
        } else {
            canvas_set_font(c, FontSecondary);
            if(e->button == GuiButtonTypeLeft) elements_button_left(c, e->t.text);
            if(e->button == GuiButtonTypeCenter) elements_button_center(c, e->t.text);
            if(e->button == GuiButtonTypeRight) elements_button_right(c, e->t.text);
        }
    }
}
static bool widget_input(InputEvent* e, void* context) {
    Widget* w = context;
    GuiButtonType want = e->key == InputKeyLeft  ? GuiButtonTypeLeft :
                         e->key == InputKeyRight ? GuiButtonTypeRight :
                         e->key == InputKeyOk    ? GuiButtonTypeCenter :
                                                   (GuiButtonType)-1;
    for(int i = 0; i < w->count; i++)
        if(w->el[i].kind == 2 && w->el[i].button == want && w->el[i].cb) {
            w->el[i].cb(want, e->type, w->el[i].ctx);
            return true;
        }
    return false;
}
Widget* widget_alloc(void) {
    Widget* w = calloc(1, sizeof(Widget));
    w->view = view_alloc();
    view_allocate_model(w->view, ViewModelTypeLocking, sizeof(Widget*));
    *(Widget**)w->view->model = w;
    view_set_context(w->view, w);
    view_set_draw_callback(w->view, widget_draw);
    view_set_input_callback(w->view, widget_input);
    return w;
}
void widget_free(Widget* w) {
    view_free(w->view);
    free(w);
}
View* widget_get_view(Widget* w) {
    return w->view;
}
void widget_reset(Widget* w) {
    w->count = 0;
}
static WidgetElement* widget_add(Widget* w) {
    furi_check(w->count < WIDGET_ELEMENTS);
    WidgetElement* e = &w->el[w->count++];
    memset(e, 0, sizeof(*e));
    return e;
}
void widget_add_string_element(
    Widget* w,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v,
    Font font,
    const char* text) {
    WidgetElement* e = widget_add(w);
    place(&e->t, text, x, y, h, v);
    e->font = font;
}
void widget_add_string_multiline_element(
    Widget* w,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v,
    Font font,
    const char* text) {
    WidgetElement* e = widget_add(w);
    e->kind = 1;
    place(&e->t, text, x, y, h, v);
    e->font = font;
}
void widget_add_button_element(
    Widget* w,
    GuiButtonType type,
    const char* text,
    ButtonCallback cb,
    void* ctx) {
    WidgetElement* e = widget_add(w);
    e->kind = 2;
    place(&e->t, text, 0, 0, AlignLeft, AlignTop);
    e->button = type;
    e->cb = cb;
    e->ctx = ctx;
}

struct Loading {
    View* view;
};
static void loading_draw(Canvas* c, void* model) {
    UNUSED(model);
    canvas_clear(c);
    canvas_draw_icon(c, (SCREEN_W - 24) / 2, (SCREEN_H - 24) / 2, &I_Loading_24);
}
Loading* loading_alloc(void) {
    Loading* l = calloc(1, sizeof(Loading));
    l->view = view_alloc();
    view_set_draw_callback(l->view, loading_draw);
    return l;
}
void loading_free(Loading* l) {
    view_free(l->view);
    free(l);
}
View* loading_get_view(Loading* l) {
    return l->view;
}

/* ---------------- in-memory SD card ---------------- */

#define FS_NODES 96
typedef struct {
    char path[128];
    bool dir;
    char* data;
    size_t len;
} FsNode;
static FsNode fs[FS_NODES];

static FsNode* fs_find(const char* path) {
    for(int i = 0; i < FS_NODES; i++)
        if(fs[i].path[0] && !strcmp(fs[i].path, path)) return &fs[i];
    return NULL;
}
static FsNode* fs_new(const char* path, bool dir) {
    for(int i = 0; i < FS_NODES; i++)
        if(!fs[i].path[0]) {
            snprintf(fs[i].path, sizeof(fs[i].path), "%s", path);
            fs[i].dir = dir;
            fs[i].data = NULL;
            fs[i].len = 0;
            return &fs[i];
        }
    fake_fail("fake SD card full", __FILE__, __LINE__);
    return NULL;
}
static bool fs_parent_exists(const char* path) {
    char p[128];
    snprintf(p, sizeof(p), "%s", path);
    char* slash = strrchr(p, '/');
    if(!slash || slash == p) return true;
    *slash = 0;
    if(!strcmp(p, "/ext")) return true;
    FsNode* n = fs_find(p);
    return n && n->dir;
}

struct File {
    FsNode* node;
    size_t pos;
    bool is_dir;
    int dir_iter;
    char dir_path[128];
};
bool file_info_is_dir(const FileInfo* info) {
    return info->flags & 1;
}
File* storage_file_alloc(Storage* s) {
    UNUSED(s);
    return calloc(1, sizeof(File));
}
void storage_file_free(File* f) {
    free(f);
}
bool storage_file_open(File* f, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    UNUSED(access);
    FsNode* n = fs_find(path);
    if(n && n->dir) return false;
    if(!n) {
        if(mode == FSOM_OPEN_EXISTING || !fs_parent_exists(path)) return false;
        n = fs_new(path, false);
    } else if(mode == FSOM_CREATE_NEW) {
        return false;
    }
    if(mode == FSOM_CREATE_ALWAYS) n->len = 0;
    f->node = n;
    f->pos = mode == FSOM_OPEN_APPEND ? n->len : 0;
    return true;
}
bool storage_file_close(File* f) {
    f->node = NULL;
    return true;
}
size_t storage_file_read(File* f, void* buf, size_t size) {
    if(!f->node) return 0;
    size_t n = f->pos < f->node->len ? MIN(size, f->node->len - f->pos) : 0;
    memcpy(buf, f->node->data + f->pos, n);
    f->pos += n;
    return n;
}
size_t storage_file_write(File* f, const void* buf, size_t size) {
    if(!f->node) return 0;
    FsNode* n = f->node;
    if(f->pos + size > n->len) {
        n->data = realloc(n->data, f->pos + size);
        n->len = f->pos + size;
    }
    memcpy(n->data + f->pos, buf, size);
    f->pos += size;
    return size;
}
bool storage_file_seek(File* f, uint32_t offset, bool from_start) {
    if(!f->node) return false;
    size_t p = from_start ? offset : f->pos + offset;
    f->pos = MIN(p, f->node->len);
    return true;
}
uint64_t storage_file_tell(File* f) {
    return f->pos;
}
uint64_t storage_file_size(File* f) {
    return f->node ? f->node->len : 0;
}
bool storage_file_exists(Storage* s, const char* path) {
    UNUSED(s);
    FsNode* n = fs_find(path);
    return n && !n->dir;
}
bool storage_dir_open(File* f, const char* path) {
    FsNode* n = fs_find(path);
    if(!n || !n->dir) return false;
    f->is_dir = true;
    f->dir_iter = 0;
    snprintf(f->dir_path, sizeof(f->dir_path), "%s", path);
    return true;
}
bool storage_dir_close(File* f) {
    f->is_dir = false;
    return true;
}
bool storage_dir_read(File* f, FileInfo* info, char* name, uint16_t name_size) {
    size_t pl = strlen(f->dir_path);
    for(; f->dir_iter < FS_NODES; f->dir_iter++) {
        FsNode* n = &fs[f->dir_iter];
        if(!n->path[0] || strncmp(n->path, f->dir_path, pl) || n->path[pl] != '/') continue;
        if(strchr(n->path + pl + 1, '/')) continue; /* not a direct child */
        if(info) {
            info->flags = n->dir ? 1 : 0;
            info->size = n->len;
        }
        if(name) snprintf(name, name_size, "%s", n->path + pl + 1);
        f->dir_iter++;
        return true;
    }
    return false;
}
bool storage_simply_mkdir(Storage* s, const char* path) {
    UNUSED(s);
    FsNode* n = fs_find(path);
    if(n) return n->dir;
    if(!fs_parent_exists(path)) return false;
    fs_new(path, true);
    return true;
}
bool storage_simply_remove(Storage* s, const char* path) {
    UNUSED(s);
    FsNode* n = fs_find(path);
    if(!n) return true;
    free(n->data);
    memset(n, 0, sizeof(*n));
    return true;
}
FS_Error storage_common_rename(Storage* s, const char* old_path, const char* new_path) {
    UNUSED(s);
    FsNode* n = fs_find(old_path);
    if(!n) return FSE_NOT_EXIST;
    if(fs_find(new_path)) return FSE_EXIST;
    snprintf(n->path, sizeof(n->path), "%s", new_path);
    return FSE_OK;
}
void fake_fs_mkdir(const char* path) {
    storage_simply_mkdir(NULL, path);
}
bool fake_fs_exists(const char* path) {
    return fs_find(path) != NULL;
}
void fake_fs_write(const char* path, const char* data) {
    File f = {0};
    furi_check(storage_file_open(&f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS));
    storage_file_write(&f, data, strlen(data));
}

const char* fake_fs_read(const char* path) {
    static char buf[8192];
    File f = {0};
    if(!storage_file_open(&f, path, FSAM_READ, FSOM_OPEN_EXISTING)) return NULL;
    size_t n = storage_file_read(&f, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    return buf;
}

void fake_fs_remove(const char* path) {
    storage_simply_remove(NULL, path);
}

/* ---------------- entry ---------------- */

int32_t ssh_runner_app(void* p);
int fake_app_main(void) {
    return ssh_runner_app(NULL);
}
