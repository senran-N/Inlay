#include "inlay_internal.h"
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_stdinc.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if INLAY_ENABLE_SCRIPT
#include <quickjs.h>
namespace {
constexpr uint32_t none = UINT32_MAX;
constexpr uint32_t input_capacity = 8192;
constexpr float deg_to_rad = 3.14159265358979323846f / 180.0f;

enum RadiusToken : uint8_t {
    RadPx = 0,
    RadPanel = 1,
    RadControl = 2,
    RadInput = 3,
    RadSmall = 4,
    RadPill = 5
};

struct SizingSpec {
    EtchSizingKind kind;
    float value;
    float min_px;
    float max_px;
    bool explicit_set;
};

struct ShadowSpec {
    float x, y, blur, spread;
    EtchStr color;
    bool inset;
    bool has_blur;
    bool has_y;
};

struct GradientSpec {
    EtchGradientKind kind;
    bool oklab;
    float dir_x, dir_y;
    float center_x, center_y;
    float angle_rad;
    uint8_t stop_count;
    EtchStr stop_colors[8];
    float stop_pos[8];
};

struct TransformSpec {
    float tx, ty;
    float sx, sy;
    float rotation_rad;
};

struct Style {
    SizingSpec w, h;
    EtchPadding padding, margin, border_sides;
    EtchCorners radius_vals;
    EtchVec2 floating_offset;
    float gap, row_gap, size, border_width, line_height, letter_spacing;
    float opacity, value, min_val, max_val, step, spacer_size;
    float aspect_ratio, flex_grow, flex_shrink;
    float backdrop_blur, backdrop_saturation, backdrop_brightness, backdrop_noise;
    float top_rim_alpha, hover_scale, press_scale;
    EtchStr background, color, border_color, track_color, hover_bg, hover_border_color;
    EtchStr action, placeholder, src;
    uint32_t sprite_id, handler;
    uint32_t shadow_offset, gradient_idx, transform_idx, item_offset;
    int32_t int_value;
    int16_t z_index;
    uint16_t icon_id, font_id, grid_cols, col_span, row_span, max_lines;
    uint8_t shadow_count, item_count, radius_mode, radius_tok[4];
    EtchAlignX align_x;
    EtchAlignY align_y;
    EtchJustify justify;
    EtchAlignSelf align_self;
    EtchElevation elevation;
    EtchAttachTo attach_to;
    EtchAnchor anchor_element, anchor_parent;
    EtchImageFit image_fit;
    EtchButtonVariant button_variant;
    bool column, animate, checked, indeterminate, disabled, open, display_math;
    bool scroll_x, scroll_y, wrap_flag, continuous_corners, clip_children;
    bool pointer_passthrough, clamp_to_viewport, flip_on_overflow;
    bool shrink_width, shrink_height, inset_shadow_bool, ellipsis;
    bool has_padding, has_margin, has_gap, has_row_gap, has_size, has_border_width;
    bool has_align_x, has_align_y, has_justify, has_align_self;
    bool has_flex_grow, has_flex_shrink, has_wrap, has_aspect_ratio;
    bool has_opacity, has_value, has_min, has_max, has_step, has_spacer_size;
    bool has_scroll_x, has_scroll_y, has_sprite, has_icon, has_font_id;
    bool has_line_height, has_letter_spacing, has_elevation, has_continuous_corners;
    bool has_backdrop_saturation, has_backdrop_brightness, has_top_rim_alpha;
    bool has_hover_scale, has_press_scale, has_attach_to, has_anchor_element;
    bool has_anchor_parent, has_floating_offset, has_z_index, has_clip_children;
    bool has_clamp_to_viewport;
};

struct Action {
    char name[64];
    InlayActionFn callback;
    void *user;
};

struct SlotBinding {
    char name[64];
    InlaySlotRenderFn callback;
    void *user;
};

struct Timer {
    JSValue function;
    double due;
    uint32_t id;
};
} // namespace

struct InlaySurface {
    EtchContext *etch;
    InlaySurfaceDesc desc;
    JSRuntime *runtime;
    JSContext *js;
    InlayUiNode *nodes;
    Style *styles;
    uint32_t node_count, node_capacity;
    char *text;
    uint32_t text_used;
    ShadowSpec *shadow_pool;
    uint32_t shadow_pool_count, shadow_pool_cap;
    GradientSpec *gradient_pool;
    uint32_t gradient_pool_count, gradient_pool_cap;
    TransformSpec *transform_pool;
    uint32_t transform_pool_count, transform_pool_cap;
    EtchStr *item_pool;
    uint32_t item_pool_count, item_pool_cap;
    char *state;
    char *json_scratch;
    char edit_buffer[input_capacity];
    char *source;
    uint32_t source_capacity, state_size, state_capacity;
    char error[1024], path[1024];
    Action actions[32];
    uint32_t action_count;
    SlotBinding slots[32];
    uint32_t slot_count;
    JSValue *handlers;
    uint32_t handler_count, handler_capacity;
    Timer timers[32];
    uint32_t next_timer;
    int64_t modified;
    uint64_t file_size, deadline;
    double frequency;
    bool running, drawing, timed_out, pending_state, state_dirty, loaded, hmr_file_error;
    JSValue rejections[32];
    uint32_t rejection_count;
    InlaySurfaceStats stats;
};

namespace {
double now_ms(const InlaySurface *s) {
    return double(SDL_GetPerformanceCounter()) * 1000 / s->frequency;
}

bool str_eq(const char *chars, size_t len, const char *lit) noexcept {
    const size_t n = std::strlen(lit);
    return len == n && (n == 0 || (chars && !std::memcmp(chars, lit, n)));
}

bool equal(EtchStr value, const char *text) noexcept {
    return str_eq(value.chars, value.length, text);
}

bool is_container_kind(InlayUiKind kind) noexcept {
    switch (kind) {
        case INLAY_UI_BOX:
        case INLAY_UI_SCROLL:
        case INLAY_UI_CARD:
        case INLAY_UI_SPACER:
        case INLAY_UI_MODAL:
        case INLAY_UI_POPOVER:
        case INLAY_UI_TOOLTIP:
        case INLAY_UI_SPLIT:
        case INLAY_UI_SLOT:
        case INLAY_UI_TABS:
        case INLAY_UI_COMBOBOX:
        case INLAY_UI_DIVIDER:
        case INLAY_UI_INPUT:
            return true;
        default:
            return false;
    }
}

JSValue parse_json(InlaySurface *s, EtchStr json) {
    std::memcpy(s->json_scratch, json.chars, json.length);
    s->json_scratch[json.length] = 0;
    return JS_ParseJSON(s->js, s->json_scratch, json.length, "<json>");
}

void error(InlaySurface *s, const char *message) {
    if (s->stats.has_error && !std::strcmp(s->error, message))
        return;
    std::snprintf(s->error, sizeof(s->error), "%s", message);
    s->stats.has_error = true;
    etch_request_redraw(s->etch);
}

int interrupt(JSContext *, void *opaque) {
    auto *s = static_cast<InlaySurface *>(opaque);
    if (s->deadline && SDL_GetPerformanceCounter() >= s->deadline) {
        s->timed_out = true;
        return 1;
    }
    return 0;
}

void begin_tick(InlaySurface *s) {
    s->running = true;
    s->timed_out = false;
    s->deadline =
        SDL_GetPerformanceCounter() + uint64_t(double(s->desc.max_tick_ms) * s->frequency / 1000);
    ++s->stats.js_ticks;
}

void clear_handlers(InlaySurface *s) {
    if (s->js) {
        for (uint32_t i = 0; i < s->handler_count; ++i)
            JS_FreeValue(s->js, s->handlers[i]);
    }
    s->handler_count = 0;
}

uint32_t register_handler(InlaySurface *s, JSValueConst fn) {
    if (s->handler_count == s->handler_capacity) {
        uint32_t cap = s->handler_capacity ? s->handler_capacity * 2 : 64;
        if (cap > s->desc.max_nodes)
            cap = s->desc.max_nodes;
        if (s->handler_count >= cap) {
            JS_ThrowInternalError(s->js, "UI handler capacity exceeded");
            return none;
        }
        auto *mem = static_cast<JSValue *>(std::realloc(s->handlers, size_t(cap) * sizeof(JSValue)));
        if (!mem) {
            JS_ThrowInternalError(s->js, "UI handler allocation failed");
            return none;
        }
        s->handlers = mem;
        s->handler_capacity = cap;
    }
    const uint32_t idx = s->handler_count++;
    s->handlers[idx] = JS_DupValue(s->js, fn);
    return idx;
}

bool ensure_node_capacity(InlaySurface *s) {
    if (s->node_count < s->node_capacity)
        return true;
    if (s->node_capacity >= s->desc.max_nodes)
        return false;
    uint32_t cap = s->node_capacity ? s->node_capacity * 2u : 256u;
    if (cap > s->desc.max_nodes)
        cap = s->desc.max_nodes;
    auto *nn = static_cast<InlayUiNode *>(std::realloc(s->nodes, size_t(cap) * sizeof(InlayUiNode)));
    if (!nn)
        return false;
    s->nodes = nn;
    auto *ns = static_cast<Style *>(std::realloc(s->styles, size_t(cap) * sizeof(Style)));
    if (!ns)
        return false;
    s->styles = ns;
    s->node_capacity = cap;
    return true;
}

template <typename T>
uint32_t alloc_aux_pool(InlaySurface *s, T **pool, uint32_t *count, uint32_t *cap,
                        uint32_t need, uint32_t hard_cap) {
    if (need == 0u)
        return 0u;
    if (need > hard_cap || *count > hard_cap - need) {
        JS_ThrowInternalError(s->js, "UI auxiliary pool capacity exceeded");
        return none;
    }
    const uint32_t target = *count + need;
    if (target > *cap) {
        uint32_t next = *cap ? *cap * 2u : 32u;
        while (next < target)
            next = next > hard_cap / 2u ? hard_cap : next * 2u;
        auto *mem = static_cast<T *>(std::realloc(*pool, size_t(next) * sizeof(T)));
        if (!mem) {
            JS_ThrowInternalError(s->js, "UI auxiliary pool allocation failed");
            return none;
        }
        *pool = mem;
        *cap = next;
    }
    const uint32_t offset = *count;
    *count += need;
    return offset;
}

uint32_t fnv1a_bytes(uint32_t seed, const char *chars, size_t length) noexcept {
    uint32_t h = seed;
    for (size_t i = 0; i < length; ++i)
        h = (h ^ uint8_t(chars[i])) * 16777619u;
    return h ? h : 1u;
}

uint32_t hash_slot_id(uint32_t slot, InlayUiKind kind) noexcept {
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t(kind) + 1u)) * 16777619u;
    h = (h ^ (slot & 0xffu)) * 16777619u;
    h = (h ^ ((slot >> 8) & 0xffu)) * 16777619u;
    h = (h ^ ((slot >> 16) & 0xffu)) * 16777619u;
    h = (h ^ ((slot >> 24) & 0xffu)) * 16777619u;
    return h ? h : 1u;
}

bool hash_string_prop(InlaySurface *s, JSValueConst props, const char *name, uint32_t seed, uint32_t *out) {
    const auto v = JS_GetPropertyStr(s->js, props, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    size_t n = 0;
    const char *chars = JS_ToCStringLen(s->js, &n, v);
    const bool ok = chars && n > 0;
    if (ok)
        *out = fnv1a_bytes(seed, chars, n);
    if (chars)
        JS_FreeCString(s->js, chars);
    JS_FreeValue(s->js, v);
    return ok;
}

bool finish_tick(InlaySurface *s, uint64_t start) {
    if (s->deadline && SDL_GetPerformanceCounter() >= s->deadline)
        s->timed_out = true;
    const bool failed = s->timed_out || JS_HasException(s->js) || s->stats.has_error;
    if (failed) {
        const auto exception = JS_GetException(s->js);
        const char *message = JS_ToCString(s->js, exception);
        error(s, s->timed_out ? "Script exceeded its tick time limit"
                 : message    ? message
                              : "Script allocation failed");
        if (message)
            JS_FreeCString(s->js, message);
        JS_FreeValue(s->js, exception);
        if (JS_HasException(s->js)) {
            const auto nested = JS_GetException(s->js);
            JS_FreeValue(s->js, nested);
        }
        clear_handlers(s);
        for (auto &timer : s->timers) {
            JS_FreeValue(s->js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0;
        }
        s->state_dirty = false;
        s->loaded = false;
    }
    s->deadline = 0;
    s->running = false;
    s->stats.last_tick_ms = double(SDL_GetPerformanceCounter() - start) * 1000 / s->frequency;
    return !failed;
}

bool save_state(InlaySurface *s, const char *bytes, size_t length) {
    if (length > 65536) {
        error(s, "State JSON exceeds 64KiB");
        return false;
    }
    if (length + 1 > s->state_capacity) {
        uint32_t cap = s->state_capacity ? s->state_capacity : 128;
        while (cap < length + 1)
            cap *= 2;
        auto *data = static_cast<char *>(std::realloc(s->state, cap));
        if (!data) {
            error(s, "State storage allocation failed");
            return false;
        }
        s->state = data;
        s->state_capacity = cap;
    }
    std::memmove(s->state, bytes, length);
    s->state[length] = 0;
    s->state_size = uint32_t(length);
    return true;
}

EtchStr copy_text(InlaySurface *s, JSValueConst value) {
    size_t n = 0;
    const char *chars = JS_ToCStringLen(s->js, &n, value);
    if (!chars)
        return {};
    EtchStr result = {};
    if (n > s->desc.max_text_bytes - s->text_used)
        JS_ThrowInternalError(s->js, "UI text capacity exceeded");
    else {
        auto *dst = s->text + s->text_used;
        if (n)
            std::memcpy(dst, chars, n);
        s->text_used += uint32_t(n);
        result = {dst, uint32_t(n), false};
    }
    JS_FreeCString(s->js, chars);
    return result;
}

bool parse_event_prop(InlaySurface *s, JSValueConst props, const char *name, Style &style) {
    const auto v = JS_GetPropertyStr(s->js, props, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    if (JS_IsFunction(s->js, v)) {
        style.handler = register_handler(s, v);
        JS_FreeValue(s->js, v);
        return style.handler != none;
    }
    style.action = copy_text(s, v);
    JS_FreeValue(s->js, v);
    return style.action.length != 0;
}

EtchStr string_prop(InlaySurface *s, JSValueConst object, const char *name) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    const auto result = (JS_IsUndefined(v) || JS_IsNull(v)) ? EtchStr{} : copy_text(s, v);
    JS_FreeValue(s->js, v);
    return result;
}

template <typename Fn>
bool peek_str_prop(InlaySurface *s, JSValueConst object, const char *name, Fn &&fn) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    size_t n = 0;
    const char *chars = JS_ToCStringLen(s->js, &n, v);
    bool matched = false;
    if (chars) {
        matched = fn(chars, n);
        JS_FreeCString(s->js, chars);
    }
    JS_FreeValue(s->js, v);
    return matched;
}

bool read_float_val(InlaySurface *s, JSValueConst v, const char *name, float *out,
                    double min_v, double max_v, bool unbounded = false) {
    double value = 0.0;
    if (JS_ToFloat64(s->js, &value, v) < 0)
        return false;
    const float narrowed = float(value);
    if (!std::isfinite(value) || !std::isfinite(narrowed) ||
        (!unbounded && (value < min_v || value > max_v))) {
        JS_ThrowTypeError(s->js, "Invalid UI numeric property: %s", name);
        return false;
    }
    *out = narrowed;
    return true;
}

bool opt_number_prop(InlaySurface *s, JSValueConst object, const char *name, float *out,
                     bool unbounded = false, double min_v = -1.0, double max_v = 16384.0) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    const bool ok = read_float_val(s, v, name, out, min_v, max_v, unbounded);
    JS_FreeValue(s->js, v);
    return ok;
}

float number_prop(InlaySurface *s, JSValueConst object, const char *name, float fallback) {
    float value = fallback;
    opt_number_prop(s, object, name, &value);
    return value;
}

bool bool_prop(InlaySurface *s, JSValueConst object, const char *name) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    const bool result = JS_ToBool(s->js, v) > 0;
    JS_FreeValue(s->js, v);
    return result;
}

bool opt_bool_prop(InlaySurface *s, JSValueConst object, const char *name, bool *out) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    *out = JS_ToBool(s->js, v) > 0;
    JS_FreeValue(s->js, v);
    return true;
}

bool opt_uint_prop(InlaySurface *s, JSValueConst object, const char *name, uint32_t max_value, uint32_t *out) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    double value = -1.0;
    const int rc = JS_ToFloat64(s->js, &value, v);
    JS_FreeValue(s->js, v);
    if (rc < 0 || !std::isfinite(value) || value < 0.0 || value > double(max_value) ||
        std::trunc(value) != value) {
        JS_ThrowTypeError(s->js, "Invalid UI integer property: %s", name);
        return false;
    }
    *out = uint32_t(value);
    return true;
}

bool opt_int_prop(InlaySurface *s, JSValueConst object, const char *name,
                  int32_t min_value, int32_t max_value, int32_t *out) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    double value = 0.0;
    const int rc = JS_ToFloat64(s->js, &value, v);
    JS_FreeValue(s->js, v);
    if (rc < 0 || !std::isfinite(value) || value < double(min_value) || value > double(max_value) ||
        std::trunc(value) != value) {
        JS_ThrowTypeError(s->js, "Invalid UI integer property: %s", name);
        return false;
    }
    *out = int32_t(value);
    return true;
}

bool read_vec2_array(InlaySurface *s, JSValueConst arr, const char *name, float *ox, float *oy,
                     double min_v, double max_v) {
    const auto v0 = JS_GetPropertyUint32(s->js, arr, 0);
    const auto v1 = JS_GetPropertyUint32(s->js, arr, 1);
    float x = *ox, y = *oy;
    bool ok = false;
    if (!JS_IsUndefined(v0) && !JS_IsUndefined(v1)) {
        ok = read_float_val(s, v0, name, &x, min_v, max_v) &&
             read_float_val(s, v1, name, &y, min_v, max_v);
    }
    JS_FreeValue(s->js, v0);
    JS_FreeValue(s->js, v1);
    if (ok) {
        *ox = x;
        *oy = y;
    }
    return ok;
}

bool parse_align_x(const char *chars, size_t len, EtchAlignX *out) noexcept {
    if (str_eq(chars, len, "left") || str_eq(chars, len, "start")) {
        *out = ETCH_ALIGN_X_LEFT;
        return true;
    }
    if (str_eq(chars, len, "center")) {
        *out = ETCH_ALIGN_X_CENTER;
        return true;
    }
    if (str_eq(chars, len, "right") || str_eq(chars, len, "end")) {
        *out = ETCH_ALIGN_X_RIGHT;
        return true;
    }
    return false;
}

bool parse_align_y(const char *chars, size_t len, EtchAlignY *out) noexcept {
    if (str_eq(chars, len, "top") || str_eq(chars, len, "start")) {
        *out = ETCH_ALIGN_Y_TOP;
        return true;
    }
    if (str_eq(chars, len, "center")) {
        *out = ETCH_ALIGN_Y_CENTER;
        return true;
    }
    if (str_eq(chars, len, "bottom") || str_eq(chars, len, "end")) {
        *out = ETCH_ALIGN_Y_BOTTOM;
        return true;
    }
    return false;
}

