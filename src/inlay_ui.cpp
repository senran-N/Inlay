#include "inlay_ui_internal.h"
#include <cstring>

// ============================================================================
// Reusable Host Row/Tree Model (Available Both With & Without Script Engine)
// ============================================================================

constexpr uint32_t kModelChangeHistoryCap = 16u;

struct InlayModel {
    InlayModelReadFn read_fn;
    void* user_data;
    EtchContext* bound_ctx;
    uint32_t count;
    uint64_t revision;
    InlayModelChange last_change;
    int64_t pending_scroll_row_delta;
    uint32_t last_structural_index;
    InlayModelChange history[kModelChangeHistoryCap];
    uint32_t history_head;
    uint32_t history_count;
};

static void model_wake_context(const InlayModel* model) noexcept {
    EtchContext* ctx = model->bound_ctx ? model->bound_ctx : etch_current_context();
    if (ctx) {
        etch_request_redraw(ctx);
        etch_wake_up(ctx);
    }
}

static void model_record_change(InlayModel* model, InlayModelChangeKind kind,
                                uint32_t first_row, uint32_t count, uint32_t dst_row) noexcept {
    ++model->revision;
    const InlayModelChange ch = {kind, first_row, count, dst_row, model->revision};
    model->last_change = ch;
    model->history[model->history_head] = ch;
    model->history_head = (model->history_head + 1u) % kModelChangeHistoryCap;
    if (model->history_count < kModelChangeHistoryCap) ++model->history_count;
    model_wake_context(model);
}

bool inlay_ui_model_read(const InlayModel* model, uint32_t index, InlayModelRow* out_row) noexcept {
    if (!model || !model->read_fn || !out_row || index >= model->count) return false;
    *out_row = {};
    return model->read_fn(index, out_row, model->user_data);
}

