#include <inlay/inlay_c.h>
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
struct Style {
    float width, height, gap, padding, size, border_width;
    float opacity, value, min_val, max_val, step;
    EtchPadding border_sides;
    EtchAlignX align_x;
    EtchAlignY align_y;
    EtchStr background, color, border_color, track_color, radius, action, placeholder, src;
    uint32_t sprite_id, handler;
    uint16_t icon_id;
    bool column, animate, checked, scroll_x, scroll_y;
    bool has_padding, has_size, has_border_width, has_align_x, has_align_y;
    bool has_opacity, has_value, has_min, has_max, has_step, has_scroll_x, has_scroll_y;
    bool has_sprite, has_icon;
};
struct Action {
    char name[64];
    InlayActionFn callback;
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
    char *text;
    uint32_t node_count, text_used;
    char *state;
    char *json_scratch;
    char edit_buffer[input_capacity];
    char *source;
    uint32_t source_capacity, state_size, state_capacity;
    char error[1024], path[1024];
    Action actions[32];
    uint32_t action_count;
    JSValue *handlers;
    uint32_t handler_count, handler_capacity;
    Timer timers[32];
    uint32_t next_timer;
    int64_t modified;
    uint64_t file_size, deadline;
    double frequency;
    bool running, timed_out, pending_state, state_dirty, loaded, hmr_file_error;
    JSValue rejections[32];
    uint32_t rejection_count;
    InlaySurfaceStats stats;
};
namespace {
double now_ms(const InlaySurface *s) {
    return double(SDL_GetPerformanceCounter()) * 1000 / s->frequency;
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
uint32_t fnv1a_bytes(uint32_t seed, const char *chars, size_t length) {
    uint32_t h = seed;
    for (size_t i = 0; i < length; ++i)
        h = (h ^ uint8_t(chars[i])) * 16777619u;
    return h ? h : 1u;
}
uint32_t hash_slot_id(uint32_t slot, InlayUiKind kind) {
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
    const auto result = JS_IsUndefined(v) ? EtchStr{} : copy_text(s, v);
    JS_FreeValue(s->js, v);
    return result;
}
bool opt_number_prop(InlaySurface *s, JSValueConst object, const char *name, float *out, bool unbounded = false) {
    const auto v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    double value = *out;
    if (JS_ToFloat64(s->js, &value, v) < 0)
        value = *out;
    JS_FreeValue(s->js, v);
    const float narrowed = float(value);
    if (!std::isfinite(value) || !std::isfinite(narrowed) || (!unbounded && (value < -1 || value > 16384))) {
        JS_ThrowTypeError(s->js, "Invalid UI numeric property: %s", name);
        return false;
    }
    *out = narrowed;
    return true;
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
bool equal(EtchStr value, const char *text) {
    return value.length == std::strlen(text) && !std::memcmp(value.chars, text, value.length);
}
bool parse_align_x(EtchStr value, EtchAlignX *out) {
    if (equal(value, "left") || equal(value, "start")) {
        *out = ETCH_ALIGN_X_LEFT;
        return true;
    }
    if (equal(value, "center")) {
        *out = ETCH_ALIGN_X_CENTER;
        return true;
    }
    if (equal(value, "right") || equal(value, "end")) {
        *out = ETCH_ALIGN_X_RIGHT;
        return true;
    }
    return false;
}
bool parse_align_y(EtchStr value, EtchAlignY *out) {
    if (equal(value, "top") || equal(value, "start")) {
        *out = ETCH_ALIGN_Y_TOP;
        return true;
    }
    if (equal(value, "center")) {
        *out = ETCH_ALIGN_Y_CENTER;
        return true;
    }
    if (equal(value, "bottom") || equal(value, "end")) {
        *out = ETCH_ALIGN_Y_BOTTOM;
        return true;
    }
    return false;
}
void parse_style(InlaySurface *s, JSValueConst props, InlayUiKind kind, Style &style) {
    style.width = number_prop(s, props, "width", 0);
    style.height = number_prop(s, props, "height", 0);
    style.gap = number_prop(s, props, "gap", 0);
    style.has_padding = opt_number_prop(s, props, "padding", &style.padding);
    style.size = 15;
    style.has_size = opt_number_prop(s, props, "fontSize", &style.size);
    style.opacity = 1.f;
    style.has_opacity = opt_number_prop(s, props, "opacity", &style.opacity);
    if (style.has_opacity)
        style.opacity = std::fmin(1.f, std::fmax(0.f, style.opacity));
    if (kind == INLAY_UI_PROGRESS || kind == INLAY_UI_SLIDER) {
        style.has_value = opt_number_prop(s, props, "value", &style.value, true);
        style.has_min = opt_number_prop(s, props, "min", &style.min_val, true);
        style.has_max = opt_number_prop(s, props, "max", &style.max_val, true);
        style.has_step = opt_number_prop(s, props, "step", &style.step, true);
    }
    style.background = string_prop(s, props, "bg");
    style.color = string_prop(s, props, "color");
    style.border_color = string_prop(s, props, "borderColor");
    style.track_color = string_prop(s, props, "trackColor");
    style.src = string_prop(s, props, "src");
    style.has_border_width = opt_number_prop(s, props, "borderWidth", &style.border_width);
    style.border_sides.top = std::fmax(0.f, number_prop(s, props, "borderTop", 0));
    style.border_sides.right = std::fmax(0.f, number_prop(s, props, "borderRight", 0));
    style.border_sides.bottom = std::fmax(0.f, number_prop(s, props, "borderBottom", 0));
    style.border_sides.left = std::fmax(0.f, number_prop(s, props, "borderLeft", 0));
    style.radius = string_prop(s, props, "radius");
    const auto align_x = string_prop(s, props, "alignX");
    style.has_align_x =
        parse_align_x(align_x.length ? align_x : string_prop(s, props, "justifyContent"), &style.align_x);
    const auto align_y = string_prop(s, props, "alignY");
    style.has_align_y =
        parse_align_y(align_y.length ? align_y : string_prop(s, props, "alignItems"), &style.align_y);
    style.has_scroll_x = opt_bool_prop(s, props, "scrollX", &style.scroll_x) ||
                         opt_bool_prop(s, props, "scroll_x", &style.scroll_x);
    style.has_scroll_y = opt_bool_prop(s, props, "scrollY", &style.scroll_y) ||
                         opt_bool_prop(s, props, "scroll_y", &style.scroll_y);
    if (kind == INLAY_UI_IMAGE) {
        uint32_t icon = 0;
        style.has_sprite = opt_uint_prop(s, props, "spriteId", UINT32_MAX, &style.sprite_id);
        if (opt_uint_prop(s, props, "iconId", uint32_t(ETCH_INVALID_ICON - 1u), &icon)) {
            style.has_icon = true;
            style.icon_id = uint16_t(icon);
        }
    }
    if (kind == INLAY_UI_INPUT || kind == INLAY_UI_TOGGLE || kind == INLAY_UI_SLIDER) {
        if (!parse_event_prop(s, props, "onChange", style))
            parse_event_prop(s, props, "action", style);
    } else if (kind == INLAY_UI_BUTTON) {
        if (!parse_event_prop(s, props, "onClick", style))
            parse_event_prop(s, props, "action", style);
    }
    const auto axis = string_prop(s, props, "axis");
    style.column = kind == INLAY_UI_SCROLL ? !equal(axis, "row") : equal(axis, "column");
    style.animate = bool_prop(s, props, "animateLayout");
}
uint32_t build(InlaySurface *s, JSValueConst value, uint32_t depth, uint32_t sibling_slot) {
    if (interrupt(s->js, s)) {
        JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
        return none;
    }
    if (JS_IsNull(value) || JS_IsUndefined(value) || JS_IsBool(value))
        return none;
    if (depth > 64 || s->node_count == s->desc.max_nodes) {
        JS_ThrowInternalError(s->js, "UI node/depth capacity exceeded");
        return none;
    }
    const uint32_t index = s->node_count++;
    auto &n = s->nodes[index];
    n = {INLAY_UI_TEXT, hash_slot_id(sibling_slot, INLAY_UI_TEXT), none, none, {}};
    s->styles[index] = {};
    s->styles[index].handler = none;
    s->styles[index].size = 15;
    s->styles[index].opacity = 1.f;
    if (!JS_IsObject(value)) {
        n.text = copy_text(s, value);
        return index;
    }
    const auto tag = string_prop(s, value, "tag");
    if (equal(tag, "box"))
        n.kind = INLAY_UI_BOX;
    else if (equal(tag, "button"))
        n.kind = INLAY_UI_BUTTON;
    else if (equal(tag, "input"))
        n.kind = INLAY_UI_INPUT;
    else if (equal(tag, "toggle"))
        n.kind = INLAY_UI_TOGGLE;
    else if (equal(tag, "scroll"))
        n.kind = INLAY_UI_SCROLL;
    else if (equal(tag, "progress"))
        n.kind = INLAY_UI_PROGRESS;
    else if (equal(tag, "slider"))
        n.kind = INLAY_UI_SLIDER;
    else if (equal(tag, "badge"))
        n.kind = INLAY_UI_BADGE;
    else if (equal(tag, "divider"))
        n.kind = INLAY_UI_DIVIDER;
    else if (equal(tag, "image"))
        n.kind = INLAY_UI_IMAGE;
    else if (!equal(tag, "text")) {
        JS_ThrowTypeError(s->js, "Unknown UI tag");
        return none;
    }
    const auto props = JS_GetPropertyStr(s->js, value, "props");
    if (!JS_IsObject(props)) {
        JS_FreeValue(s->js, props);
        JS_ThrowTypeError(s->js, "UI props must be an object");
        return none;
    }
    parse_style(s, props, n.kind, s->styles[index]);
    auto &style = s->styles[index];
    if (!hash_string_prop(s, props, "id", 2166136261u, &n.id) &&
        !hash_string_prop(s, props, "key", 0x811c9dc5u ^ 0x6b65793au, &n.id))
        n.id = hash_slot_id(sibling_slot, n.kind);
    if (n.kind == INLAY_UI_INPUT) {
        n.text = string_prop(s, props, "value");
        style.placeholder = string_prop(s, props, "placeholder");
    } else if (n.kind == INLAY_UI_TOGGLE) {
        n.text = string_prop(s, props, "label");
        style.checked = bool_prop(s, props, "checked");
    } else if (n.kind == INLAY_UI_SLIDER) {
        n.text = string_prop(s, props, "label");
        if (!n.text.length)
            n.text = string_prop(s, props, "text");
    } else if (n.kind == INLAY_UI_IMAGE) {
        n.text = string_prop(s, props, "alt");
        if (!n.text.length)
            n.text = string_prop(s, props, "text");
    } else {
        n.text = string_prop(s, props, "text");
        if (n.kind == INLAY_UI_PROGRESS && !n.text.length)
            n.text = string_prop(s, props, "label");
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
                        n.first_child = at;
                    else
                        s->nodes[previous].next_sibling = at;
                    previous = at;
                }
            }
    }
    JS_FreeValue(s->js, children);
    if (n.kind != INLAY_UI_BOX && n.kind != INLAY_UI_SCROLL && !n.text.length && n.first_child != none)
        n.text = s->nodes[n.first_child].text;
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
struct WidgetEvents { WidgetEvent *first, *last; };

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
            "(()=>{const m=t=>(p,...c)=>(p&&typeof p==='object'&&typeof p.tag==='string')?h(t,{},p,...c):h(t,p,...c),"
            "x=t=>(p,...c)=>typeof p==='string'?h(t,{text:p},...c):h(t,p,...c);"
            "globalThis.ui={box:m('box'),text:x('text'),button:m('button'),input:m('input'),toggle:m('toggle'),"
            "scroll:m('scroll'),progress:m('progress'),slider:m('slider'),badge:x('badge'),divider:m('divider'),image:m('image')};})();"
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
EtchColor color(EtchStr value, EtchTheme theme, EtchColor fallback) {
    if (equal(value, "var(--etch-bg-elevated)"))
        return theme.bg_elevated;
    if (equal(value, "var(--etch-bg-surface)"))
        return theme.bg_surface;
    if (equal(value, "var(--etch-bg-canvas)"))
        return theme.bg_canvas;
    if (equal(value, "var(--etch-bg-input)"))
        return theme.bg_input;
    if (equal(value, "var(--etch-surface-control)"))
        return theme.surface_control;
    if (equal(value, "var(--etch-accent)"))
        return theme.accent;
    if (equal(value, "var(--etch-accent-primary)"))
        return theme.accent_primary;
    if (equal(value, "var(--etch-text-primary)"))
        return theme.text_primary;
    if (equal(value, "var(--etch-text-secondary)"))
        return theme.text_secondary;
    if (equal(value, "var(--etch-text-body)"))
        return theme.text_body;
    if (equal(value, "var(--etch-border-subtle)"))
        return theme.border_subtle;
    if (equal(value, "var(--etch-border-line)"))
        return theme.border_line;
    if (equal(value, "var(--etch-border-input)"))
        return theme.border_input;
    if (equal(value, "var(--etch-border-control)"))
        return theme.border_control;
    if (equal(value, "var(--etch-text-muted)"))
        return theme.text_muted;
    if (equal(value, "var(--etch-danger)"))
        return theme.danger;
    if (equal(value, "var(--etch-on-primary)"))
        return theme.on_primary;
    if (equal(value, "transparent"))
        return etch_rgba8(0, 0, 0, 0);
    if ((value.length == 7 || value.length == 9) && value.chars[0] == '#') {
        uint32_t rgba = 0;
        for (uint32_t i = 1; i < value.length; ++i) {
            const char c = value.chars[i];
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0)
                return fallback;
            rgba = (rgba << 4) | uint32_t(digit);
        }
        if (value.length == 7)
            return etch_rgba8(uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba), 255);
        return etch_rgba8(uint8_t(rgba >> 24), uint8_t(rgba >> 16), uint8_t(rgba >> 8),
                          uint8_t(rgba));
    }
    return fallback;
}
float parse_radius(EtchStr value, const EtchTheme &theme, float fallback) {
    if (!value.length)
        return fallback;
    if (equal(value, "panel"))
        return theme.radius_panel_px;
    if (equal(value, "control"))
        return theme.radius_control_px;
    if (equal(value, "input"))
        return theme.radius_input_px;
    if (equal(value, "small"))
        return theme.radius_small_px;
    if (equal(value, "pill"))
        return theme.radius_pill_px;
    char buf[32] = {};
    if (value.length < sizeof(buf)) {
        std::memcpy(buf, value.chars, value.length);
        char *end = nullptr;
        const float r = std::strtof(buf, &end);
        if (end != buf && *end == '\0' && std::isfinite(r) && r >= 0 && r <= 16384)
            return r;
    }
    return fallback;
}
bool has_border_sides(EtchPadding sides) {
    return sides.left > 0 || sides.right > 0 || sides.top > 0 || sides.bottom > 0;
}
EtchStr frame_text(InlaySurface *s, EtchStr text) {
    return etch_frame_arena_push_str(s->etch, text.chars, text.length);
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
    auto *json = static_cast<char *>(etch_frame_arena_alloc(ctx, size_t(length)*6+2, 1));
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
            std::memcpy(json+out, "\\u00", 4);
            out += 4;
            json[out++] = hex[c>>4];
            json[out++] = hex[c&15];
        } else
            json[out++] = char(c);
    }
    json[out++] = '"';
    return {json, out, false};
}
void draw_node(InlaySurface *s, uint32_t index, uint32_t parent, const EtchTheme &theme, WidgetEvents *events) {
    const auto &n = s->nodes[index];
    const auto &st = s->styles[index];
    const uint32_t id = etch_id_index(parent, n.id);
    const bool sides = has_border_sides(st.border_sides);
    const bool use_layer = st.has_opacity && st.opacity < 1.f &&
        n.kind != INLAY_UI_BOX && n.kind != INLAY_UI_SCROLL && n.kind != INLAY_UI_BADGE && n.kind != INLAY_UI_DIVIDER;
    if (use_layer) {
        auto layer = etch_layer_desc_default();
        layer.opacity = st.opacity;
        etch_push_layer(&layer);
    }
    if (n.kind == INLAY_UI_BOX || n.kind == INLAY_UI_SCROLL) {
        auto box = etch_box_desc_default();
        box.id = id;
        box.axis = st.column ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        box.width = st.width < 0   ? etch_sizing_grow(0, 0)
                    : st.width > 0 ? etch_sizing_fixed(st.width)
                                   : etch_sizing_fit(0, 0);
        box.height = st.height < 0   ? etch_sizing_grow(0, 0)
                     : st.height > 0 ? etch_sizing_fixed(st.height)
                                     : etch_sizing_fit(0, 0);
        box.gap = st.gap;
        box.padding = etch_padding_all(std::fmax(0.f, st.padding));
        box.align_x = st.align_x;
        box.align_y = st.align_y;
        box.background = color(st.background, theme, {});
        box.border_width = st.has_border_width ? std::fmax(0.f, st.border_width) : 0.f;
        box.border_sides = st.border_sides;
        if (st.border_color.length || box.border_width > 0 || sides)
            box.border_color = color(st.border_color, theme, theme.border_subtle);
        box.radius = etch_corners_all(parse_radius(st.radius, theme, 0));
        box.animate_layout = st.animate;
        if (st.has_opacity)
            box.opacity = st.opacity;
        if (n.kind == INLAY_UI_SCROLL) {
            box.scroll_x = st.has_scroll_x ? st.scroll_x : false;
            box.scroll_y = st.has_scroll_y ? st.scroll_y : true;
            box.clip_children = true;
            box.show_scrollbar = true;
        } else if (st.has_scroll_x || st.has_scroll_y) {
            box.scroll_x = st.scroll_x;
            box.scroll_y = st.scroll_y;
            if (box.scroll_x || box.scroll_y)
                box.clip_children = true;
        }
        etch_begin_box(&box);
        for (uint32_t at = n.first_child; at != none; at = s->nodes[at].next_sibling)
            draw_node(s, at, id, theme, events);
        etch_end_box();
    } else if (n.kind == INLAY_UI_BUTTON) {
        const EtchStr label = frame_text(s, n.text);
        const float border_w = st.has_border_width ? std::fmax(0.f, st.border_width) : 0.f;
        if (border_w > 0 || sides) {
            auto box = etch_box_desc_default();
            box.id = id;
            if (st.width < 0)
                box.width = etch_sizing_grow(0, 0);
            else if (st.width > 0)
                box.width = etch_sizing_fixed(st.width);
            if (st.height < 0)
                box.height = etch_sizing_grow(0, 0);
            else
                box.height = etch_sizing_fixed(st.height > 0 ? st.height : 32.f);
            box.padding = st.has_padding ? etch_padding_all(std::fmax(0.f, st.padding))
                                         : etch_padding_xy(14, 0);
            box.gap = 8;
            box.align_x = st.has_align_x ? st.align_x : ETCH_ALIGN_X_CENTER;
            box.align_y = st.has_align_y ? st.align_y : ETCH_ALIGN_Y_CENTER;
            box.background = st.background.length
                                 ? color(st.background, theme, theme.surface_control)
                                 : theme.surface_control;
            box.hover_background = etch_color_blend_over(
                box.background,
                {theme.text_primary.r, theme.text_primary.g, theme.text_primary.b, 26});
            box.border_color = color(st.border_color, theme, theme.border_subtle);
            box.border_width = border_w;
            box.border_sides = st.border_sides;
            box.radius = etch_corners_all(parse_radius(st.radius, theme, theme.radius_control_px));
            box.focusable = true;
            etch_begin_box(&box);
            EtchA11yOverrideDesc ov = {};
            ov.element_id = id;
            ov.role = ETCH_A11Y_BUTTON;
            ov.label = label;
            ov.actions_mask = 3u;
            ov.has_role = true;
            ov.has_label = true;
            ov.has_actions_mask = true;
            etch_set_a11y_override(&ov);
            auto text = etch_text_desc_default();
            text.font_size_px = st.has_size && st.size > 0 ? st.size : 13.f;
            text.color =
                st.color.length ? color(st.color, theme, theme.text_primary) : theme.text_primary;
            text.wrap = false;
            etch_text(label, &text);
            etch_end_box();
            if (etch_hovered(id))
                etch_set_cursor(ETCH_CURSOR_HAND);
            if (etch_clicked(id) ||
                (etch_focused(id) && (etch_key_pressed(SDLK_RETURN, false) ||
                                      etch_key_pressed(SDLK_KP_ENTER, false) ||
                                      etch_key_pressed(SDLK_SPACE, false))))
                queue_event(s, events, st.action, st.handler, ETCH_STR_LIT("null"), true);
        } else {
            auto button = etch_button_desc_default();
            button.id = id;
            button.label = label;
            if (st.width < 0)
                button.width = etch_sizing_grow(0, 0);
            else if (st.width > 0)
                button.width = etch_sizing_fixed(st.width);
            if (st.height < 0)
                button.height = etch_sizing_grow(0, 0);
            else if (st.height > 0)
                button.height = etch_sizing_fixed(st.height);
            if (st.has_padding)
                button.padding = etch_padding_all(std::fmax(0.f, st.padding));
            if (st.has_size && st.size > 0)
                button.font_size_px = st.size;
            if (st.radius.length)
                button.radius =
                    etch_corners_all(parse_radius(st.radius, theme, theme.radius_control_px));
            if (st.has_align_x)
                button.align_x = st.align_x;
            const bool custom_theme = st.background.length || st.color.length;
            if (custom_theme) {
                auto btn_theme = theme;
                if (st.background.length)
                    btn_theme.surface_control =
                        color(st.background, theme, theme.surface_control);
                if (st.color.length)
                    btn_theme.text_primary = color(st.color, theme, theme.text_primary);
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
        if (st.width < 0)
            input.width = etch_sizing_grow(0, 0);
        else if (st.width > 0)
            input.width = etch_sizing_fixed(st.width);
        if (st.height < 0)
            input.height = etch_sizing_grow(0, 0);
        else if (st.height > 0)
            input.height = etch_sizing_fixed(st.height);
        if (st.has_padding)
            input.padding = etch_padding_all(std::fmax(0.f, st.padding));
        input.font_size_px = st.size > 0 ? st.size : 15;
        if (st.background.length)
            input.background = color(st.background, theme, input.background);
        if (st.color.length)
            input.text_color = color(st.color, theme, input.text_color);
        if (st.border_color.length)
            input.border_color = color(st.border_color, theme, input.border_color);
        if (st.has_border_width)
            input.border_width = std::fmax(0.f, st.border_width);
        if (st.radius.length)
            input.radius = etch_corners_all(parse_radius(st.radius, theme, theme.radius_input_px));
        // Etch keeps editing, selection, and IME state by ID and copies the
        // display text during the call. Each widget supplies its controlled value.
        uint32_t length = n.text.length;
        if (length >= input_capacity) {
            length = input_capacity-1;
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
        toggle.font_size_px = st.size;
        if (etch_toggle_switch(&toggle))
            queue_event(s, events, st.action, st.handler,
                        checked ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
    } else if (n.kind == INLAY_UI_PROGRESS) {
        auto pb = etch_progress_bar_desc_default();
        pb.id = id;
        const float min_v = st.has_min ? st.min_val : 0.f;
        const float max_v = st.has_max ? st.max_val : 1.f;
        const float denom = max_v - min_v;
        const float raw = st.has_value ? st.value : 0.f;
        pb.fraction = denom > 0.f ? std::fmin(1.f, std::fmax(0.f, (raw - min_v) / denom)) : 0.f;
        pb.overlay_text = frame_text(s, n.text);
        if (st.width < 0)
            pb.width = etch_sizing_grow(0, 0);
        else if (st.width > 0)
            pb.width = etch_sizing_fixed(st.width);
        if (st.height > 0)
            pb.height = etch_sizing_fixed(st.height);
        if (st.radius.length)
            pb.radius = etch_corners_all(parse_radius(st.radius, theme, theme.radius_pill_px));
        if (st.track_color.length)
            pb.track_color = color(st.track_color, theme, pb.track_color);
        else if (st.background.length)
            pb.track_color = color(st.background, theme, pb.track_color);
        if (st.color.length)
            pb.fill_color = color(st.color, theme, pb.fill_color);
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
        if (st.width < 0)
            sl.width = etch_sizing_grow(0, 0);
        else if (st.width > 0)
            sl.width = etch_sizing_fixed(st.width);
        if (st.height > 0)
            sl.height_px = st.height;
        if (st.background.length)
            sl.background = color(st.background, theme, sl.background);
        if (st.color.length)
            sl.fill_color = color(st.color, theme, sl.fill_color);
        if (etch_slider(&sl) && (st.action.length || st.handler != none)) {
            char buf[32] = {};
            const int len = std::snprintf(buf, sizeof(buf), "%.9g", double(val));
            if (len > 0)
                queue_event(s, events, st.action, st.handler,
                            frame_text(s, {buf, uint32_t(len), false}));
        }
    } else if (n.kind == INLAY_UI_BADGE) {
        auto box = etch_box_desc_default();
        box.id = id;
        box.width = st.width < 0   ? etch_sizing_grow(0, 0)
                    : st.width > 0 ? etch_sizing_fixed(st.width)
                                   : etch_sizing_fit(0, 0);
        box.height = st.height < 0   ? etch_sizing_grow(0, 0)
                     : st.height > 0 ? etch_sizing_fixed(st.height)
                                     : etch_sizing_fit(0, 0);
        box.padding = st.has_padding ? etch_padding_all(std::fmax(0.f, st.padding))
                                     : etch_padding_xy(8, 3);
        box.align_x = st.has_align_x ? st.align_x : ETCH_ALIGN_X_CENTER;
        box.align_y = st.has_align_y ? st.align_y : ETCH_ALIGN_Y_CENTER;
        box.background = st.background.length ? color(st.background, theme, theme.surface_subtle)
                                              : theme.surface_subtle;
        box.border_width = st.has_border_width ? std::fmax(0.f, st.border_width)
                                               : (st.border_color.length ? 1.f : 0.f);
        if (st.border_color.length || box.border_width > 0)
            box.border_color = color(st.border_color, theme, theme.border_subtle);
        box.radius = etch_corners_all(parse_radius(st.radius, theme, theme.radius_pill_px));
        if (st.has_opacity)
            box.opacity = st.opacity;
        etch_begin_box(&box);
        auto text = etch_text_desc_default();
        text.font_size_px = st.has_size && st.size > 0 ? st.size : 12.f;
        text.color = st.color.length ? color(st.color, theme, theme.text_primary) : theme.text_primary;
        text.wrap = false;
        etch_text(frame_text(s, n.text), &text);
        etch_end_box();
    } else if (n.kind == INLAY_UI_DIVIDER) {
        auto box = etch_box_desc_default();
        box.id = id;
        if (!st.column) {
            box.width = st.width < 0   ? etch_sizing_grow(0, 0)
                        : st.width > 0 ? etch_sizing_fixed(st.width)
                                       : etch_sizing_grow(0, 0);
            box.height = etch_sizing_fixed(st.height > 0 ? st.height : 1.f);
        } else {
            box.width = etch_sizing_fixed(st.width > 0 ? st.width : 1.f);
            box.height = st.height < 0   ? etch_sizing_grow(0, 0)
                         : st.height > 0 ? etch_sizing_fixed(st.height)
                                         : etch_sizing_grow(0, 0);
        }
        box.background = st.color.length        ? color(st.color, theme, theme.border_line)
                         : st.background.length ? color(st.background, theme, theme.border_line)
                                                : theme.border_line;
        if (st.has_opacity)
            box.opacity = st.opacity;
        etch_box(&box);
    } else if (n.kind == INLAY_UI_IMAGE) {
        if (st.has_sprite && st.sprite_id != ETCH_INVALID_SPRITE) {
            auto img = etch_image_desc_default();
            img.id = id;
            img.sprite_id = st.sprite_id;
            img.width = st.width > 0 ? etch_sizing_fixed(st.width) : etch_sizing_fit(0, 0);
            img.height = st.height > 0 ? etch_sizing_fixed(st.height) : etch_sizing_fit(0, 0);
            if (st.radius.length)
                img.radius = etch_corners_all(parse_radius(st.radius, theme, 0));
            if (st.color.length)
                img.tint = color(st.color, theme, etch_color_white(255));
            etch_image(&img);
        } else if (st.has_icon && st.icon_id != ETCH_INVALID_ICON) {
            auto ic = etch_icon_desc_default();
            ic.id = id;
            ic.icon_id = st.icon_id;
            ic.size_px = st.width > 0 ? st.width : (st.has_size && st.size > 0 ? st.size : 24.f);
            ic.color = st.color.length ? color(st.color, theme, theme.text_primary) : theme.text_primary;
            etch_icon(&ic);
        } else {
            auto box = etch_box_desc_default();
            box.id = id;
            box.width = st.width < 0 ? etch_sizing_grow(0, 0) : etch_sizing_fixed(st.width > 0 ? st.width : 64.f);
            box.height = st.height < 0 ? etch_sizing_grow(0, 0) : etch_sizing_fixed(st.height > 0 ? st.height : 64.f);
            box.align_x = ETCH_ALIGN_X_CENTER;
            box.align_y = ETCH_ALIGN_Y_CENTER;
            box.background = st.background.length ? color(st.background, theme, theme.surface_subtle)
                                                  : theme.surface_subtle;
            box.border_width = st.has_border_width ? std::fmax(0.f, st.border_width) : 1.f;
            box.border_color = st.border_color.length ? color(st.border_color, theme, theme.border_subtle)
                                                      : theme.border_subtle;
            box.radius = etch_corners_all(parse_radius(st.radius, theme, theme.radius_small_px));
            etch_begin_box(&box);
            const EtchStr label = n.text.length ? n.text : st.src;
            if (label.length) {
                auto text = etch_text_desc_default();
                text.font_size_px = st.has_size && st.size > 0 ? st.size : 12.f;
                text.color = st.color.length ? color(st.color, theme, theme.text_muted) : theme.text_muted;
                text.wrap = false;
                etch_text(frame_text(s, label), &text);
            }
            etch_end_box();
        }
    } else {
        auto text = etch_text_desc_default();
        text.id = id;
        text.font_size_px = st.size;
        text.color = color(st.color, theme, theme.text_primary);
        etch_text(frame_text(s, n.text), &text);
    }
    if (use_layer)
        etch_pop_layer();
}
} // namespace
extern "C" {
InlaySurfaceDesc inlay_surface_desc_default(void) {
    return {8u * 1024u * 1024u, 512u * 1024u, 4, 4096, 65536};
}
InlaySurfaceViewDesc inlay_surface_view_desc_default(void) { return {0, 640, 480}; }
InlaySurface *inlay_surface_create(EtchContext *context, const InlaySurfaceDesc *desc) {
    const auto d = desc ? *desc : inlay_surface_desc_default();
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
    s->nodes = static_cast<InlayUiNode *>(std::calloc(d.max_nodes, sizeof(InlayUiNode)));
    s->styles = static_cast<Style *>(std::calloc(d.max_nodes, sizeof(Style)));
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
void inlay_surface_destroy(InlaySurface *s) {
    if (s) {
        clear_context(s);
        if (s->runtime)
            JS_FreeRuntime(s->runtime);
        std::free(s->handlers);
        std::free(s->nodes);
        std::free(s->styles);
        std::free(s->text);
        std::free(s->json_scratch);
        std::free(s->source);
        std::free(s->state);
        std::free(s);
    }
}
bool inlay_surface_set_script(InlaySurface *s, EtchStr script) {
    if (!s || s->running || (!script.chars && script.length) || script.length > 1024u * 1024u)
        return false;
    return load(s, script);
}
bool inlay_surface_set_state_json(InlaySurface *s, EtchStr json) {
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
    return s->running || !s->loaded ? true : run_update(s, {}, {});
}
EtchStr inlay_surface_state_json(const InlaySurface *s) {
    if (!s)
        return {};
    if (s->state_dirty)
        sync_state_json(const_cast<InlaySurface *>(s));
    return EtchStr{s->state, s->state_size, false};
}
bool inlay_surface_bind_action(InlaySurface *s, EtchStr name, InlayActionFn fn, void *user) {
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
bool inlay_surface_dispatch(InlaySurface *s, EtchStr name, EtchStr payload) {
    if (!s || !s->js || !s->loaded || s->running || !name.chars || !name.length ||
        name.length >= 64 || !payload.chars || !payload.length || payload.length > 65536)
        return false;
    return run_update(s, name, payload);
}
bool inlay_surface_enable_hmr(InlaySurface *s, const char *path) {
    if (!s || !path || std::strlen(path) >= sizeof(s->path))
        return false;
    std::memcpy(s->path, path, std::strlen(path) + 1);
    s->modified = -1;
    return inlay_surface_poll_hmr(s);
}
bool inlay_surface_poll_hmr(InlaySurface *s) {
    if (!s || !s->path[0] || s->running)
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
void inlay_surface_view(InlaySurface *s, const InlaySurfaceViewDesc *input) {
    if (!s || !input || !input->id || !std::isfinite(input->width_px) ||
        !std::isfinite(input->height_px) || input->width_px <= 0 || input->height_px <= 0 ||
        etch_current_context() != s->etch || !etch_frame_in_progress(s->etch) || s->running)
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
    box.id = input->id;
    box.axis = ETCH_AXIS_COL;
    box.width = etch_sizing_fixed(input->width_px);
    box.height = etch_sizing_fixed(input->height_px);
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
    } else if (s->node_count)
        draw_node(s, 0, input->id, theme, &events);
    etch_end_box();
    if (events.first && s->js && s->loaded && !s->running)
        run_batch(s, events.first);
}
int32_t inlay_surface_next_timer_ms(const InlaySurface *s) {
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
InlaySurfaceStats inlay_surface_stats(const InlaySurface *s) {
    return s ? s->stats : InlaySurfaceStats{};
}
EtchStr inlay_surface_error(const InlaySurface *s) {
    return s ? EtchStr{s->error, uint32_t(std::strlen(s->error)), false} : EtchStr{};
}
const InlayUiNode *inlay_surface_nodes(const InlaySurface *s, uint32_t *count) {
    if (count)
        *count = s ? s->node_count : 0;
    return s ? s->nodes : nullptr;
}
}
#else
extern "C" {
InlaySurfaceDesc inlay_surface_desc_default(void) {
    return {8u * 1024u * 1024u, 512u * 1024u, 4, 4096, 65536};
}
InlaySurfaceViewDesc inlay_surface_view_desc_default(void) { return {0, 640, 480}; }
InlaySurface *inlay_surface_create(EtchContext *, const InlaySurfaceDesc *) { return nullptr; }
void inlay_surface_destroy(InlaySurface *) {}
bool inlay_surface_set_script(InlaySurface *, EtchStr) { return false; }
bool inlay_surface_set_state_json(InlaySurface *, EtchStr) { return false; }
EtchStr inlay_surface_state_json(const InlaySurface *) { return {}; }
bool inlay_surface_bind_action(InlaySurface *, EtchStr, InlayActionFn, void *) { return false; }
bool inlay_surface_dispatch(InlaySurface *, EtchStr, EtchStr) { return false; }
bool inlay_surface_enable_hmr(InlaySurface *, const char *) { return false; }
bool inlay_surface_poll_hmr(InlaySurface *) { return false; }
void inlay_surface_view(InlaySurface *, const InlaySurfaceViewDesc *) {}
int32_t inlay_surface_next_timer_ms(const InlaySurface *) { return -1; }
InlaySurfaceStats inlay_surface_stats(const InlaySurface *) { return {}; }
EtchStr inlay_surface_error(const InlaySurface *) { return {}; }
const InlayUiNode *inlay_surface_nodes(const InlaySurface *, uint32_t *count) {
    if (count)
        *count = 0;
    return nullptr;
}
}
#endif
