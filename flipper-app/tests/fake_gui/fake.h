/* Host stand-in for the parts of the Flipper SDK the app uses, for the render test.
 *
 * Every SDK header the app includes (furi.h, gui/..., storage/storage.h, ...) is a one-line
 * file in this folder that includes this one. The canvas records what is drawn (text with
 * real glyph metrics from font_metrics.h, boxes, frames, lines, icons) instead of pixels;
 * the stock modules (Submenu, VariableItemList, DialogEx, Popup, Widget, Loading) and
 * elements are re-drawn with the stock geometry, so their text lands where the firmware
 * puts it. SD card is in memory; time and timers are driven by the test.
 *
 * SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- furi ---------------- */

#define UNUSED(x)   (void)(x)
#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define furi_check(x)        ((x) ? (void)0 : fake_fail("furi_check", __FILE__, __LINE__))
#define furi_assert(x)       furi_check(x)
#define FURI_LOG_E(tag, ...) fake_log('E', tag, __VA_ARGS__)
#define FURI_LOG_W(tag, ...) fake_log('W', tag, __VA_ARGS__)
#define FURI_LOG_I(tag, ...) fake_log('I', tag, __VA_ARGS__)
#define FURI_LOG_D(tag, ...) fake_log('D', tag, __VA_ARGS__)
#define EXT_PATH(x)          "/ext/" x
#define APP_ASSETS_PATH(x)   "/assets/" x

void fake_fail(const char* what, const char* file, int line);
void fake_log(char level, const char* tag, const char* fmt, ...);

size_t fake_strlcat(char* dst, const char* src, size_t size);
#define strlcat fake_strlcat

typedef struct FuriString FuriString;
FuriString* furi_string_alloc(void);
FuriString* furi_string_alloc_set(const char* s);
void furi_string_free(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
void furi_string_set(FuriString* s, const char* v);
int furi_string_printf(FuriString* s, const char* fmt, ...);
void furi_string_cat(FuriString* s, const char* v);
void furi_string_reset(FuriString* s);
size_t furi_string_size(const FuriString* s);
void furi_string_left(FuriString* s, size_t index);

uint32_t furi_get_tick(void);
typedef void* FuriThreadId;
FuriThreadId furi_thread_get_current_id(void);
uint32_t furi_thread_get_stack_space(FuriThreadId thread_id);
uint32_t furi_kernel_get_tick_frequency(void);
uint32_t furi_ms_to_ticks(uint32_t ms);

typedef enum {
    FuriTimerTypeOnce,
    FuriTimerTypePeriodic
} FuriTimerType;
typedef void (*FuriTimerCallback)(void* context);
typedef struct FuriTimer FuriTimer;
FuriTimer* furi_timer_alloc(FuriTimerCallback cb, FuriTimerType type, void* context);
void furi_timer_free(FuriTimer* t);
int furi_timer_start(FuriTimer* t, uint32_t ticks);
int furi_timer_stop(FuriTimer* t);

#define RECORD_GUI          "gui"
#define RECORD_NOTIFICATION "notification"
#define RECORD_STORAGE      "storage"
#define RECORD_EXPANSION    "expansion"
void* furi_record_open(const char* name);
void furi_record_close(const char* name);

typedef struct {
    uint8_t hour, minute, second, day, month;
    uint16_t year;
    uint8_t weekday;
} DateTime;
void furi_hal_rtc_get_datetime(DateTime* dt);
typedef enum {
    FuriHalRtcFlagDebug = (1 << 0)
} FuriHalRtcFlag;
bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag);
void fake_set_debug(bool on); /* Settings > System > Debug */

/* ---------------- input ---------------- */

typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack
} InputKey;
typedef enum {
    InputTypePress,
    InputTypeRelease,
    InputTypeShort,
    InputTypeLong,
    InputTypeRepeat
} InputType;
typedef struct {
    uint32_t sequence;
    InputKey key;
    InputType type;
} InputEvent;