bool inlay_ui_model_replay_scroll_delta(const InlayModel* model, uint64_t since_rev,
                                        uint32_t first_visible, int64_t* out_row_delta) noexcept {
    if (!model || !out_row_delta || model->revision <= since_rev || model->history_count == 0u) {
        if (out_row_delta) *out_row_delta = 0;
        return false;
    }
    const uint32_t oldest =
        (model->history_head + kModelChangeHistoryCap - model->history_count) % kModelChangeHistoryCap;
    int64_t cur_vis = static_cast<int64_t>(first_visible);
    int64_t delta = 0;
    for (uint32_t i = 0u; i < model->history_count; ++i) {
        const InlayModelChange& ch = model->history[(oldest + i) % kModelChangeHistoryCap];
        if (ch.revision <= since_rev) continue;
        if (ch.kind == INLAY_MODEL_CHANGE_RESET) {
            *out_row_delta = 0;
            return false;
        }
        if (ch.kind == INLAY_MODEL_CHANGE_INSERT) {
            if (static_cast<int64_t>(ch.first_row) < cur_vis) {
                const int64_t add = static_cast<int64_t>(ch.count);
                delta += add;
                cur_vis += add;
            }
        } else if (ch.kind == INLAY_MODEL_CHANGE_REMOVE) {
            if (static_cast<int64_t>(ch.first_row) < cur_vis) {
                const int64_t avail_above = cur_vis - static_cast<int64_t>(ch.first_row);
                const int64_t sub = static_cast<int64_t>(ch.count) < avail_above
                    ? static_cast<int64_t>(ch.count)
                    : avail_above;
                delta -= sub;
                cur_vis -= sub;
            }
        }
    }
    *out_row_delta = delta;
    return delta != 0;
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

INLAY_API void inlay_model_bind_context(InlayModel* model, EtchContext* ctx) {
    if (!model) return;
    model->bound_ctx = ctx;
}

INLAY_API void inlay_model_set_count(InlayModel* model, uint32_t count) {
    if (!model) return;
    if (model->count != count) {
        model->count = count;
        model->pending_scroll_row_delta = 0;
        model->last_structural_index = 0u;
        model_record_change(model, INLAY_MODEL_CHANGE_RESET, 0u, count, 0u);
    }
}

INLAY_API void inlay_model_notify_change(InlayModel* model, uint32_t first_row, uint32_t count) {
    if (!model) return;
    model_record_change(model, INLAY_MODEL_CHANGE_UPDATE, first_row, count, 0u);
}

INLAY_API void inlay_model_notify_insert(InlayModel* model, uint32_t index, uint32_t count) {
    if (!model || count == 0u) return;
    const uint32_t clamped_idx = index <= model->count ? index : model->count;
    const uint32_t max_add = UINT32_MAX - model->count;
    const uint32_t added = count <= max_add ? count : max_add;
    if (added == 0u) return;
    model->count += added;
    model->pending_scroll_row_delta += static_cast<int64_t>(added);
    model->last_structural_index = clamped_idx;
    model_record_change(model, INLAY_MODEL_CHANGE_INSERT, clamped_idx, added, 0u);
}

INLAY_API void inlay_model_notify_remove(InlayModel* model, uint32_t index, uint32_t count) {
    if (!model || count == 0u) return;
    const uint32_t avail = index < model->count ? (model->count - index) : 0u;
    const uint32_t removed = count <= avail ? count : avail;
    if (removed == 0u) return;
    model->count -= removed;
    model->pending_scroll_row_delta -= static_cast<int64_t>(removed);
    model->last_structural_index = index;
    model_record_change(model, INLAY_MODEL_CHANGE_REMOVE, index, removed, 0u);
}

INLAY_API void inlay_model_notify_move(InlayModel* model, uint32_t from_index, uint32_t to_index, uint32_t count) {
    if (!model || count == 0u) return;
    model_record_change(model, INLAY_MODEL_CHANGE_MOVE, from_index, count, to_index);
}

INLAY_API bool inlay_model_last_change(const InlayModel* model, InlayModelChange* out) {
    if (!model || !out) return false;
    if (model->last_change.kind == INLAY_MODEL_CHANGE_NONE) {
        *out = {};
        return false;
    }
    *out = model->last_change;
    return true;
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
constexpr uint32_t kMaxMemoSubtreeNodes = 4096u;
constexpr uint32_t kMaxMemoSubtreeText = 65536u;
constexpr size_t kMaxMemoCacheTotalBytes = 2u * 1024u * 1024u;
constexpr uint32_t kMaxFormStates = 16u;
constexpr uint16_t kNf2Container = uint16_t(1u << 13);
constexpr uint32_t kContainerWidthCacheCap = 64u;
constexpr uint32_t kMaxBoundaryRoots = 64u;
constexpr uint32_t kMaxEventBoundaryTags = 32u;

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
    CefEditorHasGhost        = 1u << 16,
    CefEditorHasSearch       = 1u << 17,
    CefInputHasMinLen        = 1u << 18,
    CefInputHasMaxLen        = 1u << 19,
    CefSwitchHasTrackR       = 1u << 20,
    CefSwitchHasThumbR       = 1u << 21,
    CefSwitchHasThumbPad     = 1u << 22,
    CefInputHasSubmitEnter   = 1u << 23,
    CefInputSubmitEnterVal   = 1u << 24,
    CefWindowDragPassthrough = 1u << 25,
    CefSplitHasDividerSz     = 1u << 26,
    CefSplitHasDividerHit    = 1u << 27,
    CefSplitHideGrip         = 1u << 28,
    CefIconHasSecondaryCol   = 1u << 29,
    CefInputAutoGrow         = 1u << 30,
    CefInputHasAutoGrowRange = 1u << 31
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
    ColorRef color_a;         // Text selection_color, Switch off_color, OR Icon secondary_color
    ColorRef color_b;         // Switch thumb_color
    float aux_f0;             // Switch track_width, Progress ring thickness, OR Split divider_size_px
    float aux_f1;             // Switch track_height OR Split divider_hit_px
    float aux_f2;             // Switch track_radius
    float aux_f3;             // Switch thumb_radius
    float aux_f4;             // Switch thumb_padding_px
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
    size_t blob_bytes;
};

struct ContainerWidthEntry {
    uint32_t node_id;
    float width_px;
    float last_rebuild_view_w;
    uint8_t bucket;
    uint8_t built_bucket;
    uint8_t prev_bucket;
    bool measured;
    bool requested_measure;
    uint64_t last_rebuild_frame;
    uint64_t last_used_frame;
};

struct BoundaryRootEntry {
    uint32_t node_idx;
    TextSlice eb_key;
};

struct EventBoundaryTag {
    const WidgetEvent* ev;
    TextSlice eb_key;
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
    EtchStr error;
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

struct RegisteredModule {
    char name[128];
    char* source;
    uint32_t length;
};

struct PendingDialogSlot {
    uint32_t tag;
    JSValue settle[2];
    bool active;
};

struct CommandNameEntry {
    uint32_t id;
    char name[64];
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
    size_t memo_cache_bytes;
    JSAtom atom_memo_key;
    JSAtom atom_memo_rev;
    JSAtom atom_eb_key;
    // I07 Boundary-local event error isolation
    TextSlice* handler_eb_keys; // Parallel to pub.handlers (capacity = pub.handler_capacity).
    TextSlice cur_eb_key;
    TextSlice draw_eb_key;
    BoundaryRootEntry boundary_roots[kMaxBoundaryRoots];
    uint32_t boundary_root_count;
    EventBoundaryTag event_eb_tags[kMaxEventBoundaryTags];
    uint32_t event_eb_tag_count;
    // #54 Form validation state
    FormStateEntry form_states[kMaxFormStates];
    uint32_t form_state_count;
    ActiveFormCtx* active_form;
    // #52 / I03 Responsive & layout-width @container breakpoint tracking
    uint32_t responsive_cls_hits;
    bool uses_responsive_cls;
    bool has_view_width;
    ContainerWidthEntry container_widths[kContainerWidthCacheCap];
    uint32_t container_width_count;
    uint32_t draw_path_id;
    bool needs_container_rebuild;
    // #59 Subtree-scoped event consumption
    uint8_t child_consumed_mask;
    // R03 Transactional HMR bootstrap bytecode cache & staging host redirect
    uint8_t* bootstrap_bc;
    size_t bootstrap_bc_len;
    InlaySurface* host_surface;
    // Sandboxed in-memory ES modules (no host filesystem reads)
    RegisteredModule* modules;
    uint32_t module_count;
    uint32_t module_cap;
    // Round 3/4: Async file dialogs, dynamic command name map, host storage hooks & last root view ID
    PendingDialogSlot pending_dialogs[8];
    uint32_t next_dialog_tag;
    CommandNameEntry* command_names;
    uint32_t command_name_count;
    uint32_t command_name_cap;
    uint32_t pending_cmds[8];
    uint32_t pending_cmd_count;
    uint32_t last_view_id;
    InlayStorageGetFn storage_get_fn;
    InlayStorageSetFn storage_set_fn;
    InlayStorageRemoveFn storage_remove_fn;
    void* storage_user;
    struct LocalStorageEntry {
        char* key;
        char* value;
    };
    LocalStorageEntry* ls_entries;
    uint32_t ls_count;
    uint32_t ls_cap;
    uint64_t uuid_seq;
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
        case INLAY_UI_PORTAL:
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
        case INLAY_UI_CODE_EDITOR:
        case INLAY_UI_DOCK_SPACE:
        case INLAY_UI_DOCK_PANE:
        case INLAY_UI_SHEET:
        case INLAY_UI_HOVER_CARD:
        case INLAY_UI_RADIO_GROUP:
        case INLAY_UI_TOGGLE_GROUP:
        case INLAY_UI_COLOR_PICKER:
        case INLAY_UI_CHART:
        case INLAY_UI_VIEW_ZONE:
        case INLAY_UI_DIFF_VIEW:
            return true;
        default:
            return false;
    }
}

JSValue parse_json(InlaySurface* s, EtchStr json) {
    const uint32_t limit = s->desc.max_state_bytes ? s->desc.max_state_bytes : 65536u;
    if (json.length > limit) {
        return JS_ThrowRangeError(s->js, "JSON payload exceeds max_state_bytes");
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

void begin_tick(InlaySurface* s, double budget_scale = 1.0) {
    s->running = true;
    s->timed_out = false;
    double ms = double(s->desc.max_tick_ms) * budget_scale;
    if (budget_scale > 1.0 && ms > 50.0) ms = 50.0;
    s->deadline =
        SDL_GetPerformanceCounter() + uint64_t(ms * s->frequency / 1000.0);
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
            timer.due = 0.0;
            timer.interval_ms = 0.0;
            timer.id = 0u;
            timer.is_raf = false;
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
    const uint32_t limit = s->desc.max_state_bytes ? s->desc.max_state_bytes : 65536u;
    if (length > limit) {
        record_error(s, "State JSON exceeds max_state_bytes");
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

void parse_dnd_and_keys(InlaySurface* s, JSValueConst props, NodeStyle& st) {
    if (JS_HasException(s->js)) return;
    TextSlice key_act = {};
    uint32_t key_h = INLAY_UI_NONE;
    if (inlay_ui_parse_event_prop(s, props, "onKeyDown", &key_act, &key_h)) {
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->on_key_action = key_act;
            cold->on_key_handler = key_h;
            cold->global_keys = inlay_ui_bool_prop(s, props, "globalKeys");
            cold->consume_keys = inlay_ui_bool_prop(s, props, "consumeKeys");
        }
        st.flags |= NfClickable;
    }
    TextSlice txt_act = {};
    uint32_t txt_h = INLAY_UI_NONE;
    if (!JS_HasException(s->js) && inlay_ui_parse_event_prop(s, props, "onTextInput", &txt_act, &txt_h)) {
        if (auto* cold = inlay_ui_ensure_cold(s, st)) {
            cold->on_text_action = txt_act;
            cold->on_text_handler = txt_h;
            if (!cold->global_keys) cold->global_keys = inlay_ui_bool_prop(s, props, "globalKeys");
        }
        st.flags |= NfClickable;
    }
    if (JS_HasException(s->js)) return;
    const bool draggable = inlay_ui_bool_prop(s, props, "draggable");
    const TextSlice drag_kind = draggable ? inlay_ui_string_slice_prop(s, props, "dragKind") : TextSlice{};
    const TextSlice drag_label = draggable ? inlay_ui_string_slice_prop(s, props, "dragLabel") : TextSlice{};
    TextSlice drag_data = {};
    if (draggable && !JS_HasException(s->js)) {
        const JSValue dv = JS_GetPropertyStr(s->js, props, "dragData");
        if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
            if (JS_IsString(dv)) {
                drag_data = inlay_ui_copy_text_slice(s, dv);
            } else {
                const JSValue js_str = JS_JSONStringify(s->js, dv, JS_UNDEFINED, JS_UNDEFINED);
                if (!JS_IsException(js_str) && !JS_IsUndefined(js_str)) {
                    drag_data = inlay_ui_copy_text_slice(s, js_str);
                }
                JS_FreeValue(s->js, js_str);
            }
        }
        JS_FreeValue(s->js, dv);
    }
    const TextSlice drop_accept = !JS_HasException(s->js) ? inlay_ui_string_slice_prop(s, props, "dropAccept") : TextSlice{};
    TextSlice drop_act = {}, fdrop_act = {}, dhover_act = {};
    uint32_t drop_h = INLAY_UI_NONE, fdrop_h = INLAY_UI_NONE, dhover_h = INLAY_UI_NONE;
    if (!JS_HasException(s->js)) inlay_ui_parse_event_prop(s, props, "onDrop", &drop_act, &drop_h);
    if (!JS_HasException(s->js)) inlay_ui_parse_event_prop(s, props, "onFileDrop", &fdrop_act, &fdrop_h);
    if (!JS_HasException(s->js)) inlay_ui_parse_event_prop(s, props, "onDragHover", &dhover_act, &dhover_h);
    if (JS_HasException(s->js)) return;

    if (draggable || drop_accept.len > 0u || has_event(drop_act, drop_h) ||
        has_event(fdrop_act, fdrop_h) || has_event(dhover_act, dhover_h)) {
        const uint32_t dnd_idx = inlay_ui_alloc_pool(s, s->dnd_specs, s->dnd_spec_count,
                                                     s->dnd_spec_cap, 1u, s->desc.max_nodes);
        if (dnd_idx != INLAY_UI_NONE) {
            s->dnd_specs[dnd_idx] = {
                draggable, drag_kind, drag_label, drag_data, drop_accept,
                drop_act, drop_h, fdrop_act, fdrop_h, dhover_act, dhover_h
            };
            if (auto* cold = inlay_ui_ensure_cold(s, st)) cold->dnd_idx = dnd_idx;
        }
    }
}

enum StylePropMask : uint32_t {
    PmNone        = 0u,
    PmAxis        = 1u << 0,
    PmCls         = 1u << 1,
    PmOpacity     = 1u << 2,
    PmTransform   = 1u << 3,
    PmComposite   = 1u << 4,
    PmColor       = 1u << 5,
    PmTextExtras  = 1u << 6,
    PmSizing      = 1u << 7,
    PmBorderPad   = 1u << 8,
    PmVisualCraft = 1u << 9,
    PmFont        = 1u << 10,
    PmDisabled    = 1u << 11,
    PmClick       = 1u << 12,
    PmDndKeys     = 1u << 13,
    PmAll         = 0x3FFFu
};

uint32_t scan_style_prop_mask(JSContext* js, JSValueConst props) {
    JSPropertyEnum* names = nullptr;
    uint32_t count = 0u;
    if (JS_GetOwnPropertyNames(js, &names, &count, props, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return PmAll;
    }
    uint32_t mask = PmNone;
    auto sw = [](const char* s, const char* pre) noexcept -> bool {
        return std::strncmp(s, pre, std::strlen(pre)) == 0;
    };
    for (uint32_t i = 0u; i < count; ++i) {
        const char* k = JS_AtomToCString(js, names[i].atom);
        if (!k) continue;
        switch (k[0]) {
            case 'a':
                if (std::strcmp(k, "axis") == 0) mask |= PmAxis | PmBorderPad;
                else if (std::strcmp(k, "action") == 0) mask |= PmClick;
                else if (std::strcmp(k, "alphaMask") == 0) mask |= PmComposite;
                else if (sw(k, "align")) mask |= PmBorderPad | PmTextExtras;
                else if (sw(k, "active") || sw(k, "aspect") || sw(k, "attach") ||
                         sw(k, "anchor") || std::strcmp(k, "animateLayout") == 0) mask |= PmVisualCraft;
                break;
            case 'b':
                if (std::strcmp(k, "bg") == 0 || std::strcmp(k, "background") == 0) mask |= PmSizing;
                else if (sw(k, "border")) mask |= PmBorderPad;
                else if (sw(k, "backdrop")) mask |= PmVisualCraft;
                else if (std::strcmp(k, "blendMode") == 0) mask |= PmComposite;
                else if (std::strcmp(k, "bold") == 0) mask |= PmFont | PmTextExtras;
                break;
            case 'c':
                if (std::strcmp(k, "cls") == 0 || std::strcmp(k, "className") == 0) mask |= PmCls;
                else if (std::strcmp(k, "color") == 0) mask |= PmColor;
                else if (std::strcmp(k, "cursor") == 0) mask |= PmComposite;
                else if (std::strcmp(k, "continuousCorners") == 0) mask |= PmBorderPad;
                else if (std::strcmp(k, "consumeKeys") == 0) mask |= PmDndKeys;
                else if (std::strcmp(k, "colSpan") == 0 || std::strcmp(k, "clipChildren") == 0 ||
                         std::strcmp(k, "clampToViewport") == 0) mask |= PmVisualCraft;
                break;
            case 'd':
                if (std::strcmp(k, "disabled") == 0) mask |= PmDisabled;
                else if (sw(k, "drag") || sw(k, "drop")) mask |= PmDndKeys;
                break;
            case 'e':
                if (std::strcmp(k, "ellipsis") == 0) mask |= PmTextExtras;
                else if (std::strcmp(k, "elevation") == 0) mask |= PmVisualCraft;
                break;
            case 'f':
                if (std::strcmp(k, "fontSize") == 0) mask |= PmSizing | PmTextExtras;
                else if (std::strcmp(k, "fontWeight") == 0 || std::strcmp(k, "fontFeatures") == 0) mask |= PmFont | PmTextExtras;
                else if (std::strcmp(k, "flexGrow") == 0) mask |= PmSizing | PmVisualCraft | PmTextExtras;
                else if (std::strcmp(k, "flexShrink") == 0) mask |= PmSizing | PmVisualCraft;
                else if (sw(k, "focus") || std::strcmp(k, "flipOnOverflow") == 0) mask |= PmVisualCraft;
                break;
            case 'g':
                if (std::strcmp(k, "gap") == 0) mask |= PmBorderPad;
                else if (std::strcmp(k, "globalKeys") == 0) mask |= PmDndKeys;
                else if (std::strcmp(k, "gradient") == 0 || sw(k, "grid")) mask |= PmVisualCraft;
                break;
            case 'h':
                if (std::strcmp(k, "height") == 0) mask |= PmSizing;
                else if (sw(k, "hover")) mask |= PmVisualCraft | PmTextExtras;
                break;
            case 'i':
                if (std::strcmp(k, "italic") == 0) mask |= PmFont | PmTextExtras;
                else if (std::strcmp(k, "insetShadow") == 0) mask |= PmVisualCraft;
                break;
            case 'j':
                if (sw(k, "justify")) mask |= PmBorderPad;
                break;
            case 'l':
                if (std::strcmp(k, "lineHeight") == 0 || std::strcmp(k, "letterSpacing") == 0) mask |= PmTextExtras;
                break;
            case 'm':
                if (sw(k, "margin")) mask |= PmSizing | PmTextExtras;
                else if (std::strcmp(k, "mono") == 0) mask |= PmFont | PmTextExtras;
                else if (std::strcmp(k, "maxLines") == 0) mask |= PmTextExtras;
                else if (sw(k, "min") || sw(k, "max")) mask |= PmSizing;
                break;
            case 'n':
                if (std::strcmp(k, "noLiga") == 0) mask |= PmFont | PmTextExtras;
                break;
            case 'o':
                if (std::strcmp(k, "opacity") == 0) mask |= PmOpacity;
                else if (std::strcmp(k, "opentype") == 0) mask |= PmFont | PmTextExtras;
                else if (std::strcmp(k, "offsetX") == 0 || std::strcmp(k, "offsetY") == 0) mask |= PmVisualCraft;
                else if (std::strcmp(k, "onKeyDown") == 0 || std::strcmp(k, "onTextInput") == 0 ||
                         std::strcmp(k, "onDrop") == 0 || std::strcmp(k, "onFileDrop") == 0 ||
                         std::strcmp(k, "onDragHover") == 0) mask |= PmDndKeys;
                else if (std::strcmp(k, "onClick") == 0 || std::strcmp(k, "onDoubleClick") == 0 ||
                         std::strcmp(k, "onRightClick") == 0 || std::strcmp(k, "onHover") == 0 ||
                         std::strcmp(k, "onFocus") == 0) mask |= PmClick;
                break;
            case 'p':
                if (sw(k, "padding")) mask |= PmBorderPad;
                else if (sw(k, "press") || std::strcmp(k, "pointerPassthrough") == 0) mask |= PmVisualCraft;
                break;
            case 'r':
                if (std::strcmp(k, "radius") == 0 || std::strcmp(k, "rowGap") == 0) mask |= PmBorderPad;
                else if (sw(k, "rotate")) mask |= PmTransform;
                else if (std::strcmp(k, "rowSpan") == 0) mask |= PmVisualCraft;
                break;
            case 's':
                if (std::strcmp(k, "stopPropagation") == 0) mask |= PmOpacity;
                else if (std::strcmp(k, "size") == 0) mask |= PmSizing;
                else if (sw(k, "scale")) mask |= PmTransform;
                else if (std::strcmp(k, "smallCaps") == 0 || std::strcmp(k, "slashedZero") == 0) mask |= PmFont | PmTextExtras;
                else if (std::strcmp(k, "selectable") == 0 || std::strcmp(k, "selectionColor") == 0) mask |= PmTextExtras;
                else if (sw(k, "shadow") || sw(k, "shrink") || sw(k, "scroll")) mask |= PmVisualCraft;
                break;
            case 't':
                if (sw(k, "translate")) mask |= PmTransform;
                else if (std::strcmp(k, "transition") == 0) mask |= PmComposite;
                else if (std::strcmp(k, "tabularNums") == 0) mask |= PmFont | PmTextExtras;
                else if (std::strcmp(k, "topRimAlpha") == 0) mask |= PmVisualCraft;
                break;
            case 'w':
                if (std::strcmp(k, "width") == 0) mask |= PmSizing;
                else if (std::strcmp(k, "wrap") == 0) mask |= PmVisualCraft | PmTextExtras;
                else if (std::strcmp(k, "windowDragPassthrough") == 0) mask |= PmVisualCraft;
                break;
            case 'z':
                if (std::strcmp(k, "zIndex") == 0) mask |= PmVisualCraft;
                break;
            default:
                break;
        }
        JS_FreeCString(js, k);
    }
    JS_FreePropertyEnum(js, names, count);
    return mask;
}

void parse_style(InlaySurface* s, JSValueConst props, InlayUiKind kind,
                 NodeStyle& st, uint32_t node_index, bool default_col_hint) {
    inlay_ui_init_node_style(st);
    const bool default_col = default_col_hint ||
        kind == INLAY_UI_SCROLL || kind == INLAY_UI_CARD || kind == INLAY_UI_FORM ||
        kind == INLAY_UI_POPOVER || kind == INLAY_UI_TREE || kind == INLAY_UI_ACCORDION ||
        kind == INLAY_UI_DOC || kind == INLAY_UI_TERMINAL;
    if (default_col) st.flags |= NfColumn;

    const uint32_t pm = scan_style_prop_mask(s->js, props);

    // Pre-apply explicit `axis` before `cls` so axis-dependent utilities (`items-*`, `gap-y-*`) resolve accurately.
    if (pm & PmAxis) {
        peek_str_prop(s, props, "axis", [&](const char* c, size_t n) noexcept {
            if (inlay_ui_str_eq(c, n, "row")) st.flags &= ~NfColumn;
            else if (inlay_ui_str_eq(c, n, "column") || inlay_ui_str_eq(c, n, "col")) st.flags |= NfColumn;
            return true;
        });
    }

    // 1. Utility classes (`cls` / `className`) run FIRST so explicit props override them.
    if (pm & PmCls) {
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
    }
    if (JS_HasException(s->js)) return;
    if ((kind == INLAY_UI_TEXT || kind == INLAY_UI_BOX) && (pm & ~(PmAxis | PmCls)) == 0u) return;

    // 2. Shared opacity, transforms, compositing, transitions & event scoping
    if (pm & PmOpacity) {
        float op = st.opacity;
        if (inlay_ui_opt_number_prop(s, props, "opacity", &op)) {
            st.opacity = std::fmin(1.f, std::fmax(0.f, op));
            st.flags |= NfHasOpacity;
        }
        if (inlay_ui_bool_prop(s, props, "stopPropagation")) {
            if (auto* cx = ensure_core_extra(s, node_index)) cx->flags |= CefStopPropagation;
        }
    }
    if (pm & PmTransform) parse_transform_props(s, props, st);
    if (pm & PmComposite) parse_compositing_and_transition_props(s, props, st);
    if (pm & PmColor) parse_color_prop(s, props, "color", &st.color);
    if (JS_HasException(s->js)) return;

    if (kind == INLAY_UI_TEXT) {
        if ((pm & PmTextExtras) == 0u) return;
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

    if (pm & PmSizing) {
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
    }

    if (kind == INLAY_UI_SPACER) {
        if (pm & PmSizing) {
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
        }
        return;
    }

    if (kind == INLAY_UI_DIVIDER) {
        if (pm & PmAxis) {
            peek_str_prop(s, props, "axis", [&](const char* c, size_t n) noexcept {
                if (inlay_ui_str_eq(c, n, "column") || inlay_ui_str_eq(c, n, "col")) st.flags |= NfColumn;
                else if (inlay_ui_str_eq(c, n, "row")) st.flags &= ~NfColumn;
                return true;
            });
        }
        return;
    }

    // Shared visual & container styling props
    if (pm & PmBorderPad) {
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
    } // end PmBorderPad

    // Visual craft, state overrides & layout props for containers, buttons, widgets, and extended widgets
    if ((pm & PmVisualCraft) &&
        (kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_FORM || kind == INLAY_UI_SCROLL ||
         kind == INLAY_UI_POPOVER || kind == INLAY_UI_BADGE || kind == INLAY_UI_BUTTON ||
         kind == INLAY_UI_SLOT || kind == INLAY_UI_TABS || kind == INLAY_UI_MATH ||
         kind == INLAY_UI_DIAGRAM || kind == INLAY_UI_IMAGE || kind == INLAY_UI_MODAL ||
         kind == INLAY_UI_INPUT || kind == INLAY_UI_COMBOBOX || inlay_ui_is_ext_widget(kind))) {
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
        if (inlay_ui_bool_prop(s, props, "windowDragPassthrough")) {
            if (auto* cx = ensure_core_extra(s, node_index)) cx->flags |= CefWindowDragPassthrough;
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
    if ((pm & PmFont) &&
        (kind == INLAY_UI_BUTTON || kind == INLAY_UI_BADGE || kind == INLAY_UI_INPUT ||
         kind == INLAY_UI_CHECKBOX || kind == INLAY_UI_TOGGLE || kind == INLAY_UI_TABS ||
         inlay_ui_is_ext_widget(kind))) {
        parse_font_props(s, props, st);
        parse_opentype_props(s, props, st);
    }

    if ((pm & PmDisabled) &&
        (kind == INLAY_UI_BUTTON || kind == INLAY_UI_INPUT || kind == INLAY_UI_CHECKBOX ||
         kind == INLAY_UI_TOGGLE || kind == INLAY_UI_SLIDER || kind == INLAY_UI_COMBOBOX ||
         kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_MENU)) {
        if (inlay_ui_bool_prop(s, props, "disabled")) st.flags |= NfDisabled;
    }

    if ((pm & PmClick) &&
        (kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_SCROLL ||
         kind == INLAY_UI_BADGE || kind == INLAY_UI_IMAGE || kind == INLAY_UI_AVATAR)) {
        parse_clickable_events(s, props, st, node_index);
    }
    if ((pm & PmDndKeys) &&
        (kind == INLAY_UI_BOX || kind == INLAY_UI_CARD || kind == INLAY_UI_BUTTON ||
         kind == INLAY_UI_SCROLL || kind == INLAY_UI_INPUT || kind == INLAY_UI_CODE_EDITOR ||
         kind == INLAY_UI_TERMINAL || kind == INLAY_UI_TREE_ITEM || kind == INLAY_UI_DOCK_PANE)) {
        parse_dnd_and_keys(s, props, st);
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
        if (pm & PmClick) parse_clickable_events(s, props, st, node_index);
        else if (has_event(st.action, st.handler)) st.flags |= NfClickable;
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
        // Round 2 rich CodeEditor bridge (Diff / inlineHighlights / overviewMarkers / stickyHeader / autoHeight / ViewZones / Folds / scrollSync / gutterRange)
        if (!JS_HasException(s->js)) {
            CodeEditorRichSpec espec = {};
            espec.on_scroll_handler = INLAY_UI_NONE;
            espec.on_gutter_click_handler = INLAY_UI_NONE;
            espec.on_gutter_range_handler = INLAY_UI_NONE;
            espec.on_cursor_handler = INLAY_UI_NONE;
            bool any_rich = false;
            espec.language = inlay_ui_string_slice_prop(s, props, "language");
            if (espec.language.len > 0u) any_rich = true;
            if (inlay_ui_opt_bool_prop(s, props, "highlightBrackets", &espec.highlight_brackets)) {
                espec.has_highlight_brackets = true; any_rich = true;
            }
            if (inlay_ui_opt_bool_prop(s, props, "showIndentGuides", &espec.indent_guides)) {
                espec.has_indent_guides = true; any_rich = true;
            }
            if (inlay_ui_opt_bool_prop(s, props, "showDiagnosticTooltips", &espec.diag_tooltips)) {
                espec.has_diag_tooltips = true; any_rich = true;
            }
            if (inlay_ui_parse_event_prop(s, props, "onCursorChange", &espec.on_cursor_action, &espec.on_cursor_handler)) {
                any_rich = true;
            }
            if (inlay_ui_bool_prop(s, props, "autoHeight")) { espec.auto_height = true; any_rich = true; }
            if (inlay_ui_bool_prop(s, props, "showOverviewRuler")) { espec.show_overview_ruler = true; any_rich = true; }
            if (inlay_ui_bool_prop(s, props, "showMinimap")) { espec.show_minimap = true; any_rich = true; }
            if (inlay_ui_opt_bool_prop(s, props, "showLineNumbers", &espec.show_line_numbers)) {
                espec.has_show_line_numbers = true; any_rich = true;
            }
            if (inlay_ui_opt_number_prop(s, props, "gutterTagWidth", &espec.gutter_tag_width, false, 0.0, 256.0)) any_rich = true;
            if (inlay_ui_opt_number_prop(s, props, "scrollX", &espec.scroll_x, false, 0.0, 1e7)) { espec.has_scroll_x = true; any_rich = true; }
            if (inlay_ui_opt_number_prop(s, props, "scrollY", &espec.scroll_y, false, 0.0, 1e7)) { espec.has_scroll_y = true; any_rich = true; }
            espec.sticky_header = inlay_ui_string_slice_prop(s, props, "stickyHeader");
            if (espec.sticky_header.len > 0u) any_rich = true;
            if (inlay_ui_parse_event_prop(s, props, "onScroll", &espec.on_scroll_action, &espec.on_scroll_handler)) any_rich = true;
            if (inlay_ui_parse_event_prop(s, props, "onGutterClick", &espec.on_gutter_click_action, &espec.on_gutter_click_handler)) any_rich = true;
            if (inlay_ui_parse_event_prop(s, props, "onGutterRange", &espec.on_gutter_range_action, &espec.on_gutter_range_handler)) any_rich = true;

            auto read_len = [&](JSValueConst arr, uint32_t cap) -> uint32_t {
                if (!JS_IsArray(arr)) return 0u;
                const JSValue lv = JS_GetPropertyStr(s->js, arr, "length");
                uint32_t n = 0u;
                (void)JS_ToUint32(s->js, &n, lv);
                JS_FreeValue(s->js, lv);
                return n > cap ? cap : n;
            };
            // inlayHints
            {
                const JSValue arr = JS_GetPropertyStr(s->js, props, "inlayHints");
                const uint32_t n = read_len(arr, 256u);
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->editor_hints, s->editor_hint_count, s->editor_hint_cap, n, 4096u);
                    if (off != INLAY_UI_NONE) {
                        espec.hint_off = off; espec.hint_cnt = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            EditorInlayHintItem it = {};
                            const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "line", 10000000u, &it.line);
                                inlay_ui_opt_uint_prop(s, el, "col", 1000000u, &it.col);
                                it.text = inlay_ui_string_slice_prop(s, el, "text");
                                parse_color_prop(s, el, "color", &it.color);
                                parse_color_prop(s, el, "bg", &it.bg);
                                parse_color_prop(s, el, "borderColor", &it.border_color);
                                inlay_ui_opt_number_prop(s, el, "radius", &it.radius, false, 0.0, 32.0);
                            }
                            JS_FreeValue(s->js, el);
                            s->editor_hints[off + i] = it;
                        }
                    }
                }
                JS_FreeValue(s->js, arr);
            }
            // inlineHighlights
            {
                const JSValue arr = JS_GetPropertyStr(s->js, props, "inlineHighlights");
                const uint32_t n = read_len(arr, 512u);
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->editor_inline_hls, s->editor_inline_hl_count, s->editor_inline_hl_cap, n, 8192u);
                    if (off != INLAY_UI_NONE) {
                        espec.hl_off = off; espec.hl_cnt = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            EditorInlineHlItem it = {};
                            const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "line", 10000000u, &it.line);
                                inlay_ui_opt_uint_prop(s, el, "startCol", 1000000u, &it.start_col);
                                inlay_ui_opt_uint_prop(s, el, "endCol", 1000000u, &it.end_col);
                                parse_color_prop(s, el, "bg", &it.bg);
                                parse_color_prop(s, el, "borderColor", &it.border_color);
                                inlay_ui_opt_number_prop(s, el, "radius", &it.radius, false, 0.0, 32.0);
                            }
                            JS_FreeValue(s->js, el);
                            s->editor_inline_hls[off + i] = it;
                        }
                    }
                }
                JS_FreeValue(s->js, arr);
            }
            // overviewMarkers
            {
                const JSValue arr = JS_GetPropertyStr(s->js, props, "overviewMarkers");
                const uint32_t n = read_len(arr, 512u);
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->editor_markers, s->editor_marker_count, s->editor_marker_cap, n, 8192u);
                    if (off != INLAY_UI_NONE) {
                        espec.marker_off = off; espec.marker_cnt = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            EditorOverviewItem it = {};
                            const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "line", 10000000u, &it.line);
                                parse_color_prop(s, el, "color", &it.color);
                                uint32_t lane = 0u;
                                if (inlay_ui_opt_uint_prop(s, el, "lane", 2u, &lane)) it.lane = uint8_t(lane);
                            }
                            JS_FreeValue(s->js, el);
                            s->editor_markers[off + i] = it;
                        }
                    }
                }
                JS_FreeValue(s->js, arr);
            }
            // lineDecorations
            {
                const JSValue arr = JS_GetPropertyStr(s->js, props, "lineDecorations");
                const uint32_t n = read_len(arr, 512u);
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->editor_line_decos, s->editor_line_deco_count, s->editor_line_deco_cap, n, 8192u);
                    if (off != INLAY_UI_NONE) {
                        espec.deco_off = off; espec.deco_cnt = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            EditorLineDecoItem it = {};
                            const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "line", 10000000u, &it.line);
                                parse_color_prop(s, el, "rowBg", &it.row_bg);
                                parse_color_prop(s, el, "gutterBar", &it.gutter_bar);
                                parse_color_prop(s, el, "gutterTagColor", &it.gutter_tag_color);
                                it.gutter_tag = inlay_ui_string_slice_prop(s, el, "gutterTag");
                            }
                            JS_FreeValue(s->js, el);
                            s->editor_line_decos[off + i] = it;
                        }
                    }
                }
                JS_FreeValue(s->js, arr);
            }
            // folds
            {
                const JSValue arr = JS_GetPropertyStr(s->js, props, "folds");
                const uint32_t n = read_len(arr, 256u);
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->editor_folds, s->editor_fold_count, s->editor_fold_cap, n, 4096u);
                    if (off != INLAY_UI_NONE) {
                        espec.fold_off = off; espec.fold_cnt = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            EditorFoldItem it = {};
                            const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "start", 10000000u, &it.start);
                                inlay_ui_opt_uint_prop(s, el, "end", 10000000u, &it.end);
                                it.collapsed = inlay_ui_bool_prop(s, el, "collapsed");
                            }
                            JS_FreeValue(s->js, el);
                            s->editor_folds[off + i] = it;
                        }
                    }
                }
                JS_FreeValue(s->js, arr);
            }
            // viewZones
            {
                const JSValue arr = JS_GetPropertyStr(s->js, props, "viewZones");
                const uint32_t n = read_len(arr, 64u);
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->editor_view_zones, s->editor_view_zone_count, s->editor_view_zone_cap, n, 1024u);
                    if (off != INLAY_UI_NONE) {
                        espec.vz_off = off; espec.vz_cnt = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            EditorViewZoneItem it = {};
                            it.height = 60.f;
                            const JSValue el = JS_GetPropertyUint32(s->js, arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "id", UINT32_MAX, &it.id);
                                inlay_ui_opt_uint_prop(s, el, "afterLine", 10000000u, &it.after_line);
                                inlay_ui_opt_number_prop(s, el, "height", &it.height, false, 4.0, 2048.0);
                                it.above_line = inlay_ui_bool_prop(s, el, "aboveLine");
                                parse_color_prop(s, el, "fillColor", &it.fill_color);
                            }
                            JS_FreeValue(s->js, el);
                            s->editor_view_zones[off + i] = it;
                        }
                    }
                }
                JS_FreeValue(s->js, arr);
            }
            if (any_rich && !JS_HasException(s->js)) {
                const uint32_t eidx = inlay_ui_alloc_pool(s, s->editor_rich_specs, s->editor_rich_spec_count,
                                                          s->editor_rich_spec_cap, 1u, s->desc.max_nodes);
                if (eidx != INLAY_UI_NONE) {
                    s->editor_rich_specs[eidx] = espec;
                    st.ext_idx = eidx;
                }
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
        if (inlay_ui_bool_prop(s, props, "autoGrow") && !password) flags |= CefInputAutoGrow;
        float min_h = 0.f;
        float max_h = 0.f;
        const bool has_min_h = !JS_HasException(s->js) && inlay_ui_opt_number_prop(s, props, "minHeight", &min_h, false, 16.0, 2048.0);
        const bool has_max_h = !JS_HasException(s->js) && inlay_ui_opt_number_prop(s, props, "maxHeight", &max_h, false, 16.0, 4096.0);
        if (has_min_h || has_max_h) flags |= CefInputHasAutoGrowRange;
        bool submit_enter = false;
        if (inlay_ui_opt_bool_prop(s, props, "submitOnEnter", &submit_enter)) {
            flags |= CefInputHasSubmitEnter;
            if (submit_enter) flags |= CefInputSubmitEnterVal;
        }
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
                if (has_min_h) cx->aux_f0 = min_h;
                if (has_max_h) cx->aux_f1 = max_h;
            }
        }
        // Round 2 rich Input bridge (highlights/atomic chips, ghostText, onAcceptGhost, onPasteImage, onCaret)
        if (!JS_HasException(s->js)) {
            InputRichSpec ispec = {};
            ispec.on_accept_ghost_handler = INLAY_UI_NONE;
            ispec.on_paste_image_handler = INLAY_UI_NONE;
            ispec.on_caret_handler = INLAY_UI_NONE;
            bool any_rich = false;
            ispec.ghost_text = inlay_ui_string_slice_prop(s, props, "ghostText");
            if (ispec.ghost_text.len > 0u) any_rich = true;
            if (parse_color_prop(s, props, "ghostColor", &ispec.ghost_color)) any_rich = true;
            if (inlay_ui_bool_prop(s, props, "acceptGhostOnTab")) { ispec.accept_ghost_on_tab = true; any_rich = true; }
            if (inlay_ui_parse_event_prop(s, props, "onAcceptGhost", &ispec.on_accept_ghost_action, &ispec.on_accept_ghost_handler)) any_rich = true;
            if (inlay_ui_parse_event_prop(s, props, "onPasteImage", &ispec.on_paste_image_action, &ispec.on_paste_image_handler)) any_rich = true;
            if (inlay_ui_parse_event_prop(s, props, "onCaret", &ispec.on_caret_action, &ispec.on_caret_handler)) any_rich = true;
            const JSValue hl_arr = JS_GetPropertyStr(s->js, props, "highlights");
            if (JS_IsArray(hl_arr)) {
                const JSValue lv = JS_GetPropertyStr(s->js, hl_arr, "length");
                uint32_t n = 0u;
                (void)JS_ToUint32(s->js, &n, lv);
                JS_FreeValue(s->js, lv);
                if (n > 128u) n = 128u;
                if (n > 0u) {
                    const uint32_t off = inlay_ui_alloc_pool(s, s->input_highlights, s->input_highlight_count, s->input_highlight_cap, n, 4096u);
                    if (off != INLAY_UI_NONE) {
                        ispec.hl_offset = off; ispec.hl_count = n; any_rich = true;
                        for (uint32_t i = 0u; i < n; ++i) {
                            InputHighlightItem item = {};
                            const JSValue el = JS_GetPropertyUint32(s->js, hl_arr, i);
                            if (JS_IsObject(el) && !JS_IsNull(el)) {
                                inlay_ui_opt_uint_prop(s, el, "offset", 65536u, &item.offset);
                                inlay_ui_opt_uint_prop(s, el, "length", 65536u, &item.length);
                                parse_color_prop(s, el, "color", &item.color);
                                parse_color_prop(s, el, "bg", &item.bg);
                                parse_color_prop(s, el, "borderColor", &item.border_color);
                                inlay_ui_opt_number_prop(s, el, "radius", &item.radius, false, 0.0, 32.0);
                                item.atomic = inlay_ui_bool_prop(s, el, "atomic");
                            }
                            JS_FreeValue(s->js, el);
                            s->input_highlights[off + i] = item;
                        }
                    }
                }
            }
            JS_FreeValue(s->js, hl_arr);
            if (any_rich && !JS_HasException(s->js)) {
                const uint32_t ridx = inlay_ui_alloc_pool(s, s->input_rich_specs, s->input_rich_spec_count,
                                                          s->input_rich_spec_cap, 1u, s->desc.max_nodes);
                if (ridx != INLAY_UI_NONE) {
                    s->input_rich_specs[ridx] = ispec;
                    st.ext_idx = ridx;
                }
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
            float tr = 0.f, thr = 0.f, tpad = 0.f;
            const bool has_tr = inlay_ui_opt_number_prop(s, props, "trackRadius", &tr, false, 0.0, 128.0);
            const bool has_thr = inlay_ui_opt_number_prop(s, props, "thumbRadius", &thr, false, 0.0, 128.0);
            const bool has_tpad = inlay_ui_opt_number_prop(s, props, "thumbPadding", &tpad, false, 0.0, 32.0);
            ColorRef off_col = {};
            const bool has_off = parse_color_prop(s, props, "offColor", &off_col);
            ColorRef thumb_col = {};
            const bool has_thumb = parse_color_prop(s, props, "thumbColor", &thumb_col);
            if (has_tw || has_th || has_tr || has_thr || has_tpad || has_off || has_thumb) {
                if (auto* cx = ensure_core_extra(s, node_index)) {
                    if (has_tw) { cx->aux_f0 = tw; cx->flags |= CefSwitchHasTrackW; }
                    if (has_th) { cx->aux_f1 = th; cx->flags |= CefSwitchHasTrackH; }
                    if (has_tr) { cx->aux_f2 = tr; cx->flags |= CefSwitchHasTrackR; }
                    if (has_thr) { cx->aux_f3 = thr; cx->flags |= CefSwitchHasThumbR; }
                    if (has_tpad) { cx->aux_f4 = tpad; cx->flags |= CefSwitchHasThumbPad; }
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
        ColorRef sec_col = {};
        if (parse_color_prop(s, props, "secondaryColor", &sec_col)) {
            if (auto* cx = ensure_core_extra(s, node_index)) {
                cx->color_a = sec_col;
                cx->flags |= CefIconHasSecondaryCol;
            }
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
        float div_sz = 0.f, div_hit = 0.f;
        const bool has_div_sz = inlay_ui_opt_number_prop(s, props, "dividerSize", &div_sz, false, 0.0, 64.0);
        const bool has_div_hit = inlay_ui_opt_number_prop(s, props, "dividerHit", &div_hit, false, 1.0, 64.0);
        bool show_grip = true;
        const bool has_grip = inlay_ui_opt_bool_prop(s, props, "showGrip", &show_grip);
        if (has_div_sz || has_div_hit || (has_grip && !show_grip)) {
            if (auto* cx = ensure_core_extra(s, node_index)) {
                if (has_div_sz) { cx->aux_f0 = div_sz; cx->flags |= CefSplitHasDividerSz; }
                if (has_div_hit) { cx->aux_f1 = div_hit; cx->flags |= CefSplitHasDividerHit; }
                if (has_grip && !show_grip) cx->flags |= CefSplitHideGrip;
            }
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
    if (inlay_ui_str_eq(chars, len, "portal")) { *out = INLAY_UI_PORTAL; return true; }
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
    if (inlay_ui_str_eq(chars, len, "dockSpace") || inlay_ui_str_eq(chars, len, "dock-space") ||
        inlay_ui_str_eq(chars, len, "dockspace")) { *out = INLAY_UI_DOCK_SPACE; return true; }
    if (inlay_ui_str_eq(chars, len, "dockPane") || inlay_ui_str_eq(chars, len, "dock-pane") ||
        inlay_ui_str_eq(chars, len, "dockpane")) { *out = INLAY_UI_DOCK_PANE; return true; }
    if (inlay_ui_str_eq(chars, len, "sheet")) { *out = INLAY_UI_SHEET; return true; }
    if (inlay_ui_str_eq(chars, len, "hoverCard") || inlay_ui_str_eq(chars, len, "hover-card") ||
        inlay_ui_str_eq(chars, len, "hovercard")) { *out = INLAY_UI_HOVER_CARD; return true; }
    if (inlay_ui_str_eq(chars, len, "radioGroup") || inlay_ui_str_eq(chars, len, "radio-group") ||
        inlay_ui_str_eq(chars, len, "radiogroup")) { *out = INLAY_UI_RADIO_GROUP; return true; }
    if (inlay_ui_str_eq(chars, len, "toggleGroup") || inlay_ui_str_eq(chars, len, "toggle-group") ||
        inlay_ui_str_eq(chars, len, "togglegroup")) { *out = INLAY_UI_TOGGLE_GROUP; return true; }
    if (inlay_ui_str_eq(chars, len, "colorPicker") || inlay_ui_str_eq(chars, len, "color-picker") ||
        inlay_ui_str_eq(chars, len, "colorpicker")) { *out = INLAY_UI_COLOR_PICKER; return true; }
    if (inlay_ui_str_eq(chars, len, "chart")) { *out = INLAY_UI_CHART; return true; }
    if (inlay_ui_str_eq(chars, len, "viewZone") || inlay_ui_str_eq(chars, len, "view-zone") ||
        inlay_ui_str_eq(chars, len, "viewzone")) { *out = INLAY_UI_VIEW_ZONE; return true; }
    if (inlay_ui_str_eq(chars, len, "diffView") || inlay_ui_str_eq(chars, len, "diff-view") ||
        inlay_ui_str_eq(chars, len, "diffview")) { *out = INLAY_UI_DIFF_VIEW; return true; }
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
    e->memo_cache_bytes = 0u;
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
        if (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count) {
            const auto& c = s->cold_visuals[st.cold_idx];
            if (c.dnd_idx != INLAY_UI_NONE || c.on_key_handler != INLAY_UI_NONE || c.on_key_action.len > 0u ||
                c.on_text_handler != INLAY_UI_NONE || c.on_text_action.len > 0u) return;
        }
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
    if (o.total > kMaxMemoCacheTotalBytes) return;
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

    // Reuse slot with matching memo_key, or empty slot, enforcing 2 MiB total byte budget via LRU eviction.
    MemoNativeEntry* slot = nullptr;
    for (uint32_t i = 0u; i < kMemoCacheCap; ++i) {
        if (e->memo_cache[i].blob && e->memo_cache[i].memo_key == memo_key) {
            slot = &e->memo_cache[i];
            e->memo_cache_bytes -= slot->blob_bytes;
            SDL_free(slot->blob);
            *slot = {};
            break;
        }
        if (!e->memo_cache[i].blob && !slot) {
            slot = &e->memo_cache[i];
        }
    }
    while (e->memo_cache_bytes + o.total > kMaxMemoCacheTotalBytes) {
        uint32_t lru = kMemoCacheCap;
        for (uint32_t i = 0u; i < kMemoCacheCap; ++i) {
            if (!e->memo_cache[i].blob) continue;
            if (lru == kMemoCacheCap || e->memo_cache[i].last_used < e->memo_cache[lru].last_used) {
                lru = i;
            }
        }
        if (lru == kMemoCacheCap) break;
        e->memo_cache_bytes -= e->memo_cache[lru].blob_bytes;
        SDL_free(e->memo_cache[lru].blob);
        e->memo_cache[lru] = {};
        if (!slot) slot = &e->memo_cache[lru];
    }
    if (!slot) {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < kMemoCacheCap; ++i) {
            if (e->memo_cache[i].last_used < e->memo_cache[victim].last_used) {
                victim = i;
            }
        }
        slot = &e->memo_cache[victim];
        e->memo_cache_bytes -= slot->blob_bytes;
        SDL_free(slot->blob);
        *slot = {};
    }
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
        blob,
        o.total
    };
    e->memo_cache_bytes += o.total;
}

inline uint8_t container_width_bucket(float w) noexcept {
    if (w >= 840.0f) return 3u;
    if (w >= 560.0f) return 2u;
    if (w >= 360.0f) return 1u;
    return 0u;
}

ContainerWidthEntry* find_container_width(InlaySurface* s, uint32_t node_id) noexcept {
    auto* e = ext(s);
    for (uint32_t i = 0u; i < e->container_width_count; ++i) {
        if (e->container_widths[i].node_id == node_id) {
            return &e->container_widths[i];
        }
    }
    return nullptr;
}

ContainerWidthEntry* find_or_add_container_width(InlaySurface* s, uint32_t node_id) noexcept {
    if (auto* existing = find_container_width(s, node_id)) return existing;
    auto* e = ext(s);
    ContainerWidthEntry* slot = nullptr;
    if (e->container_width_count < kContainerWidthCacheCap) {
        slot = &e->container_widths[e->container_width_count++];
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < e->container_width_count; ++i) {
            if (e->container_widths[i].last_used_frame < e->container_widths[victim].last_used_frame) {
                victim = i;
            }
        }
        slot = &e->container_widths[victim];
    }
    *slot = {};
    slot->node_id = node_id;
    slot->last_used_frame = s->frame_counter;
    return slot;
}

void observe_container_width(InlaySurface* s, uint32_t path_id, float width_px) noexcept {
    ContainerWidthEntry* ent = find_or_add_container_width(s, path_id);
    if (!ent) return;
    ent->last_used_frame = s->frame_counter;
    if (width_px <= 1.0f) {
        if (!ent->measured && !ent->requested_measure) {
            ent->requested_measure = true;
            etch_request_redraw(s->etch);
        }
        return;
    }
    const uint8_t new_b = container_width_bucket(width_px);
    if (new_b == ent->built_bucket) {
        ent->width_px = width_px;
        ent->bucket = new_b;
        ent->measured = true;
        return;
    }
    if (ent->measured && ent->last_rebuild_frame > 0u &&
        s->frame_counter >= ent->last_rebuild_frame &&
        (s->frame_counter - ent->last_rebuild_frame) <= 2u &&
        std::fabs(s->last_view_width_px - ent->last_rebuild_view_w) <= 0.5f &&
        new_b == ent->prev_bucket) {
        ent->width_px = width_px;
        ent->bucket = new_b;
        return;
    }
    ent->prev_bucket = ent->built_bucket;
    ent->width_px = width_px;
    ent->last_rebuild_view_w = s->last_view_width_px;
    ent->bucket = new_b;
    ent->measured = true;
    ent->last_rebuild_frame = s->frame_counter;
    ext(s)->needs_container_rebuild = true;
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

    // I07 Track enclosing ErrorBoundary key on handlers & subtree roots.
    const TextSlice prev_eb_key = ext(s)->cur_eb_key;
    if (ext(s)->atom_eb_key != JS_ATOM_NULL) {
        const JSValue eb_val = JS_GetProperty(s->js, value, ext(s)->atom_eb_key);
        if (JS_IsString(eb_val)) {
            size_t eb_len = 0u;
            const char* eb_chars = JS_ToCStringLen(s->js, &eb_len, eb_val);
            if (eb_chars && eb_len > 0u) {
                const bool same_eb = prev_eb_key.len == eb_len && s->text &&
                                     prev_eb_key.off <= s->text_used &&
                                     prev_eb_key.len <= s->text_used - prev_eb_key.off &&
                                     std::memcmp(s->text + prev_eb_key.off, eb_chars, eb_len) == 0;
                if (!same_eb) {
                    const TextSlice eb_sl = append_text_raw(s, eb_chars, eb_len);
                    if (eb_sl.len > 0u) {
                        ext(s)->cur_eb_key = eb_sl;
                        if (ext(s)->boundary_root_count < kMaxBoundaryRoots) {
                            ext(s)->boundary_roots[ext(s)->boundary_root_count++] = {index, eb_sl};
                        }
                    }
                }
            }
            if (eb_chars) JS_FreeCString(s->js, eb_chars);
        }
        JS_FreeValue(s->js, eb_val);
    }

    // I03 Look up measured layout width for @container nodes before parsing styles.
    const float prev_container_w = s->current_container_width_px;
    ContainerWidthEntry* cw_entry = find_container_width(s, path_id);
    if (cw_entry && cw_entry->measured && cw_entry->width_px > 1.0f) {
        s->current_container_width_px = cw_entry->width_px;
    }

    parse_style(s, props, kind, s->styles[index], index, default_col_hint);
    if (JS_HasException(s->js)) {
        s->current_container_width_px = prev_container_w;
        ext(s)->cur_eb_key = prev_eb_key;
        JS_FreeValue(s->js, props);
        return INLAY_UI_NONE;
    }

    // #52 / I03 Container query width scoping for descendants.
    const bool is_at_container = (s->styles[index].flags2 & kNf2Container) != 0u;
    if (!is_at_container) {
        s->current_container_width_px = prev_container_w;
    }
    if (is_container_kind(kind) && (s->styles[index].flags2 & Nf2WExplicit) != 0u) {
        if (s->styles[index].w_kind == uint8_t(ETCH_SIZING_FIXED) && s->styles[index].w_val > 0.f) {
            s->current_container_width_px = s->styles[index].w_val;
        } else if (s->styles[index].w_kind == uint8_t(ETCH_SIZING_PERCENT) &&
                   s->styles[index].w_val > 0.f && prev_container_w > 0.f) {
            s->current_container_width_px = prev_container_w * s->styles[index].w_val;
        }
    }
    if (is_at_container) {
        const float eff_ct = s->current_container_width_px > 0.f
            ? s->current_container_width_px
            : s->last_view_width_px;
        cw_entry = find_or_add_container_width(s, path_id);
        if (cw_entry) {
            cw_entry->built_bucket = container_width_bucket(eff_ct);
            cw_entry->last_used_frame = s->frame_counter;
        }
    }

    if (inlay_ui_is_ext_widget(kind)) {
        if (!inlay_ui_parse_ext_widget(s, props, kind, s->nodes[index], s->styles[index], path_id)) {
            s->current_container_width_px = prev_container_w;
            ext(s)->cur_eb_key = prev_eb_key;
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
        ext(s)->cur_eb_key = prev_eb_key;
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
    ext(s)->cur_eb_key = prev_eb_key;
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
        InlaySurface* target = storage->host_surface ? storage->host_surface : s;
        return storage->audit_fn(target, name, perm, req_approval, payload, storage->audit_user);
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
    const uint32_t max_payload = s->desc.max_state_bytes ? s->desc.max_state_bytes : 65536u;
    if (payload && length <= max_payload) {
        const EtchStr name_str = {binding->name, static_cast<uint32_t>(std::strlen(binding->name)), true};
        const EtchStr payload_str = {payload, static_cast<uint32_t>(length), false};
        if (check_action_authorized(s, name_str, payload_str)) {
            allowed = true;
            InlaySurface* target = ext(s)->host_surface ? ext(s)->host_surface : s;
            binding->callback(target, payload_str, binding->user);
        }
    } else if (payload) {
        JS_ThrowTypeError(js, "Action payload exceeds max_state_bytes");
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

JSValue schedule_js_timer(JSContext* js, int argc, JSValueConst* argv, bool is_interval, bool is_raf) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    double delay = 0.0;
    if (argc < 1 || !JS_IsFunction(js, argv[0])) {
        return JS_ThrowTypeError(js, "Invalid timer");
    }
    if (!is_raf) {
        if ((argc > 1 && JS_ToFloat64(js, &delay, argv[1]) < 0) || !std::isfinite(delay) ||
            delay < 0.0 || delay > 86400000.0) {
            return JS_ThrowTypeError(js, "Invalid timer");
        }
        if (is_interval && delay < 1.0) delay = 1.0;
    }
    for (auto& timer : s->timers) {
        if (JS_IsUndefined(timer.function)) {
            timer.function = JS_DupValue(js, argv[0]);
            timer.due = inlay_ui_now_ms(s) + (is_raf ? 0.0 : delay);
            timer.interval_ms = is_interval ? delay : 0.0;
            timer.id = ++s->next_timer;
            timer.is_raf = is_raf;
            if (is_raf && s->etch) etch_request_redraw(s->etch);
            return JS_NewUint32(js, timer.id);
        }
    }
    return JS_ThrowInternalError(js, "Timer capacity exceeded");
}

JSValue js_set_timeout(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    return schedule_js_timer(js, argc, argv, false, false);
}

JSValue js_set_interval(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    return schedule_js_timer(js, argc, argv, true, false);
}

JSValue js_request_animation_frame(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    return schedule_js_timer(js, argc, argv, false, true);
}

JSValue js_clear_timeout(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    uint32_t id = 0u;
    if (argc) JS_ToUint32(js, &id, argv[0]);
    for (auto& timer : s->timers) {
        if (timer.id == id && id != 0u) {
            JS_FreeValue(js, timer.function);
            timer.function = JS_UNDEFINED;
            timer.due = 0.0;
            timer.interval_ms = 0.0;
            timer.id = 0u;
            timer.is_raf = false;
        }
    }
    return JS_UNDEFINED;
}

JSValue js_console_emit(JSContext* js, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->console_fn) return JS_UNDEFINED;
    char stack_buf[1024];
    char* heap_buf = nullptr;
    char* dst = stack_buf;
    size_t cap = sizeof(stack_buf);
    size_t len = 0u;
    auto append = [&](const char* p, size_t n) {
        if (!p || n == 0u) return;
        if (len + n + 1u > cap) {
            size_t ncap = cap * 2u;
            while (ncap < len + n + 1u) ncap *= 2u;
            if (ncap > 65536u) ncap = 65536u;
            if (len + 1u >= ncap) return;
            auto* nb = static_cast<char*>(heap_buf ? SDL_realloc(heap_buf, ncap) : SDL_malloc(ncap));
            if (!nb) return;
            if (!heap_buf && len > 0u) std::memcpy(nb, stack_buf, len);
            heap_buf = nb;
            dst = heap_buf;
            cap = ncap;
        }
        const size_t avail = cap - 1u - len;
        const size_t take = n < avail ? n : avail;
        if (take > 0u) {
            std::memcpy(dst + len, p, take);
            len += take;
        }
    };
    for (int i = 0; i < argc; ++i) {
        if (i > 0) append(" ", 1u);
        JSValue str_v = JS_UNDEFINED;
        bool use_json = JS_IsObject(argv[i]) && !JS_IsNull(argv[i]) && !JS_IsFunction(js, argv[i]);
        if (use_json) {
            str_v = JS_JSONStringify(js, argv[i], JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(str_v)) {
                JS_FreeValue(js, JS_GetException(js));
                str_v = JS_UNDEFINED;
                use_json = false;
            } else if (JS_IsUndefined(str_v)) {
                use_json = false;
            }
        }
        size_t slen = 0u;
        const char* cs = JS_ToCStringLen(js, &slen, use_json ? str_v : argv[i]);
        if (cs) {
            append(cs, slen);
            JS_FreeCString(js, cs);
        } else if (JS_HasException(js)) {
            JS_FreeValue(js, JS_GetException(js));
        }
        if (use_json) JS_FreeValue(js, str_v);
    }
    dst[len] = '\0';
    InlaySurface* target = ext(s)->host_surface ? ext(s)->host_surface : s;
    s->console_fn(target, static_cast<InlayConsoleLevel>(magic),
                  EtchStr{dst, static_cast<uint32_t>(len), false}, s->console_user_data);
    SDL_free(heap_buf);
    return JS_UNDEFINED;
}

uint32_t find_rendered_node_rec(const InlaySurface* s, uint32_t idx, uint32_t parent_id,
                                uint32_t target_raw_id, uint32_t depth, uint32_t* out_node_idx) noexcept {
    if (idx == INLAY_UI_NONE || idx >= s->node_count || depth > 64u) return 0u;
    const auto& n = s->nodes[idx];
    const uint32_t eid = etch_id_index(parent_id, n.id);
    if (n.id == target_raw_id) {
        if (out_node_idx) *out_node_idx = idx;
        return eid;
    }
    for (uint32_t ch = n.first_child; ch != INLAY_UI_NONE; ch = s->nodes[ch].next_sibling) {
        const uint32_t found = find_rendered_node_rec(s, ch, eid, target_raw_id, depth + 1u, out_node_idx);
        if (found != 0u) return found;
    }
    return 0u;
}

uint32_t resolve_js_element_id(const InlaySurface* s, JSContext* js, JSValueConst val,
                               uint32_t* out_node_idx = nullptr) noexcept {
    if (out_node_idx) *out_node_idx = INLAY_UI_NONE;
    if (!s || JS_IsUndefined(val) || JS_IsNull(val)) return 0u;
    if (JS_IsNumber(val)) {
        uint32_t u = 0u;
        return JS_ToUint32(js, &u, val) == 0 ? u : 0u;
    }
    size_t n = 0u;
    const char* chars = JS_ToCStringLen(js, &n, val);
    if (!chars || n == 0u) {
        if (chars) JS_FreeCString(js, chars);
        return 0u;
    }
    const uint32_t h = inlay_fnv1a_32(chars, n, 2166136261u);
    const uint32_t raw_id = h ? h : 1u;
    JS_FreeCString(js, chars);
    if (s->node_count > 0u && ext(s)->last_view_id != 0u) {
        const uint32_t found = find_rendered_node_rec(s, 0u, ext(s)->last_view_id, raw_id, 0u, out_node_idx);
        if (found != 0u) return found;
    }
    return raw_id;
}

JSValue js_focus_element(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NewBool(js, false);
    uint32_t nidx = INLAY_UI_NONE;
    uint32_t eid = resolve_js_element_id(s, js, argv[0], &nidx);
    if (!eid) return JS_NewBool(js, false);
    if (nidx != INLAY_UI_NONE && nidx < s->node_count && s->nodes[nidx].kind == INLAY_UI_TERMINAL) {
        eid = etch_id_index(eid, 0x7E4Au);
    }
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    etch_request_focus(eid);
    etch_set_current_context(prev);
    etch_request_redraw(s->etch);
    return JS_NewBool(js, true);
}

JSValue js_blur_element(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch) return JS_UNDEFINED;
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    etch_request_focus(0u);
    etch_set_current_context(prev);
    etch_request_redraw(s->etch);
    return JS_UNDEFINED;
}

JSValue js_scroll_to(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 3) return JS_NewBool(js, false);
    uint32_t nidx = INLAY_UI_NONE;
    const uint32_t eid = resolve_js_element_id(s, js, argv[0], &nidx);
    double x = 0.0, y = 0.0;
    if (!eid || JS_ToFloat64(js, &x, argv[1]) < 0 || JS_ToFloat64(js, &y, argv[2]) < 0 ||
        !std::isfinite(x) || !std::isfinite(y)) {
        return JS_NewBool(js, false);
    }
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    if (nidx != INLAY_UI_NONE && nidx < s->node_count && s->nodes[nidx].kind == INLAY_UI_VIRTUAL_LIST) {
        etch_set_virtual_list_scroll_offset(eid, y < 0.0 ? 0.0 : y, true);
    } else if (nidx != INLAY_UI_NONE && nidx < s->node_count && s->nodes[nidx].kind == INLAY_UI_CODE_EDITOR) {
        etch_code_editor_set_scroll_offset(eid, {static_cast<float>(x), static_cast<float>(y)}, true);
    } else {
        etch_set_scroll_offset(eid, {static_cast<float>(x), static_cast<float>(y)}, true);
    }
    etch_set_current_context(prev);
    etch_request_redraw(s->etch);
    return JS_NewBool(js, true);
}

JSValue js_scroll_to_bottom(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NewBool(js, false);
    uint32_t nidx = INLAY_UI_NONE;
    const uint32_t eid = resolve_js_element_id(s, js, argv[0], &nidx);
    if (!eid) return JS_NewBool(js, false);
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    if (nidx != INLAY_UI_NONE && nidx < s->node_count && s->nodes[nidx].kind == INLAY_UI_VIRTUAL_LIST) {
        etch_virtual_list_scroll_to_bottom(eid, true);
    } else if (nidx != INLAY_UI_NONE && nidx < s->node_count && s->nodes[nidx].kind == INLAY_UI_DOC) {
        inlay_doc_view_scroll_to_bottom(etch_id_index(eid, 0xD0C0u), true);
    } else {
        const EtchVec2 mx = etch_scroll_max(eid);
        const EtchVec2 cur = etch_scroll_offset(eid);
        etch_set_scroll_offset(eid, {cur.x, mx.y}, true);
        etch_set_scroll_stuck_to_bottom(eid, true);
    }
    etch_set_current_context(prev);
    etch_request_redraw(s->etch);
    return JS_NewBool(js, true);
}

JSValue js_scroll_into_view(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 2) return JS_NewBool(js, false);
    const uint32_t sc_id = resolve_js_element_id(s, js, argv[0]);
    const uint32_t item_id = resolve_js_element_id(s, js, argv[1]);
    double margin = 8.0;
    if (argc > 2 && JS_ToFloat64(js, &margin, argv[2]) < 0) margin = 8.0;
    if (!sc_id || !item_id || !std::isfinite(margin)) return JS_NewBool(js, false);
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    etch_scroll_into_view(sc_id, item_id, static_cast<float>(margin));
    etch_set_current_context(prev);
    etch_request_redraw(s->etch);
    return JS_NewBool(js, true);
}

JSValue js_measure_element(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NULL;
    const uint32_t eid = resolve_js_element_id(s, js, argv[0]);
    if (!eid) return JS_NULL;
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    EtchElementGeometry geom = {};
    const bool ok = etch_element_geometry(eid, &geom);
    const EtchRect b = ok ? geom.bounds : etch_element_bounds(eid);
    etch_set_current_context(prev);
    if (!ok && b.w <= 0.f && b.h <= 0.f) return JS_NULL;
    const JSValue obj = JS_NewObject(js);
    if (JS_IsException(obj)) return obj;
    JS_SetPropertyStr(js, obj, "x", JS_NewFloat64(js, double(b.x)));
    JS_SetPropertyStr(js, obj, "y", JS_NewFloat64(js, double(b.y)));
    JS_SetPropertyStr(js, obj, "w", JS_NewFloat64(js, double(b.w)));
    JS_SetPropertyStr(js, obj, "h", JS_NewFloat64(js, double(b.h)));
    JS_SetPropertyStr(js, obj, "visible", JS_NewBool(js, ok && geom.visible_bounds.w > 0.f && geom.visible_bounds.h > 0.f));
    JS_SetPropertyStr(js, obj, "visibleX", JS_NewFloat64(js, double(geom.visible_bounds.x)));
    JS_SetPropertyStr(js, obj, "visibleY", JS_NewFloat64(js, double(geom.visible_bounds.y)));
    JS_SetPropertyStr(js, obj, "visibleW", JS_NewFloat64(js, double(geom.visible_bounds.w)));
    JS_SetPropertyStr(js, obj, "visibleH", JS_NewFloat64(js, double(geom.visible_bounds.h)));
    JS_SetPropertyStr(js, obj, "opacity", JS_NewFloat64(js, ok ? double(geom.opacity) : 1.0));
    return obj;
}

JSValue js_open_url(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewBool(js, false);
    size_t n = 0u;
    const char* u = JS_ToCStringLen(js, &n, argv[0]);
    if (!u) return JS_EXCEPTION;
    const bool ok = etch_open_url({u, static_cast<uint32_t>(n), false});
    JS_FreeCString(js, u);
    return JS_NewBool(js, ok);
}

JSValue js_notify_desktop(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NewBool(js, false);
    size_t tlen = 0u, blen = 0u;
    const char* title = JS_ToCStringLen(js, &tlen, argv[0]);
    const char* body = (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        ? JS_ToCStringLen(js, &blen, argv[1]) : nullptr;
    EtchDesktopNotificationDesc d = etch_desktop_notification_desc_default();
    d.title = {title ? title : "", static_cast<uint32_t>(tlen), false};
    d.body = {body ? body : "", static_cast<uint32_t>(blen), false};
    if (argc > 2 && JS_IsObject(argv[2]) && !JS_IsNull(argv[2])) {
        d.silent = inlay_ui_bool_prop(s, argv[2], "silent");
        uint32_t tag = 0u;
        if (inlay_ui_opt_uint_prop(s, argv[2], "tag", UINT32_MAX, &tag)) d.tag = tag;
    }
    const bool ok = etch_notify_desktop(s->etch, &d);
    if (title) JS_FreeCString(js, title);
    if (body) JS_FreeCString(js, body);
    return JS_NewBool(js, ok);
}

JSValue start_js_file_dialog(JSContext* js, int argc, JSValueConst* argv, uint8_t kind) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    auto* stg = ext(s);
    JSValue settle[2] = {JS_UNDEFINED, JS_UNDEFINED};
    const JSValue promise = JS_NewPromiseCapability(js, settle);
    if (JS_IsException(promise)) return promise;

    auto resolve_failed = [&]() -> JSValue {
        JSValue res = JS_NewObject(js);
        JS_SetPropertyStr(js, res, "cancelled", JS_NewBool(js, true));
        JS_SetPropertyStr(js, res, "error", JS_NewBool(js, true));
        JS_SetPropertyStr(js, res, "paths", JS_NewArray(js));
        const JSValue call = JS_Call(js, settle[0], JS_UNDEFINED, 1, &res);
        JS_FreeValue(js, call);
        JS_FreeValue(js, res);
        JS_FreeValue(js, settle[0]);
        JS_FreeValue(js, settle[1]);
        return promise;
    };

    PendingDialogSlot* slot = nullptr;
    for (auto& d : stg->pending_dialogs) {
        if (!d.active) { slot = &d; break; }
    }
    if (!slot || !s || !s->etch) return resolve_failed();

    const char* def_loc = nullptr;
    bool allow_many = false;
    EtchFileDialogFilter filters[8] = {};
    const char* filter_strs[16] = {};
    uint32_t filter_count = 0u;
    if (argc > 0 && JS_IsObject(argv[0]) && !JS_IsNull(argv[0])) {
        const JSValue dv = JS_GetPropertyStr(js, argv[0], "defaultLocation");
        if (JS_IsString(dv)) def_loc = JS_ToCString(js, dv);
        JS_FreeValue(js, dv);
        allow_many = inlay_ui_bool_prop(s, argv[0], "multiple");
        const JSValue fv = JS_GetPropertyStr(js, argv[0], "filters");
        if (JS_IsArray(fv)) {
            const JSValue lv = JS_GetPropertyStr(js, fv, "length");
            uint32_t n = 0u;
            (void)JS_ToUint32(js, &n, lv);
            JS_FreeValue(js, lv);
            if (n > 8u) n = 8u;
            for (uint32_t i = 0u; i < n; ++i) {
                const JSValue el = JS_GetPropertyUint32(js, fv, i);
                if (JS_IsObject(el) && !JS_IsNull(el)) {
                    const JSValue nv = JS_GetPropertyStr(js, el, "name");
                    const JSValue pv = JS_GetPropertyStr(js, el, "pattern");
                    const char* ns = JS_IsString(nv) ? JS_ToCString(js, nv) : nullptr;
                    const char* ps = JS_IsString(pv) ? JS_ToCString(js, pv) : nullptr;
                    if (ns && ps) {
                        filter_strs[filter_count * 2u] = ns;
                        filter_strs[filter_count * 2u + 1u] = ps;
                        filters[filter_count++] = {ns, ps};
                    } else {
                        if (ns) JS_FreeCString(js, ns);
                        if (ps) JS_FreeCString(js, ps);
                    }
                    JS_FreeValue(js, nv);
                    JS_FreeValue(js, pv);
                }
                JS_FreeValue(js, el);
            }
        }
        JS_FreeValue(js, fv);
    }

    const uint32_t tag = ++stg->next_dialog_tag;
    bool started = false;
    if (kind == 0u) {
        started = etch_open_file_dialog(s->etch, nullptr, filter_count ? filters : nullptr, filter_count, def_loc, tag, allow_many);
    } else if (kind == 1u) {
        started = etch_open_folder_dialog(s->etch, nullptr, def_loc, tag, allow_many);
    } else {
        started = etch_open_save_file_dialog(s->etch, nullptr, filter_count ? filters : nullptr, filter_count, def_loc, tag);
    }
    if (def_loc) JS_FreeCString(js, def_loc);
    for (uint32_t i = 0u; i < filter_count * 2u; ++i) {
        if (filter_strs[i]) JS_FreeCString(js, filter_strs[i]);
    }
    if (!started) return resolve_failed();
    slot->tag = tag;
    slot->settle[0] = settle[0];
    slot->settle[1] = settle[1];
    slot->active = true;
    return promise;
}

JSValue js_open_file_dialog(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    return start_js_file_dialog(js, argc, argv, 0u);
}
JSValue js_open_folder_dialog(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    return start_js_file_dialog(js, argc, argv, 1u);
}
JSValue js_save_file_dialog(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    return start_js_file_dialog(js, argc, argv, 2u);
}

JSValue js_clipboard_read_image(JSContext* js, JSValueConst, int, JSValueConst*) {
    EtchArena arena = etch_arena_create(2u * 1024u * 1024u);
    const EtchClipboardImageData img = etch_get_clipboard_image(&arena);
    if (!img.bytes || img.size_bytes == 0u) {
        etch_arena_destroy(&arena);
        return JS_NULL;
    }
    static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t enc_len = ((size_t(img.size_bytes) + 2u) / 3u) * 4u;
    auto* b64 = static_cast<char*>(SDL_malloc(enc_len + 1u));
    if (!b64) {
        etch_arena_destroy(&arena);
        return JS_ThrowOutOfMemory(js);
    }
    size_t oi = 0u;
    for (uint32_t i = 0u; i < img.size_bytes; i += 3u) {
        const uint32_t b0 = img.bytes[i];
        const uint32_t b1 = (i + 1u < img.size_bytes) ? img.bytes[i + 1u] : 0u;
        const uint32_t b2 = (i + 2u < img.size_bytes) ? img.bytes[i + 2u] : 0u;
        const uint32_t trip = (b0 << 16) | (b1 << 8) | b2;
        b64[oi++] = kB64[(trip >> 18) & 63u];
        b64[oi++] = kB64[(trip >> 12) & 63u];
        b64[oi++] = (i + 1u < img.size_bytes) ? kB64[(trip >> 6) & 63u] : '=';
        b64[oi++] = (i + 2u < img.size_bytes) ? kB64[trip & 63u] : '=';
    }
    b64[oi] = '\0';
    const JSValue obj = JS_NewObject(js);
    if (!JS_IsException(obj)) {
        JS_SetPropertyStr(js, obj, "mime", JS_NewStringLen(js, img.mime_type.chars ? img.mime_type.chars : "image/png",
                                                           img.mime_type.chars ? img.mime_type.length : 9u));
        JS_SetPropertyStr(js, obj, "sizeBytes", JS_NewUint32(js, img.size_bytes));
        JS_SetPropertyStr(js, obj, "base64", JS_NewStringLen(js, b64, oi));
    }
    SDL_free(b64);
    etch_arena_destroy(&arena);
    return obj;
}

bool parse_shortcut_token(const char* str, size_t len, uint16_t* out_mods, uint32_t* out_key) noexcept {
    *out_mods = 0u;
    *out_key = 0u;
    if (!str || len == 0u) return false;
    size_t p = 0u;
    while (p < len) {
        size_t s = p;
        while (p < len && str[p] != '+') ++p;
        size_t tlen = p - s;
        if (p < len && str[p] == '+') ++p;
        while (tlen > 0u && str[s] == ' ') { ++s; --tlen; }
        while (tlen > 0u && str[s + tlen - 1u] == ' ') --tlen;
        if (tlen == 0u) continue;
        const char* tok = str + s;
        auto eq_ci = [&](const char* lit) noexcept -> bool {
            const size_t ln = std::strlen(lit);
            if (tlen != ln) return false;
            for (size_t i = 0u; i < ln; ++i) {
                char a = tok[i]; if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                char b = lit[i]; if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
                if (a != b) return false;
            }
            return true;
        };
        if (eq_ci("ctrl") || eq_ci("control")) *out_mods |= ETCH_MOD_CTRL;
        else if (eq_ci("shift")) *out_mods |= ETCH_MOD_SHIFT;
        else if (eq_ci("alt") || eq_ci("option")) *out_mods |= ETCH_MOD_ALT;
        else if (eq_ci("cmd") || eq_ci("meta") || eq_ci("super")) *out_mods |= ETCH_MOD_SUPER;
        else if (eq_ci("primary") || eq_ci("mod")) *out_mods |= ETCH_MOD_PRIMARY;
        else if (eq_ci("escape") || eq_ci("esc")) *out_key = SDLK_ESCAPE;
        else if (eq_ci("enter") || eq_ci("return")) *out_key = SDLK_RETURN;
        else if (eq_ci("tab")) *out_key = SDLK_TAB;
        else if (eq_ci("space")) *out_key = SDLK_SPACE;
        else if (eq_ci("backspace")) *out_key = SDLK_BACKSPACE;
        else if (eq_ci("delete") || eq_ci("del")) *out_key = SDLK_DELETE;
        else if (eq_ci("arrowup") || eq_ci("up")) *out_key = SDLK_UP;
        else if (eq_ci("arrowdown") || eq_ci("down")) *out_key = SDLK_DOWN;
        else if (eq_ci("arrowleft") || eq_ci("left")) *out_key = SDLK_LEFT;
        else if (eq_ci("arrowright") || eq_ci("right")) *out_key = SDLK_RIGHT;
        else if (eq_ci("home")) *out_key = SDLK_HOME;
        else if (eq_ci("end")) *out_key = SDLK_END;
        else if (eq_ci("pageup")) *out_key = SDLK_PAGEUP;
        else if (eq_ci("pagedown")) *out_key = SDLK_PAGEDOWN;
        else if (tlen == 2u && (tok[0] == 'F' || tok[0] == 'f') && tok[1] >= '1' && tok[1] <= '9') {
            *out_key = SDLK_F1 + uint32_t(tok[1] - '1');
        } else if (tlen == 3u && (tok[0] == 'F' || tok[0] == 'f') && tok[1] == '1' && tok[2] >= '0' && tok[2] <= '2') {
            *out_key = SDLK_F10 + uint32_t(tok[2] - '0');
        } else if (tlen == 1u) {
            char c = tok[0];
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            *out_key = static_cast<uint32_t>(static_cast<uint8_t>(c));
        }
    }
    return *out_key != 0u;
}

uint32_t resolve_command_id(SurfaceStorage* stg, JSContext* js, JSValueConst val, bool record_name) noexcept {
    if (JS_IsNumber(val)) {
        uint32_t u = 0u;
        return JS_ToUint32(js, &u, val) == 0 ? u : 0u;
    }
    size_t n = 0u;
    const char* str = JS_ToCStringLen(js, &n, val);
    if (!str || n == 0u) {
        if (str) JS_FreeCString(js, str);
        return 0u;
    }
    const uint32_t h = inlay_fnv1a_32(str, n, 2166136261u);
    const uint32_t cid = h ? h : 1u;
    if (record_name && stg && n < sizeof(CommandNameEntry::name)) {
        bool found = false;
        for (uint32_t i = 0u; i < stg->command_name_count; ++i) {
            if (stg->command_names[i].id == cid) {
                std::memcpy(stg->command_names[i].name, str, n);
                stg->command_names[i].name[n] = '\0';
                found = true;
                break;
            }
        }
        if (!found && inlay_grow_pod(stg->command_names, stg->command_name_cap, stg->command_name_count + 1u, 16u, 512u)) {
            auto& ent = stg->command_names[stg->command_name_count++];
            ent.id = cid;
            std::memcpy(ent.name, str, n);
            ent.name[n] = '\0';
        }
    }
    JS_FreeCString(js, str);
    return cid;
}

JSValue js_register_command(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1 || !JS_IsObject(argv[0]) || JS_IsNull(argv[0])) {
        return JS_NewBool(js, false);
    }
    const JSValue id_v = JS_GetPropertyStr(js, argv[0], "id");
    const uint32_t cid = resolve_command_id(ext(s), js, id_v, true);
    JS_FreeValue(js, id_v);
    if (!cid) return JS_NewBool(js, false);
    EtchCommandDesc desc = etch_command_desc_default();
    desc.id = cid;
    size_t nlen = 0u, tlen = 0u, clen = 0u;
    const JSValue nv = JS_GetPropertyStr(js, argv[0], "name");
    const JSValue tv = JS_GetPropertyStr(js, argv[0], "title");
    const JSValue cv = JS_GetPropertyStr(js, argv[0], "category");
    const char* ns = JS_IsString(nv) ? JS_ToCStringLen(js, &nlen, nv) : nullptr;
    const char* ts = JS_IsString(tv) ? JS_ToCStringLen(js, &tlen, tv) : nullptr;
    const char* cs = JS_IsString(cv) ? JS_ToCStringLen(js, &clen, cv) : nullptr;
    if (ns) desc.name = {ns, static_cast<uint32_t>(nlen), false};
    if (ts) desc.title = {ts, static_cast<uint32_t>(tlen), false};
    if (cs) desc.category = {cs, static_cast<uint32_t>(clen), false};
    if (!desc.name.length && desc.title.length) desc.name = desc.title;
    bool en = true;
    if (inlay_ui_opt_bool_prop(s, argv[0], "enabled", &en)) desc.enabled = en;
    desc.allow_when_typing = inlay_ui_bool_prop(s, argv[0], "allowWhenTyping");
    const JSValue scv = JS_GetPropertyStr(js, argv[0], "scope");
    if (!JS_IsUndefined(scv) && !JS_IsNull(scv)) desc.scope_id = resolve_command_id(nullptr, js, scv, false);
    JS_FreeValue(js, scv);
    peek_str_prop(s, argv[0], "shortcut", [&](const char* str, size_t len) noexcept {
        return parse_shortcut_token(str, len, &desc.shortcut_mods, &desc.shortcut_keycode);
    });
    peek_str_prop(s, argv[0], "chord", [&](const char* str, size_t len) noexcept {
        return parse_shortcut_token(str, len, &desc.chord_mods, &desc.chord_keycode);
    });
    const bool ok = etch_register_command(s->etch, &desc);
    if (ns) JS_FreeCString(js, ns);
    if (ts) JS_FreeCString(js, ts);
    if (cs) JS_FreeCString(js, cs);
    JS_FreeValue(js, nv);
    JS_FreeValue(js, tv);
    JS_FreeValue(js, cv);
    return JS_NewBool(js, ok);
}