bool parse_justify_token(const char *chars, size_t len, EtchJustify *out, bool space_only = false) noexcept {
    if (str_eq(chars, len, "between") || str_eq(chars, len, "space-between")) {
        *out = ETCH_JUSTIFY_SPACE_BETWEEN;
        return true;
    }
    if (str_eq(chars, len, "around") || str_eq(chars, len, "space-around")) {
        *out = ETCH_JUSTIFY_SPACE_AROUND;
        return true;
    }
    if (str_eq(chars, len, "evenly") || str_eq(chars, len, "space-evenly")) {
        *out = ETCH_JUSTIFY_SPACE_EVENLY;
        return true;
    }
    if (space_only)
        return false;
    if (str_eq(chars, len, "start") || str_eq(chars, len, "left") || str_eq(chars, len, "flex-start")) {
        *out = ETCH_JUSTIFY_START;
        return true;
    }
    if (str_eq(chars, len, "center")) {
        *out = ETCH_JUSTIFY_CENTER;
        return true;
    }
    if (str_eq(chars, len, "end") || str_eq(chars, len, "right") || str_eq(chars, len, "flex-end")) {
        *out = ETCH_JUSTIFY_END;
        return true;
    }
    return false;
}

bool parse_align_self_token(const char *chars, size_t len, EtchAlignSelf *out) noexcept {
    if (str_eq(chars, len, "auto")) { *out = ETCH_ALIGN_SELF_AUTO; return true; }
    if (str_eq(chars, len, "start") || str_eq(chars, len, "flex-start")) { *out = ETCH_ALIGN_SELF_START; return true; }
    if (str_eq(chars, len, "center")) { *out = ETCH_ALIGN_SELF_CENTER; return true; }
    if (str_eq(chars, len, "end") || str_eq(chars, len, "flex-end")) { *out = ETCH_ALIGN_SELF_END; return true; }
    if (str_eq(chars, len, "stretch")) { *out = ETCH_ALIGN_SELF_STRETCH; return true; }
    return false;
}

bool parse_elevation_token(const char *chars, size_t len, EtchElevation *out) noexcept {
    if (str_eq(chars, len, "none")) { *out = ETCH_ELEVATION_NONE; return true; }
    if (str_eq(chars, len, "sm")) { *out = ETCH_ELEVATION_SM; return true; }
    if (str_eq(chars, len, "md")) { *out = ETCH_ELEVATION_MD; return true; }
    if (str_eq(chars, len, "lg")) { *out = ETCH_ELEVATION_LG; return true; }
    if (str_eq(chars, len, "xl")) { *out = ETCH_ELEVATION_XL; return true; }
    return false;
}

bool parse_attach_token(const char *chars, size_t len, EtchAttachTo *out) noexcept {
    if (str_eq(chars, len, "none")) { *out = ETCH_ATTACH_NONE; return true; }
    if (str_eq(chars, len, "parent")) { *out = ETCH_ATTACH_PARENT; return true; }
    if (str_eq(chars, len, "root")) { *out = ETCH_ATTACH_ROOT; return true; }
    if (str_eq(chars, len, "element")) { *out = ETCH_ATTACH_ELEMENT; return true; }
    return false;
}

bool parse_anchor_token(const char *chars, size_t len, EtchAnchor *out) noexcept {
    if (str_eq(chars, len, "top-left") || str_eq(chars, len, "topLeft")) { *out = ETCH_ANCHOR_TOP_LEFT; return true; }
    if (str_eq(chars, len, "top-center") || str_eq(chars, len, "topCenter")) { *out = ETCH_ANCHOR_TOP_CENTER; return true; }
    if (str_eq(chars, len, "top-right") || str_eq(chars, len, "topRight")) { *out = ETCH_ANCHOR_TOP_RIGHT; return true; }
    if (str_eq(chars, len, "center-left") || str_eq(chars, len, "centerLeft")) { *out = ETCH_ANCHOR_CENTER_LEFT; return true; }
    if (str_eq(chars, len, "center")) { *out = ETCH_ANCHOR_CENTER; return true; }
    if (str_eq(chars, len, "center-right") || str_eq(chars, len, "centerRight")) { *out = ETCH_ANCHOR_CENTER_RIGHT; return true; }
    if (str_eq(chars, len, "bottom-left") || str_eq(chars, len, "bottomLeft")) { *out = ETCH_ANCHOR_BOTTOM_LEFT; return true; }
    if (str_eq(chars, len, "bottom-center") || str_eq(chars, len, "bottomCenter")) { *out = ETCH_ANCHOR_BOTTOM_CENTER; return true; }
    if (str_eq(chars, len, "bottom-right") || str_eq(chars, len, "bottomRight")) { *out = ETCH_ANCHOR_BOTTOM_RIGHT; return true; }
    return false;
}

bool parse_font_weight_token(const char *chars, size_t len, uint16_t *out) noexcept {
    if (str_eq(chars, len, "bold") || str_eq(chars, len, "700")) { *out = ETCH_FONT_BOLD; return true; }
    if (str_eq(chars, len, "semibold") || str_eq(chars, len, "semi-bold") || str_eq(chars, len, "600")) {
        *out = ETCH_FONT_SEMIBOLD;
        return true;
    }
    if (str_eq(chars, len, "medium") || str_eq(chars, len, "500")) { *out = ETCH_FONT_MEDIUM; return true; }
    if (str_eq(chars, len, "italic")) { *out = ETCH_FONT_ITALIC; return true; }
    if (str_eq(chars, len, "mono")) { *out = ETCH_FONT_MONO; return true; }
    if (str_eq(chars, len, "regular") || str_eq(chars, len, "normal") || str_eq(chars, len, "400")) {
        *out = ETCH_FONT_SANS;
        return true;
    }
    return false;
}

bool parse_sizing_prop(InlaySurface *s, JSValueConst props, const char *name, SizingSpec *out) {
    const auto v = JS_GetPropertyStr(s->js, props, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    if (JS_IsString(v)) {
        size_t n = 0;
        const char *chars = JS_ToCStringLen(s->js, &n, v);
        bool ok = false;
        if (chars) {
            if (str_eq(chars, n, "grow") || str_eq(chars, n, "fill")) {
                out->kind = ETCH_SIZING_GROW;
                out->value = 0.f;
                out->explicit_set = true;
                ok = true;
            } else if (str_eq(chars, n, "fit") || str_eq(chars, n, "auto")) {
                out->kind = ETCH_SIZING_FIT;
                out->value = 0.f;
                out->explicit_set = true;
                ok = true;
            } else if (n > 1 && chars[n - 1] == '%' && n < 32) {
                char buf[32] = {};
                std::memcpy(buf, chars, n - 1);
                char *end = nullptr;
                const float pct = std::strtof(buf, &end);
                if (end != buf && *end == '\0' && std::isfinite(pct) && pct >= 0.f && pct <= 100.f) {
                    out->kind = ETCH_SIZING_PERCENT;
                    out->value = pct * 0.01f;
                    out->explicit_set = true;
                    ok = true;
                }
            } else if (n > 0 && n < 32) {
                char buf[32] = {};
                std::memcpy(buf, chars, n);
                char *end = nullptr;
                const float px = std::strtof(buf, &end);
                if (end != buf && *end == '\0' && std::isfinite(px) && px >= -1.f && px <= 16384.f) {
                    if (px < 0.f) {
                        out->kind = ETCH_SIZING_GROW;
                        out->explicit_set = true;
                    } else if (px > 0.f) {
                        out->kind = ETCH_SIZING_FIXED;
                        out->value = px;
                        out->explicit_set = true;
                    }
                    ok = true;
                }
            }
            JS_FreeCString(s->js, chars);
        }
        JS_FreeValue(s->js, v);
        if (!ok)
            JS_ThrowTypeError(s->js, "Invalid UI sizing property: %s", name);
        return ok;
    }
    float num = 0.f;
    const bool ok = read_float_val(s, v, name, &num, -1.0, 16384.0);
    JS_FreeValue(s->js, v);
    if (!ok)
        return false;
    if (num < 0.f) {
        out->kind = ETCH_SIZING_GROW;
        out->value = 0.f;
        out->explicit_set = true;
    } else if (num > 0.f) {
        out->kind = ETCH_SIZING_FIXED;
        out->value = num;
        out->explicit_set = true;
    }
    return true;
}

void parse_sizing_limits(InlaySurface *s, JSValueConst props, SizingSpec &w, SizingSpec &h) {
    float v = 0.f;
    if (opt_number_prop(s, props, "minWidth", &v, false, 0.0) ||
        opt_number_prop(s, props, "minW", &v, false, 0.0))
        w.min_px = v;
    v = 0.f;
    if (opt_number_prop(s, props, "maxWidth", &v, false, 0.0) ||
        opt_number_prop(s, props, "maxW", &v, false, 0.0))
        w.max_px = v;
    v = 0.f;
    if (opt_number_prop(s, props, "minHeight", &v, false, 0.0) ||
        opt_number_prop(s, props, "minH", &v, false, 0.0))
        h.min_px = v;
    v = 0.f;
    if (opt_number_prop(s, props, "maxHeight", &v, false, 0.0) ||
        opt_number_prop(s, props, "maxH", &v, false, 0.0))
        h.max_px = v;
}

bool parse_single_radius_val(InlaySurface *s, JSValueConst v, float *out_px, uint8_t *out_tok) {
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return false;
    if (JS_IsString(v)) {
        size_t n = 0;
        const char *chars = JS_ToCStringLen(s->js, &n, v);
        bool ok = false;
        if (chars) {
            if (str_eq(chars, n, "panel")) { *out_tok = RadPanel; ok = true; }
            else if (str_eq(chars, n, "control")) { *out_tok = RadControl; ok = true; }
            else if (str_eq(chars, n, "input")) { *out_tok = RadInput; ok = true; }
            else if (str_eq(chars, n, "small")) { *out_tok = RadSmall; ok = true; }
            else if (str_eq(chars, n, "pill")) { *out_tok = RadPill; ok = true; }
            else if (n > 0 && n < 32) {
                char buf[32] = {};
                std::memcpy(buf, chars, n);
                char *end = nullptr;
                const float r = std::strtof(buf, &end);
                if (end != buf && *end == '\0' && std::isfinite(r) && r >= 0.f && r <= 16384.f) {
                    *out_tok = RadPx;
                    *out_px = r;
                    ok = true;
                }
            }
            JS_FreeCString(s->js, chars);
        }
        return ok;
    }
    double val = 0.0;
    if (JS_ToFloat64(s->js, &val, v) == 0 && std::isfinite(val) && val >= 0.0 && val <= 16384.0) {
        *out_tok = RadPx;
        *out_px = float(val);
        return true;
    }
    return false;
}

void parse_radius_prop(InlaySurface *s, JSValueConst props, Style &style) {
    const auto v = JS_GetPropertyStr(s->js, props, "radius");
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return;
    }
    if (JS_IsArray(v)) {
        float *dst[4] = {&style.radius_vals.tl, &style.radius_vals.tr,
                         &style.radius_vals.br, &style.radius_vals.bl};
        bool any = false;
        for (uint32_t i = 0; i < 4; ++i) {
            const auto el = JS_GetPropertyUint32(s->js, v, i);
            any |= parse_single_radius_val(s, el, dst[i], &style.radius_tok[i]);
            JS_FreeValue(s->js, el);
        }
        if (any)
            style.radius_mode = 1;
        JS_FreeValue(s->js, v);
        return;
    }
    if (JS_IsObject(v)) {
        static const char *const keys[4] = {"tl", "tr", "br", "bl"};
        float *dst[4] = {&style.radius_vals.tl, &style.radius_vals.tr,
                         &style.radius_vals.br, &style.radius_vals.bl};
        bool any = false;
        for (uint32_t i = 0; i < 4; ++i) {
            const auto el = JS_GetPropertyStr(s->js, v, keys[i]);
            any |= parse_single_radius_val(s, el, dst[i], &style.radius_tok[i]);
            JS_FreeValue(s->js, el);
        }
        if (any)
            style.radius_mode = 1;
        JS_FreeValue(s->js, v);
        return;
    }
    float px = 0.f;
    uint8_t tok = RadPx;
    if (parse_single_radius_val(s, v, &px, &tok)) {
        style.radius_mode = 1;
        style.radius_vals = {px, px, px, px};
        style.radius_tok[0] = style.radius_tok[1] = style.radius_tok[2] = style.radius_tok[3] = tok;
    }
    JS_FreeValue(s->js, v);
}

bool parse_edges_prop(InlaySurface *s, JSValueConst props, const char *base,
                      const char *kx, const char *ky, const char *kt, const char *kr,
                      const char *kb, const char *kl, EtchPadding *out, bool allow_neg) {
    bool set = false;
    const double lo = allow_neg ? -16384.0 : -1.0;
    const auto clamp_e = [&](float x) noexcept { return allow_neg ? x : std::fmax(0.f, x); };
    const auto v = JS_GetPropertyStr(s->js, props, base);
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_IsArray(v)) {
            const auto lv = JS_GetPropertyStr(s->js, v, "length");
            uint32_t len = 0;
            JS_ToUint32(s->js, &len, lv);
            JS_FreeValue(s->js, lv);
            float a[4] = {};
            const uint32_t take = len < 4u ? len : 4u;
            bool ok = take > 0u;
            for (uint32_t i = 0; i < take && ok; ++i) {
                const auto el = JS_GetPropertyUint32(s->js, v, i);
                ok = read_float_val(s, el, base, &a[i], lo, 16384.0);
                JS_FreeValue(s->js, el);
            }
            if (ok) {
                if (take == 1u) {
                    const float p = clamp_e(a[0]);
                    *out = {p, p, p, p};
                    set = true;
                } else if (take == 2u) {
                    const float pv = clamp_e(a[0]), ph = clamp_e(a[1]);
                    *out = {ph, ph, pv, pv};
                    set = true;
                } else if (take == 3u) {
                    const float pt = clamp_e(a[0]), ph = clamp_e(a[1]), pb = clamp_e(a[2]);
                    *out = {ph, ph, pt, pb};
                    set = true;
                } else if (take == 4u) {
                    *out = {clamp_e(a[3]), clamp_e(a[1]), clamp_e(a[0]), clamp_e(a[2])};
                    set = true;
                }
            }
        } else if (JS_IsObject(v)) {
            float tmp = 0.f;
            if (opt_number_prop(s, v, "x", &tmp, false, lo)) { out->left = out->right = clamp_e(tmp); set = true; }
            if (opt_number_prop(s, v, "y", &tmp, false, lo)) { out->top = out->bottom = clamp_e(tmp); set = true; }
            if (opt_number_prop(s, v, "left", &tmp, false, lo)) { out->left = clamp_e(tmp); set = true; }
            if (opt_number_prop(s, v, "right", &tmp, false, lo)) { out->right = clamp_e(tmp); set = true; }
            if (opt_number_prop(s, v, "top", &tmp, false, lo)) { out->top = clamp_e(tmp); set = true; }
            if (opt_number_prop(s, v, "bottom", &tmp, false, lo)) { out->bottom = clamp_e(tmp); set = true; }
        } else {
            float tmp = 0.f;
            if (read_float_val(s, v, base, &tmp, lo, 16384.0)) {
                const float p = clamp_e(tmp);
                *out = {p, p, p, p};
                set = true;
            }
        }
    }
    JS_FreeValue(s->js, v);
    float tmp = 0.f;
    if (opt_number_prop(s, props, kx, &tmp, false, lo)) { out->left = out->right = clamp_e(tmp); set = true; }
    if (opt_number_prop(s, props, ky, &tmp, false, lo)) { out->top = out->bottom = clamp_e(tmp); set = true; }
    if (opt_number_prop(s, props, kt, &tmp, false, lo)) { out->top = clamp_e(tmp); set = true; }
    if (opt_number_prop(s, props, kr, &tmp, false, lo)) { out->right = clamp_e(tmp); set = true; }
    if (opt_number_prop(s, props, kb, &tmp, false, lo)) { out->bottom = clamp_e(tmp); set = true; }
    if (opt_number_prop(s, props, kl, &tmp, false, lo)) { out->left = clamp_e(tmp); set = true; }
    return set;
}

bool parse_shadow_obj(InlaySurface *s, JSValueConst obj, bool force_inset, ShadowSpec *out) {
    if (!JS_IsObject(obj) || JS_IsNull(obj) || JS_IsArray(obj))
        return false;
    *out = {};
    opt_number_prop(s, obj, "x", &out->x, false, -16384.0, 16384.0);
    out->has_y = opt_number_prop(s, obj, "y", &out->y, false, -16384.0, 16384.0);
    out->has_blur = opt_number_prop(s, obj, "blur", &out->blur, false, 0.0, 16384.0);
    opt_number_prop(s, obj, "spread", &out->spread, false, -16384.0, 16384.0);
    out->color = string_prop(s, obj, "color");
    out->inset = force_inset ? true : bool_prop(s, obj, "inset");
    return true;
}

void parse_shadow_props(InlaySurface *s, JSValueConst props, Style &style) {
    ShadowSpec local[4] = {};
    uint8_t count = 0;

    const auto sh_arr = JS_GetPropertyStr(s->js, props, "shadows");
    if (JS_IsArray(sh_arr)) {
        const auto lv = JS_GetPropertyStr(s->js, sh_arr, "length");
        uint32_t len = 0;
        JS_ToUint32(s->js, &len, lv);
        JS_FreeValue(s->js, lv);
        if (len > 4u)
            len = 4u;
        for (uint32_t i = 0; i < len && count < 4u && !JS_HasException(s->js); ++i) {
            const auto el = JS_GetPropertyUint32(s->js, sh_arr, i);
            if (parse_shadow_obj(s, el, false, &local[count]))
                ++count;
            JS_FreeValue(s->js, el);
        }
    }
    JS_FreeValue(s->js, sh_arr);

    if (count == 0u) {
        const auto sh = JS_GetPropertyStr(s->js, props, "shadow");
        if (parse_shadow_obj(s, sh, false, &local[0]))
            count = 1u;
        JS_FreeValue(s->js, sh);
    }

    const auto inset_sh = JS_GetPropertyStr(s->js, props, "insetShadow");
    if (!JS_IsUndefined(inset_sh) && !JS_IsNull(inset_sh)) {
        if (JS_IsBool(inset_sh)) {
            if (JS_ToBool(s->js, inset_sh) > 0)
                style.inset_shadow_bool = true;
        } else if (JS_IsObject(inset_sh) && !JS_IsArray(inset_sh)) {
            ShadowSpec ispec = {};
            if (parse_shadow_obj(s, inset_sh, true, &ispec)) {
                if (!ispec.has_blur)
                    ispec.blur = 8.f;
                if (!ispec.has_y)
                    ispec.y = 1.f;
                if (count < 4u)
                    local[count++] = ispec;
                else
                    local[3] = ispec;
            }
        }
    }
    JS_FreeValue(s->js, inset_sh);

    if (count > 0u) {
        const uint32_t off = alloc_aux_pool(s, &s->shadow_pool, &s->shadow_pool_count,
                                            &s->shadow_pool_cap, count, s->desc.max_nodes * 4u);
        if (off != none) {
            for (uint8_t i = 0; i < count; ++i)
                s->shadow_pool[off + i] = local[i];
            style.shadow_offset = off;
            style.shadow_count = count;
        }
    }
}