/* ---------------- canvas ---------------- */

typedef enum {
    ColorWhite = 0,
    ColorBlack = 1,
    ColorXOR = 2
} Color;
typedef enum {
    FontPrimary,
    FontSecondary,
    FontKeyboard,
    FontBigNumbers,
    FontTotalNumber
} Font;
typedef enum {
    AlignLeft,
    AlignRight,
    AlignTop,
    AlignBottom,
    AlignCenter
} Align;
typedef struct {
    uint8_t leading_default, leading_min, height, descender;
} CanvasFontParameters;

typedef struct Icon {
    const char* name;
    uint8_t width, height;
} Icon;

uint16_t icon_get_width(const Icon* icon);
uint16_t icon_get_height(const Icon* icon);

typedef struct Canvas Canvas;
void canvas_clear(Canvas* c);
void canvas_set_color(Canvas* c, Color color);
void canvas_set_font(Canvas* c, Font font);
const CanvasFontParameters* canvas_get_font_params(const Canvas* c, Font font);
uint16_t canvas_string_width(Canvas* c, const char* str);
uint8_t canvas_glyph_width(Canvas* c, uint16_t symbol);
void canvas_draw_str(Canvas* c, int32_t x, int32_t y, const char* str);
void canvas_draw_str_aligned(Canvas* c, int32_t x, int32_t y, Align h, Align v, const char* str);
void canvas_draw_glyph(Canvas* c, int32_t x, int32_t y, uint16_t ch);
void canvas_draw_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h);
void canvas_draw_rbox(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, size_t r);
void canvas_draw_frame(Canvas* c, int32_t x, int32_t y, size_t w, size_t h);
void canvas_draw_rframe(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, size_t r);
void canvas_draw_line(Canvas* c, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void canvas_draw_dot(Canvas* c, int32_t x, int32_t y);
void canvas_draw_icon(Canvas* c, int32_t x, int32_t y, const Icon* icon);

void elements_string_fit_width(Canvas* c, FuriString* s, size_t width);
void elements_scrollbar_pos(
    Canvas* c,
    int32_t x,
    int32_t y,
    size_t height,
    size_t pos,
    size_t total);
void elements_scrollbar(Canvas* c, size_t pos, size_t total);
void elements_button_left(Canvas* c, const char* str);
void elements_button_center(Canvas* c, const char* str);
void elements_button_right(Canvas* c, const char* str);
void elements_multiline_text_aligned(
    Canvas* c,
    int32_t x,
    int32_t y,
    Align h,
    Align v,
    const char* text);
void elements_slightly_rounded_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h);
void elements_slightly_rounded_frame(Canvas* c, int32_t x, int32_t y, size_t w, size_t h);
void elements_scrollable_text_line_str(
    Canvas* c,
    uint8_t x,
    uint8_t y,
    uint8_t width,
    const char* string,
    size_t scroll,
    bool ellipsis,
    bool centered);

/* ---------------- view ---------------- */

typedef struct View View;
typedef void (*ViewDrawCallback)(Canvas* canvas, void* model);
typedef bool (*ViewInputCallback)(InputEvent* event, void* context);
typedef enum {
    ViewModelTypeNone,
    ViewModelTypeLockFree,
    ViewModelTypeLocking
} ViewModelType;

View* view_alloc(void);
void view_free(View* v);
void view_set_context(View* v, void* context);
void view_set_draw_callback(View* v, ViewDrawCallback cb);
void view_set_input_callback(View* v, ViewInputCallback cb);
void view_allocate_model(View* v, ViewModelType type, size_t size);
void* view_get_model(View* v);
void view_commit_model(View* v, bool update);

#define with_view_model(view, type, code, update) \
    {                                             \
        type = view_get_model(view);              \
        {code};                                   \
        view_commit_model(view, update);          \
    }