JSValue js_unregister_command(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NewBool(js, false);
    const uint32_t cid = resolve_command_id(nullptr, js, argv[0], false);
    return JS_NewBool(js, cid ? etch_unregister_command(s->etch, cid) : false);
}

JSValue js_dispatch_command(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NewBool(js, false);
    const uint32_t cid = resolve_command_id(nullptr, js, argv[0], false);
    const bool ok = cid ? etch_dispatch_command(s->etch, cid) : false;
    if (ok) {
        auto* stg = ext(s);
        if (stg->pending_cmd_count < 8u) stg->pending_cmds[stg->pending_cmd_count++] = cid;
        etch_request_redraw(s->etch);
    }
    return JS_NewBool(js, ok);
}

JSValue js_push_command_scope(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_UNDEFINED;
    const uint32_t sid = resolve_command_id(nullptr, js, argv[0], false);
    if (!sid) return JS_UNDEFINED;
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    etch_push_command_scope(sid);
    etch_set_current_context(prev);
    return JS_UNDEFINED;
}

JSValue js_pop_command_scope(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch) return JS_UNDEFINED;
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    etch_pop_command_scope();
    etch_set_current_context(prev);
    return JS_UNDEFINED;
}

JSValue js_read_stream(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    size_t nlen = 0u;
    const char* name = JS_ToCStringLen(js, &nlen, argv[0]);
    if (!name) return JS_EXCEPTION;
    StreamBinding* found = nullptr;
    for (uint32_t i = 0u; i < s->stream_count; ++i) {
        if (inlay_ui_str_eq(name, nlen, s->streams[i].name)) {
            found = &s->streams[i];
            break;
        }
    }
    JS_FreeCString(js, name);
    if (!found || !found->stream) return JS_NULL;
    bool has_since = false;
    uint64_t since = 0u;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        double dv = 0.0;
        if (JS_ToFloat64(js, &dv, argv[1]) == 0 && std::isfinite(dv) && dv >= 0.0) {
            since = static_cast<uint64_t>(dv);
            has_since = true;
        }
    }
    const uint32_t need = has_since
        ? inlay_stream_export_since_json(found->stream, since, nullptr, 0u)
        : inlay_stream_export_json(found->stream, nullptr, 0u);
    auto* buf = static_cast<char*>(SDL_malloc(size_t(need) + 1u));
    if (!buf) return JS_ThrowOutOfMemory(js);
    if (has_since) inlay_stream_export_since_json(found->stream, since, buf, need + 1u);
    else inlay_stream_export_json(found->stream, buf, need + 1u);
    buf[need] = '\0';
    found->last_seen_cursor = inlay_stream_cursor(found->stream);
    const JSValue parsed = JS_ParseJSON(js, buf, need, "<stream>");
    SDL_free(buf);
    return parsed;
}

JSValue js_save_dock_layout(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 1) return JS_NewString(js, "");
    const uint32_t eid = resolve_js_element_id(s, js, argv[0]);
    if (!eid) return JS_NewString(js, "");
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    char stack_buf[8192];
    const uint32_t need = etch_dock_save_layout_json(eid, stack_buf, sizeof(stack_buf));
    JSValue ret = JS_UNDEFINED;
    if (need < sizeof(stack_buf)) {
        ret = JS_NewStringLen(js, stack_buf, need);
    } else {
        auto* heap = static_cast<char*>(SDL_malloc(size_t(need) + 1u));
        if (heap) {
            etch_dock_save_layout_json(eid, heap, need + 1u);
            ret = JS_NewStringLen(js, heap, need);
            SDL_free(heap);
        } else {
            ret = JS_ThrowOutOfMemory(js);
        }
    }
    etch_set_current_context(prev);
    return ret;
}

JSValue js_load_dock_layout(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 2) return JS_NewBool(js, false);
    const uint32_t eid = resolve_js_element_id(s, js, argv[0]);
    size_t len = 0u;
    const char* json = JS_ToCStringLen(js, &len, argv[1]);
    if (!eid || !json) {
        if (json) JS_FreeCString(js, json);
        return JS_NewBool(js, false);
    }
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    const bool ok = etch_dock_load_layout_json(eid, {json, static_cast<uint32_t>(len), false});
    etch_set_current_context(prev);
    JS_FreeCString(js, json);
    if (ok) etch_request_redraw(s->etch);
    return JS_NewBool(js, ok);
}

JSValue js_detach_dock_pane(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s || !s->etch || argc < 2) return JS_NewBool(js, false);
    const uint32_t dock_id = resolve_js_element_id(s, js, argv[0]);
    uint32_t pane_id = 0u;
    if (JS_IsNumber(argv[1])) {
        (void)JS_ToUint32(js, &pane_id, argv[1]);
    } else if (JS_IsString(argv[1])) {
        size_t n = 0u;
        const char* ps = JS_ToCStringLen(js, &n, argv[1]);
        if (ps && n > 0u) {
            const uint32_t h = inlay_fnv1a_32(ps, n, 2166136261u);
            pane_id = h ? h : 1u;
        }
        if (ps) JS_FreeCString(js, ps);
    }
    if (!dock_id || !pane_id) return JS_NewBool(js, false);
    EtchContext* prev = etch_current_context();
    etch_set_current_context(s->etch);
    const bool ok = etch_dock_detach_pane(dock_id, pane_id);
    etch_set_current_context(prev);
    if (ok) etch_request_redraw(s->etch);
    return JS_NewBool(js, ok);
}

JSValue js_encode_terminal_key(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(js, "");
    uint32_t keycode = 0u, mods = 0u;
    if (JS_ToUint32(js, &keycode, argv[0]) < 0) return JS_EXCEPTION;
    if (argc > 1 && JS_ToUint32(js, &mods, argv[1]) < 0) return JS_EXCEPTION;
    const bool app_cursor = argc > 2 && JS_ToBool(js, argv[2]) > 0;
    char buf[32] = {};
    const uint32_t len = etch_terminal_encode_key(keycode, static_cast<uint16_t>(mods), app_cursor, buf, sizeof(buf));
    return JS_NewStringLen(js, buf, len < sizeof(buf) ? len : (sizeof(buf) - 1u));
}

JSValue js_encode_terminal_mouse_sgr(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_NewString(js, "");
    uint32_t btn = 0u, mods = 0u;
    int32_t col = 0, row = 0;
    if (JS_ToUint32(js, &btn, argv[0]) < 0 || JS_ToInt32(js, &col, argv[1]) < 0 ||
        JS_ToInt32(js, &row, argv[2]) < 0 || JS_ToUint32(js, &mods, argv[3]) < 0) {
        return JS_EXCEPTION;
    }
    const bool pressed = JS_ToBool(js, argv[4]) > 0;
    const bool motion = JS_ToBool(js, argv[5]) > 0;
    char buf[48] = {};
    const uint32_t len = etch_terminal_encode_mouse_sgr(static_cast<uint8_t>(btn), col, row,
                                                        static_cast<uint16_t>(mods), pressed, motion,
                                                        buf, sizeof(buf));
    return JS_NewStringLen(js, buf, len < sizeof(buf) ? len : (sizeof(buf) - 1u));
}

