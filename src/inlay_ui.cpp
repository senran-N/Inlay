#include "inlay_ui_internal.h"
#include <cstring>

// ============================================================================
// Reusable Host Row/Tree Model (Available Both With & Without Script Engine)
// ============================================================================

struct InlayModel {
    InlayModelReadFn read_fn;
    void* user_data;
    uint32_t count;
    uint64_t revision;
};

bool inlay_ui_model_read(const InlayModel* model, uint32_t index, InlayModelRow* out_row) noexcept {
    if (!model || !model->read_fn || !out_row || index >= model->count) return false;
    *out_row = {};
    return model->read_fn(index, out_row, model->user_data);
}

extern "C" {
INLAY_API InlayModel* inlay_model_create(InlayModelReadFn read_fn, void* user_data) {
    if (!read_fn) return nullptr;
    auto* m = static_cast<InlayModel*>(SDL_calloc(1u, sizeof(InlayModel)));
    if (!m) return nullptr;
    m->read_fn = read_fn;
    m->user_data = user_data;
    m->count = 0u;
    m->revision = 1u;
    return m;
}

INLAY_API void inlay_model_destroy(InlayModel* model) {
    SDL_free(model);
}

INLAY_API void inlay_model_set_count(InlayModel* model, uint32_t count) {
    if (!model) return;
    if (model->count != count) {
        model->count = count;
        ++model->revision;
        if (EtchContext* ctx = etch_current_context()) {
            etch_request_redraw(ctx);
        }
    }
}

INLAY_API void inlay_model_notify_change(InlayModel* model, uint32_t, uint32_t) {
    if (!model) return;
    ++model->revision;
    if (EtchContext* ctx = etch_current_context()) {
        etch_request_redraw(ctx);
    }
}

INLAY_API uint32_t inlay_model_count(const InlayModel* model) {
    return model ? model->count : 0u;
}

INLAY_API uint64_t inlay_model_revision(const InlayModel* model) {
    return model ? model->revision : 0u;
}
} // extern "C"

#if defined(INLAY_ENABLE_SCRIPT) && INLAY_ENABLE_SCRIPT
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_keycode.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

void inlay_ui_note_responsive_cls(InlaySurface* s) noexcept;

namespace {

constexpr uint32_t kDefaultMaxTextBytes = 256u * 1024u;
constexpr uint32_t kHardMaxTextBytes = 4u * 1024u * 1024u;
constexpr uint32_t kInitTextBytes = 8192u;
constexpr uint32_t kMaxScriptBytes = 1024u * 1024u;
constexpr double kHmrViewPollIntervalMs = 80.0;
constexpr uint32_t kAsyncActionCapacity = 32u;
constexpr size_t kAsyncPayloadLimit = 65536u;
constexpr uint32_t kMemoCacheCap = 128u;
constexpr uint32_t kMaxMemoSubtreeNodes = 256u;
constexpr uint32_t kMaxMemoSubtreeText = 8192u;
constexpr uint32_t kMaxFormStates = 16u;

enum BadgeVariant : uint8_t {
    BadgeDefault   = 0,
    BadgeSecondary = 1,
    BadgeAccent    = 2,
    BadgePrimary   = 3,
    BadgeSuccess   = 4,
    BadgeWarning   = 5,
    BadgeDanger    = 6,
    BadgeInfo      = 7,
    BadgeOutline   = 8
};

enum CoreExtraFlags : uint32_t {
    CefNone                = 0u,
    CefTextSelectable      = 1u << 0,
    CefProgressAnimated    = 1u << 1,
    CefProgressIndeterm    = 1u << 2,
    CefProgressRing        = 1u << 3,
    CefProgressShowPercent = 1u << 4,
    CefBadgeDot            = 1u << 5,
    CefSwitchHasTrackW     = 1u << 6,
    CefSwitchHasTrackH     = 1u << 7,
    CefProgressHasThick    = 1u << 8,
    CefInputPassword       = 1u << 9,
    CefInputMultiline      = 1u << 10,
    CefInputReadOnly       = 1u << 11,
    CefInputWordWrap       = 1u << 12,
    CefInputRequired       = 1u << 13,
    CefStopPropagation     = 1u << 14,
    CefButtonSubmit        = 1u << 15,
    CefEditorHasGhost      = 1u << 16,
    CefEditorHasSearch     = 1u << 17,
    CefInputHasMinLen      = 1u << 18,
    CefInputHasMaxLen      = 1u << 19
};

// Sparse per-node extra spec allocated lazily only for core widgets using extended props.
struct CoreNodeExtra {
    uint32_t flags;         // CoreExtraFlags
    uint16_t tabs_disabled; // Bitmask of disabled tab items (up to 16)
    uint8_t badge_variant;  // BadgeVariant
    TextSlice dbl_action;
    uint32_t dbl_handler;
    TextSlice rclick_action;
    uint32_t rclick_handler;
    TextSlice password_text;
    TextSlice submit_action;
    uint32_t submit_handler;
    TextSlice field_name;     // Input field name, Form resetKey, or CodeEditor ghostText
    TextSlice field_error;    // Input error text, Form onInvalid action, or CodeEditor searchQuery
    uint32_t invalid_handler; // Form onInvalid handler (INLAY_UI_NONE if none)
    uint32_t min_length;      // Input minLength
    uint32_t max_length;      // Input maxLength
    ColorRef color_a;         // Text selection_color OR Switch off_color
    ColorRef color_b;         // Switch thumb_color
    float aux_f0;             // Switch track_width OR Progress ring thickness
    float aux_f1;             // Switch track_height
};

struct SplitSyncEntry {
    uint32_t element_id;
    float last_prop_ratio;
    float last_live_ratio;
    uint64_t last_used_frame;
};

struct IndetPhaseSlot {
    uint32_t element_id;
    float target;
    bool initialized;
    bool reduced;
    uint64_t last_used_frame;
};

struct HoverTrackEntry {
    uint32_t element_id;
    bool hovered;
    bool focused;
    uint64_t last_used_frame;
};

struct ActionPolicyEntry {
    char name[64];
    InlayActionPermission permission;
    bool require_approval;
    char required_keys_csv[128];
};

struct MemoNativeEntry {
    uint32_t memo_key;
    uint32_t memo_rev;
    uint32_t last_used;
    uint32_t node_count;
    uint32_t cold_count;
    uint32_t extra_count;
    uint32_t text_len;
    uint8_t bp_bucket;
    bool root_explicit_id;
    bool has_responsive;
    uint8_t pad_;
    uint8_t* blob;
};

struct FormStateEntry {
    uint32_t element_id;
    uint64_t reset_key_hash;
    bool show_errors;
    uint64_t last_used_frame;
};

struct ActiveFormField {
    uint32_t element_id;
    EtchStr name;
    EtchStr value;
    bool valid;
};

struct ActiveFormCtx {
    uint32_t form_elem_id;
    bool show_errors;
    bool submit_requested;
    ActiveFormField* fields;
    uint32_t field_count;
    uint32_t field_cap;
};

enum EventConsumeBits : uint8_t {
    ConsumeNone     = 0u,
    ConsumeClick    = 1u << 0,
    ConsumeDblClick = 1u << 1,
    ConsumeRClick   = 1u << 2,
    ConsumeEscape   = 1u << 3
};

enum class AsyncActionPhase : uint8_t { Empty, Queued, Started, Cancelled };

struct AsyncActionName {
    char value[64];
};

struct AsyncActionSlot {
    JSValue settle[2];
    char* payload;
    uint32_t payload_length;
    uint32_t name_index;
    uint32_t id;
    AsyncActionPhase phase;
};

// Standard-layout wrapper keeping private inlay_ui.cpp state out of inlay_ui_internal.h.
struct SurfaceStorage {
    InlaySurface pub; // Must remain first member (pointer-interconvertible with InlaySurface*).
    uint32_t text_capacity;
    double last_hmr_check_ms;
    uint32_t* node_extra_idx; // Parallel to pub.nodes (capacity = pub.node_capacity).
    CoreNodeExtra* core_extras;
    uint32_t core_extra_count;
    uint32_t core_extra_cap;
    SplitSyncEntry split_sync[16];
    uint32_t split_sync_count;
    IndetPhaseSlot indet_slots[16];
    uint32_t indet_slot_count;
    HoverTrackEntry hover_track[32];
    uint32_t hover_track_count;
    AsyncActionName* async_action_names;
    uint32_t async_action_name_count, async_action_name_cap;
    AsyncActionSlot* async_actions;
    uint32_t next_async_action;
    bool async_actions_closed;
    char* reload_error;
    // #14 Action permissions & audit hook
    ActionPolicyEntry* action_policies;
    uint32_t action_policy_count;
    uint32_t action_policy_cap;
    InlayActionAuditFn audit_fn;
    void* audit_user;
    // #50 Native subtree cache for memo()
    MemoNativeEntry memo_cache[kMemoCacheCap];
    uint32_t memo_cache_clock;
    JSAtom atom_memo_key;
    JSAtom atom_memo_rev;
    // #54 Form validation state
    FormStateEntry form_states[kMaxFormStates];
    uint32_t form_state_count;
    ActiveFormCtx* active_form;
    // #52 Responsive breakpoint tracking
    uint32_t responsive_cls_hits;
    bool uses_responsive_cls;
    bool has_view_width;
    // #59 Subtree-scoped event consumption
    uint8_t child_consumed_mask;
};
static_assert(std::is_standard_layout_v<SurfaceStorage>);
static_assert(offsetof(SurfaceStorage, pub) == 0);

inline SurfaceStorage* ext(InlaySurface* s) noexcept {
    return reinterpret_cast<SurfaceStorage*>(s);
}

inline const SurfaceStorage* ext(const InlaySurface* s) noexcept {
    return reinterpret_cast<const SurfaceStorage*>(s);
}

void release_async_action(InlaySurface* s, AsyncActionSlot& slot, bool notify_host) noexcept {
    JS_FreeValue(s->js, slot.settle[0]);
    JS_FreeValue(s->js, slot.settle[1]);
    SDL_free(slot.payload);
    const uint32_t id = slot.id;
    const bool notify = notify_host && slot.phase == AsyncActionPhase::Started;
    slot = {};
    if (notify) {
        slot.id = id;
        slot.phase = AsyncActionPhase::Cancelled;
    }
}

void retire_async_actions(InlaySurface* s) noexcept {
    if (auto* slots = ext(s)->async_actions) {
        for (uint32_t i = 0u; i < kAsyncActionCapacity; ++i) {
            auto& slot = slots[i];
            if (slot.phase == AsyncActionPhase::Queued || slot.phase == AsyncActionPhase::Started) {
                release_async_action(s, slot, true);
            }
        }
    }
}

AsyncActionSlot* find_async_action(InlaySurface* s, uint32_t id) noexcept {
    if (auto* slots = ext(s)->async_actions) {
        for (uint32_t i = 0u; i < kAsyncActionCapacity; ++i) {
            if (slots[i].id == id) return &slots[i];
        }
    }
    return nullptr;
}

inline bool has_event(TextSlice action, uint32_t handler) noexcept {
    return action.len > 0u || handler != INLAY_UI_NONE;
}

inline const CoreNodeExtra* get_core_extra(const InlaySurface* s, uint32_t node_index) noexcept {
    const auto* e = ext(s);
    if (!e->node_extra_idx || node_index >= s->node_capacity) return nullptr;
    const uint32_t idx = e->node_extra_idx[node_index];
    return (idx != INLAY_UI_NONE && idx < e->core_extra_count) ? &e->core_extras[idx] : nullptr;
}

CoreNodeExtra* ensure_core_extra(InlaySurface* s, uint32_t node_index) {
    auto* e = ext(s);
    const uint32_t existing = e->node_extra_idx[node_index];
    if (existing != INLAY_UI_NONE && existing < e->core_extra_count) {
        return &e->core_extras[existing];
    }
    const uint32_t idx = inlay_ui_alloc_pool(s, e->core_extras, e->core_extra_count,
                                             e->core_extra_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return nullptr;
    auto& cx = e->core_extras[idx];
    cx = {};
    cx.dbl_handler = INLAY_UI_NONE;
    cx.rclick_handler = INLAY_UI_NONE;
    cx.submit_handler = INLAY_UI_NONE;
    cx.invalid_handler = INLAY_UI_NONE;
    e->node_extra_idx[node_index] = idx;
    return &cx;
}

bool is_container_kind(InlayUiKind kind) noexcept {
    switch (kind) {
        case INLAY_UI_BOX:
        case INLAY_UI_SCROLL:
        case INLAY_UI_CARD:
        case INLAY_UI_FORM:
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
        case INLAY_UI_BUTTON:
        case INLAY_UI_BADGE:
        case INLAY_UI_TREE:
        case INLAY_UI_TREE_ITEM:
        case INLAY_UI_MENU:
        case INLAY_UI_CONTEXT_MENU:
        case INLAY_UI_ACCORDION:
        case INLAY_UI_TOAST:
        case INLAY_UI_VIRTUAL_LIST:
        case INLAY_UI_TABLE:
        case INLAY_UI_AVATAR:
        case INLAY_UI_SKELETON:
            return true;
        default:
            return false;
    }
}

JSValue parse_json(InlaySurface* s, EtchStr json) {
    if (json.length > 65536u) {
        return JS_ThrowRangeError(s->js, "Event payload exceeds 64 KiB");
    }
    if (json.length > 0u) {
        std::memcpy(s->json_scratch, json.chars, json.length);
    }
    s->json_scratch[json.length] = '\0';
    return JS_ParseJSON(s->js, s->json_scratch, json.length, "<json>");
}

void record_error(InlaySurface* s, const char* message) {
    if (s->stats.has_error && !std::strcmp(s->error, message)) return;
    retire_async_actions(s);
    std::snprintf(s->error, sizeof(s->error), "%s", message);
    s->stats.has_error = true;
    etch_request_redraw(s->etch);
}

void record_reload_error(InlaySurface* s, const char* message) {
    if (!s->loaded || s->stats.has_error) {
        record_error(s, message);
        return;
    }
    auto& previous = ext(s)->reload_error;
    if (previous && std::strcmp(previous, message) == 0) return;
    char* copy = SDL_strdup(message);
    if (!copy) {
        record_error(s, message);
        return;
    }
    SDL_free(previous);
    previous = copy;
    etch_request_redraw(s->etch);
}

int interrupt(JSContext*, void* opaque) {
    auto* s = static_cast<InlaySurface*>(opaque);
    if (s->deadline && SDL_GetPerformanceCounter() >= s->deadline) {
        s->timed_out = true;
        return 1;
    }
    return 0;
}

void begin_tick(InlaySurface* s) {
    s->running = true;
    s->timed_out = false;
    s->deadline =
        SDL_GetPerformanceCounter() + uint64_t(double(s->desc.max_tick_ms) * s->frequency / 1000.0);
    ++s->stats.js_ticks;
}

void clear_handlers(InlaySurface* s) {
    if (s->js) {
        for (uint32_t i = 0u; i < s->handler_count; ++i) {
            JS_FreeValue(s->js, s->handlers[i]);
        }
    }
    s->handler_count = 0u;
}

bool ensure_node_capacity_for(InlaySurface* s, uint32_t extra) {
    if (extra > s->desc.max_nodes || s->node_count > s->desc.max_nodes - extra) return false;
    const uint32_t need = s->node_count + extra;
    if (need <= s->node_capacity) return true;
    uint32_t cap = s->node_capacity ? s->node_capacity : 256u;
    while (cap < need) {
        if (cap > s->desc.max_nodes / 2u) {
            cap = s->desc.max_nodes;
            break;
        }
        cap *= 2u;
    }
    if (cap > s->desc.max_nodes) cap = s->desc.max_nodes;
    auto* nn = static_cast<InlayUiNode*>(SDL_realloc(s->nodes, size_t(cap) * sizeof(InlayUiNode)));
    if (!nn) return false;
    s->nodes = nn;
    auto* ns = static_cast<NodeStyle*>(SDL_realloc(s->styles, size_t(cap) * sizeof(NodeStyle)));
    if (!ns) return false;
    s->styles = ns;
    auto* e = ext(s);
    auto* nx = static_cast<uint32_t*>(SDL_realloc(e->node_extra_idx, size_t(cap) * sizeof(uint32_t)));
    if (!nx) return false;
    e->node_extra_idx = nx;
    s->node_capacity = cap;
    return true;
}

bool ensure_node_capacity(InlaySurface* s) {
    return ensure_node_capacity_for(s, 1u);
}

// Rebase all raw EtchStr pointers into s->text after s->text grows/moves.
bool ensure_text_space(InlaySurface* s, size_t extra) {
    if (extra == 0u) return true;
    if (extra > size_t(s->desc.max_text_bytes - s->text_used)) {
        JS_ThrowInternalError(s->js, "UI text capacity exceeded");
        return false;
    }
    const uint32_t needed = s->text_used + static_cast<uint32_t>(extra);
    auto* e = ext(s);
    if (needed <= e->text_capacity) return true;
    const char* old_base = s->text;
    const uint32_t old_used = s->text_used;
    const uint32_t init_cap = s->desc.max_text_bytes < kInitTextBytes ? s->desc.max_text_bytes : kInitTextBytes;
    if (!inlay_grow_pod(s->text, e->text_capacity, needed, init_cap, s->desc.max_text_bytes)) {
        JS_ThrowInternalError(s->js, "UI text allocation failed");
        return false;
    }
    if (old_base && s->text != old_base && old_used > 0u) {
        const char* old_end = old_base + old_used;
        for (uint32_t i = 0u; i < s->node_count; ++i) {
            const char* p = s->nodes[i].text.chars;
            if (p >= old_base && p < old_end) {
                s->nodes[i].text.chars = s->text + (p - old_base);
            }
        }
        for (uint32_t i = 0u; i < s->item_pool_count; ++i) {
            const char* p = s->item_pool[i].chars;
            if (p >= old_base && p < old_end) {
                s->item_pool[i].chars = s->text + (p - old_base);
            }
        }
    }
    return true;
}

TextSlice append_text_raw(InlaySurface* s, const char* chars, size_t len) {
    if (len == 0u || !chars) return {};
    if (!ensure_text_space(s, len)) return {};
    const uint32_t off = s->text_used;
    std::memcpy(s->text + off, chars, len);
    s->text_used += static_cast<uint32_t>(len);
    return {off, static_cast<uint32_t>(len)};
}

uint32_t hash_slot_id(uint32_t slot, InlayUiKind kind) noexcept {
    const uint32_t pair[2] = {uint32_t(kind) + 1u, slot};
    const uint32_t h = inlay_fnv1a_32(pair, sizeof(pair));
    return h ? h : 1u;
}

bool hash_string_prop(InlaySurface* s, JSValueConst props, const char* name, uint32_t seed, uint32_t* out) {
    const JSValue v = JS_GetPropertyStr(s->js, props, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    size_t n = 0u;
    const char* chars = JS_ToCStringLen(s->js, &n, v);
    const bool ok = chars && n > 0u;
    if (ok) {
        const uint32_t h = inlay_fnv1a_32(chars, n, seed);
        *out = h ? h : 1u;
    }
    if (chars) JS_FreeCString(s->js, chars);
    JS_FreeValue(s->js, v);
    return ok;
}

// Extract the first user-code `file:line:col` location from a QuickJS-NG Error.stack string.
bool extract_stack_location(const char* stack, char* out, size_t out_cap) noexcept {
    if (!stack || !out || out_cap < 8u) return false;
    char fallback[256] = {};
    const char* p = stack;
    while (*p) {
        const char* line_start = p;
        while (*p && *p != '\n' && *p != '\r') ++p;
        const char* line_end = p;
        while (*p == '\n' || *p == '\r') ++p;

        while (line_start < line_end && (*line_start == ' ' || *line_start == '\t')) ++line_start;
        if (size_t(line_end - line_start) >= 3u &&
            line_start[0] == 'a' && line_start[1] == 't' && line_start[2] == ' ') {
            line_start += 3;
        }
        while (line_start < line_end && (*line_start == ' ' || *line_start == '\t')) ++line_start;
        while (line_end > line_start && (line_end[-1] == ' ' || line_end[-1] == '\t')) --line_end;
        if (line_end <= line_start) continue;

        const char* cand_start = line_start;
        const char* cand_end = line_end;
        if (cand_end[-1] == ')') {
            const char* lp = cand_end - 1;
            while (lp > cand_start && *lp != '(') --lp;
            if (*lp == '(') {
                cand_start = lp + 1;
                cand_end = cand_end - 1;
            }
        }
        // Validate trailing :line:col (walks backwards so Windows drive letters like C:\... work).
        const char* q = cand_end;
        const char* col_end = q;
        while (q > cand_start && q[-1] >= '0' && q[-1] <= '9') --q;
        if (q == col_end || q <= cand_start || q[-1] != ':') continue;
        --q; // skip ':'
        const char* line_num_end = q;
        while (q > cand_start && q[-1] >= '0' && q[-1] <= '9') --q;
        if (q == line_num_end || q <= cand_start || q[-1] != ':') continue;
        --q; // skip ':' before line
        if (q <= cand_start) continue; // empty filename

        const size_t len = size_t(cand_end - cand_start);
        const bool is_internal = (size_t(q - cand_start) == 7u && !std::memcmp(cand_start, "<inlay>", 7u));
        if (!is_internal) {
            const size_t take = len < out_cap - 1u ? len : out_cap - 1u;
            std::memcpy(out, cand_start, take);
            out[take] = '\0';
            return true;
        }
        if (!fallback[0]) {
            const size_t take = len < sizeof(fallback) - 1u ? len : sizeof(fallback) - 1u;
            std::memcpy(fallback, cand_start, take);
            fallback[take] = '\0';
        }
    }
    if (fallback[0]) {
        std::snprintf(out, out_cap, "%s", fallback);
        return true;
    }
    return false;
}

bool finish_tick(InlaySurface* s, uint64_t start) {
    if (s->deadline && SDL_GetPerformanceCounter() >= s->deadline) s->timed_out = true;
    const bool failed = s->timed_out || JS_HasException(s->js) || s->stats.has_error;
    if (failed) {
        const JSValue exception = JS_GetException(s->js);
        const char* message = JS_ToCString(s->js, exception);
        if (s->timed_out) {
            record_error(s, "Script exceeded its tick time limit");
        } else if (message) {
            char formatted[1024] = {};
            const char* final_msg = message;
            if (JS_IsError(exception)) {
                const JSValue stack_v = JS_GetPropertyStr(s->js, exception, "stack");
                if (!JS_IsUndefined(stack_v) && !JS_IsNull(stack_v) && !JS_IsException(stack_v)) {
                    const char* stack_str = JS_ToCString(s->js, stack_v);
                    if (stack_str) {
                        char loc[256] = {};
                        if (extract_stack_location(stack_str, loc, sizeof(loc)) &&
                            std::strstr(message, loc) == nullptr) {
                            std::snprintf(formatted, sizeof(formatted), "%s (%s)", message, loc);
                            final_msg = formatted;
                        }
                        JS_FreeCString(s->js, stack_str);
                    }
                }
                JS_FreeValue(s->js, stack_v);
            }
            record_error(s, final_msg);
        } else {
            record_error(s, "Script allocation failed");
        }
        if (message) JS_FreeCString(s->js, message);
        JS_FreeValue(s->js, exception);
        if (JS_HasException(s->js)) {
            const JSValue nested = JS_GetException(s->js);
            JS_FreeValue(s->js, nested);
        }
        clear_handlers(s);
        for (auto& timer : s->timers) {
            JS_FreeValue(s->js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0u;
        }
        s->state_dirty = false;
        s->loaded = false;
    }
    s->deadline = 0u;
    s->running = false;
    s->stats.last_tick_ms = double(SDL_GetPerformanceCounter() - start) * 1000.0 / s->frequency;
    return !failed;
}

bool save_state(InlaySurface* s, const char* bytes, size_t length) {
    if (length > 65536u) {
        record_error(s, "State JSON exceeds 64KiB");
        return false;
    }
    if (length + 1u > s->state_capacity) {
        uint32_t cap = s->state_capacity ? s->state_capacity : 128u;
        while (cap < length + 1u) cap *= 2u;
        auto* data = static_cast<char*>(SDL_realloc(s->state, cap));
        if (!data) {
            record_error(s, "State storage allocation failed");
            return false;
        }
        s->state = data;
        s->state_capacity = cap;
    }
    std::memmove(s->state, bytes, length);
    s->state[length] = '\0';
    s->state_size = static_cast<uint32_t>(length);
    return true;
}

template <typename Fn>
bool peek_str_prop(InlaySurface* s, JSValueConst object, const char* name, Fn&& fn) {
    const JSValue v = JS_GetPropertyStr(s->js, object, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    size_t n = 0u;
    const char* chars = JS_ToCStringLen(s->js, &n, v);
    bool matched = false;
    if (chars) {
        matched = fn(chars, n);
        JS_FreeCString(s->js, chars);
    }
    JS_FreeValue(s->js, v);
    return matched;
}

bool parse_color_prop(InlaySurface* s, JSValueConst obj, const char* name, ColorRef* out) {
    return peek_str_prop(s, obj, name, [&](const char* chars, size_t len) noexcept {
        const ColorRef c = inlay_ui_parse_color(chars, len);
        if (!c.set) return false;
        *out = c;
        return true;
    });
}

bool read_float_val(InlaySurface* s, JSValueConst v, const char* name, float* out,
                    double min_v, double max_v, bool unbounded = false) {
    double value = 0.0;
    if (JS_ToFloat64(s->js, &value, v) < 0) return false;
    const float narrowed = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(narrowed) ||
        (!unbounded && (value < min_v || value > max_v))) {
        JS_ThrowTypeError(s->js, "Invalid UI numeric property: %s", name);
        return false;
    }
    *out = narrowed;
    return true;
}

bool read_vec2_array(InlaySurface* s, JSValueConst arr, const char* name, float* ox, float* oy,
                     double min_v, double max_v) {
    const JSValue v0 = JS_GetPropertyUint32(s->js, arr, 0u);
    const JSValue v1 = JS_GetPropertyUint32(s->js, arr, 1u);
    float x = *ox;
    float y = *oy;
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

bool parse_align_x(const char* chars, size_t len, EtchAlignX* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "left") || inlay_ui_str_eq(chars, len, "start")) {
        *out = ETCH_ALIGN_X_LEFT;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "center")) {
        *out = ETCH_ALIGN_X_CENTER;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "right") || inlay_ui_str_eq(chars, len, "end")) {
        *out = ETCH_ALIGN_X_RIGHT;
        return true;
    }
    return false;
}

bool parse_align_y(const char* chars, size_t len, EtchAlignY* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "top") || inlay_ui_str_eq(chars, len, "start")) {
        *out = ETCH_ALIGN_Y_TOP;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "center")) {
        *out = ETCH_ALIGN_Y_CENTER;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "bottom") || inlay_ui_str_eq(chars, len, "end")) {
        *out = ETCH_ALIGN_Y_BOTTOM;
        return true;
    }
    return false;
}

bool parse_justify_token(const char* chars, size_t len, EtchJustify* out, bool space_only = false) noexcept {
    if (inlay_ui_str_eq(chars, len, "between") || inlay_ui_str_eq(chars, len, "space-between")) {
        *out = ETCH_JUSTIFY_SPACE_BETWEEN;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "around") || inlay_ui_str_eq(chars, len, "space-around")) {
        *out = ETCH_JUSTIFY_SPACE_AROUND;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "evenly") || inlay_ui_str_eq(chars, len, "space-evenly")) {
        *out = ETCH_JUSTIFY_SPACE_EVENLY;
        return true;
    }
    if (space_only) return false;
    if (inlay_ui_str_eq(chars, len, "start") || inlay_ui_str_eq(chars, len, "left") ||
        inlay_ui_str_eq(chars, len, "flex-start")) {
        *out = ETCH_JUSTIFY_START;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "center")) {
        *out = ETCH_JUSTIFY_CENTER;
        return true;
    }
    if (inlay_ui_str_eq(chars, len, "end") || inlay_ui_str_eq(chars, len, "right") ||
        inlay_ui_str_eq(chars, len, "flex-end")) {
        *out = ETCH_JUSTIFY_END;
        return true;
    }
    return false;
}

bool parse_align_self_token(const char* chars, size_t len, EtchAlignSelf* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "auto")) { *out = ETCH_ALIGN_SELF_AUTO; return true; }
    if (inlay_ui_str_eq(chars, len, "start") || inlay_ui_str_eq(chars, len, "flex-start")) {
        *out = ETCH_ALIGN_SELF_START; return true;
    }
    if (inlay_ui_str_eq(chars, len, "center")) { *out = ETCH_ALIGN_SELF_CENTER; return true; }
    if (inlay_ui_str_eq(chars, len, "end") || inlay_ui_str_eq(chars, len, "flex-end")) {
        *out = ETCH_ALIGN_SELF_END; return true;
    }
    if (inlay_ui_str_eq(chars, len, "stretch")) { *out = ETCH_ALIGN_SELF_STRETCH; return true; }
    return false;
}

bool parse_elevation_token(const char* chars, size_t len, EtchElevation* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "none")) { *out = ETCH_ELEVATION_NONE; return true; }
    if (inlay_ui_str_eq(chars, len, "sm")) { *out = ETCH_ELEVATION_SM; return true; }
    if (inlay_ui_str_eq(chars, len, "md")) { *out = ETCH_ELEVATION_MD; return true; }
    if (inlay_ui_str_eq(chars, len, "lg")) { *out = ETCH_ELEVATION_LG; return true; }
    if (inlay_ui_str_eq(chars, len, "xl")) { *out = ETCH_ELEVATION_XL; return true; }
    return false;
}

bool parse_attach_token(const char* chars, size_t len, EtchAttachTo* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "none")) { *out = ETCH_ATTACH_NONE; return true; }
    if (inlay_ui_str_eq(chars, len, "parent")) { *out = ETCH_ATTACH_PARENT; return true; }
    if (inlay_ui_str_eq(chars, len, "root")) { *out = ETCH_ATTACH_ROOT; return true; }
    if (inlay_ui_str_eq(chars, len, "element")) { *out = ETCH_ATTACH_ELEMENT; return true; }
    return false;
}

bool parse_anchor_token(const char* chars, size_t len, EtchAnchor* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "top-left") || inlay_ui_str_eq(chars, len, "topLeft")) {
        *out = ETCH_ANCHOR_TOP_LEFT; return true;
    }
    if (inlay_ui_str_eq(chars, len, "top-center") || inlay_ui_str_eq(chars, len, "topCenter")) {
        *out = ETCH_ANCHOR_TOP_CENTER; return true;
    }
    if (inlay_ui_str_eq(chars, len, "top-right") || inlay_ui_str_eq(chars, len, "topRight")) {
        *out = ETCH_ANCHOR_TOP_RIGHT; return true;
    }
    if (inlay_ui_str_eq(chars, len, "center-left") || inlay_ui_str_eq(chars, len, "centerLeft")) {
        *out = ETCH_ANCHOR_CENTER_LEFT; return true;
    }
    if (inlay_ui_str_eq(chars, len, "center")) { *out = ETCH_ANCHOR_CENTER; return true; }
    if (inlay_ui_str_eq(chars, len, "center-right") || inlay_ui_str_eq(chars, len, "centerRight")) {
        *out = ETCH_ANCHOR_CENTER_RIGHT; return true;
    }
    if (inlay_ui_str_eq(chars, len, "bottom-left") || inlay_ui_str_eq(chars, len, "bottomLeft")) {
        *out = ETCH_ANCHOR_BOTTOM_LEFT; return true;
    }
    if (inlay_ui_str_eq(chars, len, "bottom-center") || inlay_ui_str_eq(chars, len, "bottomCenter")) {
        *out = ETCH_ANCHOR_BOTTOM_CENTER; return true;
    }
    if (inlay_ui_str_eq(chars, len, "bottom-right") || inlay_ui_str_eq(chars, len, "bottomRight")) {
        *out = ETCH_ANCHOR_BOTTOM_RIGHT; return true;
    }
    return false;
}

bool parse_cursor_token(const char* chars, size_t len, EtchCursor* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "arrow") || inlay_ui_str_eq(chars, len, "default")) {
        *out = ETCH_CURSOR_ARROW; return true;
    }
    if (inlay_ui_str_eq(chars, len, "hand") || inlay_ui_str_eq(chars, len, "pointer")) {
        *out = ETCH_CURSOR_HAND; return true;
    }
    if (inlay_ui_str_eq(chars, len, "ibeam") || inlay_ui_str_eq(chars, len, "text")) {
        *out = ETCH_CURSOR_IBEAM; return true;
    }
    if (inlay_ui_str_eq(chars, len, "ew-resize") || inlay_ui_str_eq(chars, len, "col-resize") ||
        inlay_ui_str_eq(chars, len, "resize-ew")) {
        *out = ETCH_CURSOR_RESIZE_EW; return true;
    }
    if (inlay_ui_str_eq(chars, len, "ns-resize") || inlay_ui_str_eq(chars, len, "row-resize") ||
        inlay_ui_str_eq(chars, len, "resize-ns")) {
        *out = ETCH_CURSOR_RESIZE_NS; return true;
    }
    if (inlay_ui_str_eq(chars, len, "nwse-resize") || inlay_ui_str_eq(chars, len, "resize-nwse")) {
        *out = ETCH_CURSOR_RESIZE_NWSE; return true;
    }
    if (inlay_ui_str_eq(chars, len, "nesw-resize") || inlay_ui_str_eq(chars, len, "resize-nesw")) {
        *out = ETCH_CURSOR_RESIZE_NESW; return true;
    }
    if (inlay_ui_str_eq(chars, len, "move") || inlay_ui_str_eq(chars, len, "grab") ||
        inlay_ui_str_eq(chars, len, "grabbing")) {
        *out = ETCH_CURSOR_MOVE; return true;
    }
    if (inlay_ui_str_eq(chars, len, "not-allowed")) {
        *out = ETCH_CURSOR_NOT_ALLOWED; return true;
    }
    if (inlay_ui_str_eq(chars, len, "crosshair")) {
        *out = ETCH_CURSOR_CROSSHAIR; return true;
    }
    return false;
}

bool parse_font_weight_token(const char* chars, size_t len, uint16_t* out) noexcept {
    if (inlay_ui_str_eq(chars, len, "bold") || inlay_ui_str_eq(chars, len, "700")) {
        *out = ETCH_FONT_BOLD; return true;
    }
    if (inlay_ui_str_eq(chars, len, "semibold") || inlay_ui_str_eq(chars, len, "semi-bold") ||
        inlay_ui_str_eq(chars, len, "600")) {
        *out = ETCH_FONT_SEMIBOLD; return true;
    }
    if (inlay_ui_str_eq(chars, len, "medium") || inlay_ui_str_eq(chars, len, "500")) {
        *out = ETCH_FONT_MEDIUM; return true;
    }
    if (inlay_ui_str_eq(chars, len, "italic")) { *out = ETCH_FONT_ITALIC; return true; }
    if (inlay_ui_str_eq(chars, len, "mono")) { *out = ETCH_FONT_MONO; return true; }
    if (inlay_ui_str_eq(chars, len, "regular") || inlay_ui_str_eq(chars, len, "normal") ||
        inlay_ui_str_eq(chars, len, "400")) {
        *out = ETCH_FONT_SANS; return true;
    }
    return false;
}

bool parse_sizing_prop(InlaySurface* s, JSValueConst props, const char* name,
                       uint8_t* out_kind, float* out_val, uint16_t* io_flags2, uint16_t explicit_bit) {
    const JSValue v = JS_GetPropertyStr(s->js, props, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    if (JS_IsString(v)) {
        size_t n = 0u;
        const char* chars = JS_ToCStringLen(s->js, &n, v);
        bool ok = false;
        if (chars) {
            if (inlay_ui_str_eq(chars, n, "grow") || inlay_ui_str_eq(chars, n, "fill")) {
                *out_kind = uint8_t(ETCH_SIZING_GROW);
                *out_val = 0.f;
                *io_flags2 |= explicit_bit;
                ok = true;
            } else if (inlay_ui_str_eq(chars, n, "fit") || inlay_ui_str_eq(chars, n, "auto")) {
                *out_kind = uint8_t(ETCH_SIZING_FIT);
                *out_val = 0.f;
                *io_flags2 |= explicit_bit;
                ok = true;
            } else if (n > 1u && chars[n - 1u] == '%' && n < 32u) {
                char buf[32] = {};
                std::memcpy(buf, chars, n - 1u);
                char* end = nullptr;
                const float pct = std::strtof(buf, &end);
                if (end != buf && *end == '\0' && std::isfinite(pct) && pct >= 0.f && pct <= 100.f) {
                    *out_kind = uint8_t(ETCH_SIZING_PERCENT);
                    *out_val = pct * 0.01f;
                    *io_flags2 |= explicit_bit;
                    ok = true;
                }
            } else if (n > 0u && n < 32u) {
                char buf[32] = {};
                std::memcpy(buf, chars, n);
                char* end = nullptr;
                const float px = std::strtof(buf, &end);
                if (end != buf && *end == '\0' && std::isfinite(px) && px >= -1.f && px <= 16384.f) {
                    if (px < 0.f) {
                        *out_kind = uint8_t(ETCH_SIZING_GROW);
                        *out_val = 0.f;
                        *io_flags2 |= explicit_bit;
                    } else if (px > 0.f) {
                        *out_kind = uint8_t(ETCH_SIZING_FIXED);
                        *out_val = px;
                        *io_flags2 |= explicit_bit;
                    }
                    ok = true;
                }
            }
            JS_FreeCString(s->js, chars);
        }
        JS_FreeValue(s->js, v);
        if (!ok) JS_ThrowTypeError(s->js, "Invalid UI sizing property: %s", name);
        return ok;
    }
    float num = 0.f;
    const bool ok = read_float_val(s, v, name, &num, -1.0, 16384.0);
    JS_FreeValue(s->js, v);
    if (!ok) return false;
    if (num < 0.f) {
        *out_kind = uint8_t(ETCH_SIZING_GROW);
        *out_val = 0.f;
        *io_flags2 |= explicit_bit;
    } else if (num > 0.f) {
        *out_kind = uint8_t(ETCH_SIZING_FIXED);
        *out_val = num;
        *io_flags2 |= explicit_bit;
    }
    return true;
}

void parse_sizing_limits(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    float v = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "minWidth", &v, false, 0.0) ||
        inlay_ui_opt_number_prop(s, props, "minW", &v, false, 0.0)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) c->w_min = v;
    }
    v = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "maxWidth", &v, false, 0.0) ||
        inlay_ui_opt_number_prop(s, props, "maxW", &v, false, 0.0)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) c->w_max = v;
    }
    v = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "minHeight", &v, false, 0.0) ||
        inlay_ui_opt_number_prop(s, props, "minH", &v, false, 0.0)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) c->h_min = v;
    }
    v = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "maxHeight", &v, false, 0.0) ||
        inlay_ui_opt_number_prop(s, props, "maxH", &v, false, 0.0)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) c->h_max = v;
    }
}

bool parse_single_radius_val(InlaySurface* s, JSValueConst v, float* out_px, uint8_t* out_tok) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    if (JS_IsString(v)) {
        size_t n = 0u;
        const char* chars = JS_ToCStringLen(s->js, &n, v);
        bool ok = false;
        if (chars) {
            if (inlay_ui_str_eq(chars, n, "panel")) { *out_tok = RadPanel; ok = true; }
            else if (inlay_ui_str_eq(chars, n, "control")) { *out_tok = RadControl; ok = true; }
            else if (inlay_ui_str_eq(chars, n, "input")) { *out_tok = RadInput; ok = true; }
            else if (inlay_ui_str_eq(chars, n, "small")) { *out_tok = RadSmall; ok = true; }
            else if (inlay_ui_str_eq(chars, n, "pill") || inlay_ui_str_eq(chars, n, "full")) {
                *out_tok = RadPill; ok = true;
            } else if (n > 0u && n < 32u) {
                char buf[32] = {};
                std::memcpy(buf, chars, n);
                char* end = nullptr;
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
        *out_px = static_cast<float>(val);
        return true;
    }
    return false;
}

void parse_radius_prop(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    const JSValue v = JS_GetPropertyStr(s->js, props, "radius");
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return;
    }
    if (JS_IsArray(v)) {
        EtchCorners vals = {};
        uint8_t toks[4] = {};
        float* dst[4] = {&vals.tl, &vals.tr, &vals.br, &vals.bl};
        bool any = false;
        for (uint32_t i = 0u; i < 4u; ++i) {
            const JSValue el = JS_GetPropertyUint32(s->js, v, i);
            any |= parse_single_radius_val(s, el, dst[i], &toks[i]);
            JS_FreeValue(s->js, el);
        }
        if (any) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->radius_vals = vals;
                for (uint32_t i = 0u; i < 4u; ++i) c->radius_tok[i] = toks[i];
                c->cflags |= CfPerCornerRadius;
                st.flags |= NfRadiusSet;
            }
        }
        JS_FreeValue(s->js, v);
        return;
    }
    if (JS_IsObject(v)) {
        static const char* const keys[4] = {"tl", "tr", "br", "bl"};
        EtchCorners vals = {};
        uint8_t toks[4] = {};
        float* dst[4] = {&vals.tl, &vals.tr, &vals.br, &vals.bl};
        bool any = false;
        for (uint32_t i = 0u; i < 4u; ++i) {
            const JSValue el = JS_GetPropertyStr(s->js, v, keys[i]);
            any |= parse_single_radius_val(s, el, dst[i], &toks[i]);
            JS_FreeValue(s->js, el);
        }
        if (any) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->radius_vals = vals;
                for (uint32_t i = 0u; i < 4u; ++i) c->radius_tok[i] = toks[i];
                c->cflags |= CfPerCornerRadius;
                st.flags |= NfRadiusSet;
            }
        }
        JS_FreeValue(s->js, v);
        return;
    }
    float px = 0.f;
    uint8_t tok = RadPx;
    if (parse_single_radius_val(s, v, &px, &tok)) {
        st.radius_px = px;
        st.radius_tok = tok;
        st.flags |= NfRadiusSet;
        if (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count) {
            s->cold_visuals[st.cold_idx].cflags &= ~CfPerCornerRadius;
        }
    }
    JS_FreeValue(s->js, v);
}

bool parse_edges_prop(InlaySurface* s, JSValueConst props, const char* base,
                      const char* kx, const char* ky, const char* kt, const char* kr,
                      const char* kb, const char* kl, EtchPadding* out, bool allow_neg) {
    bool set = false;
    const double lo = allow_neg ? -16384.0 : -1.0;
    const auto clamp_e = [&](float x) noexcept { return allow_neg ? x : std::fmax(0.f, x); };
    const JSValue v = JS_GetPropertyStr(s->js, props, base);
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_IsArray(v)) {
            const JSValue lv = JS_GetPropertyStr(s->js, v, "length");
            uint32_t len = 0u;
            JS_ToUint32(s->js, &len, lv);
            JS_FreeValue(s->js, lv);
            float a[4] = {};
            const uint32_t take = len < 4u ? len : 4u;
            bool ok = take > 0u;
            for (uint32_t i = 0u; i < take && ok; ++i) {
                const JSValue el = JS_GetPropertyUint32(s->js, v, i);
                ok = read_float_val(s, el, base, &a[i], lo, 16384.0);
                JS_FreeValue(s->js, el);
            }
            if (ok) {
                if (take == 1u) {
                    const float p = clamp_e(a[0]);
                    *out = {p, p, p, p};
                    set = true;
                } else if (take == 2u) {
                    const float pv = clamp_e(a[0]);
                    const float ph = clamp_e(a[1]);
                    *out = {ph, ph, pv, pv};
                    set = true;
                } else if (take == 3u) {
                    const float pt = clamp_e(a[0]);
                    const float ph = clamp_e(a[1]);
                    const float pb = clamp_e(a[2]);
                    *out = {ph, ph, pt, pb};
                    set = true;
                } else if (take == 4u) {
                    *out = {clamp_e(a[3]), clamp_e(a[1]), clamp_e(a[0]), clamp_e(a[2])};
                    set = true;
                }
            }
        } else if (JS_IsObject(v)) {
            float tmp = 0.f;
            if (inlay_ui_opt_number_prop(s, v, "x", &tmp, false, lo)) { out->left = out->right = clamp_e(tmp); set = true; }
            if (inlay_ui_opt_number_prop(s, v, "y", &tmp, false, lo)) { out->top = out->bottom = clamp_e(tmp); set = true; }
            if (inlay_ui_opt_number_prop(s, v, "left", &tmp, false, lo)) { out->left = clamp_e(tmp); set = true; }
            if (inlay_ui_opt_number_prop(s, v, "right", &tmp, false, lo)) { out->right = clamp_e(tmp); set = true; }
            if (inlay_ui_opt_number_prop(s, v, "top", &tmp, false, lo)) { out->top = clamp_e(tmp); set = true; }
            if (inlay_ui_opt_number_prop(s, v, "bottom", &tmp, false, lo)) { out->bottom = clamp_e(tmp); set = true; }
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
    if (inlay_ui_opt_number_prop(s, props, kx, &tmp, false, lo)) { out->left = out->right = clamp_e(tmp); set = true; }
    if (inlay_ui_opt_number_prop(s, props, ky, &tmp, false, lo)) { out->top = out->bottom = clamp_e(tmp); set = true; }
    if (inlay_ui_opt_number_prop(s, props, kt, &tmp, false, lo)) { out->top = clamp_e(tmp); set = true; }
    if (inlay_ui_opt_number_prop(s, props, kr, &tmp, false, lo)) { out->right = clamp_e(tmp); set = true; }
    if (inlay_ui_opt_number_prop(s, props, kb, &tmp, false, lo)) { out->bottom = clamp_e(tmp); set = true; }
    if (inlay_ui_opt_number_prop(s, props, kl, &tmp, false, lo)) { out->left = clamp_e(tmp); set = true; }
    return set;
}

bool parse_shadow_obj(InlaySurface* s, JSValueConst obj, bool force_inset, ShadowSpec* out) {
    if (!JS_IsObject(obj) || JS_IsNull(obj) || JS_IsArray(obj)) return false;
    *out = {};
    inlay_ui_opt_number_prop(s, obj, "x", &out->x, false, -16384.0, 16384.0);
    out->has_y = inlay_ui_opt_number_prop(s, obj, "y", &out->y, false, -16384.0, 16384.0);
    out->has_blur = inlay_ui_opt_number_prop(s, obj, "blur", &out->blur, false, 0.0, 16384.0);
    inlay_ui_opt_number_prop(s, obj, "spread", &out->spread, false, -16384.0, 16384.0);
    parse_color_prop(s, obj, "color", &out->color);
    out->inset = force_inset ? true : inlay_ui_bool_prop(s, obj, "inset");
    return true;
}

void parse_shadow_props(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    ShadowSpec local[4] = {};
    uint8_t count = 0u;

    const JSValue sh_arr = JS_GetPropertyStr(s->js, props, "shadows");
    if (JS_IsArray(sh_arr)) {
        const JSValue lv = JS_GetPropertyStr(s->js, sh_arr, "length");
        uint32_t len = 0u;
        JS_ToUint32(s->js, &len, lv);
        JS_FreeValue(s->js, lv);
        if (len > 4u) len = 4u;
        for (uint32_t i = 0u; i < len && count < 4u && !JS_HasException(s->js); ++i) {
            const JSValue el = JS_GetPropertyUint32(s->js, sh_arr, i);
            if (parse_shadow_obj(s, el, false, &local[count])) ++count;
            JS_FreeValue(s->js, el);
        }
    }
    JS_FreeValue(s->js, sh_arr);

    if (count == 0u) {
        const JSValue sh = JS_GetPropertyStr(s->js, props, "shadow");
        if (JS_IsString(sh)) {
            size_t n = 0u;
            const char* ch = JS_ToCStringLen(s->js, &n, sh);
            EtchElevation elev = ETCH_ELEVATION_NONE;
            if (ch && parse_elevation_token(ch, n, &elev)) {
                if (auto* c = inlay_ui_ensure_cold(s, st)) {
                    c->elevation = elev;
                    c->cflags |= CfHasElevation;
                }
            }
            if (ch) JS_FreeCString(s->js, ch);
        } else if (parse_shadow_obj(s, sh, false, &local[0])) {
            count = 1u;
        }
        JS_FreeValue(s->js, sh);
    }

    const JSValue inset_sh = JS_GetPropertyStr(s->js, props, "insetShadow");
    if (!JS_IsUndefined(inset_sh) && !JS_IsNull(inset_sh)) {
        if (JS_IsBool(inset_sh)) {
            if (JS_ToBool(s->js, inset_sh) > 0) {
                if (auto* c = inlay_ui_ensure_cold(s, st)) c->cflags |= CfInsetShadowBool;
            }
        } else if (JS_IsObject(inset_sh) && !JS_IsArray(inset_sh)) {
            ShadowSpec ispec = {};
            if (parse_shadow_obj(s, inset_sh, true, &ispec)) {
                if (!ispec.has_blur) ispec.blur = 8.f;
                if (!ispec.has_y) ispec.y = 1.f;
                if (count < 4u) local[count++] = ispec;
                else local[3] = ispec;
            }
        }
    }
    JS_FreeValue(s->js, inset_sh);

    if (count > 0u) {
        const uint32_t off = inlay_ui_alloc_pool(s, s->shadow_pool, s->shadow_pool_count,
                                                 s->shadow_pool_cap, count, s->desc.max_nodes * 4u);
        if (off != INLAY_UI_NONE) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                for (uint8_t i = 0u; i < count; ++i) s->shadow_pool[off + i] = local[i];
                c->shadow_offset = off;
                c->shadow_count = count;
            }
        }
    }
}

void parse_gradient_prop(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    const JSValue gval = JS_GetPropertyStr(s->js, props, "gradient");
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
    peek_str_prop(s, gval, "kind", [&](const char* c, size_t n) noexcept {
        if (inlay_ui_str_eq(c, n, "linear")) g.kind = ETCH_GRADIENT_LINEAR;
        else if (inlay_ui_str_eq(c, n, "radial")) g.kind = ETCH_GRADIENT_RADIAL;
        else if (inlay_ui_str_eq(c, n, "conic")) g.kind = ETCH_GRADIENT_CONIC;
        else if (inlay_ui_str_eq(c, n, "none")) g.kind = ETCH_GRADIENT_NONE;
        return true;
    });
    inlay_ui_opt_bool_prop(s, gval, "oklab", &g.oklab);

    bool has_dir = false;
    const JSValue dir_v = JS_GetPropertyStr(s->js, gval, "dir");
    if (JS_IsArray(dir_v)) {
        has_dir = read_vec2_array(s, dir_v, "gradient.dir", &g.dir_x, &g.dir_y, -16384.0, 16384.0);
    }
    JS_FreeValue(s->js, dir_v);

    float angle_deg = 0.f;
    if (inlay_ui_opt_number_prop(s, gval, "angleDeg", &angle_deg, false, -36000.0, 36000.0)) {
        g.angle_rad = angle_deg * INLAY_UI_DEG_TO_RAD;
        if (!has_dir) {
            g.dir_x = std::cos(g.angle_rad);
            g.dir_y = std::sin(g.angle_rad);
        }
    } else if (inlay_ui_opt_number_prop(s, gval, "angleRad", &g.angle_rad, false, -1000.0, 1000.0)) {
        if (!has_dir) {
            g.dir_x = std::cos(g.angle_rad);
            g.dir_y = std::sin(g.angle_rad);
        }
    }

    const JSValue ctr_v = JS_GetPropertyStr(s->js, gval, "center");
    if (JS_IsArray(ctr_v)) {
        read_vec2_array(s, ctr_v, "gradient.center", &g.center_x, &g.center_y, -16.0, 16.0);
    }
    JS_FreeValue(s->js, ctr_v);

    const JSValue stops_v = JS_GetPropertyStr(s->js, gval, "stops");
    if (JS_IsArray(stops_v)) {
        const JSValue lv = JS_GetPropertyStr(s->js, stops_v, "length");
        uint32_t len = 0u;
        JS_ToUint32(s->js, &len, lv);
        JS_FreeValue(s->js, lv);
        if (len > 8u) len = 8u;
        bool has_pos[8] = {};
        for (uint32_t i = 0u; i < len && !JS_HasException(s->js); ++i) {
            const JSValue sv = JS_GetPropertyUint32(s->js, stops_v, i);
            if (JS_IsString(sv)) {
                size_t n = 0u;
                const char* ch = JS_ToCStringLen(s->js, &n, sv);
                if (ch) {
                    g.stop_colors[g.stop_count] = inlay_ui_parse_color(ch, n);
                    JS_FreeCString(s->js, ch);
                }
                ++g.stop_count;
            } else if (JS_IsObject(sv) && !JS_IsNull(sv)) {
                parse_color_prop(s, sv, "color", &g.stop_colors[g.stop_count]);
                float p = 0.f;
                if (inlay_ui_opt_number_prop(s, sv, "pos", &p, false, 0.0, 1.0) ||
                    inlay_ui_opt_number_prop(s, sv, "position", &p, false, 0.0, 1.0)) {
                    g.stop_pos[g.stop_count] = std::fmin(1.f, std::fmax(0.f, p));
                    has_pos[g.stop_count] = true;
                }
                ++g.stop_count;
            }
            JS_FreeValue(s->js, sv);
        }
        for (uint8_t i = 0u; i < g.stop_count; ++i) {
            if (!has_pos[i]) {
                g.stop_pos[i] = g.stop_count > 1u ? float(i) / float(g.stop_count - 1u) : 0.f;
            }
        }
    }
    JS_FreeValue(s->js, stops_v);
    JS_FreeValue(s->js, gval);

    if (g.kind != ETCH_GRADIENT_NONE && g.stop_count >= 2u) {
        const uint32_t idx = inlay_ui_alloc_pool(s, s->gradient_pool, s->gradient_pool_count,
                                                 s->gradient_pool_cap, 1u, s->desc.max_nodes);
        if (idx != INLAY_UI_NONE) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                s->gradient_pool[idx] = g;
                c->gradient_idx = idx;
            }
        }
    }
}

void parse_transform_props(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    const ColdVisualStyle* existing = inlay_ui_get_cold(s, st);
    TransformSpec tf = (existing && (existing->cflags & CfHasTransform))
                           ? existing->transform
                           : TransformSpec{0.f, 0.f, 1.f, 1.f, 0.f};
    bool any = false;

    float rot = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "rotate", &rot, false, -36000.0, 36000.0)) {
        tf.rotation_rad = rot * INLAY_UI_DEG_TO_RAD;
        any = true;
    } else if (inlay_ui_opt_number_prop(s, props, "rotateRad", &rot, false, -1000.0, 1000.0)) {
        tf.rotation_rad = rot;
        any = true;
    }

    const JSValue sc_v = JS_GetPropertyStr(s->js, props, "scale");
    if (!JS_IsUndefined(sc_v) && !JS_IsNull(sc_v)) {
        if (JS_IsArray(sc_v)) {
            if (read_vec2_array(s, sc_v, "scale", &tf.sx, &tf.sy, -100.0, 100.0)) any = true;
        } else {
            float sc = 1.f;
            if (read_float_val(s, sc_v, "scale", &sc, -100.0, 100.0)) {
                tf.sx = tf.sy = sc;
                any = true;
            }
        }
    }
    JS_FreeValue(s->js, sc_v);

    if (inlay_ui_opt_number_prop(s, props, "scaleX", &tf.sx, false, -100.0, 100.0)) any = true;
    if (inlay_ui_opt_number_prop(s, props, "scaleY", &tf.sy, false, -100.0, 100.0)) any = true;

    const JSValue tr_v = JS_GetPropertyStr(s->js, props, "translate");
    if (JS_IsArray(tr_v)) {
        if (read_vec2_array(s, tr_v, "translate", &tf.tx, &tf.ty, -16384.0, 16384.0)) any = true;
    }
    JS_FreeValue(s->js, tr_v);

    if (inlay_ui_opt_number_prop(s, props, "translateX", &tf.tx, false, -16384.0, 16384.0)) any = true;
    if (inlay_ui_opt_number_prop(s, props, "translateY", &tf.ty, false, -16384.0, 16384.0)) any = true;

    if (!any) return;

    // Bug 1 fix: clamp scale to [0.001, 100.0] so negative/zero scale never wipes EtchLayerDesc.
    tf.sx = std::fmin(100.f, std::fmax(0.001f, tf.sx));
    tf.sy = std::fmin(100.f, std::fmax(0.001f, tf.sy));

    const bool non_identity =
        std::fabs(tf.tx) > 1e-4f || std::fabs(tf.ty) > 1e-4f ||
        std::fabs(tf.sx - 1.f) > 1e-4f || std::fabs(tf.sy - 1.f) > 1e-4f ||
        std::fabs(tf.rotation_rad) > 1e-5f;
    if (non_identity || (existing && (existing->cflags & CfHasTransform))) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) {
            c->transform = tf;
            if (non_identity) c->cflags |= CfHasTransform;
            else c->cflags &= ~CfHasTransform;
        }
    }
}

void parse_compositing_and_transition_props(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    // transition: string ("all 200ms ease-out"), boolean (true), or object
    const JSValue tr_v = JS_GetPropertyStr(s->js, props, "transition");
    if (!JS_IsUndefined(tr_v) && !JS_IsNull(tr_v)) {
        TransitionSpec ts = {};
        bool ok = false;
        if (JS_IsString(tr_v)) {
            size_t n = 0u;
            const char* ch = JS_ToCStringLen(s->js, &n, tr_v);
            if (ch) {
                ok = inlay_ui_parse_transition(ch, n, &ts);
                JS_FreeCString(s->js, ch);
            }
        } else if (JS_IsBool(tr_v)) {
            if (JS_ToBool(s->js, tr_v) > 0) {
                ok = inlay_ui_parse_transition("all", 3u, &ts);
            } else {
                ts = {};
                ts.prop_mask = TransNone;
                ok = true;
            }
        } else if (JS_IsObject(tr_v) && !JS_IsArray(tr_v)) {
            ok = inlay_ui_parse_transition("all", 3u, &ts);
            peek_str_prop(s, tr_v, "property", [&](const char* c, size_t n) noexcept {
                TransitionSpec tmp = ts;
                if (inlay_ui_parse_transition(c, n, &tmp)) {
                    ts.prop_mask = tmp.prop_mask;
                    return true;
                }
                return false;
            });
            float dur_ms = 0.f;
            if (inlay_ui_opt_number_prop(s, tr_v, "duration", &dur_ms, false, 0.0, 60000.0)) {
                ts.duration_sec = dur_ms > 20.f ? (dur_ms * 0.001f) : dur_ms;
            }
            float del_ms = 0.f;
            if (inlay_ui_opt_number_prop(s, tr_v, "delay", &del_ms, false, 0.0, 60000.0)) {
                ts.delay_sec = del_ms > 20.f ? (del_ms * 0.001f) : del_ms;
            }
            peek_str_prop(s, tr_v, "easing", [&](const char* c, size_t n) noexcept {
                TransitionSpec tmp = {};
                if (inlay_ui_parse_transition(c, n, &tmp)) {
                    ts.easing = tmp.easing;
                    for (int k = 0; k < 4; ++k) ts.bezier[k] = tmp.bezier[k];
                    return true;
                }
                return false;
            });
        }
        if (ok) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->transition = ts;
                c->cflags |= CfHasTransition;
            }
        }
    }
    JS_FreeValue(s->js, tr_v);

    // blendMode (Etch 0.28)
    peek_str_prop(s, props, "blendMode", [&](const char* c, size_t n) {
        EtchBlendMode bm = ETCH_BLEND_NORMAL;
        if (!inlay_ui_parse_blend_mode(c, n, &bm)) return false;
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->blend_mode = bm;
            if (bm != ETCH_BLEND_NORMAL) cold->cflags |= CfHasBlendMode;
            else cold->cflags &= ~CfHasBlendMode;
        }
        return true;
    });

    // alphaMask (Etch 0.28)
    const JSValue am_v = JS_GetPropertyStr(s->js, props, "alphaMask");
    if (!JS_IsUndefined(am_v) && !JS_IsNull(am_v)) {
        EtchAlphaMaskDesc mask = etch_alpha_mask_desc_default();
        if (inlay_ui_parse_alpha_mask(s, am_v, &mask)) {
            if (auto* cold = inlay_ui_ensure_cold(s, st)) {
                cold->alpha_mask = mask;
                if (mask.kind != ETCH_ALPHA_MASK_NONE) cold->cflags |= CfHasAlphaMask;
                else cold->cflags &= ~CfHasAlphaMask;
            }
        }
    }
    JS_FreeValue(s->js, am_v);

    // cursor
    peek_str_prop(s, props, "cursor", [&](const char* c, size_t n) {
        EtchCursor cur = ETCH_CURSOR_ARROW;
        if (!parse_cursor_token(c, n, &cur)) return false;
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->cursor = cur;
            cold->cflags |= CfHasCursor;
        }
        return true;
    });
}

void parse_opentype_props(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    const ColdVisualStyle* existing = inlay_ui_get_cold(s, st);
    uint32_t ot = (existing && (existing->cflags & CfHasOpenType)) ? existing->opentype_flags : 0u;
    bool touched = false;

    peek_str_prop(s, props, "opentype", [&](const char* c, size_t n) noexcept {
        if (inlay_ui_parse_opentype_flags(c, n, &ot)) {
            touched = true;
            return true;
        }
        return false;
    });
    peek_str_prop(s, props, "fontFeatures", [&](const char* c, size_t n) noexcept {
        if (inlay_ui_parse_opentype_flags(c, n, &ot)) {
            touched = true;
            return true;
        }
        return false;
    });
    bool flag = false;
    if (inlay_ui_opt_bool_prop(s, props, "tabularNums", &flag)) {
        if (flag) ot |= ETCH_OT_TABULAR_NUMS;
        else ot &= ~uint32_t(ETCH_OT_TABULAR_NUMS);
        touched = true;
    }
    if (inlay_ui_opt_bool_prop(s, props, "noLiga", &flag)) {
        if (flag) ot |= ETCH_OT_NO_LIGA;
        else ot &= ~uint32_t(ETCH_OT_NO_LIGA);
        touched = true;
    }
    if (inlay_ui_opt_bool_prop(s, props, "slashedZero", &flag)) {
        if (flag) ot |= ETCH_OT_SLASHED_ZERO;
        else ot &= ~uint32_t(ETCH_OT_SLASHED_ZERO);
        touched = true;
    }
    if (inlay_ui_opt_bool_prop(s, props, "smallCaps", &flag)) {
        if (flag) ot |= ETCH_OT_SMALL_CAPS;
        else ot &= ~uint32_t(ETCH_OT_SMALL_CAPS);
        touched = true;
    }
    if (touched) {
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->opentype_flags = ot;
            cold->cflags |= CfHasOpenType;
        }
    }
}

void parse_items_prop(InlaySurface* s, JSValueConst props, uint32_t max_items,
                      NodeStyle& st, uint32_t node_index, bool support_disabled_objs) {
    const JSValue arr = JS_GetPropertyStr(s->js, props, "items");
    if (JS_IsArray(arr)) {
        const JSValue lv = JS_GetPropertyStr(s->js, arr, "length");
        uint32_t count = 0u;
        JS_ToUint32(s->js, &count, lv);
        JS_FreeValue(s->js, lv);
        if (count > max_items) count = max_items;
        if (count > 0u) {
            const uint32_t hard_cap = s->desc.max_nodes * 16u > 65536u ? 65536u : s->desc.max_nodes * 16u;
            const uint32_t off = inlay_ui_alloc_pool(s, s->item_pool, s->item_pool_count,
                                                     s->item_pool_cap, count, hard_cap);
            if (off != INLAY_UI_NONE) {
                std::memset(s->item_pool + off, 0, size_t(count) * sizeof(EtchStr));
                uint32_t written = 0u;
                uint16_t disabled_mask = 0u;
                for (uint32_t i = 0u; i < count && !JS_HasException(s->js); ++i) {
                    const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                    EtchStr label = {};
                    if (support_disabled_objs && JS_IsObject(el) && !JS_IsNull(el)) {
                        label = inlay_ui_string_prop(s, el, "label");
                        if (!label.length && !JS_HasException(s->js)) {
                            label = inlay_ui_string_prop(s, el, "text");
                        }
                        if (!JS_HasException(s->js) && inlay_ui_bool_prop(s, el, "disabled") && i < 16u) {
                            disabled_mask |= uint16_t(1u << i);
                        }
                    } else {
                        label = inlay_ui_copy_text(s, el);
                    }
                    s->item_pool[off + written++] = label;
                    JS_FreeValue(s->js, el);
                }
                if (auto* cold = inlay_ui_ensure_cold(s, st)) {
                    cold->item_offset = off;
                    st.item_count = static_cast<uint8_t>(written);
                }
                if (disabled_mask != 0u) {
                    if (auto* cx = ensure_core_extra(s, node_index)) {
                        cx->tabs_disabled = disabled_mask;
                    }
                }
            }
        }
    }
    JS_FreeValue(s->js, arr);
}

void parse_font_props(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    uint16_t fid = ETCH_FONT_SANS;
    if (peek_str_prop(s, props, "fontWeight", [&](const char* c, size_t n) noexcept {
            return parse_font_weight_token(c, n, &fid);
        })) {
        st.font_id = fid;
        st.flags |= NfHasFontId;
    }
    if (inlay_ui_bool_prop(s, props, "bold")) {
        st.font_id = ETCH_FONT_BOLD;
        st.flags |= NfHasFontId;
    }
    if (inlay_ui_bool_prop(s, props, "italic")) {
        st.font_id = ETCH_FONT_ITALIC;
        st.flags |= NfHasFontId;
    }
    if (inlay_ui_bool_prop(s, props, "mono")) {
        st.font_id = ETCH_FONT_MONO;
        st.flags |= NfHasFontId;
    }
}

void parse_clickable_events(InlaySurface* s, JSValueConst props, NodeStyle& st, uint32_t node_index) {
    bool clickable = has_event(st.action, st.handler);
    if (!clickable) {
        if (inlay_ui_parse_event_prop(s, props, "onClick", &st.action, &st.handler) ||
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler)) {
            clickable = true;
        }
    }
    TextSlice dbl_act = {};
    uint32_t dbl_h = INLAY_UI_NONE;
    if (!JS_HasException(s->js) && inlay_ui_parse_event_prop(s, props, "onDoubleClick", &dbl_act, &dbl_h)) {
        if (auto* cx = ensure_core_extra(s, node_index)) {
            cx->dbl_action = dbl_act;
            cx->dbl_handler = dbl_h;
            clickable = true;
        }
    }
    TextSlice rc_act = {};
    uint32_t rc_h = INLAY_UI_NONE;
    if (!JS_HasException(s->js) && inlay_ui_parse_event_prop(s, props, "onRightClick", &rc_act, &rc_h)) {
        if (auto* cx = ensure_core_extra(s, node_index)) {
            cx->rclick_action = rc_act;
            cx->rclick_handler = rc_h;
            clickable = true;
        }
    }
    TextSlice hov_act = {};
    uint32_t hov_h = INLAY_UI_NONE;
    if (!JS_HasException(s->js) && inlay_ui_parse_event_prop(s, props, "onHover", &hov_act, &hov_h)) {
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->on_hover_action = hov_act;
            cold->on_hover_handler = hov_h;
        }
    }
    TextSlice foc_act = {};
    uint32_t foc_h = INLAY_UI_NONE;
    if (!JS_HasException(s->js) && inlay_ui_parse_event_prop(s, props, "onFocus", &foc_act, &foc_h)) {
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->on_focus_action = foc_act;
            cold->on_focus_handler = foc_h;
        }
    }
    if (clickable) st.flags |= NfClickable;
}

bool has_only_class_style(JSContext* js, JSValueConst props) {
    JSPropertyEnum* names = nullptr;
    uint32_t count = 0u;
    if (JS_GetOwnPropertyNames(js, &names, &count, props, JS_GPN_STRING_MASK) < 0) return false;
    bool only_classes = count <= 6u;
    for (uint32_t i = 0u; i < count && only_classes; ++i) {
        const char* name = JS_AtomToCString(js, names[i].atom);
        only_classes = name && (std::strcmp(name, "axis") == 0 || std::strcmp(name, "cls") == 0 ||
            std::strcmp(name, "className") == 0 || std::strcmp(name, "id") == 0 ||
            std::strcmp(name, "key") == 0 || std::strcmp(name, "text") == 0);
        if (name) JS_FreeCString(js, name);
    }
    JS_FreePropertyEnum(js, names, count);
    return only_classes;
}

void parse_style(InlaySurface* s, JSValueConst props, InlayUiKind kind,
                 NodeStyle& st, uint32_t node_index, bool default_col_hint) {
    inlay_ui_init_node_style(st);
    const bool default_col = default_col_hint ||
        kind == INLAY_UI_SCROLL || kind == INLAY_UI_CARD || kind == INLAY_UI_FORM ||
        kind == INLAY_UI_POPOVER || kind == INLAY_UI_TREE || kind == INLAY_UI_ACCORDION ||
        kind == INLAY_UI_DOC || kind == INLAY_UI_TERMINAL;
    if (default_col) st.flags |= NfColumn;
    // Pre-apply explicit `axis` before `cls` so axis-dependent utilities (`items-*`, `gap-y-*`) resolve accurately.
    peek_str_prop(s, props, "axis", [&](const char* c, size_t n) noexcept {
        if (inlay_ui_str_eq(c, n, "row")) st.flags &= ~NfColumn;
        else if (inlay_ui_str_eq(c, n, "column") || inlay_ui_str_eq(c, n, "col")) st.flags |= NfColumn;
        return true;
    });

    // 1. Utility classes (`cls` / `className`) run FIRST so explicit props override them.
    const bool applied_cls = peek_str_prop(s, props, "cls", [&](const char* c, size_t n) {
        if (n > 0u) inlay_ui_apply_cls(s, c, n, kind, st);
        return n > 0u;
    });
    if (!applied_cls && !JS_HasException(s->js)) {
        peek_str_prop(s, props, "className", [&](const char* c, size_t n) {
            if (n > 0u) inlay_ui_apply_cls(s, c, n, kind, st);
            return n > 0u;
        });
    }
    if (JS_HasException(s->js)) return;
    if ((kind == INLAY_UI_TEXT || kind == INLAY_UI_BOX) && has_only_class_style(s->js, props)) return;

    // 2. Shared opacity, transforms, compositing, transitions & event scoping
    float op = st.opacity;
    if (inlay_ui_opt_number_prop(s, props, "opacity", &op)) {
        st.opacity = std::fmin(1.f, std::fmax(0.f, op));
        st.flags |= NfHasOpacity;
    }
    parse_transform_props(s, props, st);
    parse_compositing_and_transition_props(s, props, st);
    if (inlay_ui_bool_prop(s, props, "stopPropagation")) {
        if (auto* cx = ensure_core_extra(s, node_index)) cx->flags |= CefStopPropagation;
    }

    parse_color_prop(s, props, "color", &st.color);
    if (JS_HasException(s->js)) return;

    if (kind == INLAY_UI_TEXT) {
        float fs = st.font_size;
        if (inlay_ui_opt_number_prop(s, props, "fontSize", &fs)) {
            st.font_size = fs;
            st.flags |= NfHasSize;
        }
        parse_font_props(s, props, st);
        parse_opentype_props(s, props, st);

        bool wrap_v = false;
        if (inlay_ui_opt_bool_prop(s, props, "wrap", &wrap_v)) {
            st.flags |= NfHasWrap;
            if (wrap_v) st.flags |= NfWrapFlag;
            else st.flags &= ~NfWrapFlag;
        }
        float lh = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "lineHeight", &lh, false, 0.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->line_height = lh;
                c->cflags |= CfHasLineHeight;
            }
        }
        float ls = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "letterSpacing", &ls, false, -64.0, 256.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->letter_spacing = ls;
                c->cflags |= CfHasLetterSpacing;
            }
        }
        EtchAlignX ax = ETCH_ALIGN_X_LEFT;
        if (peek_str_prop(s, props, "alignX", [&](const char* c, size_t n) noexcept {
                return parse_align_x(c, n, &ax);
            })) {
            st.align_x = static_cast<uint8_t>(ax);
            st.flags |= NfHasAlignX;
        }
        if (inlay_ui_bool_prop(s, props, "ellipsis")) st.flags |= NfEllipsis;
        uint32_t ml = 0u;
        if (inlay_ui_opt_uint_prop(s, props, "maxLines", 1024u, &ml)) {
            st.max_lines = static_cast<uint16_t>(ml);
        }

        // Hover text color for text transitions
        ColorRef htc = {};
        if (parse_color_prop(s, props, "hoverColor", &htc) ||
            parse_color_prop(s, props, "hoverTextColor", &htc)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->hover_text_color = htc;
        }

        // Selectable text & selectionColor
        if (inlay_ui_bool_prop(s, props, "selectable")) {
            if (auto* cx = ensure_core_extra(s, node_index)) cx->flags |= CefTextSelectable;
        }
        ColorRef sel_col = {};
        if (parse_color_prop(s, props, "selectionColor", &sel_col)) {
            if (auto* cx = ensure_core_extra(s, node_index)) cx->color_a = sel_col;
        }

        // Optional wrapper layout props on ui.text (margin / flexGrow / alignSelf)
        const ColdVisualStyle* existing_c = inlay_ui_get_cold(s, st);
        EtchPadding marg = (existing_c && (existing_c->cflags & CfHasMargin)) ? existing_c->margin : EtchPadding{};
        if (parse_edges_prop(s, props, "margin", "marginX", "marginY",
                             "marginTop", "marginRight", "marginBottom", "marginLeft", &marg, true)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->margin = marg;
                c->cflags |= CfHasMargin;
            }
        }
        float fg_val = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "flexGrow", &fg_val, false, 0.0)) {
            st.flex_grow = fg_val;
            st.flags |= NfHasFlexGrow;
        }
        EtchAlignSelf as = ETCH_ALIGN_SELF_AUTO;
        if (peek_str_prop(s, props, "alignSelf", [&](const char* c, size_t n) noexcept {
                return parse_align_self_token(c, n, &as);
            })) {
            st.align_self = static_cast<uint8_t>(as);
            st.flags |= NfHasAlignSelf;
        }
        return;
    }

    parse_sizing_prop(s, props, "width", &st.w_kind, &st.w_val, &st.flags2, Nf2WExplicit);
    parse_sizing_prop(s, props, "height", &st.h_kind, &st.h_val, &st.flags2, Nf2HExplicit);
    parse_sizing_limits(s, props, st);

    if (!parse_color_prop(s, props, "bg", &st.bg)) {
        parse_color_prop(s, props, "background", &st.bg);
    }
    float fs = st.font_size;
    if (inlay_ui_opt_number_prop(s, props, "fontSize", &fs)) {
        st.font_size = fs;
        st.flags |= NfHasSize;
    }

    const ColdVisualStyle* existing_m = inlay_ui_get_cold(s, st);
    EtchPadding marg = (existing_m && (existing_m->cflags & CfHasMargin)) ? existing_m->margin : EtchPadding{};
    if (parse_edges_prop(s, props, "margin", "marginX", "marginY",
                         "marginTop", "marginRight", "marginBottom", "marginLeft", &marg, true)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) {
            c->margin = marg;
            c->cflags |= CfHasMargin;
        }
    }

    if (kind == INLAY_UI_SPACER) {
        float sp_sz = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "size", &sp_sz, false, 0.0)) {
            st.value = sp_sz;
            st.flags2 |= Nf2HasSpacerSize;
        }
        if (inlay_ui_opt_number_prop(s, props, "flexGrow", &st.flex_grow, false, 0.0)) {
            st.flags |= NfHasFlexGrow;
        }
        if (inlay_ui_opt_number_prop(s, props, "flexShrink", &st.flex_shrink, false, 0.0)) {
            st.flags |= NfHasFlexShrink;
        }
        return;
    }

    if (kind == INLAY_UI_DIVIDER) {
        peek_str_prop(s, props, "axis", [&](const char* c, size_t n) noexcept {
            if (inlay_ui_str_eq(c, n, "column") || inlay_ui_str_eq(c, n, "col")) st.flags |= NfColumn;
            else if (inlay_ui_str_eq(c, n, "row")) st.flags &= ~NfColumn;
            return true;
        });
        return;
    }

    // Shared visual & container styling props
    parse_color_prop(s, props, "borderColor", &st.border_color);
    if (inlay_ui_opt_number_prop(s, props, "borderWidth", &st.border_width)) {
        st.flags |= NfHasBorderWidth;
    }
    {
        const ColdVisualStyle* ec = inlay_ui_get_cold(s, st);
        EtchPadding bs = (ec && (ec->cflags & CfHasBorderSides)) ? ec->border_sides : EtchPadding{};
        bool bs_set = false;
        float side_v = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "borderTop", &side_v)) { bs.top = std::fmax(0.f, side_v); bs_set = true; }
        if (inlay_ui_opt_number_prop(s, props, "borderRight", &side_v)) { bs.right = std::fmax(0.f, side_v); bs_set = true; }
        if (inlay_ui_opt_number_prop(s, props, "borderBottom", &side_v)) { bs.bottom = std::fmax(0.f, side_v); bs_set = true; }
        if (inlay_ui_opt_number_prop(s, props, "borderLeft", &side_v)) { bs.left = std::fmax(0.f, side_v); bs_set = true; }
        if (bs_set) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->border_sides = bs;
                c->cflags |= CfHasBorderSides;
            }
        }
    }
    parse_radius_prop(s, props, st);
    bool cont_c = false;
    if (inlay_ui_opt_bool_prop(s, props, "continuousCorners", &cont_c)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) {
            c->cflags |= CfHasContinuousCorners;
            if (cont_c) c->cflags |= CfContinuousCorners;
            else c->cflags &= ~CfContinuousCorners;
        }
    }

    if (parse_edges_prop(s, props, "padding", "paddingX", "paddingY",
                         "paddingTop", "paddingRight", "paddingBottom", "paddingLeft", &st.padding, false)) {
        st.flags |= NfHasPadding;
    }
    if (inlay_ui_opt_number_prop(s, props, "gap", &st.gap, false, 0.0)) st.flags |= NfHasGap;
    float rg = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "rowGap", &rg, false, 0.0)) {
        if (auto* c = inlay_ui_ensure_cold(s, st)) {
            c->row_gap = rg;
            c->cflags |= CfHasRowGap;
        }
    }

    EtchAlignX ax = ETCH_ALIGN_X_LEFT;
    if (peek_str_prop(s, props, "alignX", [&](const char* c, size_t n) noexcept {
            return parse_align_x(c, n, &ax);
        })) {
        st.align_x = static_cast<uint8_t>(ax);
        st.flags |= NfHasAlignX;
    }
    EtchJustify jv = ETCH_JUSTIFY_START;
    if (peek_str_prop(s, props, "justify", [&](const char* c, size_t n) noexcept {
            return parse_justify_token(c, n, &jv, false);
        })) {
        st.justify = static_cast<uint8_t>(jv);
        st.flags |= NfHasJustify;
    }
    peek_str_prop(s, props, "justifyContent", [&](const char* c, size_t n) noexcept {
        EtchJustify j2 = ETCH_JUSTIFY_START;
        if (parse_justify_token(c, n, &j2, true)) {
            if ((st.flags & NfHasJustify) == 0u) {
                st.justify = static_cast<uint8_t>(j2);
                st.flags |= NfHasJustify;
            }
            return true;
        }
        EtchAlignX ax2 = ETCH_ALIGN_X_LEFT;
        if ((st.flags & NfHasAlignX) == 0u && parse_align_x(c, n, &ax2)) {
            st.align_x = static_cast<uint8_t>(ax2);
            st.flags |= NfHasAlignX;
            return true;
        }
        return false;
    });
    EtchAlignY ay = ETCH_ALIGN_Y_TOP;
    if (peek_str_prop(s, props, "alignY", [&](const char* c, size_t n) noexcept {
            return parse_align_y(c, n, &ay);
        }) || peek_str_prop(s, props, "alignItems", [&](const char* c, size_t n) noexcept {
            return parse_align_y(c, n, &ay);
        })) {
        st.align_y = static_cast<uint8_t>(ay);
        st.flags |= NfHasAlignY;
    }
    EtchAlignSelf as = ETCH_ALIGN_SELF_AUTO;
    if (peek_str_prop(s, props, "alignSelf", [&](const char* c, size_t n) noexcept {
            return parse_align_self_token(c, n, &as);
        })) {
        st.align_self = static_cast<uint8_t>(as);
        st.flags |= NfHasAlignSelf;
    }

    peek_str_prop(s, props, "axis", [&](const char* c, size_t n) noexcept {
        if (inlay_ui_str_eq(c, n, "row")) st.flags &= ~NfColumn;
        else if (inlay_ui_str_eq(c, n, "column") || inlay_ui_str_eq(c, n, "col")) st.flags |= NfColumn;
        return true;
    });

    // Visual craft, state overrides & layout props for containers, buttons, widgets, and extended widgets
    if (kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_FORM || kind == INLAY_UI_SCROLL ||
        kind == INLAY_UI_POPOVER || kind == INLAY_UI_BADGE || kind == INLAY_UI_BUTTON ||
        kind == INLAY_UI_SLOT || kind == INLAY_UI_TABS || kind == INLAY_UI_MATH ||
        kind == INLAY_UI_DIAGRAM || kind == INLAY_UI_IMAGE || kind == INLAY_UI_MODAL ||
        kind == INLAY_UI_INPUT || kind == INLAY_UI_COMBOBOX || inlay_ui_is_ext_widget(kind)) {
        EtchElevation elev = ETCH_ELEVATION_NONE;
        if (peek_str_prop(s, props, "elevation", [&](const char* c, size_t n) noexcept {
                return parse_elevation_token(c, n, &elev);
            })) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->elevation = elev;
                c->cflags |= CfHasElevation;
            }
        }
        parse_shadow_props(s, props, st);
        parse_gradient_prop(s, props, st);

        float ftmp = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "backdropBlur", &ftmp, false, 0.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->backdrop_blur = ftmp;
        }
        if (inlay_ui_opt_number_prop(s, props, "backdropSaturation", &ftmp, false, 0.0, 16.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->backdrop_saturation = ftmp;
                c->cflags |= CfHasBackdropSat;
            }
        }
        if (inlay_ui_opt_number_prop(s, props, "backdropBrightness", &ftmp, false, 0.0, 16.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->backdrop_brightness = ftmp;
                c->cflags |= CfHasBackdropBright;
            }
        }
        if (inlay_ui_opt_number_prop(s, props, "backdropNoise", &ftmp, false, 0.0, 1.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->backdrop_noise = ftmp;
        }
        if (inlay_ui_opt_number_prop(s, props, "topRimAlpha", &ftmp, false, 0.0, 1.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->top_rim_alpha = ftmp;
                c->cflags |= CfHasTopRimAlpha;
            }
        }

        ColorRef cr = {};
        if (parse_color_prop(s, props, "hoverBg", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->hover_bg = cr;
        }
        cr = {};
        if (parse_color_prop(s, props, "hoverBorderColor", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->hover_border_color = cr;
        }
        cr = {};
        if (parse_color_prop(s, props, "hoverColor", &cr) ||
            parse_color_prop(s, props, "hoverTextColor", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->hover_text_color = cr;
        }
        cr = {};
        if (parse_color_prop(s, props, "pressBg", &cr) ||
            parse_color_prop(s, props, "activeBg", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->press_bg = cr;
        }
        cr = {};
        if (parse_color_prop(s, props, "pressBorderColor", &cr) ||
            parse_color_prop(s, props, "activeBorderColor", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->press_border_color = cr;
        }
        cr = {};
        if (parse_color_prop(s, props, "focusBorderColor", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->focus_border_color = cr;
        }
        cr = {};
        if (parse_color_prop(s, props, "focusRingColor", &cr)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->focus_ring_color = cr;
        }

        if (inlay_ui_opt_number_prop(s, props, "hoverScale", &ftmp, false, 0.05, 8.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->hover_scale = ftmp;
                c->cflags |= CfHasHoverScale;
            }
        }
        if (inlay_ui_opt_number_prop(s, props, "pressScale", &ftmp, false, 0.05, 8.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->press_scale = ftmp;
                c->cflags |= CfHasPressScale;
            }
        }
        if (inlay_ui_opt_number_prop(s, props, "hoverOpacity", &ftmp, false, 0.0, 1.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->hover_opacity = ftmp;
                c->cflags |= CfHasHoverOpacity;
            }
        }

        if (inlay_ui_opt_number_prop(s, props, "flexGrow", &st.flex_grow, false, 0.0)) st.flags |= NfHasFlexGrow;
        if (inlay_ui_opt_number_prop(s, props, "flexShrink", &st.flex_shrink, false, 0.0)) st.flags |= NfHasFlexShrink;
        bool btmp = false;
        if (inlay_ui_opt_bool_prop(s, props, "wrap", &btmp)) {
            st.flags |= NfHasWrap;
            if (btmp) st.flags |= NfWrapFlag;
            else st.flags &= ~NfWrapFlag;
        }
        if (inlay_ui_opt_number_prop(s, props, "aspectRatio", &ftmp, false, 0.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->aspect_ratio = ftmp;
                c->cflags |= CfHasAspectRatio;
            }
        }
        if (inlay_ui_bool_prop(s, props, "shrinkWidth")) st.flags |= NfShrinkWidth;
        if (inlay_ui_bool_prop(s, props, "shrinkHeight")) st.flags |= NfShrinkHeight;

        uint32_t u = 0u;
        if (inlay_ui_opt_uint_prop(s, props, "gridCols", 128u, &u) && u > 0u) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->grid_cols = static_cast<uint16_t>(u);
        }
        if (inlay_ui_opt_uint_prop(s, props, "gridRows", 128u, &u) && u > 0u) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->grid_rows = static_cast<uint16_t>(u);
        }
        if (inlay_ui_opt_uint_prop(s, props, "colSpan", 128u, &u) && u >= 1u) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->col_span = static_cast<uint16_t>(u);
        }
        if (inlay_ui_opt_uint_prop(s, props, "rowSpan", 128u, &u) && u >= 1u) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->row_span = static_cast<uint16_t>(u);
        }

        EtchAttachTo at_to = ETCH_ATTACH_NONE;
        if (peek_str_prop(s, props, "attachTo", [&](const char* c, size_t n) noexcept {
                return parse_attach_token(c, n, &at_to);
            })) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->attach_to = at_to;
                c->cflags |= CfHasAttachTo;
            }
        }
        EtchAnchor anc = ETCH_ANCHOR_TOP_LEFT;
        if (peek_str_prop(s, props, "anchor", [&](const char* c, size_t n) noexcept {
                return parse_anchor_token(c, n, &anc);
            })) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->anchor_element = anc;
                c->cflags |= CfHasAnchorElement;
            }
        }
        if (peek_str_prop(s, props, "anchorParent", [&](const char* c, size_t n) noexcept {
                return parse_anchor_token(c, n, &anc);
            })) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->anchor_parent = anc;
                c->cflags |= CfHasAnchorParent;
            }
        }
        float ox = 0.f;
        float oy = 0.f;
        const bool has_ox = inlay_ui_opt_number_prop(s, props, "offsetX", &ox, false, -16384.0, 16384.0);
        const bool has_oy = inlay_ui_opt_number_prop(s, props, "offsetY", &oy, false, -16384.0, 16384.0);
        if (has_ox || has_oy) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                if (has_ox) c->floating_offset.x = ox;
                if (has_oy) c->floating_offset.y = oy;
                c->cflags |= CfHasFloatingOffset;
            }
        }
        int32_t zi = 0;
        if (inlay_ui_opt_int_prop(s, props, "zIndex", -32768, 32767, &zi)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->z_index = static_cast<int16_t>(zi);
                c->cflags |= CfHasZIndex;
            }
        }
        if (inlay_ui_opt_bool_prop(s, props, "clipChildren", &btmp)) {
            st.flags |= NfHasClipChildren;
            if (btmp) st.flags |= NfClipChildren;
            else st.flags &= ~NfClipChildren;
        }
        if (inlay_ui_bool_prop(s, props, "pointerPassthrough")) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->cflags |= CfPointerPassthrough;
        }
        if (inlay_ui_opt_bool_prop(s, props, "clampToViewport", &btmp)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->cflags |= CfHasClampToViewport;
                if (btmp) c->cflags |= CfClampToViewport;
                else c->cflags &= ~CfClampToViewport;
            }
        }
        if (inlay_ui_bool_prop(s, props, "flipOnOverflow")) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->cflags |= CfFlipOnOverflow;
        }
        if (inlay_ui_opt_bool_prop(s, props, "scrollX", &btmp) ||
            inlay_ui_opt_bool_prop(s, props, "scroll_x", &btmp)) {
            st.flags |= NfHasScrollX;
            if (btmp) st.flags |= NfScrollX;
            else st.flags &= ~NfScrollX;
        }
        if (inlay_ui_opt_bool_prop(s, props, "scrollY", &btmp) ||
            inlay_ui_opt_bool_prop(s, props, "scroll_y", &btmp)) {
            st.flags |= NfHasScrollY;
            if (btmp) st.flags |= NfScrollY;
            else st.flags &= ~NfScrollY;
        }
        if (inlay_ui_bool_prop(s, props, "animateLayout")) st.flags |= NfAnimateLayout;
    }

    // Widget-specific properties
    if (kind == INLAY_UI_BUTTON || kind == INLAY_UI_BADGE || kind == INLAY_UI_INPUT ||
        kind == INLAY_UI_CHECKBOX || kind == INLAY_UI_TOGGLE || kind == INLAY_UI_TABS ||
        inlay_ui_is_ext_widget(kind)) {
        parse_font_props(s, props, st);
        parse_opentype_props(s, props, st);
    }

    if (kind == INLAY_UI_BUTTON || kind == INLAY_UI_INPUT || kind == INLAY_UI_CHECKBOX ||
        kind == INLAY_UI_TOGGLE || kind == INLAY_UI_SLIDER || kind == INLAY_UI_COMBOBOX ||
        kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_MENU) {
        if (inlay_ui_bool_prop(s, props, "disabled")) st.flags |= NfDisabled;
    }

    if (kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_SCROLL ||
        kind == INLAY_UI_BADGE || kind == INLAY_UI_IMAGE || kind == INLAY_UI_AVATAR) {
        parse_clickable_events(s, props, st, node_index);
    }

    if (kind == INLAY_UI_BUTTON) {
        peek_str_prop(s, props, "variant", [&](const char* c, size_t n) noexcept {
            if (inlay_ui_str_eq(c, n, "primary")) st.button_variant = uint8_t(ETCH_BUTTON_PRIMARY);
            else if (inlay_ui_str_eq(c, n, "ghost")) st.button_variant = uint8_t(ETCH_BUTTON_GHOST);
            else if (inlay_ui_str_eq(c, n, "danger")) st.button_variant = uint8_t(ETCH_BUTTON_DANGER);
            else st.button_variant = uint8_t(ETCH_BUTTON_DEFAULT);
            return true;
        });
        peek_str_prop(s, props, "type", [&](const char* c, size_t n) {
            if (inlay_ui_str_eq(c, n, "submit")) {
                if (auto* cx = ensure_core_extra(s, node_index)) cx->flags |= CefButtonSubmit;
                return true;
            }
            return false;
        });
        parse_clickable_events(s, props, st, node_index);
    } else if (kind == INLAY_UI_FORM) {
        if (!inlay_ui_parse_event_prop(s, props, "onSubmit", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
        const TextSlice reset_key = !JS_HasException(s->js) ? inlay_ui_string_slice_prop(s, props, "resetKey") : TextSlice{};
        TextSlice inv_action = {};
        uint32_t inv_handler = INLAY_UI_NONE;
        if (!JS_HasException(s->js)) {
            inlay_ui_parse_event_prop(s, props, "onInvalid", &inv_action, &inv_handler);
        }
        if (reset_key.len > 0u || has_event(inv_action, inv_handler)) {
            if (auto* cx = ensure_core_extra(s, node_index)) {
                cx->field_name = reset_key;
                cx->field_error = inv_action;
                cx->invalid_handler = inv_handler;
            }
        }
    } else if (kind == INLAY_UI_CODE_EDITOR) {
        inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler);
        uint32_t flags = CefNone;
        if (inlay_ui_bool_prop(s, props, "readOnly")) flags |= CefInputReadOnly;
        if (inlay_ui_bool_prop(s, props, "wordWrap")) flags |= CefInputWordWrap;
        TextSlice ghost_sl = {};
        if (!JS_HasException(s->js)) {
            const JSValue gv = JS_GetPropertyStr(s->js, props, "ghostText");
            if (!JS_IsUndefined(gv) && !JS_IsNull(gv)) {
                flags |= CefEditorHasGhost;
                ghost_sl = inlay_ui_copy_text_slice(s, gv);
            }
            JS_FreeValue(s->js, gv);
        }
        TextSlice search_sl = {};
        if (!JS_HasException(s->js)) {
            const JSValue sv = JS_GetPropertyStr(s->js, props, "searchQuery");
            if (!JS_IsUndefined(sv) && !JS_IsNull(sv)) {
                flags |= CefEditorHasSearch;
                search_sl = inlay_ui_copy_text_slice(s, sv);
            }
            JS_FreeValue(s->js, sv);
        }
        if (flags) {
            if (auto* options = ensure_core_extra(s, node_index)) {
                options->flags |= flags;
                options->field_name = ghost_sl;
                options->field_error = search_sl;
            }
        }
    } else if (kind == INLAY_UI_INPUT) {
        if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
        bool password = inlay_ui_bool_prop(s, props, "password");
        peek_str_prop(s, props, "type", [&](const char* c, size_t n) noexcept {
            password |= inlay_ui_str_eq(c, n, "password");
            return true;
        });
        uint32_t flags = password ? CefInputPassword : CefNone;
        if (inlay_ui_bool_prop(s, props, "multiline") && !password) flags |= CefInputMultiline;
        if (inlay_ui_bool_prop(s, props, "readOnly")) flags |= CefInputReadOnly;
        if (inlay_ui_bool_prop(s, props, "wordWrap")) flags |= CefInputWordWrap;
        if (inlay_ui_bool_prop(s, props, "required")) flags |= CefInputRequired;
        TextSlice submit_action = {};
        uint32_t submit_handler = INLAY_UI_NONE;
        if (!JS_HasException(s->js)) {
            inlay_ui_parse_event_prop(s, props, "onSubmit", &submit_action, &submit_handler);
        }
        const TextSlice fname = !JS_HasException(s->js) ? inlay_ui_string_slice_prop(s, props, "name") : TextSlice{};
        const TextSlice ferr = !JS_HasException(s->js) ? inlay_ui_string_slice_prop(s, props, "error") : TextSlice{};
        uint32_t min_len = 0u;
        uint32_t max_len = 0u;
        if (!JS_HasException(s->js) && inlay_ui_opt_uint_prop(s, props, "minLength", 65536u, &min_len)) {
            flags |= CefInputHasMinLen;
        }
        if (!JS_HasException(s->js) && inlay_ui_opt_uint_prop(s, props, "maxLength", 65536u, &max_len)) {
            flags |= CefInputHasMaxLen;
        }
        if (flags || has_event(submit_action, submit_handler) || fname.len > 0u || ferr.len > 0u) {
            if (auto* cx = ensure_core_extra(s, node_index)) {
                cx->flags |= flags;
                cx->submit_action = submit_action;
                cx->submit_handler = submit_handler;
                cx->field_name = fname;
                cx->field_error = ferr;
                cx->min_length = min_len;
                cx->max_length = max_len;
            }
        }
    } else if (kind == INLAY_UI_TOGGLE || kind == INLAY_UI_CHECKBOX) {
        if (inlay_ui_bool_prop(s, props, "checked")) st.flags |= NfChecked;
        if (kind == INLAY_UI_CHECKBOX && inlay_ui_bool_prop(s, props, "indeterminate")) {
            st.flags |= NfIndeterminate;
        }
        if (kind == INLAY_UI_TOGGLE) {
            // Switch / Toggle extra styling: size ('sm'|'md'|'lg'), onColor, offColor, thumbColor, trackWidth, trackHeight
            parse_color_prop(s, props, "onColor", &st.color);
            float tw = 0.f;
            float th = 0.f;
            bool has_tw = false;
            bool has_th = false;
            peek_str_prop(s, props, "size", [&](const char* c, size_t n) noexcept {
                if (inlay_ui_str_eq(c, n, "sm")) { tw = 30.f; th = 16.f; has_tw = has_th = true; return true; }
                if (inlay_ui_str_eq(c, n, "md")) { tw = 38.f; th = 20.f; has_tw = has_th = true; return true; }
                if (inlay_ui_str_eq(c, n, "lg")) { tw = 46.f; th = 24.f; has_tw = has_th = true; return true; }
                return false;
            });
            if (inlay_ui_opt_number_prop(s, props, "trackWidth", &tw, false, 16.0, 256.0)) has_tw = true;
            if (inlay_ui_opt_number_prop(s, props, "trackHeight", &th, false, 10.0, 128.0)) has_th = true;
            ColorRef off_col = {};
            const bool has_off = parse_color_prop(s, props, "offColor", &off_col);
            ColorRef thumb_col = {};
            const bool has_thumb = parse_color_prop(s, props, "thumbColor", &thumb_col);
            if (has_tw || has_th || has_off || has_thumb) {
                if (auto* cx = ensure_core_extra(s, node_index)) {
                    if (has_tw) { cx->aux_f0 = tw; cx->flags |= CefSwitchHasTrackW; }
                    if (has_th) { cx->aux_f1 = th; cx->flags |= CefSwitchHasTrackH; }
                    if (has_off) cx->color_a = off_col;
                    if (has_thumb) cx->color_b = thumb_col;
                }
            }
        }
        if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
    } else if (kind == INLAY_UI_PROGRESS || kind == INLAY_UI_SLIDER) {
        ColorRef tc = {};
        if (parse_color_prop(s, props, "trackColor", &tc)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->track_color = tc;
        }
        if (inlay_ui_opt_number_prop(s, props, "value", &st.value, true)) st.flags2 |= Nf2HasValue;
        float mv = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "min", &mv, true)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) { c->min_val = mv; st.flags2 |= Nf2HasMin; }
        }
        if (inlay_ui_opt_number_prop(s, props, "max", &mv, true)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) { c->max_val = mv; st.flags2 |= Nf2HasMax; }
        }
        if (inlay_ui_opt_number_prop(s, props, "step", &mv, true)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) { c->step = mv; st.flags2 |= Nf2HasStep; }
        }
        if (kind == INLAY_UI_PROGRESS) {
            const bool anim = inlay_ui_bool_prop(s, props, "animated");
            const bool indet = inlay_ui_bool_prop(s, props, "indeterminate");
            const bool show_pct = inlay_ui_bool_prop(s, props, "showPercent");
            bool is_ring = false;
            peek_str_prop(s, props, "variant", [&](const char* c, size_t n) noexcept {
                if (inlay_ui_str_eq(c, n, "ring") || inlay_ui_str_eq(c, n, "circular")) {
                    is_ring = true;
                    return true;
                }
                return false;
            });
            float thick = 0.f;
            const bool has_thick = inlay_ui_opt_number_prop(s, props, "thickness", &thick, false, 1.0, 64.0);
            if (anim || indet || show_pct || is_ring || has_thick) {
                if (auto* cx = ensure_core_extra(s, node_index)) {
                    if (anim) cx->flags |= CefProgressAnimated;
                    if (indet) cx->flags |= CefProgressIndeterm;
                    if (is_ring) cx->flags |= CefProgressRing;
                    if (show_pct) cx->flags |= CefProgressShowPercent;
                    if (has_thick) { cx->aux_f0 = thick; cx->flags |= CefProgressHasThick; }
                }
            }
        } else {
            if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
                inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
            }
        }
    } else if (kind == INLAY_UI_BADGE) {
        uint8_t bv = BadgeDefault;
        bool has_bv = false;
        peek_str_prop(s, props, "variant", [&](const char* c, size_t n) noexcept {
            if (inlay_ui_str_eq(c, n, "secondary")) { bv = BadgeSecondary; has_bv = true; return true; }
            if (inlay_ui_str_eq(c, n, "accent")) { bv = BadgeAccent; has_bv = true; return true; }
            if (inlay_ui_str_eq(c, n, "primary")) { bv = BadgePrimary; has_bv = true; return true; }
            if (inlay_ui_str_eq(c, n, "success")) { bv = BadgeSuccess; has_bv = true; return true; }
            if (inlay_ui_str_eq(c, n, "warning") || inlay_ui_str_eq(c, n, "warn")) {
                bv = BadgeWarning; has_bv = true; return true;
            }
            if (inlay_ui_str_eq(c, n, "danger") || inlay_ui_str_eq(c, n, "error")) {
                bv = BadgeDanger; has_bv = true; return true;
            }
            if (inlay_ui_str_eq(c, n, "info")) { bv = BadgeInfo; has_bv = true; return true; }
            if (inlay_ui_str_eq(c, n, "outline")) { bv = BadgeOutline; has_bv = true; return true; }
            if (inlay_ui_str_eq(c, n, "default")) { bv = BadgeDefault; has_bv = true; return true; }
            return false;
        });
        const bool dot = inlay_ui_bool_prop(s, props, "dot");
        if (has_bv || dot) {
            if (auto* cx = ensure_core_extra(s, node_index)) {
                if (has_bv) cx->badge_variant = bv;
                if (dot) cx->flags |= CefBadgeDot;
            }
        }
    } else if (kind == INLAY_UI_IMAGE) {
        const TextSlice src_sl = inlay_ui_string_slice_prop(s, props, "src");
        if (src_sl.len > 0u) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) c->src = src_sl;
        }
        uint32_t sp = 0u;
        if (inlay_ui_opt_uint_prop(s, props, "spriteId", UINT32_MAX, &sp)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) {
                c->sprite_id = sp;
                st.flags2 |= Nf2HasSprite;
            }
        }
        uint32_t icon = 0u;
        if (inlay_ui_opt_uint_prop(s, props, "iconId", uint32_t(ETCH_INVALID_ICON - 1u), &icon)) {
            st.flags2 |= Nf2HasIcon;
            st.icon_id = static_cast<uint16_t>(icon);
        }
        peek_str_prop(s, props, "fit", [&](const char* c, size_t n) noexcept {
            if (inlay_ui_str_eq(c, n, "contain")) st.image_fit = uint8_t(ETCH_IMAGE_FIT_CONTAIN);
            else if (inlay_ui_str_eq(c, n, "cover")) st.image_fit = uint8_t(ETCH_IMAGE_FIT_COVER);
            else st.image_fit = uint8_t(ETCH_IMAGE_FIT_FILL);
            return true;
        });
    } else if (kind == INLAY_UI_TABS) {
        parse_items_prop(s, props, 16u, st, node_index, true);
        int32_t idx = 0;
        if (inlay_ui_opt_int_prop(s, props, "active", -1, 15, &idx) ||
            inlay_ui_opt_int_prop(s, props, "selected", -1, 15, &idx) ||
            inlay_ui_opt_int_prop(s, props, "value", -1, 15, &idx)) {
            st.int_value = idx;
        }
        if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
    } else if (kind == INLAY_UI_COMBOBOX) {
        parse_items_prop(s, props, 64u, st, node_index, false);
        int32_t idx = 0;
        if (inlay_ui_opt_int_prop(s, props, "selected", -1, 63, &idx) ||
            inlay_ui_opt_int_prop(s, props, "active", -1, 63, &idx) ||
            inlay_ui_opt_int_prop(s, props, "value", -1, 63, &idx)) {
            st.int_value = idx;
        }
        if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
    } else if (kind == INLAY_UI_MODAL) {
        if (inlay_ui_bool_prop(s, props, "open")) st.flags |= NfOpen;
        if (!inlay_ui_parse_event_prop(s, props, "onClose", &st.action, &st.handler) &&
            !inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
    } else if (kind == INLAY_UI_POPOVER) {
        bool open_v = true;
        inlay_ui_opt_bool_prop(s, props, "open", &open_v);
        if (open_v) st.flags |= NfOpen;
        if (!inlay_ui_parse_event_prop(s, props, "onClose", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
    } else if (kind == INLAY_UI_SPLIT) {
        if (inlay_ui_opt_number_prop(s, props, "ratio", &st.value, false, 0.0, 1.0)) st.flags2 |= Nf2HasValue;
        float mv = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "minRatio", &mv, false, 0.0, 1.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) { c->min_val = mv; st.flags2 |= Nf2HasMin; }
        }
        if (inlay_ui_opt_number_prop(s, props, "maxRatio", &mv, false, 0.0, 1.0)) {
            if (auto* c = inlay_ui_ensure_cold(s, st)) { c->max_val = mv; st.flags2 |= Nf2HasMax; }
        }
        if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "action", &st.action, &st.handler);
        }
    } else if (kind == INLAY_UI_MATH) {
        bool dm = true;
        inlay_ui_opt_bool_prop(s, props, "display", &dm);
        if (dm) st.flags |= NfDisplayMath;
    }
}

bool parse_tag_kind(const char* chars, size_t len, InlayUiKind* out, bool* out_default_col) noexcept {
    *out_default_col = false;
    if (inlay_ui_str_eq(chars, len, "box") || inlay_ui_str_eq(chars, len, "row")) {
        *out = INLAY_UI_BOX; return true;
    }
    if (inlay_ui_str_eq(chars, len, "col") || inlay_ui_str_eq(chars, len, "column")) {
        *out = INLAY_UI_BOX; *out_default_col = true; return true;
    }
    if (inlay_ui_str_eq(chars, len, "text")) { *out = INLAY_UI_TEXT; return true; }
    if (inlay_ui_str_eq(chars, len, "button")) { *out = INLAY_UI_BUTTON; return true; }
    if (inlay_ui_str_eq(chars, len, "input")) { *out = INLAY_UI_INPUT; return true; }
    if (inlay_ui_str_eq(chars, len, "codeEditor")) { *out = INLAY_UI_CODE_EDITOR; return true; }
    if (inlay_ui_str_eq(chars, len, "toggle") || inlay_ui_str_eq(chars, len, "switch")) {
        *out = INLAY_UI_TOGGLE; return true;
    }
    if (inlay_ui_str_eq(chars, len, "scroll")) { *out = INLAY_UI_SCROLL; return true; }
    if (inlay_ui_str_eq(chars, len, "progress")) { *out = INLAY_UI_PROGRESS; return true; }
    if (inlay_ui_str_eq(chars, len, "slider")) { *out = INLAY_UI_SLIDER; return true; }
    if (inlay_ui_str_eq(chars, len, "badge")) { *out = INLAY_UI_BADGE; return true; }
    if (inlay_ui_str_eq(chars, len, "divider")) { *out = INLAY_UI_DIVIDER; return true; }
    if (inlay_ui_str_eq(chars, len, "image")) { *out = INLAY_UI_IMAGE; return true; }
    if (inlay_ui_str_eq(chars, len, "card")) { *out = INLAY_UI_CARD; return true; }
    if (inlay_ui_str_eq(chars, len, "form")) { *out = INLAY_UI_FORM; *out_default_col = true; return true; }
    if (inlay_ui_str_eq(chars, len, "spacer")) { *out = INLAY_UI_SPACER; return true; }
    if (inlay_ui_str_eq(chars, len, "checkbox")) { *out = INLAY_UI_CHECKBOX; return true; }
    if (inlay_ui_str_eq(chars, len, "tabs")) { *out = INLAY_UI_TABS; return true; }
    if (inlay_ui_str_eq(chars, len, "combobox")) { *out = INLAY_UI_COMBOBOX; return true; }
    if (inlay_ui_str_eq(chars, len, "modal")) { *out = INLAY_UI_MODAL; return true; }
    if (inlay_ui_str_eq(chars, len, "popover")) { *out = INLAY_UI_POPOVER; return true; }
    if (inlay_ui_str_eq(chars, len, "tooltip")) { *out = INLAY_UI_TOOLTIP; return true; }
    if (inlay_ui_str_eq(chars, len, "split")) { *out = INLAY_UI_SPLIT; return true; }
    if (inlay_ui_str_eq(chars, len, "math")) { *out = INLAY_UI_MATH; return true; }
    if (inlay_ui_str_eq(chars, len, "diagram")) { *out = INLAY_UI_DIAGRAM; return true; }
    if (inlay_ui_str_eq(chars, len, "slot")) { *out = INLAY_UI_SLOT; return true; }
    if (inlay_ui_str_eq(chars, len, "doc") || inlay_ui_str_eq(chars, len, "markdown")) {
        *out = INLAY_UI_DOC; return true;
    }
    if (inlay_ui_str_eq(chars, len, "terminal")) { *out = INLAY_UI_TERMINAL; return true; }
    if (inlay_ui_str_eq(chars, len, "virtualList") || inlay_ui_str_eq(chars, len, "virtual-list") ||
        inlay_ui_str_eq(chars, len, "virtuallist")) {
        *out = INLAY_UI_VIRTUAL_LIST; return true;
    }
    if (inlay_ui_str_eq(chars, len, "table")) { *out = INLAY_UI_TABLE; return true; }
    if (inlay_ui_str_eq(chars, len, "tree")) { *out = INLAY_UI_TREE; return true; }
    if (inlay_ui_str_eq(chars, len, "treeItem") || inlay_ui_str_eq(chars, len, "tree-item") ||
        inlay_ui_str_eq(chars, len, "treeitem")) {
        *out = INLAY_UI_TREE_ITEM; return true;
    }
    if (inlay_ui_str_eq(chars, len, "menu")) { *out = INLAY_UI_MENU; return true; }
    if (inlay_ui_str_eq(chars, len, "contextMenu") || inlay_ui_str_eq(chars, len, "context-menu") ||
        inlay_ui_str_eq(chars, len, "contextmenu")) {
        *out = INLAY_UI_CONTEXT_MENU; return true;
    }
    if (inlay_ui_str_eq(chars, len, "avatar")) { *out = INLAY_UI_AVATAR; return true; }
    if (inlay_ui_str_eq(chars, len, "toast")) { *out = INLAY_UI_TOAST; return true; }
    if (inlay_ui_str_eq(chars, len, "accordion")) { *out = INLAY_UI_ACCORDION; return true; }
    if (inlay_ui_str_eq(chars, len, "skeleton")) { *out = INLAY_UI_SKELETON; return true; }
    return false;
}

uint32_t build_node(InlaySurface* s, JSValueConst value, uint32_t depth, uint32_t sibling_slot, uint32_t parent_path);

// Recursively flatten nested child arrays / JSX fragments to arbitrary depth while
// preserving sibling slot IDs for top-level conditional children.
void append_children(InlaySurface* s, uint32_t parent_index, JSValueConst arr,
                     uint32_t depth, uint32_t base_slot, uint32_t parent_path, bool nested, uint32_t* io_previous) {
    if (depth > 64u) {
        JS_ThrowInternalError(s->js, "UI node/depth capacity exceeded");
        return;
    }
    const JSValue len_v = JS_GetPropertyStr(s->js, arr, "length");
    uint32_t count = 0u;
    JS_ToUint32(s->js, &count, len_v);
    JS_FreeValue(s->js, len_v);
    if (count > s->desc.max_nodes) {
        JS_ThrowInternalError(s->js, "Too many UI children");
        return;
    }
    for (uint32_t i = 0u; i < count && !JS_HasException(s->js); ++i) {
        if ((i & 63u) == 0u && interrupt(s->js, s)) {
            JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
            return;
        }
        const JSValue child = JS_GetPropertyUint32(s->js, arr, i);
        const uint32_t slot = nested ? etch_id_index(base_slot ^ 0x517cc1b7u, i) : i;
        if (JS_IsNull(child) || JS_IsUndefined(child) || JS_IsBool(child)) {
            JS_FreeValue(s->js, child);
            continue;
        }
        if (JS_IsArray(child)) {
            append_children(s, parent_index, child, depth + 1u, slot, parent_path, true, io_previous);
            JS_FreeValue(s->js, child);
            continue;
        }
        const uint32_t at = build_node(s, child, depth + 1u, slot, parent_path);
        JS_FreeValue(s->js, child);
        if (at != INLAY_UI_NONE) {
            if (*io_previous == INLAY_UI_NONE) s->nodes[parent_index].first_child = at;
            else s->nodes[*io_previous].next_sibling = at;
            *io_previous = at;
        }
    }
}

// Concatenate plain text children for leaf text-carrying elements (e.g. <Text>Count: {n}</Text>)
// so interpolated JSX expressions don't truncate after the first child.
void collect_leaf_children_text(InlaySurface* s, JSValueConst arr, uint32_t depth, uint32_t* io_bytes) {
    if (depth > 64u || JS_HasException(s->js)) return;
    const JSValue len_v = JS_GetPropertyStr(s->js, arr, "length");
    uint32_t count = 0u;
    JS_ToUint32(s->js, &count, len_v);
    JS_FreeValue(s->js, len_v);
    if (count > s->desc.max_nodes) {
        JS_ThrowInternalError(s->js, "Too many UI children");
        return;
    }
    for (uint32_t i = 0u; i < count && !JS_HasException(s->js); ++i) {
        if ((i & 63u) == 0u && interrupt(s->js, s)) {
            JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
            return;
        }
        const JSValue child = JS_GetPropertyUint32(s->js, arr, i);
        if (JS_IsNull(child) || JS_IsUndefined(child) || JS_IsBool(child)) {
            JS_FreeValue(s->js, child);
            continue;
        }
        if (JS_IsArray(child)) {
            collect_leaf_children_text(s, child, depth + 1u, io_bytes);
            JS_FreeValue(s->js, child);
            continue;
        }
        if (!JS_IsObject(child)) {
            const TextSlice sl = inlay_ui_copy_text_slice(s, child);
            *io_bytes += sl.len;
        } else {
            const JSValue props = JS_GetPropertyStr(s->js, child, "props");
            if (JS_IsObject(props) && !JS_IsNull(props)) {
                const TextSlice sl = inlay_ui_string_slice_prop(s, props, "text");
                *io_bytes += sl.len;
            }
            JS_FreeValue(s->js, props);
        }
        JS_FreeValue(s->js, child);
    }
}

// Coalesce adjacent unstyled INLAY_UI_TEXT children on BUTTON / BADGE (e.g. <Button>Save {n}</Button>)
// so JSX interpolation renders as one clean label without artificial flex gap between fragments.
void coalesce_inline_text_children(InlaySurface* s, uint32_t parent_index) {
    uint32_t cur = s->nodes[parent_index].first_child;
    while (cur != INLAY_UI_NONE && !JS_HasException(s->js)) {
        const auto is_plain_text = [&](uint32_t idx) noexcept {
            const auto& nd = s->nodes[idx];
            const auto& st = s->styles[idx];
            return nd.kind == INLAY_UI_TEXT && nd.first_child == INLAY_UI_NONE &&
                   st.cold_idx == INLAY_UI_NONE && !st.color.set &&
                   (st.flags & (NfHasSize | NfHasFontId | NfHasOpacity)) == 0u &&
                   get_core_extra(s, idx) == nullptr;
        };
        const uint32_t next = s->nodes[cur].next_sibling;
        if (next != INLAY_UI_NONE && is_plain_text(cur) && is_plain_text(next)) {
            // Measure total run length
            uint32_t total = 0u;
            uint32_t end_node = cur;
            while (end_node != INLAY_UI_NONE && is_plain_text(end_node)) {
                total += s->nodes[end_node].text.length;
                end_node = s->nodes[end_node].next_sibling;
            }
            if (total > 0u && ensure_text_space(s, total)) {
                const uint32_t off = s->text_used;
                uint32_t pos = off;
                for (uint32_t k = cur; k != end_node; k = s->nodes[k].next_sibling) {
                    const EtchStr t = s->nodes[k].text;
                    if (t.length > 0u && t.chars) {
                        std::memcpy(s->text + pos, t.chars, t.length);
                        pos += t.length;
                    }
                }
                s->text_used = pos;
                s->nodes[cur].text = {s->text + off, total, false};
                s->nodes[cur].next_sibling = end_node;
            }
            cur = end_node;
        } else {
            cur = next;
        }
    }
}

uint8_t responsive_width_bucket(float w) noexcept {
    if (w >= 1280.f) return 7u;
    if (w >= 1024.f) return 6u;
    if (w >= 840.f)  return 5u;
    if (w >= 768.f)  return 4u;
    if (w >= 560.f)  return 3u;
    if (w >= 480.f)  return 2u;
    if (w >= 360.f)  return 1u;
    return 0u;
}

uint8_t current_bp_bucket(const InlaySurface* s) noexcept {
    const uint8_t vp = responsive_width_bucket(s->last_view_width_px);
    const float cw = s->current_container_width_px > 0.f ? s->current_container_width_px : s->last_view_width_px;
    const uint8_t ct = responsive_width_bucket(cw);
    return static_cast<uint8_t>((vp & 7u) | ((ct & 7u) << 3));
}

inline size_t memo_align8(size_t v) noexcept {
    return (v + 7u) & ~size_t(7u);
}

inline bool memo_slice_in_range(TextSlice sl, uint32_t text_start, uint32_t text_end) noexcept {
    return sl.len == 0u || (sl.off >= text_start && sl.off <= text_end && sl.len <= text_end - sl.off);
}

inline TextSlice memo_rel_slice(TextSlice sl, uint32_t text_start) noexcept {
    return sl.len ? TextSlice{sl.off - text_start, sl.len} : TextSlice{};
}

inline TextSlice memo_abs_slice(TextSlice sl, uint32_t base_text) noexcept {
    return sl.len ? TextSlice{sl.off + base_text, sl.len} : TextSlice{};
}

void clear_memo_cache(InlaySurface* s) noexcept {
    if (!s) return;
    auto* e = ext(s);
    for (uint32_t i = 0u; i < kMemoCacheCap; ++i) {
        SDL_free(e->memo_cache[i].blob);
        e->memo_cache[i] = {};
    }
    e->memo_cache_clock = 0u;
}

struct MemoBlobOffsets {
    size_t nodes;
    size_t node_texts;
    size_t styles;
    size_t extra_idx;
    size_t colds;
    size_t extras;
    size_t text;
    size_t total;
};

MemoBlobOffsets compute_memo_blob_offsets(uint32_t node_count, uint32_t cold_count,
                                          uint32_t extra_count, uint32_t text_len) noexcept {
    MemoBlobOffsets o = {};
    o.nodes = 0u;
    o.node_texts = memo_align8(o.nodes + size_t(node_count) * sizeof(InlayUiNode));
    o.styles = memo_align8(o.node_texts + size_t(node_count) * sizeof(TextSlice));
    o.extra_idx = memo_align8(o.styles + size_t(node_count) * sizeof(NodeStyle));
    o.colds = memo_align8(o.extra_idx + size_t(node_count) * sizeof(uint32_t));
    o.extras = memo_align8(o.colds + size_t(cold_count) * sizeof(ColdVisualStyle));
    o.text = memo_align8(o.extras + size_t(extra_count) * sizeof(CoreNodeExtra));
    o.total = o.text + size_t(text_len);
    return o;
}

uint32_t splice_memo_native(InlaySurface* s, uint32_t memo_key, uint32_t memo_rev, uint32_t sibling_slot) {
    auto* e = ext(s);
    const uint8_t bp = current_bp_bucket(s);
    MemoNativeEntry* hit = nullptr;
    for (uint32_t i = 0u; i < kMemoCacheCap; ++i) {
        auto& ent = e->memo_cache[i];
        if (ent.blob && ent.memo_key == memo_key && ent.memo_rev == memo_rev && ent.bp_bucket == bp) {
            hit = &ent;
            break;
        }
    }
    if (!hit || hit->node_count == 0u) return INLAY_UI_NONE;

    if (!ensure_node_capacity_for(s, hit->node_count)) {
        JS_ThrowInternalError(s->js, "UI node/depth capacity exceeded");
        return INLAY_UI_NONE;
    }
    if (hit->text_len > 0u && !ensure_text_space(s, hit->text_len)) {
        return INLAY_UI_NONE;
    }

    uint32_t base_cold = 0u;
    if (hit->cold_count > 0u) {
        base_cold = inlay_ui_alloc_pool(s, s->cold_visuals, s->cold_visual_count,
                                        s->cold_visual_cap, hit->cold_count, s->desc.max_nodes);
        if (base_cold == INLAY_UI_NONE) return INLAY_UI_NONE;
    }
    uint32_t base_extra = 0u;
    if (hit->extra_count > 0u) {
        base_extra = inlay_ui_alloc_pool(s, e->core_extras, e->core_extra_count,
                                         e->core_extra_cap, hit->extra_count, s->desc.max_nodes);
        if (base_extra == INLAY_UI_NONE) {
            s->cold_visual_count -= hit->cold_count;
            return INLAY_UI_NONE;
        }
    }

    const MemoBlobOffsets o = compute_memo_blob_offsets(hit->node_count, hit->cold_count,
                                                        hit->extra_count, hit->text_len);
    const uint8_t* blob = hit->blob;
    const auto* src_nodes = reinterpret_cast<const InlayUiNode*>(blob + o.nodes);
    const auto* src_ntext = reinterpret_cast<const TextSlice*>(blob + o.node_texts);
    const auto* src_styles = reinterpret_cast<const NodeStyle*>(blob + o.styles);
    const auto* src_xidx = reinterpret_cast<const uint32_t*>(blob + o.extra_idx);
    const auto* src_colds = reinterpret_cast<const ColdVisualStyle*>(blob + o.colds);
    const auto* src_extras = reinterpret_cast<const CoreNodeExtra*>(blob + o.extras);
    const char* src_text = reinterpret_cast<const char*>(blob + o.text);

    const uint32_t base_node = s->node_count;
    const uint32_t base_text = s->text_used;
    if (hit->text_len > 0u) {
        std::memcpy(s->text + base_text, src_text, hit->text_len);
        s->text_used += hit->text_len;
    }

    for (uint32_t i = 0u; i < hit->cold_count; ++i) {
        ColdVisualStyle c = src_colds[i];
        c.placeholder = memo_abs_slice(c.placeholder, base_text);
        c.src = memo_abs_slice(c.src, base_text);
        c.on_hover_action = memo_abs_slice(c.on_hover_action, base_text);
        c.on_focus_action = memo_abs_slice(c.on_focus_action, base_text);
        s->cold_visuals[base_cold + i] = c;
    }

    for (uint32_t i = 0u; i < hit->extra_count; ++i) {
        CoreNodeExtra cx = src_extras[i];
        cx.dbl_action = memo_abs_slice(cx.dbl_action, base_text);
        cx.rclick_action = memo_abs_slice(cx.rclick_action, base_text);
        cx.password_text = memo_abs_slice(cx.password_text, base_text);
        cx.submit_action = memo_abs_slice(cx.submit_action, base_text);
        cx.field_name = memo_abs_slice(cx.field_name, base_text);
        cx.field_error = memo_abs_slice(cx.field_error, base_text);
        e->core_extras[base_extra + i] = cx;
    }

    for (uint32_t i = 0u; i < hit->node_count; ++i) {
        InlayUiNode nd = src_nodes[i];
        if (nd.first_child != INLAY_UI_NONE) nd.first_child += base_node;
        if (nd.next_sibling != INLAY_UI_NONE) nd.next_sibling += base_node;
        const TextSlice nt = src_ntext[i];
        nd.text = nt.len ? EtchStr{s->text + base_text + nt.off, nt.len, false} : EtchStr{};
        s->nodes[base_node + i] = nd;

        NodeStyle st = src_styles[i];
        if (st.cold_idx != INLAY_UI_NONE) st.cold_idx += base_cold;
        st.action = memo_abs_slice(st.action, base_text);
        s->styles[base_node + i] = st;

        const uint32_t xi = src_xidx[i];
        e->node_extra_idx[base_node + i] = (xi != INLAY_UI_NONE) ? (xi + base_extra) : INLAY_UI_NONE;
    }

    s->nodes[base_node].next_sibling = INLAY_UI_NONE;
    if (!hit->root_explicit_id) {
        s->nodes[base_node].id = hash_slot_id(sibling_slot, s->nodes[base_node].kind);
    }
    s->node_count += hit->node_count;
    if (hit->has_responsive) {
        inlay_ui_note_responsive_cls(s);
    }
    hit->last_used = ++e->memo_cache_clock;
    s->memo_reused_nodes += hit->node_count;
    return base_node;
}

void store_memo_native(InlaySurface* s, uint32_t memo_key, uint32_t memo_rev,
                       uint32_t node_start, uint32_t cold_start, uint32_t extra_start,
                       uint32_t text_start, uint32_t handler_start, uint32_t shadow_start,
                       uint32_t grad_start, uint32_t item_start, uint32_t resp_hits_start,
                       bool root_explicit_id) noexcept {
    if (JS_HasException(s->js)) return;
    auto* e = ext(s);
    if (s->node_count <= node_start) return;
    const uint32_t node_count = s->node_count - node_start;
    const uint32_t cold_count = s->cold_visual_count - cold_start;
    const uint32_t extra_count = e->core_extra_count - extra_start;
    const uint32_t text_len = s->text_used - text_start;
    if (node_count > kMaxMemoSubtreeNodes || text_len > kMaxMemoSubtreeText) return;
    if (s->handler_count != handler_start || s->shadow_pool_count != shadow_start ||
        s->gradient_pool_count != grad_start || s->item_pool_count != item_start) {
        return;
    }

    const uint32_t text_end = s->text_used;
    const char* text_lo = s->text ? (s->text + text_start) : nullptr;
    const char* text_hi = s->text ? (s->text + text_end) : nullptr;
    for (uint32_t i = 0u; i < node_count; ++i) {
        const uint32_t ni = node_start + i;
        const auto& nd = s->nodes[ni];
        const auto& st = s->styles[ni];
        if (inlay_ui_is_ext_widget(nd.kind) || nd.kind == INLAY_UI_FORM) return;
        if (st.ext_idx != INLAY_UI_NONE || st.handler != INLAY_UI_NONE) return;
        if (nd.first_child != INLAY_UI_NONE && (nd.first_child < node_start || nd.first_child >= s->node_count)) return;
        if (i > 0u && nd.next_sibling != INLAY_UI_NONE &&
            (nd.next_sibling < node_start || nd.next_sibling >= s->node_count)) return;
        if (nd.text.length > 0u) {
            if (!text_lo || !nd.text.chars || nd.text.chars < text_lo ||
                size_t(text_hi - nd.text.chars) < nd.text.length) return;
        }
        if (!memo_slice_in_range(st.action, text_start, text_end)) return;
        if (st.cold_idx != INLAY_UI_NONE) {
            if (st.cold_idx < cold_start || st.cold_idx >= s->cold_visual_count) return;
            const auto& c = s->cold_visuals[st.cold_idx];
            if (c.on_hover_handler != INLAY_UI_NONE || c.on_focus_handler != INLAY_UI_NONE) return;
            if (c.shadow_count > 0u || c.gradient_idx != INLAY_UI_NONE) return;
            if (!memo_slice_in_range(c.placeholder, text_start, text_end) ||
                !memo_slice_in_range(c.src, text_start, text_end) ||
                !memo_slice_in_range(c.on_hover_action, text_start, text_end) ||
                !memo_slice_in_range(c.on_focus_action, text_start, text_end)) {
                return;
            }
        }
        const uint32_t xi = e->node_extra_idx[ni];
        if (xi != INLAY_UI_NONE) {
            if (xi < extra_start || xi >= e->core_extra_count) return;
            const auto& cx = e->core_extras[xi];
            if (cx.dbl_handler != INLAY_UI_NONE || cx.rclick_handler != INLAY_UI_NONE ||
                cx.submit_handler != INLAY_UI_NONE || cx.invalid_handler != INLAY_UI_NONE) {
                return;
            }
            if (!memo_slice_in_range(cx.dbl_action, text_start, text_end) ||
                !memo_slice_in_range(cx.rclick_action, text_start, text_end) ||
                !memo_slice_in_range(cx.password_text, text_start, text_end) ||
                !memo_slice_in_range(cx.submit_action, text_start, text_end) ||
                !memo_slice_in_range(cx.field_name, text_start, text_end) ||
                !memo_slice_in_range(cx.field_error, text_start, text_end)) {
                return;
            }
        }
    }

    const MemoBlobOffsets o = compute_memo_blob_offsets(node_count, cold_count, extra_count, text_len);
    auto* blob = static_cast<uint8_t*>(SDL_malloc(o.total));
    if (!blob) return;

    auto* dst_nodes = reinterpret_cast<InlayUiNode*>(blob + o.nodes);
    auto* dst_ntext = reinterpret_cast<TextSlice*>(blob + o.node_texts);
    auto* dst_styles = reinterpret_cast<NodeStyle*>(blob + o.styles);
    auto* dst_xidx = reinterpret_cast<uint32_t*>(blob + o.extra_idx);
    auto* dst_colds = reinterpret_cast<ColdVisualStyle*>(blob + o.colds);
    auto* dst_extras = reinterpret_cast<CoreNodeExtra*>(blob + o.extras);
    char* dst_text = reinterpret_cast<char*>(blob + o.text);

    if (text_len > 0u) {
        std::memcpy(dst_text, s->text + text_start, text_len);
    }
    for (uint32_t i = 0u; i < cold_count; ++i) {
        ColdVisualStyle c = s->cold_visuals[cold_start + i];
        c.placeholder = memo_rel_slice(c.placeholder, text_start);
        c.src = memo_rel_slice(c.src, text_start);
        c.on_hover_action = memo_rel_slice(c.on_hover_action, text_start);
        c.on_focus_action = memo_rel_slice(c.on_focus_action, text_start);
        dst_colds[i] = c;
    }
    for (uint32_t i = 0u; i < extra_count; ++i) {
        CoreNodeExtra cx = e->core_extras[extra_start + i];
        cx.dbl_action = memo_rel_slice(cx.dbl_action, text_start);
        cx.rclick_action = memo_rel_slice(cx.rclick_action, text_start);
        cx.password_text = memo_rel_slice(cx.password_text, text_start);
        cx.submit_action = memo_rel_slice(cx.submit_action, text_start);
        cx.field_name = memo_rel_slice(cx.field_name, text_start);
        cx.field_error = memo_rel_slice(cx.field_error, text_start);
        dst_extras[i] = cx;
    }
    for (uint32_t i = 0u; i < node_count; ++i) {
        const uint32_t ni = node_start + i;
        InlayUiNode nd = s->nodes[ni];
        if (nd.first_child != INLAY_UI_NONE) nd.first_child -= node_start;
        if (i == 0u) nd.next_sibling = INLAY_UI_NONE;
        else if (nd.next_sibling != INLAY_UI_NONE) nd.next_sibling -= node_start;
        dst_ntext[i] = nd.text.length > 0u
            ? TextSlice{static_cast<uint32_t>(nd.text.chars - text_lo), nd.text.length}
            : TextSlice{};
        nd.text = {};
        dst_nodes[i] = nd;

        NodeStyle st = s->styles[ni];
        if (st.cold_idx != INLAY_UI_NONE) st.cold_idx -= cold_start;
        st.action = memo_rel_slice(st.action, text_start);
        dst_styles[i] = st;

        const uint32_t xi = e->node_extra_idx[ni];
        dst_xidx[i] = (xi != INLAY_UI_NONE) ? (xi - extra_start) : INLAY_UI_NONE;
    }

    // Reuse slot with matching memo_key, or empty slot, or LRU victim.
    MemoNativeEntry* slot = nullptr;
    uint32_t victim = 0u;
    for (uint32_t i = 0u; i < kMemoCacheCap; ++i) {
        if (e->memo_cache[i].blob && e->memo_cache[i].memo_key == memo_key) {
            slot = &e->memo_cache[i];
            break;
        }
        if (!e->memo_cache[i].blob && !slot) {
            slot = &e->memo_cache[i];
        }
        if (e->memo_cache[i].last_used < e->memo_cache[victim].last_used) {
            victim = i;
        }
    }
    if (!slot) slot = &e->memo_cache[victim];
    SDL_free(slot->blob);
    *slot = {
        memo_key,
        memo_rev,
        ++e->memo_cache_clock,
        node_count,
        cold_count,
        extra_count,
        text_len,
        current_bp_bucket(s),
        root_explicit_id,
        e->responsive_cls_hits > resp_hits_start,
        0u,
        blob
    };
}

uint32_t build_node(InlaySurface* s, JSValueConst value, uint32_t depth, uint32_t sibling_slot, uint32_t parent_path) {
    if (interrupt(s->js, s)) {
        JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
        return INLAY_UI_NONE;
    }
    if (JS_IsNull(value) || JS_IsUndefined(value) || JS_IsBool(value)) return INLAY_UI_NONE;
    if (depth > 64u) {
        JS_ThrowInternalError(s->js, "UI node/depth capacity exceeded");
        return INLAY_UI_NONE;
    }

    // #50 Native subtree fast-path splice for memoized VNodes (skips JS props & cls parsing).
    uint32_t memo_key = 0u;
    uint32_t memo_rev = 0u;
    bool has_memo = false;
    if (JS_IsObject(value) && !JS_IsArray(value)) {
        const auto* stg = ext(s);
        if (stg->atom_memo_key != JS_ATOM_NULL && stg->atom_memo_rev != JS_ATOM_NULL) {
            const JSValue mk = JS_GetProperty(s->js, value, stg->atom_memo_key);
            if (JS_IsNumber(mk)) {
                const JSValue mr = JS_GetProperty(s->js, value, stg->atom_memo_rev);
                if (JS_IsNumber(mr) &&
                    JS_ToUint32(s->js, &memo_key, mk) == 0 &&
                    JS_ToUint32(s->js, &memo_rev, mr) == 0 &&
                    memo_key > 0u && memo_rev > 0u) {
                    has_memo = true;
                }
                JS_FreeValue(s->js, mr);
            }
            JS_FreeValue(s->js, mk);
        }
        if (has_memo) {
            const uint32_t spliced = splice_memo_native(s, memo_key, memo_rev, sibling_slot);
            if (spliced != INLAY_UI_NONE) return spliced;
            if (JS_HasException(s->js)) return INLAY_UI_NONE;
        }
    }

    if (s->node_count == s->desc.max_nodes || !ensure_node_capacity(s)) {
        JS_ThrowInternalError(s->js, "UI node/depth capacity exceeded");
        return INLAY_UI_NONE;
    }

    const uint32_t memo_cold_start = s->cold_visual_count;
    const uint32_t memo_extra_start = ext(s)->core_extra_count;
    const uint32_t memo_text_start = s->text_used;
    const uint32_t memo_handler_start = s->handler_count;
    const uint32_t memo_shadow_start = s->shadow_pool_count;
    const uint32_t memo_grad_start = s->gradient_pool_count;
    const uint32_t memo_item_start = s->item_pool_count;
    const uint32_t memo_resp_start = ext(s)->responsive_cls_hits;

    // Handle top-level Fragment / Array transparently by wrapping in a fit/grow column box.
    if (JS_IsArray(value)) {
        const uint32_t idx = s->node_count++;
        const uint32_t nid = hash_slot_id(sibling_slot, INLAY_UI_BOX);
        s->nodes[idx] = {INLAY_UI_BOX, nid, INLAY_UI_NONE, INLAY_UI_NONE, {}};
        inlay_ui_init_node_style(s->styles[idx]);
        s->styles[idx].flags |= NfColumn;
        ext(s)->node_extra_idx[idx] = INLAY_UI_NONE;
        const uint32_t path_id = etch_id_index(parent_path, nid);
        uint32_t prev = INLAY_UI_NONE;
        append_children(s, idx, value, depth, sibling_slot, path_id, false, &prev);
        return idx;
    }

    const uint32_t index = s->node_count++;
    s->nodes[index] = {INLAY_UI_TEXT, hash_slot_id(sibling_slot, INLAY_UI_TEXT), INLAY_UI_NONE, INLAY_UI_NONE, {}};
    inlay_ui_init_node_style(s->styles[index]);
    ext(s)->node_extra_idx[index] = INLAY_UI_NONE;
    if (!JS_IsObject(value)) {
        s->nodes[index].text = inlay_ui_copy_text(s, value);
        return index;
    }

    InlayUiKind kind = INLAY_UI_TEXT;
    bool default_col_hint = false;
    const bool valid_tag = peek_str_prop(s, value, "tag", [&](const char* c, size_t n) noexcept {
        return parse_tag_kind(c, n, &kind, &default_col_hint);
    });
    if (!valid_tag) {
        JS_ThrowTypeError(s->js, "Unknown UI tag");
        return INLAY_UI_NONE;
    }
    s->nodes[index].kind = kind;

    const JSValue props = JS_GetPropertyStr(s->js, value, "props");
    if (!JS_IsObject(props) || JS_IsNull(props)) {
        JS_FreeValue(s->js, props);
        JS_ThrowTypeError(s->js, "UI props must be an object");
        return INLAY_UI_NONE;
    }

    // Assign stable node ID and root-relative path ID BEFORE parsing extended widgets.
    const bool explicit_id =
        hash_string_prop(s, props, "id", 2166136261u, &s->nodes[index].id) ||
        hash_string_prop(s, props, "key", 0x811c9dc5u ^ 0x6b65793au, &s->nodes[index].id);
    if (!explicit_id) {
        s->nodes[index].id = hash_slot_id(sibling_slot, kind);
    }
    const uint32_t path_id = etch_id_index(parent_path, s->nodes[index].id);

    parse_style(s, props, kind, s->styles[index], index, default_col_hint);
    if (JS_HasException(s->js)) {
        JS_FreeValue(s->js, props);
        return INLAY_UI_NONE;
    }

    // #52 Container query width scoping for descendants when this container has explicit width.
    const float prev_container_w = s->current_container_width_px;
    if (is_container_kind(kind) && (s->styles[index].flags2 & Nf2WExplicit) != 0u) {
        if (s->styles[index].w_kind == uint8_t(ETCH_SIZING_FIXED) && s->styles[index].w_val > 0.f) {
            s->current_container_width_px = s->styles[index].w_val;
        } else if (s->styles[index].w_kind == uint8_t(ETCH_SIZING_PERCENT) &&
                   s->styles[index].w_val > 0.f && prev_container_w > 0.f) {
            s->current_container_width_px = prev_container_w * s->styles[index].w_val;
        }
    }

    if (inlay_ui_is_ext_widget(kind)) {
        if (!inlay_ui_parse_ext_widget(s, props, kind, s->nodes[index], s->styles[index], path_id)) {
            s->current_container_width_px = prev_container_w;
            JS_FreeValue(s->js, props);
            return INLAY_UI_NONE;
        }
    } else if (kind == INLAY_UI_CODE_EDITOR) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "buffer");
        if (!s->nodes[index].text.length && !JS_HasException(s->js)) {
            JS_ThrowTypeError(s->js, "CodeEditor requires a buffer name");
        }
    } else if (kind == INLAY_UI_INPUT) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "value");
        const TextSlice ph = inlay_ui_string_slice_prop(s, props, "placeholder");
        if (ph.len > 0u) {
            if (auto* c = inlay_ui_ensure_cold(s, s->styles[index])) c->placeholder = ph;
        }
    } else if (kind == INLAY_UI_TOGGLE || kind == INLAY_UI_CHECKBOX || kind == INLAY_UI_SLIDER) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "label");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
    } else if (kind == INLAY_UI_IMAGE) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "alt");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
    } else if (kind == INLAY_UI_COMBOBOX) {
        const TextSlice ph = inlay_ui_string_slice_prop(s, props, "placeholder");
        if (ph.len > 0u) {
            if (auto* c = inlay_ui_ensure_cold(s, s->styles[index])) c->placeholder = ph;
        }
        s->nodes[index].text = inlay_ui_string_prop(s, props, "label");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
    } else if (kind == INLAY_UI_MODAL) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "title");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
    } else if (kind == INLAY_UI_TOOLTIP) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "tip");
    } else if (kind == INLAY_UI_MATH) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "tex");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
    } else if (kind == INLAY_UI_DIAGRAM) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "source");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
    } else if (kind == INLAY_UI_SLOT) {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "name");
        if (!s->nodes[index].text.length) s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
        const JSValue sp = JS_GetPropertyStr(s->js, props, "props");
        if (!JS_IsUndefined(sp) && !JS_IsNull(sp)) {
            TextSlice json_sl = {};
            if (JS_IsString(sp)) {
                json_sl = inlay_ui_copy_text_slice(s, sp);
            } else {
                const JSValue json = JS_JSONStringify(s->js, sp, JS_UNDEFINED, JS_UNDEFINED);
                if (!JS_IsException(json) && !JS_IsUndefined(json)) {
                    json_sl = inlay_ui_copy_text_slice(s, json);
                }
                JS_FreeValue(s->js, json);
            }
            if (json_sl.len > 0u) {
                if (auto* c = inlay_ui_ensure_cold(s, s->styles[index])) c->placeholder = json_sl;
            }
        }
        JS_FreeValue(s->js, sp);
    } else {
        s->nodes[index].text = inlay_ui_string_prop(s, props, "text");
        if (kind == INLAY_UI_PROGRESS && !s->nodes[index].text.length) {
            s->nodes[index].text = inlay_ui_string_prop(s, props, "label");
        }
    }

    // VirtualList windowed renderItem callback hook (coordinated with inlay_ui_widgets.cpp via vlist_cache).
    bool built_vlist_items = false;
    if (kind == INLAY_UI_VIRTUAL_LIST && (s->styles[index].flags2 & Nf2HasValue) != 0u &&
        !JS_HasException(s->js)) {
        const uint32_t ext_idx = s->styles[index].ext_idx;
        if (ext_idx != INLAY_UI_NONE && ext_idx < s->vlist_spec_count) {
            const JSValue render_fn = JS_GetPropertyStr(s->js, props, "renderItem");
            if (JS_IsFunction(s->js, render_fn)) {
                built_vlist_items = true;
                const uint32_t total = s->vlist_specs[ext_idx].total_count;
                uint32_t first = s->vlist_specs[ext_idx].window_start;
                uint32_t end = first + 48u;
                const uint32_t cache_key = s->vlist_specs[ext_idx].path_id;
                for (uint32_t ci = 0u; ci < s->vlist_cache_count; ++ci) {
                    if (s->vlist_cache[ci].element_id == cache_key) {
                        first = s->vlist_cache[ci].first_index;
                        end = s->vlist_cache[ci].end_index;
                        break;
                    }
                }
                if (first > total) first = total;
                if (end > total) end = total;
                if (end <= first && total > 0u) {
                    first = (first < total) ? first : (total > 48u ? total - 48u : 0u);
                    end = (first + 48u < total) ? (first + 48u) : total;
                }
                s->vlist_specs[ext_idx].window_start = first;

                uint32_t previous = INLAY_UI_NONE;
                for (uint32_t i = first; i < end && !JS_HasException(s->js); ++i) {
                    if (interrupt(s->js, s)) {
                        JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
                        break;
                    }
                    JSValue arg = JS_NewUint32(s->js, i);
                    const JSValue item_v = JS_Call(s->js, render_fn, JS_UNDEFINED, 1, &arg);
                    JS_FreeValue(s->js, arg);
                    uint32_t at = INLAY_UI_NONE;
                    if (!JS_IsException(item_v)) {
                        at = build_node(s, item_v, depth + 1u, i, path_id);
                        if (at == INLAY_UI_NONE && !JS_HasException(s->js) &&
                            s->node_count < s->desc.max_nodes && ensure_node_capacity(s)) {
                            // Keep 1:1 row index alignment when renderItem(i) returns null/false.
                            at = s->node_count++;
                            s->nodes[at] = {INLAY_UI_SPACER, hash_slot_id(i, INLAY_UI_SPACER),
                                            INLAY_UI_NONE, INLAY_UI_NONE, {}};
                            inlay_ui_init_node_style(s->styles[at]);
                            ext(s)->node_extra_idx[at] = INLAY_UI_NONE;
                        }
                    }
                    JS_FreeValue(s->js, item_v);
                    if (at != INLAY_UI_NONE) {
                        if (previous == INLAY_UI_NONE) s->nodes[index].first_child = at;
                        else s->nodes[previous].next_sibling = at;
                        previous = at;
                    }
                }
            }
            JS_FreeValue(s->js, render_fn);
        }
    }
    JS_FreeValue(s->js, props);
    if (JS_HasException(s->js)) {
        s->current_container_width_px = prev_container_w;
        return INLAY_UI_NONE;
    }

    if (!built_vlist_items) {
        const JSValue children = JS_GetPropertyStr(s->js, value, "children");
        if (JS_IsArray(children)) {
            if (!is_container_kind(kind) && !s->nodes[index].text.length) {
                // Leaf text-carrying kind (<Text>Count: {n}</Text>, <Doc>...</Doc>, etc.)
                const uint32_t start_off = s->text_used;
                uint32_t total_len = 0u;
                collect_leaf_children_text(s, children, depth + 1u, &total_len);
                if (total_len > 0u && !JS_HasException(s->js)) {
                    s->nodes[index].text = {s->text + start_off, total_len, false};
                }
            } else {
                uint32_t previous = INLAY_UI_NONE;
                append_children(s, index, children, depth, 0u, path_id, false, &previous);
                if ((kind == INLAY_UI_BUTTON || kind == INLAY_UI_BADGE) && !JS_HasException(s->js)) {
                    coalesce_inline_text_children(s, index);
                }
            }
        } else if (!JS_IsUndefined(children) && !JS_IsNull(children) && !JS_IsBool(children)) {
            if (!is_container_kind(kind) && !s->nodes[index].text.length && !JS_IsObject(children)) {
                s->nodes[index].text = inlay_ui_copy_text(s, children);
            } else {
                const uint32_t at = build_node(s, children, depth + 1u, 0u, path_id);
                if (at != INLAY_UI_NONE) s->nodes[index].first_child = at;
            }
        }
        JS_FreeValue(s->js, children);
    }
    s->current_container_width_px = prev_container_w;
    if (kind == INLAY_UI_DOC && s->styles[index].ext_idx != INLAY_UI_NONE) {
        const EtchStr text = s->nodes[index].text;
        auto& spec = s->doc_specs[s->styles[index].ext_idx];
        if (!spec.document_name.len) spec.content_hash = inlay_fnv1a_64(text.chars, text.length);
    } else if (kind == INLAY_UI_INPUT) {
        const auto* input = get_core_extra(s, index);
        auto& text = s->nodes[index].text;
        if (input && (input->flags & CefInputPassword) && text.length) {
            ensure_core_extra(s, index)->password_text = {static_cast<uint32_t>(text.chars - s->text), text.length};
            text = {};
        }
    }
    if (has_memo && !JS_HasException(s->js)) {
        store_memo_native(s, memo_key, memo_rev, index, memo_cold_start, memo_extra_start,
                          memo_text_start, memo_handler_start, memo_shadow_start,
                          memo_grad_start, memo_item_start, memo_resp_start, explicit_id);
    }
    return index;
}

// ============================================================================
// #14 Action Permission Policy & Audit Gate
// ============================================================================

const ActionPolicyEntry* find_action_policy(const InlaySurface* s, EtchStr name) noexcept {
    const auto* storage = ext(s);
    for (uint32_t i = 0u; i < storage->action_policy_count; ++i) {
        if (inlay_ui_equal(name, storage->action_policies[i].name)) {
            return &storage->action_policies[i];
        }
    }
    return nullptr;
}

bool json_object_has_key(EtchStr payload, const char* key, size_t key_len) noexcept {
    if (!payload.chars || payload.length < 2u || !key || key_len == 0u) return false;
    const char* p = payload.chars;
    const char* end = p + payload.length;
    auto skip_ws = [&]() noexcept {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    };
    auto skip_str = [&]() noexcept -> bool {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end) {
            if (*p == '\\') {
                ++p;
                if (p < end) ++p;
                continue;
            }
            if (*p == '"') {
                ++p;
                return true;
            }
            ++p;
        }
        return false;
    };
    skip_ws();
    if (p >= end || *p != '{') return false;
    ++p;
    while (p < end) {
        skip_ws();
        if (p >= end || *p == '}') return false;
        if (*p != '"') return false;
        const char* kstart = p + 1;
        if (!skip_str()) return false;
        const char* kend = p - 1;
        skip_ws();
        if (p >= end || *p != ':') return false;
        ++p;
        if (size_t(kend - kstart) == key_len && std::memcmp(kstart, key, key_len) == 0) {
            return true;
        }
        int depth = 0;
        while (p < end) {
            const char ch = *p;
            if (ch == '"') {
                if (!skip_str()) return false;
                continue;
            }
            if (ch == '{' || ch == '[') {
                ++depth;
            } else if (ch == '}' || ch == ']') {
                if (depth == 0) break;
                --depth;
            } else if (ch == ',' && depth == 0) {
                ++p;
                break;
            }
            ++p;
        }
    }
    return false;
}

bool payload_satisfies_required_keys(const char* csv, EtchStr payload) noexcept {
    if (!csv || !csv[0]) return true;
    const char* p = csv;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        const char* tok = p;
        while (*p && *p != ',') ++p;
        const char* tok_end = p;
        while (tok_end > tok && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) --tok_end;
        const size_t len = size_t(tok_end - tok);
        if (len > 0u && !json_object_has_key(payload, tok, len)) {
            return false;
        }
    }
    return true;
}

bool check_action_authorized(InlaySurface* s, EtchStr name, EtchStr payload) {
    const ActionPolicyEntry* pol = find_action_policy(s, name);
    const InlayActionPermission perm = pol ? pol->permission : INLAY_ACTION_READ;
    const bool req_approval = pol ? pol->require_approval : false;
    if (pol && pol->required_keys_csv[0] != '\0') {
        if (!payload_satisfies_required_keys(pol->required_keys_csv, payload)) {
            return false;
        }
    }
    auto* storage = ext(s);
    if (storage->audit_fn) {
        return storage->audit_fn(s, name, perm, req_approval, payload, storage->audit_user);
    }
    if (req_approval) return false;
    return true;
}

JSValue js_action(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (argc < 1) return JS_ThrowTypeError(js, "action requires a name");
    const char* name = JS_ToCString(js, argv[0]);
    if (!name) return JS_EXCEPTION;
    ActionBinding* binding = nullptr;
    for (uint32_t i = 0u; i < s->action_count; ++i) {
        if (!std::strcmp(name, s->actions[i].name)) {
            binding = &s->actions[i];
            break;
        }
    }
    JS_FreeCString(js, name);
    if (!binding) return JS_ThrowTypeError(js, "Action is not bound");
    const JSValue json =
        JS_JSONStringify(js, argc > 1 ? argv[1] : JS_NULL, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) return json;
    if (JS_IsUndefined(json)) return JS_ThrowTypeError(js, "Action payload is not JSON serializable");
    size_t length = 0u;
    const char* payload = JS_ToCStringLen(js, &length, json);
    bool allowed = false;
    if (payload && length <= 65536u) {
        const EtchStr name_str = {binding->name, static_cast<uint32_t>(std::strlen(binding->name)), true};
        const EtchStr payload_str = {payload, static_cast<uint32_t>(length), false};
        if (check_action_authorized(s, name_str, payload_str)) {
            allowed = true;
            binding->callback(s, payload_str, binding->user);
        }
    } else if (payload) {
        JS_ThrowTypeError(js, "Action payload exceeds 64KiB");
    }
    if (payload) JS_FreeCString(js, payload);
    JS_FreeValue(js, json);
    return JS_HasException(js) ? JS_EXCEPTION : JS_NewBool(js, allowed);
}

JSValue settle_async_action(InlaySurface* s, AsyncActionSlot& slot, JSValueConst result,
                           bool success, bool notify_host) {
    JSValue function = JS_DupValue(s->js, slot.settle[success ? 0 : 1]);
    release_async_action(s, slot, notify_host);
    JSValue completed = JS_Call(s->js, function, JS_UNDEFINED, 1, &result);
    JS_FreeValue(s->js, function);
    return completed;
}

JSValue js_cancel_async_action(JSContext* js, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    uint32_t id = 0u;
    if (JS_ToUint32(js, &id, data[0]) < 0) return JS_EXCEPTION;
    auto* slot = find_async_action(s, id);
    if (!slot || slot->phase == AsyncActionPhase::Cancelled) return JS_NewBool(js, false);
    const JSValue error = JS_NewError(js);
    if (JS_IsException(error)) return error;
    if (JS_SetPropertyStr(js, error, "name", JS_NewString(js, "AbortError")) < 0 ||
        JS_SetPropertyStr(js, error, "message", JS_NewString(js, "Async action cancelled")) < 0) {
        JS_FreeValue(js, error);
        return JS_EXCEPTION;
    }
    JSValue done = settle_async_action(s, *slot, error, false, true);
    JS_FreeValue(js, error);
    if (JS_IsException(done)) return done;
    JS_FreeValue(js, done);
    return JS_NewBool(js, true);
}

JSValue js_request_async_action(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    auto* storage = ext(s);
    if (s->stats.has_error) return JS_ThrowTypeError(js, "Surface has an error");
    if (storage->async_actions_closed) return JS_ThrowTypeError(js, "Async actions are closed");
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(js, "request requires an action name");
    size_t name_length = 0u;
    const char* name = JS_ToCStringLen(js, &name_length, argv[0]);
    if (!name) return JS_EXCEPTION;
    uint32_t name_index = 0u;
    for (; name_index < storage->async_action_name_count; ++name_index) {
        const char* allowed = storage->async_action_names[name_index].value;
        if (name_length == std::strlen(allowed) && std::memcmp(name, allowed, name_length) == 0) break;
    }
    JS_FreeCString(js, name);
    if (name_index == storage->async_action_name_count) {
        return JS_ThrowTypeError(js, "Async action is not allowed");
    }

    const JSValue json = JS_JSONStringify(js, argc > 1 ? argv[1] : JS_NULL, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) return json;
    if (JS_IsUndefined(json)) return JS_ThrowTypeError(js, "Async action payload must be JSON serializable");
    size_t payload_length = 0u;
    const char* payload = JS_ToCStringLen(js, &payload_length, json);
    JS_FreeValue(js, json);
    if (!payload) return JS_EXCEPTION;
    if (payload_length > kAsyncPayloadLimit) {
        JS_FreeCString(js, payload);
        return JS_ThrowTypeError(js, "Async action payload exceeds 64 KiB");
    }
    {
        const char* allowed_name = storage->async_action_names[name_index].value;
        const EtchStr name_str = {allowed_name, static_cast<uint32_t>(std::strlen(allowed_name)), true};
        const EtchStr payload_str = {payload, static_cast<uint32_t>(payload_length), false};
        if (!check_action_authorized(s, name_str, payload_str)) {
            JS_FreeCString(js, payload);
            return JS_ThrowTypeError(js, "Async action blocked by policy");
        }
    }
    // JSON serialization can run toJSON and enqueue another action.
    if (!storage->async_actions) {
        storage->async_actions = static_cast<AsyncActionSlot*>(SDL_calloc(kAsyncActionCapacity, sizeof(AsyncActionSlot)));
        if (!storage->async_actions) {
            JS_FreeCString(js, payload);
            return JS_ThrowOutOfMemory(js);
        }
    }
    AsyncActionSlot* slot = nullptr;
    for (uint32_t i = 0u; i < kAsyncActionCapacity; ++i) {
        if (storage->async_actions[i].phase == AsyncActionPhase::Empty) {
            slot = &storage->async_actions[i];
            break;
        }
    }
    if (!slot || storage->next_async_action == UINT32_MAX) {
        JS_FreeCString(js, payload);
        return JS_ThrowInternalError(js, "Async action capacity exceeded");
    }
    auto* copy = static_cast<char*>(SDL_malloc(payload_length));
    if (copy) std::memcpy(copy, payload, payload_length);
    JS_FreeCString(js, payload);
    if (!copy) return JS_ThrowOutOfMemory(js);

    const JSValue promise = JS_NewPromiseCapability(js, slot->settle);
    if (JS_IsException(promise)) {
        SDL_free(copy);
        return promise;
    }
    slot->id = ++storage->next_async_action;
    slot->payload = copy;
    slot->payload_length = static_cast<uint32_t>(payload_length);
    slot->name_index = name_index;
    slot->phase = AsyncActionPhase::Queued;
    JSValue id = JS_NewUint32(js, slot->id);
    const JSValue cancel = JS_NewCFunctionData(js, js_cancel_async_action, 0, 0, 1, &id);
    if (JS_IsException(cancel) || JS_DefinePropertyValueStr(js, promise, "cancel", cancel, 0) < 0) {
        release_async_action(s, *slot, false);
        JS_FreeValue(js, promise);
        return JS_EXCEPTION;
    }
    return promise;
}

JSValue js_set_timeout(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    double delay = 0.0;
    if (argc < 1 || !JS_IsFunction(js, argv[0]) ||
        (argc > 1 && JS_ToFloat64(js, &delay, argv[1]) < 0) || !std::isfinite(delay) ||
        delay < 0.0 || delay > 86400000.0) {
        return JS_ThrowTypeError(js, "Invalid timer");
    }
    for (auto& timer : s->timers) {
        if (JS_IsUndefined(timer.function)) {
            timer.function = JS_DupValue(js, argv[0]);
            timer.due = inlay_ui_now_ms(s) + delay;
            timer.id = ++s->next_timer;
            return JS_NewUint32(js, timer.id);
        }
    }
    return JS_ThrowInternalError(js, "Timer capacity exceeded");
}

JSValue js_clear_timeout(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    uint32_t id = 0u;
    if (argc) JS_ToUint32(js, &id, argv[0]);
    for (auto& timer : s->timers) {
        if (timer.id == id) {
            JS_FreeValue(js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0u;
        }
    }
    return JS_UNDEFINED;
}

bool install_state(InlaySurface* s, JSValueConst global) {
    const JSValue state = JS_ParseJSON(s->js, s->state, s->state_size, "<state>");
    if (JS_IsException(state)) return false;
    return JS_SetPropertyStr(s->js, global, "state", state) >= 0;
}

void rejection_tracker(JSContext* js, JSValueConst promise, JSValueConst, bool handled, void* opaque) {
    auto* s = static_cast<InlaySurface*>(opaque);
    if (handled) {
        for (uint32_t i = 0u; i < s->rejection_count; ++i) {
            if (JS_IsStrictEqual(js, promise, s->rejections[i])) {
                JS_FreeValue(js, s->rejections[i]);
                s->rejections[i] = s->rejections[--s->rejection_count];
                break;
            }
        }
    } else if (s->rejection_count < 32u) {
        s->rejections[s->rejection_count++] = JS_DupValue(js, promise);
    } else {
        record_error(s, "Too many unhandled Promise rejections");
    }
}

bool drain_jobs(InlaySurface* s) {
    while (JS_IsJobPending(s->runtime)) {
        if (interrupt(s->js, s)) {
            JS_ThrowInternalError(s->js, "Script job deadline exceeded");
            return false;
        }
        JSContext* job_context = nullptr;
        if (JS_ExecutePendingJob(s->runtime, &job_context) < 0) return false;
    }
    return true;
}

bool serialize_state(InlaySurface* s, JSValueConst global) {
    const JSValue current = JS_GetPropertyStr(s->js, global, "state");
    const JSValue json = JS_JSONStringify(s->js, current, JS_UNDEFINED, JS_UNDEFINED);
    bool ok = true;
    if (JS_IsUndefined(json)) {
        JS_ThrowTypeError(s->js, "State must be JSON serializable");
        ok = false;
    } else if (JS_IsException(json)) {
        ok = false;
    } else {
        size_t length = 0u;
        const char* chars = JS_ToCStringLen(s->js, &length, json);
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
    if (ok) s->state_dirty = false;
    return ok;
}

void sync_state_json(InlaySurface* s) {
    if (!s || !s->state_dirty || !s->js || !s->loaded || s->running || s->pending_state || s->stats.has_error) {
        return;
    }
    s->running = true;
    s->timed_out = false;
    s->deadline = SDL_GetPerformanceCounter() + uint64_t(double(s->desc.max_tick_ms) * s->frequency / 1000.0);
    const JSValue global = JS_GetGlobalObject(s->js);
    if (!serialize_state(s, global) || s->timed_out || JS_HasException(s->js)) {
        const JSValue exception = JS_GetException(s->js);
        const char* message = JS_ToCString(s->js, exception);
        record_error(s, s->timed_out ? "State serialization exceeded tick limit"
                        : message    ? message
                                     : "State serialization failed");
        if (message) JS_FreeCString(s->js, message);
        JS_FreeValue(s->js, exception);
        s->state_dirty = false;
    }
    JS_FreeValue(s->js, global);
    s->deadline = 0u;
    s->running = false;
}

bool rebuild(InlaySurface* s, bool eager_state) {
    if (!drain_jobs(s)) return false;
    bool repeat = false;
    do {
        const JSValue global = JS_GetGlobalObject(s->js);
        {
            const JSValue begin_fn = JS_GetPropertyStr(s->js, global, "__inlay_begin");
            if (JS_IsFunction(s->js, begin_fn)) {
                const JSValue br = JS_Call(s->js, begin_fn, global, 0, nullptr);
                JS_FreeValue(s->js, br);
            }
            JS_FreeValue(s->js, begin_fn);
        }
        const JSValue render = JS_GetPropertyStr(s->js, global, "render");
        JSValue state = JS_GetPropertyStr(s->js, global, "state");
        if (!JS_IsFunction(s->js, render)) {
            JS_ThrowTypeError(s->js, "Script must define render(state)");
        }
        const JSValue tree =
            JS_HasException(s->js) ? JS_EXCEPTION : JS_Call(s->js, render, global, 1, &state);
        clear_handlers(s);
        s->node_count = 0u;
        s->text_used = 0u;
        s->cold_visual_count = 0u;
        s->shadow_pool_count = 0u;
        s->gradient_pool_count = 0u;
        s->item_pool_count = 0u;
        ext(s)->core_extra_count = 0u;
        ext(s)->uses_responsive_cls = false;
        ext(s)->responsive_cls_hits = 0u;
        s->current_container_width_px = s->last_view_width_px;
        inlay_ui_reset_widget_specs(s);
        if (!JS_IsException(tree)) build_node(s, tree, 0u, 0u, 0u);
        JS_FreeValue(s->js, tree);
        JS_FreeValue(s->js, render);
        JS_FreeValue(s->js, state);
        if (!JS_HasException(s->js)) {
            const JSValue commit_fn = JS_GetPropertyStr(s->js, global, "__inlay_commit");
            if (JS_IsFunction(s->js, commit_fn)) {
                const JSValue cr = JS_Call(s->js, commit_fn, global, 0, nullptr);
                JS_FreeValue(s->js, cr);
            }
            JS_FreeValue(s->js, commit_fn);
        }
        if (!JS_HasException(s->js) && !s->pending_state) {
            if (eager_state || s->path[0]) {
                serialize_state(s, global);
            } else {
                const JSValue current = JS_GetPropertyStr(s->js, global, "state");
                if (JS_IsUndefined(current)) {
                    JS_ThrowTypeError(s->js, "State must be JSON serializable");
                } else {
                    s->state_dirty = true;
                }
                JS_FreeValue(s->js, current);
            }
        }
        const bool jobs = JS_IsJobPending(s->runtime);
        if (jobs && !JS_HasException(s->js) && !drain_jobs(s)) {
            JS_FreeValue(s->js, global);
            return false;
        }
        const bool hook_dirty = !JS_HasException(s->js) && inlay_ui_bool_prop(s, global, "__inlay_dirty");
        JS_FreeValue(s->js, global);
        repeat = (jobs || hook_dirty) && !JS_HasException(s->js);
        if (repeat && interrupt(s->js, s)) {
            JS_ThrowInternalError(s->js, "Script update deadline exceeded");
            return false;
        }
    } while (repeat);
    if (s->rejection_count) JS_ThrowInternalError(s->js, "Unhandled Promise rejection");
    s->stats.node_count = s->node_count;
    return !JS_HasException(s->js);
}

bool dispatch_one_event(InlaySurface* s, JSValueConst global, const WidgetEvent& ev) {
    if (ev.handler != INLAY_UI_NONE) {
        if (ev.handler >= s->handler_count) return true;
        const JSValue fn = JS_DupValue(s->js, s->handlers[ev.handler]);
        const JSValue state = JS_GetPropertyStr(s->js, global, "state");
        JSValue result = JS_UNDEFINED;
        const bool bare_click =
            ev.is_click && (!ev.payload.length || inlay_ui_equal(ev.payload, "null"));
        if (bare_click) {
            JSValue args[] = {state};
            result = JS_Call(s->js, fn, global, 1, args);
        } else {
            const JSValue parsed = parse_json(s, ev.payload);
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
    if (!ev.name.length) return true;
    const JSValue parsed = parse_json(s, ev.payload);
    if (!JS_IsException(parsed)) {
        const JSValue handler = JS_GetPropertyStr(s->js, global, "onEvent");
        if (JS_IsFunction(s->js, handler)) {
            JSValue args[] = {
                JS_NewStringLen(s->js, ev.name.chars, ev.name.length),
                JS_DupValue(s->js, parsed),
                JS_GetPropertyStr(s->js, global, "state")
            };
            if (!JS_IsException(args[1])) {
                const JSValue result = JS_Call(s->js, handler, global, 3, args);
                JS_FreeValue(s->js, result);
            }
            for (auto arg : args) JS_FreeValue(s->js, arg);
        } else {
            for (uint32_t i = 0u; i < s->action_count; ++i) {
                if (inlay_ui_equal(ev.name, s->actions[i].name)) {
                    if (check_action_authorized(s, ev.name, ev.payload)) {
                        s->actions[i].callback(s, ev.payload, s->actions[i].user);
                    }
                    break;
                }
            }
        }
        JS_FreeValue(s->js, handler);
    }
    JS_FreeValue(s->js, parsed);
    return !JS_HasException(s->js);
}

bool run_batch(InlaySurface* s, const WidgetEvent* first) {
    const uint64_t start = SDL_GetPerformanceCounter();
    begin_tick(s);
    s->stats.has_error = false;
    s->error[0] = '\0';
    const JSValue global = JS_GetGlobalObject(s->js);
    bool ok = true;
    if (s->pending_state) {
        s->pending_state = false;
        s->state_dirty = false;
        ok = install_state(s, global);
    }
    for (const auto* ev = first; ok && ev && !JS_HasException(s->js); ev = ev->next) {
        ok = dispatch_one_event(s, global, *ev) && ok;
    }
    if (!JS_HasException(s->js)) ok = rebuild(s, false) && ok;
    JS_FreeValue(s->js, global);
    const bool result = finish_tick(s, start) && ok;
    etch_request_redraw(s->etch);
    return result;
}

bool run_update(InlaySurface* s, EtchStr name, EtchStr payload) {
    WidgetEvent ev = {nullptr, name, payload, INLAY_UI_NONE, false};
    return run_batch(s, name.length ? &ev : nullptr);
}

void clear_context(InlaySurface* s) {
    clear_memo_cache(s);
    if (s->js) {
        retire_async_actions(s);
        clear_handlers(s);
        for (uint32_t i = 0u; i < s->rejection_count; ++i) {
            JS_FreeValue(s->js, s->rejections[i]);
        }
        s->rejection_count = 0u;
        for (auto& timer : s->timers) {
            JS_FreeValue(s->js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0u;
        }
        auto* storage = ext(s);
        if (storage->atom_memo_key != JS_ATOM_NULL) {
            JS_FreeAtom(s->js, storage->atom_memo_key);
            storage->atom_memo_key = JS_ATOM_NULL;
        }
        if (storage->atom_memo_rev != JS_ATOM_NULL) {
            JS_FreeAtom(s->js, storage->atom_memo_rev);
            storage->atom_memo_rev = JS_ATOM_NULL;
        }
        JS_FreeContext(s->js);
        s->js = nullptr;
    }
}

bool load(InlaySurface* s, EtchStr script, bool is_hmr = false) {
    // No newline in prefix so QuickJS line 1 == user script line 1!
    static constexpr char prefix[] = "(function(){";
    static constexpr char suffix[] =
        "\n;return {render:typeof render==='function'?render:undefined,onEvent:typeof "
        "onEvent==='function'?onEvent:undefined};})()";

    const char* filename = s->path[0] ? s->path : "<surface>";
    EtchStr effective = script;

    if (s->state_dirty && s->js && s->loaded && !s->stats.has_error && !s->pending_state) {
        sync_state_json(s);
    }

    // Transpile JSX cleanly when enabled (zero-copy fast-path if no JSX candidate).
    if (!s->desc.disable_jsx && script.length > 0u) {
        char jsx_err[1024] = {};
        size_t jsx_len = 0u;
        if (!inlay_jsx_transpile(script.chars, script.length, filename,
                                 &s->jsx_buf, &s->jsx_cap, &jsx_len,
                                 jsx_err, sizeof(jsx_err))) {
            // Fail fast; keep last-good tree/state intact on HMR.
            const char* message = jsx_err[0] ? jsx_err : "JSXSyntaxError: Failed to transpile JSX";
            if (is_hmr) record_reload_error(s, message);
            else record_error(s, message);
            return false;
        }
        if (jsx_len > 0u) {
            effective = {s->jsx_buf, static_cast<uint32_t>(jsx_len), false};
        }
    }
    const uint32_t needed = effective.length + uint32_t(sizeof(prefix) + sizeof(suffix));
    if (needed > s->source_capacity) {
        uint32_t cap = s->source_capacity ? s->source_capacity : 4096u;
        while (cap < needed) cap *= 2u;
        auto* memory = static_cast<char*>(SDL_realloc(s->source, cap));
        if (!memory) {
            if (is_hmr) record_reload_error(s, "Source storage allocation failed");
            else record_error(s, "Source storage allocation failed");
            return false;
        }
        s->source = memory;
        s->source_capacity = cap;
    }
    std::memcpy(s->source, prefix, sizeof(prefix) - 1u);
    if (effective.length) {
        std::memcpy(s->source + sizeof(prefix) - 1u, effective.chars, effective.length);
    }
    std::memcpy(s->source + sizeof(prefix) - 1u + effective.length, suffix, sizeof(suffix));
    const size_t length = sizeof(prefix) - 1u + effective.length + sizeof(suffix) - 1u;

    const bool fresh = !is_hmr || !s->loaded || s->stats.has_error || !s->js;
    JSValue compiled = JS_UNDEFINED;
    uint64_t start = 0u;
    if (fresh) {
        clear_context(s);
        if (s->runtime) JS_FreeRuntime(s->runtime);
        s->runtime = JS_NewRuntime();
        if (!s->runtime) {
            record_error(s, "Unable to allocate script runtime");
            return false;
        }
        JS_SetMemoryLimit(s->runtime, s->desc.memory_limit_bytes);
        JS_SetMaxStackSize(s->runtime, s->desc.max_stack_bytes);
        JS_SetInterruptHandler(s->runtime, interrupt, s);
        JS_SetHostPromiseRejectionTracker(s->runtime, rejection_tracker, s);
        s->js = JS_NewContext(s->runtime);
        if (!s->js) {
            record_error(s, "Unable to allocate script context");
            return false;
        }
        JS_SetContextOpaque(s->js, s);
        ext(s)->atom_memo_key = JS_NewAtom(s->js, "__memoKey");
        ext(s)->atom_memo_rev = JS_NewAtom(s->js, "__memoRev");
    } else {
        start = SDL_GetPerformanceCounter();
        begin_tick(s);
        compiled = JS_Eval(s->js, s->source, length, filename,
                           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        const bool expired = SDL_GetPerformanceCounter() >= s->deadline;
        if (JS_IsException(compiled) || expired) {
            if (JS_IsException(compiled)) {
                const JSValue error = JS_GetException(s->js);
                const char* message = JS_ToCString(s->js, error);
                record_reload_error(s, expired ? "Script compilation exceeded its tick time limit"
                                               : message ? message : "HMR script compilation failed");
                if (message) JS_FreeCString(s->js, message);
                JS_FreeValue(s->js, error);
                if (JS_HasException(s->js)) JS_FreeValue(s->js, JS_GetException(s->js));
            } else {
                JS_FreeValue(s->js, compiled);
                record_reload_error(s, "Script compilation exceeded its tick time limit");
            }
            s->deadline = 0u;
            s->running = false;
            s->stats.last_tick_ms = double(SDL_GetPerformanceCounter() - start) * 1000.0 / s->frequency;
            return false;
        }
        clear_memo_cache(s);
        retire_async_actions(s);
        for (auto& timer : s->timers) {
            JS_FreeValue(s->js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.id = 0u;
        }
    }

    s->stats.has_error = false;
    s->error[0] = '\0';
    s->pending_state = false;
    s->state_dirty = false;
    const JSValue global = JS_GetGlobalObject(s->js);
    if (fresh) {
        JS_SetPropertyStr(s->js, global, "action", JS_NewCFunction(s->js, js_action, "action", 2));
        JS_SetPropertyStr(s->js, global, "request", JS_NewCFunction(s->js, js_request_async_action, "request", 2));
        JS_SetPropertyStr(s->js, global, "setTimeout",
                          JS_NewCFunction(s->js, js_set_timeout, "setTimeout", 2));
        JS_SetPropertyStr(s->js, global, "clearTimeout",
                          JS_NewCFunction(s->js, js_clear_timeout, "clearTimeout", 1));
        JS_SetPropertyStr(s->js, global, "Atomics", JS_UNDEFINED);
        JS_SetPropertyStr(s->js, global, "SharedArrayBuffer", JS_UNDEFINED);
        static constexpr char bootstrap[] =
            "globalThis.__frag=Symbol('frag');"
            "globalThis.__inlay_hm=Object.create(null);globalThis.__inlay_stk=[];globalThis.__inlay_has_hm=false;"
            "globalThis.__inlay_seen=Object.create(null);globalThis.__inlay_fx=[];globalThis.__inlay_dirty=false;"
            "globalThis.__inlay_begin=()=>{globalThis.__inlay_dirty=false;globalThis.__inlay_seen=Object.create(null);"
            "globalThis.__inlay_fx.length=0;globalThis.__inlay_stk=[{k:'__root',i:0,c:0}];"
            "globalThis.__inlay_seen.__root=globalThis.__inlay_visit=1;};"
            "globalThis.__inlay_commit=()=>{if(globalThis.__inlay_has_hm){const hm=globalThis.__inlay_hm,seen=globalThis.__inlay_seen;"
            "let any=false;for(const k in hm){if(!seen[k]){const a=hm[k];for(let j=0;j<a.length;++j){"
            "const sl=a[j];if(sl&&sl.__fx&&typeof sl.cl==='function'){try{sl.cl();}catch(_){}}}delete hm[k];}else any=true;}"
            "globalThis.__inlay_has_hm=any;}const fx=globalThis.__inlay_fx;if(!fx.length)return;"
            "globalThis.__inlay_fx=[];for(let j=0;j<fx.length;++j){const r=fx[j];if(typeof r.cl==='function')r.cl();"
            "const cl=r.fn();r.cl=typeof cl==='function'?cl:undefined;}};"
            "globalThis.h=(tag,props,...children)=>{"
            "if(tag===globalThis.__frag)return children;"
            "if(tag===Math)tag='math';"
            "if(typeof tag==='function'){"
            "if(tag.__ui)return tag(props||{},...children);"
            "const stk=globalThis.__inlay_stk,pf=stk.length?stk[stk.length-1]:{k:'__root',i:0,c:0};"
            "const nm=tag.name||'C',hk=props&&props.key!==undefined&&props.key!==null;"
            "const sk=pf.k+'/'+(hk?(nm+'#'+String(props.key)):(nm+'@'+(pf.c++)));"
            "globalThis.__inlay_seen[sk]=++globalThis.__inlay_visit;stk.push({k:sk,i:0,c:0,memo:pf.memo});"
            "const mp=children.length||!(props&&'children'in props)?Object.assign({},props,{children}):Object.assign({},props);"
            "try{return tag(mp);}finally{stk.pop();}}"
            "if(tag==='switch')return{tag:'toggle',props:props||{},children};"
            "return{tag,props:props||{},children};};"
            "globalThis.__jsx=globalThis.h;"
            "(()=>{"
            "const isP=p=>!!(p&&typeof p==='object'&&!Array.isArray(p)&&typeof p.tag!=='string'),"
            "mk=f=>{f.__ui=true;return f;},"
            "m=t=>mk((p,...c)=>isP(p)?h(t,p,...c):((p===undefined||p===null)&&!c.length?h(t,{}):h(t,{},p,...c))),"
            "x=(t,k='text')=>mk((p,...c)=>typeof p==='string'?h(t,{[k]:p},...c):m(t)(p,...c));"
            "globalThis.ui={"
            "box:m('box'),row:m('row'),col:m('col'),card:m('card'),form:m('form'),spacer:m('spacer'),"
            "text:x('text'),button:x('button'),input:m('input'),toggle:m('toggle'),switch:m('toggle'),"
            "checkbox:m('checkbox'),tabs:m('tabs'),combobox:m('combobox'),scroll:m('scroll'),"
            "progress:m('progress'),slider:m('slider'),badge:x('badge'),divider:m('divider'),"
            "image:m('image'),modal:m('modal'),popover:m('popover'),tooltip:m('tooltip'),"
            "split:m('split'),math:x('math'),diagram:x('diagram'),slot:x('slot','name'),"
            "doc:x('doc','markdown'),codeEditor:x('codeEditor','buffer'),terminal:x('terminal','ansi'),virtualList:m('virtualList'),"
            "table:m('table'),tree:m('tree'),treeItem:x('treeItem','label'),"
            "menu:m('menu'),contextMenu:m('contextMenu'),avatar:x('avatar','name'),"
            "toast:x('toast','description'),accordion:x('accordion','title'),skeleton:m('skeleton'),"
            "h:globalThis.h,fragment:(...c)=>c};"
            "const G=globalThis;"
            "G.Box='box';G.Row='row';G.Col='col';G.Card='card';G.Form='form';G.Text='text';"
            "G.Button='button';G.Input='input';G.Toggle='toggle';G.Switch='switch';"
            "G.Checkbox='checkbox';G.Tabs='tabs';G.Combobox='combobox';G.Scroll='scroll';"
            "G.Progress='progress';G.Slider='slider';G.Badge='badge';G.Divider='divider';"
            "G.Image='image';G.Modal='modal';G.Popover='popover';G.Tooltip='tooltip';"
            "G.Split='split';G.Spacer='spacer';G.Diagram='diagram';G.Slot='slot';"
            "G.Doc='doc';G.CodeEditor='codeEditor';G.Terminal='terminal';G.VirtualList='virtualList';G.Table='table';"
            "G.Tree='tree';G.TreeItem='treeItem';G.Menu='menu';G.ContextMenu='contextMenu';"
            "G.Avatar='avatar';G.Toast='toast';G.Accordion='accordion';G.Skeleton='skeleton';"
            "G.Fragment=G.__frag;"
            "const slot=()=>{const stk=G.__inlay_stk,f=stk.length?stk[stk.length-1]:{k:'__root',i:0,c:0};"
            "if(f.i===0&&G.__inlay_collect)G.__inlay_collect.push(f.k);"
            "const hm=G.__inlay_hm;let a=hm[f.k];if(!a){a=hm[f.k]=[];G.__inlay_has_hm=true;}return[a,f.i++];};"
            "const same=(a,b)=>{if(!a||!b||a.length!==b.length)return false;"
            "for(let j=0;j<a.length;++j)if(!Object.is(a[j],b[j]))return false;return true;};"
            "G.useState=init=>{const[a,i]=slot();if(i>=a.length)a[i]=typeof init==='function'?init():init;"
            "const frame=G.__inlay_stk[G.__inlay_stk.length-1],owner=frame&&frame.memo;"
            "return[a[i],v=>{const n=typeof v==='function'?v(a[i]):v;"
            "if(!Object.is(a[i],n)){a[i]=n;for(let m=owner;m;m=m.parent)m.dirty=true;G.__inlay_dirty=true;}}];};"
            "G.useRef=init=>{const[a,i]=slot();if(i>=a.length)a[i]={current:init};return a[i];};"
            "G.useMemo=(fn,deps)=>{const[a,i]=slot();"
            "if(i>=a.length||!deps||!same(a[i].d,deps))a[i]={v:fn(),d:deps?deps.slice():null};return a[i].v;};"
            "G.useCallback=(fn,deps)=>G.useMemo(()=>fn,deps);"
            "G.useEffect=(fn,deps)=>{const[a,i]=slot();"
            "if(i>=a.length){const r={__fx:1,d:deps?deps.slice():null,fn,cl:undefined};a[i]=r;G.__inlay_fx.push(r);}"
            "else if(!deps||!same(a[i].d,deps)){a[i].d=deps?deps.slice():null;a[i].fn=fn;G.__inlay_fx.push(a[i]);}};})();"
            R"JS(
            (() => {
                const G = globalThis;
                function equalProps(a, b) {
                    const keys = Object.keys(a);
                    if (keys.length !== Object.keys(b).length) return false;
                    for (const key of keys) {
                        if (!Object.hasOwn(b, key)) return false;
                        const x = a[key], y = b[key];
                        if (Object.is(x, y)) continue;
                        if (key !== 'children' || !Array.isArray(x) || !Array.isArray(y) ||
                            x.length !== y.length || !x.every((v, i) => Object.is(v, y[i]))) return false;
                    }
                    return true;
                }
                let nextMemoKey = 1;
                G.memo = (component, equal = equalProps) => {
                    function Memo(props) {
                        const ref = G.useRef(null);
                        const cache = ref.current || (ref.current = {id: nextMemoKey++, rev: 0});
                        const stack = G.__inlay_stk, frame = stack[stack.length - 1];
                        const collect = G.__inlay_collect;
                        cache.parent = frame.memo;
                        if (cache.component === component && !cache.dirty && equal(cache.props, props)) {
                            for (const key of cache.scopes) {
                                G.__inlay_seen[key] = ++G.__inlay_visit;
                                if (collect) collect.push(key);
                            }
                            return cache.tree;
                        }
                        cache.component = null;
                        cache.dirty = false;
                        frame.memo = cache;
                        const scopes = [];
                        G.__inlay_collect = scopes;
                        try {
                            cache.tree = G.h(component, props);
                        } finally {
                            G.__inlay_collect = collect;
                            frame.memo = cache.parent;
                        }
                        // A nested boundary may have unmounted scopes while rendering.
                        cache.scopes = scopes.filter(k => Object.hasOwn(G.__inlay_seen, k));
                        if (collect) for (const key of cache.scopes) collect.push(key);
                        cache.props = props;
                        cache.component = component;
                        cache.rev = (cache.rev + 1) >>> 0;
                        if (cache.tree && typeof cache.tree === 'object' && !Array.isArray(cache.tree) &&
                            typeof cache.tree.tag === 'string' && Object.isExtensible(cache.tree)) {
                            Object.defineProperty(cache.tree, '__memoKey', {value: cache.id, configurable: true});
                            Object.defineProperty(cache.tree, '__memoRev', {value: cache.rev, configurable: true});
                        }
                        return cache.tree;
                    }
                    Object.defineProperty(Memo, 'name', {value: 'memo(' + component.name + ')'});
                    return Memo;
                };
                function Content(p) { return p.render(); }
                function Fallback(p) { return p.render(p.error, p.retry); }
                function ErrorBoundary(p) {
                    const slot = G.useRef(null);
                    const [, invalidate] = G.useState(0);
                    if (!slot.current || !Object.is(slot.current.key, p.resetKey))
                        slot.current = {key: p.resetKey, failed: false};
                    const state = slot.current;
                    if (!state.failed) {
                        const visit = G.__inlay_visit, effects = G.__inlay_fx.length;
                        try {
                            return G.h(Content, {render: p.render});
                        } catch (error) {
                            G.__inlay_fx.length = effects;
                            for (const key in G.__inlay_seen)
                                if (G.__inlay_seen[key] > visit) delete G.__inlay_seen[key];
                            state.failed = true;
                            state.error = error;
                        }
                    }
                    const retry = () => {
                        if (state !== slot.current || !state.failed) return;
                        slot.current = null;
                        invalidate(n => n + 1);
                    };
                    return typeof p.fallback === 'function'
                        ? G.h(Fallback, {render: p.fallback, error: state.error, retry})
                        : G.ui.text(String(state.error));
                }
                G.ErrorBoundary = ErrorBoundary;
                G.ui.errorBoundary = p => G.h(ErrorBoundary, p);
                G.ui.errorBoundary.__ui = true;
            })();
            )JS";
        const JSValue bootstrap_result =
            JS_Eval(s->js, bootstrap, sizeof(bootstrap) - 1u, "<inlay>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(s->js, bootstrap_result);
        // The fixed framework bootstrap runs in a new, empty VM. Start the
        // caller's execution budget before installing state or running user JS.
        start = SDL_GetPerformanceCounter();
        begin_tick(s);
    }
    JSValue result = JS_UNDEFINED;
    bool ok = !JS_HasException(s->js) && install_state(s, global);
    JS_FreeValue(s->js, global);
    if (ok) {
        if (fresh) {
            result = JS_Eval(s->js, s->source, length, filename, JS_EVAL_TYPE_GLOBAL);
        } else {
            result = JS_EvalFunction(s->js, compiled);
            compiled = JS_UNDEFINED;
        }
        ok = !JS_IsException(result);
        if (ok) {
            const JSValue target = JS_GetGlobalObject(s->js);
            JS_SetPropertyStr(s->js, target, "render", JS_GetPropertyStr(s->js, result, "render"));
            JS_SetPropertyStr(s->js, target, "onEvent", JS_GetPropertyStr(s->js, result, "onEvent"));
            JS_FreeValue(s->js, target);
        }
        JS_FreeValue(s->js, result);
    }
    JS_FreeValue(s->js, compiled);
    if (ok && !JS_HasException(s->js)) ok = rebuild(s, true);
    const bool completed = finish_tick(s, start) && ok;
    s->loaded = completed;
    if (completed) {
        SDL_free(ext(s)->reload_error);
        ext(s)->reload_error = nullptr;
    }
    etch_request_redraw(s->etch);
    return completed;
}

float resolve_corner_val(float px, uint8_t tok, const EtchTheme& theme) noexcept {
    switch (tok) {
        case RadPanel:   return theme.radius_panel_px;
        case RadControl: return theme.radius_control_px;
        case RadInput:   return theme.radius_input_px;
        case RadSmall:   return theme.radius_small_px;
        case RadPill:    return theme.radius_pill_px;
        default:         return px;
    }
}

inline bool has_border_sides(EtchPadding sides) noexcept {
    return sides.left > 0.f || sides.right > 0.f || sides.top > 0.f || sides.bottom > 0.f;
}

inline bool has_sizing_override(const InlaySurface* s, const NodeStyle& st,
                                const ColdVisualStyle* cold, bool vertical) noexcept {
    if (s && (st.flags & NfHasFlexGrow) != 0u && st.flex_grow > 0.f &&
        (vertical == s->draw_parent_col)) {
        return true;
    }
    if (vertical) {
        return (st.flags2 & Nf2HExplicit) != 0u || (cold && (cold->h_min > 0.f || cold->h_max > 0.f));
    }
    return (st.flags2 & Nf2WExplicit) != 0u || (cold && (cold->w_min > 0.f || cold->w_max > 0.f));
}

EtchStr input_json(EtchContext* ctx, const char* text, uint32_t length) {
    auto* json = static_cast<char*>(etch_frame_arena_alloc(ctx, size_t(length) * 6u + 2u, 1u));
    if (!json) return {};
    constexpr char hex[] = "0123456789abcdef";
    uint32_t out = 0u;
    json[out++] = '"';
    for (uint32_t i = 0u; i < length; ++i) {
        const auto c = static_cast<uint8_t>(text[i]);
        if (c == '"' || c == '\\') {
            json[out++] = '\\';
            json[out++] = static_cast<char>(c);
        } else if (c < 32u) {
            std::memcpy(json + out, "\\u00", 4u);
            out += 4u;
            json[out++] = hex[c >> 4];
            json[out++] = hex[c & 15u];
        } else {
            json[out++] = static_cast<char>(c);
        }
    }
    json[out++] = '"';
    return {json, out, false};
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

void draw_ui_math(EtchRect bounds, void* user) {
    const auto& d = *static_cast<const UiMathDraw*>(user);
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

void draw_ui_diagram(EtchRect, void* user) {
    const auto& d = *static_cast<const UiDiagramDraw*>(user);
    inlay_diagram_draw_ex(d.result, d.desc, &d.meta);
}

struct UiIndetBarDraw {
    float phase;
    EtchColor fill;
    EtchCorners radius;
    bool reduced;
};

void draw_ui_indet_bar(EtchRect local, void* user) {
    const auto& d = *static_cast<const UiIndetBarDraw*>(user);
    if (local.w <= 1.f || local.h <= 0.f) return;
    const float pill_w = d.reduced ? (local.w * 0.40f) : std::fmax(16.f, local.w * 0.34f);
    const float x = d.reduced
        ? (local.w - pill_w) * 0.5f
        : ((local.w + pill_w) * d.phase - pill_w);
    auto r = etch_rect_draw_desc_default();
    r.rect = {x, 0.f, pill_w, local.h};
    r.fill = d.fill;
    r.radius = d.radius;
    etch_draw_rect(&r);
}

float eval_indet_phase(InlaySurface* s, uint32_t element_id, bool* out_reduced) noexcept {
    auto* e = ext(s);
    IndetPhaseSlot* slot = nullptr;
    for (uint32_t i = 0u; i < e->indet_slot_count; ++i) {
        if (e->indet_slots[i].element_id == element_id) {
            slot = &e->indet_slots[i];
            break;
        }
    }
    if (!slot) {
        if (e->indet_slot_count < 16u) {
            slot = &e->indet_slots[e->indet_slot_count++];
        } else {
            uint32_t victim = 0u;
            for (uint32_t i = 1u; i < 16u; ++i) {
                if (e->indet_slots[i].last_used_frame < e->indet_slots[victim].last_used_frame) {
                    victim = i;
                }
            }
            slot = &e->indet_slots[victim];
        }
        *slot = {};
        slot->element_id = element_id;
    }
    slot->last_used_frame = s->frame_counter;
    const uint32_t tween_id = etch_id_index(element_id, 0x9B01u);
    const EtchEasingSpec ease = etch_easing_in_out();
    constexpr float kHalfPeriodSec = 1.15f;

    if (!slot->initialized) {
        slot->initialized = true;
        (void)etch_tween(tween_id, 0.0f, kHalfPeriodSec, ease, 0.0f);
        const float probe = etch_tween(tween_id, 1.0f, kHalfPeriodSec, ease, 0.0f);
        slot->reduced = (probe > 0.5f);
        slot->target = slot->reduced ? 0.5f : 1.0f;
        *out_reduced = slot->reduced;
        return slot->reduced ? 0.5f : probe;
    }
    *out_reduced = slot->reduced;
    if (slot->reduced) return 0.5f;

    float val = etch_tween(tween_id, slot->target, kHalfPeriodSec, ease, 0.0f);
    if (slot->target > 0.5f && val >= 0.995f) {
        slot->target = 0.0f;
        val = etch_tween(tween_id, 0.0f, kHalfPeriodSec, ease, 0.0f);
    } else if (slot->target < 0.5f && val <= 0.005f) {
        slot->target = 1.0f;
        val = etch_tween(tween_id, 1.0f, kHalfPeriodSec, ease, 0.0f);
    }
    return std::fmin(1.f, std::fmax(0.f, val));
}

SplitSyncEntry* find_or_add_split_sync(InlaySurface* s, uint32_t element_id, float init_ratio) noexcept {
    auto* e = ext(s);
    for (uint32_t i = 0u; i < e->split_sync_count; ++i) {
        if (e->split_sync[i].element_id == element_id) {
            e->split_sync[i].last_used_frame = s->frame_counter;
            return &e->split_sync[i];
        }
    }
    SplitSyncEntry* slot = nullptr;
    if (e->split_sync_count < 16u) {
        slot = &e->split_sync[e->split_sync_count++];
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < 16u; ++i) {
            if (e->split_sync[i].last_used_frame < e->split_sync[victim].last_used_frame) victim = i;
        }
        slot = &e->split_sync[victim];
    }
    *slot = {element_id, init_ratio, init_ratio, s->frame_counter};
    return slot;
}

HoverTrackEntry* find_or_add_hover_track(InlaySurface* s, uint32_t element_id) noexcept {
    auto* e = ext(s);
    for (uint32_t i = 0u; i < e->hover_track_count; ++i) {
        if (e->hover_track[i].element_id == element_id) {
            e->hover_track[i].last_used_frame = s->frame_counter;
            return &e->hover_track[i];
        }
    }
    HoverTrackEntry* slot = nullptr;
    if (e->hover_track_count < 32u) {
        slot = &e->hover_track[e->hover_track_count++];
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < 32u; ++i) {
            if (e->hover_track[i].last_used_frame < e->hover_track[victim].last_used_frame) victim = i;
        }
        slot = &e->hover_track[victim];
    }
    *slot = {element_id, false, false, s->frame_counter};
    return slot;
}

bool is_activated(uint32_t id) noexcept {
    return etch_clicked(id) ||
           (etch_focused(id) && (etch_key_pressed(SDLK_RETURN, false) ||
                                 etch_key_pressed(SDLK_KP_ENTER, false) ||
                                 etch_key_pressed(SDLK_SPACE, false)));
}

void dispatch_hover_focus(InlaySurface* s, const ColdVisualStyle* cold,
                          uint32_t id, WidgetEvents* events) {
    if (!cold) return;
    const bool want_hover = has_event(cold->on_hover_action, cold->on_hover_handler);
    const bool want_focus = has_event(cold->on_focus_action, cold->on_focus_handler);
    if (!want_hover && !want_focus) return;
    auto* tr = find_or_add_hover_track(s, id);
    if (want_hover) {
        const bool hovered = etch_hovered(id);
        if (tr->hovered != hovered) {
            tr->hovered = hovered;
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, cold->on_hover_action),
                                 cold->on_hover_handler,
                                 hovered ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
        }
    }
    if (want_focus) {
        const bool focused = etch_focused(id);
        if (tr->focused != focused) {
            tr->focused = focused;
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, cold->on_focus_action),
                                 cold->on_focus_handler,
                                 focused ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
        }
    }
}

FormStateEntry* find_or_add_form_state(InlaySurface* s, uint32_t element_id) noexcept {
    auto* e = ext(s);
    for (uint32_t i = 0u; i < e->form_state_count; ++i) {
        if (e->form_states[i].element_id == element_id) {
            e->form_states[i].last_used_frame = s->frame_counter;
            return &e->form_states[i];
        }
    }
    FormStateEntry* slot = nullptr;
    if (e->form_state_count < kMaxFormStates) {
        slot = &e->form_states[e->form_state_count++];
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < kMaxFormStates; ++i) {
            if (e->form_states[i].last_used_frame < e->form_states[victim].last_used_frame) victim = i;
        }
        slot = &e->form_states[victim];
    }
    *slot = {element_id, 0u, false, s->frame_counter};
    return slot;
}

uint32_t utf8_codepoint_count(const char* chars, uint32_t byte_len) noexcept {
    if (!chars || byte_len == 0u) return 0u;
    uint32_t count = 0u;
    size_t pos = 0u;
    while (pos < byte_len) {
        (void)inlay_utf8_next(chars, byte_len, &pos);
        ++count;
    }
    return count;
}

bool validate_input_field(const InlaySurface* s, const CoreNodeExtra* extra,
                          const char* val, uint32_t byte_len,
                          char out_err[96], EtchStr* out_err_str) noexcept {
    if (extra && extra->field_error.len > 0u) {
        *out_err_str = inlay_ui_slice(s, extra->field_error);
        return false;
    }
    if (!extra) return true;
    const bool req = (extra->flags & CefInputRequired) != 0u;
    const bool has_min = (extra->flags & CefInputHasMinLen) != 0u;
    const bool has_max = (extra->flags & CefInputHasMaxLen) != 0u;
    if (!req && !has_min && !has_max) return true;

    bool non_ws = false;
    for (uint32_t i = 0u; i < byte_len; ++i) {
        const char c = val[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            non_ws = true;
            break;
        }
    }
    if (req && !non_ws) {
        *out_err_str = ETCH_STR_LIT("This field is required");
        return false;
    }
    const uint32_t cp_len = utf8_codepoint_count(val, byte_len);
    if (has_min && cp_len < extra->min_length) {
        const int n = std::snprintf(out_err, 96u, "Minimum %u characters", extra->min_length);
        *out_err_str = n > 0 ? EtchStr{out_err, static_cast<uint32_t>(n), false} : ETCH_STR_LIT("Too short");
        return false;
    }
    if (has_max && cp_len > extra->max_length) {
        const int n = std::snprintf(out_err, 96u, "Maximum %u characters", extra->max_length);
        *out_err_str = n > 0 ? EtchStr{out_err, static_cast<uint32_t>(n), false} : ETCH_STR_LIT("Too long");
        return false;
    }
    return true;
}

struct ArenaJsonWriter {
    char* data;
    uint32_t len;
    uint32_t cap;
    bool overflow;
};

ArenaJsonWriter arena_json_begin(EtchContext* ctx, uint32_t cap) noexcept {
    if (cap > 65536u) cap = 65536u;
    char* mem = static_cast<char*>(etch_frame_arena_alloc(ctx, cap, 1u));
    return {mem, 0u, mem ? cap : 0u, mem == nullptr};
}

void arena_json_raw(ArenaJsonWriter& w, const char* s, uint32_t n) noexcept {
    if (n == 0u) return;
    if (!w.data || n > w.cap - w.len) {
        w.overflow = true;
        return;
    }
    std::memcpy(w.data + w.len, s, n);
    w.len += n;
}

void arena_json_quoted(ArenaJsonWriter& w, EtchStr str) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    arena_json_raw(w, "\"", 1u);
    for (uint32_t i = 0u; i < str.length; ++i) {
        const uint8_t c = static_cast<uint8_t>(str.chars[i]);
        switch (c) {
            case '"':  arena_json_raw(w, "\\\"", 2u); break;
            case '\\': arena_json_raw(w, "\\\\", 2u); break;
            case '\b': arena_json_raw(w, "\\b", 2u); break;
            case '\f': arena_json_raw(w, "\\f", 2u); break;
            case '\n': arena_json_raw(w, "\\n", 2u); break;
            case '\r': arena_json_raw(w, "\\r", 2u); break;
            case '\t': arena_json_raw(w, "\\t", 2u); break;
            default:
                if (c < 0x20u) {
                    const char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15u]};
                    arena_json_raw(w, esc, 6u);
                } else {
                    const char ch = static_cast<char>(c);
                    arena_json_raw(w, &ch, 1u);
                }
                break;
        }
    }
    arena_json_raw(w, "\"", 1u);
}

EtchStr arena_json_finish(const ArenaJsonWriter& w) noexcept {
    return (w.data && !w.overflow) ? EtchStr{w.data, w.len, false} : EtchStr{};
}

void dispatch_box_interaction(InlaySurface* s, const NodeStyle& st, const ColdVisualStyle* cold,
                              const CoreNodeExtra* extra, uint32_t id, WidgetEvents* events,
                              uint8_t descendants_consumed = 0u) {
    const bool disabled = (st.flags & NfDisabled) != 0u;
    const bool hovered = etch_hovered(id);
    const bool stop_prop = extra && (extra->flags & CefStopPropagation) != 0u;
    if (!disabled) {
        if (hovered) {
            if (cold && (cold->cflags & CfHasCursor)) {
                etch_set_cursor(cold->cursor);
            } else if ((st.flags & NfClickable) != 0u) {
                etch_set_cursor(ETCH_CURSOR_HAND);
            }
        }
        const bool activated = is_activated(id);
        if ((descendants_consumed & ConsumeClick) == 0u && has_event(st.action, st.handler) && activated) {
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                 ETCH_STR_LIT("null"), true);
        }
        const bool dbl = etch_double_clicked(id);
        if ((descendants_consumed & ConsumeDblClick) == 0u && extra &&
            has_event(extra->dbl_action, extra->dbl_handler) && dbl) {
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, extra->dbl_action), extra->dbl_handler,
                                 ETCH_STR_LIT("null"), true);
        }
        const bool rclick = etch_right_clicked(id);
        if ((descendants_consumed & ConsumeRClick) == 0u && extra &&
            has_event(extra->rclick_action, extra->rclick_handler) && rclick) {
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, extra->rclick_action), extra->rclick_handler,
                                 ETCH_STR_LIT("null"), true);
        }
        if (stop_prop) {
            if (activated || etch_clicked(id)) ext(s)->child_consumed_mask |= ConsumeClick;
            if (dbl) ext(s)->child_consumed_mask |= ConsumeDblClick;
            if (rclick) ext(s)->child_consumed_mask |= ConsumeRClick;
            if ((hovered || etch_focused(id)) && etch_key_pressed(SDLK_ESCAPE, false)) {
                ext(s)->child_consumed_mask |= ConsumeEscape;
            }
        }
        dispatch_hover_focus(s, cold, id, events);
    }
}

EtchColor resolve_live_text_color(InlaySurface* s, const NodeStyle& st, const ColdVisualStyle* cold,
                                  uint32_t id, const EtchTheme& theme, EtchColor fallback) noexcept {
    const ColorRef eff_text = inlay_ui_eff_text(st, cold, inlay_is_dark_theme(theme));
    EtchColor target = eff_text.set ? inlay_ui_resolve_color(eff_text, theme, fallback) : fallback;
    const bool disabled = (st.flags & NfDisabled) != 0u;
    if (!disabled && cold && cold->hover_text_color.set && etch_hovered(id)) {
        target = inlay_ui_resolve_color(cold->hover_text_color, theme, target);
    }
    if (cold && (cold->cflags & CfHasTransition) && (cold->transition.prop_mask & TransColor)) {
        return inlay_ui_tween_color(s, etch_id_index(id, 0x7202u), TransColor, target, cold->transition);
    }
    return target;
}

bool box_handles_opacity(InlayUiKind kind) noexcept {
    switch (kind) {
        case INLAY_UI_BOX:
        case INLAY_UI_CARD:
        case INLAY_UI_FORM:
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

bool is_plain_text_child(const InlaySurface* s, uint32_t idx) noexcept {
    const auto& nd = s->nodes[idx];
    const auto& st = s->styles[idx];
    return nd.kind == INLAY_UI_TEXT && nd.first_child == INLAY_UI_NONE &&
           st.cold_idx == INLAY_UI_NONE && !st.color.set &&
           (st.flags & (NfHasSize | NfHasFontId | NfHasOpacity)) == 0u &&
           get_core_extra(s, idx) == nullptr;
}

EtchStr find_subtree_label(const InlaySurface* s, uint32_t first_child) noexcept {
    for (uint32_t at = first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
        if (s->nodes[at].text.length > 0u) return s->nodes[at].text;
    }
    return {};
}

struct BadgeColors {
    EtchColor bg;
    EtchColor border;
    EtchColor fg;
    EtchColor dot;
    float border_w;
};

BadgeColors resolve_badge_variant(uint8_t variant, const EtchTheme& theme, bool dark) noexcept {
    switch (variant) {
        case BadgeSecondary:
            return {theme.surface_subtle, theme.border_subtle, theme.text_secondary, theme.text_muted, 1.f};
        case BadgeAccent:
        case BadgePrimary: {
            const EtchColor ac = theme.accent;
            const EtchColor fg = dark
                ? etch_color_blend_over(ac, etch_color_white(110u))
                : (theme.accent_primary.a ? theme.accent_primary : ac);
            return {
                etch_rgba8(ac.r, ac.g, ac.b, dark ? 44u : 26u),
                etch_rgba8(ac.r, ac.g, ac.b, dark ? 92u : 70u),
                fg, ac, 1.f
            };
        }
        case BadgeSuccess:
            return {
                dark ? etch_rgba8(34u, 197u, 94u, 40u) : etch_rgba8(22u, 163u, 74u, 24u),
                dark ? etch_rgba8(52u, 211u, 153u, 84u) : etch_rgba8(22u, 163u, 74u, 68u),
                dark ? etch_rgb_hex(0x4ADE80u, 255u) : etch_rgb_hex(0x15803Du, 255u),
                dark ? etch_rgb_hex(0x34D399u, 255u) : etch_rgb_hex(0x16A34Au, 255u),
                1.f
            };
        case BadgeWarning:
            return {
                dark ? etch_rgba8(245u, 158u, 11u, 40u) : etch_rgba8(217u, 119u, 6u, 26u),
                dark ? etch_rgba8(251u, 191u, 36u, 84u) : etch_rgba8(217u, 119u, 6u, 72u),
                dark ? etch_rgb_hex(0xFBBF24u, 255u) : etch_rgb_hex(0xB45309u, 255u),
                dark ? etch_rgb_hex(0xFBBF24u, 255u) : etch_rgb_hex(0xD97706u, 255u),
                1.f
            };
        case BadgeDanger:
            return {
                dark ? etch_rgba8(239u, 68u, 68u, 42u) : etch_rgba8(220u, 38u, 38u, 24u),
                dark ? etch_rgba8(248u, 113u, 113u, 88u) : etch_rgba8(220u, 38u, 38u, 68u),
                dark ? etch_rgb_hex(0xF87171u, 255u) : etch_rgb_hex(0xB91C1Cu, 255u),
                dark ? etch_rgb_hex(0xF87171u, 255u) : etch_rgb_hex(0xDC2626u, 255u),
                1.f
            };
        case BadgeInfo:
            return {
                dark ? etch_rgba8(59u, 130u, 246u, 42u) : etch_rgba8(37u, 99u, 235u, 24u),
                dark ? etch_rgba8(96u, 165u, 250u, 88u) : etch_rgba8(37u, 99u, 235u, 68u),
                dark ? etch_rgb_hex(0x60A5FAu, 255u) : etch_rgb_hex(0x1D4ED8u, 255u),
                dark ? etch_rgb_hex(0x60A5FAu, 255u) : etch_rgb_hex(0x2563EBu, 255u),
                1.f
            };
        case BadgeOutline:
            return {
                etch_color_transparent(),
                theme.border_control.a ? theme.border_control : theme.border_line,
                theme.text_primary,
                theme.accent,
                1.f
            };
        case BadgeDefault:
        default:
            return {theme.surface_subtle, theme.border_subtle, theme.text_primary, theme.accent, 0.f};
    }
}

void draw_error_card(InlaySurface* s, const EtchTheme& theme, const char* error, bool reload) {
    const bool dark = inlay_is_dark_theme(theme);
    auto wrap = etch_box_desc_default();
    wrap.axis = ETCH_AXIS_COL;
    wrap.width = etch_sizing_grow(0.f, 0.f);
    wrap.height = etch_sizing_fit(0.f, 0.f);
    wrap.padding = etch_padding_all(14.f);
    etch_begin_box(&wrap);

    auto card = etch_box_desc_default();
    card.axis = ETCH_AXIS_COL;
    card.width = etch_sizing_grow(0.f, 0.f);
    card.height = etch_sizing_fit(0.f, 0.f);
    card.background = theme.bg_elevated;
    card.border_color = etch_rgba8(theme.danger.r, theme.danger.g, theme.danger.b, dark ? 165u : 140u);
    card.border_width = 1.f;
    card.radius = etch_corners_all(theme.radius_panel_px);
    card.continuous_corners = true;
    card.clip_children = true;
    card.elevation = ETCH_ELEVATION_MD;
    etch_begin_box(&card);

    auto hdr = etch_box_desc_default();
    hdr.axis = ETCH_AXIS_ROW;
    hdr.width = etch_sizing_grow(0.f, 0.f);
    hdr.height = etch_sizing_fit(0.f, 0.f);
    hdr.padding = etch_padding_xy(14.f, 10.f);
    hdr.gap = 8.f;
    hdr.align_y = ETCH_ALIGN_Y_CENTER;
    hdr.background = etch_rgba8(theme.danger.r, theme.danger.g, theme.danger.b, dark ? 44u : 26u);
    hdr.border_sides = {0.f, 0.f, 0.f, 1.f};
    hdr.border_color = etch_rgba8(theme.danger.r, theme.danger.g, theme.danger.b, dark ? 96u : 64u);
    etch_begin_box(&hdr);

    auto dot = etch_box_desc_default();
    dot.width = etch_sizing_fixed(8.f);
    dot.height = etch_sizing_fixed(8.f);
    dot.radius = etch_corners_all(4.f);
    dot.background = theme.danger;
    etch_box(&dot);

    auto title = etch_text_desc_default();
    title.font_id = uint16_t(ETCH_FONT_SEMIBOLD);
    title.font_size_px = 13.f;
    title.color = dark ? etch_rgb_hex(0xFCA5A5u, 255u) : etch_rgb_hex(0xB91C1Cu, 255u);
    title.wrap = false;
    etch_text(reload ? ETCH_STR_LIT("Reload Error") : ETCH_STR_LIT("Surface Error"), &title);

    if (s->path[0]) {
        const char* base = s->path;
        for (const char* p = s->path; *p; ++p) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        auto sp = etch_box_desc_default();
        sp.width = etch_sizing_grow(0.f, 0.f);
        sp.height = etch_sizing_fixed(1.f);
        etch_box(&sp);

        auto fn = etch_text_desc_default();
        fn.font_id = uint16_t(ETCH_FONT_MONO);
        fn.font_size_px = 11.5f;
        fn.color = theme.text_muted;
        fn.wrap = false;
        fn.ellipsis = true;
        etch_text(inlay_ui_frame_text(s, {base, uint32_t(std::strlen(base)), false}), &fn);
    }
    etch_end_box();

    auto body = etch_box_desc_default();
    body.axis = ETCH_AXIS_COL;
    body.width = etch_sizing_grow(0.f, 0.f);
    body.height = etch_sizing_fit(0.f, 0.f);
    body.padding = etch_padding_xy(14.f, 12.f);
    etch_begin_box(&body);

    auto msg = etch_text_desc_default();
    msg.font_id = uint16_t(ETCH_FONT_MONO);
    msg.font_size_px = 12.5f;
    msg.line_height_px = 18.5f;
    msg.color = theme.text_primary;
    msg.wrap = true;
    msg.selectable = true;
    etch_text(inlay_ui_frame_text(s, {error, uint32_t(std::strlen(error)), false}), &msg);

    etch_end_box();
    etch_end_box();
    etch_end_box();
}

} // namespace

// ============================================================================
// Exported Cross-Module Helpers (Declared in src/inlay_ui_internal.h)
// ============================================================================

void inlay_ui_note_responsive_cls(InlaySurface* s) noexcept {
    if (!s) return;
    ext(s)->uses_responsive_cls = true;
    ++ext(s)->responsive_cls_hits;
}

TextSlice inlay_ui_copy_text_slice(InlaySurface* s, JSValueConst value) {
    size_t n = 0u;
    const char* chars = JS_ToCStringLen(s->js, &n, value);
    if (!chars) return {};
    const TextSlice result = append_text_raw(s, chars, n);
    JS_FreeCString(s->js, chars);
    return result;
}

EtchStr inlay_ui_copy_text(InlaySurface* s, JSValueConst value) {
    const TextSlice sl = inlay_ui_copy_text_slice(s, value);
    return inlay_ui_slice(s, sl);
}

TextSlice inlay_ui_string_slice_prop(InlaySurface* s, JSValueConst obj, const char* name) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
    const TextSlice result = (JS_IsUndefined(v) || JS_IsNull(v)) ? TextSlice{} : inlay_ui_copy_text_slice(s, v);
    JS_FreeValue(s->js, v);
    return result;
}

EtchStr inlay_ui_string_prop(InlaySurface* s, JSValueConst obj, const char* name) {
    const TextSlice sl = inlay_ui_string_slice_prop(s, obj, name);
    return inlay_ui_slice(s, sl);
}

bool inlay_ui_opt_number_prop(InlaySurface* s, JSValueConst obj, const char* name, float* out,
                              bool unbounded, double min_v, double max_v) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    const bool ok = read_float_val(s, v, name, out, min_v, max_v, unbounded);
    JS_FreeValue(s->js, v);
    return ok;
}

bool inlay_ui_bool_prop(InlaySurface* s, JSValueConst obj, const char* name) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
    const bool result = JS_ToBool(s->js, v) > 0;
    JS_FreeValue(s->js, v);
    return result;
}

bool inlay_ui_opt_bool_prop(InlaySurface* s, JSValueConst obj, const char* name, bool* out) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    *out = JS_ToBool(s->js, v) > 0;
    JS_FreeValue(s->js, v);
    return true;
}

bool inlay_ui_opt_uint_prop(InlaySurface* s, JSValueConst obj, const char* name,
                            uint32_t max_value, uint32_t* out) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
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
    *out = static_cast<uint32_t>(value);
    return true;
}

bool inlay_ui_opt_int_prop(InlaySurface* s, JSValueConst obj, const char* name,
                           int32_t min_value, int32_t max_value, int32_t* out) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
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
    *out = static_cast<int32_t>(value);
    return true;
}

uint32_t inlay_ui_register_handler(InlaySurface* s, JSValueConst fn) {
    if (s->handler_count == s->handler_capacity) {
        uint32_t cap = s->handler_capacity ? s->handler_capacity * 2u : 64u;
        if (cap > s->desc.max_nodes) cap = s->desc.max_nodes;
        if (s->handler_count >= cap) {
            JS_ThrowInternalError(s->js, "UI handler capacity exceeded");
            return INLAY_UI_NONE;
        }
        auto* mem = static_cast<JSValue*>(SDL_realloc(s->handlers, size_t(cap) * sizeof(JSValue)));
        if (!mem) {
            JS_ThrowInternalError(s->js, "UI handler allocation failed");
            return INLAY_UI_NONE;
        }
        s->handlers = mem;
        s->handler_capacity = cap;
    }
    const uint32_t idx = s->handler_count++;
    s->handlers[idx] = JS_DupValue(s->js, fn);
    return idx;
}

bool inlay_ui_parse_event_prop(InlaySurface* s, JSValueConst props, const char* name,
                               TextSlice* out_action, uint32_t* out_handler) {
    const JSValue v = JS_GetPropertyStr(s->js, props, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(s->js, v);
        return false;
    }
    if (JS_IsFunction(s->js, v)) {
        const uint32_t h = inlay_ui_register_handler(s, v);
        JS_FreeValue(s->js, v);
        if (h == INLAY_UI_NONE) return false;
        if (out_handler) *out_handler = h;
        if (out_action) *out_action = {};
        return true;
    }
    const TextSlice sl = inlay_ui_copy_text_slice(s, v);
    JS_FreeValue(s->js, v);
    if (sl.len == 0u) return false;
    if (out_action) *out_action = sl;
    if (out_handler) *out_handler = INLAY_UI_NONE;
    return true;
}

bool inlay_ui_check_watchdog(InlaySurface* s) noexcept {
    return interrupt(s->js, s) != 0;
}

void inlay_ui_queue_event(InlaySurface* s, WidgetEvents* events, EtchStr name, uint32_t handler,
                          EtchStr payload, bool is_click) {
    const bool has_name = name.length > 0u && name.length < 64u;
    const bool has_handler = handler != INLAY_UI_NONE;
    if ((!has_name && !has_handler) || (!is_click && !payload.length) || payload.length > 65536u) return;
    auto* event = static_cast<WidgetEvent*>(
        etch_frame_arena_alloc(s->etch, sizeof(WidgetEvent), alignof(WidgetEvent)));
    if (!event) return;
    EtchStr copy = {};
    if (has_name) {
        copy = inlay_ui_frame_text(s, name);
        if (!copy.chars) return;
    }
    *event = {nullptr, copy, payload, handler, is_click};
    if (events->last) events->last->next = event;
    else events->first = event;
    events->last = event;
}

void inlay_ui_dispatch_box_interaction(InlaySurface* s, uint32_t index, uint32_t id, WidgetEvents* events) {
    const auto& st = s->styles[index];
    dispatch_box_interaction(s, st, inlay_ui_get_cold(s, st), get_core_extra(s, index), id, events);
}

EtchSizing inlay_ui_resolve_sizing(const InlaySurface* s, const NodeStyle& st, const ColdVisualStyle* cold,
                                   bool vertical, EtchSizing fallback) noexcept {
    const bool explicit_set = vertical ? ((st.flags2 & Nf2HExplicit) != 0u) : ((st.flags2 & Nf2WExplicit) != 0u);
    const auto kind = static_cast<EtchSizingKind>(vertical ? st.h_kind : st.w_kind);
    const float val = vertical ? st.h_val : st.w_val;
    const float min_px = cold ? (vertical ? cold->h_min : cold->w_min) : 0.f;
    const float max_px = cold ? (vertical ? cold->h_max : cold->w_max) : 0.f;

    EtchSizing r = fallback;
    if (explicit_set) {
        if (kind == ETCH_SIZING_GROW) {
            r = etch_sizing_grow(min_px, max_px);
        } else if (kind == ETCH_SIZING_FIXED) {
            float v = val;
            if (min_px > 0.f && v < min_px) v = min_px;
            if (max_px > 0.f && v > max_px) v = max_px;
            r = etch_sizing_fixed(v);
        } else if (kind == ETCH_SIZING_PERCENT) {
            r = etch_sizing_percent(val);
            r.min = min_px;
            r.max = max_px;
        } else {
            r = etch_sizing_fit(min_px, max_px);
        }
    } else if (s && (st.flags & NfHasFlexGrow) != 0u && st.flex_grow > 0.f &&
               (vertical == s->draw_parent_col)) {
        r = etch_sizing_grow(min_px, max_px);
    } else {
        if (min_px > 0.f) r.min = min_px;
        if (max_px > 0.f) r.max = max_px;
    }
    return r;
}

EtchCorners inlay_ui_resolve_corners(const NodeStyle& st, const ColdVisualStyle* cold,
                                     const EtchTheme& theme, float fallback) noexcept {
    if ((st.flags & NfRadiusSet) == 0u) return etch_corners_all(fallback);
    if (cold && (cold->cflags & CfPerCornerRadius)) {
        return {
            resolve_corner_val(cold->radius_vals.tl, cold->radius_tok[0], theme),
            resolve_corner_val(cold->radius_vals.tr, cold->radius_tok[1], theme),
            resolve_corner_val(cold->radius_vals.br, cold->radius_tok[2], theme),
            resolve_corner_val(cold->radius_vals.bl, cold->radius_tok[3], theme),
        };
    }
    return etch_corners_all(resolve_corner_val(st.radius_px, st.radius_tok, theme));
}

void inlay_ui_populate_box_desc(InlaySurface* s, const NodeStyle& st, const EtchTheme& theme,
                                uint32_t id, EtchBoxDesc& box, bool use_layer) noexcept {
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    const bool dark = inlay_is_dark_theme(theme);
    const EtchColor default_inset = dark ? etch_rgba8(0u, 0u, 0u, 120u) : etch_rgba8(15u, 23u, 42u, 38u);

    box.id = id;
    box.axis = (st.flags & NfColumn) ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
    box.width = inlay_ui_resolve_sizing(s, st, cold, false, box.width);
    box.height = inlay_ui_resolve_sizing(s, st, cold, true, box.height);
    if (st.flags & NfHasPadding) box.padding = st.padding;
    if (cold && (cold->cflags & CfHasMargin)) box.margin = cold->margin;
    if (st.flags & NfHasGap) box.gap = st.gap;
    if (cold && (cold->cflags & CfHasRowGap)) box.row_gap = cold->row_gap;
    if (st.flags & NfHasAlignX) box.align_x = static_cast<EtchAlignX>(st.align_x);
    if (st.flags & NfHasAlignY) box.align_y = static_cast<EtchAlignY>(st.align_y);
    if (st.flags & NfHasJustify) box.justify = static_cast<EtchJustify>(st.justify);
    if (st.flags & NfHasAlignSelf) box.align_self = static_cast<EtchAlignSelf>(st.align_self);
    if (st.flags & NfHasFlexGrow) box.flex_grow = st.flex_grow;
    if (st.flags & NfHasFlexShrink) box.flex_shrink = st.flex_shrink;
    if (st.flags & NfHasWrap) box.wrap = (st.flags & NfWrapFlag) != 0u;
    if (cold && (cold->cflags & CfHasAspectRatio)) box.aspect_ratio = cold->aspect_ratio;
    if (st.flags & NfShrinkWidth) box.shrink_width = true;
    if (st.flags & NfShrinkHeight) box.shrink_height = true;
    if (cold) {
        if (cold->grid_cols > 0u) box.grid_columns = cold->grid_cols;
        if (cold->col_span > 1u) box.grid_col_span = cold->col_span;
        if (cold->row_span > 1u) box.grid_row_span = cold->row_span;
    }

    const ColorRef eff_bg = inlay_ui_eff_bg(st, cold, dark);
    const ColorRef eff_border = inlay_ui_eff_border(st, cold, dark);
    if (eff_bg.set) box.background = inlay_ui_resolve_color(eff_bg, theme, box.background);
    if (st.flags & NfHasBorderWidth) box.border_width = std::fmax(0.f, st.border_width);
    if (cold && (cold->cflags & CfHasBorderSides) && has_border_sides(cold->border_sides)) {
        box.border_sides = cold->border_sides;
    }
    if (eff_border.set) {
        box.border_color = inlay_ui_resolve_color(eff_border, theme, theme.border_subtle);
    } else if ((box.border_width > 0.f || has_border_sides(box.border_sides)) && box.border_color.a == 0u) {
        box.border_color = theme.border_subtle;
    }
    if (st.flags & NfRadiusSet) box.radius = inlay_ui_resolve_corners(st, cold, theme, 0.f);
    if (cold && (cold->cflags & CfHasContinuousCorners)) {
        box.continuous_corners = (cold->cflags & CfContinuousCorners) != 0u;
    }

    if (cold) {
        if (cold->cflags & CfHasElevation) box.elevation = cold->elevation;
        if (cold->shadow_count > 0u) {
            const uint8_t sc = cold->shadow_count > 4u ? 4u : cold->shadow_count;
            box.shadow_count = sc;
            for (uint8_t i = 0u; i < sc; ++i) {
                const auto& sh = s->shadow_pool[cold->shadow_offset + i];
                auto sl = etch_shadow_layer_default();
                sl.offset = {sh.x, sh.y};
                sl.blur_px = std::fmax(0.f, sh.blur);
                sl.spread_px = sh.spread;
                sl.inset = sh.inset;
                const EtchColor fallback_sh = sh.inset ? default_inset : theme.shadow;
                sl.color = sh.color.set ? inlay_ui_resolve_color(sh.color, theme, fallback_sh) : fallback_sh;
                box.shadows[i] = sl;
            }
        }
        if (cold->cflags & CfInsetShadowBool) {
            box.shadow_inset = true;
            if (box.shadow_color.a == 0u) box.shadow_color = default_inset;
            if (box.shadow_blur <= 0.f) {
                box.shadow_blur = 8.f;
                box.shadow_offset = {0.f, 1.f};
            }
        }
        if (cold->gradient_idx != INLAY_UI_NONE && cold->gradient_idx < s->gradient_pool_count) {
            const auto& gs = s->gradient_pool[cold->gradient_idx];
            auto gd = etch_gradient_desc_default();
            gd.kind = gs.kind;
            gd.oklab = gs.oklab;
            gd.dir = {gs.dir_x, gs.dir_y};
            gd.center = {gs.center_x, gs.center_y};
            gd.angle_rad = gs.angle_rad;
            gd.stop_count = gs.stop_count;
            for (uint8_t i = 0u; i < gs.stop_count; ++i) {
                gd.stops[i].color = gs.stop_colors[i].set
                    ? inlay_ui_resolve_color(gs.stop_colors[i], theme, theme.accent)
                    : theme.accent;
                gd.stops[i].position = gs.stop_pos[i];
            }
            box.gradient = gd;
        }
        if (cold->backdrop_blur > 0.f) box.backdrop_blur = cold->backdrop_blur;
        if (cold->cflags & CfHasBackdropSat) box.backdrop_saturation = cold->backdrop_saturation;
        if (cold->cflags & CfHasBackdropBright) box.backdrop_brightness = cold->backdrop_brightness;
        if (cold->backdrop_noise > 0.f) box.backdrop_noise = cold->backdrop_noise;
        if (cold->cflags & CfHasTopRimAlpha) box.top_rim_alpha = cold->top_rim_alpha;
        if (cold->cflags & CfHasBlendMode) box.blend_mode = cold->blend_mode;

        if (cold->cflags & CfHasAttachTo) box.attach_to = cold->attach_to;
        if (cold->cflags & CfHasAnchorElement) box.anchor_element = cold->anchor_element;
        if (cold->cflags & CfHasAnchorParent) box.anchor_parent = cold->anchor_parent;
        if (cold->cflags & CfHasFloatingOffset) box.floating_offset = cold->floating_offset;
        if (cold->cflags & CfHasZIndex) box.z_index = cold->z_index;
        if (cold->cflags & CfPointerPassthrough) box.pointer_passthrough = true;
        if (cold->cflags & CfHasClampToViewport) {
            box.clamp_to_viewport = (cold->cflags & CfClampToViewport) != 0u;
        }
        if (cold->cflags & CfFlipOnOverflow) box.flip_on_overflow = true;
    }

    // State overrides (hover / press / focus) & automatic CSS-like transition interpolation.
    const bool disabled = (st.flags & NfDisabled) != 0u;
    const bool hov = !disabled && etch_hovered(id);
    const bool prs = !disabled && etch_pressed(id);
    const bool foc = !disabled && etch_focused(id);
    const bool explicit_trans = cold && (cold->cflags & CfHasTransition);
    const bool has_trans = explicit_trans && cold->transition.prop_mask != TransNone;
    const uint16_t tmask = has_trans ? cold->transition.prop_mask : 0u;

    // Background (TransBg)
    if (tmask & TransBg) {
        EtchColor target_bg = box.background;
        if (prs && cold->press_bg.set) {
            target_bg = inlay_ui_resolve_color(cold->press_bg, theme, target_bg);
        } else if (hov && cold->hover_bg.set) {
            target_bg = inlay_ui_resolve_color(cold->hover_bg, theme, target_bg);
        } else if (hov && box.hover_background.a != 0u) {
            target_bg = box.hover_background;
        }
        box.background = inlay_ui_tween_color(s, etch_id_index(id, 0x7201u), TransBg,
                                              target_bg, cold->transition);
        box.hover_background = {}; // Double-animation guard
    } else if (cold) {
        if (prs && cold->press_bg.set) {
            box.background = inlay_ui_resolve_color(cold->press_bg, theme, box.background);
            box.hover_background = {};
        } else if (cold->hover_bg.set) {
            box.hover_background = inlay_ui_resolve_color(cold->hover_bg, theme, box.hover_background);
        }
    }

    // Border color (TransBorder)
    if (tmask & TransBorder) {
        EtchColor target_bc = box.border_color;
        if (foc && cold->focus_border_color.set) {
            target_bc = inlay_ui_resolve_color(cold->focus_border_color, theme, target_bc);
        } else if (prs && cold->press_border_color.set) {
            target_bc = inlay_ui_resolve_color(cold->press_border_color, theme, target_bc);
        } else if (hov && cold->hover_border_color.set) {
            target_bc = inlay_ui_resolve_color(cold->hover_border_color, theme, target_bc);
        } else if (hov && box.hover_border_color.a != 0u) {
            target_bc = box.hover_border_color;
        }
        box.border_color = inlay_ui_tween_color(s, etch_id_index(id, 0x7203u), TransBorder,
                                                target_bc, cold->transition);
        box.hover_border_color = {}; // Double-animation guard
    } else if (cold) {
        if (foc && cold->focus_border_color.set) {
            box.border_color = inlay_ui_resolve_color(cold->focus_border_color, theme, box.border_color);
            box.hover_border_color = {};
        } else if (prs && cold->press_border_color.set) {
            box.border_color = inlay_ui_resolve_color(cold->press_border_color, theme, box.border_color);
            box.hover_border_color = {};
        } else if (cold->hover_border_color.set) {
            box.hover_border_color = inlay_ui_resolve_color(cold->hover_border_color, theme, box.hover_border_color);
        }
    }

    // Hover / press scale (TransTransform)
    if (tmask & TransTransform) {
        // Double-animation guard: layer transform already interpolated hover/press scale.
        box.hover_scale = 1.0f;
        box.press_scale = 1.0f;
    } else if (cold) {
        if (cold->cflags & CfHasHoverScale) box.hover_scale = cold->hover_scale;
        if (cold->cflags & CfHasPressScale) box.press_scale = cold->press_scale;
    }

    // Corner radius (TransLayout)
    if (tmask & TransLayout) {
        const bool uniform = (box.radius.tl == box.radius.tr &&
                              box.radius.tl == box.radius.br &&
                              box.radius.tl == box.radius.bl);
        if (uniform) {
            const float r = std::fmax(0.f, inlay_ui_tween_scalar(
                s, etch_id_index(id, 0x720Au), TransLayout, box.radius.tl, cold->transition));
            box.radius = etch_corners_all(r);
        } else {
            box.radius.tl = std::fmax(0.f, inlay_ui_tween_scalar(
                s, etch_id_index(id, 0x720Au), TransLayout, box.radius.tl, cold->transition));
            box.radius.tr = std::fmax(0.f, inlay_ui_tween_scalar(
                s, etch_id_index(id, 0x720Bu), TransLayout, box.radius.tr, cold->transition));
            box.radius.br = std::fmax(0.f, inlay_ui_tween_scalar(
                s, etch_id_index(id, 0x720Cu), TransLayout, box.radius.br, cold->transition));
            box.radius.bl = std::fmax(0.f, inlay_ui_tween_scalar(
                s, etch_id_index(id, 0x720Du), TransLayout, box.radius.bl, cold->transition));
        }
    }

    // When no explicit transition is set, default to snappy spring so hover/press smoothly spring.
    if (!explicit_trans) {
        box.transition = etch_spring_spec_snappy();
    }

    if (st.flags & NfHasClipChildren) box.clip_children = (st.flags & NfClipChildren) != 0u;
    box.animate_layout = (st.flags & NfAnimateLayout) != 0u;
    if (!use_layer) {
        if (hov && cold && (cold->cflags & CfHasHoverOpacity)) {
            box.opacity = cold->hover_opacity;
        } else if (st.flags & NfHasOpacity) {
            box.opacity = st.opacity;
        }
    }
    if (st.flags & NfHasScrollX) box.scroll_x = (st.flags & NfScrollX) != 0u;
    if (st.flags & NfHasScrollY) box.scroll_y = (st.flags & NfScrollY) != 0u;
    if ((box.scroll_x || box.scroll_y) && (st.flags & NfHasClipChildren) == 0u) {
        box.clip_children = true;
    }
}

void inlay_ui_draw_node(InlaySurface* s, uint32_t index, uint32_t parent,
                        const EtchTheme& theme, WidgetEvents* events) {
    const auto& n = s->nodes[index];
    const auto& st = s->styles[index];
    if ((n.kind == INLAY_UI_MODAL || n.kind == INLAY_UI_POPOVER) && (st.flags & NfOpen) == 0u) {
        return;
    }

    const uint32_t id = etch_id_index(parent, n.id);
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    const CoreNodeExtra* extra = get_core_extra(s, index);
    const bool dark = inlay_is_dark_theme(theme);
    const bool disabled = (st.flags & NfDisabled) != 0u;
    const bool explicit_trans = cold && (cold->cflags & CfHasTransition);
    const bool has_trans = explicit_trans && cold->transition.prop_mask != TransNone;
    const uint16_t tmask = has_trans ? cold->transition.prop_mask : 0u;

    // Step transform & opacity transitions unconditionally (identity-return guard).
    float tx = 0.f;
    float ty = 0.f;
    float sx = 1.f;
    float sy = 1.f;
    float rot = 0.f;
    if (cold && (cold->cflags & CfHasTransform)) {
        tx = cold->transform.tx;
        ty = cold->transform.ty;
        sx = cold->transform.sx;
        sy = cold->transform.sy;
        rot = cold->transform.rotation_rad;
    }
    if (tmask & TransTransform) {
        const bool hov = !disabled && etch_hovered(id);
        const bool prs = !disabled && etch_pressed(id);
        float state_sc = 1.f;
        if (prs && (cold->cflags & CfHasPressScale)) state_sc = cold->press_scale;
        else if (hov && (cold->cflags & CfHasHoverScale)) state_sc = cold->hover_scale;
        sx *= state_sc;
        sy *= state_sc;
        sx  = inlay_ui_tween_scalar(s, etch_id_index(id, 0x7205u), TransTransform, sx, cold->transition);
        sy  = inlay_ui_tween_scalar(s, etch_id_index(id, 0x7206u), TransTransform, sy, cold->transition);
        rot = inlay_ui_tween_scalar(s, etch_id_index(id, 0x7207u), TransTransform, rot, cold->transition);
        tx  = inlay_ui_tween_scalar(s, etch_id_index(id, 0x7208u), TransTransform, tx, cold->transition);
        ty  = inlay_ui_tween_scalar(s, etch_id_index(id, 0x7209u), TransTransform, ty, cold->transition);
        sx  = std::fmin(100.f, std::fmax(0.001f, sx));
        sy  = std::fmin(100.f, std::fmax(0.001f, sy));
    }

    float eff_opacity = (st.flags & NfHasOpacity) ? st.opacity : 1.f;
    if (!disabled && cold && (cold->cflags & CfHasHoverOpacity) && etch_hovered(id)) {
        eff_opacity = cold->hover_opacity;
    }
    if (tmask & TransOpacity) {
        eff_opacity = inlay_ui_tween_scalar(s, etch_id_index(id, 0x7204u), TransOpacity,
                                            eff_opacity, cold->transition);
        eff_opacity = std::fmin(1.f, std::fmax(0.f, eff_opacity));
    }

    const bool tf_non_identity =
        std::fabs(tx) > 1e-4f || std::fabs(ty) > 1e-4f ||
        std::fabs(sx - 1.f) > 1e-4f || std::fabs(sy - 1.f) > 1e-4f ||
        std::fabs(rot) > 1e-5f;
    const bool has_blend = cold && (cold->cflags & CfHasBlendMode) && cold->blend_mode != ETCH_BLEND_NORMAL;
    const bool has_alpha_mask =
        cold && (cold->cflags & CfHasAlphaMask) && cold->alpha_mask.kind != ETCH_ALPHA_MASK_NONE;
    const bool opacity_needs_layer =
        eff_opacity < 0.9999f && ((tmask & TransOpacity) != 0u || !box_handles_opacity(n.kind));
    const bool use_layer = tf_non_identity || has_blend || has_alpha_mask || opacity_needs_layer;

    if (use_layer) {
        auto layer = etch_layer_desc_default();
        layer.opacity = eff_opacity;
        if (tf_non_identity) {
            layer.translate = {tx, ty};
            layer.scale = {sx, sy};
            layer.rotation_rad = rot;
        }
        if (has_blend) layer.blend_mode = cold->blend_mode;
        if (has_alpha_mask) layer.alpha_mask = cold->alpha_mask;
        etch_push_layer(&layer);
    }

    if (inlay_ui_is_ext_widget(n.kind)) {
        inlay_ui_draw_ext_widget(s, index, id, parent, theme, use_layer, events);
    } else if (n.kind == INLAY_UI_BOX || n.kind == INLAY_UI_SCROLL ||
               n.kind == INLAY_UI_CARD || n.kind == INLAY_UI_POPOVER || n.kind == INLAY_UI_FORM) {
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
        } else if (n.kind == INLAY_UI_FORM) {
            box.gap = 10.f;
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
        if (((st.flags & NfClickable) != 0u ||
             (cold && has_event(cold->on_focus_action, cold->on_focus_handler))) && !disabled) {
            box.focusable = true;
        }
        inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);

        // #54 Form validation & grouped submit scope
        FormStateEntry* form_state = nullptr;
        ActiveFormField form_fields[64] = {};
        ActiveFormCtx form_ctx = {};
        ActiveFormCtx* prev_form = ext(s)->active_form;
        if (n.kind == INLAY_UI_FORM) {
            form_state = find_or_add_form_state(s, id);
            const uint64_t rhash = (extra && extra->field_name.len > 0u)
                ? inlay_fnv1a_64(s->text + extra->field_name.off, extra->field_name.len)
                : 0u;
            if (form_state->reset_key_hash != rhash) {
                form_state->reset_key_hash = rhash;
                form_state->show_errors = false;
            }
            form_ctx = {id, form_state->show_errors, false, form_fields, 0u, 64u};
            ext(s)->active_form = &form_ctx;
        }

        const uint8_t parent_consumed_before = ext(s)->child_consumed_mask;
        ext(s)->child_consumed_mask = 0u;
        etch_begin_box(&box);
        {
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = (box.axis == ETCH_AXIS_COL);
            for (uint32_t at = n.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                inlay_ui_draw_node(s, at, id, theme, events);
            }
            s->draw_parent_col = prev_col;
        }
        etch_end_box();
        const uint8_t descendants_consumed = ext(s)->child_consumed_mask;
        ext(s)->child_consumed_mask = static_cast<uint8_t>(parent_consumed_before | descendants_consumed);

        if (n.kind == INLAY_UI_FORM) {
            ext(s)->active_form = prev_form;
            if (form_ctx.submit_requested) {
                bool all_valid = true;
                uint32_t est_bytes = 16u;
                uint32_t invalid_bytes = 16u;
                for (uint32_t fi = 0u; fi < form_ctx.field_count; ++fi) {
                    est_bytes += (form_ctx.fields[fi].name.length + form_ctx.fields[fi].value.length) * 6u + 8u;
                    if (!form_ctx.fields[fi].valid) {
                        all_valid = false;
                        invalid_bytes += form_ctx.fields[fi].name.length * 6u + 4u;
                    }
                }
                if (all_valid) {
                    form_state->show_errors = false;
                    if (has_event(st.action, st.handler)) {
                        auto jw = arena_json_begin(s->etch, est_bytes);
                        arena_json_raw(jw, "{", 1u);
                        for (uint32_t fi = 0u; fi < form_ctx.field_count; ++fi) {
                            if (fi > 0u) arena_json_raw(jw, ",", 1u);
                            arena_json_quoted(jw, form_ctx.fields[fi].name);
                            arena_json_raw(jw, ":", 1u);
                            arena_json_quoted(jw, form_ctx.fields[fi].value);
                        }
                        arena_json_raw(jw, "}", 1u);
                        const EtchStr payload = arena_json_finish(jw);
                        if (payload.length > 0u) {
                            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler, payload, true);
                        }
                    }
                } else {
                    form_state->show_errors = true;
                    etch_request_redraw(s->etch);
                    for (uint32_t fi = 0u; fi < form_ctx.field_count; ++fi) {
                        if (!form_ctx.fields[fi].valid) {
                            EtchA11yOverrideDesc inv_ov = {};
                            inv_ov.element_id = form_ctx.fields[fi].element_id;
                            inv_ov.add_states = uint32_t(ETCH_A11Y_STATE_INVALID);
                            etch_set_a11y_override(&inv_ov);
                        }
                    }
                    if (extra && has_event(extra->field_error, extra->invalid_handler)) {
                        auto jw = arena_json_begin(s->etch, invalid_bytes);
                        arena_json_raw(jw, "[", 1u);
                        bool first_inv = true;
                        for (uint32_t fi = 0u; fi < form_ctx.field_count; ++fi) {
                            if (form_ctx.fields[fi].valid) continue;
                            if (!first_inv) arena_json_raw(jw, ",", 1u);
                            arena_json_quoted(jw, form_ctx.fields[fi].name);
                            first_inv = false;
                        }
                        arena_json_raw(jw, "]", 1u);
                        const EtchStr payload = arena_json_finish(jw);
                        if (payload.length > 0u) {
                            inlay_ui_queue_event(s, events, inlay_ui_slice(s, extra->field_error),
                                                 extra->invalid_handler, payload, false);
                        }
                    }
                }
            }
        } else if (n.kind == INLAY_UI_POPOVER) {
            if ((descendants_consumed & ConsumeEscape) == 0u && has_event(st.action, st.handler) &&
                etch_should_close_popover(id, box.attach_to == ETCH_ATTACH_ROOT ? 0u : parent)) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler, ETCH_STR_LIT("false"));
                ext(s)->child_consumed_mask |= ConsumeEscape;
            }
        } else {
            dispatch_box_interaction(s, st, cold, extra, id, events, descendants_consumed);
        }
    } else if (n.kind == INLAY_UI_SPACER) {
        auto box = etch_box_desc_default();
        box.id = id;
        if (st.flags2 & Nf2HasSpacerSize) {
            box.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_fixed(st.value));
            box.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(st.value));
        } else if ((st.flags2 & (Nf2WExplicit | Nf2HExplicit)) != 0u) {
            box.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_fit(0.f, 0.f));
            box.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fit(0.f, 0.f));
        } else {
            box.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
            box.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_grow(0.f, 0.f));
        }
        if (st.flags & NfHasFlexGrow) box.flex_grow = st.flex_grow;
        if (st.flags & NfHasFlexShrink) box.flex_shrink = st.flex_shrink;
        if (cold && (cold->cflags & CfHasMargin)) box.margin = cold->margin;
        etch_box(&box);
    } else if (n.kind == INLAY_UI_BUTTON) {
        const bool single_plain_child =
            n.text.length == 0u && n.first_child != INLAY_UI_NONE &&
            s->nodes[n.first_child].next_sibling == INLAY_UI_NONE &&
            is_plain_text_child(s, n.first_child);
        const bool has_custom_children = n.first_child != INLAY_UI_NONE && !single_plain_child;
        const EtchStr raw_label = n.text.length > 0u ? n.text
                                : single_plain_child ? s->nodes[n.first_child].text
                                                     : find_subtree_label(s, n.first_child);
        const EtchStr label = inlay_ui_frame_text(s, raw_label);

        const float border_w = (st.flags & NfHasBorderWidth) ? std::fmax(0.f, st.border_width) : 0.f;
        const bool sides = cold && (cold->cflags & CfHasBorderSides) && has_border_sides(cold->border_sides);
        const bool cold_rich = cold && (
            (cold->cflags & (CfHasElevation | CfInsetShadowBool | CfHasContinuousCorners |
                             CfHasTopRimAlpha | CfHasHoverScale | CfHasPressScale |
                             CfHasHoverOpacity | CfHasTransition | CfHasBlendMode |
                             CfHasMargin | CfHasCursor)) != 0u ||
            cold->shadow_count > 0u || cold->gradient_idx != INLAY_UI_NONE ||
            cold->backdrop_blur > 0.f || cold->hover_bg.set || cold->hover_border_color.set ||
            cold->hover_text_color.set || cold->press_bg.set || cold->press_border_color.set ||
            cold->focus_border_color.set || (cold->cflags & CfHasOpenType) != 0u);
        const bool extra_rich = extra && (has_event(extra->dbl_action, extra->dbl_handler) ||
                                          has_event(extra->rclick_action, extra->rclick_handler));
        const bool rich_box_btn =
            has_custom_children || border_w > 0.f || sides || cold_rich || extra_rich ||
            st.border_color.set || (st.flags & (NfHasFlexGrow | NfHasAlignSelf)) != 0u;

        if (rich_box_btn) {
            const auto variant = static_cast<EtchButtonVariant>(st.button_variant);
            EtchColor var_bg = theme.surface_control;
            EtchColor var_fg = theme.text_primary;
            EtchColor var_border = theme.border_control.a != 0u ? theme.border_control : theme.border_subtle;
            if (variant == ETCH_BUTTON_PRIMARY) {
                var_bg = theme.accent_primary.a != 0u ? theme.accent_primary : theme.accent;
                var_fg = theme.on_primary;
                var_border = etch_rgba8(255u, 255u, 255u, 28u);
            } else if (variant == ETCH_BUTTON_DANGER) {
                var_bg = theme.danger;
                var_fg = theme.on_error.a != 0u ? theme.on_error : theme.on_primary;
                var_border = etch_rgba8(255u, 255u, 255u, 28u);
            } else if (variant == ETCH_BUTTON_GHOST) {
                var_bg = etch_rgba8(0u, 0u, 0u, 0u);
                var_fg = theme.text_primary;
                var_border = etch_rgba8(0u, 0u, 0u, 0u);
            }
            const bool surface_btn = !disabled && variant != ETCH_BUTTON_GHOST;
            auto box = etch_box_desc_default();
            box.height = etch_sizing_fixed(32.f);
            box.padding = etch_padding_xy(14.f, 0.f);
            box.gap = 8.f;
            box.align_x = ETCH_ALIGN_X_CENTER;
            box.align_y = ETCH_ALIGN_Y_CENTER;
            box.background = disabled ? theme.surface_disabled : var_bg;
            box.border_color = disabled ? etch_rgba8(0u, 0u, 0u, 0u) : var_border;
            if (surface_btn && !sides && (st.flags & NfHasBorderWidth) == 0u) box.border_width = 1.f;
            box.radius = etch_corners_all(theme.radius_control_px);
            box.focusable = !disabled;
            if (!disabled && !(cold && cold->hover_bg.set)) {
                const ColorRef eff_bg = inlay_ui_eff_bg(st, cold, inlay_is_dark_theme(theme));
                const EtchColor base_bg = eff_bg.set ? inlay_ui_resolve_color(eff_bg, theme, var_bg) : var_bg;
                const EtchColor tint = (variant == ETCH_BUTTON_PRIMARY || variant == ETCH_BUTTON_DANGER)
                                           ? var_fg
                                           : theme.text_primary;
                box.hover_background = etch_color_blend_over(base_bg, {tint.r, tint.g, tint.b, 26u});
            }
            inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
            // Bug 3 fix: disabled rich buttons never let st.bg/hover/gradient overwrite surface_disabled!
            if (disabled) {
                box.background = theme.surface_disabled;
                box.border_color = etch_rgba8(0u, 0u, 0u, 0u);
                box.hover_background = {};
                box.hover_border_color = {};
                box.hover_scale = 1.0f;
                box.press_scale = 1.0f;
                box.gradient.kind = ETCH_GRADIENT_NONE;
                box.focusable = false;
            }
            etch_begin_box(&box);
            EtchA11yOverrideDesc ov = {};
            ov.element_id = id;
            ov.role = ETCH_A11Y_BUTTON;
            ov.label = label;
            ov.add_states = disabled ? uint32_t(ETCH_A11Y_STATE_DISABLED) : 0u;
            ov.actions_mask = disabled ? 0u : 3u;
            ov.has_role = true;
            ov.has_label = true;
            ov.has_actions_mask = true;
            etch_set_a11y_override(&ov);

            const EtchColor btn_fg = disabled
                ? theme.text_disabled
                : resolve_live_text_color(s, st, cold, id, theme, var_fg);
            const float btn_font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.f;
            const uint16_t btn_font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SANS);
            const uint32_t btn_ot = (cold && (cold->cflags & CfHasOpenType)) ? cold->opentype_flags : 0u;

            const uint8_t btn_parent_consumed = ext(s)->child_consumed_mask;
            ext(s)->child_consumed_mask = 0u;
            if (has_custom_children) {
                if (n.text.length > 0u) {
                    auto td = etch_text_desc_default();
                    td.font_size_px = btn_font_px;
                    td.font_id = btn_font_id;
                    td.color = btn_fg;
                    td.wrap = false;
                    td.opentype_flags = btn_ot;
                    etch_text(inlay_ui_frame_text(s, n.text), &td);
                }
                const bool prev_col = s->draw_parent_col;
                s->draw_parent_col = (box.axis == ETCH_AXIS_COL);
                for (uint32_t at = n.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                    if (is_plain_text_child(s, at)) {
                        auto td = etch_text_desc_default();
                        td.font_size_px = btn_font_px;
                        td.font_id = btn_font_id;
                        td.color = btn_fg;
                        td.wrap = false;
                        td.opentype_flags = btn_ot;
                        etch_text(inlay_ui_frame_text(s, s->nodes[at].text), &td);
                    } else {
                        inlay_ui_draw_node(s, at, id, theme, events);
                    }
                }
                s->draw_parent_col = prev_col;
            } else {
                auto td = etch_text_desc_default();
                td.font_size_px = btn_font_px;
                td.font_id = btn_font_id;
                td.color = btn_fg;
                td.wrap = false;
                td.opentype_flags = btn_ot;
                etch_text(label, &td);
            }
            etch_end_box();
            const uint8_t btn_desc_consumed = ext(s)->child_consumed_mask;
            ext(s)->child_consumed_mask = static_cast<uint8_t>(btn_parent_consumed | btn_desc_consumed);
            if (!disabled) {
                if (etch_hovered(id)) {
                    etch_set_cursor((cold && (cold->cflags & CfHasCursor)) ? cold->cursor : ETCH_CURSOR_HAND);
                }
                const bool activated = (btn_desc_consumed & ConsumeClick) == 0u && is_activated(id);
                if (activated) {
                    ext(s)->child_consumed_mask |= ConsumeClick;
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                         ETCH_STR_LIT("null"), true);
                    if (extra && (extra->flags & CefButtonSubmit) != 0u && ext(s)->active_form) {
                        ext(s)->active_form->submit_requested = true;
                    }
                }
                if ((btn_desc_consumed & ConsumeDblClick) == 0u && extra &&
                    has_event(extra->dbl_action, extra->dbl_handler) && etch_double_clicked(id)) {
                    ext(s)->child_consumed_mask |= ConsumeDblClick;
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, extra->dbl_action),
                                         extra->dbl_handler, ETCH_STR_LIT("null"), true);
                }
                if ((btn_desc_consumed & ConsumeRClick) == 0u && extra &&
                    has_event(extra->rclick_action, extra->rclick_handler) && etch_right_clicked(id)) {
                    ext(s)->child_consumed_mask |= ConsumeRClick;
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, extra->rclick_action),
                                         extra->rclick_handler, ETCH_STR_LIT("null"), true);
                }
                dispatch_hover_focus(s, cold, id, events);
            }
        } else {
            auto button = etch_button_desc_default();
            button.id = id;
            button.label = label;
            button.variant = static_cast<EtchButtonVariant>(st.button_variant);
            button.disabled = disabled;
            if (has_sizing_override(s, st, cold, false)) {
                button.width = inlay_ui_resolve_sizing(s, st, cold, false, button.width);
            }
            if (has_sizing_override(s, st, cold, true)) {
                button.height = inlay_ui_resolve_sizing(s, st, cold, true, button.height);
            }
            if (st.flags & NfHasPadding) button.padding = st.padding;
            if ((st.flags & NfHasSize) && st.font_size > 0.f) button.font_size_px = st.font_size;
            if (st.flags & NfHasFontId) button.font_id = st.font_id;
            if (st.flags & NfRadiusSet) {
                button.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_control_px);
            }
            if (st.flags & NfHasAlignX) button.align_x = static_cast<EtchAlignX>(st.align_x);
            // Bug 3 fix: override variant's actual theme colors (primary -> accent_primary/on_primary, danger -> danger/on_error).
            const bool btn_dark = inlay_is_dark_theme(theme);
            const ColorRef btn_bg = inlay_ui_eff_bg(st, cold, btn_dark);
            const ColorRef btn_fg = inlay_ui_eff_text(st, cold, btn_dark);
            const bool custom_theme = btn_bg.set || btn_fg.set;
            if (custom_theme) {
                EtchTheme btn_theme = theme;
                if (btn_bg.set) {
                    if (button.variant == ETCH_BUTTON_PRIMARY) {
                        const EtchColor c = inlay_ui_resolve_color(btn_bg, theme, theme.accent_primary);
                        btn_theme.accent_primary = c;
                        btn_theme.accent = c;
                    } else if (button.variant == ETCH_BUTTON_DANGER) {
                        btn_theme.danger = inlay_ui_resolve_color(btn_bg, theme, theme.danger);
                    } else {
                        btn_theme.surface_control = inlay_ui_resolve_color(btn_bg, theme, theme.surface_control);
                    }
                }
                if (btn_fg.set) {
                    if (button.variant == ETCH_BUTTON_PRIMARY) {
                        btn_theme.on_primary = inlay_ui_resolve_color(btn_fg, theme, theme.on_primary);
                    } else if (button.variant == ETCH_BUTTON_DANGER) {
                        const EtchColor c = inlay_ui_resolve_color(btn_fg, theme, theme.on_error);
                        btn_theme.on_error = c;
                        btn_theme.on_primary = c;
                    } else {
                        btn_theme.text_primary = inlay_ui_resolve_color(btn_fg, theme, theme.text_primary);
                    }
                }
                etch_push_theme(&btn_theme);
            }
            const bool pressed = etch_button(&button);
            if (custom_theme) etch_pop_theme();
            if (pressed) {
                ext(s)->child_consumed_mask |= ConsumeClick;
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                     ETCH_STR_LIT("null"), true);
                if (extra && (extra->flags & CefButtonSubmit) != 0u && ext(s)->active_form) {
                    ext(s)->active_form->submit_requested = true;
                }
            } else if (!disabled && extra && (extra->flags & CefStopPropagation) != 0u && etch_clicked(id)) {
                ext(s)->child_consumed_mask |= ConsumeClick;
            }
            if (!disabled) {
                dispatch_hover_focus(s, cold, id, events);
            }
        }
    } else if (n.kind == INLAY_UI_CODE_EDITOR) {
        const uint32_t i = inlay_ui_content_index(s, n.kind, n.text);
        if (i < s->content_count) {
            auto editor = etch_code_editor_desc_default();
            editor.id = id;
            editor.buffer = static_cast<EtchTextBuffer*>(s->content[i].resource);
            editor.width = inlay_ui_resolve_sizing(s, st, cold, false, editor.width);
            editor.height = inlay_ui_resolve_sizing(s, st, cold, true, editor.height);
            if (st.flags & NfHasSize) editor.font_size_px = st.font_size;
            if (st.flags & NfHasFontId) editor.font_id = st.font_id;
            if (st.flags & NfHasPadding) editor.padding = st.padding;
            editor.read_only = extra && (extra->flags & CefInputReadOnly);
            editor.word_wrap = extra && (extra->flags & CefInputWordWrap);
            if (extra && (extra->flags & CefEditorHasGhost) != 0u) {
                etch_code_editor_set_inline_completion(id, inlay_ui_frame_text(s, inlay_ui_slice(s, extra->field_name)));
            }
            if (extra && (extra->flags & CefEditorHasSearch) != 0u) {
                etch_code_editor_set_search_highlights(id, inlay_ui_frame_text(s, inlay_ui_slice(s, extra->field_error)), false);
            }
            const auto edit = etch_code_editor(&editor);
            if (edit.changed || etch_clicked(id)) ext(s)->child_consumed_mask |= ConsumeClick;
            if (edit.changed && has_event(st.action, st.handler)) {
                char payload[128];
                const int len = std::snprintf(payload, sizeof(payload),
                    "{\"cursorByte\":%u,\"anchorByte\":%u,\"byteLength\":%u,\"lineCount\":%u}",
                    edit.cursor_byte, edit.selection_anchor_byte, etch_text_buffer_byte_length(editor.buffer), edit.total_lines);
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                     inlay_ui_frame_text(s, {payload, uint32_t(len), false}));
            }
        }
    } else if (n.kind == INLAY_UI_INPUT) {
        auto input = etch_text_input_desc_default();
        input.id = id;
        if (cold && cold->placeholder.len > 0u) {
            input.placeholder = inlay_ui_frame_text(s, inlay_ui_slice(s, cold->placeholder));
        }
        if (has_sizing_override(s, st, cold, false)) {
            input.width = inlay_ui_resolve_sizing(s, st, cold, false, input.width);
        }
        if (has_sizing_override(s, st, cold, true)) {
            input.height = inlay_ui_resolve_sizing(s, st, cold, true, input.height);
        }
        if (st.flags & NfHasPadding) input.padding = st.padding;
        input.font_size_px = st.font_size > 0.f ? st.font_size : 15.f;
        if (st.flags & NfHasFontId) input.font_id = st.font_id;
        const bool inp_dark = inlay_is_dark_theme(theme);
        const ColorRef inp_bg = inlay_ui_eff_bg(st, cold, inp_dark);
        const ColorRef inp_fg = inlay_ui_eff_text(st, cold, inp_dark);
        const ColorRef inp_bd = inlay_ui_eff_border(st, cold, inp_dark);
        if (inp_bg.set) input.background = inlay_ui_resolve_color(inp_bg, theme, input.background);
        if (inp_fg.set) input.text_color = inlay_ui_resolve_color(inp_fg, theme, input.text_color);
        if (inp_bd.set) input.border_color = inlay_ui_resolve_color(inp_bd, theme, input.border_color);
        if (cold && cold->hover_border_color.set) {
            input.hover_border_color = inlay_ui_resolve_color(cold->hover_border_color, theme, input.hover_border_color);
        }
        if (cold && cold->focus_border_color.set) {
            input.focus_border_color = inlay_ui_resolve_color(cold->focus_border_color, theme, input.focus_border_color);
        }
        if (st.flags & NfHasBorderWidth) input.border_width = std::fmax(0.f, st.border_width);
        if (st.flags & NfRadiusSet) input.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_input_px);
        if (cold && (cold->cflags & CfHasContinuousCorners)) {
            input.continuous_corners = (cold->cflags & CfContinuousCorners) != 0u;
        }
        input.disabled = disabled;
        input.password = extra && (extra->flags & CefInputPassword);
        input.multiline = extra && (extra->flags & CefInputMultiline);
        input.read_only = extra && (extra->flags & CefInputReadOnly);
        input.word_wrap = extra && (extra->flags & CefInputWordWrap);
        ActiveFormCtx* active_form = ext(s)->active_form;
        const bool has_own_submit = extra && has_event(extra->submit_action, extra->submit_handler);
        input.submit_on_enter = has_own_submit || (active_form != nullptr && !input.multiline);
        const EtchStr edit_text = input.password ? inlay_ui_slice(s, extra->password_text) : n.text;
        uint32_t length = edit_text.length;
        if (length >= INLAY_UI_INPUT_CAPACITY) {
            length = INLAY_UI_INPUT_CAPACITY - 1u;
            while (length > 0u && (static_cast<uint8_t>(edit_text.chars[length]) & 0xC0u) == 0x80u) {
                --length;
            }
        }
        if (length > 0u) std::memcpy(s->edit_buffer, edit_text.chars, length);
        s->edit_buffer[length] = '\0';
        input.buffer = s->edit_buffer;
        input.capacity = INLAY_UI_INPUT_CAPACITY;

        char pre_err_buf[96] = {};
        EtchStr pre_err_str = {};
        const bool pre_valid = validate_input_field(s, extra, s->edit_buffer, length, pre_err_buf, &pre_err_str);
        const bool explicit_err = extra && extra->field_error.len > 0u;
        const bool show_field_err = explicit_err || (!pre_valid && active_form && active_form->show_errors);
        if (show_field_err) {
            input.border_color = theme.danger;
            input.hover_border_color = theme.danger;
            input.focus_border_color = theme.danger;
            auto wrap = etch_box_desc_default();
            wrap.axis = ETCH_AXIS_COL;
            wrap.width = input.width;
            wrap.height = etch_sizing_fit(0.f, 0.f);
            wrap.gap = 4.f;
            etch_begin_box(&wrap);
            if (wrap.width.kind != ETCH_SIZING_FIT) {
                input.width = etch_sizing_grow(0.f, 0.f);
            }
        }

        const auto edit = etch_text_input(&input);
        if (show_field_err) {
            EtchA11yOverrideDesc inv_ov = {};
            inv_ov.element_id = id;
            inv_ov.add_states = uint32_t(ETCH_A11Y_STATE_INVALID);
            etch_set_a11y_override(&inv_ov);
            if (pre_err_str.length > 0u) {
                auto etd = etch_text_desc_default();
                etd.font_size_px = 11.5f;
                etd.color = theme.danger;
                etd.wrap = true;
                etch_text(inlay_ui_frame_text(s, pre_err_str), &etd);
            }
            etch_end_box();
        }
        if (edit.changed || edit.submitted || etch_clicked(id)) {
            ext(s)->child_consumed_mask |= ConsumeClick;
        }
        if (active_form && active_form->field_count < active_form->field_cap) {
            char post_err_buf[96] = {};
            EtchStr post_err_str = {};
            const bool post_valid = validate_input_field(s, extra, s->edit_buffer, edit.byte_length, post_err_buf, &post_err_str);
            EtchStr field_key = {};
            if (extra && extra->field_name.len > 0u) {
                field_key = inlay_ui_frame_text(s, inlay_ui_slice(s, extra->field_name));
            } else {
                char fb[24] = {};
                const int fn = std::snprintf(fb, sizeof(fb), "field_%u", active_form->field_count);
                field_key = fn > 0 ? inlay_ui_frame_text(s, {fb, static_cast<uint32_t>(fn), false}) : ETCH_STR_LIT("field");
            }
            const EtchStr field_val = inlay_ui_frame_text(s, {s->edit_buffer, edit.byte_length, false});
            active_form->fields[active_form->field_count++] = {id, field_key, field_val, post_valid};
        }
        if (edit.changed && has_event(st.action, st.handler)) {
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                 input_json(s->etch, s->edit_buffer, edit.byte_length));
        }
        if (edit.submitted) {
            if (has_own_submit) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, extra->submit_action), extra->submit_handler,
                                     input_json(s->etch, s->edit_buffer, edit.byte_length), true);
            } else if (active_form) {
                active_form->submit_requested = true;
            }
        }
    } else if (n.kind == INLAY_UI_TOGGLE) {
        auto toggle = etch_toggle_switch_desc_default();
        bool checked = (st.flags & NfChecked) != 0u;
        toggle.id = id;
        toggle.label = inlay_ui_frame_text(s, n.text);
        toggle.value = &checked;
        toggle.disabled = disabled;
        toggle.font_size_px = st.font_size;
        if (st.flags & NfHasFontId) toggle.font_id = st.font_id;
        if (st.color.set) toggle.on_color = inlay_ui_resolve_color(st.color, theme, theme.accent);
        if (st.border_color.set) {
            toggle.border_color = inlay_ui_resolve_color(st.border_color, theme, toggle.border_color);
        }
        if (extra) {
            if (extra->flags & CefSwitchHasTrackW) toggle.track_width_px = extra->aux_f0;
            if (extra->flags & CefSwitchHasTrackH) toggle.track_height_px = extra->aux_f1;
            if (extra->color_a.set) toggle.off_color = inlay_ui_resolve_color(extra->color_a, theme, toggle.off_color);
            if (extra->color_b.set) toggle.thumb_color = inlay_ui_resolve_color(extra->color_b, theme, toggle.thumb_color);
        }
        if (etch_toggle_switch(&toggle)) {
            ext(s)->child_consumed_mask |= ConsumeClick;
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                 checked ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
        }
    } else if (n.kind == INLAY_UI_CHECKBOX) {
        auto cb = etch_checkbox_desc_default();
        bool checked = (st.flags & NfChecked) != 0u;
        cb.id = id;
        cb.label = inlay_ui_frame_text(s, n.text);
        cb.checked = &checked;
        cb.indeterminate = (st.flags & NfIndeterminate) != 0u;
        cb.disabled = disabled;
        if ((st.flags & NfHasSize) && st.font_size > 0.f) cb.font_size_px = st.font_size;
        if (st.flags & NfHasFontId) cb.font_id = st.font_id;
        if (st.color.set) cb.accent = inlay_ui_resolve_color(st.color, theme, theme.accent);
        if (etch_checkbox(&cb)) {
            ext(s)->child_consumed_mask |= ConsumeClick;
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                 checked ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
        }
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
        inlay_ui_populate_box_desc(s, st, theme, id, track, use_layer);
        track.axis = ETCH_AXIS_ROW;
        etch_begin_box(&track);
        EtchA11yOverrideDesc list_ov = {};
        list_ov.element_id = id;
        list_ov.role = ETCH_A11Y_TAB_LIST;
        list_ov.has_role = true;
        etch_set_a11y_override(&list_ov);

        const bool stretch_tabs = (st.flags2 & Nf2WExplicit) != 0u && st.w_kind != uint8_t(ETCH_SIZING_FIT);
        const float font_px = ((st.flags & NfHasSize) && st.font_size > 0.f) ? st.font_size : 13.f;
        const EtchCorners pill_r = (st.flags & NfRadiusSet)
            ? inlay_ui_resolve_corners(st, cold, theme, theme.radius_control_px)
            : etch_corners_all(theme.radius_control_px);
        const bool cont_c = (cold && (cold->cflags & CfHasContinuousCorners))
            ? ((cold->cflags & CfContinuousCorners) != 0u)
            : true;
        const uint32_t item_off = cold ? cold->item_offset : 0u;
        const uint16_t dis_mask = extra ? extra->tabs_disabled : 0u;
        for (uint32_t i = 0u; i < st.item_count; ++i) {
            const uint32_t tab_id = etch_id_index(id, 0x7AB0u + i);
            const bool tab_disabled = disabled || (i < 16u && ((dis_mask & uint16_t(1u << i)) != 0u));
            const bool active = int32_t(i) == st.int_value;
            const EtchStr label = inlay_ui_frame_text(s, s->item_pool[item_off + i]);
            auto tab = etch_box_desc_default();
            tab.id = tab_id;
            tab.width = stretch_tabs ? etch_sizing_grow(0.f, 0.f) : etch_sizing_fit(0.f, 0.f);
            tab.height = etch_sizing_fit(0.f, 0.f);
            tab.padding = etch_padding_xy(12.f, 5.f);
            tab.align_x = ETCH_ALIGN_X_CENTER;
            tab.align_y = ETCH_ALIGN_Y_CENTER;
            tab.radius = pill_r;
            tab.continuous_corners = cont_c;
            tab.focusable = !tab_disabled;
            if (active) {
                tab.background = dark ? theme.surface_active : theme.bg_elevated;
                tab.border_width = 1.f;
                tab.border_color = dark ? theme.border_control : theme.border_subtle;
                if (!dark) tab.elevation = ETCH_ELEVATION_SM;
                else tab.top_rim_alpha = 0.12f;
            } else {
                tab.background = etch_color_transparent();
                if (!tab_disabled) {
                    tab.hover_background = dark ? etch_rgba8(255u, 255u, 255u, 14u)
                                                : etch_rgba8(15u, 23u, 42u, 12u);
                }
            }
            etch_begin_box(&tab);
            EtchA11yOverrideDesc tab_ov = {};
            tab_ov.element_id = tab_id;
            tab_ov.role = ETCH_A11Y_TAB;
            tab_ov.label = label;
            tab_ov.add_states = (active ? uint32_t(ETCH_A11Y_STATE_SELECTED) : 0u) |
                                (tab_disabled ? uint32_t(ETCH_A11Y_STATE_DISABLED) : 0u);
            tab_ov.actions_mask = tab_disabled ? 0u : 3u;
            tab_ov.has_role = true;
            tab_ov.has_label = true;
            tab_ov.has_actions_mask = true;
            etch_set_a11y_override(&tab_ov);
            auto td = etch_text_desc_default();
            td.font_size_px = font_px;
            td.font_id = (st.flags & NfHasFontId)
                ? st.font_id
                : static_cast<uint16_t>(active ? ETCH_FONT_SEMIBOLD : ETCH_FONT_SANS);
            td.color = tab_disabled
                ? theme.text_disabled
                : (active ? (st.color.set ? inlay_ui_resolve_color(st.color, theme, theme.text_primary)
                                          : theme.text_primary)
                          : theme.text_secondary);
            td.wrap = false;
            etch_text(label, &td);
            etch_end_box();
            if (!tab_disabled) {
                if (etch_hovered(tab_id)) etch_set_cursor(ETCH_CURSOR_HAND);
                if (is_activated(tab_id)) {
                    ext(s)->child_consumed_mask |= ConsumeClick;
                    if (!active) {
                        char buf[16] = {};
                        const int len = std::snprintf(buf, sizeof(buf), "%u", i);
                        if (len > 0) {
                            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                                 inlay_ui_frame_text(s, {buf, uint32_t(len), false}));
                        }
                    }
                }
            }
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_COMBOBOX) {
        auto cb = etch_combo_box_desc_default();
        cb.id = id;
        const EtchStr ph = (cold && cold->placeholder.len > 0u) ? inlay_ui_slice(s, cold->placeholder) : n.text;
        cb.label = inlay_ui_frame_text(s, ph);
        int selected = st.int_value;
        cb.selected_index = &selected;
        if (st.item_count > 0u && cold) {
            auto* items = static_cast<EtchStr*>(
                etch_frame_arena_alloc(s->etch, size_t(st.item_count) * sizeof(EtchStr), alignof(EtchStr)));
            if (items) {
                for (uint32_t i = 0u; i < st.item_count; ++i) {
                    items[i] = inlay_ui_frame_text(s, s->item_pool[cold->item_offset + i]);
                }
                cb.items = items;
                cb.item_count = st.item_count;
            }
        }
        if (has_sizing_override(s, st, cold, false)) {
            cb.width = inlay_ui_resolve_sizing(s, st, cold, false, cb.width);
        }
        if ((st.flags2 & Nf2HExplicit) && st.h_kind == uint8_t(ETCH_SIZING_FIXED) && st.h_val > 0.f) {
            cb.height_px = st.h_val;
        }
        cb.disabled = disabled;
        if (st.bg.set) cb.background = inlay_ui_resolve_color(st.bg, theme, cb.background);
        if (cold && cold->hover_bg.set) cb.hover_bg = inlay_ui_resolve_color(cold->hover_bg, theme, cb.hover_bg);
        if (st.color.set) cb.text_color = inlay_ui_resolve_color(st.color, theme, cb.text_color);
        if (st.border_color.set) cb.border_color = inlay_ui_resolve_color(st.border_color, theme, cb.border_color);
        if (cold && cold->focus_border_color.set) {
            cb.focus_border_color = inlay_ui_resolve_color(cold->focus_border_color, theme, cb.focus_border_color);
        }
        if (st.flags & NfHasBorderWidth) cb.border_width = std::fmax(0.f, st.border_width);
        if (cb.item_count > 0u && etch_combo_box(&cb)) {
            ext(s)->child_consumed_mask |= ConsumeClick;
            char buf[16] = {};
            const int len = std::snprintf(buf, sizeof(buf), "%d", selected);
            if (len > 0) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                     inlay_ui_frame_text(s, {buf, uint32_t(len), false}));
            }
        }
    } else if (n.kind == INLAY_UI_MODAL) {
        auto m = etch_modal_desc_default();
        bool open = true;
        m.id = id;
        m.open = &open;
        m.title = inlay_ui_frame_text(s, n.text);
        if ((st.flags2 & Nf2WExplicit) && st.w_kind == uint8_t(ETCH_SIZING_FIXED) && st.w_val > 0.f) {
            m.width_px = st.w_val;
        }
        if (has_sizing_override(s, st, cold, true)) {
            m.height = inlay_ui_resolve_sizing(s, st, cold, true, m.height);
        }
        if (st.bg.set) m.background = inlay_ui_resolve_color(st.bg, theme, m.background);
        if (st.border_color.set) m.border_color = inlay_ui_resolve_color(st.border_color, theme, m.border_color);
        if (st.flags & NfRadiusSet) m.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_panel_px);
        if (cold && (cold->cflags & CfHasZIndex)) m.z_index = cold->z_index;
        if (etch_begin_modal(&m)) {
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = true;
            for (uint32_t at = n.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                inlay_ui_draw_node(s, at, id, theme, events);
            }
            s->draw_parent_col = prev_col;
            etch_end_modal();
        } else if (!open) {
            inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler, ETCH_STR_LIT("false"));
        }
    } else if (n.kind == INLAY_UI_TOOLTIP) {
        auto box = etch_box_desc_default();
        box.id = id;
        box.axis = (st.flags & NfColumn) ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        box.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_fit(0.f, 0.f));
        box.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fit(0.f, 0.f));
        if (st.flags & NfHasGap) box.gap = st.gap;
        if (cold && (cold->cflags & CfHasMargin)) box.margin = cold->margin;
        if (st.flags & NfHasAlignSelf) box.align_self = static_cast<EtchAlignSelf>(st.align_self);
        etch_begin_box(&box);
        {
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = (box.axis == ETCH_AXIS_COL);
            for (uint32_t at = n.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                inlay_ui_draw_node(s, at, id, theme, events);
            }
            s->draw_parent_col = prev_col;
        }
        etch_end_box();
        if (n.text.length > 0u) etch_set_tooltip_text(id, inlay_ui_frame_text(s, n.text));
    } else if (n.kind == INLAY_UI_SPLIT) {
        auto sp = etch_split_desc_default();
        sp.id = id;
        sp.axis = (st.flags & NfColumn) ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        sp.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
        sp.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_grow(0.f, 0.f));
        const bool has_min = (st.flags2 & Nf2HasMin) != 0u && cold != nullptr;
        const bool has_max = (st.flags2 & Nf2HasMax) != 0u && cold != nullptr;
        float ratio = (st.flags2 & Nf2HasValue) ? st.value : 0.5f;
        const float min_r = has_min ? std::fmin(0.95f, std::fmax(0.02f, cold->min_val)) : 0.0f;
        const float max_r = has_max ? std::fmin(0.98f, std::fmax(0.05f, cold->max_val)) : 1.0f;
        if (has_min && ratio < min_r) ratio = min_r;
        if (has_max && ratio > max_r) ratio = max_r;
        ratio = std::fmin(0.98f, std::fmax(0.02f, ratio));
        sp.default_ratio = ratio;

        // Programmatic ratio sync without clobbering user drags or triggering idle redraws.
        SplitSyncEntry* sync = find_or_add_split_sync(s, id, ratio);
        if ((st.flags2 & Nf2HasValue) && std::fabs(ratio - sync->last_prop_ratio) > 1e-4f) {
            sync->last_prop_ratio = ratio;
            if (std::fabs(etch_split_ratio(id) - ratio) > 1e-4f) {
                etch_set_split_ratio(id, ratio);
                sync->last_live_ratio = ratio;
            }
        }

        if (has_min || has_max) {
            const auto prev_b = etch_element_bounds(id);
            const float span = (sp.axis == ETCH_AXIS_ROW)
                ? (prev_b.w > 1.f ? prev_b.w : (st.w_kind == uint8_t(ETCH_SIZING_FIXED) ? st.w_val : 0.f))
                : (prev_b.h > 1.f ? prev_b.h : (st.h_kind == uint8_t(ETCH_SIZING_FIXED) ? st.h_val : 0.f));
            const float avail = span - sp.divider_size_px;
            if (avail > 1.f) {
                if (has_min) sp.min_first_px = std::fmax(0.f, min_r * avail);
                if (has_max) sp.min_second_px = std::fmax(0.f, (1.f - max_r) * avail);
            } else {
                if (has_min) sp.min_first_px = 0.f;
                if (has_max) sp.min_second_px = 0.f;
            }
        }
        const EtchSplitResult res = etch_begin_split(&sp);
        {
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = true;
            const uint32_t first = n.first_child;
            if (first != INLAY_UI_NONE) inlay_ui_draw_node(s, first, id, theme, events);
            etch_split_second();
            const uint32_t second = first != INLAY_UI_NONE ? s->nodes[first].next_sibling : INLAY_UI_NONE;
            if (second != INLAY_UI_NONE) inlay_ui_draw_node(s, second, id, theme, events);
            s->draw_parent_col = prev_col;
        }
        etch_end_split();

        if (std::fabs(res.ratio - sync->last_live_ratio) > 1e-4f) {
            sync->last_live_ratio = res.ratio;
            if (has_event(st.action, st.handler)) {
                char buf[32] = {};
                const int len = std::snprintf(buf, sizeof(buf), "%.6g", double(res.ratio));
                if (len > 0) {
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                         inlay_ui_frame_text(s, {buf, uint32_t(len), false}));
                }
            }
        }
    } else if (n.kind == INLAY_UI_MATH) {
        const EtchStr copied = inlay_ui_frame_text(s, n.text);
        const float font_px =
            std::fmin(256.f, std::fmax(6.f, (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 18.f));
        const EtchColor fg = st.color.set ? inlay_ui_resolve_color(st.color, theme, theme.text_primary)
                                          : theme.text_primary;
        InlayMathPrepared prep = {};
        const bool ok = copied.length > 0u &&
                        inlay_math_prepare(copied, font_px, (st.flags & NfDisplayMath) != 0u, &prep);
        auto box = etch_box_desc_default();
        if (ok) {
            const float pad_w = (st.flags & NfHasPadding) ? (st.padding.left + st.padding.right) : 0.f;
            const float pad_h = (st.flags & NfHasPadding) ? (st.padding.top + st.padding.bottom) : 0.f;
            box.width = etch_sizing_fixed(std::fmax(1.f, prep.metrics.width + pad_w));
            box.height = etch_sizing_fixed(std::fmax(1.f, prep.metrics.height + pad_h));
        }
        inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
        etch_begin_box(&box);
        if (ok) {
            auto* payload = static_cast<UiMathDraw*>(
                etch_frame_arena_alloc(s->etch, sizeof(UiMathDraw), alignof(UiMathDraw)));
            if (payload) {
                *payload = {
                    prep, fg, box.padding,
                    static_cast<EtchAlignX>(st.align_x),
                    static_cast<EtchAlignY>(st.align_y),
                    (st.flags & NfHasAlignX) != 0u,
                    (st.flags & NfHasAlignY) != 0u
                };
                etch_draw_after_layout(draw_ui_math, payload, false);
            }
        } else if (copied.length > 0u) {
            auto td = etch_text_desc_default();
            td.font_size_px = font_px;
            td.color = fg;
            etch_text(copied, &td);
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_DIAGRAM) {
        const EtchStr copied = inlay_ui_frame_text(s, n.text);
        auto ddesc = inlay_diagram_desc_default();
        ddesc.font_size_px =
            std::fmin(64.f, std::fmax(8.f, (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 14.f));
        const float pad_w = (st.flags & NfHasPadding) ? (st.padding.left + st.padding.right) : 0.f;
        const float pad_h = (st.flags & NfHasPadding) ? (st.padding.top + st.padding.bottom) : 0.f;
        const bool fixed_w = (st.flags2 & Nf2WExplicit) != 0u && st.w_kind == uint8_t(ETCH_SIZING_FIXED);
        const bool flex_w = (st.flags2 & Nf2WExplicit) != 0u &&
                            (st.w_kind == uint8_t(ETCH_SIZING_GROW) || st.w_kind == uint8_t(ETCH_SIZING_PERCENT));
        if (fixed_w && st.w_val > pad_w) {
            ddesc.max_width_px = st.w_val - pad_w;
        } else if (cold && cold->w_max > pad_w) {
            ddesc.max_width_px = cold->w_max - pad_w;
        } else if (flex_w) {
            const auto prev_b = etch_element_bounds(id);
            if (prev_b.w > pad_w + 16.f) {
                ddesc.max_width_px = prev_b.w - pad_w;
            } else if (prev_b.w <= 0.f) {
                // Bug 6 fix: schedule one follow-up frame so layout width converges on frame 1.
                etch_request_redraw(s->etch);
            }
        }
        InlayDiagramMeta meta = {};
        const auto res = copied.length > 0u ? inlay_diagram_layout_ex(copied, &ddesc, &meta)
                                            : InlayDiagramResult{};
        auto box = etch_box_desc_default();
        if (res.valid) {
            box.width = etch_sizing_fixed(std::fmax(1.f, res.width + pad_w));
            box.height = etch_sizing_fixed(std::fmax(1.f, res.height + pad_h));
        }
        if (flex_w) box.clip_children = true;
        inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
        etch_begin_box(&box);
        if (res.valid) {
            auto dbox = etch_box_desc_default();
            dbox.width = etch_sizing_fixed(std::fmax(1.f, res.width));
            dbox.height = etch_sizing_fixed(std::fmax(1.f, res.height));
            etch_begin_box(&dbox);
            auto* payload = static_cast<UiDiagramDraw*>(
                etch_frame_arena_alloc(s->etch, sizeof(UiDiagramDraw), alignof(UiDiagramDraw)));
            if (payload) {
                *payload = {res, meta, ddesc};
                etch_draw_after_layout(draw_ui_diagram, payload, false);
            }
            etch_end_box();
        } else if (copied.length > 0u) {
            auto td = etch_text_desc_default();
            td.font_size_px = ddesc.font_size_px;
            td.color = theme.text_muted;
            etch_text(copied, &td);
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_SLOT) {
        SlotBinding* binding = nullptr;
        for (uint32_t i = 0u; i < s->slot_count; ++i) {
            if (inlay_ui_equal(n.text, s->slots[i].name)) {
                binding = &s->slots[i];
                break;
            }
        }
        const EtchStr pjson = (cold && cold->placeholder.len > 0u)
            ? inlay_ui_frame_text(s, inlay_ui_slice(s, cold->placeholder))
            : EtchStr{};

        auto box = etch_box_desc_default();
        inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
        const auto prev_b = etch_element_bounds(id);

        // #53 First-frame native intrinsic measurement via slot measure_callback
        const bool explicit_w = has_sizing_override(s, st, cold, false);
        const bool explicit_h = has_sizing_override(s, st, cold, true);
        if (binding && binding->measure_callback && (!explicit_w || !explicit_h)) {
            const float pad_x = box.padding.left + box.padding.right;
            const float pad_y = box.padding.top + box.padding.bottom;
            const EtchRect par_b = parent ? etch_element_bounds(parent) : EtchRect{};
            float avail_w = 0.f;
            if (box.width.kind == ETCH_SIZING_FIXED && box.width.value > 0.f) avail_w = std::fmax(0.f, box.width.value - pad_x);
            else if (prev_b.w > 0.f) avail_w = std::fmax(0.f, prev_b.w - pad_x);
            else if (par_b.w > 0.f) avail_w = std::fmax(0.f, par_b.w - pad_x);
            else if (s->last_view_width_px > 0.f) avail_w = std::fmax(0.f, s->last_view_width_px - pad_x);

            float avail_h = 0.f;
            if (box.height.kind == ETCH_SIZING_FIXED && box.height.value > 0.f) avail_h = std::fmax(0.f, box.height.value - pad_y);
            else if (prev_b.h > 0.f) avail_h = std::fmax(0.f, prev_b.h - pad_y);
            else if (par_b.h > 0.f) avail_h = std::fmax(0.f, par_b.h - pad_y);

            EtchVec2 measured = {0.f, 0.f};
            if (binding->measure_callback(s, id, avail_w, avail_h, pjson, &measured, binding->user)) {
                if (!explicit_w && std::isfinite(measured.x) && measured.x > 0.f) {
                    box.width = etch_sizing_fixed(measured.x + pad_x);
                }
                if (!explicit_h && std::isfinite(measured.y) && measured.y > 0.f) {
                    box.height = etch_sizing_fixed(measured.y + pad_y);
                }
            }
        }

        const bool fixed_w = (box.width.kind == ETCH_SIZING_FIXED && box.width.value > 0.f);
        const bool fixed_h = (box.height.kind == ETCH_SIZING_FIXED && box.height.value > 0.f);
        // Bug 6 fix: non-fixed slot reading 0 bounds on frame 1 requests a follow-up layout pass.
        if ((!fixed_w && prev_b.w <= 0.f) || (!fixed_h && prev_b.h <= 0.f)) {
            etch_request_redraw(s->etch);
        }
        float eff_w = fixed_w ? box.width.value : prev_b.w;
        float eff_h = fixed_h ? box.height.value : prev_b.h;
        if (eff_w > 0.f) eff_w = std::fmax(0.f, eff_w - box.padding.left - box.padding.right);
        if (eff_h > 0.f) eff_h = std::fmax(0.f, eff_h - box.padding.top - box.padding.bottom);

        etch_begin_box(&box);
        if (binding) {
            if (binding->a11y_role != ETCH_A11Y_NONE) {
                const EtchStr a11y_lbl = binding->a11y_label[0]
                    ? inlay_ui_frame_text(s, {binding->a11y_label, static_cast<uint32_t>(std::strlen(binding->a11y_label)), false})
                    : inlay_ui_frame_text(s, n.text);
                etch_emit_a11y_node(id, binding->a11y_role, a11y_lbl, {}, 0u, 0u, 0.f, {});
                EtchA11yOverrideDesc ov = {};
                ov.element_id = id;
                ov.role = binding->a11y_role;
                ov.label = a11y_lbl;
                ov.has_role = true;
                ov.has_label = a11y_lbl.length > 0u;
                etch_set_a11y_override(&ov);
            }
            binding->callback(s, id, eff_w, eff_h, pjson, binding->user);
        } else {
            const uint32_t cap = n.text.length + 12u;
            auto* buf = static_cast<char*>(etch_frame_arena_alloc(s->etch, cap, 1u));
            if (buf) {
                const int len = std::snprintf(buf, cap, "[slot: %.*s]", int(n.text.length),
                                              n.text.chars ? n.text.chars : "");
                if (len > 0) {
                    auto td = etch_text_desc_default();
                    td.font_size_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 12.f;
                    td.color = theme.text_muted;
                    td.wrap = false;
                    etch_text({buf, uint32_t(len), false}, &td);
                }
            }
        }
        etch_end_box();
    } else if (n.kind == INLAY_UI_PROGRESS) {
        const float min_v = ((st.flags2 & Nf2HasMin) && cold) ? cold->min_val : 0.f;
        const float max_v = ((st.flags2 & Nf2HasMax) && cold) ? cold->max_val : 1.f;
        const float denom = max_v - min_v;
        const float raw = (st.flags2 & Nf2HasValue) ? st.value : 0.f;
        float frac = denom > 0.f ? std::fmin(1.f, std::fmax(0.f, (raw - min_v) / denom)) : 0.f;
        const bool animated = extra && (extra->flags & CefProgressAnimated) != 0u;
        const bool indet = extra && (extra->flags & CefProgressIndeterm) != 0u;
        const bool is_ring = extra && (extra->flags & CefProgressRing) != 0u;
        const bool show_pct = extra && (extra->flags & CefProgressShowPercent) != 0u;

        if (animated && !indet) {
            frac = std::fmin(1.f, std::fmax(0.f, etch_spring(
                etch_id_index(id, 0x9B02u), frac, etch_spring_spec_fluid())));
        }

        EtchStr overlay = inlay_ui_frame_text(s, n.text);
        if (!overlay.length && show_pct && !indet) {
            char pbuf[16] = {};
            const int pct = static_cast<int>(std::round(frac * 100.f));
            const int len = std::snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
            if (len > 0) overlay = inlay_ui_frame_text(s, {pbuf, uint32_t(len), false});
        }

        const EtchColor track_col = (cold && cold->track_color.set)
            ? inlay_ui_resolve_color(cold->track_color, theme, theme.surface_subtle)
            : (st.bg.set ? inlay_ui_resolve_color(st.bg, theme, theme.surface_subtle)
                         : theme.surface_subtle);
        const EtchColor fill_col = st.color.set
            ? inlay_ui_resolve_color(st.color, theme, theme.accent)
            : theme.accent;

        if (is_ring) {
            float d = 48.f;
            if ((st.flags2 & Nf2WExplicit) && st.w_kind == uint8_t(ETCH_SIZING_FIXED) && st.w_val > 0.f) {
                d = st.w_val;
            } else if ((st.flags2 & Nf2HExplicit) && st.h_kind == uint8_t(ETCH_SIZING_FIXED) && st.h_val > 0.f) {
                d = st.h_val;
            }
            const float thick = (extra && (extra->flags & CefProgressHasThick))
                ? extra->aux_f0
                : ((st.flags & NfHasBorderWidth) && st.border_width > 0.f
                       ? st.border_width
                       : std::fmax(3.f, std::round(d * 0.11f)));
            constexpr float kTwoPi = 6.28318530718f;
            constexpr float kTopRad = -1.57079632679f;
            float start_a = kTopRad;
            float sweep_a = frac * kTwoPi;
            if (indet) {
                bool reduced = false;
                const float ph = eval_indet_phase(s, id, &reduced);
                start_a = kTopRad + (reduced ? 0.f : ph * kTwoPi);
                sweep_a = 1.85f;
            }

            auto box = etch_box_desc_default();
            box.id = id;
            box.width = etch_sizing_fixed(d);
            box.height = etch_sizing_fixed(d);
            box.align_x = ETCH_ALIGN_X_CENTER;
            box.align_y = ETCH_ALIGN_Y_CENTER;
            if (cold && (cold->cflags & CfHasMargin)) box.margin = cold->margin;
            if (st.flags & NfHasAlignSelf) box.align_self = static_cast<EtchAlignSelf>(st.align_self);
            etch_begin_box(&box);

            auto ring = etch_ring_desc_default();
            ring.center = {d * 0.5f, d * 0.5f};
            ring.radius = std::fmax(2.f, (d - thick) * 0.5f);
            ring.thickness = thick;
            ring.start_angle_rad = start_a;
            ring.sweep_angle_rad = sweep_a;
            ring.color = fill_col;
            ring.track_color = track_col;
            ring.round_caps = true;
            etch_draw_ring(&ring);

            if (overlay.length > 0u) {
                auto td = etch_text_desc_default();
                td.font_size_px = (st.flags & NfHasSize) && st.font_size > 0.f
                    ? st.font_size
                    : std::fmax(10.f, std::round(d * 0.24f));
                td.font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SEMIBOLD);
                td.opentype_flags = ETCH_OT_TABULAR_NUMS;
                td.color = theme.text_primary;
                td.wrap = false;
                etch_text(overlay, &td);
            }
            etch_end_box();
        } else if (indet) {
            bool reduced = false;
            const float ph = eval_indet_phase(s, id, &reduced);
            auto def_pb = etch_progress_bar_desc_default();
            auto track = etch_box_desc_default();
            track.id = id;
            track.width = has_sizing_override(s, st, cold, false)
                ? inlay_ui_resolve_sizing(s, st, cold, false, def_pb.width)
                : def_pb.width;
            track.height = has_sizing_override(s, st, cold, true)
                ? inlay_ui_resolve_sizing(s, st, cold, true, def_pb.height)
                : def_pb.height;
            track.background = track_col;
            track.radius = (st.flags & NfRadiusSet)
                ? inlay_ui_resolve_corners(st, cold, theme, theme.radius_pill_px)
                : etch_corners_all(theme.radius_pill_px);
            track.clip_children = true;
            track.align_x = ETCH_ALIGN_X_CENTER;
            track.align_y = ETCH_ALIGN_Y_CENTER;
            if (cold && (cold->cflags & CfHasMargin)) track.margin = cold->margin;
            if (st.flags & NfHasFlexGrow) track.flex_grow = st.flex_grow;
            if (st.flags & NfHasAlignSelf) track.align_self = static_cast<EtchAlignSelf>(st.align_self);
            etch_begin_box(&track);
            auto* payload = static_cast<UiIndetBarDraw*>(
                etch_frame_arena_alloc(s->etch, sizeof(UiIndetBarDraw), alignof(UiIndetBarDraw)));
            if (payload) {
                *payload = {ph, fill_col, track.radius, reduced};
                etch_draw_after_layout(draw_ui_indet_bar, payload, false);
            }
            if (overlay.length > 0u) {
                auto td = etch_text_desc_default();
                td.font_size_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 11.f;
                td.color = theme.text_primary;
                td.wrap = false;
                etch_text(overlay, &td);
            }
            etch_end_box();
        } else {
            auto pb = etch_progress_bar_desc_default();
            pb.id = id;
            pb.fraction = frac;
            pb.overlay_text = overlay;
            if (has_sizing_override(s, st, cold, false)) {
                pb.width = inlay_ui_resolve_sizing(s, st, cold, false, pb.width);
            }
            if (has_sizing_override(s, st, cold, true)) {
                pb.height = inlay_ui_resolve_sizing(s, st, cold, true, pb.height);
            }
            if (st.flags & NfRadiusSet) {
                pb.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_pill_px);
            }
            if ((cold && cold->track_color.set) || st.bg.set) pb.track_color = track_col;
            if (st.color.set) pb.fill_color = fill_col;
            if ((st.flags & NfHasSize) && st.font_size > 0.f) pb.font_size_px = st.font_size;
            etch_progress_bar(&pb);
        }
    } else if (n.kind == INLAY_UI_SLIDER) {
        auto sl = etch_slider_desc_default();
        float val = (st.flags2 & Nf2HasValue) ? st.value : 0.f;
        sl.id = id;
        sl.label = inlay_ui_frame_text(s, n.text);
        sl.value = &val;
        sl.min_value = ((st.flags2 & Nf2HasMin) && cold) ? cold->min_val : 0.f;
        sl.max_value = ((st.flags2 & Nf2HasMax) && cold) ? cold->max_val : 1.f;
        if (sl.max_value <= sl.min_value) sl.max_value = sl.min_value + 1.f;
        sl.step = ((st.flags2 & Nf2HasStep) && cold && cold->step >= 0.f) ? cold->step : 0.f;
        sl.disabled = disabled;
        if (has_sizing_override(s, st, cold, false)) {
            sl.width = inlay_ui_resolve_sizing(s, st, cold, false, sl.width);
        }
        if ((st.flags2 & Nf2HExplicit) && st.h_kind == uint8_t(ETCH_SIZING_FIXED) && st.h_val > 0.f) {
            sl.height_px = st.h_val;
        }
        if (st.bg.set) sl.background = inlay_ui_resolve_color(st.bg, theme, sl.background);
        if (cold && cold->track_color.set) {
            sl.track_color = inlay_ui_resolve_color(cold->track_color, theme, sl.track_color);
        }
        if (st.color.set) sl.fill_color = inlay_ui_resolve_color(st.color, theme, sl.fill_color);
        if (etch_slider(&sl)) {
            ext(s)->child_consumed_mask |= ConsumeClick;
            if (has_event(st.action, st.handler)) {
                char buf[32] = {};
                const int len = std::snprintf(buf, sizeof(buf), "%.9g", double(val));
                if (len > 0) {
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                         inlay_ui_frame_text(s, {buf, uint32_t(len), false}));
                }
            }
        }
    } else if (n.kind == INLAY_UI_BADGE) {
        const uint8_t variant = extra ? extra->badge_variant : uint8_t(BadgeDefault);
        const bool has_dot = extra && (extra->flags & CefBadgeDot) != 0u;
        const bool has_content = n.text.length > 0u || n.first_child != INLAY_UI_NONE;
        const BadgeColors bc = resolve_badge_variant(variant, theme, dark);

        if (has_dot && !has_content) {
            auto dot = etch_box_desc_default();
            dot.width = etch_sizing_fixed(8.f);
            dot.height = etch_sizing_fixed(8.f);
            dot.radius = etch_corners_all(4.f);
            dot.background = st.bg.set ? inlay_ui_resolve_color(st.bg, theme, bc.dot)
                           : st.color.set ? inlay_ui_resolve_color(st.color, theme, bc.dot)
                                          : bc.dot;
            inlay_ui_populate_box_desc(s, st, theme, id, dot, use_layer);
            etch_box(&dot);
            dispatch_box_interaction(s, st, cold, extra, id, events);
        } else {
            auto box = etch_box_desc_default();
            box.padding = etch_padding_xy(8.f, 3.f);
            box.gap = 5.f;
            box.align_x = ETCH_ALIGN_X_CENTER;
            box.align_y = ETCH_ALIGN_Y_CENTER;
            box.background = bc.bg;
            box.border_color = bc.border;
            box.border_width = st.border_color.set ? 1.f : bc.border_w;
            box.radius = etch_corners_all(theme.radius_pill_px);
            if ((st.flags & NfClickable) != 0u && !disabled) box.focusable = true;
            inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
            etch_begin_box(&box);

            const EtchColor fg = resolve_live_text_color(s, st, cold, id, theme, bc.fg);
            const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 12.f;
            const uint16_t font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_MEDIUM);
            const uint32_t ot = (cold && (cold->cflags & CfHasOpenType)) ? cold->opentype_flags : 0u;

            if (has_dot) {
                auto dot = etch_box_desc_default();
                dot.width = etch_sizing_fixed(6.f);
                dot.height = etch_sizing_fixed(6.f);
                dot.radius = etch_corners_all(3.f);
                dot.background = st.color.set ? fg : bc.dot;
                etch_box(&dot);
            }
            if (n.text.length > 0u) {
                auto td = etch_text_desc_default();
                td.font_size_px = font_px;
                td.font_id = font_id;
                td.color = fg;
                td.wrap = false;
                td.opentype_flags = ot;
                etch_text(inlay_ui_frame_text(s, n.text), &td);
            }
            const uint8_t bdg_parent_consumed = ext(s)->child_consumed_mask;
            ext(s)->child_consumed_mask = 0u;
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = (box.axis == ETCH_AXIS_COL);
            for (uint32_t at = n.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                if (is_plain_text_child(s, at)) {
                    auto td = etch_text_desc_default();
                    td.font_size_px = font_px;
                    td.font_id = font_id;
                    td.color = fg;
                    td.wrap = false;
                    td.opentype_flags = ot;
                    etch_text(inlay_ui_frame_text(s, s->nodes[at].text), &td);
                } else {
                    inlay_ui_draw_node(s, at, id, theme, events);
                }
            }
            s->draw_parent_col = prev_col;
            etch_end_box();
            const uint8_t bdg_desc_consumed = ext(s)->child_consumed_mask;
            ext(s)->child_consumed_mask = static_cast<uint8_t>(bdg_parent_consumed | bdg_desc_consumed);
            dispatch_box_interaction(s, st, cold, extra, id, events, bdg_desc_consumed);
        }
    } else if (n.kind == INLAY_UI_DIVIDER) {
        auto box = etch_box_desc_default();
        box.id = id;
        if ((st.flags & NfColumn) == 0u) {
            box.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
            box.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(1.f));
        } else {
            box.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_fixed(1.f));
            box.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_grow(0.f, 0.f));
        }
        if (cold && (cold->cflags & CfHasMargin)) box.margin = cold->margin;
        box.background = st.color.set ? inlay_ui_resolve_color(st.color, theme, theme.border_line)
                       : st.bg.set    ? inlay_ui_resolve_color(st.bg, theme, theme.border_line)
                                      : theme.border_line;
        if ((st.flags & NfHasOpacity) && !use_layer) box.opacity = st.opacity;
        if (cold && (cold->cflags & CfHasBlendMode)) box.blend_mode = cold->blend_mode;
        etch_box(&box);
    } else if (n.kind == INLAY_UI_IMAGE) {
        InlayImageResult resolved = {};
        const bool has_resolved = !(st.flags2 & Nf2HasSprite) && s->image_resolver_fn && cold && cold->src.len > 0u &&
            s->image_resolver_fn(n.text, inlay_ui_slice(s, cold->src), &resolved, s->image_resolver_user_data) &&
            resolved.texture != nullptr && std::isfinite(resolved.width_px) && std::isfinite(resolved.height_px) &&
            resolved.width_px > 0.f && resolved.height_px > 0.f;
        if (((st.flags2 & Nf2HasSprite) && cold && cold->sprite_id != ETCH_INVALID_SPRITE) || has_resolved) {
            auto img = etch_image_desc_default();
            img.id = id;
            if (has_resolved) {
                img.texture = resolved.texture;
                img.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_fixed(resolved.width_px));
                img.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(resolved.height_px));
                if (resolved.radius_px > 0.f && !(st.flags & NfRadiusSet)) {
                    img.radius = etch_corners_all(resolved.radius_px);
                }
            } else {
                img.sprite_id = cold->sprite_id;
                img.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_fit(0.f, 0.f));
                img.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fit(0.f, 0.f));
            }
            if (st.flags & NfRadiusSet) img.radius = inlay_ui_resolve_corners(st, cold, theme, 0.f);
            if (st.color.set) img.tint = inlay_ui_resolve_color(st.color, theme, etch_color_white(255u));
            if (st.border_color.set) img.border_color = inlay_ui_resolve_color(st.border_color, theme, {});
            if (st.flags & NfHasBorderWidth) img.border_width = std::fmax(0.f, st.border_width);
            if (cold->cflags & CfHasAspectRatio) img.aspect_ratio = cold->aspect_ratio;
            if (cold->col_span > 1u) img.grid_col_span = cold->col_span;
            if (cold->cflags & CfHasContinuousCorners) {
                img.continuous_corners = (cold->cflags & CfContinuousCorners) != 0u;
            } else if (has_resolved) {
                img.continuous_corners = true;
            }
            if (cold->cflags & CfHasBlendMode) img.blend_mode = cold->blend_mode;
            img.fit = static_cast<EtchImageFit>(st.image_fit);
            etch_image(&img);
            dispatch_box_interaction(s, st, cold, extra, id, events);
        } else if ((st.flags2 & Nf2HasIcon) && st.icon_id != ETCH_INVALID_ICON) {
            auto ic = etch_icon_desc_default();
            ic.id = id;
            ic.icon_id = st.icon_id;
            ic.size_px = ((st.flags2 & Nf2WExplicit) && st.w_kind == uint8_t(ETCH_SIZING_FIXED) && st.w_val > 0.f)
                ? st.w_val
                : ((st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 24.f);
            ic.color = resolve_live_text_color(s, st, cold, id, theme, theme.text_primary);
            etch_icon(&ic);
            dispatch_box_interaction(s, st, cold, extra, id, events);
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
            if ((st.flags & NfClickable) != 0u && !disabled) box.focusable = true;
            inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
            etch_begin_box(&box);
            const EtchStr label = n.text.length > 0u ? n.text
                                : (cold && cold->src.len > 0u ? inlay_ui_slice(s, cold->src) : EtchStr{});
            if (label.length > 0u) {
                auto td = etch_text_desc_default();
                td.font_size_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 12.f;
                td.color = st.color.set ? inlay_ui_resolve_color(st.color, theme, theme.text_muted)
                                        : theme.text_muted;
                td.wrap = false;
                etch_text(inlay_ui_frame_text(s, label), &td);
            }
            etch_end_box();
            dispatch_box_interaction(s, st, cold, extra, id, events);
        }
    } else {
        // INLAY_UI_TEXT
        const bool need_wrapper =
            (cold && (cold->cflags & CfHasMargin)) ||
            (st.flags & (NfHasFlexGrow | NfHasAlignSelf)) != 0u;
        if (need_wrapper) {
            auto wbox = etch_box_desc_default();
            wbox.id = etch_id_index(id, 0x7E01u);
            const bool fg = (st.flags & NfHasFlexGrow) != 0u && st.flex_grow > 0.f;
            wbox.width = (fg && !s->draw_parent_col) ? etch_sizing_grow(0.f, 0.f) : etch_sizing_fit(0.f, 0.f);
            wbox.height = (fg && s->draw_parent_col) ? etch_sizing_grow(0.f, 0.f) : etch_sizing_fit(0.f, 0.f);
            if (cold && (cold->cflags & CfHasMargin)) wbox.margin = cold->margin;
            if (st.flags & NfHasFlexGrow) wbox.flex_grow = st.flex_grow;
            if (st.flags & NfHasAlignSelf) wbox.align_self = static_cast<EtchAlignSelf>(st.align_self);
            etch_begin_box(&wbox);
        }

        auto td = etch_text_desc_default();
        td.id = id;
        td.font_size_px = st.font_size;
        td.color = resolve_live_text_color(s, st, cold, id, theme, theme.text_primary);
        if (st.flags & NfHasFontId) td.font_id = st.font_id;
        if (st.flags & NfHasWrap) td.wrap = (st.flags & NfWrapFlag) != 0u;
        if (cold && (cold->cflags & CfHasLineHeight)) td.line_height_px = cold->line_height;
        if (cold && (cold->cflags & CfHasLetterSpacing)) td.letter_spacing = cold->letter_spacing;
        if (cold && (cold->cflags & CfHasOpenType)) td.opentype_flags = cold->opentype_flags;
        if (st.flags & NfHasAlignX) td.align_x = static_cast<EtchAlignX>(st.align_x);
        if (st.flags & NfEllipsis) td.ellipsis = true;
        if (st.max_lines > 0u) td.max_lines = st.max_lines;
        if (extra && (extra->flags & CefTextSelectable)) td.selectable = true;
        if (extra && extra->color_a.set) {
            td.selection_color = inlay_ui_resolve_color(extra->color_a, theme, theme.selection);
        }
        etch_text(inlay_ui_frame_text(s, n.text), &td);

        if (need_wrapper) etch_end_box();
        if (cold && (cold->cflags & CfHasCursor) && etch_hovered(id)) {
            etch_set_cursor(cold->cursor);
        }
    }

    if (use_layer) etch_pop_layer();
}

// ============================================================================
// Public C ABI Surface Lifecycle & Rendering
// ============================================================================

extern "C" {
INLAY_API InlaySurface* inlay_surface_create(EtchContext* context, const InlaySurfaceDesc* desc) {
    InlaySurfaceDesc d = inlay_surface_desc_default();
    if (desc && !inlay_read_abi_struct(desc, &d)) return nullptr;
    if (d.max_text_bytes == 0u) d.max_text_bytes = kDefaultMaxTextBytes;
    if (!context || d.memory_limit_bytes < 512u * 1024u ||
        d.memory_limit_bytes > 256u * 1024u * 1024u || d.max_stack_bytes < 16384u ||
        d.max_stack_bytes > 512u * 1024u || !std::isfinite(d.max_tick_ms) || d.max_tick_ms <= 0.f ||
        d.max_tick_ms > 50.f || !d.max_nodes || d.max_nodes > 65536u ||
        d.max_text_bytes > kHardMaxTextBytes) {
        return nullptr;
    }

    auto* storage = static_cast<SurfaceStorage*>(SDL_calloc(1u, sizeof(SurfaceStorage)));
    if (!storage) return nullptr;
    InlaySurface* s = &storage->pub;
    s->etch = context;
    s->desc = d;
    s->frequency = double(SDL_GetPerformanceFrequency());
    storage->last_hmr_check_ms = -1000.0;
    for (auto& timer : s->timers) timer.function = JS_UNDEFINED;

    const uint32_t init_nodes = d.max_nodes < 256u ? d.max_nodes : 256u;
    const uint32_t init_text = d.max_text_bytes < kInitTextBytes ? d.max_text_bytes : kInitTextBytes;
    s->nodes = static_cast<InlayUiNode*>(SDL_calloc(init_nodes, sizeof(InlayUiNode)));
    s->styles = static_cast<NodeStyle*>(SDL_calloc(init_nodes, sizeof(NodeStyle)));
    storage->node_extra_idx = static_cast<uint32_t*>(SDL_malloc(size_t(init_nodes) * sizeof(uint32_t)));
    if (storage->node_extra_idx) {
        for (uint32_t i = 0u; i < init_nodes; ++i) storage->node_extra_idx[i] = INLAY_UI_NONE;
    }
    s->node_capacity = init_nodes;

    const bool text_ok = inlay_grow_pod(s->text, storage->text_capacity, init_text, init_text, d.max_text_bytes);
    s->json_scratch = static_cast<char*>(SDL_malloc(65537u));
    s->runtime = JS_NewRuntime();
    if (!s->nodes || !s->styles || !storage->node_extra_idx || !text_ok || !s->text ||
        !s->json_scratch || !s->runtime || !save_state(s, "{}", 2u)) {
        inlay_surface_destroy(s);
        return nullptr;
    }
    JS_SetMemoryLimit(s->runtime, d.memory_limit_bytes);
    JS_SetMaxStackSize(s->runtime, d.max_stack_bytes);
    JS_SetInterruptHandler(s->runtime, interrupt, s);
    JS_SetHostPromiseRejectionTracker(s->runtime, rejection_tracker, s);
    s->js = JS_NewContext(s->runtime);
    if (!s->js) {
        inlay_surface_destroy(s);
        return nullptr;
    }
    JS_SetContextOpaque(s->js, s);
    storage->atom_memo_key = JS_NewAtom(s->js, "__memoKey");
    storage->atom_memo_rev = JS_NewAtom(s->js, "__memoRev");
    return s;
}

INLAY_API void inlay_surface_destroy(InlaySurface* s) {
    if (!s) return;
    auto* storage = ext(s);
    SDL_free(s->content);
    clear_context(s);
    SDL_free(storage->action_policies);
    SDL_free(storage->async_actions);
    SDL_free(storage->async_action_names);
    SDL_free(storage->reload_error);
    if (s->runtime) JS_FreeRuntime(s->runtime);
    inlay_ui_free_widget_caches(s);
    SDL_free(s->handlers);
    SDL_free(s->nodes);
    SDL_free(s->styles);
    SDL_free(s->cold_visuals);
    SDL_free(s->shadow_pool);
    SDL_free(s->gradient_pool);
    SDL_free(s->item_pool);
    SDL_free(s->color_tweens);
    SDL_free(s->cls_cache);
    SDL_free(s->cls_cache_strs);
    SDL_free(s->jsx_buf);
    SDL_free(s->text);
    SDL_free(s->json_scratch);
    SDL_free(s->source);
    SDL_free(s->state);
    SDL_free(storage->node_extra_idx);
    SDL_free(storage->core_extras);
    SDL_free(storage);
}

INLAY_API bool inlay_surface_set_script(InlaySurface* s, EtchStr script) {
    if (!s || s->running || s->drawing || (!script.chars && script.length) ||
        script.length > kMaxScriptBytes) {
        return false;
    }
    return load(s, script, false);
}

INLAY_API bool inlay_surface_set_state_json(InlaySurface* s, EtchStr json) {
    if (!s || !json.chars || !json.length || json.length > 65536u) return false;
    if (!s->js) return false;
    const JSValue parsed = parse_json(s, json);
    if (JS_IsException(parsed)) {
        const JSValue exception = JS_GetException(s->js);
        JS_FreeValue(s->js, exception);
        record_error(s, "Invalid state JSON");
        return false;
    }
    JS_FreeValue(s->js, parsed);
    if (!save_state(s, json.chars, json.length)) return false;
    s->pending_state = true;
    s->state_dirty = false;
    etch_request_redraw(s->etch);
    return s->running || s->drawing || !s->loaded ? true : run_update(s, {}, {});
}

INLAY_API EtchStr inlay_surface_state_json(const InlaySurface* s) {
    if (!s) return {};
    if (s->state_dirty) sync_state_json(const_cast<InlaySurface*>(s));
    return EtchStr{s->state, s->state_size, false};
}

INLAY_API bool inlay_surface_bind_action(InlaySurface* s, EtchStr name, InlayActionFn fn, void* user) {
    if (!s || !name.chars || !name.length || name.length >= 64u || !fn ||
        std::memchr(name.chars, 0, name.length)) {
        return false;
    }
    for (uint32_t i = 0u; i < s->action_count; ++i) {
        if (inlay_ui_equal(name, s->actions[i].name)) {
            s->actions[i].callback = fn;
            s->actions[i].user = user;
            return true;
        }
    }
    if (s->action_count == 32u) return false;
    auto& a = s->actions[s->action_count++];
    std::memcpy(a.name, name.chars, name.length);
    a.name[name.length] = '\0';
    a.callback = fn;
    a.user = user;
    return true;
}

INLAY_API bool inlay_surface_allow_async_action(InlaySurface* s, EtchStr name) {
    if (!s || s->running || s->drawing || !name.chars || !name.length ||
        name.length >= sizeof(AsyncActionName::value) || std::memchr(name.chars, 0, name.length)) return false;
    auto* storage = ext(s);
    if (storage->async_actions_closed) return false;
    for (uint32_t i = 0u; i < storage->async_action_name_count; ++i) {
        if (inlay_ui_equal(name, storage->async_action_names[i].value)) return true;
    }
    if (!inlay_grow_pod(storage->async_action_names, storage->async_action_name_cap,
                        storage->async_action_name_count + 1u, 4u, kAsyncActionCapacity)) return false;
    auto& allowed = storage->async_action_names[storage->async_action_name_count++];
    std::memcpy(allowed.value, name.chars, name.length);
    allowed.value[name.length] = '\0';
    return true;
}

INLAY_API InlayAsyncActionEvent inlay_surface_poll_async_action(InlaySurface* s) {
    InlayAsyncActionEvent event = {sizeof(InlayAsyncActionEvent), INLAY_ASYNC_ACTION_NONE, 0u, {}, {}};
    if (!s || s->running || s->drawing || !ext(s)->async_actions) return event;
    auto* storage = ext(s);
    AsyncActionSlot* next = nullptr;
    for (uint32_t i = 0u; i < kAsyncActionCapacity; ++i) {
        auto& slot = storage->async_actions[i];
        if ((slot.phase == AsyncActionPhase::Queued || slot.phase == AsyncActionPhase::Cancelled) &&
            (!next || slot.id < next->id)) next = &slot;
    }
    if (!next) return event;
    event.request_id = next->id;
    if (next->phase == AsyncActionPhase::Cancelled) {
        event.kind = INLAY_ASYNC_ACTION_CANCEL;
        *next = {};
    } else {
        event.kind = INLAY_ASYNC_ACTION_START;
        const char* name = storage->async_action_names[next->name_index].value;
        event.name = {name, static_cast<uint32_t>(std::strlen(name)), true};
        event.payload_json = {next->payload, next->payload_length, false};
        next->phase = AsyncActionPhase::Started;
    }
    return event;
}

INLAY_API bool inlay_surface_complete_async_action(InlaySurface* s, uint32_t id, bool success, EtchStr json) {
    if (!s || !s->loaded || s->stats.has_error || s->running || s->drawing ||
        !id || !json.chars || !json.length || json.length > kAsyncPayloadLimit) return false;
    auto* slot = find_async_action(s, id);
    if (!slot || slot->phase != AsyncActionPhase::Started) return false;
    const JSValue result = parse_json(s, json);
    if (JS_IsException(result)) {
        JS_FreeValue(s->js, JS_GetException(s->js));
        return false;
    }
    const uint64_t start = SDL_GetPerformanceCounter();
    begin_tick(s);
    const JSValue done = settle_async_action(s, *slot, result, success, false);
    bool ok = !JS_IsException(done);
    JS_FreeValue(s->js, done);
    JS_FreeValue(s->js, result);
    if (ok) ok = rebuild(s, false);
    ok = finish_tick(s, start) && ok;
    etch_request_redraw(s->etch);
    return ok;
}

INLAY_API void inlay_surface_close_async_actions(InlaySurface* s) {
    if (!s || s->running || s->drawing) return;
    ext(s)->async_actions_closed = true;
    retire_async_actions(s);
}

INLAY_API bool inlay_surface_bind_slot_ex(InlaySurface* s, EtchStr name, InlaySlotRenderFn fn,
                                          InlaySlotMeasureFn measure_fn, EtchA11yRole a11y_role,
                                          EtchStr a11y_label, void* user) {
    if (!s || !name.chars || !name.length || name.length >= 64u || !fn ||
        std::memchr(name.chars, 0, name.length)) {
        return false;
    }
    if (a11y_label.length > 0u && (!a11y_label.chars || std::memchr(a11y_label.chars, 0, a11y_label.length))) {
        return false;
    }
    SlotBinding* slot = nullptr;
    for (uint32_t i = 0u; i < s->slot_count; ++i) {
        if (inlay_ui_equal(name, s->slots[i].name)) {
            slot = &s->slots[i];
            break;
        }
    }
    if (!slot) {
        if (s->slot_count == 32u) return false;
        slot = &s->slots[s->slot_count++];
        *slot = {};
        std::memcpy(slot->name, name.chars, name.length);
        slot->name[name.length] = '\0';
    }
    slot->callback = fn;
    slot->measure_callback = measure_fn;
    slot->a11y_role = a11y_role;
    slot->a11y_label[0] = '\0';
    if (a11y_label.length > 0u) {
        const uint32_t copy_n = a11y_label.length < (sizeof(slot->a11y_label) - 1u)
            ? a11y_label.length
            : uint32_t(sizeof(slot->a11y_label) - 1u);
        std::memcpy(slot->a11y_label, a11y_label.chars, copy_n);
        slot->a11y_label[copy_n] = '\0';
    }
    slot->user = user;
    etch_request_redraw(s->etch);
    return true;
}

INLAY_API bool inlay_surface_bind_slot(InlaySurface* s, EtchStr name, InlaySlotRenderFn fn, void* user) {
    return inlay_surface_bind_slot_ex(s, name, fn, nullptr, ETCH_A11Y_NONE, {}, user);
}

INLAY_API bool inlay_surface_set_action_policy(InlaySurface* s, EtchStr name, const InlayActionPolicy* policy_in) {
    if (!s || !name.chars || !name.length || name.length >= sizeof(ActionPolicyEntry::name) ||
        std::memchr(name.chars, 0, name.length)) {
        return false;
    }
    auto* storage = ext(s);
    uint32_t found = storage->action_policy_count;
    for (uint32_t i = 0u; i < storage->action_policy_count; ++i) {
        if (inlay_ui_equal(name, storage->action_policies[i].name)) {
            found = i;
            break;
        }
    }
    if (!policy_in) {
        if (found < storage->action_policy_count) {
            storage->action_policies[found] = storage->action_policies[--storage->action_policy_count];
        }
        return true;
    }
    InlayActionPolicy policy = inlay_action_policy_default();
    if (!inlay_read_abi_struct(policy_in, &policy)) return false;
    if (policy.required_keys_csv.length > 0u &&
        (!policy.required_keys_csv.chars ||
         policy.required_keys_csv.length >= sizeof(ActionPolicyEntry::required_keys_csv) ||
         std::memchr(policy.required_keys_csv.chars, 0, policy.required_keys_csv.length))) {
        return false;
    }
    if (found == storage->action_policy_count) {
        if (!inlay_grow_pod(storage->action_policies, storage->action_policy_cap,
                            storage->action_policy_count + 1u, 8u, 128u)) {
            return false;
        }
        ++storage->action_policy_count;
    }
    auto& dst = storage->action_policies[found];
    dst = {};
    std::memcpy(dst.name, name.chars, name.length);
    dst.name[name.length] = '\0';
    dst.permission = policy.permission;
    dst.require_approval = policy.require_approval;
    if (policy.required_keys_csv.length > 0u) {
        std::memcpy(dst.required_keys_csv, policy.required_keys_csv.chars, policy.required_keys_csv.length);
        dst.required_keys_csv[policy.required_keys_csv.length] = '\0';
    }
    return true;
}

INLAY_API void inlay_surface_set_action_audit_hook(InlaySurface* s, InlayActionAuditFn fn, void* user_data) {
    if (!s) return;
    ext(s)->audit_fn = fn;
    ext(s)->audit_user = user_data;
}

static bool bind_content(InlaySurface* s, EtchStr name, InlayUiKind kind, void* resource) {
    if (!s || s->running || s->drawing || !name.chars || !name.length ||
        name.length >= sizeof(ContentBinding::name) || std::memchr(name.chars, 0, name.length)) return false;
    const uint32_t i = inlay_ui_content_index(s, kind, name);
    if (i < s->content_count) {
        if (resource) s->content[i].resource = resource;
        else s->content[i] = s->content[--s->content_count];
    } else {
        if (!resource) return true;
        if (!inlay_grow_pod(s->content, s->content_cap, i + 1u, 4u, 64u)) return false;
        auto& binding = s->content[s->content_count++];
        std::memcpy(binding.name, name.chars, name.length);
        binding.name[name.length] = '\0';
        binding.kind = kind;
        binding.resource = resource;
    }
    etch_request_redraw(s->etch);
    return true;
}

INLAY_API bool inlay_surface_bind_model(InlaySurface* s, EtchStr name, InlayModel* model) {
    return bind_content(s, name, INLAY_UI_VIRTUAL_LIST, model);
}

INLAY_API uint64_t inlay_surface_memo_reused_nodes(const InlaySurface* s) {
    return s ? s->memo_reused_nodes : 0u;
}

INLAY_API uint32_t inlay_surface_export_dts(const InlaySurface* s, char* out_buf, uint32_t cap) {
    if (!s) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }
    struct DtsWriter {
        char* buf;
        uint32_t cap;
        uint32_t len;
        void put_raw(const char* p, uint32_t n) noexcept {
            if (buf && cap > 1u && len < cap - 1u) {
                const uint32_t avail = (cap - 1u) - len;
                const uint32_t take = n < avail ? n : avail;
                std::memcpy(buf + len, p, take);
            }
            len += n;
        }
        void put(const char* lit) noexcept {
            put_raw(lit, static_cast<uint32_t>(std::strlen(lit)));
        }
        void put_ts_str(const char* str) noexcept {
            put_raw("'", 1u);
            for (const char* p = str; *p; ++p) {
                const char c = *p;
                if (c == '\'' || c == '\\') {
                    put_raw("\\", 1u);
                    put_raw(&c, 1u);
                } else if (c == '\n') {
                    put_raw("\\n", 2u);
                } else if (c == '\r') {
                    put_raw("\\r", 2u);
                } else {
                    put_raw(&c, 1u);
                }
            }
            put_raw("'", 1u);
        }
    };

    DtsWriter w = {out_buf, cap, 0u};
    const auto* storage = ext(s);
    w.put("// Auto-generated Inlay surface declarations\n");

    auto emit_content_union = [&](const char* type_name, InlayUiKind kind) noexcept {
        w.put("export type ");
        w.put(type_name);
        w.put(" = ");
        bool any = false;
        for (uint32_t i = 0u; i < s->content_count; ++i) {
            if (s->content[i].kind != kind) continue;
            if (any) w.put(" | ");
            w.put_ts_str(s->content[i].name);
            any = true;
        }
        if (!any) w.put("never");
        w.put(";\n");
    };

    w.put("export type ActionName = ");
    if (s->action_count == 0u) {
        w.put("never");
    } else {
        for (uint32_t i = 0u; i < s->action_count; ++i) {
            if (i > 0u) w.put(" | ");
            w.put_ts_str(s->actions[i].name);
        }
    }
    w.put(";\n");

    w.put("export type AsyncActionName = ");
    if (storage->async_action_name_count == 0u) {
        w.put("never");
    } else {
        for (uint32_t i = 0u; i < storage->async_action_name_count; ++i) {
            if (i > 0u) w.put(" | ");
            w.put_ts_str(storage->async_action_names[i].value);
        }
    }
    w.put(";\n");

    w.put("export type SlotName = ");
    if (s->slot_count == 0u) {
        w.put("never");
    } else {
        for (uint32_t i = 0u; i < s->slot_count; ++i) {
            if (i > 0u) w.put(" | ");
            w.put_ts_str(s->slots[i].name);
        }
    }
    w.put(";\n");

    emit_content_union("DocumentName", INLAY_UI_DOC);
    emit_content_union("BufferName", INLAY_UI_CODE_EDITOR);
    emit_content_union("ModelName", INLAY_UI_VIRTUAL_LIST);

    w.put(
        "\n"
        "export type InlayNode = { tag: string; props: Record<string, unknown>; children: unknown[] } | string | number | boolean | null | undefined | InlayNode[];\n"
        "export interface CommonProps {\n"
        "  id?: string;\n"
        "  key?: string | number;\n"
        "  cls?: string;\n"
        "  className?: string;\n"
        "  width?: number | string;\n"
        "  height?: number | string;\n"
        "  bg?: string;\n"
        "  color?: string;\n"
        "  opacity?: number;\n"
        "  disabled?: boolean;\n"
        "  stopPropagation?: boolean;\n"
        "  onClick?: ActionName | ((state: unknown) => void);\n"
        "  [key: string]: unknown;\n"
        "}\n"
        "export interface FormProps extends CommonProps {\n"
        "  resetKey?: string;\n"
        "  onSubmit?: ActionName | ((values: Record<string, string>, state: unknown) => void);\n"
        "  onInvalid?: ActionName | ((fields: string[], state: unknown) => void);\n"
        "}\n"
        "export interface InputProps extends CommonProps {\n"
        "  name?: string;\n"
        "  value?: string;\n"
        "  placeholder?: string;\n"
        "  required?: boolean;\n"
        "  minLength?: number;\n"
        "  maxLength?: number;\n"
        "  error?: string;\n"
        "  password?: boolean;\n"
        "  multiline?: boolean;\n"
        "  readOnly?: boolean;\n"
        "  onChange?: ActionName | ((value: string, state: unknown) => void);\n"
        "  onSubmit?: ActionName | ((value: string, state: unknown) => void);\n"
        "}\n"
        "export interface SlotProps extends CommonProps { name: SlotName; props?: unknown; }\n"
        "export interface DocProps extends CommonProps { document?: DocumentName; markdown?: string; }\n"
        "export interface CodeEditorProps extends CommonProps { buffer: BufferName; readOnly?: boolean; wordWrap?: boolean; ghostText?: string; searchQuery?: string; }\n"
        "export interface VirtualListProps extends CommonProps { model?: ModelName; count?: number; itemHeight?: number; items?: unknown[]; renderItem?: (index: number) => InlayNode; }\n"
        "export interface TreeProps extends CommonProps { model?: ModelName; showIndentGuide?: boolean; rowHeight?: number; indent?: number; }\n");

    w.put(
        "\n"
        "declare function action(name: ActionName, payload?: unknown): boolean;\n"
        "declare function request<T = unknown>(name: AsyncActionName, payload?: unknown): Promise<T> & { cancel(): boolean };\n"
        "declare function setTimeout(fn: () => void, ms?: number): number;\n"
        "declare function clearTimeout(id: number): void;\n"
        "declare function useState<T>(init: T | (() => T)): [T, (next: T | ((prev: T) => T)) => void];\n"
        "declare function useRef<T>(init: T): { current: T };\n"
        "declare function useMemo<T>(fn: () => T, deps?: readonly unknown[]): T;\n"
        "declare function useCallback<T extends (...args: unknown[]) => unknown>(fn: T, deps?: readonly unknown[]): T;\n"
        "declare function useEffect(fn: () => void | (() => void), deps?: readonly unknown[]): void;\n"
        "declare function memo<P extends object>(component: (props: P) => InlayNode, equal?: (a: P, b: P) => boolean): (props: P) => InlayNode;\n"
        "\n"
        "declare namespace ui {\n"
        "  function box(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function row(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function col(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function card(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function form(props?: FormProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function spacer(props?: CommonProps): InlayNode;\n"
        "  function text(props?: string | CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function button(props?: string | CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function input(props?: InputProps): InlayNode;\n"
        "  function toggle(props?: CommonProps): InlayNode;\n"
        "  function checkbox(props?: CommonProps): InlayNode;\n"
        "  function tabs(props?: CommonProps): InlayNode;\n"
        "  function combobox(props?: CommonProps): InlayNode;\n"
        "  function scroll(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function progress(props?: CommonProps): InlayNode;\n"
        "  function slider(props?: CommonProps): InlayNode;\n"
        "  function badge(props?: string | CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function divider(props?: CommonProps): InlayNode;\n"
        "  function image(props?: CommonProps): InlayNode;\n"
        "  function modal(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function popover(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function tooltip(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function split(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function math(props?: string | CommonProps): InlayNode;\n"
        "  function diagram(props?: string | CommonProps): InlayNode;\n"
        "  function slot(props?: SlotName | SlotProps): InlayNode;\n"
        "  function doc(props?: string | DocProps): InlayNode;\n"
        "  function codeEditor(props?: BufferName | CodeEditorProps): InlayNode;\n"
        "  function terminal(props?: string | CommonProps): InlayNode;\n"
        "  function virtualList(props?: VirtualListProps, ...children: InlayNode[]): InlayNode;\n"
        "  function table(props?: CommonProps): InlayNode;\n"
        "  function tree(props?: TreeProps, ...children: InlayNode[]): InlayNode;\n"
        "  function treeItem(props?: string | CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function menu(props?: CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function contextMenu(props?: CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function avatar(props?: string | CommonProps): InlayNode;\n"
        "  function toast(props?: string | CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function accordion(props?: string | CommonProps, ...children: InlayNode[]): InlayNode;\n"
        "  function skeleton(props?: CommonProps): InlayNode;\n"
        "  function errorBoundary(props: { resetKey?: unknown; render: () => InlayNode; fallback?: (err: unknown, retry: () => void) => InlayNode }): InlayNode;\n"
        "  function h(tag: string | ((props: Record<string, unknown>) => InlayNode), props?: Record<string, unknown> | null, ...children: unknown[]): InlayNode;\n"
        "  function fragment(...children: InlayNode[]): InlayNode[];\n"
        "}\n");

    w.put(
        "\n"
        "declare namespace JSX {\n"
        "  interface IntrinsicElements {\n"
        "    Box: CommonProps;\n"
        "    Row: CommonProps;\n"
        "    Col: CommonProps;\n"
        "    Card: CommonProps;\n"
        "    Form: FormProps;\n"
        "    Text: CommonProps;\n"
        "    Button: CommonProps;\n"
        "    Input: InputProps;\n"
        "    Toggle: CommonProps;\n"
        "    Switch: CommonProps;\n"
        "    Checkbox: CommonProps;\n"
        "    Tabs: CommonProps;\n"
        "    Combobox: CommonProps;\n"
        "    Scroll: CommonProps;\n"
        "    Progress: CommonProps;\n"
        "    Slider: CommonProps;\n"
        "    Badge: CommonProps;\n"
        "    Divider: CommonProps;\n"
        "    Image: CommonProps;\n"
        "    Modal: CommonProps;\n"
        "    Popover: CommonProps;\n"
        "    Tooltip: CommonProps;\n"
        "    Split: CommonProps;\n"
        "    Spacer: CommonProps;\n"
        "    Diagram: CommonProps;\n"
        "    Slot: SlotProps;\n"
        "    Doc: DocProps;\n"
        "    CodeEditor: CodeEditorProps;\n"
        "    Terminal: CommonProps;\n"
        "    VirtualList: VirtualListProps;\n"
        "    Table: CommonProps;\n"
        "    Tree: TreeProps;\n"
        "    TreeItem: CommonProps;\n"
        "    Menu: CommonProps;\n"
        "    ContextMenu: CommonProps;\n"
        "    Avatar: CommonProps;\n"
        "    Toast: CommonProps;\n"
        "    Accordion: CommonProps;\n"
        "    Skeleton: CommonProps;\n"
        "    [elemName: string]: CommonProps;\n"
        "  }\n"
        "}\n");

    if (out_buf && cap > 0u) {
        const uint32_t nul_pos = w.len < cap - 1u ? w.len : (cap - 1u);
        out_buf[nul_pos] = '\0';
    }
    return w.len;
}

INLAY_API bool inlay_surface_bind_document(InlaySurface* s, EtchStr name, InlayDoc* doc) {
    if (doc && (!s || doc->context != s->etch)) return false;
    return bind_content(s, name, INLAY_UI_DOC, doc);
}

INLAY_API bool inlay_surface_bind_text_buffer(InlaySurface* s, EtchStr name, EtchTextBuffer* buffer) {
    return bind_content(s, name, INLAY_UI_CODE_EDITOR, buffer);
}

INLAY_API bool inlay_surface_set_image_resolver(InlaySurface* s, InlayImageResolverFn fn, void* user_data) {
    if (!s) return false;
    s->image_resolver_fn = fn;
    s->image_resolver_user_data = user_data;
    etch_request_redraw(s->etch);
    return true;
}

INLAY_API bool inlay_surface_dispatch(InlaySurface* s, EtchStr name, EtchStr payload) {
    if (!s || !s->js || !s->loaded || s->running || s->drawing || !name.chars || !name.length ||
        name.length >= 64u || !payload.chars || !payload.length || payload.length > 65536u) {
        return false;
    }
    return run_update(s, name, payload);
}

INLAY_API bool inlay_surface_enable_hmr(InlaySurface* s, const char* path) {
    if (!s || s->running || s->drawing || !path || std::strlen(path) >= sizeof(s->path)) return false;
    std::memcpy(s->path, path, std::strlen(path) + 1u);
    s->modified = -1;
    ext(s)->last_hmr_check_ms = -1000.0;
    return inlay_surface_poll_hmr(s);
}

INLAY_API bool inlay_surface_poll_hmr(InlaySurface* s) {
    if (!s || !s->path[0] || s->running || s->drawing) return false;
    ext(s)->last_hmr_check_ms = inlay_ui_now_ms(s);
    SDL_PathInfo info = {};
    if (!SDL_GetPathInfo(s->path, &info)) {
        s->hmr_file_error = true;
        record_reload_error(s, "HMR file is unavailable");
        return false;
    }
    if (info.modify_time == s->modified && info.size == s->file_size && !s->hmr_file_error) {
        return !s->stats.has_error && !ext(s)->reload_error;
    }
    const uint64_t start = SDL_GetPerformanceCounter();
    size_t length = 0u;
    char* script =
        info.size <= kMaxScriptBytes ? static_cast<char*>(SDL_LoadFile(s->path, &length)) : nullptr;
    if (script && length > kMaxScriptBytes) {
        SDL_free(script);
        script = nullptr;
    }
    if (!script) {
        s->hmr_file_error = true;
        record_reload_error(s, "HMR source could not be read");
        return false;
    }
    s->hmr_file_error = false;
    const uint64_t io_done = SDL_GetPerformanceCounter();
    const bool ok = load(s, {script, static_cast<uint32_t>(length), false}, true);
    const uint64_t eval_done = SDL_GetPerformanceCounter();
    SDL_free(script);
    s->modified = info.modify_time;
    s->file_size = info.size;
    ++s->stats.reloads;
    s->stats.last_reload_ms = double(eval_done - io_done) * 1000.0 / s->frequency;
    s->stats.last_reload_total_ms = double(SDL_GetPerformanceCounter() - start) * 1000.0 / s->frequency;
    return ok;
}

INLAY_API void inlay_surface_view(InlaySurface* s, const InlaySurfaceViewDesc* input_in) {
    InlaySurfaceViewDesc input = inlay_surface_view_desc_default();
    if (!s || !inlay_read_abi_struct(input_in, &input) || !input.id ||
        !std::isfinite(input.width_px) || !std::isfinite(input.height_px) ||
        etch_current_context() != s->etch || !etch_frame_in_progress(s->etch) ||
        s->running || s->drawing) {
        return;
    }
    ++s->frame_counter;

    // Bug 7 fix: throttle auto-polling of SDL_GetPathInfo inside view to 80 ms.
    const double current = inlay_ui_now_ms(s);
    if (s->path[0] && (current - ext(s)->last_hmr_check_ms) >= kHmrViewPollIntervalMs) {
        inlay_surface_poll_hmr(s);
    }
    if (s->pending_state && s->loaded && !s->stats.has_error) run_update(s, {}, {});

    bool timer_due = false;
    for (const auto& timer : s->timers) {
        timer_due |= !JS_IsUndefined(timer.function) && timer.due <= current;
    }
    if (timer_due && s->loaded && !s->stats.has_error) {
        const uint64_t start = SDL_GetPerformanceCounter();
        begin_tick(s);
        for (auto& timer : s->timers) {
            if (!JS_IsUndefined(timer.function) && timer.due <= current && !JS_HasException(s->js)) {
                const JSValue function = timer.function;
                timer.function = JS_UNDEFINED;
                timer.id = 0u;
                const JSValue result = JS_Call(s->js, function, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(s->js, result);
                JS_FreeValue(s->js, function);
            }
        }
        if (!JS_HasException(s->js)) rebuild(s, false);
        finish_tick(s, start);
        etch_request_redraw(s->etch);
    }
    if (inlay_surface_next_timer_ms(s) == 0) etch_request_redraw(s->etch);

    // #52 Capture effective surface width & trigger responsive breakpoint rebuild ONLY when responsive cls is used.
    {
        float eff_view_w = input.width_px;
        if (eff_view_w <= 0.f) {
            const EtchRect prev_b = etch_element_bounds(input.id);
            if (prev_b.w > 0.f) eff_view_w = prev_b.w;
        }
        if (eff_view_w > 0.f) {
            const bool had_width = ext(s)->has_view_width;
            const uint8_t prev_bp = responsive_width_bucket(s->last_view_width_px);
            const uint8_t next_bp = responsive_width_bucket(eff_view_w);
            s->last_view_width_px = eff_view_w;
            s->current_container_width_px = eff_view_w;
            ext(s)->has_view_width = true;
            if (ext(s)->uses_responsive_cls && (!had_width || prev_bp != next_bp) &&
                s->js && s->loaded && !s->stats.has_error) {
                run_update(s, {}, {});
            }
        } else if (ext(s)->uses_responsive_cls && !ext(s)->has_view_width) {
            etch_request_redraw(s->etch);
        }
    }

    const EtchTheme theme = etch_get_theme(s->etch);
    auto box = etch_box_desc_default();
    box.id = input.id;
    box.axis = ETCH_AXIS_COL;
    // Bug 8 fix: <= 0 dimensions grow to fill the parent container.
    box.width = input.width_px > 0.f ? etch_sizing_fixed(input.width_px) : etch_sizing_grow(0.f, 0.f);
    box.height = input.height_px > 0.f ? etch_sizing_fixed(input.height_px) : etch_sizing_grow(0.f, 0.f);
    box.clip_children = true;
    etch_begin_box(&box);

    // Snapshot VirtualList window bounds before draw to detect renderItem window refills (0 idle JS ticks).
    struct VListSnap { uint32_t id, first, end; };
    VListSnap vlist_before[16] = {};
    const uint32_t vlist_before_n = s->vlist_cache_count < 16u ? s->vlist_cache_count : 16u;
    for (uint32_t i = 0u; i < vlist_before_n; ++i) {
        vlist_before[i] = {s->vlist_cache[i].element_id, s->vlist_cache[i].first_index, s->vlist_cache[i].end_index};
    }
    WidgetEvents events = {};
    if (s->stats.has_error) {
        draw_error_card(s, theme, s->error, false);
    } else {
        if (const char* message = ext(s)->reload_error) draw_error_card(s, theme, message, true);
        if (s->node_count > 0u) {
            s->drawing = true;
            s->draw_parent_col = true;
            ext(s)->child_consumed_mask = 0u;
            inlay_ui_draw_node(s, 0u, input.id, theme, &events);
            s->drawing = false;
        }
    }
    etch_end_box();

    inlay_ui_trim_document_cache(s);
    bool vlist_refill = false;
    if (s->vlist_cache_count > 0u) {
        for (uint32_t i = 0u; i < s->vlist_cache_count; ++i) {
            const auto& cur = s->vlist_cache[i];
            bool matched_before = false;
            for (uint32_t j = 0u; j < vlist_before_n; ++j) {
                if (vlist_before[j].id == cur.element_id) {
                    matched_before = true;
                    if (cur.first_index != vlist_before[j].first || cur.end_index != vlist_before[j].end) {
                        vlist_refill = true;
                    }
                    break;
                }
            }
            if (vlist_refill) break;
            if (!matched_before) {
                for (uint32_t ni = 0u; ni < s->node_count; ++ni) {
                    const auto& nd = s->nodes[ni];
                    if (nd.kind != INLAY_UI_VIRTUAL_LIST) continue;
                    const auto& nst = s->styles[ni];
                    if ((nst.flags2 & Nf2HasValue) == 0u || nst.ext_idx >= s->vlist_spec_count) continue;
                    if (s->vlist_specs[nst.ext_idx].path_id != cur.element_id) continue;
                    uint32_t built_count = 0u;
                    for (uint32_t at = nd.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                        ++built_count;
                    }
                    const uint32_t built_start = s->vlist_specs[nst.ext_idx].window_start;
                    if (cur.first_index != built_start || cur.end_index != built_start + built_count) {
                        vlist_refill = true;
                        break;
                    }
                }
            }
            if (vlist_refill) break;
        }
    }

    if ((events.first || s->pending_state || vlist_refill) && s->js && s->loaded && !s->running) {
        run_batch(s, events.first);
    }
}

INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface* s) {
    if (!s || !s->loaded || s->stats.has_error) return -1;
    bool has = false;
    double soonest = 0.0;
    for (const auto& timer : s->timers) {
        if (JS_IsUndefined(timer.function)) continue;
        if (!has || timer.due < soonest) {
            soonest = timer.due;
            has = true;
        }
    }
    if (!has) return -1;
    const double rem = soonest - inlay_ui_now_ms(s);
    if (rem <= 0.0) return 0;
    const double ceiled = std::ceil(rem);
    return ceiled >= double(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(ceiled);
}

INLAY_API InlaySurfaceStats inlay_surface_stats(const InlaySurface* s) {
    if (!s) return {};
    InlaySurfaceStats result = s->stats;
    result.cached_documents = s->doc_cache_count;
    result.has_error |= ext(s)->reload_error != nullptr;
    return result;
}

INLAY_API EtchStr inlay_surface_error(const InlaySurface* s) {
    if (!s) return {};
    const char* message = s->stats.has_error ? s->error : ext(s)->reload_error;
    return message ? EtchStr{message, static_cast<uint32_t>(std::strlen(message)), true} : EtchStr{};
}

INLAY_API const InlayUiNode* inlay_surface_nodes(const InlaySurface* s, uint32_t* count) {
    if (count) *count = s ? s->node_count : 0u;
    return s ? s->nodes : nullptr;
}
} // extern "C"
#else
extern "C" {
INLAY_API InlaySurface* inlay_surface_create(EtchContext*, const InlaySurfaceDesc*) { return nullptr; }
INLAY_API void inlay_surface_destroy(InlaySurface*) {}
INLAY_API bool inlay_surface_set_script(InlaySurface*, EtchStr) { return false; }
INLAY_API bool inlay_surface_set_state_json(InlaySurface*, EtchStr) { return false; }
INLAY_API EtchStr inlay_surface_state_json(const InlaySurface*) { return {}; }
INLAY_API bool inlay_surface_bind_action(InlaySurface*, EtchStr, InlayActionFn, void*) { return false; }
INLAY_API bool inlay_surface_allow_async_action(InlaySurface*, EtchStr) { return false; }
INLAY_API InlayAsyncActionEvent inlay_surface_poll_async_action(InlaySurface*) {
    return {sizeof(InlayAsyncActionEvent), INLAY_ASYNC_ACTION_NONE, 0u, {}, {}};
}
INLAY_API bool inlay_surface_complete_async_action(InlaySurface*, uint32_t, bool, EtchStr) { return false; }
INLAY_API void inlay_surface_close_async_actions(InlaySurface*) {}
INLAY_API bool inlay_surface_bind_slot(InlaySurface*, EtchStr, InlaySlotRenderFn, void*) { return false; }
INLAY_API bool inlay_surface_bind_slot_ex(InlaySurface*, EtchStr, InlaySlotRenderFn, InlaySlotMeasureFn,
                                          EtchA11yRole, EtchStr, void*) { return false; }
INLAY_API bool inlay_surface_set_action_policy(InlaySurface*, EtchStr, const InlayActionPolicy*) { return false; }
INLAY_API void inlay_surface_set_action_audit_hook(InlaySurface*, InlayActionAuditFn, void*) {}
INLAY_API bool inlay_surface_bind_model(InlaySurface*, EtchStr, InlayModel*) { return false; }
INLAY_API uint64_t inlay_surface_memo_reused_nodes(const InlaySurface*) { return 0u; }
INLAY_API uint32_t inlay_surface_export_dts(const InlaySurface*, char* out_buf, uint32_t cap) {
    if (out_buf && cap > 0u) out_buf[0] = '\0';
    return 0u;
}
INLAY_API bool inlay_surface_bind_document(InlaySurface*, EtchStr, InlayDoc*) { return false; }
INLAY_API bool inlay_surface_bind_text_buffer(InlaySurface*, EtchStr, EtchTextBuffer*) { return false; }
INLAY_API bool inlay_surface_set_image_resolver(InlaySurface*, InlayImageResolverFn, void*) { return false; }
INLAY_API bool inlay_surface_dispatch(InlaySurface*, EtchStr, EtchStr) { return false; }
INLAY_API bool inlay_surface_enable_hmr(InlaySurface*, const char*) { return false; }
INLAY_API bool inlay_surface_poll_hmr(InlaySurface*) { return false; }
INLAY_API void inlay_surface_view(InlaySurface*, const InlaySurfaceViewDesc*) {}
INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface*) { return -1; }
INLAY_API InlaySurfaceStats inlay_surface_stats(const InlaySurface*) { return {}; }
INLAY_API EtchStr inlay_surface_error(const InlaySurface*) { return {}; }
INLAY_API const InlayUiNode* inlay_surface_nodes(const InlaySurface*, uint32_t* count) {
    if (count) *count = 0u;
    return nullptr;
}
} // extern "C"
#endif