void parse_gradient_prop(InlaySurface *s, JSValueConst props, Style &style) {
    const auto gval = JS_GetPropertyStr(s->js, props, "gradient");
    if (!JS_IsObject(gval) || JS_IsNull(gval) || JS_IsArray(gval)) {
        JS_FreeValue(s->js, gval);
        return;
    }
    GradientSpec g = {};
    g.kind = ETCH_GRADIENT_LINEAR;
    g.oklab = true;
    g.dir_x = 0.f;
    g.dir_y = 1.f;
    g.center_x = 0.5f;
    g.center_y = 0.5f;
    peek_str_prop(s, gval, "kind", [&](const char *c, size_t n) {
        if (str_eq(c, n, "linear")) g.kind = ETCH_GRADIENT_LINEAR;
        else if (str_eq(c, n, "radial")) g.kind = ETCH_GRADIENT_RADIAL;
        else if (str_eq(c, n, "conic")) g.kind = ETCH_GRADIENT_CONIC;
        else if (str_eq(c, n, "none")) g.kind = ETCH_GRADIENT_NONE;
        return true;
    });
    opt_bool_prop(s, gval, "oklab", &g.oklab);

    bool has_dir = false;
    const auto dir_v = JS_GetPropertyStr(s->js, gval, "dir");
    if (JS_IsArray(dir_v))
        has_dir = read_vec2_array(s, dir_v, "gradient.dir", &g.dir_x, &g.dir_y, -16384.0, 16384.0);
    JS_FreeValue(s->js, dir_v);

    float angle_deg = 0.f;
    if (opt_number_prop(s, gval, "angleDeg", &angle_deg, false, -36000.0, 36000.0)) {
        g.angle_rad = angle_deg * deg_to_rad;
        if (!has_dir) {
            g.dir_x = std::cos(g.angle_rad);
            g.dir_y = std::sin(g.angle_rad);
        }
    } else if (opt_number_prop(s, gval, "angleRad", &g.angle_rad, false, -1000.0, 1000.0)) {
        if (!has_dir) {
            g.dir_x = std::cos(g.angle_rad);
            g.dir_y = std::sin(g.angle_rad);
        }
    }

    const auto ctr_v = JS_GetPropertyStr(s->js, gval, "center");
    if (JS_IsArray(ctr_v))
        read_vec2_array(s, ctr_v, "gradient.center", &g.center_x, &g.center_y, -16.0, 16.0);
    JS_FreeValue(s->js, ctr_v);

    const auto stops_v = JS_GetPropertyStr(s->js, gval, "stops");
    if (JS_IsArray(stops_v)) {
        const auto lv = JS_GetPropertyStr(s->js, stops_v, "length");
        uint32_t len = 0;
        JS_ToUint32(s->js, &len, lv);
        JS_FreeValue(s->js, lv);
        if (len > 8u)
            len = 8u;
        bool has_pos[8] = {};
        for (uint32_t i = 0; i < len && !JS_HasException(s->js); ++i) {
            const auto sv = JS_GetPropertyUint32(s->js, stops_v, i);
            if (JS_IsString(sv)) {
                g.stop_colors[g.stop_count] = copy_text(s, sv);
                ++g.stop_count;
            } else if (JS_IsObject(sv) && !JS_IsNull(sv)) {
                g.stop_colors[g.stop_count] = string_prop(s, sv, "color");
                float p = 0.f;
                if (opt_number_prop(s, sv, "pos", &p, false, 0.0, 1.0) ||
                    opt_number_prop(s, sv, "position", &p, false, 0.0, 1.0)) {
                    g.stop_pos[g.stop_count] = std::fmin(1.f, std::fmax(0.f, p));
                    has_pos[g.stop_count] = true;
                }
                ++g.stop_count;
            }
            JS_FreeValue(s->js, sv);
        }
        for (uint8_t i = 0; i < g.stop_count; ++i) {
            if (!has_pos[i])
                g.stop_pos[i] = g.stop_count > 1u ? float(i) / float(g.stop_count - 1u) : 0.f;
        }
    }
    JS_FreeValue(s->js, stops_v);
    JS_FreeValue(s->js, gval);

    if (g.kind != ETCH_GRADIENT_NONE && g.stop_count >= 2u) {
        const uint32_t idx = alloc_aux_pool(s, &s->gradient_pool, &s->gradient_pool_count,
                                            &s->gradient_pool_cap, 1u, s->desc.max_nodes);
        if (idx != none) {
            s->gradient_pool[idx] = g;
            style.gradient_idx = idx;
        }
    }
}

void parse_transform_props(InlaySurface *s, JSValueConst props, Style &style) {
    TransformSpec tf = {0.f, 0.f, 1.f, 1.f, 0.f};
    bool any = false;

    float rot = 0.f;
    if (opt_number_prop(s, props, "rotate", &rot, false, -36000.0, 36000.0)) {
        tf.rotation_rad = rot * deg_to_rad;
        any = true;
    } else if (opt_number_prop(s, props, "rotateRad", &rot, false, -1000.0, 1000.0)) {
        tf.rotation_rad = rot;
        any = true;
    }

    const auto sc_v = JS_GetPropertyStr(s->js, props, "scale");
    if (!JS_IsUndefined(sc_v) && !JS_IsNull(sc_v)) {
        if (JS_IsArray(sc_v)) {
            if (read_vec2_array(s, sc_v, "scale", &tf.sx, &tf.sy, -100.0, 100.0))
                any = true;
        } else {
            float sc = 1.f;
            if (read_float_val(s, sc_v, "scale", &sc, -100.0, 100.0)) {
                tf.sx = tf.sy = sc;
                any = true;
            }
        }
    }
    JS_FreeValue(s->js, sc_v);

    const auto tr_v = JS_GetPropertyStr(s->js, props, "translate");
    if (JS_IsArray(tr_v)) {
        if (read_vec2_array(s, tr_v, "translate", &tf.tx, &tf.ty, -16384.0, 16384.0))
            any = true;
    }
    JS_FreeValue(s->js, tr_v);

    if (opt_number_prop(s, props, "translateX", &tf.tx, false, -16384.0, 16384.0))
        any = true;
    if (opt_number_prop(s, props, "translateY", &tf.ty, false, -16384.0, 16384.0))
        any = true;

    const bool non_identity =
        std::fabs(tf.tx) > 1e-4f || std::fabs(tf.ty) > 1e-4f ||
        std::fabs(tf.sx - 1.f) > 1e-4f || std::fabs(tf.sy - 1.f) > 1e-4f ||
        std::fabs(tf.rotation_rad) > 1e-5f;
    if (any && non_identity) {
        const uint32_t idx = alloc_aux_pool(s, &s->transform_pool, &s->transform_pool_count,
                                            &s->transform_pool_cap, 1u, s->desc.max_nodes);
        if (idx != none) {
            s->transform_pool[idx] = tf;
            style.transform_idx = idx;
        }
    }
}

void parse_items_prop(InlaySurface *s, JSValueConst props, uint32_t max_items, Style &style) {
    const auto arr = JS_GetPropertyStr(s->js, props, "items");
    if (JS_IsArray(arr)) {
        const auto lv = JS_GetPropertyStr(s->js, arr, "length");
        uint32_t count = 0;
        JS_ToUint32(s->js, &count, lv);
        JS_FreeValue(s->js, lv);
        if (count > max_items)
            count = max_items;
        if (count > 0u) {
            const uint32_t hard_cap = s->desc.max_nodes * 16u > 65536u ? 65536u : s->desc.max_nodes * 16u;
            const uint32_t off = alloc_aux_pool(s, &s->item_pool, &s->item_pool_count,
                                                &s->item_pool_cap, count, hard_cap);
            if (off != none) {
                uint32_t written = 0;
                for (uint32_t i = 0; i < count && !JS_HasException(s->js); ++i) {
                    const auto el = JS_GetPropertyUint32(s->js, arr, i);
                    s->item_pool[off + written++] = copy_text(s, el);
                    JS_FreeValue(s->js, el);
                }
                style.item_offset = off;
                style.item_count = uint8_t(written);
            }
        }
    }
    JS_FreeValue(s->js, arr);
}

void parse_font_props(InlaySurface *s, JSValueConst props, Style &style) {
    uint16_t fid = ETCH_FONT_SANS;
    if (peek_str_prop(s, props, "fontWeight", [&](const char *c, size_t n) {
            return parse_font_weight_token(c, n, &fid);
        })) {
        style.font_id = fid;
        style.has_font_id = true;
    }
    if (bool_prop(s, props, "bold")) {
        style.font_id = ETCH_FONT_BOLD;
        style.has_font_id = true;
    }
    if (bool_prop(s, props, "italic")) {
        style.font_id = ETCH_FONT_ITALIC;
        style.has_font_id = true;
    }
    if (bool_prop(s, props, "mono")) {
        style.font_id = ETCH_FONT_MONO;
        style.has_font_id = true;
    }
}