typedef struct Gui Gui;
typedef enum {
    ViewDispatcherTypeDesktop,
    ViewDispatcherTypeWindow,
    ViewDispatcherTypeFullscreen
} ViewDispatcherType;
typedef struct ViewDispatcher ViewDispatcher;
typedef bool (*ViewDispatcherCustomEventCallback)(void* context, uint32_t event);
typedef bool (*ViewDispatcherNavigationEventCallback)(void* context);
ViewDispatcher* view_dispatcher_alloc(void);
void view_dispatcher_free(ViewDispatcher* vd);
void view_dispatcher_set_event_callback_context(ViewDispatcher* vd, void* context);
void view_dispatcher_set_custom_event_callback(
    ViewDispatcher* vd,
    ViewDispatcherCustomEventCallback cb);
void view_dispatcher_set_navigation_event_callback(
    ViewDispatcher* vd,
    ViewDispatcherNavigationEventCallback cb);
void view_dispatcher_add_view(ViewDispatcher* vd, uint32_t id, View* v);
void view_dispatcher_remove_view(ViewDispatcher* vd, uint32_t id);
void view_dispatcher_switch_to_view(ViewDispatcher* vd, uint32_t id);
void view_dispatcher_send_custom_event(ViewDispatcher* vd, uint32_t event);
void view_dispatcher_attach_to_gui(ViewDispatcher* vd, Gui* gui, ViewDispatcherType type);
void view_dispatcher_run(ViewDispatcher* vd);
void view_dispatcher_stop(ViewDispatcher* vd);

typedef enum {
    SceneManagerEventTypeCustom,
    SceneManagerEventTypeBack,
    SceneManagerEventTypeTick
} SceneManagerEventType;
typedef struct {
    SceneManagerEventType type;
    uint32_t event;
} SceneManagerEvent;
typedef void (*AppSceneOnEnterCallback)(void* context);
typedef bool (*AppSceneOnEventCallback)(void* context, SceneManagerEvent event);
typedef void (*AppSceneOnExitCallback)(void* context);
typedef struct {
    const AppSceneOnEnterCallback* on_enter_handlers;
    const AppSceneOnEventCallback* on_event_handlers;
    const AppSceneOnExitCallback* on_exit_handlers;
    const uint32_t scene_num;
} SceneManagerHandlers;
typedef struct SceneManager SceneManager;
SceneManager* scene_manager_alloc(const SceneManagerHandlers* handlers, void* context);
void scene_manager_free(SceneManager* sm);
void scene_manager_set_scene_state(SceneManager* sm, uint32_t scene_id, uint32_t state);
uint32_t scene_manager_get_scene_state(const SceneManager* sm, uint32_t scene_id);
bool scene_manager_handle_custom_event(SceneManager* sm, uint32_t event);
bool scene_manager_handle_back_event(SceneManager* sm);
void scene_manager_next_scene(SceneManager* sm, uint32_t scene_id);
bool scene_manager_previous_scene(SceneManager* sm);
bool scene_manager_search_and_switch_to_previous_scene(SceneManager* sm, uint32_t scene_id);

/* ---------------- stock modules ---------------- */

typedef enum {
    GuiButtonTypeLeft,
    GuiButtonTypeCenter,
    GuiButtonTypeRight
} GuiButtonType;
typedef void (*ButtonCallback)(GuiButtonType result, InputType type, void* context);

typedef struct Submenu Submenu;
typedef void (*SubmenuItemCallback)(void* context, uint32_t index);
typedef void (*SubmenuItemCallbackEx)(void* context, InputType input_type, uint32_t index);
Submenu* submenu_alloc(void);
void submenu_free(Submenu* s);
View* submenu_get_view(Submenu* s);
void submenu_reset(Submenu* s);
void submenu_set_header(Submenu* s, const char* header);
void submenu_add_item(
    Submenu* s,
    const char* label,
    uint32_t index,
    SubmenuItemCallback cb,
    void* ctx);