JSValue js_fuzzy_match(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(js, "fuzzyMatch(pattern, candidate) requires 2 arguments");
    size_t plen = 0u, clen = 0u;
    const char* pat = JS_ToCStringLen(js, &plen, argv[0]);
    const char* cand = JS_ToCStringLen(js, &clen, argv[1]);
    if (!pat || !cand) {
        if (pat) JS_FreeCString(js, pat);
        if (cand) JS_FreeCString(js, cand);
        return JS_EXCEPTION;
    }
    uint32_t pos[64] = {};
    const EtchFuzzyMatchResult r = etch_fuzzy_match(
        {pat, static_cast<uint32_t>(plen), false},
        {cand, static_cast<uint32_t>(clen), false},
        pos, 64u);
    JS_FreeCString(js, pat);
    JS_FreeCString(js, cand);
    const JSValue obj = JS_NewObject(js);
    if (JS_IsException(obj)) return obj;
    JS_SetPropertyStr(js, obj, "matched", JS_NewBool(js, r.matched));
    JS_SetPropertyStr(js, obj, "score", JS_NewInt32(js, r.score));
    const JSValue arr = JS_NewArray(js);
    const uint32_t n = r.match_count > 64u ? 64u : r.match_count;
    for (uint32_t i = 0u; i < n; ++i) {
        JS_SetPropertyUint32(js, arr, i, JS_NewUint32(js, pos[i]));
    }
    JS_SetPropertyStr(js, obj, "positions", arr);
    return obj;
}

JSValue js_clipboard_write(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewBool(js, false);
    const char* txt = JS_ToCString(js, argv[0]);
    if (!txt) return JS_EXCEPTION;
    const bool ok = etch_set_clipboard_text(txt);
    JS_FreeCString(js, txt);
    return JS_NewBool(js, ok);
}

JSValue js_clipboard_read(JSContext* js, JSValueConst, int, JSValueConst*) {
    char scratch[16384];
    EtchArena arena = etch_arena_from_buffer(scratch, sizeof(scratch));
    const EtchStr s = etch_get_clipboard_text(&arena);
    return JS_NewStringLen(js, s.chars ? s.chars : "", s.length);
}

JSValue js_push_focus_restore(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    return JS_NewBool(js, s && s->etch ? etch_push_focus_restore_point(s->etch) : false);
}

JSValue js_pop_focus_restore(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    return JS_NewUint32(js, s && s->etch ? etch_pop_focus_restore_point(s->etch) : 0u);
}

JSValue js_set_ui_zoom(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    double z = 1.0;
    if (argc < 1 || JS_ToFloat64(js, &z, argv[0]) < 0 || !std::isfinite(z)) {
        return JS_ThrowTypeError(js, "setUiZoom expects a finite number");
    }
    if (s && s->etch) etch_set_ui_zoom(s->etch, static_cast<float>(z));
    return JS_NewFloat64(js, s && s->etch ? double(etch_ui_zoom(s->etch)) : 1.0);
}

JSValue js_get_ui_zoom(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    return JS_NewFloat64(js, s && s->etch ? double(etch_ui_zoom(s->etch)) : 1.0);
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
        ext(s)->boundary_root_count = 0u;
        ext(s)->cur_eb_key = {};
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

TextSlice find_event_eb_key(const InlaySurface* s, const WidgetEvent* ev) noexcept {
    const auto* e = ext(s);
    for (uint32_t i = 0u; i < e->event_eb_tag_count; ++i) {
        if (e->event_eb_tags[i].ev == ev) return e->event_eb_tags[i].eb_key;
    }
    return {};
}

bool try_trip_error_boundary(InlaySurface* s, JSValueConst global, TextSlice eb_key) {
    if (s->timed_out || eb_key.len == 0u || !s->text ||
        eb_key.off > s->text_used || eb_key.len > s->text_used - eb_key.off) {
        return false;
    }
    const JSValue exc = JS_GetException(s->js);
    if (JS_IsUncatchableError(exc)) {
        JS_Throw(s->js, exc);
        return false;
    }
    const JSValue trip_fn = JS_GetPropertyStr(s->js, global, "__inlay_trip_eb");
    bool tripped = false;
    if (JS_IsFunction(s->js, trip_fn)) {
        JSValue args[2] = {
            JS_NewStringLen(s->js, s->text + eb_key.off, eb_key.len),
            JS_DupValue(s->js, exc)
        };
        const JSValue res = JS_Call(s->js, trip_fn, global, 2, args);
        if (!JS_IsException(res)) {
            tripped = JS_ToBool(s->js, res) > 0;
        }
        JS_FreeValue(s->js, res);
        JS_FreeValue(s->js, args[0]);
        JS_FreeValue(s->js, args[1]);
    }
    JS_FreeValue(s->js, trip_fn);
    if (tripped && !JS_HasException(s->js)) {
        JS_FreeValue(s->js, exc);
        return true;
    }
    if (!JS_HasException(s->js)) {
        JS_Throw(s->js, exc);
    } else {
        JS_FreeValue(s->js, exc);
    }
    return false;
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
        if (JS_HasException(s->js)) {
            TextSlice eb_key = ext(s)->handler_eb_keys ? ext(s)->handler_eb_keys[ev.handler] : TextSlice{};
            if (eb_key.len == 0u) eb_key = find_event_eb_key(s, &ev);
            if (eb_key.len > 0u && try_trip_error_boundary(s, global, eb_key)) {
                return true;
            }
            return false;
        }
        return true;
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
    if (JS_HasException(s->js)) {
        const TextSlice eb_key = find_event_eb_key(s, &ev);
        if (eb_key.len > 0u && try_trip_error_boundary(s, global, eb_key)) {
            return true;
        }
        return false;
    }
    return true;
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
            timer.due = 0.0;
            timer.interval_ms = 0.0;
            timer.id = 0u;
            timer.is_raf = false;
        }
        auto* storage = ext(s);
        for (auto& pd : storage->pending_dialogs) {
            if (pd.active) {
                JS_FreeValue(s->js, pd.settle[0]);
                JS_FreeValue(s->js, pd.settle[1]);
                pd = {};
            }
        }
        if (storage->atom_memo_key != JS_ATOM_NULL) {
            JS_FreeAtom(s->js, storage->atom_memo_key);
            storage->atom_memo_key = JS_ATOM_NULL;
        }
        if (storage->atom_memo_rev != JS_ATOM_NULL) {
            JS_FreeAtom(s->js, storage->atom_memo_rev);
            storage->atom_memo_rev = JS_ATOM_NULL;
        }
        if (storage->atom_eb_key != JS_ATOM_NULL) {
            JS_FreeAtom(s->js, storage->atom_eb_key);
            storage->atom_eb_key = JS_ATOM_NULL;
        }
        JS_FreeContext(s->js);
        s->js = nullptr;
    }
}

void free_spec_pools(InlaySurface* s) noexcept {
    inlay_ui_free_spec_pools(s);
}

void move_spec_pools(InlaySurface* dst, InlaySurface* src) noexcept {
    inlay_ui_move_spec_pools(dst, src);
}

static const RegisteredModule* find_registered_module(const SurfaceStorage* stg, const char* name, size_t len) noexcept {
    if (!stg || !name || len == 0u || len >= sizeof(RegisteredModule::name)) return nullptr;
    for (uint32_t i = 0u; i < stg->module_count; ++i) {
        if (inlay_ui_str_eq(name, len, stg->modules[i].name)) return &stg->modules[i];
    }
    return nullptr;
}

static char* module_normalize_cb(JSContext* ctx, const char* base_name, const char* name, void* opaque) {
    auto* s = static_cast<InlaySurface*>(opaque);
    const auto* stg = s ? ext(s) : nullptr;
    if (!stg || !name || !name[0]) {
        JS_ThrowReferenceError(ctx, "Invalid module specifier");
        return nullptr;
    }
    const size_t name_len = std::strlen(name);
    if (name_len >= sizeof(RegisteredModule::name)) {
        JS_ThrowReferenceError(ctx, "Module specifier too long");
        return nullptr;
    }

    char resolved[128] = {};
    size_t res_len = 0u;
    const bool is_rel = (name[0] == '.' && (name[1] == '/' || (name[1] == '.' && name[2] == '/')));
    if (is_rel) {
        const char* segs[32];
        size_t seg_lens[32];
        uint32_t depth = 0u;
        if (base_name && base_name[0] && base_name[0] != '<') {
            const char* last_slash = std::strrchr(base_name, '/');
            if (last_slash) {
                const char* p = base_name;
                while (p < last_slash) {
                    const char* sl = p;
                    while (sl < last_slash && *sl != '/') ++sl;
                    const size_t slen = size_t(sl - p);
                    if (slen > 0u && !(slen == 1u && p[0] == '.')) {
                        if (depth < 32u) {
                            segs[depth] = p;
                            seg_lens[depth] = slen;
                            ++depth;
                        }
                    }
                    p = (sl < last_slash) ? (sl + 1) : last_slash;
                }
            }
        }
        const char* p = name;
        while (*p) {
            const char* sl = p;
            while (*sl && *sl != '/') ++sl;
            const size_t slen = size_t(sl - p);
            if (slen == 2u && p[0] == '.' && p[1] == '.') {
                if (depth == 0u) {
                    JS_ThrowReferenceError(ctx, "Module specifier escapes sandbox root: %s", name);
                    return nullptr;
                }
                --depth;
            } else if (slen > 0u && !(slen == 1u && p[0] == '.')) {
                if (depth >= 32u) {
                    JS_ThrowReferenceError(ctx, "Module path depth exceeded: %s", name);
                    return nullptr;
                }
                segs[depth] = p;
                seg_lens[depth] = slen;
                ++depth;
            }
            p = *sl ? (sl + 1) : sl;
        }
        for (uint32_t i = 0u; i < depth; ++i) {
            if (i > 0u) {
                if (res_len + 1u >= sizeof(resolved)) {
                    JS_ThrowReferenceError(ctx, "Resolved module specifier too long");
                    return nullptr;
                }
                resolved[res_len++] = '/';
            }
            if (res_len + seg_lens[i] >= sizeof(resolved)) {
                JS_ThrowReferenceError(ctx, "Resolved module specifier too long");
                return nullptr;
            }
            std::memcpy(resolved + res_len, segs[i], seg_lens[i]);
            res_len += seg_lens[i];
        }
        resolved[res_len] = '\0';
    } else {
        std::memcpy(resolved, name, name_len + 1u);
        res_len = name_len;
    }

    if (find_registered_module(stg, resolved, res_len)) {
        return js_strdup(ctx, resolved);
    }
    static constexpr const char* kExts[] = {".tsx", ".ts", ".jsx", ".js"};
    for (const char* ex : kExts) {
        const size_t elen = std::strlen(ex);
        if (res_len + elen < sizeof(resolved)) {
            char cand[128] = {};
            std::memcpy(cand, resolved, res_len);
            std::memcpy(cand + res_len, ex, elen + 1u);
            if (find_registered_module(stg, cand, res_len + elen)) {
                return js_strdup(ctx, cand);
            }
        }
    }
    return js_strdup(ctx, resolved);
}

static JSModuleDef* module_loader_cb(JSContext* ctx, const char* module_name, void* opaque) {
    auto* s = static_cast<InlaySurface*>(opaque);
    const auto* stg = s ? ext(s) : nullptr;
    if (!stg || !module_name) {
        JS_ThrowReferenceError(ctx, "Module not found");
        return nullptr;
    }
    const RegisteredModule* mod = find_registered_module(stg, module_name, std::strlen(module_name));
    if (!mod || !mod->source) {
        JS_ThrowReferenceError(ctx, "Module not registered: %s", module_name);
        return nullptr;
    }
    const char* src = mod->source;
    size_t src_len = mod->length;
    char* jsx_tmp = nullptr;
    size_t jsx_cap = 0u;
    if (!s->desc.disable_jsx && src_len > 0u) {
        char jsx_err[512] = {};
        size_t jsx_len = 0u;
        if (!inlay_jsx_transpile(src, src_len, module_name, &jsx_tmp, &jsx_cap, &jsx_len,
                                 jsx_err, sizeof(jsx_err))) {
            SDL_free(jsx_tmp);
            JS_ThrowSyntaxError(ctx, "%s", jsx_err[0] ? jsx_err : "JSXSyntaxError in module");
            return nullptr;
        }
        if (jsx_len > 0u && jsx_tmp) {
            src = jsx_tmp;
            src_len = jsx_len;
        }
    }
    const JSValue func_val = JS_Eval(ctx, src, src_len, module_name,
                                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    SDL_free(jsx_tmp);
    if (JS_IsException(func_val)) return nullptr;
    auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
    JS_FreeValue(ctx, func_val);
    return def;
}

static bool is_esm_ident_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static bool script_uses_esm(const char* s, size_t len) noexcept {
    if (!s || len < 6u) return false;
    int brace = 0, paren = 0, bracket = 0;
    bool stmt_start = true;
    size_t i = 0u;
    while (i < len) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '\n') {
            if (brace == 0 && paren == 0 && bracket == 0) stmt_start = true;
            ++i;
            continue;
        }
        if (c == '/' && i + 1u < len && s[i + 1u] == '/') {
            i += 2u;
            while (i < len && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1u < len && s[i + 1u] == '*') {
            i += 2u;
            while (i + 1u < len && !(s[i] == '*' && s[i + 1u] == '/')) ++i;
            i = (i + 1u < len) ? (i + 2u) : len;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            const char q = c;
            ++i;
            while (i < len) {
                if (s[i] == '\\' && i + 1u < len) { i += 2u; continue; }
                if (s[i] == q) { ++i; break; }
                ++i;
            }
            stmt_start = false;
            continue;
        }
        if (brace == 0 && paren == 0 && bracket == 0 && stmt_start) {
            if (i + 6u <= len && std::memcmp(s + i, "import", 6u) == 0 &&
                (i + 6u == len || !is_esm_ident_char(s[i + 6u]))) {
                size_t k = i + 6u;
                while (k < len && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r' || s[k] == '\n')) ++k;
                if (k < len && s[k] != '(' && s[k] != '.' && s[k] != ':' && s[k] != '=') return true;
            }
            if (i + 6u <= len && std::memcmp(s + i, "export", 6u) == 0 &&
                (i + 6u == len || !is_esm_ident_char(s[i + 6u]))) {
                size_t k = i + 6u;
                while (k < len && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r' || s[k] == '\n')) ++k;
                if (k < len && s[k] != ':' && s[k] != '=' && s[k] != '.') return true;
            }
        }
        if (is_esm_ident_char(c)) {
            while (i < len && is_esm_ident_char(s[i])) ++i;
            stmt_start = false;
            continue;
        }
        if (c == '{') { ++brace; stmt_start = false; }
        else if (c == '}') { if (brace > 0) --brace; stmt_start = (brace == 0 && paren == 0 && bracket == 0); }
        else if (c == '(') { ++paren; stmt_start = false; }
        else if (c == ')') { if (paren > 0) --paren; stmt_start = false; }
        else if (c == '[') { ++bracket; stmt_start = false; }
        else if (c == ']') { if (bracket > 0) --bracket; stmt_start = false; }
        else if (c == ';') { stmt_start = (brace == 0 && paren == 0 && bracket == 0); }
        else { stmt_start = false; }
        ++i;
    }
    return false;
}

static bool eval_entry_module(InlaySurface* s, const char* src, size_t len, const char* filename) {
    const JSValue mod_fn = JS_Eval(s->js, src, len, filename,
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(mod_fn)) return false;
    auto* mod = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod_fn));
    const JSValue eval_res = JS_EvalFunction(s->js, mod_fn);
    if (JS_IsException(eval_res)) return false;

    JSContext* pctx = nullptr;
    int job_rc = 0;
    while (!s->timed_out && (job_rc = JS_ExecutePendingJob(s->runtime, &pctx)) > 0) {}
    if (job_rc < 0 || s->timed_out) {
        JS_FreeValue(s->js, eval_res);
        return false;
    }

    const JSPromiseStateEnum pstate = JS_PromiseState(s->js, eval_res);
    if (pstate == JS_PROMISE_REJECTED) {
        const JSValue err = JS_PromiseResult(s->js, eval_res);
        JS_FreeValue(s->js, eval_res);
        JS_Throw(s->js, err);
        return false;
    }
    if (pstate == JS_PROMISE_PENDING) {
        JS_FreeValue(s->js, eval_res);
        JS_ThrowTypeError(s->js, "Top-level await is not supported in surface entry modules");
        return false;
    }
    JS_FreeValue(s->js, eval_res);

    const JSValue ns = JS_GetModuleNamespace(s->js, mod);
    if (JS_IsException(ns)) return false;
    const JSValue target = JS_GetGlobalObject(s->js);
    JSValue render_fn = JS_GetPropertyStr(s->js, ns, "render");
    if (!JS_IsFunction(s->js, render_fn)) {
        JS_FreeValue(s->js, render_fn);
        render_fn = JS_GetPropertyStr(s->js, ns, "default");
    }
    if (JS_IsFunction(s->js, render_fn)) {
        JS_SetPropertyStr(s->js, target, "render", render_fn);
    } else {
        JS_FreeValue(s->js, render_fn);
    }
    const JSValue ev_fn = JS_GetPropertyStr(s->js, ns, "onEvent");
    if (JS_IsFunction(s->js, ev_fn)) {
        JS_SetPropertyStr(s->js, target, "onEvent", ev_fn);
    } else {
        JS_FreeValue(s->js, ev_fn);
    }
    const JSValue cmd_fn = JS_GetPropertyStr(s->js, ns, "onCommand");
    if (JS_IsFunction(s->js, cmd_fn)) {
        JS_SetPropertyStr(s->js, target, "onCommand", cmd_fn);
    } else {
        JS_FreeValue(s->js, cmd_fn);
    }
    JS_FreeValue(s->js, target);
    JS_FreeValue(s->js, ns);
    return !JS_HasException(s->js);
}

// ============================================================================
// Round 4 Built-In Standard Web/JS Globals (performance, crypto, Base64, localStorage)
// ============================================================================

static JSValue js_perf_now(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    return JS_NewFloat64(js, s ? inlay_ui_now_ms(s) : 0.0);
}