void parse_style(InlaySurface *s, JSValueConst props, InlayUiKind kind, Style &style) {
    style.size = 15.f;
    style.opacity = 1.f;
    style.gradient_idx = none;
    style.transform_idx = none;
    style.handler = none;
    style.col_span = 1;
    style.row_span = 1;
    style.flex_shrink = 1.f;
    style.backdrop_saturation = 1.f;
    style.backdrop_brightness = 1.f;
    style.hover_scale = 1.f;
    style.press_scale = 1.f;

    style.has_opacity = opt_number_prop(s, props, "opacity", &style.opacity);
    if (style.has_opacity)
        style.opacity = std::fmin(1.f, std::fmax(0.f, style.opacity));
    parse_transform_props(s, props, style);

    style.color = string_prop(s, props, "color");

    if (kind == INLAY_UI_TEXT) {
        style.has_size = opt_number_prop(s, props, "fontSize", &style.size);
        parse_font_props(s, props, style);
        style.has_wrap = opt_bool_prop(s, props, "wrap", &style.wrap_flag);
        style.has_line_height = opt_number_prop(s, props, "lineHeight", &style.line_height, false, 0.0);
        style.has_letter_spacing =
            opt_number_prop(s, props, "letterSpacing", &style.letter_spacing, false, -64.0, 256.0);
        style.has_align_x = peek_str_prop(s, props, "alignX", [&](const char *c, size_t n) {
            return parse_align_x(c, n, &style.align_x);
        });
        style.ellipsis = bool_prop(s, props, "ellipsis");
        uint32_t ml = 0;
        if (opt_uint_prop(s, props, "maxLines", 1024u, &ml))
            style.max_lines = uint16_t(ml);
        return;
    }

    parse_sizing_prop(s, props, "width", &style.w);
    parse_sizing_prop(s, props, "height", &style.h);
    parse_sizing_limits(s, props, style.w, style.h);

    style.background = string_prop(s, props, "bg");
    style.has_size = opt_number_prop(s, props, "fontSize", &style.size);
    style.has_margin = parse_edges_prop(s, props, "margin", "marginX", "marginY",
                                        "marginTop", "marginRight", "marginBottom", "marginLeft",
                                        &style.margin, true);

    if (kind == INLAY_UI_SPACER) {
        style.has_spacer_size = opt_number_prop(s, props, "size", &style.spacer_size, false, 0.0);
        style.has_flex_grow = opt_number_prop(s, props, "flexGrow", &style.flex_grow, false, 0.0);
        style.has_flex_shrink = opt_number_prop(s, props, "flexShrink", &style.flex_shrink, false, 0.0);
        return;
    }

    if (kind == INLAY_UI_DIVIDER) {
        bool col = false;
        peek_str_prop(s, props, "axis", [&](const char *c, size_t n) {
            col = str_eq(c, n, "column") || str_eq(c, n, "col");
            return true;
        });
        style.column = col;
        return;
    }

    // Shared visual & container styling props
    style.border_color = string_prop(s, props, "borderColor");
    style.has_border_width = opt_number_prop(s, props, "borderWidth", &style.border_width);
    style.border_sides.top = std::fmax(0.f, number_prop(s, props, "borderTop", 0));
    style.border_sides.right = std::fmax(0.f, number_prop(s, props, "borderRight", 0));
    style.border_sides.bottom = std::fmax(0.f, number_prop(s, props, "borderBottom", 0));
    style.border_sides.left = std::fmax(0.f, number_prop(s, props, "borderLeft", 0));
    parse_radius_prop(s, props, style);
    style.has_continuous_corners = opt_bool_prop(s, props, "continuousCorners", &style.continuous_corners);

    style.has_padding = parse_edges_prop(s, props, "padding", "paddingX", "paddingY",
                                         "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
                                         &style.padding, false);
    style.has_gap = opt_number_prop(s, props, "gap", &style.gap, false, 0.0);
    style.has_row_gap = opt_number_prop(s, props, "rowGap", &style.row_gap, false, 0.0);

    style.has_align_x = peek_str_prop(s, props, "alignX", [&](const char *c, size_t n) {
        return parse_align_x(c, n, &style.align_x);
    });
    style.has_justify = peek_str_prop(s, props, "justify", [&](const char *c, size_t n) {
        return parse_justify_token(c, n, &style.justify, false);
    });
    peek_str_prop(s, props, "justifyContent", [&](const char *c, size_t n) {
        EtchJustify j = ETCH_JUSTIFY_START;
        if (parse_justify_token(c, n, &j, true)) {
            if (!style.has_justify) {
                style.justify = j;
                style.has_justify = true;
            }
            return true;
        }
        if (!style.has_align_x && parse_align_x(c, n, &style.align_x)) {
            style.has_align_x = true;
            return true;
        }
        return false;
    });
    style.has_align_y = peek_str_prop(s, props, "alignY", [&](const char *c, size_t n) {
        return parse_align_y(c, n, &style.align_y);
    });
    if (!style.has_align_y) {
        style.has_align_y = peek_str_prop(s, props, "alignItems", [&](const char *c, size_t n) {
            return parse_align_y(c, n, &style.align_y);
        });
    }
    style.has_align_self = peek_str_prop(s, props, "alignSelf", [&](const char *c, size_t n) {
        return parse_align_self_token(c, n, &style.align_self);
    });

    const bool default_col =
        kind == INLAY_UI_SCROLL || kind == INLAY_UI_CARD || kind == INLAY_UI_POPOVER;
    style.column = default_col;
    peek_str_prop(s, props, "axis", [&](const char *c, size_t n) {
        if (str_eq(c, n, "row"))
            style.column = false;
        else if (str_eq(c, n, "column") || str_eq(c, n, "col"))
            style.column = true;
        return true;
    });

    // Etch 0.27 visual craft & layout props for containers / styled boxes / buttons / popovers / slots
    if (kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_SCROLL ||
        kind == INLAY_UI_POPOVER || kind == INLAY_UI_BADGE || kind == INLAY_UI_BUTTON ||
        kind == INLAY_UI_SLOT || kind == INLAY_UI_TABS || kind == INLAY_UI_MATH ||
        kind == INLAY_UI_DIAGRAM || kind == INLAY_UI_IMAGE || kind == INLAY_UI_MODAL) {
        style.has_elevation = peek_str_prop(s, props, "elevation", [&](const char *c, size_t n) {
            return parse_elevation_token(c, n, &style.elevation);
        });
        parse_shadow_props(s, props, style);
        parse_gradient_prop(s, props, style);
        opt_number_prop(s, props, "backdropBlur", &style.backdrop_blur, false, 0.0);
        style.has_backdrop_saturation =
            opt_number_prop(s, props, "backdropSaturation", &style.backdrop_saturation, false, 0.0, 16.0);
        style.has_backdrop_brightness =
            opt_number_prop(s, props, "backdropBrightness", &style.backdrop_brightness, false, 0.0, 16.0);
        opt_number_prop(s, props, "backdropNoise", &style.backdrop_noise, false, 0.0, 1.0);
        style.has_top_rim_alpha = opt_number_prop(s, props, "topRimAlpha", &style.top_rim_alpha, false, 0.0, 1.0);
        style.hover_bg = string_prop(s, props, "hoverBg");
        style.hover_border_color = string_prop(s, props, "hoverBorderColor");
        style.has_hover_scale = opt_number_prop(s, props, "hoverScale", &style.hover_scale, false, 0.05, 8.0);
        style.has_press_scale = opt_number_prop(s, props, "pressScale", &style.press_scale, false, 0.05, 8.0);

        style.has_flex_grow = opt_number_prop(s, props, "flexGrow", &style.flex_grow, false, 0.0);
        style.has_flex_shrink = opt_number_prop(s, props, "flexShrink", &style.flex_shrink, false, 0.0);
        style.has_wrap = opt_bool_prop(s, props, "wrap", &style.wrap_flag);
        style.has_aspect_ratio = opt_number_prop(s, props, "aspectRatio", &style.aspect_ratio, false, 0.0);
        style.shrink_width = bool_prop(s, props, "shrinkWidth");
        style.shrink_height = bool_prop(s, props, "shrinkHeight");

        uint32_t u = 0;
        if (opt_uint_prop(s, props, "gridCols", 128u, &u))
            style.grid_cols = uint16_t(u);
        if (opt_uint_prop(s, props, "colSpan", 128u, &u) && u >= 1u)
            style.col_span = uint16_t(u);
        if (opt_uint_prop(s, props, "rowSpan", 128u, &u) && u >= 1u)
            style.row_span = uint16_t(u);

        style.has_attach_to = peek_str_prop(s, props, "attachTo", [&](const char *c, size_t n) {
            return parse_attach_token(c, n, &style.attach_to);
        });
        style.has_anchor_element = peek_str_prop(s, props, "anchor", [&](const char *c, size_t n) {
            return parse_anchor_token(c, n, &style.anchor_element);
        });
        style.has_anchor_parent = peek_str_prop(s, props, "anchorParent", [&](const char *c, size_t n) {
            return parse_anchor_token(c, n, &style.anchor_parent);
        });
        const bool has_ox = opt_number_prop(s, props, "offsetX", &style.floating_offset.x, false, -16384.0, 16384.0);
        const bool has_oy = opt_number_prop(s, props, "offsetY", &style.floating_offset.y, false, -16384.0, 16384.0);
        style.has_floating_offset = has_ox || has_oy;
        int32_t zi = 0;
        if (opt_int_prop(s, props, "zIndex", -32768, 32767, &zi)) {
            style.z_index = int16_t(zi);
            style.has_z_index = true;
        }
        style.has_clip_children = opt_bool_prop(s, props, "clipChildren", &style.clip_children);
        style.pointer_passthrough = bool_prop(s, props, "pointerPassthrough");
        style.has_clamp_to_viewport = opt_bool_prop(s, props, "clampToViewport", &style.clamp_to_viewport);
        style.flip_on_overflow = bool_prop(s, props, "flipOnOverflow");
        style.has_scroll_x = opt_bool_prop(s, props, "scrollX", &style.scroll_x) ||
                             opt_bool_prop(s, props, "scroll_x", &style.scroll_x);
        style.has_scroll_y = opt_bool_prop(s, props, "scrollY", &style.scroll_y) ||
                             opt_bool_prop(s, props, "scroll_y", &style.scroll_y);
        style.animate = bool_prop(s, props, "animateLayout");
    }

    // Widget-specific properties
    if (kind == INLAY_UI_BUTTON || kind == INLAY_UI_BADGE || kind == INLAY_UI_INPUT ||
        kind == INLAY_UI_CHECKBOX || kind == INLAY_UI_TOGGLE || kind == INLAY_UI_TABS)
        parse_font_props(s, props, style);

    if (kind == INLAY_UI_BUTTON || kind == INLAY_UI_INPUT || kind == INLAY_UI_CHECKBOX ||
        kind == INLAY_UI_TOGGLE || kind == INLAY_UI_SLIDER || kind == INLAY_UI_COMBOBOX)
        style.disabled = bool_prop(s, props, "disabled");

    if (kind == INLAY_UI_BUTTON) {
        peek_str_prop(s, props, "variant", [&](const char *c, size_t n) {
            if (str_eq(c, n, "primary")) style.button_variant = ETCH_BUTTON_PRIMARY;
            else if (str_eq(c, n, "ghost")) style.button_variant = ETCH_BUTTON_GHOST;
            else if (str_eq(c, n, "danger")) style.button_variant = ETCH_BUTTON_DANGER;
            else style.button_variant = ETCH_BUTTON_DEFAULT;
            return true;
        });
        if (!parse_event_prop(s, props, "onClick", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_INPUT) {
        style.hover_border_color = string_prop(s, props, "hoverBorderColor");
        if (!parse_event_prop(s, props, "onChange", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_TOGGLE || kind == INLAY_UI_CHECKBOX) {
        style.checked = bool_prop(s, props, "checked");
        if (kind == INLAY_UI_CHECKBOX)
            style.indeterminate = bool_prop(s, props, "indeterminate");
        if (!parse_event_prop(s, props, "onChange", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_PROGRESS || kind == INLAY_UI_SLIDER) {
        style.track_color = string_prop(s, props, "trackColor");
        style.has_value = opt_number_prop(s, props, "value", &style.value, true);
        style.has_min = opt_number_prop(s, props, "min", &style.min_val, true);
        style.has_max = opt_number_prop(s, props, "max", &style.max_val, true);
        style.has_step = opt_number_prop(s, props, "step", &style.step, true);
        if (kind == INLAY_UI_SLIDER) {
            if (!parse_event_prop(s, props, "onChange", style))
                parse_event_prop(s, props, "action", style);
        }
    } else if (kind == INLAY_UI_IMAGE) {
        style.src = string_prop(s, props, "src");
        uint32_t icon = 0;
        style.has_sprite = opt_uint_prop(s, props, "spriteId", UINT32_MAX, &style.sprite_id);
        if (opt_uint_prop(s, props, "iconId", uint32_t(ETCH_INVALID_ICON - 1u), &icon)) {
            style.has_icon = true;
            style.icon_id = uint16_t(icon);
        }
        peek_str_prop(s, props, "fit", [&](const char *c, size_t n) {
            if (str_eq(c, n, "contain")) style.image_fit = ETCH_IMAGE_FIT_CONTAIN;
            else if (str_eq(c, n, "cover")) style.image_fit = ETCH_IMAGE_FIT_COVER;
            else style.image_fit = ETCH_IMAGE_FIT_FILL;
            return true;
        });
    } else if (kind == INLAY_UI_TABS) {
        parse_items_prop(s, props, 16u, style);
        int32_t idx = 0;
        if (opt_int_prop(s, props, "active", -1, 15, &idx) ||
            opt_int_prop(s, props, "selected", -1, 15, &idx) ||
            opt_int_prop(s, props, "value", -1, 15, &idx))
            style.int_value = idx;
        if (!parse_event_prop(s, props, "onChange", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_COMBOBOX) {
        parse_items_prop(s, props, 64u, style);
        style.hover_bg = string_prop(s, props, "hoverBg");
        int32_t idx = 0;
        if (opt_int_prop(s, props, "selected", -1, 63, &idx) ||
            opt_int_prop(s, props, "active", -1, 63, &idx) ||
            opt_int_prop(s, props, "value", -1, 63, &idx))
            style.int_value = idx;
        if (!parse_event_prop(s, props, "onChange", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_MODAL) {
        style.open = bool_prop(s, props, "open");
        if (!parse_event_prop(s, props, "onClose", style) &&
            !parse_event_prop(s, props, "onChange", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_POPOVER) {
        style.open = true;
        opt_bool_prop(s, props, "open", &style.open);
        if (!parse_event_prop(s, props, "onClose", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_SPLIT) {
        style.has_value = opt_number_prop(s, props, "ratio", &style.value, false, 0.0, 1.0);
        style.has_min = opt_number_prop(s, props, "minRatio", &style.min_val, false, 0.0, 1.0);
        style.has_max = opt_number_prop(s, props, "maxRatio", &style.max_val, false, 0.0, 1.0);
    } else if (kind == INLAY_UI_MATH) {
        style.display_math = true;
        opt_bool_prop(s, props, "display", &style.display_math);
    }
}

bool parse_tag_kind(const char *chars, size_t len, InlayUiKind *out) noexcept {
    if (str_eq(chars, len, "box")) { *out = INLAY_UI_BOX; return true; }
    if (str_eq(chars, len, "text")) { *out = INLAY_UI_TEXT; return true; }
    if (str_eq(chars, len, "button")) { *out = INLAY_UI_BUTTON; return true; }
    if (str_eq(chars, len, "input")) { *out = INLAY_UI_INPUT; return true; }
    if (str_eq(chars, len, "toggle")) { *out = INLAY_UI_TOGGLE; return true; }
    if (str_eq(chars, len, "scroll")) { *out = INLAY_UI_SCROLL; return true; }
    if (str_eq(chars, len, "progress")) { *out = INLAY_UI_PROGRESS; return true; }
    if (str_eq(chars, len, "slider")) { *out = INLAY_UI_SLIDER; return true; }
    if (str_eq(chars, len, "badge")) { *out = INLAY_UI_BADGE; return true; }
    if (str_eq(chars, len, "divider")) { *out = INLAY_UI_DIVIDER; return true; }
    if (str_eq(chars, len, "image")) { *out = INLAY_UI_IMAGE; return true; }
    if (str_eq(chars, len, "card")) { *out = INLAY_UI_CARD; return true; }
    if (str_eq(chars, len, "spacer")) { *out = INLAY_UI_SPACER; return true; }
    if (str_eq(chars, len, "checkbox")) { *out = INLAY_UI_CHECKBOX; return true; }
    if (str_eq(chars, len, "tabs")) { *out = INLAY_UI_TABS; return true; }
    if (str_eq(chars, len, "combobox")) { *out = INLAY_UI_COMBOBOX; return true; }
    if (str_eq(chars, len, "modal")) { *out = INLAY_UI_MODAL; return true; }
    if (str_eq(chars, len, "popover")) { *out = INLAY_UI_POPOVER; return true; }
    if (str_eq(chars, len, "tooltip")) { *out = INLAY_UI_TOOLTIP; return true; }
    if (str_eq(chars, len, "split")) { *out = INLAY_UI_SPLIT; return true; }
    if (str_eq(chars, len, "math")) { *out = INLAY_UI_MATH; return true; }
    if (str_eq(chars, len, "diagram")) { *out = INLAY_UI_DIAGRAM; return true; }
    if (str_eq(chars, len, "slot")) { *out = INLAY_UI_SLOT; return true; }
    return false;
}

uint32_t build(InlaySurface *s, JSValueConst value, uint32_t depth, uint32_t sibling_slot) {
    if (interrupt(s->js, s)) {
        JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
        return none;
    }
    if (JS_IsNull(value) || JS_IsUndefined(value) || JS_IsBool(value))
        return none;
    if (depth > 64 || s->node_count == s->desc.max_nodes || !ensure_node_capacity(s)) {
        JS_ThrowInternalError(s->js, "UI node/depth capacity exceeded");
        return none;
    }
    const uint32_t index = s->node_count++;
    s->nodes[index] = {INLAY_UI_TEXT, hash_slot_id(sibling_slot, INLAY_UI_TEXT), none, none, {}};
    s->styles[index] = {};
    s->styles[index].handler = none;
    s->styles[index].gradient_idx = none;
    s->styles[index].transform_idx = none;
    s->styles[index].size = 15.f;
    s->styles[index].opacity = 1.f;
    if (!JS_IsObject(value)) {
        s->nodes[index].text = copy_text(s, value);
        return index;
    }

    InlayUiKind kind = INLAY_UI_TEXT;
    const bool valid_tag = peek_str_prop(s, value, "tag", [&](const char *c, size_t n) {
        return parse_tag_kind(c, n, &kind);
    });
    if (!valid_tag) {
        JS_ThrowTypeError(s->js, "Unknown UI tag");
        return none;
    }
    s->nodes[index].kind = kind;

    const auto props = JS_GetPropertyStr(s->js, value, "props");
    if (!JS_IsObject(props)) {
        JS_FreeValue(s->js, props);
        JS_ThrowTypeError(s->js, "UI props must be an object");
        return none;
    }
    parse_style(s, props, kind, s->styles[index]);
    if (!hash_string_prop(s, props, "id", 2166136261u, &s->nodes[index].id) &&
        !hash_string_prop(s, props, "key", 0x811c9dc5u ^ 0x6b65793au, &s->nodes[index].id))
        s->nodes[index].id = hash_slot_id(sibling_slot, kind);

    if (kind == INLAY_UI_INPUT) {
        s->nodes[index].text = string_prop(s, props, "value");
        s->styles[index].placeholder = string_prop(s, props, "placeholder");
    } else if (kind == INLAY_UI_TOGGLE || kind == INLAY_UI_CHECKBOX) {
        s->nodes[index].text = string_prop(s, props, "label");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_SLIDER) {
        s->nodes[index].text = string_prop(s, props, "label");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_IMAGE) {
        s->nodes[index].text = string_prop(s, props, "alt");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_COMBOBOX) {
        s->styles[index].placeholder = string_prop(s, props, "placeholder");
        s->nodes[index].text = string_prop(s, props, "label");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_MODAL) {
        s->nodes[index].text = string_prop(s, props, "title");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_TOOLTIP) {
        s->nodes[index].text = string_prop(s, props, "text");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "tip");
    } else if (kind == INLAY_UI_MATH) {
        s->nodes[index].text = string_prop(s, props, "tex");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_DIAGRAM) {
        s->nodes[index].text = string_prop(s, props, "source");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
    } else if (kind == INLAY_UI_SLOT) {
        s->nodes[index].text = string_prop(s, props, "name");
        if (!s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "text");
        const auto sp = JS_GetPropertyStr(s->js, props, "props");
        if (!JS_IsUndefined(sp) && !JS_IsNull(sp)) {
            if (JS_IsString(sp)) {
                s->styles[index].placeholder = copy_text(s, sp);
            } else {
                const auto json = JS_JSONStringify(s->js, sp, JS_UNDEFINED, JS_UNDEFINED);
                if (!JS_IsException(json) && !JS_IsUndefined(json))
                    s->styles[index].placeholder = copy_text(s, json);
                JS_FreeValue(s->js, json);
            }
        }
        JS_FreeValue(s->js, sp);
    } else {
        s->nodes[index].text = string_prop(s, props, "text");
        if (kind == INLAY_UI_PROGRESS && !s->nodes[index].text.length)
            s->nodes[index].text = string_prop(s, props, "label");
    }
    JS_FreeValue(s->js, props);

    const auto children = JS_GetPropertyStr(s->js, value, "children");
    if (JS_IsArray(children)) {
        const auto len = JS_GetPropertyStr(s->js, children, "length");
        uint32_t count = 0;
        JS_ToUint32(s->js, &count, len);
        JS_FreeValue(s->js, len);
        uint32_t previous = none;
        if (count > s->desc.max_nodes)
            JS_ThrowInternalError(s->js, "Too many UI children");
        else
            for (uint32_t i = 0; i < count && !JS_HasException(s->js); ++i) {
                const auto child = JS_GetPropertyUint32(s->js, children, i);
                const uint32_t at = build(s, child, depth + 1, i);
                JS_FreeValue(s->js, child);
                if (at != none) {
                    if (previous == none)
                        s->nodes[index].first_child = at;
                    else
                        s->nodes[previous].next_sibling = at;
                    previous = at;
                }
            }
    }
    JS_FreeValue(s->js, children);
    if (!is_container_kind(kind) && !s->nodes[index].text.length && s->nodes[index].first_child != none)
        s->nodes[index].text = s->nodes[s->nodes[index].first_child].text;
    return index;
}

JSValue action(JSContext *js, JSValueConst, int argc, JSValueConst *argv) {
    auto *s = static_cast<InlaySurface *>(JS_GetContextOpaque(js));
    if (argc < 1)
        return JS_ThrowTypeError(js, "action requires a name");
    const char *name = JS_ToCString(js, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    Action *binding = nullptr;
    for (uint32_t i = 0; i < s->action_count; ++i)
        if (!std::strcmp(name, s->actions[i].name)) {
            binding = &s->actions[i];
            break;
        }
    JS_FreeCString(js, name);
    if (!binding)
        return JS_ThrowTypeError(js, "Action is not bound");
    const auto json =
        JS_JSONStringify(js, argc > 1 ? argv[1] : JS_NULL, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json))
        return json;
    if (JS_IsUndefined(json))
        return JS_ThrowTypeError(js, "Action payload is not JSON serializable");
    size_t length = 0;
    const char *payload = JS_ToCStringLen(js, &length, json);
    if (payload && length <= 65536)
        binding->callback(s, {payload, uint32_t(length), false}, binding->user);
    else if (payload)
        JS_ThrowTypeError(js, "Action payload exceeds 64KiB");
    if (payload)
        JS_FreeCString(js, payload);
    JS_FreeValue(js, json);
    return JS_HasException(js) ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue set_timeout(JSContext *js, JSValueConst, int argc, JSValueConst *argv) {
    auto *s = static_cast<InlaySurface *>(JS_GetContextOpaque(js));
    double delay = 0;
    if (argc < 1 || !JS_IsFunction(js, argv[0]) ||
        (argc > 1 && JS_ToFloat64(js, &delay, argv[1]) < 0) || !std::isfinite(delay) || delay < 0 ||
        delay > 86400000)
        return JS_ThrowTypeError(js, "Invalid timer");
    for (auto &timer : s->timers)
        if (JS_IsUndefined(timer.function)) {
            timer.function = JS_DupValue(js, argv[0]);
            timer.due = now_ms(s) + delay;
            timer.id = ++s->next_timer;
            return JS_NewUint32(js, timer.id);
        }
    return JS_ThrowInternalError(js, "Timer capacity exceeded");
}

JSValue clear_timeout(JSContext *js, JSValueConst, int argc, JSValueConst *argv) {
    auto *s = static_cast<InlaySurface *>(JS_GetContextOpaque(js));
    uint32_t id = 0;
    if (argc)
        JS_ToUint32(js, &id, argv[0]);
    for (auto &timer : s->timers)
        if (timer.id == id) {
            JS_FreeValue(js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0;
        }
    return JS_UNDEFINED;
}

bool install_state(InlaySurface *s, JSValueConst global) {
    const auto state = JS_ParseJSON(s->js, s->state, s->state_size, "<state>");
    if (JS_IsException(state))
        return false;
    return JS_SetPropertyStr(s->js, global, "state", state) >= 0;
}

void rejection(JSContext *js, JSValueConst promise, JSValueConst, bool handled, void *opaque) {
    auto *s = static_cast<InlaySurface *>(opaque);
    if (handled) {
        for (uint32_t i = 0; i < s->rejection_count; ++i)
            if (JS_IsStrictEqual(js, promise, s->rejections[i])) {
                JS_FreeValue(js, s->rejections[i]);
                s->rejections[i] = s->rejections[--s->rejection_count];
                break;
            }
    } else if (s->rejection_count < 32)
        s->rejections[s->rejection_count++] = JS_DupValue(js, promise);
    else
        error(s, "Too many unhandled Promise rejections");
}

bool drain_jobs(InlaySurface *s) {
    while (JS_IsJobPending(s->runtime)) {
        if (interrupt(s->js, s)) {
            JS_ThrowInternalError(s->js, "Script job deadline exceeded");
            return false;
        }
        JSContext *job_context = nullptr;
        if (JS_ExecutePendingJob(s->runtime, &job_context) < 0)
            return false;
    }
    return true;
}

struct WidgetEvent {
    WidgetEvent *next;
    EtchStr name, payload;
    uint32_t handler;
    bool is_click;
};
struct WidgetEvents {
    WidgetEvent *first, *last;
};

bool serialize_state(InlaySurface *s, JSValueConst global) {
    const auto current = JS_GetPropertyStr(s->js, global, "state");
    const auto json = JS_JSONStringify(s->js, current, JS_UNDEFINED, JS_UNDEFINED);
    bool ok = true;
    if (JS_IsUndefined(json)) {
        JS_ThrowTypeError(s->js, "State must be JSON serializable");
        ok = false;
    } else if (JS_IsException(json)) {
        ok = false;
    } else {
        size_t length = 0;
        const char *chars = JS_ToCStringLen(s->js, &length, json);
        if (chars) {
            if (!save_state(s, chars, length)) {
                JS_ThrowInternalError(s->js, "State persistence failed");
                ok = false;
            }
            JS_FreeCString(s->js, chars);
        }
    }
    JS_FreeValue(s->js, json);
    JS_FreeValue(s->js, current);
    if (ok)
        s->state_dirty = false;
    return ok;
}

void sync_state_json(InlaySurface *s) {
    if (!s || !s->state_dirty || !s->js || !s->loaded || s->running || s->pending_state || s->stats.has_error)
        return;
    s->running = true;
    s->timed_out = false;
    s->deadline = SDL_GetPerformanceCounter() + uint64_t(double(s->desc.max_tick_ms) * s->frequency / 1000);
    const auto global = JS_GetGlobalObject(s->js);
    if (!serialize_state(s, global) || s->timed_out || JS_HasException(s->js)) {
        const auto exception = JS_GetException(s->js);
        const char *message = JS_ToCString(s->js, exception);
        error(s, s->timed_out ? "State serialization exceeded tick limit"
                 : message    ? message
                              : "State serialization failed");
        if (message)
            JS_FreeCString(s->js, message);
        JS_FreeValue(s->js, exception);
        s->state_dirty = false;
    }
    JS_FreeValue(s->js, global);
    s->deadline = 0;
    s->running = false;
}

bool rebuild(InlaySurface *s, bool eager_state) {
    if (!drain_jobs(s))
        return false;
    bool repeat = false;
    do {
        const auto global = JS_GetGlobalObject(s->js);
        JS_SetPropertyStr(s->js, global, "__inlay_hi", JS_NewUint32(s->js, 0));
        JS_SetPropertyStr(s->js, global, "__inlay_dirty", JS_FALSE);
        const auto render = JS_GetPropertyStr(s->js, global, "render");
        auto state = JS_GetPropertyStr(s->js, global, "state");
        if (!JS_IsFunction(s->js, render))
            JS_ThrowTypeError(s->js, "Script must define render(state)");
        const auto tree =
            JS_HasException(s->js) ? JS_EXCEPTION : JS_Call(s->js, render, global, 1, &state);
        clear_handlers(s);
        s->node_count = 0;
        s->text_used = 0;
        s->shadow_pool_count = 0;
        s->gradient_pool_count = 0;
        s->transform_pool_count = 0;
        s->item_pool_count = 0;
        if (!JS_IsException(tree))
            build(s, tree, 0, 0);
        JS_FreeValue(s->js, tree);
        JS_FreeValue(s->js, render);
        JS_FreeValue(s->js, state);
        if (!JS_HasException(s->js) && !s->pending_state) {
            if (eager_state || s->path[0]) {
                serialize_state(s, global);
            } else {
                const auto current = JS_GetPropertyStr(s->js, global, "state");
                if (JS_IsUndefined(current))
                    JS_ThrowTypeError(s->js, "State must be JSON serializable");
                else
                    s->state_dirty = true;
                JS_FreeValue(s->js, current);
            }
        }
        const bool jobs = JS_IsJobPending(s->runtime);
        if (jobs && !JS_HasException(s->js) && !drain_jobs(s)) {
            JS_FreeValue(s->js, global);
            return false;
        }
        const bool hook_dirty = !JS_HasException(s->js) && bool_prop(s, global, "__inlay_dirty");
        JS_FreeValue(s->js, global);
        repeat = (jobs || hook_dirty) && !JS_HasException(s->js);
        if (repeat && interrupt(s->js, s)) {
            JS_ThrowInternalError(s->js, "Script update deadline exceeded");
            return false;
        }
    } while (repeat);
    if (s->rejection_count)
        JS_ThrowInternalError(s->js, "Unhandled Promise rejection");
    s->stats.node_count = s->node_count;
    return !JS_HasException(s->js);
}

bool dispatch_one_event(InlaySurface *s, JSValueConst global, const WidgetEvent &ev) {
    if (ev.handler != none) {
        if (ev.handler >= s->handler_count)
            return true;
        const auto fn = JS_DupValue(s->js, s->handlers[ev.handler]);
        const auto state = JS_GetPropertyStr(s->js, global, "state");
        JSValue result = JS_UNDEFINED;
        if (ev.is_click) {
            JSValue args[] = {state};
            result = JS_Call(s->js, fn, global, 1, args);
        } else {
            const auto parsed = parse_json(s, ev.payload);
            if (!JS_IsException(parsed)) {
                JSValue args[] = {parsed, state};
                result = JS_Call(s->js, fn, global, 2, args);
            }
            JS_FreeValue(s->js, parsed);
        }
        JS_FreeValue(s->js, result);
        JS_FreeValue(s->js, state);
        JS_FreeValue(s->js, fn);
        return !JS_HasException(s->js);
    }
    if (!ev.name.length)
        return true;
    const auto parsed = parse_json(s, ev.payload);
    if (!JS_IsException(parsed)) {
        const auto handler = JS_GetPropertyStr(s->js, global, "onEvent");
        if (JS_IsFunction(s->js, handler)) {
            JSValue args[] = {JS_NewStringLen(s->js, ev.name.chars, ev.name.length),
                              JS_DupValue(s->js, parsed),
                              JS_GetPropertyStr(s->js, global, "state")};
            if (!JS_IsException(args[1])) {
                const auto result = JS_Call(s->js, handler, global, 3, args);
                JS_FreeValue(s->js, result);
            }
            for (auto arg : args)
                JS_FreeValue(s->js, arg);
        } else {
            for (uint32_t i = 0; i < s->action_count; ++i)
                if (equal(ev.name, s->actions[i].name)) {
                    s->actions[i].callback(s, ev.payload, s->actions[i].user);
                    break;
                }
        }
        JS_FreeValue(s->js, handler);
    }
    JS_FreeValue(s->js, parsed);
    return !JS_HasException(s->js);
}

bool run_batch(InlaySurface *s, const WidgetEvent *first) {
    const uint64_t start = SDL_GetPerformanceCounter();
    begin_tick(s);
    s->stats.has_error = false;
    s->error[0] = 0;
    const auto global = JS_GetGlobalObject(s->js);
    bool ok = true;
    if (s->pending_state) {
        s->pending_state = false;
        s->state_dirty = false;
        ok = install_state(s, global);
    }
    for (const auto *ev = first; ok && ev && !JS_HasException(s->js); ev = ev->next)
        ok = dispatch_one_event(s, global, *ev) && ok;
    if (!JS_HasException(s->js))
        ok = rebuild(s, false) && ok;
    JS_FreeValue(s->js, global);
    const bool result = finish_tick(s, start) && ok;
    etch_request_redraw(s->etch);
    return result;
}

bool run_update(InlaySurface *s, EtchStr name, EtchStr payload) {
    WidgetEvent ev = {nullptr, name, payload, none, false};
    return run_batch(s, name.length ? &ev : nullptr);
}

void clear_context(InlaySurface *s) {
    if (s->js) {
        clear_handlers(s);
        for (uint32_t i = 0; i < s->rejection_count; ++i)
            JS_FreeValue(s->js, s->rejections[i]);
        s->rejection_count = 0;
        for (auto &timer : s->timers) {
            JS_FreeValue(s->js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0;
        }
        JS_FreeContext(s->js);
        s->js = nullptr;
    }
}

bool load(InlaySurface *s, EtchStr script, bool reuse = false) {
    static const char
        prefix[] = "(function(){\n",
        suffix[] = "\n;return {render:typeof render==='function'?render:undefined,onEvent:typeof "
                   "onEvent==='function'?onEvent:undefined};})()";
    if (s->state_dirty && s->js && s->loaded && !s->stats.has_error && !s->pending_state)
        sync_state_json(s);
    const uint32_t needed = script.length + uint32_t(sizeof(prefix) + sizeof(suffix));
    if (needed > s->source_capacity) {
        uint32_t cap = s->source_capacity ? s->source_capacity : 4096;
        while (cap < needed)
            cap *= 2;
        auto *memory = static_cast<char *>(std::realloc(s->source, cap));
        if (!memory) {
            error(s, "Source storage allocation failed");
            return false;
        }
        s->source = memory;
        s->source_capacity = cap;
    }
    std::memcpy(s->source, prefix, sizeof(prefix) - 1);
    if (script.length)
        std::memcpy(s->source + sizeof(prefix) - 1, script.chars, script.length);
    std::memcpy(s->source + sizeof(prefix) - 1 + script.length, suffix, sizeof(suffix));
    const size_t length = sizeof(prefix) - 1 + script.length + sizeof(suffix) - 1;
    const bool fresh = !reuse || !s->loaded || s->stats.has_error || !s->js;
    if (fresh) {
        clear_context(s);
        if (s->runtime)
            JS_FreeRuntime(s->runtime);
        s->runtime = JS_NewRuntime();
        if (!s->runtime) {
            error(s, "Unable to allocate script runtime");
            return false;
        }
        JS_SetMemoryLimit(s->runtime, s->desc.memory_limit_bytes);
        JS_SetMaxStackSize(s->runtime, s->desc.max_stack_bytes);
        JS_SetInterruptHandler(s->runtime, interrupt, s);
        JS_SetHostPromiseRejectionTracker(s->runtime, rejection, s);
        s->js = JS_NewContext(s->runtime);
        if (!s->js) {
            error(s, "Unable to allocate script context");
            return false;
        }
        JS_SetContextOpaque(s->js, s);
    } else
        for (auto &timer : s->timers) {
            JS_FreeValue(s->js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0;
        }
    const uint64_t start = SDL_GetPerformanceCounter();
    begin_tick(s);
    s->stats.has_error = false;
    s->error[0] = 0;
    s->pending_state = false;
    s->state_dirty = false;
    const auto global = JS_GetGlobalObject(s->js);
    if (fresh) {
        JS_SetPropertyStr(s->js, global, "action", JS_NewCFunction(s->js, action, "action", 2));
        JS_SetPropertyStr(s->js, global, "setTimeout",
                          JS_NewCFunction(s->js, set_timeout, "setTimeout", 2));
        JS_SetPropertyStr(s->js, global, "clearTimeout",
                          JS_NewCFunction(s->js, clear_timeout, "clearTimeout", 1));
        JS_SetPropertyStr(s->js, global, "Atomics", JS_UNDEFINED);
        JS_SetPropertyStr(s->js, global, "SharedArrayBuffer", JS_UNDEFINED);
        static const char bootstrap[] =
            "globalThis.h=(tag,props,...children)=>({tag,props:props||{},children:children.flat()});"
            "(()=>{const isP=p=>p&&typeof p==='object'&&!Array.isArray(p)&&typeof p.tag!=='string',"
            "m=t=>(p,...c)=>isP(p)?h(t,p,...c):(p===undefined&&!c.length?h(t,{}):h(t,{},p,...c)),"
            "x=t=>(p,...c)=>typeof p==='string'?h(t,{text:p},...c):m(t)(p,...c),"
            "a=ax=>(p,...c)=>isP(p)?h('box',Object.assign({axis:ax},p),...c):(p===undefined&&!c.length?h('box',{axis:ax}):h('box',{axis:ax},p,...c)),"
            "sl=(p,...c)=>typeof p==='string'?h('slot',{name:p},...c):m('slot')(p,...c);"
            "globalThis.ui={box:m('box'),row:a('row'),col:a('column'),card:m('card'),spacer:m('spacer'),"
            "text:x('text'),button:x('button'),input:m('input'),toggle:m('toggle'),checkbox:m('checkbox'),"
            "tabs:m('tabs'),combobox:m('combobox'),scroll:m('scroll'),progress:m('progress'),"
            "slider:m('slider'),badge:x('badge'),divider:m('divider'),image:m('image'),"
            "modal:m('modal'),popover:m('popover'),tooltip:m('tooltip'),split:m('split'),"
            "math:x('math'),diagram:x('diagram'),slot:sl};})();"
            "globalThis.__inlay_hooks=[];globalThis.__inlay_hi=0;globalThis.__inlay_dirty=false;"
            "globalThis.useState=init=>{const i=globalThis.__inlay_hi++,a=globalThis.__inlay_hooks;"
            "if(i>=a.length)a[i]=typeof init==='function'?init():init;"
            "return[a[i],v=>{const n=typeof v==='function'?v(a[i]):v;if(!Object.is(a[i],n)){a[i]=n;globalThis.__inlay_dirty=true;}}];};";
        auto bootstrap_result =
            JS_Eval(s->js, bootstrap, sizeof(bootstrap) - 1, "<inlay>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(s->js, bootstrap_result);
    }
    JSValue result = JS_UNDEFINED;
    bool ok = install_state(s, global);
    JS_FreeValue(s->js, global);
    if (ok) {
        result = JS_Eval(s->js, s->source, length, "<surface>", JS_EVAL_TYPE_GLOBAL);
        ok = !JS_IsException(result);
        if (ok) {
            const auto target = JS_GetGlobalObject(s->js);
            JS_SetPropertyStr(s->js, target, "render", JS_GetPropertyStr(s->js, result, "render"));
            JS_SetPropertyStr(s->js, target, "onEvent",
                              JS_GetPropertyStr(s->js, result, "onEvent"));
            JS_FreeValue(s->js, target);
        }
        JS_FreeValue(s->js, result);
    }
    if (ok && !JS_HasException(s->js))
        ok = rebuild(s, true);
    const bool completed = finish_tick(s, start) && ok;
    s->loaded = completed;
    etch_request_redraw(s->etch);
    return completed;
}

EtchColor resolve_color(EtchStr value, const EtchTheme &theme, EtchColor fallback) noexcept {
    if (!value.length || !value.chars)
        return fallback;
    if (value.chars[0] == '#') {
        const auto hex_digit = [](char c) noexcept -> int {
            return c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                   : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                          : -1;
        };
        if (value.length == 7 || value.length == 9) {
            uint32_t rgba = 0;
            for (uint32_t i = 1; i < value.length; ++i) {
                const int d = hex_digit(value.chars[i]);
                if (d < 0)
                    return fallback;
                rgba = (rgba << 4) | uint32_t(d);
            }
            if (value.length == 7)
                return etch_rgba8(uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba), 255);
            return etch_rgba8(uint8_t(rgba >> 24), uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba));
        }
        if (value.length == 4 || value.length == 5) {
            uint8_t ch[4] = {0, 0, 0, 255};
            for (uint32_t i = 1; i < value.length; ++i) {
                const int d = hex_digit(value.chars[i]);
                if (d < 0)
                    return fallback;
                ch[i - 1] = uint8_t(d * 17);
            }
            return etch_rgba8(ch[0], ch[1], ch[2], ch[3]);
        }
        return fallback;
    }
    if (equal(value, "transparent"))
        return etch_rgba8(0, 0, 0, 0);
    if (value.length > 12 && value.chars[value.length - 1] == ')' &&
        !std::memcmp(value.chars, "var(--etch-", 11)) {
        const char *tok = value.chars + 11;
        const size_t n = value.length - 12;
        const bool dark = inlay_is_dark_theme(theme);
        if (str_eq(tok, n, "bg-canvas")) return theme.bg_canvas;
        if (str_eq(tok, n, "bg-surface")) return theme.bg_surface;
        if (str_eq(tok, n, "bg-elevated")) return theme.bg_elevated;
        if (str_eq(tok, n, "bg-chrome")) return theme.bg_chrome;
        if (str_eq(tok, n, "bg-popup")) return theme.bg_popup;
        if (str_eq(tok, n, "bg-tooltip")) return theme.bg_tooltip;
        if (str_eq(tok, n, "bg-input")) return theme.bg_input;
        if (str_eq(tok, n, "bg-table")) return theme.bg_table;
        if (str_eq(tok, n, "bg-node-canvas")) return theme.bg_node_canvas;
        if (str_eq(tok, n, "surface-control")) return theme.surface_control;
        if (str_eq(tok, n, "surface-hover")) return theme.surface_hover;
        if (str_eq(tok, n, "surface-active")) return theme.surface_active;
        if (str_eq(tok, n, "surface-disabled")) return theme.surface_disabled;
        if (str_eq(tok, n, "surface-subtle")) return theme.surface_subtle;
        if (str_eq(tok, n, "surface-subtle-hover")) return theme.surface_subtle_hover;
        if (str_eq(tok, n, "surface-subtle-body")) return theme.surface_subtle_body;
        if (str_eq(tok, n, "surface-selected")) return theme.surface_selected;
        if (str_eq(tok, n, "text-primary")) return theme.text_primary;
        if (str_eq(tok, n, "text-secondary")) return theme.text_secondary;
        if (str_eq(tok, n, "text-body")) return theme.text_body;
        if (str_eq(tok, n, "text-code")) return theme.text_code;
        if (str_eq(tok, n, "text-muted")) return theme.text_muted;
        if (str_eq(tok, n, "text-disabled")) return theme.text_disabled;
        if (str_eq(tok, n, "accent")) return theme.accent;
        if (str_eq(tok, n, "accent-secondary")) return theme.accent_secondary;
        if (str_eq(tok, n, "accent-primary")) return theme.accent_primary;
        if (str_eq(tok, n, "accent-hover")) {
            return etch_color_blend_over(
                theme.accent, dark ? etch_rgba8(255, 255, 255, 28) : etch_rgba8(0, 0, 0, 24));
        }
        if (str_eq(tok, n, "focus-ring")) return theme.focus_ring;
        if (str_eq(tok, n, "danger")) return theme.danger;
        if (str_eq(tok, n, "warning")) return theme.warning;
        if (str_eq(tok, n, "success"))
            return dark ? etch_rgb_hex(0x34D399, 255) : etch_rgb_hex(0x16A34A, 255);
        if (str_eq(tok, n, "info"))
            return dark ? etch_rgb_hex(0x60A5FA, 255) : etch_rgb_hex(0x2563EB, 255);
        if (str_eq(tok, n, "border-subtle")) return theme.border_subtle;
        if (str_eq(tok, n, "border-line")) return theme.border_line;
        if (str_eq(tok, n, "border-input")) return theme.border_input;
        if (str_eq(tok, n, "border-node")) return theme.border_node;
        if (str_eq(tok, n, "border-control")) return theme.border_control;
        if (str_eq(tok, n, "border-tooltip")) return theme.border_tooltip;
        if (str_eq(tok, n, "selection")) return theme.selection;
        if (str_eq(tok, n, "selection-hover")) return theme.selection_hover;
        if (str_eq(tok, n, "zebra-row")) return theme.zebra_row;
        if (str_eq(tok, n, "active-line")) return theme.active_line;
        if (str_eq(tok, n, "backdrop")) return theme.backdrop;
        if (str_eq(tok, n, "shadow")) return theme.shadow;
        if (str_eq(tok, n, "on-primary")) return theme.on_primary;
        if (str_eq(tok, n, "on-error")) return theme.on_error;
        if (str_eq(tok, n, "syntax-comment")) return theme.syntax_comment;
        if (str_eq(tok, n, "syntax-string")) return theme.syntax_string;
        if (str_eq(tok, n, "syntax-number")) return theme.syntax_number;
        if (str_eq(tok, n, "syntax-keyword")) return theme.syntax_keyword;
    }
    return fallback;
}

float resolve_corner_val(float px, uint8_t tok, const EtchTheme &theme) noexcept {
    switch (tok) {
        case RadPanel:   return theme.radius_panel_px;
        case RadControl: return theme.radius_control_px;
        case RadInput:   return theme.radius_input_px;
        case RadSmall:   return theme.radius_small_px;
        case RadPill:    return theme.radius_pill_px;
        default:         return px;
    }
}

EtchCorners resolve_corners(const Style &st, const EtchTheme &theme, float fallback) noexcept {
    if (!st.radius_mode)
        return etch_corners_all(fallback);
    return {
        resolve_corner_val(st.radius_vals.tl, st.radius_tok[0], theme),
        resolve_corner_val(st.radius_vals.tr, st.radius_tok[1], theme),
        resolve_corner_val(st.radius_vals.br, st.radius_tok[2], theme),
        resolve_corner_val(st.radius_vals.bl, st.radius_tok[3], theme),
    };
}

EtchSizing resolve_sizing(const SizingSpec &sp, EtchSizing fallback) noexcept {
    EtchSizing r = fallback;
    if (sp.explicit_set) {
        if (sp.kind == ETCH_SIZING_GROW) {
            r = etch_sizing_grow(sp.min_px, sp.max_px);
        } else if (sp.kind == ETCH_SIZING_FIXED) {
            float v = sp.value;
            if (sp.min_px > 0.f && v < sp.min_px) v = sp.min_px;
            if (sp.max_px > 0.f && v > sp.max_px) v = sp.max_px;
            r = etch_sizing_fixed(v);
        } else if (sp.kind == ETCH_SIZING_PERCENT) {
            r = etch_sizing_percent(sp.value);
            r.min = sp.min_px;
            r.max = sp.max_px;
        } else {
            r = etch_sizing_fit(sp.min_px, sp.max_px);
        }
    } else {
        if (sp.min_px > 0.f) r.min = sp.min_px;
        if (sp.max_px > 0.f) r.max = sp.max_px;
    }
    return r;
}

bool has_border_sides(EtchPadding sides) noexcept {
    return sides.left > 0 || sides.right > 0 || sides.top > 0 || sides.bottom > 0;
}

EtchStr frame_text(InlaySurface *s, EtchStr text) {
    return text.length ? etch_frame_arena_push_str(s->etch, text.chars, text.length) : EtchStr{};
}

void queue_event(InlaySurface *s, WidgetEvents *events, EtchStr name, uint32_t handler,
                 EtchStr payload, bool is_click = false) {
    const bool has_name = name.length && name.length < 64;
    const bool has_handler = handler != none;
    if ((!has_name && !has_handler) || (!is_click && !payload.length))
        return;
    auto *event = static_cast<WidgetEvent *>(
        etch_frame_arena_alloc(s->etch, sizeof(WidgetEvent), alignof(WidgetEvent)));
    if (!event)
        return;
    EtchStr copy = {};
    if (has_name) {
        copy = frame_text(s, name);
        if (!copy.chars)
            return;
    }
    *event = {nullptr, copy, payload, handler, is_click};
    if (events->last)
        events->last->next = event;
    else
        events->first = event;
    events->last = event;
}

EtchStr input_json(EtchContext *ctx, const char *text, uint32_t length) {
    auto *json = static_cast<char *>(etch_frame_arena_alloc(ctx, size_t(length) * 6 + 2, 1));
    if (!json)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    uint32_t out = 0;
    json[out++] = '"';
    for (uint32_t i = 0; i < length; ++i) {
        const auto c = uint8_t(text[i]);
        if (c == '"' || c == '\\') {
            json[out++] = '\\';
            json[out++] = char(c);
        } else if (c < 32) {
            std::memcpy(json + out, "\\u00", 4);
            out += 4;
            json[out++] = hex[c >> 4];
            json[out++] = hex[c & 15];
        } else
            json[out++] = char(c);
    }
    json[out++] = '"';
    return {json, out, false};
}

void populate_box_desc(InlaySurface *s, const Style &st, const EtchTheme &theme,
                       uint32_t id, EtchBoxDesc &box, bool use_layer) noexcept {
    const bool dark = inlay_is_dark_theme(theme);
    const EtchColor default_inset = dark ? etch_rgba8(0, 0, 0, 120) : etch_rgba8(15, 23, 42, 38);
    box.id = id;
    box.axis = st.column ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
    box.width = resolve_sizing(st.w, box.width);
    box.height = resolve_sizing(st.h, box.height);
    if (st.has_padding)
        box.padding = st.padding;
    if (st.has_margin)
        box.margin = st.margin;
    if (st.has_gap)
        box.gap = st.gap;
    if (st.has_row_gap)
        box.row_gap = st.row_gap;
    if (st.has_align_x)
        box.align_x = st.align_x;
    if (st.has_align_y)
        box.align_y = st.align_y;
    if (st.has_justify)
        box.justify = st.justify;
    if (st.has_align_self)
        box.align_self = st.align_self;
    if (st.has_flex_grow)
        box.flex_grow = st.flex_grow;
    if (st.has_flex_shrink)
        box.flex_shrink = st.flex_shrink;
    if (st.has_wrap)
        box.wrap = st.wrap_flag;
    if (st.has_aspect_ratio)
        box.aspect_ratio = st.aspect_ratio;
    if (st.shrink_width)
        box.shrink_width = true;
    if (st.shrink_height)
        box.shrink_height = true;
    if (st.grid_cols > 0)
        box.grid_columns = st.grid_cols;
    if (st.col_span > 1)
        box.grid_col_span = st.col_span;
    if (st.row_span > 1)
        box.grid_row_span = st.row_span;

    if (st.background.length)
        box.background = resolve_color(st.background, theme, box.background);
    if (st.has_border_width)
        box.border_width = std::fmax(0.f, st.border_width);
    if (has_border_sides(st.border_sides))
        box.border_sides = st.border_sides;
    if (st.border_color.length)
        box.border_color = resolve_color(st.border_color, theme, theme.border_subtle);
    else if ((box.border_width > 0.f || has_border_sides(box.border_sides)) && box.border_color.a == 0u)
        box.border_color = theme.border_subtle;
    if (st.radius_mode)
        box.radius = resolve_corners(st, theme, 0.f);
    if (st.has_continuous_corners)
        box.continuous_corners = st.continuous_corners;

    if (st.has_elevation)
        box.elevation = st.elevation;
    if (st.shadow_count > 0u) {
        const uint8_t sc = st.shadow_count > 4u ? 4u : st.shadow_count;
        box.shadow_count = sc;
        for (uint8_t i = 0; i < sc; ++i) {
            const auto &sh = s->shadow_pool[st.shadow_offset + i];
            auto sl = etch_shadow_layer_default();
            sl.offset = {sh.x, sh.y};
            sl.blur_px = std::fmax(0.f, sh.blur);
            sl.spread_px = sh.spread;
            sl.inset = sh.inset;
            const EtchColor fallback_sh = sh.inset ? default_inset : theme.shadow;
            sl.color = sh.color.length ? resolve_color(sh.color, theme, fallback_sh) : fallback_sh;
            box.shadows[i] = sl;
        }
    }
    if (st.inset_shadow_bool) {
        box.shadow_inset = true;
        if (box.shadow_color.a == 0u)
            box.shadow_color = default_inset;
        if (box.shadow_blur <= 0.f) {
            box.shadow_blur = 8.f;
            box.shadow_offset = {0.f, 1.f};
        }
    }
    if (st.gradient_idx != none) {
        const auto &gs = s->gradient_pool[st.gradient_idx];
        auto gd = etch_gradient_desc_default();
        gd.kind = gs.kind;
        gd.oklab = gs.oklab;
        gd.dir = {gs.dir_x, gs.dir_y};
        gd.center = {gs.center_x, gs.center_y};
        gd.angle_rad = gs.angle_rad;
        gd.stop_count = gs.stop_count;
        for (uint8_t i = 0; i < gs.stop_count; ++i) {
            gd.stops[i].color = resolve_color(gs.stop_colors[i], theme, theme.accent);
            gd.stops[i].position = gs.stop_pos[i];
        }
        box.gradient = gd;
    }
    if (st.backdrop_blur > 0.f)
        box.backdrop_blur = st.backdrop_blur;
    if (st.has_backdrop_saturation)
        box.backdrop_saturation = st.backdrop_saturation;
    if (st.has_backdrop_brightness)
        box.backdrop_brightness = st.backdrop_brightness;
    if (st.backdrop_noise > 0.f)
        box.backdrop_noise = st.backdrop_noise;
    if (st.has_top_rim_alpha)
        box.top_rim_alpha = st.top_rim_alpha;
    if (st.hover_bg.length)
        box.hover_background = resolve_color(st.hover_bg, theme, box.hover_background);
    if (st.hover_border_color.length)
        box.hover_border_color = resolve_color(st.hover_border_color, theme, box.hover_border_color);
    if (st.has_hover_scale)
        box.hover_scale = st.hover_scale;
    if (st.has_press_scale)
        box.press_scale = st.press_scale;

    if (st.has_attach_to)
        box.attach_to = st.attach_to;
    if (st.has_anchor_element)
        box.anchor_element = st.anchor_element;
    if (st.has_anchor_parent)
        box.anchor_parent = st.anchor_parent;
    if (st.has_floating_offset)
        box.floating_offset = st.floating_offset;
    if (st.has_z_index)
        box.z_index = st.z_index;
    if (st.has_clip_children)
        box.clip_children = st.clip_children;
    if (st.pointer_passthrough)
        box.pointer_passthrough = true;
    if (st.has_clamp_to_viewport)
        box.clamp_to_viewport = st.clamp_to_viewport;
    if (st.flip_on_overflow)
        box.flip_on_overflow = true;

    box.animate_layout = st.animate;
    if (st.has_opacity && !use_layer)
        box.opacity = st.opacity;
    if (st.has_scroll_x)
        box.scroll_x = st.scroll_x;
    if (st.has_scroll_y)
        box.scroll_y = st.scroll_y;
    if ((box.scroll_x || box.scroll_y) && !st.has_clip_children)
        box.clip_children = true;
}

struct UiMathDraw {
    InlayMathPrepared prep;
    EtchColor color;
    EtchPadding pad;
    EtchAlignX align_x;
    EtchAlignY align_y;
    bool has_align_x;
    bool has_align_y;
};

void draw_ui_math(EtchRect bounds, void *user) {
    const auto &d = *static_cast<const UiMathDraw *>(user);
    const float inner_w = std::fmax(0.f, bounds.w - d.pad.left - d.pad.right);
    const float inner_h = std::fmax(0.f, bounds.h - d.pad.top - d.pad.bottom);
    float ox = d.pad.left;
    float oy = d.pad.top;
    if (d.has_align_x && inner_w > d.prep.metrics.width) {
        if (d.align_x == ETCH_ALIGN_X_CENTER) ox += (inner_w - d.prep.metrics.width) * 0.5f;
        else if (d.align_x == ETCH_ALIGN_X_RIGHT) ox += (inner_w - d.prep.metrics.width);
    }
    if (d.has_align_y && inner_h > d.prep.metrics.height) {
        if (d.align_y == ETCH_ALIGN_Y_CENTER) oy += (inner_h - d.prep.metrics.height) * 0.5f;
        else if (d.align_y == ETCH_ALIGN_Y_BOTTOM) oy += (inner_h - d.prep.metrics.height);
    }
    inlay_math_draw_prepared(d.prep, {ox, oy}, d.color);
}

struct UiDiagramDraw {
    InlayDiagramResult result;
    InlayDiagramMeta meta;
    InlayDiagramDesc desc;
};

void draw_ui_diagram(EtchRect, void *user) {
    const auto &d = *static_cast<const UiDiagramDraw *>(user);
    inlay_diagram_draw_ex(d.result, d.desc, &d.meta);
}

bool is_activated(uint32_t id) noexcept {
    return etch_clicked(id) ||
           (etch_focused(id) && (etch_key_pressed(SDLK_RETURN, false) ||
                                 etch_key_pressed(SDLK_KP_ENTER, false) ||
                                 etch_key_pressed(SDLK_SPACE, false)));
}

bool box_handles_opacity(InlayUiKind kind) noexcept {
    switch (kind) {
        case INLAY_UI_BOX:
        case INLAY_UI_CARD:
        case INLAY_UI_SCROLL:
        case INLAY_UI_POPOVER:
        case INLAY_UI_BADGE:
        case INLAY_UI_DIVIDER:
        case INLAY_UI_SLOT:
        case INLAY_UI_TABS:
            return true;
        default:
            return false;
    }
}

void draw_node(InlaySurface *s, uint32_t index, uint32_t parent, const EtchTheme &theme, WidgetEvents *events) {
    const auto &n = s->nodes[index];
    const auto &st = s->styles[index];
    if ((n.kind == INLAY_UI_MODAL || n.kind == INLAY_UI_POPOVER) && !st.open)
        return;

    const uint32_t id = etch_id_index(parent, n.id);
    const bool sides = has_border_sides(st.border_sides);
    const bool dark = inlay_is_dark_theme(theme);
    const bool has_transform = st.transform_idx != none;
    const bool use_layer =
        has_transform || (st.has_opacity && st.opacity < 1.f && !box_handles_opacity(n.kind));

    if (use_layer) {
        auto layer = etch_layer_desc_default();
        if (st.has_opacity)
            layer.opacity = st.opacity;
        if (has_transform) {
            const auto &tf = s->transform_pool[st.transform_idx];
            layer.translate = {tf.tx, tf.ty};
            layer.scale = {tf.sx, tf.sy};
            layer.rotation_rad = tf.rotation_rad;
        }
        etch_push_layer(&layer);
    }

    if (n.kind == INLAY_UI_BOX || n.kind == INLAY_UI_SCROLL || n.kind == INLAY_UI_CARD ||
        n.kind == INLAY_UI_POPOVER) {
        auto box = etch_box_desc_default();
        if (n.kind == INLAY_UI_SCROLL) {
            box.scroll_x = false;
            box.scroll_y = true;
            box.clip_children = true;
            box.show_scrollbar = true;
        } else if (n.kind == INLAY_UI_CARD) {
            box.padding = etch_padding_all(16.f);
            box.gap = 10.f;
            box.background = theme.bg_elevated;
            box.border_color = theme.border_subtle;
            box.border_width = 1.f;
            box.radius = etch_corners_all(theme.radius_panel_px);
            box.continuous_corners = true;
            box.elevation = ETCH_ELEVATION_MD;
            box.top_rim_alpha = dark ? 0.14f : 0.0f;
        } else if (n.kind == INLAY_UI_POPOVER) {
            box.attach_to = ETCH_ATTACH_PARENT;
            box.anchor_parent = ETCH_ANCHOR_BOTTOM_LEFT;
            box.anchor_element = ETCH_ANCHOR_TOP_LEFT;
            box.z_index = 60;
            box.elevation = ETCH_ELEVATION_LG;
            box.clamp_to_viewport = true;
            box.continuous_corners = true;
            box.background = theme.bg_elevated;
            box.border_color = theme.border_subtle;
            box.border_width = 1.f;
            box.radius = etch_corners_all(theme.radius_panel_px);
            box.padding = etch_padding_all(12.f);
            box.gap = 6.f;
        }
        populate_box_desc(s, st, theme, id, box, use_layer);
        etch_begin_box(&box);
        for (uint32_t at = n.first_child; at != none; at = s->nodes[at].next_sibling)
            draw_node(s, at, id, theme, events);
        etch_end_box();
        if (n.kind == INLAY_UI_POPOVER && (st.action.length || st.handler != none) &&
            etch_should_close_popover(id, box.attach_to == ETCH_ATTACH_ROOT ? 0u : parent)) {
            queue_event(s, events, st.action, st.handler, ETCH_STR_LIT("false"));
        }
    } else if (n.kind == INLAY_UI_SPACER) {
        auto box = etch_box_desc_default();
        box.id = id;
        if (st.has_spacer_size) {
            box.width = resolve_sizing(st.w, etch_sizing_fixed(st.spacer_size));
            box.height = resolve_sizing(st.h, etch_sizing_fixed(st.spacer_size));
        } else if (st.w.explicit_set || st.h.explicit_set) {
            box.width = resolve_sizing(st.w, etch_sizing_fit(0, 0));
            box.height = resolve_sizing(st.h, etch_sizing_fit(0, 0));
        } else {
            box.width = resolve_sizing(st.w, etch_sizing_grow(0, 0));
            box.height = resolve_sizing(st.h, etch_sizing_grow(0, 0));
        }
        if (st.has_flex_grow)
            box.flex_grow = st.flex_grow;
        if (st.has_flex_shrink)
            box.flex_shrink = st.flex_shrink;
        if (st.has_margin)
            box.margin = st.margin;
        etch_box(&box);
    } else if (n.kind == INLAY_UI_BUTTON) {
        const EtchStr label = frame_text(s, n.text);
        const float border_w = st.has_border_width ? std::fmax(0.f, st.border_width) : 0.f;
        const bool rich_box_btn =
            border_w > 0.f || sides || st.has_elevation || st.shadow_count > 0u ||
            st.inset_shadow_bool || st.gradient_idx != none || st.backdrop_blur > 0.f ||
            st.has_top_rim_alpha || st.hover_bg.length || st.hover_border_color.length ||
            st.has_hover_scale || st.has_press_scale || st.has_continuous_corners;
        if (rich_box_btn) {
            EtchColor var_bg = theme.surface_control;
            EtchColor var_fg = theme.text_primary;
            EtchColor var_border = theme.border_control.a != 0u ? theme.border_control : theme.border_subtle;
            if (st.button_variant == ETCH_BUTTON_PRIMARY) {
                var_bg = theme.accent_primary.a != 0u ? theme.accent_primary : theme.accent;
                var_fg = theme.on_primary;
                var_border = etch_rgba8(255, 255, 255, 28);
            } else if (st.button_variant == ETCH_BUTTON_DANGER) {
                var_bg = theme.danger;
                var_fg = theme.on_error.a != 0u ? theme.on_error : theme.on_primary;
                var_border = etch_rgba8(255, 255, 255, 28);
            } else if (st.button_variant == ETCH_BUTTON_GHOST) {
                var_bg = etch_rgba8(0, 0, 0, 0);
                var_fg = theme.text_primary;
                var_border = etch_rgba8(0, 0, 0, 0);
            }
            const bool surface_btn = !st.disabled && st.button_variant != ETCH_BUTTON_GHOST;
            auto box = etch_box_desc_default();
            box.height = etch_sizing_fixed(32.f);
            box.padding = etch_padding_xy(14.f, 0.f);
            box.gap = 8.f;
            box.align_x = ETCH_ALIGN_X_CENTER;
            box.align_y = ETCH_ALIGN_Y_CENTER;
            box.background = st.disabled ? theme.surface_disabled : var_bg;
            box.border_color = st.disabled ? etch_rgba8(0, 0, 0, 0) : var_border;
            if (surface_btn && !sides && !st.has_border_width)
                box.border_width = 1.f;
            box.radius = etch_corners_all(theme.radius_control_px);
            box.focusable = !st.disabled;
            populate_box_desc(s, st, theme, id, box, use_layer);
            if (!st.disabled && !st.hover_bg.length) {
                const EtchColor tint = (st.button_variant == ETCH_BUTTON_PRIMARY ||
                                        st.button_variant == ETCH_BUTTON_DANGER)
                                           ? var_fg
                                           : theme.text_primary;
                box.hover_background = etch_color_blend_over(box.background, {tint.r, tint.g, tint.b, 26});
            }
            etch_begin_box(&box);
            EtchA11yOverrideDesc ov = {};
            ov.element_id = id;
            ov.role = ETCH_A11Y_BUTTON;
            ov.label = label;
            ov.add_states = st.disabled ? ETCH_A11Y_STATE_DISABLED : 0u;
            ov.actions_mask = 3u;
            ov.has_role = true;
            ov.has_label = true;
            ov.has_actions_mask = true;
            etch_set_a11y_override(&ov);
            auto text = etch_text_desc_default();
            text.font_size_px = st.has_size && st.size > 0 ? st.size : 13.f;
            if (st.has_font_id)
                text.font_id = st.font_id;
            text.color = st.disabled ? theme.text_disabled
                         : st.color.length ? resolve_color(st.color, theme, var_fg)
                                           : var_fg;
            text.wrap = false;
            etch_text(label, &text);
            etch_end_box();
            if (!st.disabled) {
                if (etch_hovered(id))
                    etch_set_cursor(ETCH_CURSOR_HAND);
                if (is_activated(id))
                    queue_event(s, events, st.action, st.handler, ETCH_STR_LIT("null"), true);
            }
        } else {
            auto button = etch_button_desc_default();
            button.id = id;
            button.label = label;
            button.variant = st.button_variant;
            button.disabled = st.disabled;
            if (st.w.explicit_set || st.w.min_px > 0.f || st.w.max_px > 0.f)
                button.width = resolve_sizing(st.w, button.width);
            if (st.h.explicit_set || st.h.min_px > 0.f || st.h.max_px > 0.f)
                button.height = resolve_sizing(st.h, button.height);
            if (st.has_padding)
                button.padding = st.padding;
            if (st.has_size && st.size > 0)
                button.font_size_px = st.size;
            if (st.has_font_id)
                button.font_id = st.font_id;
            if (st.radius_mode)
                button.radius = resolve_corners(st, theme, theme.radius_control_px);
            if (st.has_align_x)
                button.align_x = st.align_x;
            const bool custom_theme = st.background.length || st.color.length;
            if (custom_theme) {
                auto btn_theme = theme;
                if (st.background.length)
                    btn_theme.surface_control =
                        resolve_color(st.background, theme, theme.surface_control);
                if (st.color.length)
                    btn_theme.text_primary = resolve_color(st.color, theme, theme.text_primary);
                etch_push_theme(&btn_theme);
            }
            const bool pressed = etch_button(&button);
            if (custom_theme)
                etch_pop_theme();
            if (pressed)
                queue_event(s, events, st.action, st.handler, ETCH_STR_LIT("null"), true);
        }
    } else if (n.kind == INLAY_UI_INPUT) {
        auto input = etch_text_input_desc_default();
        input.id = id;
        input.placeholder = frame_text(s, st.placeholder);
        if (st.w.explicit_set || st.w.min_px > 0.f || st.w.max_px > 0.f)
            input.width = resolve_sizing(st.w, input.width);
        if (st.h.explicit_set || st.h.min_px > 0.f || st.h.max_px > 0.f)
            input.height = resolve_sizing(st.h, input.height);
        if (st.has_padding)
            input.padding = st.padding;
        input.font_size_px = st.size > 0 ? st.size : 15.f;
        if (st.has_font_id)
            input.font_id = st.font_id;
        if (st.background.length)
            input.background = resolve_color(st.background, theme, input.background);
        if (st.color.length)
            input.text_color = resolve_color(st.color, theme, input.text_color);
        if (st.border_color.length)
            input.border_color = resolve_color(st.border_color, theme, input.border_color);
        if (st.hover_border_color.length)
            input.hover_border_color = resolve_color(st.hover_border_color, theme, input.hover_border_color);
        if (st.has_border_width)
            input.border_width = std::fmax(0.f, st.border_width);
        if (st.radius_mode)
            input.radius = resolve_corners(st, theme, theme.radius_input_px);
        if (st.has_continuous_corners)
            input.continuous_corners = st.continuous_corners;
        input.disabled = st.disabled;
        uint32_t length = n.text.length;
        if (length >= input_capacity) {
            length = input_capacity - 1;
            while ((uint8_t(n.text.chars[length]) & 0xc0) == 0x80)
                --length;
        }
        if (length)
            std::memcpy(s->edit_buffer, n.text.chars, length);
        s->edit_buffer[length] = 0;
        input.buffer = s->edit_buffer;
        input.capacity = input_capacity;
        const auto edit = etch_text_input(&input);
        if (edit.changed && (st.action.length || st.handler != none))
            queue_event(s, events, st.action, st.handler,
                        input_json(s->etch, s->edit_buffer, edit.byte_length));
    } else if (n.kind == INLAY_UI_TOGGLE) {
        auto toggle = etch_toggle_switch_desc_default();
        bool checked = st.checked;
        toggle.id = id;
        toggle.label = frame_text(s, n.text);
        toggle.value = &checked;
        toggle.disabled = st.disabled;
        toggle.font_size_px = st.size;
        if (st.has_font_id)
            toggle.font_id = st.font_id;
        if (st.color.length)
            toggle.on_color = resolve_color(st.color, theme, theme.accent);
        if (etch_toggle_switch(&toggle))
            queue_event(s, events, st.action, st.handler,
                        checked ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
    } else if (n.kind == INLAY_UI_CHECKBOX) {
        auto cb = etch_checkbox_desc_default();
        bool checked = st.checked;
        cb.id = id;
        cb.label = frame_text(s, n.text);
        cb.checked = &checked;
        cb.indeterminate = st.indeterminate;
        cb.disabled = st.disabled;
        if (st.has_size && st.size > 0)
            cb.font_size_px = st.size;
        if (st.has_font_id)
            cb.font_id = st.font_id;
        if (st.color.length)
            cb.accent = resolve_color(st.color, theme, theme.accent);
        if (etch_checkbox(&cb))
            queue_event(s, events, st.action, st.handler,
                        checked ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
    } else if (n.kind == INLAY_UI_TABS) {
        auto track = etch_box_desc_default();
        track.axis = ETCH_AXIS_ROW;
        track.padding = etch_padding_all(3.f);
        track.gap = 2.f;
        track.align_y = ETCH_ALIGN_Y_CENTER;
        track.background = theme.surface_subtle;
        track.border_width = 1.f;
        track.border_color = theme.border_subtle;
        track.radius = etch_corners_all(theme.radius_control_px + 2.f);
        track.continuous_corners = true;
        populate_box_desc(s, st, theme, id, track, use_layer);
        track.axis = ETCH_AXIS_ROW;
        etch_begin_box(&track);
        EtchA11yOverrideDesc list_ov = {};
        list_ov.element_id = id;
        list_ov.role = ETCH_A11Y_TAB_LIST;
        list_ov.has_role = true;
        etch_set_a11y_override(&list_ov);

        const bool stretch_tabs = st.w.explicit_set && st.w.kind != ETCH_SIZING_FIT;
        const float font_px = (st.has_size && st.size > 0) ? st.size : 13.f;
        const EtchCorners pill_r = st.radius_mode
            ? resolve_corners(st, theme, theme.radius_control_px)
            : etch_corners_all(theme.radius_control_px);
        const bool cont_c = st.has_continuous_corners ? st.continuous_corners : true;
        for (uint32_t i = 0; i < st.item_count; ++i) {
            const uint32_t tab_id = etch_id_index(id, 0x7AB0u + i);
            const bool active = int32_t(i) == st.int_value;
            const EtchStr label = frame_text(s, s->item_pool[st.item_offset + i]);
            auto tab = etch_box_desc_default();
            tab.id = tab_id;
            tab.width = stretch_tabs ? etch_sizing_grow(0, 0) : etch_sizing_fit(0, 0);
            tab.height = etch_sizing_fit(0, 0);
            tab.padding = etch_padding_xy(12.f, 5.f);
            tab.align_x = ETCH_ALIGN_X_CENTER;
            tab.align_y = ETCH_ALIGN_Y_CENTER;
            tab.radius = pill_r;
            tab.continuous_corners = cont_c;
            tab.focusable = true;
            if (active) {
                tab.background = dark ? theme.surface_active : theme.bg_elevated;
                tab.border_width = 1.f;
                tab.border_color = dark ? theme.border_control : theme.border_subtle;
                if (!dark)
                    tab.elevation = ETCH_ELEVATION_SM;
                else
                    tab.top_rim_alpha = 0.12f;
            } else {
                tab.background = etch_color_transparent();
                tab.hover_background = dark ? etch_rgba8(255, 255, 255, 14) : etch_rgba8(15, 23, 42, 12);
            }
            etch_begin_box(&tab);
            EtchA11yOverrideDesc tab_ov = {};
            tab_ov.element_id = tab_id;
            tab_ov.role = ETCH_A11Y_TAB;
            tab_ov.label = label;
            tab_ov.add_states = active ? ETCH_A11Y_STATE_SELECTED : 0u;
            tab_ov.actions_mask = 3u;
            tab_ov.has_role = true;
            tab_ov.has_label = true;
            tab_ov.has_actions_mask = true;
            etch_set_a11y_override(&tab_ov);
            auto td = etch_text_desc_default();
            td.font_size_px = font_px;
            td.font_id = st.has_font_id ? st.font_id : static_cast<uint16_t>(active ? ETCH_FONT_SEMIBOLD : ETCH_FONT_SANS);
            td.color = active ? (st.color.length ? resolve_color(st.color, theme, theme.text_primary)
                                                 : theme.text_primary)
                              : theme.text_secondary;
            td.wrap = false;
            etch_text(label, &td);
            etch_end_box();
            if (etch_hovered(tab_id))
                etch_set_cursor(ETCH_CURSOR_HAND);
            if (is_activated(tab_id) && !active) {
                char buf[16] = {};
                const int len = std::snprintf(buf, sizeof(buf), "%u", i);
                if (len > 0)
                    queue_event(s, events, st.action, st.handler,
                                frame_text(s, {buf, uint32_t(len), false}));
            }
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_COMBOBOX) {
        auto cb = etch_combo_box_desc_default();
        cb.id = id;
        cb.label = frame_text(s, st.placeholder.length ? st.placeholder : n.text);
        int selected = st.int_value;
        cb.selected_index = &selected;
        if (st.item_count > 0u) {
            auto *items = static_cast<EtchStr *>(
                etch_frame_arena_alloc(s->etch, size_t(st.item_count) * sizeof(EtchStr), alignof(EtchStr)));
            if (items) {
                for (uint32_t i = 0; i < st.item_count; ++i)
                    items[i] = frame_text(s, s->item_pool[st.item_offset + i]);
                cb.items = items;
                cb.item_count = st.item_count;
            }
        }
        if (st.w.explicit_set || st.w.min_px > 0.f || st.w.max_px > 0.f)
            cb.width = resolve_sizing(st.w, cb.width);
        if (st.h.explicit_set && st.h.kind == ETCH_SIZING_FIXED && st.h.value > 0.f)
            cb.height_px = st.h.value;
        cb.disabled = st.disabled;
        if (st.background.length)
            cb.background = resolve_color(st.background, theme, cb.background);
        if (st.hover_bg.length)
            cb.hover_bg = resolve_color(st.hover_bg, theme, cb.hover_bg);
        if (st.color.length)
            cb.text_color = resolve_color(st.color, theme, cb.text_color);
        if (st.border_color.length)
            cb.border_color = resolve_color(st.border_color, theme, cb.border_color);
        if (st.has_border_width)
            cb.border_width = std::fmax(0.f, st.border_width);
        if (cb.item_count > 0u && etch_combo_box(&cb)) {
            char buf[16] = {};
            const int len = std::snprintf(buf, sizeof(buf), "%d", selected);
            if (len > 0)
                queue_event(s, events, st.action, st.handler,
                            frame_text(s, {buf, uint32_t(len), false}));
        }
    } else if (n.kind == INLAY_UI_MODAL) {
        auto m = etch_modal_desc_default();
        bool open = true;
        m.id = id;
        m.open = &open;
        m.title = frame_text(s, n.text);
        if (st.w.explicit_set && st.w.kind == ETCH_SIZING_FIXED && st.w.value > 0.f)
            m.width_px = st.w.value;
        if (st.h.explicit_set || st.h.min_px > 0.f || st.h.max_px > 0.f)
            m.height = resolve_sizing(st.h, m.height);
        if (st.background.length)
            m.background = resolve_color(st.background, theme, m.background);
        if (st.border_color.length)
            m.border_color = resolve_color(st.border_color, theme, m.border_color);
        if (st.radius_mode)
            m.radius = resolve_corners(st, theme, theme.radius_panel_px);
        if (st.has_z_index)
            m.z_index = st.z_index;
        if (etch_begin_modal(&m)) {
            for (uint32_t at = n.first_child; at != none; at = s->nodes[at].next_sibling)
                draw_node(s, at, id, theme, events);
            etch_end_modal();
        } else if (!open) {
            queue_event(s, events, st.action, st.handler, ETCH_STR_LIT("false"));
        }
    } else if (n.kind == INLAY_UI_TOOLTIP) {
        auto box = etch_box_desc_default();
        box.id = id;
        box.axis = st.column ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        box.width = resolve_sizing(st.w, etch_sizing_fit(0, 0));
        box.height = resolve_sizing(st.h, etch_sizing_fit(0, 0));
        if (st.has_gap)
            box.gap = st.gap;
        if (st.has_margin)
            box.margin = st.margin;
        if (st.has_align_self)
            box.align_self = st.align_self;
        etch_begin_box(&box);
        for (uint32_t at = n.first_child; at != none; at = s->nodes[at].next_sibling)
            draw_node(s, at, id, theme, events);
        etch_end_box();
        if (n.text.length)
            etch_set_tooltip_text(id, frame_text(s, n.text));
    } else if (n.kind == INLAY_UI_SPLIT) {
        auto sp = etch_split_desc_default();
        sp.id = id;
        sp.axis = st.column ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        sp.width = resolve_sizing(st.w, etch_sizing_grow(0, 0));
        sp.height = resolve_sizing(st.h, etch_sizing_grow(0, 0));
        float ratio = st.has_value ? st.value : 0.5f;
        const float min_r = st.has_min ? std::fmin(0.95f, std::fmax(0.02f, st.min_val)) : 0.0f;
        const float max_r = st.has_max ? std::fmin(0.98f, std::fmax(0.05f, st.max_val)) : 1.0f;
        if (st.has_min && ratio < min_r) ratio = min_r;
        if (st.has_max && ratio > max_r) ratio = max_r;
        sp.default_ratio = std::fmin(0.98f, std::fmax(0.02f, ratio));
        if (st.has_min || st.has_max) {
            const auto prev_b = etch_element_bounds(id);
            const float span = (sp.axis == ETCH_AXIS_ROW)
                ? (prev_b.w > 1.f ? prev_b.w : (st.w.kind == ETCH_SIZING_FIXED ? st.w.value : 0.f))
                : (prev_b.h > 1.f ? prev_b.h : (st.h.kind == ETCH_SIZING_FIXED ? st.h.value : 0.f));
            const float avail = span - sp.divider_size_px;
            if (avail > 1.f) {
                if (st.has_min) sp.min_first_px = std::fmax(0.f, min_r * avail);
                if (st.has_max) sp.min_second_px = std::fmax(0.f, (1.f - max_r) * avail);
            } else {
                if (st.has_min) sp.min_first_px = 0.f;
                if (st.has_max) sp.min_second_px = 0.f;
            }
        }
        etch_begin_split(&sp);
        const uint32_t first = n.first_child;
        if (first != none)
            draw_node(s, first, id, theme, events);
        etch_split_second();
        const uint32_t second = first != none ? s->nodes[first].next_sibling : none;
        if (second != none)
            draw_node(s, second, id, theme, events);
        etch_end_split();
    } else if (n.kind == INLAY_UI_MATH) {
        const EtchStr copied = frame_text(s, n.text);
        const float font_px = std::fmin(256.f, std::fmax(6.f, st.has_size && st.size > 0 ? st.size : 18.f));
        const EtchColor fg = st.color.length ? resolve_color(st.color, theme, theme.text_primary)
                                             : theme.text_primary;
        InlayMathPrepared prep = {};
        const bool ok = copied.length > 0u && inlay_math_prepare(copied, font_px, st.display_math, &prep);
        auto box = etch_box_desc_default();
        if (ok) {
            const float pad_w = st.has_padding ? (st.padding.left + st.padding.right) : 0.f;
            const float pad_h = st.has_padding ? (st.padding.top + st.padding.bottom) : 0.f;
            box.width = etch_sizing_fixed(std::fmax(1.f, prep.metrics.width + pad_w));
            box.height = etch_sizing_fixed(std::fmax(1.f, prep.metrics.height + pad_h));
        }
        populate_box_desc(s, st, theme, id, box, use_layer);
        etch_begin_box(&box);
        if (ok) {
            auto *payload = static_cast<UiMathDraw *>(
                etch_frame_arena_alloc(s->etch, sizeof(UiMathDraw), alignof(UiMathDraw)));
            if (payload) {
                *payload = {prep, fg, box.padding, st.align_x, st.align_y,
                            st.has_align_x, st.has_align_y};
                etch_draw_after_layout(draw_ui_math, payload, false);
            }
        } else if (copied.length) {
            auto td = etch_text_desc_default();
            td.font_size_px = font_px;
            td.color = fg;
            etch_text(copied, &td);
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_DIAGRAM) {
        const EtchStr copied = frame_text(s, n.text);
        auto ddesc = inlay_diagram_desc_default();
        ddesc.font_size_px =
            std::fmin(64.f, std::fmax(8.f, st.has_size && st.size > 0 ? st.size : 14.f));
        const float pad_w = st.has_padding ? (st.padding.left + st.padding.right) : 0.f;
        const float pad_h = st.has_padding ? (st.padding.top + st.padding.bottom) : 0.f;
        if (st.w.explicit_set && st.w.kind == ETCH_SIZING_FIXED && st.w.value > pad_w) {
            ddesc.max_width_px = st.w.value - pad_w;
        } else if (st.w.max_px > pad_w) {
            ddesc.max_width_px = st.w.max_px - pad_w;
        } else if (st.w.explicit_set && st.w.kind == ETCH_SIZING_GROW) {
            const auto prev_b = etch_element_bounds(id);
            if (prev_b.w > pad_w + 16.f)
                ddesc.max_width_px = prev_b.w - pad_w;
        }
        InlayDiagramMeta meta = {};
        const auto res = copied.length > 0u ? inlay_diagram_layout_ex(copied, &ddesc, &meta)
                                            : InlayDiagramResult{};
        auto box = etch_box_desc_default();
        if (res.valid) {
            box.width = etch_sizing_fixed(std::fmax(1.f, res.width + pad_w));
            box.height = etch_sizing_fixed(std::fmax(1.f, res.height + pad_h));
        }
        if (st.w.explicit_set && st.w.kind == ETCH_SIZING_GROW)
            box.clip_children = true;
        populate_box_desc(s, st, theme, id, box, use_layer);
        etch_begin_box(&box);
        if (res.valid) {
            auto dbox = etch_box_desc_default();
            dbox.width = etch_sizing_fixed(std::fmax(1.f, res.width));
            dbox.height = etch_sizing_fixed(std::fmax(1.f, res.height));
            etch_begin_box(&dbox);
            auto *payload = static_cast<UiDiagramDraw *>(
                etch_frame_arena_alloc(s->etch, sizeof(UiDiagramDraw), alignof(UiDiagramDraw)));
            if (payload) {
                *payload = {res, meta, ddesc};
                etch_draw_after_layout(draw_ui_diagram, payload, false);
            }
            etch_end_box();
        } else if (copied.length) {
            auto td = etch_text_desc_default();
            td.font_size_px = ddesc.font_size_px;
            td.color = theme.text_muted;
            etch_text(copied, &td);
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_SLOT) {
        auto box = etch_box_desc_default();
        populate_box_desc(s, st, theme, id, box, use_layer);
        const auto prev_b = etch_element_bounds(id);
        float eff_w = (box.width.kind == ETCH_SIZING_FIXED && box.width.value > 0.f)
                          ? box.width.value
                          : prev_b.w;
        float eff_h = (box.height.kind == ETCH_SIZING_FIXED && box.height.value > 0.f)
                          ? box.height.value
                          : prev_b.h;
        if (eff_w > 0.f)
            eff_w = std::fmax(0.f, eff_w - box.padding.left - box.padding.right);
        if (eff_h > 0.f)
            eff_h = std::fmax(0.f, eff_h - box.padding.top - box.padding.bottom);
        SlotBinding *binding = nullptr;
        for (uint32_t i = 0; i < s->slot_count; ++i) {
            if (equal(n.text, s->slots[i].name)) {
                binding = &s->slots[i];
                break;
            }
        }
        etch_begin_box(&box);
        if (binding) {
            binding->callback(s, id, eff_w, eff_h, frame_text(s, st.placeholder), binding->user);
        } else {
            const uint32_t cap = n.text.length + 12u;
            auto *buf = static_cast<char *>(etch_frame_arena_alloc(s->etch, cap, 1));
            if (buf) {
                const int len = std::snprintf(buf, cap, "[slot: %.*s]", int(n.text.length),
                                              n.text.chars ? n.text.chars : "");
                if (len > 0) {
                    auto td = etch_text_desc_default();
                    td.font_size_px = st.has_size && st.size > 0 ? st.size : 12.f;
                    td.color = theme.text_muted;
                    td.wrap = false;
                    etch_text({buf, uint32_t(len), false}, &td);
                }
            }
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_PROGRESS) {
        auto pb = etch_progress_bar_desc_default();
        pb.id = id;
        const float min_v = st.has_min ? st.min_val : 0.f;
        const float max_v = st.has_max ? st.max_val : 1.f;
        const float denom = max_v - min_v;
        const float raw = st.has_value ? st.value : 0.f;
        pb.fraction = denom > 0.f ? std::fmin(1.f, std::fmax(0.f, (raw - min_v) / denom)) : 0.f;
        pb.overlay_text = frame_text(s, n.text);
        if (st.w.explicit_set || st.w.min_px > 0.f || st.w.max_px > 0.f)
            pb.width = resolve_sizing(st.w, pb.width);
        if (st.h.explicit_set || st.h.min_px > 0.f || st.h.max_px > 0.f)
            pb.height = resolve_sizing(st.h, pb.height);
        if (st.radius_mode)
            pb.radius = resolve_corners(st, theme, theme.radius_pill_px);
        if (st.track_color.length)
            pb.track_color = resolve_color(st.track_color, theme, pb.track_color);
        else if (st.background.length)
            pb.track_color = resolve_color(st.background, theme, pb.track_color);
        if (st.color.length)
            pb.fill_color = resolve_color(st.color, theme, pb.fill_color);
        if (st.has_size && st.size > 0)
            pb.font_size_px = st.size;
        etch_progress_bar(&pb);
    } else if (n.kind == INLAY_UI_SLIDER) {
        auto sl = etch_slider_desc_default();
        float val = st.has_value ? st.value : 0.f;
        sl.id = id;
        sl.label = frame_text(s, n.text);
        sl.value = &val;
        sl.min_value = st.has_min ? st.min_val : 0.f;
        sl.max_value = st.has_max ? st.max_val : 1.f;
        if (sl.max_value <= sl.min_value)
            sl.max_value = sl.min_value + 1.f;
        sl.step = st.has_step && st.step >= 0.f ? st.step : 0.f;
        sl.disabled = st.disabled;
        if (st.w.explicit_set || st.w.min_px > 0.f || st.w.max_px > 0.f)
            sl.width = resolve_sizing(st.w, sl.width);
        if (st.h.explicit_set && st.h.kind == ETCH_SIZING_FIXED && st.h.value > 0.f)
            sl.height_px = st.h.value;
        if (st.background.length)
            sl.background = resolve_color(st.background, theme, sl.background);
        if (st.track_color.length)
            sl.track_color = resolve_color(st.track_color, theme, sl.track_color);
        if (st.color.length)
            sl.fill_color = resolve_color(st.color, theme, sl.fill_color);
        if (etch_slider(&sl) && (st.action.length || st.handler != none)) {
            char buf[32] = {};
            const int len = std::snprintf(buf, sizeof(buf), "%.9g", double(val));
            if (len > 0)
                queue_event(s, events, st.action, st.handler,
                            frame_text(s, {buf, uint32_t(len), false}));
        }
    } else if (n.kind == INLAY_UI_BADGE) {
        auto box = etch_box_desc_default();
        box.padding = etch_padding_xy(8.f, 3.f);
        box.align_x = ETCH_ALIGN_X_CENTER;
        box.align_y = ETCH_ALIGN_Y_CENTER;
        box.background = theme.surface_subtle;
        box.border_width = st.border_color.length ? 1.f : 0.f;
        box.radius = etch_corners_all(theme.radius_pill_px);
        populate_box_desc(s, st, theme, id, box, use_layer);
        etch_begin_box(&box);
        auto text = etch_text_desc_default();
        text.font_size_px = st.has_size && st.size > 0 ? st.size : 12.f;
        if (st.has_font_id)
            text.font_id = st.font_id;
        text.color = st.color.length ? resolve_color(st.color, theme, theme.text_primary) : theme.text_primary;
        text.wrap = false;
        etch_text(frame_text(s, n.text), &text);
        etch_end_box();
    } else if (n.kind == INLAY_UI_DIVIDER) {
        auto box = etch_box_desc_default();
        box.id = id;
        if (!st.column) {
            box.width = resolve_sizing(st.w, etch_sizing_grow(0, 0));
            box.height = resolve_sizing(st.h, etch_sizing_fixed(1.f));
        } else {
            box.width = resolve_sizing(st.w, etch_sizing_fixed(1.f));
            box.height = resolve_sizing(st.h, etch_sizing_grow(0, 0));
        }
        if (st.has_margin)
            box.margin = st.margin;
        box.background = st.color.length        ? resolve_color(st.color, theme, theme.border_line)
                         : st.background.length ? resolve_color(st.background, theme, theme.border_line)
                                                : theme.border_line;
        if (st.has_opacity && !use_layer)
            box.opacity = st.opacity;
        etch_box(&box);
    } else if (n.kind == INLAY_UI_IMAGE) {
        if (st.has_sprite && st.sprite_id != ETCH_INVALID_SPRITE) {
            auto img = etch_image_desc_default();
            img.id = id;
            img.sprite_id = st.sprite_id;
            img.width = resolve_sizing(st.w, etch_sizing_fit(0, 0));
            img.height = resolve_sizing(st.h, etch_sizing_fit(0, 0));
            if (st.radius_mode)
                img.radius = resolve_corners(st, theme, 0.f);
            if (st.color.length)
                img.tint = resolve_color(st.color, theme, etch_color_white(255));
            if (st.border_color.length)
                img.border_color = resolve_color(st.border_color, theme, {});
            if (st.has_border_width)
                img.border_width = std::fmax(0.f, st.border_width);
            if (st.has_aspect_ratio)
                img.aspect_ratio = st.aspect_ratio;
            if (st.col_span > 1)
                img.grid_col_span = st.col_span;
            if (st.has_continuous_corners)
                img.continuous_corners = st.continuous_corners;
            img.fit = st.image_fit;
            etch_image(&img);
        } else if (st.has_icon && st.icon_id != ETCH_INVALID_ICON) {
            auto ic = etch_icon_desc_default();
            ic.id = id;
            ic.icon_id = st.icon_id;
            ic.size_px = (st.w.explicit_set && st.w.kind == ETCH_SIZING_FIXED && st.w.value > 0.f)
                             ? st.w.value
                             : (st.has_size && st.size > 0 ? st.size : 24.f);
            ic.color = st.color.length ? resolve_color(st.color, theme, theme.text_primary)
                                       : theme.text_primary;
            etch_icon(&ic);
        } else {
            auto box = etch_box_desc_default();
            box.width = etch_sizing_fixed(64.f);
            box.height = etch_sizing_fixed(64.f);
            box.align_x = ETCH_ALIGN_X_CENTER;
            box.align_y = ETCH_ALIGN_Y_CENTER;
            box.background = theme.surface_subtle;
            box.border_width = 1.f;
            box.border_color = theme.border_subtle;
            box.radius = etch_corners_all(theme.radius_small_px);
            populate_box_desc(s, st, theme, id, box, use_layer);
            etch_begin_box(&box);
            const EtchStr label = n.text.length ? n.text : st.src;
            if (label.length) {
                auto text = etch_text_desc_default();
                text.font_size_px = st.has_size && st.size > 0 ? st.size : 12.f;
                text.color = st.color.length ? resolve_color(st.color, theme, theme.text_muted)
                                             : theme.text_muted;
                text.wrap = false;
                etch_text(frame_text(s, label), &text);
            }
            etch_end_box();
        }
    } else {
        auto text = etch_text_desc_default();
        text.id = id;
        text.font_size_px = st.size;
        text.color = resolve_color(st.color, theme, theme.text_primary);
        if (st.has_font_id)
            text.font_id = st.font_id;
        if (st.has_wrap)
            text.wrap = st.wrap_flag;
        if (st.has_line_height)
            text.line_height_px = st.line_height;
        if (st.has_letter_spacing)
            text.letter_spacing = st.letter_spacing;
        if (st.has_align_x)
            text.align_x = st.align_x;
        if (st.ellipsis)
            text.ellipsis = true;
        if (st.max_lines > 0)
            text.max_lines = st.max_lines;
        etch_text(frame_text(s, n.text), &text);
    }

    if (use_layer)
        etch_pop_layer();
}
} // namespace

extern "C" {
INLAY_API InlaySurface *inlay_surface_create(EtchContext *context, const InlaySurfaceDesc *desc) {
    InlaySurfaceDesc d = inlay_surface_desc_default();
    if (desc && !inlay_read_abi_struct(desc, &d))
        return nullptr;
    if (!context || d.memory_limit_bytes < 512u * 1024u ||
        d.memory_limit_bytes > 256u * 1024u * 1024u || d.max_stack_bytes < 16384 ||
        d.max_stack_bytes > 512u * 1024u || !std::isfinite(d.max_tick_ms) || d.max_tick_ms <= 0 ||
        d.max_tick_ms > 50 || !d.max_nodes || d.max_nodes > 65536 || !d.max_text_bytes ||
        d.max_text_bytes > 1024u * 1024u)
        return nullptr;
    auto *s = static_cast<InlaySurface *>(std::calloc(1, sizeof(InlaySurface)));
    if (!s)
        return nullptr;
    s->etch = context;
    s->desc = d;
    s->frequency = double(SDL_GetPerformanceFrequency());
    for (auto &timer : s->timers)
        timer.function = JS_UNDEFINED;
    const uint32_t init_nodes = d.max_nodes < 256u ? d.max_nodes : 256u;
    s->nodes = static_cast<InlayUiNode *>(std::calloc(init_nodes, sizeof(InlayUiNode)));
    s->styles = static_cast<Style *>(std::calloc(init_nodes, sizeof(Style)));
    s->node_capacity = init_nodes;
    s->text = static_cast<char *>(std::malloc(d.max_text_bytes));
    s->json_scratch = static_cast<char *>(std::malloc(65537));
    s->runtime = JS_NewRuntime();
    if (!s->nodes || !s->styles || !s->text || !s->json_scratch || !s->runtime ||
        !save_state(s, "{}", 2)) {
        inlay_surface_destroy(s);
        return nullptr;
    }
    JS_SetMemoryLimit(s->runtime, d.memory_limit_bytes);
    JS_SetMaxStackSize(s->runtime, d.max_stack_bytes);
    JS_SetInterruptHandler(s->runtime, interrupt, s);
    JS_SetHostPromiseRejectionTracker(s->runtime, rejection, s);
    s->js = JS_NewContext(s->runtime);
    if (!s->js) {
        inlay_surface_destroy(s);
        return nullptr;
    }
    JS_SetContextOpaque(s->js, s);
    return s;
}

INLAY_API void inlay_surface_destroy(InlaySurface *s) {
    if (s) {
        clear_context(s);
        if (s->runtime)
            JS_FreeRuntime(s->runtime);
        std::free(s->handlers);
        std::free(s->nodes);
        std::free(s->styles);
        std::free(s->shadow_pool);
        std::free(s->gradient_pool);
        std::free(s->transform_pool);
        std::free(s->item_pool);
        std::free(s->text);
        std::free(s->json_scratch);
        std::free(s->source);
        std::free(s->state);
        std::free(s);
    }
}

INLAY_API bool inlay_surface_set_script(InlaySurface *s, EtchStr script) {
    if (!s || s->running || s->drawing || (!script.chars && script.length) || script.length > 1024u * 1024u)
        return false;
    return load(s, script);
}

INLAY_API bool inlay_surface_set_state_json(InlaySurface *s, EtchStr json) {
    if (!s || !json.chars || !json.length || json.length > 65536)
        return false;
    // Parse JSON as data; installing it into script globals belongs to a bounded tick.
    if (!s->js)
        return false;
    const auto parsed = parse_json(s, json);
    if (JS_IsException(parsed)) {
        const auto exception = JS_GetException(s->js);
        JS_FreeValue(s->js, exception);
        error(s, "Invalid state JSON");
        return false;
    }
    JS_FreeValue(s->js, parsed);
    if (!save_state(s, json.chars, json.length))
        return false;
    s->pending_state = true;
    s->state_dirty = false;
    etch_request_redraw(s->etch);
    return s->running || s->drawing || !s->loaded ? true : run_update(s, {}, {});
}

INLAY_API EtchStr inlay_surface_state_json(const InlaySurface *s) {
    if (!s)
        return {};
    if (s->state_dirty)
        sync_state_json(const_cast<InlaySurface *>(s));
    return EtchStr{s->state, s->state_size, false};
}

INLAY_API bool inlay_surface_bind_action(InlaySurface *s, EtchStr name, InlayActionFn fn, void *user) {
    if (!s || !name.chars || !name.length || name.length >= 64 || !fn ||
        std::memchr(name.chars, 0, name.length))
        return false;
    for (uint32_t i = 0; i < s->action_count; ++i)
        if (equal(name, s->actions[i].name)) {
            s->actions[i].callback = fn;
            s->actions[i].user = user;
            return true;
        }
    if (s->action_count == 32)
        return false;
    auto &a = s->actions[s->action_count++];
    std::memcpy(a.name, name.chars, name.length);
    a.name[name.length] = 0;
    a.callback = fn;
    a.user = user;
    return true;
}

INLAY_API bool inlay_surface_bind_slot(InlaySurface *s, EtchStr name, InlaySlotRenderFn fn, void *user) {
    if (!s || !name.chars || !name.length || name.length >= 64 || !fn ||
        std::memchr(name.chars, 0, name.length))
        return false;
    for (uint32_t i = 0; i < s->slot_count; ++i)
        if (equal(name, s->slots[i].name)) {
            s->slots[i].callback = fn;
            s->slots[i].user = user;
            etch_request_redraw(s->etch);
            return true;
        }
    if (s->slot_count == 32)
        return false;
    auto &sl = s->slots[s->slot_count++];
    std::memcpy(sl.name, name.chars, name.length);
    sl.name[name.length] = 0;
    sl.callback = fn;
    sl.user = user;
    etch_request_redraw(s->etch);
    return true;
}

INLAY_API bool inlay_surface_dispatch(InlaySurface *s, EtchStr name, EtchStr payload) {
    if (!s || !s->js || !s->loaded || s->running || s->drawing || !name.chars || !name.length ||
        name.length >= 64 || !payload.chars || !payload.length || payload.length > 65536)
        return false;
    return run_update(s, name, payload);
}

INLAY_API bool inlay_surface_enable_hmr(InlaySurface *s, const char *path) {
    if (!s || s->running || s->drawing || !path || std::strlen(path) >= sizeof(s->path))
        return false;
    std::memcpy(s->path, path, std::strlen(path) + 1);
    s->modified = -1;
    return inlay_surface_poll_hmr(s);
}

INLAY_API bool inlay_surface_poll_hmr(InlaySurface *s) {
    if (!s || !s->path[0] || s->running || s->drawing)
        return false;
    SDL_PathInfo info = {};
    if (!SDL_GetPathInfo(s->path, &info)) {
        s->hmr_file_error = true;
        error(s, "HMR file is unavailable");
        return false;
    }
    if (info.modify_time == s->modified && info.size == s->file_size && !s->hmr_file_error)
        return true;
    const uint64_t start = SDL_GetPerformanceCounter();
    size_t length = 0;
    char *script =
        info.size <= 1024u * 1024u ? static_cast<char *>(SDL_LoadFile(s->path, &length)) : nullptr;
    if (script && length > 1024u * 1024u) {
        SDL_free(script);
        script = nullptr;
    }
    if (!script) {
        s->hmr_file_error = true;
        error(s, "HMR source could not be read");
        return false;
    }
    s->hmr_file_error = false;
    const uint64_t io_done = SDL_GetPerformanceCounter();
    const bool ok = load(s, {script, uint32_t(length), false}, true);
    const uint64_t eval_done = SDL_GetPerformanceCounter();
    SDL_free(script);
    s->modified = info.modify_time;
    s->file_size = info.size;
    ++s->stats.reloads;
    s->stats.last_reload_ms = double(eval_done - io_done) * 1000 / s->frequency;
    s->stats.last_reload_total_ms =
        double(SDL_GetPerformanceCounter() - start) * 1000 / s->frequency;
    return ok;
}

INLAY_API void inlay_surface_view(InlaySurface *s, const InlaySurfaceViewDesc *input_in) {
    InlaySurfaceViewDesc input = inlay_surface_view_desc_default();
    if (!s || !inlay_read_abi_struct(input_in, &input) || !input.id || !std::isfinite(input.width_px) ||
        !std::isfinite(input.height_px) || input.width_px <= 0 || input.height_px <= 0 ||
        etch_current_context() != s->etch || !etch_frame_in_progress(s->etch) || s->running || s->drawing)
        return;
    if (s->path[0])
        inlay_surface_poll_hmr(s);
    if (s->pending_state && s->loaded && !s->stats.has_error)
        run_update(s, {}, {});
    bool timer_due = false;
    const double current = now_ms(s);
    for (const auto &timer : s->timers)
        timer_due |= !JS_IsUndefined(timer.function) && timer.due <= current;
    if (timer_due && s->loaded && !s->stats.has_error) {
        const uint64_t start = SDL_GetPerformanceCounter();
        begin_tick(s);
        for (auto &timer : s->timers)
            if (!JS_IsUndefined(timer.function) && timer.due <= current &&
                !JS_HasException(s->js)) {
                const auto function = timer.function;
                timer.function = JS_UNDEFINED;
                timer.id = 0;
                const auto result = JS_Call(s->js, function, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(s->js, result);
                JS_FreeValue(s->js, function);
            }
        if (!JS_HasException(s->js))
            rebuild(s, false);
        finish_tick(s, start);
        etch_request_redraw(s->etch);
    }
    if (inlay_surface_next_timer_ms(s) == 0)
        etch_request_redraw(s->etch);
    const auto theme = etch_get_theme(s->etch);
    auto box = etch_box_desc_default();
    box.id = input.id;
    box.axis = ETCH_AXIS_COL;
    box.width = etch_sizing_fixed(input.width_px);
    box.height = etch_sizing_fixed(input.height_px);
    box.clip_children = true;
    etch_begin_box(&box);
    WidgetEvents events = {};
    if (s->stats.has_error) {
        auto card = etch_box_desc_default();
        card.padding = etch_padding_all(12);
        card.background = theme.bg_elevated;
        card.border_color = theme.danger;
        card.border_width = 1;
        card.radius = etch_corners_all(theme.radius_panel_px);
        etch_begin_box(&card);
        auto text = etch_text_desc_default();
        text.color = theme.danger;
        etch_text(frame_text(s, {s->error, uint32_t(std::strlen(s->error)), false}), &text);
        etch_end_box();
    } else if (s->node_count) {
        s->drawing = true;
        draw_node(s, 0, input.id, theme, &events);
        s->drawing = false;
    }
    etch_end_box();
    if ((events.first || s->pending_state) && s->js && s->loaded && !s->running)
        run_batch(s, events.first);
}

INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface *s) {
    if (!s || !s->loaded || s->stats.has_error)
        return -1;
    bool has = false;
    double soonest = 0.0;
    for (const auto &timer : s->timers) {
        if (JS_IsUndefined(timer.function))
            continue;
        if (!has || timer.due < soonest) {
            soonest = timer.due;
            has = true;
        }
    }
    if (!has)
        return -1;
    const double rem = soonest - now_ms(s);
    if (rem <= 0.0)
        return 0;
    const double ceiled = std::ceil(rem);
    return ceiled >= double(INT32_MAX) ? INT32_MAX : int32_t(ceiled);
}

INLAY_API InlaySurfaceStats inlay_surface_stats(const InlaySurface *s) {
    return s ? s->stats : InlaySurfaceStats{};
}

INLAY_API EtchStr inlay_surface_error(const InlaySurface *s) {
    return s ? EtchStr{s->error, uint32_t(std::strlen(s->error)), false} : EtchStr{};
}

INLAY_API const InlayUiNode *inlay_surface_nodes(const InlaySurface *s, uint32_t *count) {
    if (count)
        *count = s ? s->node_count : 0;
    return s ? s->nodes : nullptr;
}
}
#else
extern "C" {
INLAY_API InlaySurface *inlay_surface_create(EtchContext *, const InlaySurfaceDesc *) { return nullptr; }
INLAY_API void inlay_surface_destroy(InlaySurface *) {}
INLAY_API bool inlay_surface_set_script(InlaySurface *, EtchStr) { return false; }
INLAY_API bool inlay_surface_set_state_json(InlaySurface *, EtchStr) { return false; }
INLAY_API EtchStr inlay_surface_state_json(const InlaySurface *) { return {}; }
INLAY_API bool inlay_surface_bind_action(InlaySurface *, EtchStr, InlayActionFn, void *) { return false; }
INLAY_API bool inlay_surface_bind_slot(InlaySurface *, EtchStr, InlaySlotRenderFn, void *) { return false; }
INLAY_API bool inlay_surface_dispatch(InlaySurface *, EtchStr, EtchStr) { return false; }
INLAY_API bool inlay_surface_enable_hmr(InlaySurface *, const char *) { return false; }
INLAY_API bool inlay_surface_poll_hmr(InlaySurface *) { return false; }
INLAY_API void inlay_surface_view(InlaySurface *, const InlaySurfaceViewDesc *) {}
INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface *) { return -1; }
INLAY_API InlaySurfaceStats inlay_surface_stats(const InlaySurface *) { return {}; }
INLAY_API EtchStr inlay_surface_error(const InlaySurface *) { return {}; }
INLAY_API const InlayUiNode *inlay_surface_nodes(const InlaySurface *, uint32_t *count) {
    if (count)
        *count = 0;
    return nullptr;
}
}
#endif