void submenu_add_item_ex(
    Submenu* s,
    const char* label,
    uint32_t index,
    SubmenuItemCallbackEx cb,
    void* ctx);
void submenu_change_item_label(Submenu* s, uint32_t index, const char* label);
uint32_t submenu_get_selected_item(Submenu* s);
void submenu_set_selected_item(Submenu* s, uint32_t index);

typedef struct VariableItemList VariableItemList;
typedef struct VariableItem VariableItem;
typedef void (*VariableItemChangeCallback)(VariableItem* item);
typedef void (*VariableItemListEnterCallback)(void* context, uint32_t index);
VariableItemList* variable_item_list_alloc(void);
void variable_item_list_free(VariableItemList* l);
View* variable_item_list_get_view(VariableItemList* l);
void variable_item_list_reset(VariableItemList* l);
VariableItem* variable_item_list_add(
    VariableItemList* l,
    const char* label,
    uint8_t values_count,
    VariableItemChangeCallback cb,
    void* context);
void variable_item_list_set_enter_callback(
    VariableItemList* l,
    VariableItemListEnterCallback cb,
    void* context);
void variable_item_list_set_selected_item(VariableItemList* l, uint8_t index);
uint8_t variable_item_list_get_selected_item_index(VariableItemList* l);
void variable_item_set_current_value_index(VariableItem* item, uint8_t index);
void variable_item_set_current_value_text(VariableItem* item, const char* text);
uint8_t variable_item_get_current_value_index(VariableItem* item);
void* variable_item_get_context(VariableItem* item);

typedef struct DialogEx DialogEx;
typedef enum {
    DialogExResultLeft,
    DialogExResultCenter,
    DialogExResultRight
} DialogExResult;
typedef void (*DialogExResultCallback)(DialogExResult result, void* context);
DialogEx* dialog_ex_alloc(void);
void dialog_ex_free(DialogEx* d);
View* dialog_ex_get_view(DialogEx* d);
void dialog_ex_reset(DialogEx* d);
void dialog_ex_set_context(DialogEx* d, void* context);
void dialog_ex_set_result_callback(DialogEx* d, DialogExResultCallback cb);
void dialog_ex_set_header(DialogEx* d, const char* text, uint8_t x, uint8_t y, Align h, Align v);
void dialog_ex_set_text(DialogEx* d, const char* text, uint8_t x, uint8_t y, Align h, Align v);
void dialog_ex_set_left_button_text(DialogEx* d, const char* text);
void dialog_ex_set_center_button_text(DialogEx* d, const char* text);
void dialog_ex_set_right_button_text(DialogEx* d, const char* text);

typedef struct Popup Popup;
typedef void (*PopupCallback)(void* context);
Popup* popup_alloc(void);
void popup_free(Popup* p);
View* popup_get_view(Popup* p);
void popup_reset(Popup* p);
void popup_set_header(Popup* p, const char* text, uint8_t x, uint8_t y, Align h, Align v);
void popup_set_text(Popup* p, const char* text, uint8_t x, uint8_t y, Align h, Align v);
void popup_set_icon(Popup* p, uint8_t x, uint8_t y, const Icon* icon);

typedef struct Widget Widget;
Widget* widget_alloc(void);
void widget_free(Widget* w);
View* widget_get_view(Widget* w);
void widget_reset(Widget* w);
void widget_add_string_element(
    Widget* w,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v,
    Font font,
    const char* text);
void widget_add_string_multiline_element(
    Widget* w,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v,
    Font font,
    const char* text);
void widget_add_button_element(
    Widget* w,
    GuiButtonType type,
    const char* text,
    ButtonCallback cb,
    void* ctx);

typedef struct Loading Loading;
Loading* loading_alloc(void);
void loading_free(Loading* l);
View* loading_get_view(Loading* l);

/* ---------------- heap, dialogs ---------------- */

size_t memmgr_get_free_heap(void);
size_t memmgr_heap_get_max_free_block(void);
size_t memmgr_get_minimum_free_heap(void);