static uint64_t splitmix64_next(uint64_t& x) noexcept {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static JSValue js_crypto_random_uuid(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    auto* owner = s ? ext(ext(s)->host_surface ? ext(s)->host_surface : s) : nullptr;
    uint64_t seed = SDL_GetPerformanceCounter() ^ (uint64_t(uintptr_t(s)) * 0x517CC1B727220A95ULL);
    if (owner) seed ^= (++owner->uuid_seq) * 0x9E3779B97F4A7C15ULL;
    uint8_t b[16];
    const uint64_t r0 = splitmix64_next(seed);
    const uint64_t r1 = splitmix64_next(seed);
    std::memcpy(b, &r0, 8u);
    std::memcpy(b + 8u, &r1, 8u);
    b[6] = static_cast<uint8_t>((b[6] & 0x0Fu) | 0x40u); // UUID v4
    b[8] = static_cast<uint8_t>((b[8] & 0x3Fu) | 0x80u); // RFC 4122 variant
    char out[40];
    std::snprintf(out, sizeof(out),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return JS_NewStringLen(js, out, 36u);
}

static JSValue js_btoa(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(js, "btoa requires 1 argument");
    size_t len = 0u;
    const char* raw = JS_ToCStringLen(js, &len, argv[0]);
    if (!raw) return JS_EXCEPTION;
    if (len > 1024u * 1024u) {
        JS_FreeCString(js, raw);
        return JS_ThrowRangeError(js, "btoa input too large");
    }
    // Decode UTF-8 input to Latin-1 code points (Web API btoa contract).
    uint8_t* bytes = static_cast<uint8_t*>(SDL_malloc(len ? len : 1u));
    if (!bytes) { JS_FreeCString(js, raw); return JS_ThrowOutOfMemory(js); }
    size_t blen = 0u;
    for (size_t i = 0u; i < len; ) {
        const uint8_t c0 = static_cast<uint8_t>(raw[i]);
        if (c0 < 0x80u) {
            bytes[blen++] = c0;
            ++i;
        } else if ((c0 & 0xE0u) == 0xC0u && i + 1u < len) {
            const uint32_t cp = ((uint32_t(c0 & 0x1Fu) << 6) | uint32_t(static_cast<uint8_t>(raw[i + 1u]) & 0x3Fu));
            if (cp > 0xFFu) {
                SDL_free(bytes); JS_FreeCString(js, raw);
                return JS_ThrowRangeError(js, "btoa: string contains character outside Latin-1 range");
            }
            bytes[blen++] = static_cast<uint8_t>(cp);
            i += 2u;
        } else {
            SDL_free(bytes); JS_FreeCString(js, raw);
            return JS_ThrowRangeError(js, "btoa: string contains character outside Latin-1 range");
        }
    }
    JS_FreeCString(js, raw);
    static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t out_len = ((blen + 2u) / 3u) * 4u;
    char* out = static_cast<char*>(SDL_malloc(out_len + 1u));
    if (!out) { SDL_free(bytes); return JS_ThrowOutOfMemory(js); }
    size_t o = 0u;
    for (size_t i = 0u; i < blen; i += 3u) {
        const uint32_t v0 = bytes[i];
        const uint32_t v1 = (i + 1u < blen) ? bytes[i + 1u] : 0u;
        const uint32_t v2 = (i + 2u < blen) ? bytes[i + 2u] : 0u;
        const uint32_t tri = (v0 << 16) | (v1 << 8) | v2;
        out[o++] = kB64[(tri >> 18) & 0x3Fu];
        out[o++] = kB64[(tri >> 12) & 0x3Fu];
        out[o++] = (i + 1u < blen) ? kB64[(tri >> 6) & 0x3Fu] : '=';
        out[o++] = (i + 2u < blen) ? kB64[tri & 0x3Fu] : '=';
    }
    out[o] = '\0';
    SDL_free(bytes);
    const JSValue res = JS_NewStringLen(js, out, o);
    SDL_free(out);
    return res;
}

static int b64_dec_val(uint8_t c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static JSValue js_atob(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(js, "atob requires 1 argument");
    size_t len = 0u;
    const char* raw = JS_ToCStringLen(js, &len, argv[0]);
    if (!raw) return JS_EXCEPTION;
    if (len > 2u * 1024u * 1024u) {
        JS_FreeCString(js, raw);
        return JS_ThrowRangeError(js, "atob input too large");
    }
    // Strip ASCII whitespace per WHATWG atob spec.
    uint8_t* clean = static_cast<uint8_t*>(SDL_malloc(len ? len : 1u));
    if (!clean) { JS_FreeCString(js, raw); return JS_ThrowOutOfMemory(js); }
    size_t clen = 0u;
    for (size_t i = 0u; i < len; ++i) {
        const uint8_t ch = static_cast<uint8_t>(raw[i]);
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') continue;
        clean[clen++] = ch;
    }
    JS_FreeCString(js, raw);
    if (clen % 4u == 1u) {
        SDL_free(clean);
        return JS_ThrowRangeError(js, "atob: invalid base64 length");
    }
    // Encode decoded Latin-1 bytes as UTF-8 (max 2 bytes per byte).
    const size_t max_bytes = (clen * 3u) / 4u + 4u;
    char* utf8 = static_cast<char*>(SDL_malloc(max_bytes * 2u + 1u));
    if (!utf8) { SDL_free(clean); return JS_ThrowOutOfMemory(js); }
    size_t uo = 0u;
    auto push_latin1 = [&](uint8_t b) noexcept {
        if (b < 0x80u) {
            utf8[uo++] = static_cast<char>(b);
        } else {
            utf8[uo++] = static_cast<char>(0xC0u | (b >> 6));
            utf8[uo++] = static_cast<char>(0x80u | (b & 0x3Fu));
        }
    };
    bool ok = true;
    for (size_t i = 0u; i < clen && ok; i += 4u) {
        const size_t rem = clen - i;
        const int v0 = b64_dec_val(clean[i]);
        const int v1 = (rem > 1u) ? b64_dec_val(clean[i + 1u]) : -1;
        const bool p2 = (rem <= 2u) || clean[i + 2u] == '=';
        const bool p3 = (rem <= 3u) || clean[i + 3u] == '=';
        const int v2 = p2 ? 0 : b64_dec_val(clean[i + 2u]);
        const int v3 = p3 ? 0 : b64_dec_val(clean[i + 3u]);
        if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0) || (p2 && !p3)) {
            ok = false;
            break;
        }
        const uint32_t tri = (uint32_t(v0) << 18) | (uint32_t(v1) << 12) | (uint32_t(v2) << 6) | uint32_t(v3);
        push_latin1(static_cast<uint8_t>((tri >> 16) & 0xFFu));
        if (!p2) push_latin1(static_cast<uint8_t>((tri >> 8) & 0xFFu));
        if (!p3) push_latin1(static_cast<uint8_t>(tri & 0xFFu));
    }
    SDL_free(clean);
    if (!ok) {
        SDL_free(utf8);
        return JS_ThrowRangeError(js, "atob: invalid base64 character");
    }
    utf8[uo] = '\0';
    const JSValue res = JS_NewStringLen(js, utf8, uo);
    SDL_free(utf8);
    return res;
}

static SurfaceStorage* ls_owner(JSContext* js) noexcept {
    auto* s = static_cast<InlaySurface*>(JS_GetContextOpaque(js));
    if (!s) return nullptr;
    InlaySurface* target = ext(s)->host_surface ? ext(s)->host_surface : s;
    return ext(target);
}

static JSValue js_ls_get_item(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* stg = ls_owner(js);
    if (!stg || argc < 1) return JS_NULL;
    size_t klen = 0u;
    const char* key = JS_ToCStringLen(js, &klen, argv[0]);
    if (!key) return JS_EXCEPTION;
    if (stg->storage_get_fn) {
        const uint32_t cap = 65536u;
        char* buf = static_cast<char*>(SDL_malloc(cap + 1u));
        if (!buf) { JS_FreeCString(js, key); return JS_ThrowOutOfMemory(js); }
        uint32_t out_len = 0u;
        const bool found = stg->storage_get_fn({key, static_cast<uint32_t>(klen), false},
                                               buf, cap, &out_len, stg->storage_user);
        JS_FreeCString(js, key);
        if (!found) { SDL_free(buf); return JS_NULL; }
        const uint32_t n = out_len <= cap ? out_len : cap;
        const JSValue res = JS_NewStringLen(js, buf, n);
        SDL_free(buf);
        return res;
    }
    for (uint32_t i = 0u; i < stg->ls_count; ++i) {
        if (stg->ls_entries[i].key && std::strcmp(stg->ls_entries[i].key, key) == 0) {
            JS_FreeCString(js, key);
            return JS_NewString(js, stg->ls_entries[i].value ? stg->ls_entries[i].value : "");
        }
    }
    JS_FreeCString(js, key);
    return JS_NULL;
}

static JSValue js_ls_set_item(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* stg = ls_owner(js);
    if (!stg || argc < 2) return JS_UNDEFINED;
    size_t klen = 0u, vlen = 0u;
    const char* key = JS_ToCStringLen(js, &klen, argv[0]);
    if (!key) return JS_EXCEPTION;
    const char* val = JS_ToCStringLen(js, &vlen, argv[1]);
    if (!val) { JS_FreeCString(js, key); return JS_EXCEPTION; }
    if (klen > 256u || vlen > 65536u) {
        JS_FreeCString(js, key);
        JS_FreeCString(js, val);
        return JS_ThrowRangeError(js, "localStorage entry exceeds limit");
    }
    if (stg->storage_set_fn) {
        (void)stg->storage_set_fn({key, static_cast<uint32_t>(klen), false},
                                  {val, static_cast<uint32_t>(vlen), false}, stg->storage_user);
    }
    // Always mirror in local table too so `length` and `key(i)` work consistently.
    for (uint32_t i = 0u; i < stg->ls_count; ++i) {
        if (stg->ls_entries[i].key && std::strcmp(stg->ls_entries[i].key, key) == 0) {
            char* nv = SDL_strdup(val);
            if (nv) {
                SDL_free(stg->ls_entries[i].value);
                stg->ls_entries[i].value = nv;
            }
            JS_FreeCString(js, key);
            JS_FreeCString(js, val);
            return JS_UNDEFINED;
        }
    }
    if (inlay_grow_pod(stg->ls_entries, stg->ls_cap, stg->ls_count + 1u, 16u, 256u)) {
        char* nk = SDL_strdup(key);
        char* nv = SDL_strdup(val);
        if (nk && nv) {
            stg->ls_entries[stg->ls_count++] = {nk, nv};
        } else {
            SDL_free(nk);
            SDL_free(nv);
        }
    }
    JS_FreeCString(js, key);
    JS_FreeCString(js, val);
    return JS_UNDEFINED;
}

static JSValue js_ls_remove_item(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* stg = ls_owner(js);
    if (!stg || argc < 1) return JS_UNDEFINED;
    size_t klen = 0u;
    const char* key = JS_ToCStringLen(js, &klen, argv[0]);
    if (!key) return JS_EXCEPTION;
    if (stg->storage_remove_fn) {
        (void)stg->storage_remove_fn({key, static_cast<uint32_t>(klen), false}, stg->storage_user);
    }
    for (uint32_t i = 0u; i < stg->ls_count; ++i) {
        if (stg->ls_entries[i].key && std::strcmp(stg->ls_entries[i].key, key) == 0) {
            SDL_free(stg->ls_entries[i].key);
            SDL_free(stg->ls_entries[i].value);
            stg->ls_entries[i] = stg->ls_entries[--stg->ls_count];
            break;
        }
    }
    JS_FreeCString(js, key);
    return JS_UNDEFINED;
}

static JSValue js_ls_clear(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* stg = ls_owner(js);
    if (!stg) return JS_UNDEFINED;
    for (uint32_t i = 0u; i < stg->ls_count; ++i) {
        if (stg->storage_remove_fn && stg->ls_entries[i].key) {
            (void)stg->storage_remove_fn({stg->ls_entries[i].key,
                static_cast<uint32_t>(std::strlen(stg->ls_entries[i].key)), false}, stg->storage_user);
        }
        SDL_free(stg->ls_entries[i].key);
        SDL_free(stg->ls_entries[i].value);
    }
    stg->ls_count = 0u;
    return JS_UNDEFINED;
}

static JSValue js_ls_key(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* stg = ls_owner(js);
    if (!stg || argc < 1) return JS_NULL;
    uint32_t idx = 0u;
    if (JS_ToUint32(js, &idx, argv[0]) != 0 || idx >= stg->ls_count) return JS_NULL;
    return JS_NewString(js, stg->ls_entries[idx].key ? stg->ls_entries[idx].key : "");
}

static JSValue js_ls_length(JSContext* js, JSValueConst, int, JSValueConst*) {
    auto* stg = ls_owner(js);
    return JS_NewUint32(js, stg ? stg->ls_count : 0u);
}

bool init_vm_context(InlaySurface* s, const char** out_err) {
    clear_context(s);
    if (s->runtime) {
        JS_FreeRuntime(s->runtime);
        s->runtime = nullptr;
    }
    s->runtime = JS_NewRuntime();
    if (!s->runtime) {
        if (out_err) *out_err = "Unable to allocate script runtime";
        return false;
    }
    JS_SetMemoryLimit(s->runtime, s->desc.memory_limit_bytes);
    JS_SetMaxStackSize(s->runtime, s->desc.max_stack_bytes);
    JS_SetInterruptHandler(s->runtime, interrupt, s);
    JS_SetHostPromiseRejectionTracker(s->runtime, rejection_tracker, s);
    JS_SetModuleLoaderFunc(s->runtime, module_normalize_cb, module_loader_cb, s);
    s->js = JS_NewContext(s->runtime);
    if (!s->js) {
        if (out_err) *out_err = "Unable to allocate script context";
        return false;
    }
    JS_SetContextOpaque(s->js, s);
    auto* stg = ext(s);
    stg->atom_memo_key = JS_NewAtom(s->js, "__memoKey");
    stg->atom_memo_rev = JS_NewAtom(s->js, "__memoRev");
    stg->atom_eb_key = JS_NewAtom(s->js, "__ebKey");

    const JSValue global = JS_GetGlobalObject(s->js);
    JS_SetPropertyStr(s->js, global, "action", JS_NewCFunction(s->js, js_action, "action", 2));
    JS_SetPropertyStr(s->js, global, "request", JS_NewCFunction(s->js, js_request_async_action, "request", 2));
    JS_SetPropertyStr(s->js, global, "setTimeout",
                      JS_NewCFunction(s->js, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(s->js, global, "clearTimeout",
                      JS_NewCFunction(s->js, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(s->js, global, "setInterval",
                      JS_NewCFunction(s->js, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(s->js, global, "clearInterval",
                      JS_NewCFunction(s->js, js_clear_timeout, "clearInterval", 1));
    JS_SetPropertyStr(s->js, global, "requestAnimationFrame",
                      JS_NewCFunction(s->js, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(s->js, global, "cancelAnimationFrame",
                      JS_NewCFunction(s->js, js_clear_timeout, "cancelAnimationFrame", 1));
    {
        const JSValue con = JS_NewObject(s->js);
        JS_SetPropertyStr(s->js, con, "debug",
                          JS_NewCFunctionMagic(s->js, js_console_emit, "debug", 1, JS_CFUNC_generic_magic, INLAY_CONSOLE_DEBUG));
        JS_SetPropertyStr(s->js, con, "log",
                          JS_NewCFunctionMagic(s->js, js_console_emit, "log", 1, JS_CFUNC_generic_magic, INLAY_CONSOLE_INFO));
        JS_SetPropertyStr(s->js, con, "info",
                          JS_NewCFunctionMagic(s->js, js_console_emit, "info", 1, JS_CFUNC_generic_magic, INLAY_CONSOLE_INFO));
        JS_SetPropertyStr(s->js, con, "warn",
                          JS_NewCFunctionMagic(s->js, js_console_emit, "warn", 1, JS_CFUNC_generic_magic, INLAY_CONSOLE_WARN));
        JS_SetPropertyStr(s->js, con, "error",
                          JS_NewCFunctionMagic(s->js, js_console_emit, "error", 1, JS_CFUNC_generic_magic, INLAY_CONSOLE_ERROR));
        JS_SetPropertyStr(s->js, global, "console", con);
    }
    JS_SetPropertyStr(s->js, global, "fuzzyMatch",
                      JS_NewCFunction(s->js, js_fuzzy_match, "fuzzyMatch", 2));
    JS_SetPropertyStr(s->js, global, "clipboardWrite",
                      JS_NewCFunction(s->js, js_clipboard_write, "clipboardWrite", 1));
    JS_SetPropertyStr(s->js, global, "clipboardRead",
                      JS_NewCFunction(s->js, js_clipboard_read, "clipboardRead", 0));
    JS_SetPropertyStr(s->js, global, "clipboardReadImage",
                      JS_NewCFunction(s->js, js_clipboard_read_image, "clipboardReadImage", 0));
    JS_SetPropertyStr(s->js, global, "pushFocusRestorePoint",
                      JS_NewCFunction(s->js, js_push_focus_restore, "pushFocusRestorePoint", 0));
    JS_SetPropertyStr(s->js, global, "popFocusRestorePoint",
                      JS_NewCFunction(s->js, js_pop_focus_restore, "popFocusRestorePoint", 0));
    JS_SetPropertyStr(s->js, global, "setUiZoom",
                      JS_NewCFunction(s->js, js_set_ui_zoom, "setUiZoom", 1));
    JS_SetPropertyStr(s->js, global, "getUiZoom",
                      JS_NewCFunction(s->js, js_get_ui_zoom, "getUiZoom", 0));
    JS_SetPropertyStr(s->js, global, "focusElement",
                      JS_NewCFunction(s->js, js_focus_element, "focusElement", 1));
    JS_SetPropertyStr(s->js, global, "blurElement",
                      JS_NewCFunction(s->js, js_blur_element, "blurElement", 0));
    JS_SetPropertyStr(s->js, global, "scrollTo",
                      JS_NewCFunction(s->js, js_scroll_to, "scrollTo", 3));
    JS_SetPropertyStr(s->js, global, "scrollToBottom",
                      JS_NewCFunction(s->js, js_scroll_to_bottom, "scrollToBottom", 1));
    JS_SetPropertyStr(s->js, global, "scrollIntoView",
                      JS_NewCFunction(s->js, js_scroll_into_view, "scrollIntoView", 3));
    JS_SetPropertyStr(s->js, global, "measureElement",
                      JS_NewCFunction(s->js, js_measure_element, "measureElement", 1));
    JS_SetPropertyStr(s->js, global, "openUrl",
                      JS_NewCFunction(s->js, js_open_url, "openUrl", 1));
    JS_SetPropertyStr(s->js, global, "notifyDesktop",
                      JS_NewCFunction(s->js, js_notify_desktop, "notifyDesktop", 3));
    JS_SetPropertyStr(s->js, global, "openFileDialog",
                      JS_NewCFunction(s->js, js_open_file_dialog, "openFileDialog", 1));
    JS_SetPropertyStr(s->js, global, "openFolderDialog",
                      JS_NewCFunction(s->js, js_open_folder_dialog, "openFolderDialog", 1));
    JS_SetPropertyStr(s->js, global, "saveFileDialog",
                      JS_NewCFunction(s->js, js_save_file_dialog, "saveFileDialog", 1));
    JS_SetPropertyStr(s->js, global, "registerCommand",
                      JS_NewCFunction(s->js, js_register_command, "registerCommand", 1));
    JS_SetPropertyStr(s->js, global, "unregisterCommand",
                      JS_NewCFunction(s->js, js_unregister_command, "unregisterCommand", 1));
    JS_SetPropertyStr(s->js, global, "dispatchCommand",
                      JS_NewCFunction(s->js, js_dispatch_command, "dispatchCommand", 1));
    JS_SetPropertyStr(s->js, global, "pushCommandScope",
                      JS_NewCFunction(s->js, js_push_command_scope, "pushCommandScope", 1));
    JS_SetPropertyStr(s->js, global, "popCommandScope",
                      JS_NewCFunction(s->js, js_pop_command_scope, "popCommandScope", 0));
    JS_SetPropertyStr(s->js, global, "readStream",
                      JS_NewCFunction(s->js, js_read_stream, "readStream", 2));
    JS_SetPropertyStr(s->js, global, "saveDockLayout",
                      JS_NewCFunction(s->js, js_save_dock_layout, "saveDockLayout", 1));
    JS_SetPropertyStr(s->js, global, "loadDockLayout",
                      JS_NewCFunction(s->js, js_load_dock_layout, "loadDockLayout", 2));
    JS_SetPropertyStr(s->js, global, "detachDockPane",
                      JS_NewCFunction(s->js, js_detach_dock_pane, "detachDockPane", 2));
    JS_SetPropertyStr(s->js, global, "encodeTerminalKey",
                      JS_NewCFunction(s->js, js_encode_terminal_key, "encodeTerminalKey", 3));
    JS_SetPropertyStr(s->js, global, "encodeTerminalMouseSgr",
                      JS_NewCFunction(s->js, js_encode_terminal_mouse_sgr, "encodeTerminalMouseSgr", 6));
    {
        const JSValue perf = JS_NewObject(s->js);
        JS_SetPropertyStr(s->js, perf, "now", JS_NewCFunction(s->js, js_perf_now, "now", 0));
        JS_SetPropertyStr(s->js, global, "performance", perf);
    }
    {
        const JSValue cry = JS_NewObject(s->js);
        JS_SetPropertyStr(s->js, cry, "randomUUID", JS_NewCFunction(s->js, js_crypto_random_uuid, "randomUUID", 0));
        JS_SetPropertyStr(s->js, global, "crypto", cry);
    }
    JS_SetPropertyStr(s->js, global, "btoa", JS_NewCFunction(s->js, js_btoa, "btoa", 1));
    JS_SetPropertyStr(s->js, global, "atob", JS_NewCFunction(s->js, js_atob, "atob", 1));
    {
        const JSValue ls = JS_NewObject(s->js);
        JS_SetPropertyStr(s->js, ls, "getItem", JS_NewCFunction(s->js, js_ls_get_item, "getItem", 1));
        JS_SetPropertyStr(s->js, ls, "setItem", JS_NewCFunction(s->js, js_ls_set_item, "setItem", 2));
        JS_SetPropertyStr(s->js, ls, "removeItem", JS_NewCFunction(s->js, js_ls_remove_item, "removeItem", 1));
        JS_SetPropertyStr(s->js, ls, "clear", JS_NewCFunction(s->js, js_ls_clear, "clear", 0));
        JS_SetPropertyStr(s->js, ls, "key", JS_NewCFunction(s->js, js_ls_key, "key", 1));
        const JSAtom len_atom = JS_NewAtom(s->js, "length");
        const JSValue len_get = JS_NewCFunction(s->js, js_ls_length, "get length", 0);
        JS_DefinePropertyGetSet(s->js, ls, len_atom, len_get, JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(s->js, len_atom);
        JS_SetPropertyStr(s->js, global, "localStorage", ls);
    }
    JS_SetPropertyStr(s->js, global, "Atomics", JS_UNDEFINED);
    JS_SetPropertyStr(s->js, global, "SharedArrayBuffer", JS_UNDEFINED);
    JS_FreeValue(s->js, global);

    static constexpr char bootstrap[] =
        "globalThis.__frag=Symbol('frag');"
        "globalThis.__inlay_hm=Object.create(null);globalThis.__inlay_eb_map=Object.create(null);"
        "globalThis.__inlay_stk=[];globalThis.__inlay_has_hm=false;"
        "globalThis.__inlay_seen=Object.create(null);globalThis.__inlay_fx=[];globalThis.__inlay_dirty=false;"
        "globalThis.__inlay_begin=()=>{globalThis.__inlay_dirty=false;globalThis.__inlay_seen=Object.create(null);"
        "globalThis.__inlay_fx.length=0;globalThis.__inlay_stk=[{k:'__root',i:0,c:0,eb:'',ctx:null}];"
        "globalThis.__inlay_seen.__root=globalThis.__inlay_visit=1;};"
        "globalThis.__inlay_commit=()=>{if(globalThis.__inlay_has_hm){const hm=globalThis.__inlay_hm,seen=globalThis.__inlay_seen;"
        "let any=false;for(const k in hm){if(!seen[k]){const a=hm[k];for(let j=0;j<a.length;++j){"
        "const sl=a[j];if(sl&&sl.__fx&&typeof sl.cl==='function'){try{sl.cl();}catch(_){}}}delete hm[k];delete globalThis.__inlay_eb_map[k];}else any=true;}"
        "globalThis.__inlay_has_hm=any;}const fx=globalThis.__inlay_fx;if(!fx.length)return;"
        "globalThis.__inlay_fx=[];for(let j=0;j<fx.length;++j){const r=fx[j];if(typeof r.cl==='function')r.cl();"
        "const cl=r.fn();r.cl=typeof cl==='function'?cl:undefined;}};"
        "globalThis.h=(tag,props,...children)=>{"
        "if(tag===globalThis.__frag)return children;"
        "if(tag===Math)tag='math';"
        "if(typeof tag==='function'){"
        "if(tag.__ui)return tag(props||{},...children);"
        "const stk=globalThis.__inlay_stk,pf=stk.length?stk[stk.length-1]:{k:'__root',i:0,c:0,eb:'',ctx:null};"
        "const nm=tag.name||'C',hk=props&&props.key!==undefined&&props.key!==null;"
        "const sk=pf.k+'/'+(hk?(nm+'#'+String(props.key)):(nm+'@'+(pf.c++)));"
        "globalThis.__inlay_seen[sk]=++globalThis.__inlay_visit;stk.push({k:sk,i:0,c:0,memo:pf.memo,eb:pf.eb||'',ctx:pf.ctx||null});"
        "const mp=children.length||!(props&&'children'in props)?Object.assign({},props,{children}):Object.assign({},props);"
        "try{return tag(mp);}finally{stk.pop();}}"
        "const stk=globalThis.__inlay_stk,pf=stk.length?stk[stk.length-1]:null;"
        "const v={tag:tag==='switch'?'toggle':tag,props:props||{},children};"
        "if(pf&&pf.eb)Object.defineProperty(v,'__ebKey',{value:pf.eb,configurable:true});"
        "return v;};"
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
        "split:m('split'),math:x('math'),diagram:x('diagram'),slot:x('slot','name'),portal:x('portal','name'),"
        "doc:x('doc','markdown'),codeEditor:x('codeEditor','buffer'),terminal:x('terminal','ansi'),virtualList:m('virtualList'),"
        "table:m('table'),tree:m('tree'),treeItem:x('treeItem','label'),"
        "menu:m('menu'),contextMenu:m('contextMenu'),avatar:x('avatar','name'),"
        "toast:x('toast','description'),accordion:x('accordion','title'),skeleton:m('skeleton'),"
        "dockSpace:m('dockSpace'),dockPane:m('dockPane'),sheet:m('sheet'),hoverCard:m('hoverCard'),"
        "radioGroup:m('radioGroup'),toggleGroup:m('toggleGroup'),colorPicker:m('colorPicker'),"
        "chart:m('chart'),viewZone:m('viewZone'),diffView:m('diffView'),"
        "h:globalThis.h,fragment:(...c)=>c};"
        "const G=globalThis;"
        "G.Box='box';G.Row='row';G.Col='col';G.Card='card';G.Form='form';G.Text='text';"
        "G.Button='button';G.Input='input';G.Toggle='toggle';G.Switch='switch';"
        "G.Checkbox='checkbox';G.Tabs='tabs';G.Combobox='combobox';G.Scroll='scroll';"
        "G.Progress='progress';G.Slider='slider';G.Badge='badge';G.Divider='divider';"
        "G.Image='image';G.Modal='modal';G.Popover='popover';G.Tooltip='tooltip';"
        "G.Split='split';G.Spacer='spacer';G.Diagram='diagram';G.Slot='slot';G.Portal='portal';"
        "G.Doc='doc';G.CodeEditor='codeEditor';G.Terminal='terminal';G.VirtualList='virtualList';G.Table='table';"
        "G.Tree='tree';G.TreeItem='treeItem';G.Menu='menu';G.ContextMenu='contextMenu';"
        "G.Avatar='avatar';G.Toast='toast';G.Accordion='accordion';G.Skeleton='skeleton';"
        "G.DockSpace='dockSpace';G.DockPane='dockPane';G.Sheet='sheet';G.HoverCard='hoverCard';"
        "G.RadioGroup='radioGroup';G.ToggleGroup='toggleGroup';G.ColorPicker='colorPicker';"
        "G.Chart='chart';G.ViewZone='viewZone';G.DiffView='diffView';"
        "G.Fragment=G.__frag;"
        "const slot=()=>{const stk=G.__inlay_stk,f=stk.length?stk[stk.length-1]:{k:'__root',i:0,c:0,eb:'',ctx:null};"
        "if(f.i===0&&G.__inlay_collect)G.__inlay_collect.push(f.k);"
        "const hm=G.__inlay_hm;let a=hm[f.k];if(!a){a=hm[f.k]=[];G.__inlay_has_hm=true;}return[a,f.i++];};"
        "const same=(a,b)=>{if(!a||!b||a.length!==b.length)return false;"
        "for(let j=0;j<a.length;++j)if(!Object.is(a[j],b[j]))return false;return true;};"
        "G.useState=init=>{const[a,i]=slot();if(i>=a.length)a[i]=typeof init==='function'?init():init;"
        "const frame=G.__inlay_stk[G.__inlay_stk.length-1],owner=frame&&frame.memo;"
        "return[a[i],v=>{const n=typeof v==='function'?v(a[i]):v;"
        "if(!Object.is(a[i],n)){a[i]=n;for(let m=owner;m;m=m.parent)m.dirty=true;G.__inlay_dirty=true;}}];};"
        "G.useReducer=(reducer,initArg,init)=>{const[a,i]=slot();"
        "const frame=G.__inlay_stk[G.__inlay_stk.length-1],owner=frame&&frame.memo;"
        "if(i>=a.length){const st=typeof init==='function'?init(initArg):initArg;"
        "const cell={s:st,r:reducer,d:null};"
        "cell.d=act=>{const n=cell.r(cell.s,act);if(!Object.is(cell.s,n)){cell.s=n;for(let m=owner;m;m=m.parent)m.dirty=true;G.__inlay_dirty=true;}};"
        "a[i]=cell;}else{a[i].r=reducer;}return[a[i].s,a[i].d];};"
        "G.useRef=init=>{const[a,i]=slot();if(i>=a.length)a[i]={current:init};return a[i];};"
        "G.useMemo=(fn,deps)=>{const[a,i]=slot();"
        "if(i>=a.length||!deps||!same(a[i].d,deps))a[i]={v:fn(),d:deps?deps.slice():null};return a[i].v;};"
        "G.useCallback=(fn,deps)=>G.useMemo(()=>fn,deps);"
        "G.useEffect=(fn,deps)=>{const[a,i]=slot();"
        "if(i>=a.length){const r={__fx:1,d:deps?deps.slice():null,fn,cl:undefined};a[i]=r;G.__inlay_fx.push(r);}"
        "else if(!deps||!same(a[i].d,deps)){a[i].d=deps?deps.slice():null;a[i].fn=fn;G.__inlay_fx.push(a[i]);}};"
        "let nextCtxId=1;"
        "G.provideContext=(ctx,value)=>{const stk=G.__inlay_stk,f=stk.length?stk[stk.length-1]:null;"
        "if(!f||!ctx||!ctx.__ctxId)return;"
        "if(!f.ctxOwn){f.ctx=Object.create(f.ctx||null);f.ctxOwn=true;}"
        "f.ctx[ctx.__ctxId]=value;};"
        "G.useContext=ctx=>{const stk=G.__inlay_stk,f=stk.length?stk[stk.length-1]:null;"
        "if(!ctx||!ctx.__ctxId)return undefined;"
        "const v=(f&&f.ctx&&(ctx.__ctxId in f.ctx))?f.ctx[ctx.__ctxId]:ctx.defaultValue;"
        "for(let m=f&&f.memo;m;m=m.parent){if(!m.ctxs)m.ctxs=[];m.ctxs.push({c:ctx,v});}"
        "return v;};"
        "G.createContext=defaultValue=>{const id=nextCtxId++;"
        "const ctx={__ctxId:id,defaultValue,Provider:null};"
        "function Provider(p){G.provideContext(ctx,p.value);"
        "if(typeof p.render==='function')return p.render(p.value);"
        "const ch=p.children;"
        "if(typeof ch==='function')return ch(p.value);"
        "if(Array.isArray(ch)&&ch.length===1&&typeof ch[0]==='function')return ch[0](p.value);"
        "return ch;}"
        "ctx.Provider=Provider;return ctx;};})();"
        R"JS(
        (() => {
            const G = globalThis;
            const readCtxVal = (map, c) => (map && (c.__ctxId in map)) ? map[c.__ctxId] : c.defaultValue;
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
                    const ctxMatch = !cache.ctxs || cache.ctxs.every(e => Object.is(readCtxVal(frame.ctx, e.c), e.v));
                    if (cache.component === component && !cache.dirty && ctxMatch && equal(cache.props, props)) {
                        for (const key of cache.scopes) {
                            G.__inlay_seen[key] = ++G.__inlay_visit;
                            if (collect) collect.push(key);
                        }
                        if (cache.ctxs) {
                            for (let m = cache.parent; m; m = m.parent) {
                                if (!m.ctxs) m.ctxs = [];
                                for (const e of cache.ctxs) m.ctxs.push(e);
                            }
                        }
                        return cache.tree;
                    }
                    cache.component = null;
                    cache.dirty = false;
                    cache.ctxs = null;
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
            function Content(p) {
                const stk = G.__inlay_stk;
                if (stk.length) stk[stk.length - 1].eb = p.ebKey;
                return p.render();
            }
            function Fallback(p) { return p.render(p.error, p.retry); }
            function ErrorBoundary(p) {
                const stk = G.__inlay_stk, frame = stk.length ? stk[stk.length - 1] : null;
                const ebKey = frame ? frame.k : '__root';
                const slot = G.useRef(null);
                const [, invalidate] = G.useState(0);
                if (!slot.current || !Object.is(slot.current.key, p.resetKey))
                    slot.current = {key: p.resetKey, failed: false};
                const state = slot.current;
                G.__inlay_eb_map[ebKey] = {slot, state, invalidate};
                if (!state.failed) {
                    const visit = G.__inlay_visit, effects = G.__inlay_fx.length;
                    try {
                        return G.h(Content, {render: p.render, ebKey});
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
            G.__inlay_trip_eb = (key, err) => {
                const r = G.__inlay_eb_map[key];
                if (!r || r.state !== r.slot.current || r.state.failed) return false;
                r.state.failed = true;
                r.state.error = err;
                r.invalidate(n => n + 1);
                return true;
            };
            G.ErrorBoundary = ErrorBoundary;
            G.ui.errorBoundary = p => G.h(ErrorBoundary, p);
            G.ui.errorBoundary.__ui = true;
        })();
        )JS";
    static constexpr char bootstrap_globals[] = R"JS(
        (() => {
            const G = globalThis;
            // Standard JS/Web globals: queueMicrotask, structuredClone, TextEncoder/Decoder, URL/URLSearchParams
            if (typeof G.queueMicrotask !== 'function') {
                G.queueMicrotask = fn => { Promise.resolve().then(fn); };
            }
            if (typeof G.structuredClone !== 'function') {
                G.structuredClone = val => {
                    const seen = new WeakMap();
                    const clone = v => {
                        if (v === null || typeof v !== 'object') return v;
                        if (seen.has(v)) return seen.get(v);
                        if (v instanceof Date) return new Date(v.getTime());
                        if (v instanceof RegExp) return new RegExp(v.source, v.flags);
                        if (v instanceof ArrayBuffer) return v.slice(0);
                        if (ArrayBuffer.isView(v)) {
                            const Ctor = v.constructor;
                            return new Ctor(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength));
                        }
                        if (v instanceof Map) {
                            const out = new Map();
                            seen.set(v, out);
                            for (const [k, x] of v) out.set(clone(k), clone(x));
                            return out;
                        }
                        if (v instanceof Set) {
                            const out = new Set();
                            seen.set(v, out);
                            for (const x of v) out.add(clone(x));
                            return out;
                        }
                        if (Array.isArray(v)) {
                            const out = new Array(v.length);
                            seen.set(v, out);
                            for (let i = 0; i < v.length; ++i) out[i] = clone(v[i]);
                            return out;
                        }
                        const out = {};
                        seen.set(v, out);
                        for (const k of Object.keys(v)) out[k] = clone(v[k]);
                        return out;
                    };
                    return clone(val);
                };
            }
            if (typeof G.TextEncoder !== 'function') {
                G.TextEncoder = class TextEncoder {
                    constructor() { this.encoding = 'utf-8'; }
                    encode(str = '') {
                        const s = String(str), out = [];
                        for (let i = 0; i < s.length; ++i) {
                            let cp = s.charCodeAt(i);
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.length) {
                                const lo = s.charCodeAt(i + 1);
                                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
                            }
                            if (cp < 0x80) out.push(cp);
                            else if (cp < 0x800) out.push(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F));
                            else if (cp < 0x10000) out.push(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
                            else out.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
                        }
                        return new Uint8Array(out);
                    }
                    encodeInto(str, dest) {
                        const bytes = this.encode(str);
                        const n = Math.min(bytes.length, dest.length);
                        dest.set(bytes.subarray(0, n));
                        return { read: str.length, written: n };
                    }
                };
            }
            if (typeof G.TextDecoder !== 'function') {
                G.TextDecoder = class TextDecoder {
                    constructor(enc = 'utf-8') { this.encoding = String(enc).toLowerCase(); }
                    decode(input) {
                        if (!input) return '';
                        const b = input instanceof Uint8Array ? input
                            : ArrayBuffer.isView(input) ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength)
                            : new Uint8Array(input);
                        let res = '';
                        for (let i = 0; i < b.length; ) {
                            const c0 = b[i++];
                            if (c0 < 0x80) { res += String.fromCharCode(c0); continue; }
                            if ((c0 & 0xE0) === 0xC0 && i < b.length) {
                                const cp = ((c0 & 0x1F) << 6) | (b[i++] & 0x3F);
                                res += String.fromCharCode(cp);
                            } else if ((c0 & 0xF0) === 0xE0 && i + 1 < b.length) {
                                const cp = ((c0 & 0x0F) << 12) | ((b[i++] & 0x3F) << 6) | (b[i++] & 0x3F);
                                res += String.fromCharCode(cp);
                            } else if ((c0 & 0xF8) === 0xF0 && i + 2 < b.length) {
                                let cp = ((c0 & 0x07) << 18) | ((b[i++] & 0x3F) << 12) | ((b[i++] & 0x3F) << 6) | (b[i++] & 0x3F);
                                cp -= 0x10000;
                                res += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                            } else {
                                res += '\uFFFD';
                            }
                        }
                        return res;
                    }
                };
            }
            if (typeof G.URLSearchParams !== 'function') {
                const decQ = s => { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (_) { return s; } };
                const encQ = s => encodeURIComponent(String(s)).replace(/%20/g, '+');
                G.URLSearchParams = class URLSearchParams {
                    constructor(init = '') {
                        this._p = [];
                        this._cb = null;
                        if (typeof init === 'string') {
                            const s = init.startsWith('?') ? init.slice(1) : init;
                            if (s) for (const part of s.split('&')) {
                                if (!part) continue;
                                const eq = part.indexOf('=');
                                this._p.push(eq >= 0 ? [decQ(part.slice(0, eq)), decQ(part.slice(eq + 1))] : [decQ(part), '']);
                            }
                        } else if (Array.isArray(init)) {
                            for (const kv of init) if (kv && kv.length >= 2) this._p.push([String(kv[0]), String(kv[1])]);
                        } else if (init && typeof init === 'object') {
                            if (init instanceof URLSearchParams) {
                                for (const [k, v] of init._p) this._p.push([k, v]);
                            } else {
                                for (const k of Object.keys(init)) this._p.push([k, String(init[k])]);
                            }
                        }
                    }
                    _sync() { if (this._cb) this._cb(); }
                    get size() { return this._p.length; }
                    append(k, v) { this._p.push([String(k), String(v)]); this._sync(); }
                    delete(k) { const ks = String(k); this._p = this._p.filter(e => e[0] !== ks); this._sync(); }
                    get(k) { const ks = String(k); const e = this._p.find(x => x[0] === ks); return e ? e[1] : null; }
                    getAll(k) { const ks = String(k); return this._p.filter(x => x[0] === ks).map(x => x[1]); }
                    has(k) { const ks = String(k); return this._p.some(x => x[0] === ks); }
                    set(k, v) {
                        const ks = String(k), vs = String(v);
                        let found = false;
                        this._p = this._p.filter(x => { if (x[0] !== ks) return true; if (!found) { x[1] = vs; found = true; return true; } return false; });
                        if (!found) this._p.push([ks, vs]);
                        this._sync();
                    }
                    sort() { this._p.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); this._sync(); }
                    entries() { return this._p.map(e => [e[0], e[1]])[Symbol.iterator](); }
                    keys() { return this._p.map(e => e[0])[Symbol.iterator](); }
                    values() { return this._p.map(e => e[1])[Symbol.iterator](); }
                    forEach(fn, thisArg) { for (const [k, v] of this._p) fn.call(thisArg, v, k, this); }
                    [Symbol.iterator]() { return this.entries(); }
                    toString() { return this._p.map(([k, v]) => encQ(k) + '=' + encQ(v)).join('&'); }
                };
            }
            if (typeof G.URL !== 'function') {
                G.URL = class URL {
                    constructor(url, base) {
                        let raw = String(url);
                        if (base !== undefined) {
                            const b = base instanceof URL ? base : new URL(base);
                            if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(raw)) {
                                if (raw.startsWith('//')) raw = b.protocol + raw;
                                else if (raw.startsWith('/')) raw = b.origin + raw;
                                else if (raw.startsWith('?')) raw = b.origin + b.pathname + raw;
                                else if (raw.startsWith('#')) raw = b.origin + b.pathname + b.search + raw;
                                else {
                                    const dir = b.pathname.slice(0, b.pathname.lastIndexOf('/') + 1);
                                    raw = b.origin + dir + raw;
                                }
                            }
                        }
                        const m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@/?#]*)(?::([^@/?#]*))?@)?(\[[^\]]+\]|[^:/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(raw);
                        if (!m) throw new TypeError('Invalid URL: ' + raw);
                        this.protocol = m[1].toLowerCase();
                        this.username = m[2] || '';
                        this.password = m[3] || '';
                        this.hostname = (m[4] || '').toLowerCase();
                        this.port = m[5] || '';
                        this.pathname = m[6] || (this.hostname ? '/' : '');
                        this._search = m[7] || '';
                        this.hash = m[8] || '';
                        this.searchParams = new G.URLSearchParams(this._search);
                        this.searchParams._cb = () => { const s = this.searchParams.toString(); this._search = s ? ('?' + s) : ''; };
                    }
                    get host() { return this.port ? (this.hostname + ':' + this.port) : this.hostname; }
                    set host(v) { const s = String(v), c = s.lastIndexOf(':'); if (c >= 0) { this.hostname = s.slice(0, c); this.port = s.slice(c + 1); } else { this.hostname = s; this.port = ''; } }
                    get origin() { return this.hostname ? (this.protocol + '//' + this.host) : 'null'; }
                    get search() { return this._search; }
                    set search(v) { const s = String(v); this._search = s ? (s.startsWith('?') ? s : '?' + s) : ''; this.searchParams = new G.URLSearchParams(this._search); this.searchParams._cb = () => { const qs = this.searchParams.toString(); this._search = qs ? ('?' + qs) : ''; }; }
                    get href() {
                        const auth = this.username ? (this.username + (this.password ? ':' + this.password : '') + '@') : '';
                        const authHost = this.hostname ? ('//' + auth + this.host) : '';
                        return this.protocol + authHost + this.pathname + this._search + this.hash;
                    }
                    toString() { return this.href; }
                    toJSON() { return this.href; }
                };
            }
        })();
        )JS";

    bool ran_bc = false;
    if (stg->bootstrap_bc && stg->bootstrap_bc_len > 0u) {
        const JSValue fn = JS_ReadObject(s->js, stg->bootstrap_bc, stg->bootstrap_bc_len, JS_READ_OBJ_BYTECODE);
        if (!JS_IsException(fn)) {
            const JSValue br = JS_EvalFunction(s->js, fn);
            ran_bc = !JS_IsException(br);
            JS_FreeValue(s->js, br);
        }
        if (!ran_bc && JS_HasException(s->js)) {
            JS_FreeValue(s->js, JS_GetException(s->js));
        }
    }
    if (!ran_bc) {
        const size_t len1 = sizeof(bootstrap) - 1u;
        const size_t len2 = sizeof(bootstrap_globals) - 1u;
        const size_t total = len1 + 1u + len2;
        auto* joined = static_cast<char*>(SDL_malloc(total + 1u));
        if (!joined) {
            if (out_err) *out_err = "Unable to allocate script bootstrap";
            return false;
        }
        std::memcpy(joined, bootstrap, len1);
        joined[len1] = '\n';
        std::memcpy(joined + len1 + 1u, bootstrap_globals, len2);
        joined[total] = '\0';
        const JSValue fn = JS_Eval(s->js, joined, total, "<inlay>",
                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        SDL_free(joined);
        if (!JS_IsException(fn)) {
            if (!stg->bootstrap_bc) {
                size_t bc_len = 0u;
                uint8_t* raw = JS_WriteObject(s->js, &bc_len, fn,
                                              JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE);
                if (raw) {
                    if (bc_len > 0u) {
                        auto* copy = static_cast<uint8_t*>(SDL_malloc(bc_len));
                        if (copy) {
                            std::memcpy(copy, raw, bc_len);
                            stg->bootstrap_bc = copy;
                            stg->bootstrap_bc_len = bc_len;
                        }
                    }
                    js_free(s->js, raw);
                } else if (JS_HasException(s->js)) {
                    JS_FreeValue(s->js, JS_GetException(s->js));
                }
            }
            const JSValue br = JS_EvalFunction(s->js, fn);
            JS_FreeValue(s->js, br);
        }
    }
    if (JS_HasException(s->js)) {
        if (out_err) *out_err = "Unable to initialize script bootstrap";
        return false;
    }
    return true;
}

bool load_hmr_staged(InlaySurface* s, EtchStr effective, const char* filename) {
    static constexpr char prefix[] = "(function(){";
    static constexpr char suffix[] =
        "\n;return {render:typeof render==='function'?render:undefined,onEvent:typeof "
        "onEvent==='function'?onEvent:undefined,onCommand:typeof onCommand==='function'?onCommand:undefined};})()";

    auto* staged = static_cast<SurfaceStorage*>(SDL_calloc(1u, sizeof(SurfaceStorage)));
    if (!staged) {
        record_reload_error(s, "HMR staging allocation failed");
        return false;
    }
    InlaySurface* st = &staged->pub;
    auto* live_ext = ext(s);

    st->etch = s->etch;
    st->desc = s->desc;
    st->frequency = s->frequency;
    st->frame_counter = s->frame_counter;
    st->last_view_width_px = s->last_view_width_px;
    st->current_container_width_px = s->last_view_width_px;
    st->next_timer = s->next_timer;
    st->image_resolver_fn = s->image_resolver_fn;
    st->image_resolver_user_data = s->image_resolver_user_data;
    st->console_fn = s->console_fn;
    st->console_user_data = s->console_user_data;
    st->code_tok_fn = s->code_tok_fn;
    st->code_tok_user_data = s->code_tok_user_data;
    st->doc_hl_fn = s->doc_hl_fn;
    st->doc_hl_user_data = s->doc_hl_user_data;
    st->doc_fence_est = s->doc_fence_est;
    st->doc_fence_ren = s->doc_fence_ren;
    st->doc_fence_user_data = s->doc_fence_user_data;
    staged->last_view_id = live_ext->last_view_id;
    staged->command_names = live_ext->command_names;
    staged->command_name_count = live_ext->command_name_count;
    staged->command_name_cap = live_ext->command_name_cap;
    std::memcpy(st->path, s->path, sizeof(st->path));
    st->actions = s->actions;
    st->action_count = s->action_count;
    st->action_cap = s->action_cap;
    std::memcpy(st->slots, s->slots, sizeof(st->slots));
    st->slot_count = s->slot_count;
    for (auto& timer : st->timers) timer.function = JS_UNDEFINED;

    staged->host_surface = s;
    staged->has_view_width = live_ext->has_view_width;
    staged->audit_fn = live_ext->audit_fn;
    staged->audit_user = live_ext->audit_user;
    staged->async_actions_closed = live_ext->async_actions_closed;
    staged->next_async_action = live_ext->next_async_action;
    staged->container_width_count = live_ext->container_width_count;
    if (staged->container_width_count > 0u) {
        std::memcpy(staged->container_widths, live_ext->container_widths,
                    size_t(staged->container_width_count) * sizeof(ContainerWidthEntry));
    }

    // Borrow shared/read-only buffers from live surface (always restored & nulled before teardown).
    st->json_scratch = s->json_scratch;
    st->content = s->content;
    st->content_count = s->content_count;
    st->content_cap = s->content_cap;
    st->terminals = s->terminals;
    st->terminal_count = s->terminal_count;
    st->terminal_cap = s->terminal_cap;
    st->streams = s->streams;
    st->stream_count = s->stream_count;
    st->stream_cap = s->stream_cap;
    staged->modules = live_ext->modules;
    staged->module_count = live_ext->module_count;
    staged->module_cap = live_ext->module_cap;
    st->vlist_cache = s->vlist_cache;
    st->vlist_cache_count = s->vlist_cache_count;
    st->vlist_cache_cap = s->vlist_cache_cap;
    st->cls_cache = s->cls_cache;
    st->cls_cache_strs = s->cls_cache_strs;
    st->cls_cache_str_used = s->cls_cache_str_used;
    st->cls_cache_clock = s->cls_cache_clock;
    staged->action_policies = live_ext->action_policies;
    staged->action_policy_count = live_ext->action_policy_count;
    staged->action_policy_cap = live_ext->action_policy_cap;
    staged->async_action_names = live_ext->async_action_names;
    staged->async_action_name_count = live_ext->async_action_name_count;
    staged->async_action_name_cap = live_ext->async_action_name_cap;
    staged->bootstrap_bc = live_ext->bootstrap_bc;
    staged->bootstrap_bc_len = live_ext->bootstrap_bc_len;

    bool borrowed_released = false;
    auto release_borrowed = [&]() noexcept {
        if (borrowed_released) return;
        borrowed_released = true;
        s->cls_cache = st->cls_cache;
        s->cls_cache_strs = st->cls_cache_strs;
        s->cls_cache_str_used = st->cls_cache_str_used;
        s->cls_cache_clock = st->cls_cache_clock;
        if (!live_ext->bootstrap_bc && staged->bootstrap_bc) {
            live_ext->bootstrap_bc = staged->bootstrap_bc;
            live_ext->bootstrap_bc_len = staged->bootstrap_bc_len;
        }
        live_ext->command_names = staged->command_names;
        live_ext->command_name_count = staged->command_name_count;
        live_ext->command_name_cap = staged->command_name_cap;
        staged->command_names = nullptr;
        staged->command_name_count = 0u;
        staged->command_name_cap = 0u;
        st->actions = nullptr;
        st->action_count = 0u;
        st->action_cap = 0u;
        st->json_scratch = nullptr;
        st->content = nullptr;
        st->content_count = 0u;
        st->content_cap = 0u;
        st->terminals = nullptr;
        st->terminal_count = 0u;
        st->terminal_cap = 0u;
        st->streams = nullptr;
        st->stream_count = 0u;
        st->stream_cap = 0u;
        staged->modules = nullptr;
        staged->module_count = 0u;
        staged->module_cap = 0u;
        st->vlist_cache = nullptr;
        st->vlist_cache_count = 0u;
        st->vlist_cache_cap = 0u;
        st->cls_cache = nullptr;
        st->cls_cache_strs = nullptr;
        staged->action_policies = nullptr;
        staged->action_policy_count = 0u;
        staged->action_policy_cap = 0u;
        staged->async_action_names = nullptr;
        staged->async_action_name_count = 0u;
        staged->async_action_name_cap = 0u;
        staged->bootstrap_bc = nullptr;
        staged->bootstrap_bc_len = 0u;
    };

    auto destroy_staged = [&]() noexcept {
        release_borrowed();
        if (staged->async_actions) {
            if (st->js) {
                for (uint32_t i = 0u; i < kAsyncActionCapacity; ++i) {
                    auto& slot = staged->async_actions[i];
                    if (slot.phase == AsyncActionPhase::Queued || slot.phase == AsyncActionPhase::Started) {
                        release_async_action(st, slot, false);
                    }
                }
            }
            SDL_free(staged->async_actions);
            staged->async_actions = nullptr;
        }
        clear_context(st);
        if (st->runtime) {
            JS_FreeRuntime(st->runtime);
            st->runtime = nullptr;
        }
        free_spec_pools(st);
        SDL_free(st->handlers);
        SDL_free(staged->handler_eb_keys);
        SDL_free(st->nodes);
        SDL_free(st->styles);
        SDL_free(staged->node_extra_idx);
        SDL_free(staged->core_extras);
        SDL_free(st->cold_visuals);
        SDL_free(st->shadow_pool);
        SDL_free(st->gradient_pool);
        SDL_free(st->item_pool);
        SDL_free(st->text);
        SDL_free(st->source);
        SDL_free(st->state);
        SDL_free(staged);
    };

    const uint32_t init_nodes = st->desc.max_nodes < 64u ? st->desc.max_nodes : 64u;
    const uint32_t init_text = st->desc.max_text_bytes < 2048u ? st->desc.max_text_bytes : 2048u;
    st->nodes = static_cast<InlayUiNode*>(SDL_calloc(init_nodes, sizeof(InlayUiNode)));
    st->styles = static_cast<NodeStyle*>(SDL_calloc(init_nodes, sizeof(NodeStyle)));
    staged->node_extra_idx = static_cast<uint32_t*>(SDL_malloc(size_t(init_nodes) * sizeof(uint32_t)));
    if (staged->node_extra_idx) {
        for (uint32_t i = 0u; i < init_nodes; ++i) staged->node_extra_idx[i] = INLAY_UI_NONE;
    }
    st->node_capacity = init_nodes;
    const bool text_ok = inlay_grow_pod(st->text, staged->text_capacity, init_text, init_text, st->desc.max_text_bytes);

    const bool is_module = effective.length > 0u && script_uses_esm(effective.chars, effective.length);
    const uint32_t needed = is_module
        ? (effective.length + 1u)
        : (effective.length + uint32_t(sizeof(prefix) + sizeof(suffix)));
    uint32_t src_cap = 4096u;
    while (src_cap < needed) src_cap *= 2u;
    st->source = static_cast<char*>(SDL_malloc(src_cap));
    st->source_capacity = st->source ? src_cap : 0u;

    const bool state_ok = save_state(st, s->state ? s->state : "{}", s->state ? s->state_size : 2u);
    if (!st->nodes || !st->styles || !staged->node_extra_idx || !text_ok || !st->text || !st->source || !state_ok) {
        destroy_staged();
        record_reload_error(s, "HMR staging storage allocation failed");
        return false;
    }
    size_t length = 0u;
    if (is_module) {
        std::memcpy(st->source, effective.chars, effective.length);
        st->source[effective.length] = '\0';
        length = effective.length;
    } else {
        std::memcpy(st->source, prefix, sizeof(prefix) - 1u);
        if (effective.length > 0u) {
            std::memcpy(st->source + sizeof(prefix) - 1u, effective.chars, effective.length);
        }
        std::memcpy(st->source + sizeof(prefix) - 1u + effective.length, suffix, sizeof(suffix));
        length = sizeof(prefix) - 1u + effective.length + sizeof(suffix) - 1u;
    }

    const char* vm_err = nullptr;
    if (!init_vm_context(st, &vm_err)) {
        destroy_staged();
        record_reload_error(s, vm_err ? vm_err : "HMR staging VM initialization failed");
        return false;
    }

    const uint64_t start = SDL_GetPerformanceCounter();
    begin_tick(st, 4.0);
    const JSValue global = JS_GetGlobalObject(st->js);
    bool ok = !JS_HasException(st->js) && install_state(st, global);
    JS_FreeValue(st->js, global);
    if (ok) {
        if (is_module) {
            ok = eval_entry_module(st, st->source, length, filename);
        } else {
            const JSValue result = JS_Eval(st->js, st->source, length, filename, JS_EVAL_TYPE_GLOBAL);
            ok = !JS_IsException(result);
            if (ok) {
                const JSValue target = JS_GetGlobalObject(st->js);
                JS_SetPropertyStr(st->js, target, "render", JS_GetPropertyStr(st->js, result, "render"));
                JS_SetPropertyStr(st->js, target, "onEvent", JS_GetPropertyStr(st->js, result, "onEvent"));
                JS_SetPropertyStr(st->js, target, "onCommand", JS_GetPropertyStr(st->js, result, "onCommand"));
                JS_FreeValue(st->js, target);
            }
            JS_FreeValue(st->js, result);
        }
    }
    if (ok && !JS_HasException(st->js)) ok = rebuild(st, true);
    const bool completed = finish_tick(st, start) && ok && !st->stats.has_error && !st->timed_out;

    s->stats.js_ticks += st->stats.js_ticks;
    s->stats.last_tick_ms = st->stats.last_tick_ms;

    if (!completed) {
        char err_copy[1024] = {};
        std::snprintf(err_copy, sizeof(err_copy), "%s", st->error[0] ? st->error : "HMR candidate reload failed");
        destroy_staged();
        record_reload_error(s, err_copy);
        return false;
    }

    // Candidate fully succeeded: commit transaction atomically into live surface.
    release_borrowed();

    // Retire old live runtime, timers, handlers, memo cache, and started async actions.
    clear_context(s);
    if (s->runtime) {
        JS_FreeRuntime(s->runtime);
        s->runtime = nullptr;
    }

    // Merge newly queued async actions from staged into live surface slots.
    if (staged->async_actions) {
        if (!live_ext->async_actions) {
            live_ext->async_actions =
                static_cast<AsyncActionSlot*>(SDL_calloc(kAsyncActionCapacity, sizeof(AsyncActionSlot)));
        }
        for (uint32_t i = 0u; i < kAsyncActionCapacity; ++i) {
            auto& src_slot = staged->async_actions[i];
            if (src_slot.phase != AsyncActionPhase::Queued) continue;
            bool placed = false;
            if (live_ext->async_actions) {
                for (uint32_t j = 0u; j < kAsyncActionCapacity; ++j) {
                    if (live_ext->async_actions[j].phase == AsyncActionPhase::Empty) {
                        live_ext->async_actions[j] = src_slot;
                        src_slot = {};
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed) release_async_action(st, src_slot, false);
        }
        SDL_free(staged->async_actions);
        staged->async_actions = nullptr;
    }
    live_ext->next_async_action = staged->next_async_action;

    // Move validated VM, atoms, timers, memo cache, nodes, pools, handlers, source, and state.
    s->runtime = st->runtime;           st->runtime = nullptr;
    s->js = st->js;                     st->js = nullptr;
    live_ext->atom_memo_key = staged->atom_memo_key; staged->atom_memo_key = JS_ATOM_NULL;
    live_ext->atom_memo_rev = staged->atom_memo_rev; staged->atom_memo_rev = JS_ATOM_NULL;
    live_ext->atom_eb_key = staged->atom_eb_key;     staged->atom_eb_key = JS_ATOM_NULL;
    JS_SetInterruptHandler(s->runtime, interrupt, s);
    JS_SetHostPromiseRejectionTracker(s->runtime, rejection_tracker, s);
    JS_SetModuleLoaderFunc(s->runtime, module_normalize_cb, module_loader_cb, s);
    JS_SetContextOpaque(s->js, s);

    for (uint32_t i = 0u; i < 64u; ++i) {
        s->timers[i] = st->timers[i];
        st->timers[i] = {JS_UNDEFINED, 0.0, 0.0, 0u, false};
    }
    s->next_timer = st->next_timer;

    std::memcpy(live_ext->memo_cache, staged->memo_cache, sizeof(live_ext->memo_cache));
    std::memset(staged->memo_cache, 0, sizeof(staged->memo_cache));
    live_ext->memo_cache_clock = staged->memo_cache_clock;
    live_ext->memo_cache_bytes = staged->memo_cache_bytes;
    staged->memo_cache_bytes = 0u;

    SDL_free(s->handlers);               s->handlers = st->handlers;                     st->handlers = nullptr;
    SDL_free(live_ext->handler_eb_keys); live_ext->handler_eb_keys = staged->handler_eb_keys; staged->handler_eb_keys = nullptr;
    s->handler_count = st->handler_count;       st->handler_count = 0u;
    s->handler_capacity = st->handler_capacity; st->handler_capacity = 0u;

    SDL_free(s->nodes);                  s->nodes = st->nodes;                           st->nodes = nullptr;
    SDL_free(s->styles);                 s->styles = st->styles;                         st->styles = nullptr;
    SDL_free(live_ext->node_extra_idx);  live_ext->node_extra_idx = staged->node_extra_idx; staged->node_extra_idx = nullptr;
    s->node_count = st->node_count;             st->node_count = 0u;
    s->node_capacity = st->node_capacity;       st->node_capacity = 0u;

    SDL_free(live_ext->core_extras);     live_ext->core_extras = staged->core_extras;    staged->core_extras = nullptr;
    live_ext->core_extra_count = staged->core_extra_count; staged->core_extra_count = 0u;
    live_ext->core_extra_cap = staged->core_extra_cap;     staged->core_extra_cap = 0u;

    SDL_free(s->cold_visuals);           s->cold_visuals = st->cold_visuals;             st->cold_visuals = nullptr;
    s->cold_visual_count = st->cold_visual_count; st->cold_visual_count = 0u;
    s->cold_visual_cap = st->cold_visual_cap;     st->cold_visual_cap = 0u;

    SDL_free(s->shadow_pool);            s->shadow_pool = st->shadow_pool;               st->shadow_pool = nullptr;
    s->shadow_pool_count = st->shadow_pool_count; st->shadow_pool_count = 0u;
    s->shadow_pool_cap = st->shadow_pool_cap;     st->shadow_pool_cap = 0u;

    SDL_free(s->gradient_pool);          s->gradient_pool = st->gradient_pool;           st->gradient_pool = nullptr;
    s->gradient_pool_count = st->gradient_pool_count; st->gradient_pool_count = 0u;
    s->gradient_pool_cap = st->gradient_pool_cap;     st->gradient_pool_cap = 0u;

    SDL_free(s->item_pool);              s->item_pool = st->item_pool;                   st->item_pool = nullptr;
    s->item_pool_count = st->item_pool_count;     st->item_pool_count = 0u;
    s->item_pool_cap = st->item_pool_cap;         st->item_pool_cap = 0u;

    move_spec_pools(s, st);

    SDL_free(s->text);                   s->text = st->text;                             st->text = nullptr;
    s->text_used = st->text_used;                 st->text_used = 0u;
    live_ext->text_capacity = staged->text_capacity; staged->text_capacity = 0u;

    SDL_free(s->source);                 s->source = st->source;                         st->source = nullptr;
    s->source_capacity = st->source_capacity;     st->source_capacity = 0u;

    SDL_free(s->state);                  s->state = st->state;                           st->state = nullptr;
    s->state_size = st->state_size;               st->state_size = 0u;
    s->state_capacity = st->state_capacity;       st->state_capacity = 0u;

    live_ext->boundary_root_count = staged->boundary_root_count;
    if (live_ext->boundary_root_count > 0u) {
        std::memcpy(live_ext->boundary_roots, staged->boundary_roots,
                    size_t(live_ext->boundary_root_count) * sizeof(BoundaryRootEntry));
    }
    live_ext->cur_eb_key = {};
    live_ext->container_width_count = staged->container_width_count;
    if (live_ext->container_width_count > 0u) {
        std::memcpy(live_ext->container_widths, staged->container_widths,
                    size_t(live_ext->container_width_count) * sizeof(ContainerWidthEntry));
    }
    live_ext->uses_responsive_cls = staged->uses_responsive_cls;
    live_ext->responsive_cls_hits = staged->responsive_cls_hits;
    live_ext->needs_container_rebuild = false;
    s->memo_reused_nodes += st->memo_reused_nodes;

    s->loaded = true;
    s->stats.has_error = false;
    s->error[0] = '\0';
    s->stats.node_count = s->node_count;
    s->pending_state = false;
    s->state_dirty = false;
    SDL_free(live_ext->reload_error);
    live_ext->reload_error = nullptr;

    destroy_staged();
    etch_request_redraw(s->etch);
    return true;
}

bool load(InlaySurface* s, EtchStr script, bool is_hmr = false) {
    // No newline in prefix so QuickJS line 1 == user script line 1!
    static constexpr char prefix[] = "(function(){";
    static constexpr char suffix[] =
        "\n;return {render:typeof render==='function'?render:undefined,onEvent:typeof "
        "onEvent==='function'?onEvent:undefined,onCommand:typeof onCommand==='function'?onCommand:undefined};})()";

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

    // R03: Transactional HMR staging on live surfaces — candidate compiles, inits & renders in isolation.
    if (is_hmr && s->loaded && !s->stats.has_error && s->js) {
        return load_hmr_staged(s, effective, filename);
    }

    const bool is_module = effective.length > 0u && script_uses_esm(effective.chars, effective.length);
    const uint32_t needed = is_module
        ? (effective.length + 1u)
        : (effective.length + uint32_t(sizeof(prefix) + sizeof(suffix)));
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
    size_t length = 0u;
    if (is_module) {
        std::memcpy(s->source, effective.chars, effective.length);
        s->source[effective.length] = '\0';
        length = effective.length;
    } else {
        std::memcpy(s->source, prefix, sizeof(prefix) - 1u);
        if (effective.length) {
            std::memcpy(s->source + sizeof(prefix) - 1u, effective.chars, effective.length);
        }
        std::memcpy(s->source + sizeof(prefix) - 1u + effective.length, suffix, sizeof(suffix));
        length = sizeof(prefix) - 1u + effective.length + sizeof(suffix) - 1u;
    }

    ext(s)->container_width_count = 0u;
    ext(s)->needs_container_rebuild = false;
    const char* vm_err = nullptr;
    if (!init_vm_context(s, &vm_err)) {
        record_error(s, vm_err ? vm_err : "Unable to allocate script context");
        return false;
    }

    s->stats.has_error = false;
    s->error[0] = '\0';
    s->pending_state = false;
    s->state_dirty = false;
    // The fixed framework bootstrap runs in a new, empty VM. Start the
    // caller's execution budget before installing state or running user JS.
    const uint64_t start = SDL_GetPerformanceCounter();
    begin_tick(s, 4.0);
    const JSValue global = JS_GetGlobalObject(s->js);
    bool ok = !JS_HasException(s->js) && install_state(s, global);
    JS_FreeValue(s->js, global);
    if (ok) {
        if (is_module) {
            ok = eval_entry_module(s, s->source, length, filename);
        } else {
            const JSValue result = JS_Eval(s->js, s->source, length, filename, JS_EVAL_TYPE_GLOBAL);
            ok = !JS_IsException(result);
            if (ok) {
                const JSValue target = JS_GetGlobalObject(s->js);
                JS_SetPropertyStr(s->js, target, "render", JS_GetPropertyStr(s->js, result, "render"));
                JS_SetPropertyStr(s->js, target, "onEvent", JS_GetPropertyStr(s->js, result, "onEvent"));
                JS_SetPropertyStr(s->js, target, "onCommand", JS_GetPropertyStr(s->js, result, "onCommand"));
                JS_FreeValue(s->js, target);
            }
            JS_FreeValue(s->js, result);
        }
    }
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

uint32_t hash_str_bytes(EtchStr s) noexcept {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < s.length; ++i) {
        h ^= static_cast<uint8_t>(s.chars[i]);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

void dispatch_dnd_and_keys(InlaySurface* s, const ColdVisualStyle* cold, uint32_t id, WidgetEvents* events) {
    if (!cold || !events) return;
    if (cold->dnd_idx != INLAY_UI_NONE && cold->dnd_idx < s->dnd_spec_count) {
        const NodeDndSpec& dnd = s->dnd_specs[cold->dnd_idx];
        if (dnd.draggable) {
            const EtchStr kind_s = inlay_ui_slice(s, dnd.drag_kind);
            const uint32_t kind_h = kind_s.length > 0u ? hash_str_bytes(kind_s) : etch_id("item");
            const EtchStr lbl = dnd.drag_label.len ? inlay_ui_frame_text(s, inlay_ui_slice(s, dnd.drag_label))
                                                   : ETCH_STR_LIT("item");
            const EtchStr dat = inlay_ui_slice(s, dnd.drag_data);
            (void)etch_begin_drag_payload(id, kind_h, lbl, dat.chars, dat.length, 0u, 0u);
        }
        if (has_event(dnd.on_drop_action, dnd.on_drop_handler)) {
            const EtchStr acc_s = inlay_ui_slice(s, dnd.drop_accept);
            const uint32_t acc_h = acc_s.length > 0u ? hash_str_bytes(acc_s) : 0u;
            EtchDragPayloadView pv = {};
            if (etch_accept_drag_payload(id, acc_h, &pv)) {
                const uint32_t cap = pv.label.length * 2u + pv.size * 2u + 96u;
                ArenaJsonWriter w = arena_json_begin(s->etch, cap);
                arena_json_raw(w, "{\"kindHash\":", 12u);
                char kh[24];
                const int kn = std::snprintf(kh, sizeof(kh), "%u", pv.kind_hash);
                if (kn > 0) arena_json_raw(w, kh, uint32_t(kn));
                arena_json_raw(w, ",\"label\":", 9u);
                arena_json_quoted(w, pv.label);
                arena_json_raw(w, ",\"data\":", 8u);
                arena_json_quoted(w, {static_cast<const char*>(pv.data), pv.size, false});
                arena_json_raw(w, "}", 1u);
                const EtchStr payload = arena_json_finish(w);
                if (payload.length > 0u) {
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, dnd.on_drop_action),
                                         dnd.on_drop_handler, payload, true);
                }
            }
        }
        if (has_event(dnd.on_file_drop_action, dnd.on_file_drop_handler)) {
            const uint32_t fc = etch_dropped_file_on_count(id);
            const uint32_t tc = etch_dropped_text_on_count(id);
            if (fc > 0u || tc > 0u) {
                uint32_t cap = 64u;
                for (uint32_t i = 0u; i < fc; ++i) cap += etch_dropped_file_on_at(id, i).length * 2u + 8u;
                for (uint32_t i = 0u; i < tc; ++i) cap += etch_dropped_text_on_at(id, i).length * 2u + 8u;
                ArenaJsonWriter w = arena_json_begin(s->etch, cap);
                arena_json_raw(w, "{\"files\":[", 10u);
                for (uint32_t i = 0u; i < fc; ++i) {
                    if (i > 0u) arena_json_raw(w, ",", 1u);
                    arena_json_quoted(w, etch_dropped_file_on_at(id, i));
                }
                arena_json_raw(w, "],\"texts\":[", 11u);
                for (uint32_t i = 0u; i < tc; ++i) {
                    if (i > 0u) arena_json_raw(w, ",", 1u);
                    arena_json_quoted(w, etch_dropped_text_on_at(id, i));
                }
                arena_json_raw(w, "]}", 2u);
                const EtchStr payload = arena_json_finish(w);
                if (payload.length > 0u) {
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, dnd.on_file_drop_action),
                                         dnd.on_file_drop_handler, payload, true);
                }
            }
        }
        if (has_event(dnd.on_drag_hover_action, dnd.on_drag_hover_handler)) {
            const bool hov = etch_os_drag_hovering(id);
            DragHoverSlot* dh = nullptr;
            for (uint32_t i = 0u; i < s->drag_hover_state_count; ++i) {
                if (s->drag_hover_states[i].element_id == id) { dh = &s->drag_hover_states[i]; break; }
            }
            if (!dh && inlay_grow_pod(s->drag_hover_states, s->drag_hover_state_cap, s->drag_hover_state_count + 1u, 8u, 64u)) {
                dh = &s->drag_hover_states[s->drag_hover_state_count++];
                *dh = {id, false, s->frame_counter};
            }
            if (dh) {
                dh->last_used_frame = s->frame_counter;
                if (dh->hovering != hov) {
                    dh->hovering = hov;
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, dnd.on_drag_hover_action),
                                         dnd.on_drag_hover_handler, hov ? ETCH_STR_LIT("true") : ETCH_STR_LIT("false"));
                }
            }
        }
    }

    if (has_event(cold->on_key_action, cold->on_key_handler) && (cold->global_keys || etch_focused(id))) {
        auto key_name = [](uint32_t code, char tmp[16]) noexcept -> const char* {
            switch (code) {
                case SDLK_ESCAPE: return "Escape";
                case SDLK_RETURN: case SDLK_KP_ENTER: return "Enter";
                case SDLK_TAB: return "Tab";
                case SDLK_BACKSPACE: return "Backspace";
                case SDLK_DELETE: return "Delete";
                case SDLK_SPACE: return "Space";
                case SDLK_UP: return "ArrowUp";
                case SDLK_DOWN: return "ArrowDown";
                case SDLK_LEFT: return "ArrowLeft";
                case SDLK_RIGHT: return "ArrowRight";
                case SDLK_HOME: return "Home";
                case SDLK_END: return "End";
                case SDLK_PAGEUP: return "PageUp";
                case SDLK_PAGEDOWN: return "PageDown";
                case SDLK_INSERT: return "Insert";
                case SDLK_F1: return "F1"; case SDLK_F2: return "F2"; case SDLK_F3: return "F3"; case SDLK_F4: return "F4";
                case SDLK_F5: return "F5"; case SDLK_F6: return "F6"; case SDLK_F7: return "F7"; case SDLK_F8: return "F8";
                case SDLK_F9: return "F9"; case SDLK_F10: return "F10"; case SDLK_F11: return "F11"; case SDLK_F12: return "F12";
                default: break;
            }
            if (code >= 0x21u && code <= 0x7Eu) {
                if (code == '"') return "\\\"";
                if (code == '\\') return "\\\\";
                tmp[0] = static_cast<char>(code);
                tmp[1] = '\0';
                return tmp;
            }
            std::snprintf(tmp, 16u, "Key%u", code);
            return tmp;
        };
        const uint32_t kc = etch_frame_key_count();
        for (uint32_t ki = 0u; ki < kc; ++ki) {
            EtchFrameKeyEvent fk = {};
            if (!etch_frame_key_at(ki, &fk) || fk.consumed || fk.keycode == 0u) continue;
            char tmp[16] = {};
            const char* name = key_name(fk.keycode, tmp);
            char payload[160];
            const int n = std::snprintf(payload, sizeof(payload),
                "{\"key\":\"%s\",\"keyCode\":%u,\"ctrl\":%s,\"shift\":%s,\"alt\":%s,\"meta\":%s,\"repeat\":%s}",
                name, fk.keycode,
                (fk.mods & ETCH_MOD_CTRL) ? "true" : "false",
                (fk.mods & ETCH_MOD_SHIFT) ? "true" : "false",
                (fk.mods & ETCH_MOD_ALT) ? "true" : "false",
                (fk.mods & ETCH_MOD_SUPER) ? "true" : "false",
                fk.repeat ? "true" : "false");
            if (n > 0) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, cold->on_key_action),
                                     cold->on_key_handler, inlay_ui_frame_text(s, {payload, uint32_t(n), false}));
                if (cold->consume_keys) (void)etch_consume_frame_key_at(ki);
            }
        }
    }
    if (has_event(cold->on_text_action, cold->on_text_handler) && (cold->global_keys || etch_focused(id))) {
        if (etch_focused(id)) {
            etch_request_text_input(id, etch_element_bounds(id));
        }
        const uint32_t tid = etch_focused(id) ? id : 0u;
        const EtchStr typed = etch_typed_text(tid);
        if (typed.length > 0u && typed.chars) {
            const EtchStr payload = input_json(s->etch, typed.chars, typed.length);
            if (payload.length > 0u) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, cold->on_text_action),
                                     cold->on_text_handler, payload);
            }
        }
    }
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
        dispatch_dnd_and_keys(s, cold, id, events);
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
        case INLAY_UI_PORTAL:
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
    auto* e = ext(s);
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
        auto* eb = static_cast<TextSlice*>(SDL_realloc(e->handler_eb_keys, size_t(cap) * sizeof(TextSlice)));
        if (!eb) {
            JS_ThrowInternalError(s->js, "UI handler allocation failed");
            return INLAY_UI_NONE;
        }
        e->handler_eb_keys = eb;
        s->handler_capacity = cap;
    }
    const uint32_t idx = s->handler_count++;
    s->handlers[idx] = JS_DupValue(s->js, fn);
    e->handler_eb_keys[idx] = e->cur_eb_key;
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
    const uint32_t max_payload = s->desc.max_state_bytes ? s->desc.max_state_bytes : 65536u;
    if ((!has_name && !has_handler) || (!is_click && !payload.length) || payload.length > max_payload) return;
    auto* event = static_cast<WidgetEvent*>(
        etch_frame_arena_alloc(s->etch, sizeof(WidgetEvent), alignof(WidgetEvent)));
    if (!event) return;
    EtchStr copy = {};
    if (has_name) {
        copy = inlay_ui_frame_text(s, name);
        if (!copy.chars) return;
    }
    *event = {nullptr, copy, payload, handler, is_click};
    if (ext(s)->draw_eb_key.len > 0u && ext(s)->event_eb_tag_count < kMaxEventBoundaryTags) {
        ext(s)->event_eb_tags[ext(s)->event_eb_tag_count++] = {event, ext(s)->draw_eb_key};
    }
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
    const CoreNodeExtra* extra = (&st >= s->styles && &st < s->styles + s->node_count)
        ? get_core_extra(s, static_cast<uint32_t>(&st - s->styles))
        : nullptr;
    if (extra && (extra->flags & CefWindowDragPassthrough)) {
        box.window_drag_passthrough = true;
    }
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
    const uint32_t prev_draw_path = ext(s)->draw_path_id;
    const uint32_t cur_draw_path = etch_id_index(prev_draw_path, n.id);
    ext(s)->draw_path_id = cur_draw_path;
    if ((st.flags2 & kNf2Container) != 0u) {
        observe_container_width(s, cur_draw_path, etch_element_bounds(id).w);
    }
    const TextSlice prev_draw_eb = ext(s)->draw_eb_key;
    if (ext(s)->boundary_root_count > 0u) {
        const auto* e = ext(s);
        for (uint32_t bi = 0u; bi < e->boundary_root_count; ++bi) {
            if (e->boundary_roots[bi].node_idx == index) {
                ext(s)->draw_eb_key = e->boundary_roots[bi].eb_key;
                break;
            }
        }
    }
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
                    uint32_t first_invalid_id = 0u;
                    EtchStr first_invalid_name = {};
                    EtchStr first_invalid_err = {};
                    uint32_t invalid_count = 0u;
                    for (uint32_t fi = 0u; fi < form_ctx.field_count; ++fi) {
                        if (!form_ctx.fields[fi].valid) {
                            if (first_invalid_id == 0u) {
                                first_invalid_id = form_ctx.fields[fi].element_id;
                                first_invalid_name = form_ctx.fields[fi].name;
                                first_invalid_err = form_ctx.fields[fi].error;
                            }
                            ++invalid_count;
                            EtchA11yOverrideDesc inv_ov = {};
                            inv_ov.element_id = form_ctx.fields[fi].element_id;
                            inv_ov.add_states = uint32_t(ETCH_A11Y_STATE_INVALID);
                            etch_set_a11y_override(&inv_ov);
                        }
                    }
                    if (first_invalid_id != 0u) {
                        etch_request_focus(first_invalid_id);
                        char ann_buf[256] = {};
                        int ann_len = 0;
                        if (first_invalid_err.length > 0u) {
                            ann_len = std::snprintf(
                                ann_buf, sizeof(ann_buf), "Form validation failed (%u): %.*s: %.*s",
                                invalid_count,
                                int(first_invalid_name.length),
                                first_invalid_name.chars ? first_invalid_name.chars : "",
                                int(first_invalid_err.length),
                                first_invalid_err.chars ? first_invalid_err.chars : "");
                        } else {
                            ann_len = std::snprintf(
                                ann_buf, sizeof(ann_buf), "Form validation failed (%u): %.*s is invalid",
                                invalid_count,
                                int(first_invalid_name.length),
                                first_invalid_name.chars ? first_invalid_name.chars : "");
                        }
                        if (ann_len > 0) {
                            const uint32_t clamped = ann_len < int(sizeof(ann_buf))
                                ? static_cast<uint32_t>(ann_len)
                                : static_cast<uint32_t>(sizeof(ann_buf) - 1u);
                            etch_a11y_announce({ann_buf, clamped, false}, ETCH_A11Y_LIVE_ASSERTIVE, s->etch, nullptr);
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
            const CodeEditorRichSpec* espec = (st.ext_idx != INLAY_UI_NONE && st.ext_idx < s->editor_rich_spec_count)
                ? &s->editor_rich_specs[st.ext_idx] : nullptr;
            struct DecoCtx { InlaySurface* s; const CodeEditorRichSpec* spec; EtchTheme theme; };
            if (s->code_tok_fn) {
                editor.tokenizer = s->code_tok_fn;
                editor.tokenizer_user_data = s->code_tok_user_data;
            }
            if (espec) {
                editor.auto_height = espec->auto_height;
                editor.show_overview_ruler = espec->show_overview_ruler;
                editor.show_minimap = espec->show_minimap;
                if (espec->has_show_line_numbers) editor.show_line_numbers = espec->show_line_numbers;
                if (espec->language.len > 0u) editor.language = inlay_ui_frame_text(s, inlay_ui_slice(s, espec->language));
                if (espec->has_highlight_brackets) editor.highlight_matching_brackets = espec->highlight_brackets;
                if (espec->has_indent_guides) editor.show_indent_guides = espec->indent_guides;
                if (espec->has_diag_tooltips) editor.show_diagnostic_tooltips = espec->diag_tooltips;
                if (espec->gutter_tag_width > 0.f) editor.gutter_tag_width_px = espec->gutter_tag_width;
                if (espec->sticky_header.len > 0u) {
                    editor.show_sticky_header = true;
                    editor.sticky_header_text = inlay_ui_frame_text(s, inlay_ui_slice(s, espec->sticky_header));
                }
                if (espec->deco_cnt > 0u) {
                    auto* dctx = static_cast<DecoCtx*>(etch_frame_arena_alloc(s->etch, sizeof(DecoCtx), alignof(DecoCtx)));
                    if (dctx) {
                        *dctx = {s, espec, theme};
                        editor.line_decorator = [](uint32_t line_idx, EtchCodeLineDecoration* out_dec, void* user) -> bool {
                            const auto* dc = static_cast<const DecoCtx*>(user);
                            const auto* sp = dc->spec;
                            for (uint32_t k = 0u; k < sp->deco_cnt; ++k) {
                                const auto& d = dc->s->editor_line_decos[sp->deco_off + k];
                                if (d.line != line_idx) continue;
                                if (d.row_bg.set) out_dec->row_bg = inlay_ui_resolve_color(d.row_bg, dc->theme, EtchColor{});
                                if (d.gutter_bar.set) out_dec->gutter_bar_color = inlay_ui_resolve_color(d.gutter_bar, dc->theme, EtchColor{});
                                if (d.gutter_tag.len > 0u) out_dec->gutter_tag = inlay_ui_frame_text(dc->s, inlay_ui_slice(dc->s, d.gutter_tag));
                                if (d.gutter_tag_color.set) out_dec->gutter_tag_color = inlay_ui_resolve_color(d.gutter_tag_color, dc->theme, dc->theme.text_muted);
                                return true;
                            }
                            return false;
                        };
                        editor.line_decorator_user_data = dctx;
                    }
                }
                if (espec->hint_cnt > 0u && espec->hint_off + espec->hint_cnt <= s->editor_hint_count) {
                    auto* arr = static_cast<EtchCodeInlayHint*>(etch_frame_arena_alloc(
                        s->etch, size_t(espec->hint_cnt) * sizeof(EtchCodeInlayHint), alignof(EtchCodeInlayHint)));
                    if (arr) {
                        for (uint32_t k = 0u; k < espec->hint_cnt; ++k) {
                            const auto& h = s->editor_hints[espec->hint_off + k];
                            arr[k] = {
                                h.line, h.col, inlay_ui_frame_text(s, inlay_ui_slice(s, h.text)),
                                h.color.set ? inlay_ui_resolve_color(h.color, theme, theme.text_muted) : EtchColor{},
                                h.bg.set ? inlay_ui_resolve_color(h.bg, theme, EtchColor{}) : EtchColor{},
                                h.border_color.set ? inlay_ui_resolve_color(h.border_color, theme, EtchColor{}) : EtchColor{},
                                h.radius
                            };
                        }
                        etch_code_editor_set_inlay_hints(id, arr, espec->hint_cnt);
                    }
                }
                if (espec->hl_cnt > 0u && espec->hl_off + espec->hl_cnt <= s->editor_inline_hl_count) {
                    auto* arr = static_cast<EtchCodeInlineHighlight*>(etch_frame_arena_alloc(
                        s->etch, size_t(espec->hl_cnt) * sizeof(EtchCodeInlineHighlight), alignof(EtchCodeInlineHighlight)));
                    if (arr) {
                        for (uint32_t k = 0u; k < espec->hl_cnt; ++k) {
                            const auto& h = s->editor_inline_hls[espec->hl_off + k];
                            arr[k] = {
                                h.line, h.start_col, h.end_col,
                                h.bg.set ? inlay_ui_resolve_color(h.bg, theme, EtchColor{}) : EtchColor{},
                                h.border_color.set ? inlay_ui_resolve_color(h.border_color, theme, EtchColor{}) : EtchColor{},
                                h.radius
                            };
                        }
                        etch_code_editor_set_inline_highlights(id, arr, espec->hl_cnt);
                    }
                }
                if (espec->marker_cnt > 0u && espec->marker_off + espec->marker_cnt <= s->editor_marker_count) {
                    auto* arr = static_cast<EtchCodeOverviewMarker*>(etch_frame_arena_alloc(
                        s->etch, size_t(espec->marker_cnt) * sizeof(EtchCodeOverviewMarker), alignof(EtchCodeOverviewMarker)));
                    if (arr) {
                        for (uint32_t k = 0u; k < espec->marker_cnt; ++k) {
                            const auto& m = s->editor_markers[espec->marker_off + k];
                            arr[k] = {
                                m.line,
                                m.color.set ? inlay_ui_resolve_color(m.color, theme, theme.accent) : theme.accent,
                                m.lane
                            };
                        }
                        etch_code_editor_set_overview_markers(id, arr, espec->marker_cnt);
                    }
                }
                for (uint32_t k = 0u; k < espec->fold_cnt && espec->fold_off + k < s->editor_fold_count; ++k) {
                    const auto& f = s->editor_folds[espec->fold_off + k];
                    etch_code_editor_set_fold(id, f.start, f.end, f.collapsed);
                }
                if (espec->vz_cnt > 0u && espec->vz_off + espec->vz_cnt <= s->editor_view_zone_count) {
                    auto* arr = static_cast<EtchCodeViewZone*>(etch_frame_arena_alloc(
                        s->etch, size_t(espec->vz_cnt) * sizeof(EtchCodeViewZone), alignof(EtchCodeViewZone)));
                    if (arr) {
                        for (uint32_t k = 0u; k < espec->vz_cnt; ++k) {
                            const auto& z = s->editor_view_zones[espec->vz_off + k];
                            arr[k] = {
                                z.id, z.after_line, z.height, z.above_line,
                                z.fill_color.set ? inlay_ui_resolve_color(z.fill_color, theme, EtchColor{}) : EtchColor{}
                            };
                        }
                        etch_code_editor_set_view_zones(id, arr, espec->vz_cnt);
                    }
                }
                if (espec->has_scroll_x || espec->has_scroll_y) {
                    EditorScrollSlot* es = nullptr;
                    for (uint32_t si = 0u; si < s->editor_scroll_state_count; ++si) {
                        if (s->editor_scroll_states[si].element_id == id) { es = &s->editor_scroll_states[si]; break; }
                    }
                    if (!es && inlay_grow_pod(s->editor_scroll_states, s->editor_scroll_state_cap, s->editor_scroll_state_count + 1u, 8u, 64u)) {
                        es = &s->editor_scroll_states[s->editor_scroll_state_count++];
                        *es = {id, 0.f, 0.f, 0.f, 0.f, false, false, 0u, 0u, false, s->frame_counter};
                    }
                    if (es) {
                        es->last_used_frame = s->frame_counter;
                        const bool px_chg = espec->has_scroll_x && (!es->had_prop_x || std::fabs(es->applied_prop_x - espec->scroll_x) > 0.5f);
                        const bool py_chg = espec->has_scroll_y && (!es->had_prop_y || std::fabs(es->applied_prop_y - espec->scroll_y) > 0.5f);
                        if (px_chg || py_chg) {
                            EtchVec2 cur_off = etch_scroll_offset(id);
                            if (px_chg) { cur_off.x = espec->scroll_x; es->applied_prop_x = espec->scroll_x; es->had_prop_x = true; }
                            if (py_chg) { cur_off.y = espec->scroll_y; es->applied_prop_y = espec->scroll_y; es->had_prop_y = true; }
                            etch_code_editor_set_scroll_offset(id, cur_off, true);
                        }
                    }
                }
            }
            const auto edit = etch_code_editor(&editor);
            // Render declarative <ViewZone zoneId={...}> children inside editor view zones
            for (uint32_t at = n.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                if (s->nodes[at].kind != INLAY_UI_VIEW_ZONE) continue;
                const uint32_t zid = static_cast<uint32_t>(s->styles[at].int_value);
                if (zid != 0u && etch_code_editor_begin_view_zone(id, zid)) {
                    const bool prev_col = s->draw_parent_col;
                    s->draw_parent_col = true;
                    for (uint32_t ch = s->nodes[at].first_child; ch != INLAY_UI_NONE; ch = s->nodes[ch].next_sibling) {
                        inlay_ui_draw_node(s, ch, id, theme, events);
                    }
                    s->draw_parent_col = prev_col;
                    etch_code_editor_end_view_zone();
                }
            }
            if (edit.changed || etch_clicked(id)) ext(s)->child_consumed_mask |= ConsumeClick;
            if (edit.changed && has_event(st.action, st.handler)) {
                char payload[128];
                const int len = std::snprintf(payload, sizeof(payload),
                    "{\"cursorByte\":%u,\"anchorByte\":%u,\"byteLength\":%u,\"lineCount\":%u}",
                    edit.cursor_byte, edit.selection_anchor_byte, etch_text_buffer_byte_length(editor.buffer), edit.total_lines);
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, st.action), st.handler,
                                     inlay_ui_frame_text(s, {payload, uint32_t(len), false}));
            }
            if (espec && events) {
                if (edit.clicked_gutter_line >= 0 && has_event(espec->on_gutter_click_action, espec->on_gutter_click_handler)) {
                    char payload[48];
                    const int len = std::snprintf(payload, sizeof(payload), "{\"line\":%d}", edit.clicked_gutter_line);
                    if (len > 0) inlay_ui_queue_event(s, events, inlay_ui_slice(s, espec->on_gutter_click_action),
                                                      espec->on_gutter_click_handler,
                                                      inlay_ui_frame_text(s, {payload, uint32_t(len), false}), true);
                }
                if (edit.gutter_drag_start_line >= 0 && edit.gutter_drag_end_line >= 0 &&
                    has_event(espec->on_gutter_range_action, espec->on_gutter_range_handler)) {
                    char payload[96];
                    const int len = std::snprintf(payload, sizeof(payload),
                        "{\"startLine\":%d,\"endLine\":%d,\"dragging\":%s}",
                        edit.gutter_drag_start_line, edit.gutter_drag_end_line,
                        edit.gutter_dragging ? "true" : "false");
                    if (len > 0) inlay_ui_queue_event(s, events, inlay_ui_slice(s, espec->on_gutter_range_action),
                                                      espec->on_gutter_range_handler,
                                                      inlay_ui_frame_text(s, {payload, uint32_t(len), false}));
                }
                if (has_event(espec->on_scroll_action, espec->on_scroll_handler)) {
                    const EtchVec2 so = etch_scroll_offset(id);
                    EditorScrollSlot* es = nullptr;
                    for (uint32_t si = 0u; si < s->editor_scroll_state_count; ++si) {
                        if (s->editor_scroll_states[si].element_id == id) { es = &s->editor_scroll_states[si]; break; }
                    }
                    if (!es && inlay_grow_pod(s->editor_scroll_states, s->editor_scroll_state_cap, s->editor_scroll_state_count + 1u, 8u, 64u)) {
                        es = &s->editor_scroll_states[s->editor_scroll_state_count++];
                        *es = {id, so.x, so.y, 0.f, 0.f, false, false, 0u, 0u, false, s->frame_counter};
                    }
                    if (es) {
                        es->last_used_frame = s->frame_counter;
                        if (std::fabs(so.x - es->last_x) > 0.5f || std::fabs(so.y - es->last_y) > 0.5f) {
                            es->last_x = so.x; es->last_y = so.y;
                            char payload[64];
                            const int len = std::snprintf(payload, sizeof(payload), "{\"x\":%.2f,\"y\":%.2f}", double(so.x), double(so.y));
                            if (len > 0) inlay_ui_queue_event(s, events, inlay_ui_slice(s, espec->on_scroll_action),
                                                              espec->on_scroll_handler,
                                                              inlay_ui_frame_text(s, {payload, uint32_t(len), false}));
                        }
                    }
                }
                if (has_event(espec->on_cursor_action, espec->on_cursor_handler)) {
                    EditorScrollSlot* es = nullptr;
                    for (uint32_t si = 0u; si < s->editor_scroll_state_count; ++si) {
                        if (s->editor_scroll_states[si].element_id == id) { es = &s->editor_scroll_states[si]; break; }
                    }
                    if (!es && inlay_grow_pod(s->editor_scroll_states, s->editor_scroll_state_cap, s->editor_scroll_state_count + 1u, 8u, 64u)) {
                        es = &s->editor_scroll_states[s->editor_scroll_state_count++];
                        *es = {id, 0.f, 0.f, 0.f, 0.f, false, false, 0u, 0u, false, s->frame_counter};
                    }
                    if (es) {
                        es->last_used_frame = s->frame_counter;
                        if (!es->has_cursor_state || es->last_cursor_byte != edit.cursor_byte || es->last_anchor_byte != edit.selection_anchor_byte) {
                            const bool had_prev = es->has_cursor_state;
                            es->has_cursor_state = true;
                            es->last_cursor_byte = edit.cursor_byte;
                            es->last_anchor_byte = edit.selection_anchor_byte;
                            if (had_prev) {
                                char payload[160];
                                const int len = std::snprintf(payload, sizeof(payload),
                                    "{\"cursorByte\":%u,\"anchorByte\":%u,\"line\":%u,\"col\":%u,\"cursorCount\":%u}",
                                    edit.cursor_byte, edit.selection_anchor_byte,
                                    edit.cursor_line, edit.cursor_col_byte, etch_code_editor_cursor_count(id));
                                if (len > 0) inlay_ui_queue_event(s, events, inlay_ui_slice(s, espec->on_cursor_action),
                                                                  espec->on_cursor_handler,
                                                                  inlay_ui_frame_text(s, {payload, uint32_t(len), false}));
                            }
                        }
                    }
                }
            }
            dispatch_dnd_and_keys(s, cold, id, events);
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
        input.auto_grow = extra && (extra->flags & CefInputAutoGrow);
        if (extra && (extra->flags & CefInputHasAutoGrowRange)) {
            if (extra->aux_f0 > 0.f) input.min_height_px = extra->aux_f0;
            if (extra->aux_f1 > 0.f) input.max_height_px = extra->aux_f1;
        }
        ActiveFormCtx* active_form = ext(s)->active_form;
        const bool has_own_submit = extra && has_event(extra->submit_action, extra->submit_handler);
        input.submit_on_enter = (extra && (extra->flags & CefInputHasSubmitEnter))
            ? ((extra->flags & CefInputSubmitEnterVal) != 0u)
            : (has_own_submit || (active_form != nullptr && !input.multiline));
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

        const InputRichSpec* ispec = (st.ext_idx != INLAY_UI_NONE && st.ext_idx < s->input_rich_spec_count)
            ? &s->input_rich_specs[st.ext_idx] : nullptr;
        if (ispec) {
            if (ispec->ghost_text.len > 0u) {
                input.ghost_text = inlay_ui_frame_text(s, inlay_ui_slice(s, ispec->ghost_text));
            }
            if (ispec->ghost_color.set) {
                input.ghost_color = inlay_ui_resolve_color(ispec->ghost_color, theme, theme.text_muted);
            }
            input.accept_ghost_on_tab = ispec->accept_ghost_on_tab;
            if (ispec->hl_count > 0u && ispec->hl_offset + ispec->hl_count <= s->input_highlight_count) {
                auto* hls = static_cast<EtchTextInputHighlight*>(etch_frame_arena_alloc(
                    s->etch, size_t(ispec->hl_count) * sizeof(EtchTextInputHighlight), alignof(EtchTextInputHighlight)));
                if (hls) {
                    for (uint32_t hi = 0u; hi < ispec->hl_count; ++hi) {
                        const auto& h = s->input_highlights[ispec->hl_offset + hi];
                        hls[hi] = {
                            h.offset, h.length,
                            h.color.set ? inlay_ui_resolve_color(h.color, theme, theme.accent) : EtchColor{},
                            h.bg.set ? inlay_ui_resolve_color(h.bg, theme, EtchColor{}) : EtchColor{},
                            h.border_color.set ? inlay_ui_resolve_color(h.border_color, theme, EtchColor{}) : EtchColor{},
                            h.radius, h.atomic
                        };
                    }
                    input.highlights = hls;
                    input.highlight_count = ispec->hl_count;
                }
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
            const EtchStr field_err = (!post_valid && post_err_str.length > 0u)
                ? inlay_ui_frame_text(s, post_err_str)
                : EtchStr{};
            active_form->fields[active_form->field_count++] = {id, field_key, field_val, field_err, post_valid};
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
        if (ispec && events) {
            if (edit.ghost_accepted && has_event(ispec->on_accept_ghost_action, ispec->on_accept_ghost_handler)) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, ispec->on_accept_ghost_action),
                                     ispec->on_accept_ghost_handler,
                                     input_json(s->etch, s->edit_buffer, edit.byte_length), true);
            }
            if (edit.pasted_image && has_event(ispec->on_paste_image_action, ispec->on_paste_image_handler)) {
                inlay_ui_queue_event(s, events, inlay_ui_slice(s, ispec->on_paste_image_action),
                                     ispec->on_paste_image_handler, ETCH_STR_LIT("true"), true);
            }
            if (has_event(ispec->on_caret_action, ispec->on_caret_handler) &&
                (edit.changed || etch_clicked(id) ||
                 (edit.focused && (etch_key_pressed(SDLK_LEFT, true) || etch_key_pressed(SDLK_RIGHT, true) ||
                                   etch_key_pressed(SDLK_UP, true) || etch_key_pressed(SDLK_DOWN, true))))) {
                char payload[128];
                const int len = std::snprintf(payload, sizeof(payload),
                    "{\"cursor\":%u,\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}",
                    edit.cursor_byte, double(edit.caret_rect.x), double(edit.caret_rect.y),
                    double(edit.caret_rect.w), double(edit.caret_rect.h));
                if (len > 0) {
                    inlay_ui_queue_event(s, events, inlay_ui_slice(s, ispec->on_caret_action),
                                         ispec->on_caret_handler,
                                         inlay_ui_frame_text(s, {payload, uint32_t(len), false}));
                }
            }
        }
        if (!disabled) dispatch_dnd_and_keys(s, cold, id, events);
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
        if (st.flags & NfHasBorderWidth) toggle.border_width = std::fmax(0.f, st.border_width);
        if ((st.flags2 & Nf2WExplicit) && st.w_kind == uint8_t(ETCH_SIZING_FIXED) && st.w_val > 0.f) {
            toggle.track_width_px = st.w_val;
        }
        if ((st.flags2 & Nf2HExplicit) && st.h_kind == uint8_t(ETCH_SIZING_FIXED) && st.h_val > 0.f) {
            toggle.track_height_px = st.h_val;
        }
        if (extra) {
            if (extra->flags & CefSwitchHasTrackW) toggle.track_width_px = extra->aux_f0;
            if (extra->flags & CefSwitchHasTrackH) toggle.track_height_px = extra->aux_f1;
            if (extra->flags & CefSwitchHasTrackR) toggle.track_radius = etch_corners_all(extra->aux_f2);
            if (extra->flags & CefSwitchHasThumbR) toggle.thumb_radius = etch_corners_all(extra->aux_f3);
            if (extra->flags & CefSwitchHasThumbPad) toggle.thumb_padding_px = extra->aux_f4;
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
        if (extra) {
            if (extra->flags & CefSplitHasDividerSz) sp.divider_size_px = extra->aux_f0;
            if (extra->flags & CefSplitHasDividerHit) sp.divider_hit_px = extra->aux_f1;
            if (extra->flags & CefSplitHideGrip) sp.show_grip = false;
        }

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
            if (extra && (extra->flags & CefIconHasSecondaryCol) && extra->color_a.set) {
                ic.secondary_color = inlay_ui_resolve_color(extra->color_a, theme, ic.color);
            }
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
    ext(s)->draw_eb_key = prev_draw_eb;
    ext(s)->draw_path_id = prev_draw_path;
}