#define RECORD_DIALOGS "dialogs"
typedef struct DialogsApp DialogsApp;
typedef struct DialogMessage DialogMessage;
typedef enum {
    DialogMessageButtonBack,
    DialogMessageButtonLeft,
    DialogMessageButtonCenter,
    DialogMessageButtonRight,
} DialogMessageButton;
DialogMessage* dialog_message_alloc(void);
void dialog_message_free(DialogMessage* m);
void dialog_message_set_header(
    DialogMessage* m,
    const char* text,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v);
void dialog_message_set_text(
    DialogMessage* m,
    const char* text,
    uint8_t x,
    uint8_t y,
    Align h,
    Align v);
void dialog_message_set_buttons(
    DialogMessage* m,
    const char* left,
    const char* center,
    const char* right);
DialogMessageButton dialog_message_show(DialogsApp* app, const DialogMessage* m);

/* ---------------- notification, expansion ---------------- */

typedef struct NotificationApp NotificationApp;
typedef struct {
    const char* name;
} NotificationSequence;
extern const NotificationSequence sequence_success, sequence_error, sequence_single_vibro,
    sequence_blink_stop, sequence_blink_start_blue;
void notification_message(NotificationApp* app, const NotificationSequence* seq);

/* ---------------- storage (in memory) ---------------- */

typedef struct Storage Storage;
typedef struct File File;
typedef enum {
    FSAM_READ = 1,
    FSAM_WRITE = 2,
    FSAM_READ_WRITE = 3
} FS_AccessMode;
typedef enum {
    FSOM_OPEN_EXISTING = 1,
    FSOM_OPEN_ALWAYS = 2,
    FSOM_OPEN_APPEND = 4,
    FSOM_CREATE_NEW = 8,
    FSOM_CREATE_ALWAYS = 16,
} FS_OpenMode;
typedef struct {
    uint32_t flags;
    uint64_t size;
} FileInfo;
bool file_info_is_dir(const FileInfo* info);
File* storage_file_alloc(Storage* s);
void storage_file_free(File* f);
bool storage_file_open(File* f, const char* path, FS_AccessMode access, FS_OpenMode mode);
bool storage_file_close(File* f);
size_t storage_file_read(File* f, void* buf, size_t size);
size_t storage_file_write(File* f, const void* buf, size_t size);
bool storage_file_seek(File* f, uint32_t offset, bool from_start);
uint64_t storage_file_tell(File* f);
uint64_t storage_file_size(File* f);
bool storage_file_exists(Storage* s, const char* path);
bool storage_dir_open(File* f, const char* path);
bool storage_dir_close(File* f);
bool storage_dir_read(File* f, FileInfo* info, char* name, uint16_t name_size);
bool storage_simply_mkdir(Storage* s, const char* path);
bool storage_simply_remove(Storage* s, const char* path);
typedef enum {
    FSE_OK = 0,
    FSE_NOT_EXIST = 2,
    FSE_EXIST = 3
} FS_Error;
FS_Error storage_common_rename(Storage* s, const char* old_path, const char* new_path);

/* ---------------- icons (images/, plus the firmware ones the stock modules use) ---------------- */

extern const Icon I_WifiOn_9x8, I_Busy0_5x8, I_Busy1_5x8, I_Busy2_5x8, I_Busy3_5x8, I_Busy4_5x8,
    I_Busy5_5x8, I_Back_8x7, I_LanOn_9x8, I_Alert_9x8, I_Lock_7x8, I_Key_10x10, I_KeySave_24x11,
    I_KeySaveSelected_24x11, I_KeyBackspace_16x9, I_KeyBackspaceSelected_16x9, I_Spinner0_13x13,
    I_Spinner1_13x13, I_Spinner2_13x13, I_Spinner3_13x13, I_Spinner4_13x13, I_Spinner5_13x13,
    I_Spinner6_13x13, I_Spinner7_13x13;