// ============================================================================
// Public C ABI Surface Lifecycle & Rendering
// ============================================================================

extern "C" {
INLAY_API InlaySurface* inlay_surface_create(EtchContext* context, const InlaySurfaceDesc* desc) {
    InlaySurfaceDesc d = inlay_surface_desc_default();
    if (desc && !inlay_read_abi_struct(desc, &d)) return nullptr;
    if (d.max_text_bytes == 0u) d.max_text_bytes = kDefaultMaxTextBytes;
    if (d.max_state_bytes == 0u) d.max_state_bytes = 65536u;
    if (!context || d.memory_limit_bytes < 512u * 1024u ||
        d.memory_limit_bytes > 256u * 1024u * 1024u || d.max_stack_bytes < 16384u ||
        d.max_stack_bytes > 512u * 1024u || !std::isfinite(d.max_tick_ms) || d.max_tick_ms <= 0.f ||
        d.max_tick_ms > 50.f || !d.max_nodes || d.max_nodes > 65536u ||
        d.max_text_bytes > kHardMaxTextBytes ||
        d.max_state_bytes < 1024u || d.max_state_bytes > 4u * 1024u * 1024u) {
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
    s->json_scratch = static_cast<char*>(SDL_malloc(size_t(d.max_state_bytes) + 1u));
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
    storage->atom_eb_key = JS_NewAtom(s->js, "__ebKey");
    return s;
}

INLAY_API void inlay_surface_destroy(InlaySurface* s) {
    if (!s) return;
    auto* storage = ext(s);
    SDL_free(s->actions);
    SDL_free(s->content);
    SDL_free(s->terminals);
    SDL_free(s->streams);
    SDL_free(storage->command_names);
    for (uint32_t i = 0u; i < storage->ls_count; ++i) {
        SDL_free(storage->ls_entries[i].key);
        SDL_free(storage->ls_entries[i].value);
    }
    SDL_free(storage->ls_entries);
    for (uint32_t i = 0u; i < storage->module_count; ++i) {
        SDL_free(storage->modules[i].source);
    }
    SDL_free(storage->modules);
    clear_context(s);
    SDL_free(storage->action_policies);
    SDL_free(storage->async_actions);
    SDL_free(storage->async_action_names);
    SDL_free(storage->reload_error);
    SDL_free(storage->bootstrap_bc);
    if (s->runtime) JS_FreeRuntime(s->runtime);
    inlay_ui_free_widget_caches(s);
    SDL_free(s->handlers);
    SDL_free(storage->handler_eb_keys);
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
    if (!s || !json.chars || !json.length) return false;
    const uint32_t limit = s->desc.max_state_bytes ? s->desc.max_state_bytes : 65536u;
    if (json.length > limit) return false;
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
    if (!inlay_grow_pod(s->actions, s->action_cap, s->action_count + 1u, 16u, 512u)) return false;
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
    const bool ok = bind_content(s, name, INLAY_UI_VIRTUAL_LIST, model);
    if (ok && model && !model->bound_ctx) {
        model->bound_ctx = s->etch;
    }
    return ok;
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
    emit_content_union("PortalName", INLAY_UI_PORTAL);
    w.put("export type TerminalName = ");
    if (s->terminal_count == 0u) {
        w.put("never");
    } else {
        for (uint32_t i = 0u; i < s->terminal_count; ++i) {
            if (i > 0u) w.put(" | ");
            w.put_ts_str(s->terminals[i].name);
        }
    }
    w.put(";\n");
    w.put("export type StreamName = ");
    if (s->stream_count == 0u) {
        w.put("never");
    } else {
        for (uint32_t i = 0u; i < s->stream_count; ++i) {
            if (i > 0u) w.put(" | ");
            w.put_ts_str(s->streams[i].name);
        }
    }
    w.put(";\n");

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
        "  draggable?: boolean;\n"
        "  dragKind?: string;\n"
        "  dragLabel?: string;\n"
        "  dragData?: unknown;\n"
        "  dropAccept?: string;\n"
        "  globalKeys?: boolean;\n"
        "  consumeKeys?: boolean;\n"
        "  onClick?: ActionName | ((state: unknown) => void);\n"
        "  onDrop?: ActionName | ((ev: { kindHash: number; label: string; data: string }, state: unknown) => void);\n"
        "  onFileDrop?: ActionName | ((ev: { files: string[]; texts: string[] }, state: unknown) => void);\n"
        "  onDragHover?: ActionName | ((hovering: boolean, state: unknown) => void);\n"
        "  onKeyDown?: ActionName | ((ev: { key: string; keyCode: number; ctrl: boolean; shift: boolean; alt: boolean; meta: boolean; repeat: boolean }, state: unknown) => void);\n"
        "  onTextInput?: ActionName | ((text: string, state: unknown) => void);\n"
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
        "  autoGrow?: boolean;\n"
        "  minHeight?: number;\n"
        "  maxHeight?: number;\n"
        "  readOnly?: boolean;\n"
        "  highlights?: { offset: number; length: number; color?: string; bg?: string; borderColor?: string; radius?: number; atomic?: boolean }[];\n"
        "  ghostText?: string;\n"
        "  ghostColor?: string;\n"
        "  acceptGhostOnTab?: boolean;\n"
        "  onAcceptGhost?: ActionName | ((value: string, state: unknown) => void);\n"
        "  onPasteImage?: ActionName | ((ok: boolean, state: unknown) => void);\n"
        "  onCaret?: ActionName | ((ev: { cursor: number; x: number; y: number; w: number; h: number }, state: unknown) => void);\n"
        "  onChange?: ActionName | ((value: string, state: unknown) => void);\n"
        "  onSubmit?: ActionName | ((value: string, state: unknown) => void);\n"
        "}\n"
        "export interface SlotProps extends CommonProps { name: SlotName; props?: unknown; }\n"
        "export interface PortalProps extends CommonProps {\n"
        "  name: PortalName;\n"
        "  url?: string;\n"
        "  html?: string;\n"
        "  zoom?: number;\n"
        "  findQuery?: string;\n"
        "  findMatchCase?: boolean;\n"
        "  findForward?: boolean;\n"
        "  navAction?: string;\n"
        "  evalScript?: string;\n"
        "  initScript?: string;\n"
        "  postMessage?: string;\n"
        "  onStateChange?: ActionName | ((ev: { loading: boolean; canGoBack: boolean; canGoForward: boolean; zoom: number; title: string; url: string; faviconUrl: string; find: { active: boolean; activeMatch: number; totalMatches: number } }, state: unknown) => void);\n"
        "  onMessage?: ActionName | ((message: unknown, state: unknown) => void);\n"
        "  onNavigate?: ActionName | ((ev: { url: string }, state: unknown) => void);\n"
        "}\n"
        "export interface DocProps extends CommonProps {\n"
        "  document?: DocumentName;\n"
        "  markdown?: string;\n"
        "  streamingRepair?: boolean;\n"
        "  findQuery?: string;\n"
        "  onLinkClick?: ActionName | ((ev: { url: string; text: string; kind: string }, state: unknown) => void);\n"
        "  onSelectionChange?: ActionName | ((ev: { active: boolean; text: string; markdown: string; rect: { x: number; y: number; w: number; h: number } }, state: unknown) => void);\n"
        "  onFindResult?: ActionName | ((ev: { count: number; activeIndex: number }, state: unknown) => void);\n"
        "  onTaskToggle?: ActionName | ((ev: { blockIndex: number; checked: boolean }, state: unknown) => void);\n"
        "  onDiagramNodeClick?: ActionName | ((ev: { nodeId: string; blockIndex: number }, state: unknown) => void);\n"
        "}\n"
        "export interface TerminalProps extends CommonProps {\n"
        "  buffer?: TerminalName;\n"
        "  ansi?: string;\n"
        "  autoFit?: boolean;\n"
        "  focusable?: boolean;\n"
        "  interactiveSelection?: boolean;\n"
        "  palette?: string[];\n"
        "  captureInput?: boolean;\n"
        "  bracketedPaste?: boolean;\n"
        "  mouseReporting?: boolean;\n"
        "  appCursor?: boolean;\n"
        "  searchMatches?: { row: number; startCol: number; endCol: number; active?: boolean }[];\n"
        "  cursorShape?: 'underline' | 'block' | 'bar' | 'hollow';\n"
        "  cursorVisible?: boolean;\n"
        "  onCellClick?: ActionName | ((ev: { col: number; row: number; right: boolean }, state: unknown) => void);\n"
        "  onLinkClick?: ActionName | ((ev: { hyperlinkId: number; col: number; row: number }, state: unknown) => void);\n"
        "  onResize?: ActionName | ((ev: { cols: number; rows: number }, state: unknown) => void);\n"
        "  onSelectionChange?: ActionName | ((ev: { hasSelection: boolean; startCol: number; startRow: number; endCol: number; endRow: number }, state: unknown) => void);\n"
        "  onWheel?: ActionName | ((ev: { deltaY: number }, state: unknown) => void);\n"
        "  onInput?: ActionName | ((ev: { text: string; keyCode: number; mods: number; repeat: boolean; vt: string }, state: unknown) => void);\n"
        "  onPaste?: ActionName | ((ev: { text: string }, state: unknown) => void);\n"
        "  onMouse?: ActionName | ((ev: { button: number; col: number; row: number; down: boolean; released: boolean; moved: boolean; sgr: string }, state: unknown) => void);\n"
        "}\n"
        "export interface CodeEditorProps extends CommonProps {\n"
        "  buffer: BufferName;\n"
        "  language?: string;\n"
        "  highlightBrackets?: boolean;\n"
        "  showIndentGuides?: boolean;\n"
        "  showDiagnosticTooltips?: boolean;\n"
        "  readOnly?: boolean;\n"
        "  wordWrap?: boolean;\n"
        "  autoHeight?: boolean;\n"
        "  showOverviewRuler?: boolean;\n"
        "  showMinimap?: boolean;\n"
        "  showLineNumbers?: boolean;\n"
        "  stickyHeader?: string;\n"
        "  ghostText?: string;\n"
        "  searchQuery?: string;\n"
        "  inlayHints?: { line: number; col: number; text: string; color?: string; bg?: string; borderColor?: string; radius?: number }[];\n"
        "  inlineHighlights?: { line: number; startCol: number; endCol: number; bg?: string; borderColor?: string; radius?: number }[];\n"
        "  overviewMarkers?: { line: number; color?: string; lane?: number }[];\n"
        "  lineDecorations?: { line: number; rowBg?: string; gutterBar?: string; gutterTag?: string; gutterTagColor?: string }[];\n"
        "  folds?: { start: number; end: number; collapsed?: boolean }[];\n"
        "  viewZones?: { id: number; afterLine: number; height: number; aboveLine?: boolean; fillColor?: string }[];\n"
        "  scrollX?: number;\n"
        "  scrollY?: number;\n"
        "  onScroll?: ActionName | ((ev: { x: number; y: number }, state: unknown) => void);\n"
        "  onCursorChange?: ActionName | ((ev: { cursorByte: number; anchorByte: number; line: number; col: number; cursorCount: number }, state: unknown) => void);\n"
        "  onGutterClick?: ActionName | ((ev: { line: number }, state: unknown) => void);\n"
        "  onGutterRange?: ActionName | ((ev: { startLine: number; endLine: number; dragging: boolean }, state: unknown) => void);\n"
        "}\n"
        "export interface DiffViewProps extends CommonProps {\n"
        "  oldText: string;\n"
        "  newText: string;\n"
        "  language?: string;\n"
        "  filePath?: string;\n"
        "  mode?: 'split' | 'unified';\n"
        "  contextLines?: number;\n"
        "  collapseUnchanged?: boolean;\n"
        "  wordDiff?: boolean;\n"
        "  showOverviewRuler?: boolean;\n"
        "  showMinimap?: boolean;\n"
        "  showLineNumbers?: boolean;\n"
        "  editable?: boolean;\n"
        "  hunkActions?: { id: string; label: string }[];\n"
        "  annotations?: { line: number; side?: 'old' | 'new'; height?: number; author?: string; body: string; color?: string }[];\n"
        "  onGutterClick?: ActionName | ((ev: { line: number; side: string; origLine: number }, state: unknown) => void);\n"
        "  onGutterRange?: ActionName | ((ev: { startLine: number; endLine: number; dragging: boolean; side: string }, state: unknown) => void);\n"
        "  onChange?: ActionName | ((value: string, state: unknown) => void);\n"
        "  onHunkAction?: ActionName | ((ev: { hunkIndex: number; actionId: string; oldStart: number; oldCount: number; newStart: number; newCount: number }, state: unknown) => void);\n"
        "}\n"
        "export interface VirtualListProps extends CommonProps { model?: ModelName; count?: number; itemHeight?: number; items?: unknown[]; renderItem?: (index: number) => InlayNode; }\n"
        "export interface TreeProps extends CommonProps { model?: ModelName; showIndentGuide?: boolean; rowHeight?: number; indent?: number; }\n");

    w.put(
        "\n"
        "declare function action(name: ActionName, payload?: unknown): boolean;\n"
        "declare function request<T = unknown>(name: AsyncActionName, payload?: unknown): Promise<T> & { cancel(): boolean };\n"
        "declare function setTimeout(fn: () => void, ms?: number): number;\n"
        "declare function clearTimeout(id: number): void;\n"
        "declare function setInterval(fn: () => void, ms?: number): number;\n"
        "declare function clearInterval(id: number): void;\n"
        "declare function requestAnimationFrame(fn: (nowMs: number) => void): number;\n"
        "declare function cancelAnimationFrame(id: number): void;\n"
        "declare const console: { debug(...args: unknown[]): void; log(...args: unknown[]): void; info(...args: unknown[]): void; warn(...args: unknown[]): void; error(...args: unknown[]): void };\n"
        "declare function fuzzyMatch(pattern: string, candidate: string): { matched: boolean; score: number; positions: number[] };\n"
        "declare function clipboardWrite(text: string): boolean;\n"
        "declare function clipboardRead(): string;\n"
        "declare function clipboardReadImage(): { mime: string; sizeBytes: number; base64: string } | null;\n"
        "declare function pushFocusRestorePoint(): boolean;\n"
        "declare function popFocusRestorePoint(): number;\n"
        "declare function setUiZoom(zoom: number): number;\n"
        "declare function getUiZoom(): number;\n"
        "declare function focusElement(id: string | number): boolean;\n"
        "declare function blurElement(): void;\n"
        "declare function scrollTo(id: string | number, x: number, y: number): boolean;\n"
        "declare function scrollToBottom(id: string | number): boolean;\n"
        "declare function scrollIntoView(scrollId: string | number, itemId: string | number, margin?: number): boolean;\n"
        "declare function measureElement(id: string | number): { x: number; y: number; w: number; h: number; visible: boolean; visibleX: number; visibleY: number; visibleW: number; visibleH: number; opacity: number } | null;\n"
        "declare function openUrl(url: string): boolean;\n"
        "declare function notifyDesktop(title: string, body?: string, opts?: { tag?: number; silent?: boolean }): boolean;\n"
        "declare function openFileDialog(opts?: { defaultLocation?: string; multiple?: boolean; filters?: { name: string; pattern: string }[] }): Promise<{ cancelled: boolean; error: boolean; selectedFilter?: number; paths: string[] }>;\n"
        "declare function openFolderDialog(opts?: { defaultLocation?: string; multiple?: boolean }): Promise<{ cancelled: boolean; error: boolean; paths: string[] }>;\n"
        "declare function saveFileDialog(opts?: { defaultLocation?: string; filters?: { name: string; pattern: string }[] }): Promise<{ cancelled: boolean; error: boolean; selectedFilter?: number; paths: string[] }>;\n"
        "declare function registerCommand(desc: { id: string | number; title?: string; name?: string; category?: string; shortcut?: string; chord?: string; enabled?: boolean; allowWhenTyping?: boolean; scope?: string | number }): boolean;\n"
        "declare function unregisterCommand(id: string | number): boolean;\n"
        "declare function dispatchCommand(id: string | number): boolean;\n"
        "declare function pushCommandScope(id: string | number): void;\n"
        "declare function popCommandScope(): void;\n"
        "declare function readStream(name: StreamName, sinceCursor?: number): { cursor: number; items: unknown[] } | null;\n"
        "declare function saveDockLayout(id: string | number): string;\n"
        "declare function loadDockLayout(id: string | number, json: string): boolean;\n"
        "declare function detachDockPane(dockId: string | number, paneId: string | number): boolean;\n"
        "declare function encodeTerminalKey(keyCode: number, mods?: number, appCursor?: boolean): string;\n"
        "declare function encodeTerminalMouseSgr(button: number, col: number, row: number, mods: number, pressed: boolean, motion: boolean): string;\n"
        "declare const performance: { now(): number };\n"
        "declare const crypto: { randomUUID(): string };\n"
        "declare function btoa(data: string): string;\n"
        "declare function atob(data: string): string;\n"
        "declare function queueMicrotask(fn: () => void): void;\n"
        "declare function structuredClone<T>(value: T): T;\n"
        "declare const localStorage: { readonly length: number; getItem(key: string): string | null; setItem(key: string, value: string): void; removeItem(key: string): void; clear(): void; key(index: number): string | null };\n"
        "declare class TextEncoder { readonly encoding: string; encode(input?: string): Uint8Array; encodeInto(source: string, dest: Uint8Array): { read: number; written: number }; }\n"
        "declare class TextDecoder { readonly encoding: string; constructor(label?: string); decode(input?: ArrayBuffer | ArrayBufferView): string; }\n"
        "declare class URLSearchParams implements Iterable<[string, string]> { readonly size: number; constructor(init?: string | string[][] | Record<string, string> | URLSearchParams); append(name: string, value: string): void; delete(name: string): void; get(name: string): string | null; getAll(name: string): string[]; has(name: string): boolean; set(name: string, value: string): void; sort(): void; entries(): IterableIterator<[string, string]>; keys(): IterableIterator<string>; values(): IterableIterator<string>; forEach(cb: (value: string, key: string, parent: URLSearchParams) => void): void; [Symbol.iterator](): IterableIterator<[string, string]>; toString(): string; }\n"
        "declare class URL { protocol: string; username: string; password: string; hostname: string; port: string; host: string; readonly origin: string; pathname: string; search: string; readonly searchParams: URLSearchParams; hash: string; readonly href: string; constructor(url: string, base?: string | URL); toString(): string; toJSON(): string; }\n"
        "declare function useState<T>(init: T | (() => T)): [T, (next: T | ((prev: T) => T)) => void];\n"
        "declare function useReducer<S, A>(reducer: (state: S, action: A) => S, initialArg: S): [S, (action: A) => void];\n"
        "declare function useReducer<S, I, A>(reducer: (state: S, action: A) => S, initialArg: I, init: (arg: I) => S): [S, (action: A) => void];\n"
        "export interface Context<T> { defaultValue: T; Provider: (props: { value: T; render?: (v: T) => InlayNode; children?: ((v: T) => InlayNode) | InlayNode }) => InlayNode; }\n"
        "declare function createContext<T>(defaultValue: T): Context<T>;\n"
        "declare function useContext<T>(ctx: Context<T>): T;\n"
        "declare function provideContext<T>(ctx: Context<T>, value: T): void;\n"
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
        "  function portal(props?: PortalName | PortalProps): InlayNode;\n"
        "  function doc(props?: string | DocProps): InlayNode;\n"
        "  function codeEditor(props?: BufferName | CodeEditorProps, ...children: InlayNode[]): InlayNode;\n"
        "  function terminal(props?: string | TerminalProps): InlayNode;\n"
        "  function dockSpace(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function dockPane(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function sheet(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function hoverCard(props?: CommonProps | InlayNode, ...children: InlayNode[]): InlayNode;\n"
        "  function radioGroup(props?: CommonProps): InlayNode;\n"
        "  function toggleGroup(props?: CommonProps): InlayNode;\n"
        "  function colorPicker(props?: CommonProps): InlayNode;\n"
        "  function chart(props?: CommonProps): InlayNode;\n"
        "  function viewZone(props?: { zoneId: number }, ...children: InlayNode[]): InlayNode;\n"
        "  function diffView(props?: DiffViewProps): InlayNode;\n"
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
        "    Portal: PortalProps;\n"
        "    Doc: DocProps;\n"
        "    CodeEditor: CodeEditorProps;\n"
        "    Terminal: TerminalProps;\n"
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
        "    DiffView: DiffViewProps;\n"
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

INLAY_API bool inlay_surface_bind_terminal(InlaySurface* s, EtchStr name, InlayTerminalReadFn read_fn, void* user_data) {
    if (!s || s->running || s->drawing || !name.chars || !name.length ||
        name.length >= sizeof(TerminalBinding::name) || std::memchr(name.chars, 0, name.length)) {
        return false;
    }
    uint32_t found = s->terminal_count;
    for (uint32_t i = 0u; i < s->terminal_count; ++i) {
        if (inlay_ui_equal(name, s->terminals[i].name)) {
            found = i;
            break;
        }
    }
    if (!read_fn) {
        if (found < s->terminal_count) {
            s->terminals[found] = s->terminals[--s->terminal_count];
            etch_request_redraw(s->etch);
        }
        return true;
    }
    if (found == s->terminal_count) {
        if (!inlay_grow_pod(s->terminals, s->terminal_cap, s->terminal_count + 1u, 4u, 64u)) return false;
        ++s->terminal_count;
    }
    auto& dst = s->terminals[found];
    dst = {};
    std::memcpy(dst.name, name.chars, name.length);
    dst.name[name.length] = '\0';
    dst.read_fn = read_fn;
    dst.user = user_data;
    etch_request_redraw(s->etch);
    return true;
}

INLAY_API bool inlay_surface_bind_portal(InlaySurface* s, EtchStr name, InlayPortal* portal) {
    if (portal && (!s || inlay_portal_context(portal) != s->etch)) return false;
    return bind_content(s, name, INLAY_UI_PORTAL, portal);
}

INLAY_API bool inlay_surface_bind_stream(InlaySurface* s, EtchStr name, InlayStream* stream) {
    if (!s || s->running || s->drawing || !name.chars || !name.length ||
        name.length >= sizeof(StreamBinding::name) || std::memchr(name.chars, 0, name.length)) {
        return false;
    }
    uint32_t found = s->stream_count;
    for (uint32_t i = 0u; i < s->stream_count; ++i) {
        if (inlay_ui_equal(name, s->streams[i].name)) {
            found = i;
            break;
        }
    }
    if (!stream) {
        if (found < s->stream_count) {
            s->streams[found] = s->streams[--s->stream_count];
            etch_request_redraw(s->etch);
        }
        return true;
    }
    if (found == s->stream_count) {
        if (!inlay_grow_pod(s->streams, s->stream_cap, s->stream_count + 1u, 4u, 64u)) return false;
        ++s->stream_count;
    }
    auto& dst = s->streams[found];
    dst = {};
    std::memcpy(dst.name, name.chars, name.length);
    dst.name[name.length] = '\0';
    dst.stream = stream;
    dst.last_seen_cursor = UINT64_MAX;
    etch_request_redraw(s->etch);
    return true;
}

INLAY_API void inlay_surface_set_console_callback(InlaySurface* s, InlayConsoleFn fn, void* user_data) {
    if (!s) return;
    s->console_fn = fn;
    s->console_user_data = user_data;
}

INLAY_API void inlay_surface_set_storage_hooks(InlaySurface* s, InlayStorageGetFn get_fn,
                                               InlayStorageSetFn set_fn, InlayStorageRemoveFn remove_fn,
                                               void* user_data) {
    if (!s) return;
    auto* stg = ext(s);
    stg->storage_get_fn = get_fn;
    stg->storage_set_fn = set_fn;
    stg->storage_remove_fn = remove_fn;
    stg->storage_user = user_data;
}

INLAY_API void inlay_surface_set_code_tokenizer(InlaySurface* s, EtchSyntaxTokenizeFn fn, void* user_data) {
    if (!s) return;
    s->code_tok_fn = fn;
    s->code_tok_user_data = user_data;
    etch_request_redraw(s->etch);
}

INLAY_API void inlay_surface_set_doc_hooks(InlaySurface* s, InlayCodeHighlightFn hl_fn, void* hl_user,
                                           InlayCustomFenceEstimateFn fence_est, InlayCustomFenceRenderFn fence_ren,
                                           void* fence_user) {
    if (!s) return;
    s->doc_hl_fn = hl_fn;
    s->doc_hl_user_data = hl_user;
    s->doc_fence_est = fence_est;
    s->doc_fence_ren = fence_ren;
    s->doc_fence_user_data = fence_user;
    etch_request_redraw(s->etch);
}

INLAY_API bool inlay_surface_register_module(InlaySurface* s, EtchStr module_name, EtchStr source_utf8) {
    if (!s || s->running || s->drawing || !module_name.chars || !module_name.length ||
        module_name.length >= sizeof(RegisteredModule::name) ||
        std::memchr(module_name.chars, 0, module_name.length) ||
        (!source_utf8.chars && source_utf8.length > 0u) ||
        source_utf8.length > kMaxScriptBytes ||
        (source_utf8.length > 0u && std::memchr(source_utf8.chars, 0, source_utf8.length))) {
        return false;
    }
    auto* stg = ext(s);
    uint32_t found = stg->module_count;
    for (uint32_t i = 0u; i < stg->module_count; ++i) {
        if (inlay_ui_equal(module_name, stg->modules[i].name)) {
            found = i;
            break;
        }
    }
    char* copy = static_cast<char*>(SDL_malloc(size_t(source_utf8.length) + 1u));
    if (!copy) return false;
    if (source_utf8.length > 0u) {
        std::memcpy(copy, source_utf8.chars, source_utf8.length);
    }
    copy[source_utf8.length] = '\0';
    if (found == stg->module_count) {
        if (!inlay_grow_pod(stg->modules, stg->module_cap, stg->module_count + 1u, 8u, 256u)) {
            SDL_free(copy);
            return false;
        }
        ++stg->module_count;
        auto& dst = stg->modules[found];
        dst = {};
        std::memcpy(dst.name, module_name.chars, module_name.length);
        dst.name[module_name.length] = '\0';
        dst.source = copy;
        dst.length = source_utf8.length;
    } else {
        SDL_free(stg->modules[found].source);
        stg->modules[found].source = copy;
        stg->modules[found].length = source_utf8.length;
    }
    return true;
}

INLAY_API bool inlay_surface_unregister_module(InlaySurface* s, EtchStr module_name) {
    if (!s || s->running || s->drawing || !module_name.chars || !module_name.length ||
        module_name.length >= sizeof(RegisteredModule::name) ||
        std::memchr(module_name.chars, 0, module_name.length)) {
        return false;
    }
    auto* stg = ext(s);
    for (uint32_t i = 0u; i < stg->module_count; ++i) {
        if (inlay_ui_equal(module_name, stg->modules[i].name)) {
            SDL_free(stg->modules[i].source);
            stg->modules[i] = stg->modules[--stg->module_count];
            return true;
        }
    }
    return false;
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
        name.length >= 64u || !payload.chars || !payload.length) {
        return false;
    }
    const uint32_t limit = s->desc.max_state_bytes ? s->desc.max_state_bytes : 65536u;
    if (payload.length > limit) return false;
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
    ext(s)->last_view_id = input.id;

    // Bug 7 fix: throttle auto-polling of SDL_GetPathInfo inside view to 80 ms.
    const double current = inlay_ui_now_ms(s);
    if (s->path[0] && (current - ext(s)->last_hmr_check_ms) >= kHmrViewPollIntervalMs) {
        inlay_surface_poll_hmr(s);
    }
    if (s->pending_state && s->loaded && !s->stats.has_error) run_update(s, {}, {});

    // Reactive bound InlayStream change detection (auto-rebuilds when host appends stream items).
    if (s->stream_count > 0u && s->loaded && !s->stats.has_error) {
        bool stream_changed = false;
        for (uint32_t si = 0u; si < s->stream_count; ++si) {
            auto& sb = s->streams[si];
            if (!sb.stream || sb.last_seen_cursor == UINT64_MAX) continue;
            const uint64_t cur = inlay_stream_cursor(sb.stream);
            if (cur != sb.last_seen_cursor) {
                sb.last_seen_cursor = cur;
                stream_changed = true;
            }
        }
        if (stream_changed) run_update(s, {}, {});
    }

    // Async native file/folder/save dialog completion bridge.
    if (s->loaded && !s->stats.has_error) {
        bool any_dialog_pending = false;
        for (const auto& pd : ext(s)->pending_dialogs) {
            if (pd.active) { any_dialog_pending = true; break; }
        }
        if (any_dialog_pending) {
            char dscratch[8192];
            EtchArena darena = etch_arena_from_buffer(dscratch, sizeof(dscratch));
            const EtchFileDialogResult dres = etch_poll_file_dialog(s->etch, &darena);
            if (dres.ready) {
                for (auto& pd : ext(s)->pending_dialogs) {
                    if (!pd.active || pd.tag != dres.request_tag) continue;
                    const JSValue resolve_fn = pd.settle[0];
                    const JSValue reject_fn = pd.settle[1];
                    pd = {};
                    const uint64_t start = SDL_GetPerformanceCounter();
                    begin_tick(s);
                    JSValue obj = JS_NewObject(s->js);
                    JS_SetPropertyStr(s->js, obj, "cancelled", JS_NewBool(s->js, dres.cancelled));
                    JS_SetPropertyStr(s->js, obj, "error", JS_NewBool(s->js, dres.error));
                    JS_SetPropertyStr(s->js, obj, "selectedFilter", JS_NewInt32(s->js, dres.selected_filter));
                    const JSValue parr = JS_NewArray(s->js);
                    for (uint32_t pi = 0u; pi < dres.path_count; ++pi) {
                        JS_SetPropertyUint32(s->js, parr, pi,
                            JS_NewStringLen(s->js, dres.paths[pi].chars ? dres.paths[pi].chars : "", dres.paths[pi].length));
                    }
                    JS_SetPropertyStr(s->js, obj, "paths", parr);
                    const JSValue call_r = JS_Call(s->js, resolve_fn, JS_UNDEFINED, 1, &obj);
                    JS_FreeValue(s->js, call_r);
                    JS_FreeValue(s->js, obj);
                    JS_FreeValue(s->js, resolve_fn);
                    JS_FreeValue(s->js, reject_fn);
                    if (!JS_HasException(s->js)) rebuild(s, false);
                    finish_tick(s, start);
                    etch_request_redraw(s->etch);
                    break;
                }
            }
        }
    }

    // Command registry triggered-command dispatch -> global onCommand(id, state).
    if (s->loaded && !s->stats.has_error) {
        auto* stg = ext(s);
        uint32_t trig[8] = {};
        uint32_t trig_n = 0u;
        for (uint32_t pi = 0u; pi < stg->pending_cmd_count && trig_n < 8u; ++pi) {
            trig[trig_n++] = stg->pending_cmds[pi];
        }
        stg->pending_cmd_count = 0u;
        while (trig_n < 8u) {
            const uint32_t cid = etch_poll_triggered_command(s->etch);
            if (!cid) break;
            trig[trig_n++] = cid;
        }
        if (trig_n > 0u) {
            const uint64_t start = SDL_GetPerformanceCounter();
            begin_tick(s);
            const JSValue global = JS_GetGlobalObject(s->js);
            const JSValue cmd_fn = JS_GetPropertyStr(s->js, global, "onCommand");
            if (JS_IsFunction(s->js, cmd_fn)) {
                for (uint32_t ti = 0u; ti < trig_n && !JS_HasException(s->js); ++ti) {
                    JSValue id_arg = JS_NewUint32(s->js, trig[ti]);
                    for (uint32_t ni = 0u; ni < stg->command_name_count; ++ni) {
                        if (stg->command_names[ni].id == trig[ti]) {
                            JS_FreeValue(s->js, id_arg);
                            id_arg = JS_NewString(s->js, stg->command_names[ni].name);
                            break;
                        }
                    }
                    JSValue args[2] = {id_arg, JS_GetPropertyStr(s->js, global, "state")};
                    const JSValue res = JS_Call(s->js, cmd_fn, global, 2, args);
                    JS_FreeValue(s->js, res);
                    JS_FreeValue(s->js, args[0]);
                    JS_FreeValue(s->js, args[1]);
                }
            }
            JS_FreeValue(s->js, cmd_fn);
            JS_FreeValue(s->js, global);
            if (!JS_HasException(s->js)) rebuild(s, false);
            finish_tick(s, start);
            etch_request_redraw(s->etch);
        }
    }

    bool timer_due = false;
    for (const auto& timer : s->timers) {
        timer_due |= !JS_IsUndefined(timer.function) && (timer.is_raf || timer.due <= current);
    }
    if (timer_due && s->loaded && !s->stats.has_error) {
        const uint64_t start = SDL_GetPerformanceCounter();
        begin_tick(s);
        // Snapshot timer IDs due this frame so rAF/timers scheduled inside callbacks wait for next frame.
        uint32_t due_ids[64] = {};
        uint32_t due_count = 0u;
        for (const auto& timer : s->timers) {
            if (!JS_IsUndefined(timer.function) && (timer.is_raf || timer.due <= current) && due_count < 64u) {
                due_ids[due_count++] = timer.id;
            }
        }
        for (uint32_t di = 0u; di < due_count && !JS_HasException(s->js); ++di) {
            for (auto& timer : s->timers) {
                if (timer.id != due_ids[di] || JS_IsUndefined(timer.function)) continue;
                const bool raf = timer.is_raf;
                const double iv = timer.interval_ms;
                JSValue function = JS_UNDEFINED;
                if (iv > 0.0 && !raf) {
                    function = JS_DupValue(s->js, timer.function);
                    timer.due = current + iv;
                } else {
                    function = timer.function;
                    timer.function = JS_UNDEFINED;
                    timer.due = 0.0;
                    timer.interval_ms = 0.0;
                    timer.id = 0u;
                    timer.is_raf = false;
                }
                JSValue result = JS_UNDEFINED;
                if (raf) {
                    JSValue ts = JS_NewFloat64(s->js, current);
                    result = JS_Call(s->js, function, JS_UNDEFINED, 1, &ts);
                    JS_FreeValue(s->js, ts);
                } else {
                    result = JS_Call(s->js, function, JS_UNDEFINED, 0, nullptr);
                }
                JS_FreeValue(s->js, result);
                JS_FreeValue(s->js, function);
                break;
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
            const float prev_view_w = s->last_view_width_px;
            const uint8_t prev_bp = responsive_width_bucket(prev_view_w);
            const uint8_t next_bp = responsive_width_bucket(eff_view_w);
            s->last_view_width_px = eff_view_w;
            s->current_container_width_px = eff_view_w;
            ext(s)->has_view_width = true;
            if (ext(s)->uses_responsive_cls && (!had_width || prev_bp != next_bp) &&
                s->js && s->loaded && !s->stats.has_error) {
                run_update(s, {}, {});
            } else if (ext(s)->container_width_count > 0u &&
                       (!had_width || std::fabs(eff_view_w - prev_view_w) > 0.5f)) {
                etch_request_redraw(s->etch);
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
            ext(s)->draw_path_id = 0u;
            ext(s)->draw_eb_key = {};
            ext(s)->event_eb_tag_count = 0u;
            ext(s)->needs_container_rebuild = false;
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

    if ((events.first || s->pending_state || vlist_refill || ext(s)->needs_container_rebuild) &&
        s->js && s->loaded && !s->running) {
        ext(s)->needs_container_rebuild = false;
        run_batch(s, events.first);
    }
}

INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface* s) {
    if (!s || !s->loaded || s->stats.has_error) return -1;
    bool has = false;
    double soonest = 0.0;
    for (const auto& timer : s->timers) {
        if (JS_IsUndefined(timer.function)) continue;
        if (timer.is_raf) return 0;
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
INLAY_API bool inlay_surface_bind_terminal(InlaySurface*, EtchStr, InlayTerminalReadFn, void*) { return false; }
INLAY_API bool inlay_surface_bind_portal(InlaySurface*, EtchStr, InlayPortal*) { return false; }
INLAY_API bool inlay_surface_bind_stream(InlaySurface*, EtchStr, InlayStream*) { return false; }
INLAY_API void inlay_surface_set_console_callback(InlaySurface*, InlayConsoleFn, void*) {}
INLAY_API void inlay_surface_set_storage_hooks(InlaySurface*, InlayStorageGetFn, InlayStorageSetFn, InlayStorageRemoveFn, void*) {}
INLAY_API void inlay_surface_set_code_tokenizer(InlaySurface*, EtchSyntaxTokenizeFn, void*) {}
INLAY_API void inlay_surface_set_doc_hooks(InlaySurface*, InlayCodeHighlightFn, void*,
                                           InlayCustomFenceEstimateFn, InlayCustomFenceRenderFn, void*) {}
INLAY_API bool inlay_surface_register_module(InlaySurface*, EtchStr, EtchStr) { return false; }
INLAY_API bool inlay_surface_unregister_module(InlaySurface*, EtchStr) { return false; }
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
