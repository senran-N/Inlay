#include "inlay_ui_internal.h"

#if defined(INLAY_ENABLE_SCRIPT) && INLAY_ENABLE_SCRIPT
#include <SDL3/SDL_keycode.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool inlay_ui_model_read(const InlayModel* model, uint32_t index, InlayModelRow* out_row) noexcept;
bool inlay_ui_model_replay_scroll_delta(const InlayModel* model, uint64_t since_rev,
                                        uint32_t first_visible, int64_t* out_row_delta) noexcept;

namespace {

constexpr uint32_t kRetainedDocs = 16u;
constexpr uint32_t kTermCacheCap = 8u;
constexpr uint32_t kVListCacheCap = 16u;
constexpr uint32_t kTableSortCacheCap = 8u;
constexpr uint32_t kSkeletonCacheCap = 64u;
constexpr uint32_t kMaxModelSelections = 32u;
constexpr uint32_t kTableCellHardCap = 262144u;
constexpr uint32_t kTableRowHardCap = 65536u;
constexpr uint32_t kTableColHardCap = 16384u;
constexpr uint32_t kMenuItemHardCap = 16384u;
constexpr uint32_t kMaxEventJsonBytes = 65536u;

struct ModelSelectionEntry {
    const InlaySurface* surface;
    uint32_t element_id;
    uint64_t selected_row_id;
    uint32_t selected_index;
    bool has_selection;
    uint64_t last_used_frame;
    const InlayModel* last_model;
    uint64_t last_model_rev;
};

ModelSelectionEntry g_model_selections[kMaxModelSelections] = {};
uint32_t g_model_selection_count = 0u;

ModelSelectionEntry* find_or_add_model_selection(const InlaySurface* s, uint32_t element_id) noexcept {
    for (uint32_t i = 0u; i < g_model_selection_count; ++i) {
        if (g_model_selections[i].surface == s && g_model_selections[i].element_id == element_id) {
            g_model_selections[i].last_used_frame = s->frame_counter;
            return &g_model_selections[i];
        }
    }
    ModelSelectionEntry* entry = nullptr;
    if (g_model_selection_count < kMaxModelSelections) {
        entry = &g_model_selections[g_model_selection_count++];
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < g_model_selection_count; ++i) {
            if (g_model_selections[i].last_used_frame < g_model_selections[victim].last_used_frame) {
                victim = i;
            }
        }
        entry = &g_model_selections[victim];
    }
    *entry = {};
    entry->surface = s;
    entry->element_id = element_id;
    entry->last_used_frame = s->frame_counter;
    return entry;
}

void purge_model_selections(const InlaySurface* s) noexcept {
    for (uint32_t i = g_model_selection_count; i > 0u; --i) {
        if (g_model_selections[i - 1u].surface == s) {
            g_model_selections[i - 1u] = g_model_selections[--g_model_selection_count];
        }
    }
}

// ============================================================================
// Common JS Prop, Text Pool, Color & JSON Payload Helpers
// ============================================================================

inline bool has_event(TextSlice action, uint32_t handler) noexcept {
    return action.len > 0u || handler != INLAY_UI_NONE;
}

inline void queue_slice_event(InlaySurface* s, WidgetEvents* events,
                              TextSlice action, uint32_t handler,
                              EtchStr payload, bool is_click = false) {
    if (!has_event(action, handler)) return;
    inlay_ui_queue_event(s, events, inlay_ui_slice(s, action), handler, payload, is_click);
}

inline EtchColor resolve_ref(ColorRef ref, const EtchTheme& theme, EtchColor fallback) noexcept {
    return ref.set ? inlay_ui_resolve_color(ref, theme, fallback) : fallback;
}

inline bool has_explicit_w(const InlaySurface* s, const NodeStyle& st, const ColdVisualStyle* cold) noexcept {
    return (st.flags2 & Nf2WExplicit) != 0u || (cold && (cold->w_min > 0.f || cold->w_max > 0.f)) ||
           (s && !s->draw_parent_col && (st.flags & NfHasFlexGrow) != 0u && st.flex_grow > 0.f);
}

inline bool has_explicit_h(const InlaySurface* s, const NodeStyle& st, const ColdVisualStyle* cold) noexcept {
    return (st.flags2 & Nf2HExplicit) != 0u || (cold && (cold->h_min > 0.f || cold->h_max > 0.f)) ||
           (s && s->draw_parent_col && (st.flags & NfHasFlexGrow) != 0u && st.flex_grow > 0.f);
}

inline bool is_activated(uint32_t id) noexcept {
    return etch_clicked(id) ||
           (etch_focused(id) && (etch_key_pressed(SDLK_RETURN, false) ||
                                 etch_key_pressed(SDLK_KP_ENTER, false) ||
                                 etch_key_pressed(SDLK_SPACE, false)));
}

bool subtree_clicked(const InlaySurface* s, uint32_t first_child, uint32_t parent_id, uint32_t depth = 0u) noexcept {
    if (depth >= 16u) return false;
    for (uint32_t at = first_child; at != INLAY_UI_NONE && at < s->node_count; at = s->nodes[at].next_sibling) {
        const auto& child = s->nodes[at];
        const uint32_t cid = etch_id_index(parent_id, child.id);
        if (etch_clicked(cid)) return true;
        if (child.first_child != INLAY_UI_NONE && subtree_clicked(s, child.first_child, cid, depth + 1u)) {
            return true;
        }
    }
    return false;
}

bool subtree_right_clicked(const InlaySurface* s, uint32_t first_child, uint32_t parent_id, uint32_t depth = 0u) noexcept {
    if (depth >= 16u) return false;
    for (uint32_t at = first_child; at != INLAY_UI_NONE && at < s->node_count; at = s->nodes[at].next_sibling) {
        const auto& child = s->nodes[at];
        const uint32_t cid = etch_id_index(parent_id, child.id);
        if (etch_right_clicked(cid)) return true;
        if (child.first_child != INLAY_UI_NONE && subtree_right_clicked(s, child.first_child, cid, depth + 1u)) {
            return true;
        }
    }
    return false;
}

template <typename Fn>
bool peek_str_prop(InlaySurface* s, JSValueConst obj, const char* name, Fn&& fn) {
    const JSValue v = JS_GetPropertyStr(s->js, obj, name);
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

bool read_array_length(InlaySurface* s, JSValueConst arr, uint32_t* out_len) {
    const JSValue lv = JS_GetPropertyStr(s->js, arr, "length");
    uint32_t n = 0u;
    const bool ok = JS_ToUint32(s->js, &n, lv) == 0;
    JS_FreeValue(s->js, lv);
    if (ok) *out_len = n;
    return ok;
}

bool copy_terminal_lines(InlaySurface* s, JSValueConst lines_v, EtchStr* out_text) {
    uint32_t line_count = 0u;
    if (!read_array_length(s, lines_v, &line_count)) {
        return !JS_HasException(s->js);
    }
    if (line_count == 0u) return true;

    const uint32_t max_avail = s->desc.max_text_bytes - s->text_used;
    if (line_count > max_avail + 1u) {
        JS_ThrowInternalError(s->js, "UI text capacity exceeded");
        return false;
    }
    char* scratch = nullptr;
    uint32_t scratch_len = 0u;
    uint32_t scratch_cap = 0u;
    bool ok = true;

    const auto push_bytes = [&](const char* data, size_t len) -> bool {
        if (len == 0u) return true;
        if (len > size_t(max_avail - scratch_len)) {
            JS_ThrowInternalError(s->js, "UI text capacity exceeded");
            return false;
        }
        const uint32_t need = scratch_len + static_cast<uint32_t>(len);
        if (!inlay_grow_pod(scratch, scratch_cap, need, 256u, max_avail)) {
            JS_ThrowInternalError(s->js, "UI text allocation failed");
            return false;
        }
        std::memcpy(scratch + scratch_len, data, len);
        scratch_len = need;
        return true;
    };

    for (uint32_t i = 0u; i < line_count && ok; ++i) {
        if ((i & 63u) == 0u && inlay_ui_check_watchdog(s)) {
            JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
            ok = false;
            break;
        }
        if (i > 0u) ok = push_bytes("\n", 1u);
        if (!ok) break;
        const JSValue lv = JS_GetPropertyUint32(s->js, lines_v, i);
        if (!JS_IsUndefined(lv) && !JS_IsNull(lv)) {
            size_t n = 0u;
            const char* cs = JS_ToCStringLen(s->js, &n, lv);
            if (cs) {
                ok = push_bytes(cs, n);
                JS_FreeCString(s->js, cs);
            } else {
                ok = false;
            }
        }
        JS_FreeValue(s->js, lv);
    }

    if (ok && scratch_len > 0u) {
        const JSValue joined = JS_NewStringLen(s->js, scratch, scratch_len);
        if (JS_IsException(joined)) {
            ok = false;
        } else {
            const TextSlice sl = inlay_ui_copy_text_slice(s, joined);
            JS_FreeValue(s->js, joined);
            if (JS_HasException(s->js)) {
                ok = false;
            } else {
                *out_text = inlay_ui_slice(s, sl);
            }
        }
    }

    SDL_free(scratch);
    return ok;
}

bool parse_align_x_token(const char* chars, size_t len, EtchAlignX* out) noexcept {
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

// Frame-arena JSON builder with full RFC 8259 string escaping (zero heap allocations).
struct FrameJsonBuf {
    char* data;
    uint32_t len;
    uint32_t cap;
    bool overflow;
};

inline uint32_t frame_json_cap(uint32_t quoted_bytes, uint32_t overhead) noexcept {
    const uint32_t max_raw = (kMaxEventJsonBytes - overhead) / 6u;
    return quoted_bytes >= max_raw ? kMaxEventJsonBytes : (quoted_bytes * 6u + overhead);
}

FrameJsonBuf frame_json_begin(EtchContext* ctx, uint32_t cap) noexcept {
    if (cap > kMaxEventJsonBytes) cap = kMaxEventJsonBytes;
    char* mem = static_cast<char*>(etch_frame_arena_alloc(ctx, cap, 1u));
    return {mem, 0u, mem ? cap : 0u, mem == nullptr};
}

void frame_json_raw(FrameJsonBuf& b, const char* s, uint32_t n) noexcept {
    if (n == 0u) return;
    if (!b.data || n > b.cap - b.len) {
        b.overflow = true;
        return;
    }
    std::memcpy(b.data + b.len, s, n);
    b.len += n;
}

void frame_json_lit(FrameJsonBuf& b, const char* lit) noexcept {
    frame_json_raw(b, lit, static_cast<uint32_t>(std::strlen(lit)));
}

void frame_json_u32(FrameJsonBuf& b, uint32_t v) noexcept {
    char tmp[16] = {};
    const int n = std::snprintf(tmp, sizeof(tmp), "%u", v);
    if (n > 0) frame_json_raw(b, tmp, static_cast<uint32_t>(n));
}

void frame_json_u64(FrameJsonBuf& b, uint64_t v) noexcept {
    char tmp[24] = {};
    const int n = std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(v));
    if (n > 0) frame_json_raw(b, tmp, static_cast<uint32_t>(n));
}

void frame_json_i32(FrameJsonBuf& b, int32_t v) noexcept {
    char tmp[16] = {};
    const int n = std::snprintf(tmp, sizeof(tmp), "%d", v);
    if (n > 0) frame_json_raw(b, tmp, static_cast<uint32_t>(n));
}

void frame_json_bool(FrameJsonBuf& b, bool v) noexcept {
    frame_json_lit(b, v ? "true" : "false");
}

void frame_json_quoted(FrameJsonBuf& b, EtchStr str) noexcept {
    frame_json_raw(b, "\"", 1u);
    for (uint32_t i = 0u; i < str.length; ++i) {
        const uint8_t ch = static_cast<uint8_t>(str.chars[i]);
        switch (ch) {
            case '"':  frame_json_raw(b, "\\\"", 2u); break;
            case '\\': frame_json_raw(b, "\\\\", 2u); break;
            case '\b': frame_json_raw(b, "\\b", 2u); break;
            case '\f': frame_json_raw(b, "\\f", 2u); break;
            case '\n': frame_json_raw(b, "\\n", 2u); break;
            case '\r': frame_json_raw(b, "\\r", 2u); break;
            case '\t': frame_json_raw(b, "\\t", 2u); break;
            default:
                if (ch < 0x20u) {
                    char esc[7] = {};
                    std::snprintf(esc, sizeof(esc), "\\u%04x", unsigned(ch));
                    frame_json_raw(b, esc, 6u);
                } else {
                    const char c = static_cast<char>(ch);
                    frame_json_raw(b, &c, 1u);
                }
                break;
        }
    }
    frame_json_raw(b, "\"", 1u);
}

EtchStr frame_json_finish(const FrameJsonBuf& b) noexcept {
    return (b.data && !b.overflow) ? EtchStr{b.data, b.len, false} : EtchStr{};
}

// ============================================================================
// Persistent Bounded LRU Cache Helpers
// ============================================================================

DocCacheEntry* find_or_add_doc_cache(InlaySurface* s, DocWidgetSpec& spec) noexcept {
    const uint32_t id = spec.path_id;
    if (spec.cache_index < s->doc_cache_count && s->doc_cache[spec.cache_index].element_id == id) {
        auto& entry = s->doc_cache[spec.cache_index];
        entry.last_used_frame = s->frame_counter;
        return &entry;
    }
    for (uint32_t i = 0u; i < s->doc_cache_count; ++i) {
        if (s->doc_cache[i].element_id == id) {
            spec.cache_index = i;
            s->doc_cache[i].last_used_frame = s->frame_counter;
            return &s->doc_cache[i];
        }
    }
    DocCacheEntry* entry = nullptr;
    const uint32_t limit = s->doc_spec_count > kRetainedDocs ? s->doc_spec_count : kRetainedDocs;
    if (s->doc_cache_count < limit) {
        if (!inlay_grow_pod(s->doc_cache, s->doc_cache_cap, s->doc_cache_count + 1u, 4u, limit))
            return nullptr;
        entry = &s->doc_cache[s->doc_cache_count++];
        *entry = {};
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < s->doc_cache_count; ++i) {
            if (s->doc_cache[i].last_used_frame < s->doc_cache[victim].last_used_frame)
                victim = i;
        }
        entry = &s->doc_cache[victim];
        if (entry->doc) {
            inlay_doc_destroy(entry->doc);
            entry->doc = nullptr;
        }
        *entry = {};
    }
    InlayDocConfig cfg = inlay_doc_config_default();
    cfg.initial_source_bytes = 256u;
    cfg.initial_blocks = 8u;
    ++s->stats.document_cache_misses;
    entry->doc = inlay_doc_create(s->etch, &cfg);
    if (!entry->doc) {
        *entry = s->doc_cache[--s->doc_cache_count];
        return nullptr;
    }
    entry->element_id = id;
    spec.cache_index = static_cast<uint32_t>(entry - s->doc_cache);
    entry->last_used_frame = s->frame_counter;
    return entry;
}

TermCacheEntry* find_or_add_term_cache(InlaySurface* s, uint32_t id) noexcept {
    for (uint32_t i = 0u; i < s->term_cache_count; ++i) {
        if (s->term_cache[i].element_id == id) {
            s->term_cache[i].last_used_frame = s->frame_counter;
            return &s->term_cache[i];
        }
    }
    TermCacheEntry* entry = nullptr;
    if (s->term_cache_count < kTermCacheCap) {
        if (!inlay_grow_pod(s->term_cache, s->term_cache_cap, s->term_cache_count + 1u, 4u, kTermCacheCap))
            return nullptr;
        entry = &s->term_cache[s->term_cache_count++];
        *entry = {};
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < s->term_cache_count; ++i) {
            if (s->term_cache[i].last_used_frame < s->term_cache[victim].last_used_frame)
                victim = i;
        }
        entry = &s->term_cache[victim];
        SDL_free(entry->cells);
        *entry = {};
    }
    entry->element_id = id;
    entry->cursor_col = -1;
    entry->cursor_row = -1;
    entry->last_used_frame = s->frame_counter;
    return entry;
}

VirtualListWinEntry* find_vlist_cache(InlaySurface* s, uint32_t id) noexcept {
    for (uint32_t i = 0u; i < s->vlist_cache_count; ++i) {
        if (s->vlist_cache[i].element_id == id) return &s->vlist_cache[i];
    }
    return nullptr;
}

VirtualListWinEntry* find_or_add_vlist_cache(InlaySurface* s, uint32_t id,
                                             uint32_t init_first, uint32_t init_end) noexcept {
    if (auto* existing = find_vlist_cache(s, id)) {
        existing->last_used_frame = s->frame_counter;
        return existing;
    }
    VirtualListWinEntry* entry = nullptr;
    if (s->vlist_cache_count < kVListCacheCap) {
        if (!inlay_grow_pod(s->vlist_cache, s->vlist_cache_cap, s->vlist_cache_count + 1u, 4u, kVListCacheCap))
            return nullptr;
        entry = &s->vlist_cache[s->vlist_cache_count++];
        *entry = {};
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < s->vlist_cache_count; ++i) {
            if (s->vlist_cache[i].last_used_frame < s->vlist_cache[victim].last_used_frame)
                victim = i;
        }
        entry = &s->vlist_cache[victim];
        *entry = {};
    }
    entry->element_id = id;
    entry->first_index = init_first;
    entry->end_index = init_end;
    entry->last_used_frame = s->frame_counter;
    return entry;
}

TableSortCache* find_or_add_table_sort_cache(InlaySurface* s, uint32_t id) noexcept {
    for (uint32_t i = 0u; i < s->table_sort_cache_count; ++i) {
        if (s->table_sort_cache[i].element_id == id) {
            s->table_sort_cache[i].last_used_frame = s->frame_counter;
            return &s->table_sort_cache[i];
        }
    }
    TableSortCache* entry = nullptr;
    if (s->table_sort_cache_count < kTableSortCacheCap) {
        if (!inlay_grow_pod(s->table_sort_cache, s->table_sort_cache_cap,
                            s->table_sort_cache_count + 1u, 4u, kTableSortCacheCap))
            return nullptr;
        entry = &s->table_sort_cache[s->table_sort_cache_count++];
        *entry = {};
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < s->table_sort_cache_count; ++i) {
            if (s->table_sort_cache[i].last_used_frame < s->table_sort_cache[victim].last_used_frame)
                victim = i;
        }
        entry = &s->table_sort_cache[victim];
        SDL_free(entry->row_order);
        *entry = {};
    }
    entry->element_id = id;
    entry->sort_col = UINT32_MAX;
    entry->controlled_applied = false;
    entry->last_controlled_col = -1;
    entry->last_controlled_desc = false;
    entry->last_used_frame = s->frame_counter;
    return entry;
}

SkeletonPulseSlot* find_or_add_skeleton_cache(InlaySurface* s, uint32_t id) noexcept {
    for (uint32_t i = 0u; i < s->skeleton_cache_count; ++i) {
        if (s->skeleton_cache[i].element_id == id) {
            s->skeleton_cache[i].last_used_frame = s->frame_counter;
            return &s->skeleton_cache[i];
        }
    }
    SkeletonPulseSlot* entry = nullptr;
    if (s->skeleton_cache_count < kSkeletonCacheCap) {
        if (!inlay_grow_pod(s->skeleton_cache, s->skeleton_cache_cap,
                            s->skeleton_cache_count + 1u, 8u, kSkeletonCacheCap))
            return nullptr;
        entry = &s->skeleton_cache[s->skeleton_cache_count++];
        *entry = {};
    } else {
        uint32_t victim = 0u;
        for (uint32_t i = 1u; i < s->skeleton_cache_count; ++i) {
            if (s->skeleton_cache[i].last_used_frame < s->skeleton_cache[victim].last_used_frame)
                victim = i;
        }
        entry = &s->skeleton_cache[victim];
        *entry = {};
    }
    entry->element_id = id;
    entry->last_used_frame = s->frame_counter;
    return entry;
}

float eval_skeleton_pulse(InlaySurface* s, uint32_t element_id) noexcept {
    SkeletonPulseSlot* slot = find_or_add_skeleton_cache(s, element_id);
    if (!slot) return 0.35f;
    const uint32_t tween_id = etch_id_index(element_id, 0x5CE1u);
    const EtchEasingSpec ease = etch_easing_in_out();
    constexpr float kHalfPeriodSec = 0.92f;

    if (!slot->initialized) {
        slot->initialized = true;
        (void)etch_tween(tween_id, 0.0f, kHalfPeriodSec, ease, 0.0f);
        const float probe = etch_tween(tween_id, 1.0f, kHalfPeriodSec, ease, 0.0f);
        slot->reduced = (probe > 0.5f);
        slot->target = slot->reduced ? 0.35f : 1.0f;
        return slot->reduced ? 0.35f : probe;
    }
    if (slot->reduced) {
        return 0.35f;
    }
    float val = etch_tween(tween_id, slot->target, kHalfPeriodSec, ease, 0.0f);
    if (slot->target > 0.5f && val >= 0.995f) {
        slot->target = 0.0f;
        val = etch_tween(tween_id, 0.0f, kHalfPeriodSec, ease, 0.0f);
    } else if (slot->target < 0.5f && val <= 0.005f) {
        slot->target = 1.0f;
        val = etch_tween(tween_id, 1.0f, kHalfPeriodSec, ease, 0.0f);
    }
    return std::fmin(1.0f, std::fmax(0.0f, val));
}

// ============================================================================
// ANSI SGR + UTF-8 Terminal Parser
// ============================================================================

uint8_t term_codepoint_width(uint32_t cp) noexcept {
    // Zero-width combining marks and format characters.
    if ((cp >= 0x0300u && cp <= 0x036Fu) ||
        (cp >= 0x1AB0u && cp <= 0x1AFFu) ||
        (cp >= 0x1DC0u && cp <= 0x1DFFu) ||
        (cp >= 0x200Bu && cp <= 0x200Fu) ||
        (cp >= 0x20D0u && cp <= 0x20FFu) ||
        (cp >= 0xFE00u && cp <= 0xFE0Fu) ||
        (cp >= 0xFE20u && cp <= 0xFE2Fu) ||
        cp == 0xFEFFu) {
        return 0u;
    }
    // East Asian Wide / Fullwidth + Emoji blocks.
    if ((cp >= 0x1100u && cp <= 0x115Fu) ||
        cp == 0x2329u || cp == 0x232Au ||
        (cp >= 0x2E80u && cp <= 0xA4CFu && cp != 0x303Fu) ||
        (cp >= 0xAC00u && cp <= 0xD7A3u) ||
        (cp >= 0xF900u && cp <= 0xFAFFu) ||
        (cp >= 0xFE10u && cp <= 0xFE19u) ||
        (cp >= 0xFE30u && cp <= 0xFE6Fu) ||
        (cp >= 0xFF00u && cp <= 0xFF60u) ||
        (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
        (cp >= 0x1F300u && cp <= 0x1FAFFu) ||
        (cp >= 0x20000u && cp <= 0x3FFFDu)) {
        return 2u;
    }
    return 1u;
}

void parse_terminal_ansi(TermCacheEntry* entry, EtchStr text, uint32_t cols, uint32_t rows, uint64_t hash) noexcept {
    const uint32_t total = cols * rows;
    if (!inlay_grow_pod(entry->cells, entry->cell_cap, total, total, 512u * 512u)) return;

    const EtchTerminalCell blank = etch_terminal_cell_default();
    for (uint32_t i = 0u; i < total; ++i) entry->cells[i] = blank;

    auto scroll_up = [&]() noexcept {
        if (rows > 1u) {
            std::memmove(entry->cells, entry->cells + cols,
                         size_t(rows - 1u) * size_t(cols) * sizeof(EtchTerminalCell));
        }
        const uint32_t last_off = (rows - 1u) * cols;
        for (uint32_t c = 0u; c < cols; ++c) entry->cells[last_off + c] = blank;
    };

    uint32_t col = 0u;
    uint32_t row = 0u;
    EtchTerminalColor fg = {ETCH_TERMINAL_DEFAULT, 0u};
    EtchTerminalColor bg = {ETCH_TERMINAL_DEFAULT, 0u};
    bool bold = false;
    bool underline = false;
    bool inverse = false;

    const char* src = text.chars ? text.chars : "";
    const size_t len = text.length;
    size_t pos = 0u;

    while (pos < len) {
        const uint8_t b = static_cast<uint8_t>(src[pos]);
        if (b == 0x1Bu && pos + 1u < len && src[pos + 1u] == ']') {
            // Skip OSC sequence (ESC ] ... BEL or ESC \) cleanly without leaking bytes.
            pos += 2u;
            while (pos < len) {
                const uint8_t c = static_cast<uint8_t>(src[pos++]);
                if (c == 0x07u) break;
                if (c == 0x1Bu && pos < len && src[pos] == '\\') {
                    ++pos;
                    break;
                }
            }
            continue;
        }
        if (b == 0x1Bu && pos + 1u < len && src[pos + 1u] == '[') {
            pos += 2u;
            uint32_t params[16] = {};
            uint32_t param_count = 0u;
            uint32_t cur = 0u;
            bool in_num = false;
            char final_ch = '\0';

            while (pos < len) {
                const uint8_t c = static_cast<uint8_t>(src[pos++]);
                if (c >= '0' && c <= '9') {
                    cur = cur * 10u + uint32_t(c - '0');
                    if (cur > 65535u) cur = 65535u;
                    in_num = true;
                } else if (c == ';' || c == ':') {
                    if (param_count < 16u) params[param_count++] = in_num ? cur : 0u;
                    cur = 0u;
                    in_num = false;
                } else if (c >= 0x40u && c <= 0x7Eu) {
                    if (in_num || param_count > 0u) {
                        if (param_count < 16u) params[param_count++] = in_num ? cur : 0u;
                    }
                    final_ch = static_cast<char>(c);
                    break;
                }
            }

            if (final_ch == 'm') {
                if (param_count == 0u) {
                    params[0] = 0u;
                    param_count = 1u;
                }
                for (uint32_t pi = 0u; pi < param_count; ++pi) {
                    const uint32_t p = params[pi];
                    if (p == 0u) {
                        fg = {ETCH_TERMINAL_DEFAULT, 0u};
                        bg = {ETCH_TERMINAL_DEFAULT, 0u};
                        bold = false;
                        underline = false;
                        inverse = false;
                    } else if (p == 1u) {
                        bold = true;
                    } else if (p == 4u) {
                        underline = true;
                    } else if (p == 7u) {
                        inverse = true;
                    } else if (p == 22u) {
                        bold = false;
                    } else if (p == 24u) {
                        underline = false;
                    } else if (p == 27u) {
                        inverse = false;
                    } else if (p >= 30u && p <= 37u) {
                        fg = {ETCH_TERMINAL_ANSI16, p - 30u};
                    } else if (p == 39u) {
                        fg = {ETCH_TERMINAL_DEFAULT, 0u};
                    } else if (p >= 40u && p <= 47u) {
                        bg = {ETCH_TERMINAL_ANSI16, p - 40u};
                    } else if (p == 49u) {
                        bg = {ETCH_TERMINAL_DEFAULT, 0u};
                    } else if (p >= 90u && p <= 97u) {
                        fg = {ETCH_TERMINAL_ANSI16, 8u + (p - 90u)};
                    } else if (p >= 100u && p <= 107u) {
                        bg = {ETCH_TERMINAL_ANSI16, 8u + (p - 100u)};
                    } else if ((p == 38u || p == 48u) && pi + 1u < param_count) {
                        EtchTerminalColor* target = (p == 38u) ? &fg : &bg;
                        const uint32_t mode = params[pi + 1u];
                        if (mode == 5u && pi + 2u < param_count) {
                            *target = {ETCH_TERMINAL_ANSI256, params[pi + 2u] & 255u};
                            pi += 2u;
                        } else if (mode == 2u && pi + 4u < param_count) {
                            const uint32_t r = params[pi + 2u] & 255u;
                            const uint32_t g = params[pi + 3u] & 255u;
                            const uint32_t bl = params[pi + 4u] & 255u;
                            *target = {ETCH_TERMINAL_TRUE_COLOR, (r << 16) | (g << 8) | bl};
                            pi += 4u;
                        }
                    }
                }
            } else if (final_ch == 'K') {
                // Erase from cursor to end of line.
                const uint32_t row_off = row * cols;
                for (uint32_t c = col; c < cols; ++c) entry->cells[row_off + c] = blank;
            } else if (final_ch == 'J' && param_count > 0u && params[0] == 2u) {
                for (uint32_t i = 0u; i < total; ++i) entry->cells[i] = blank;
                col = 0u;
                row = 0u;
            }
            continue;
        }
        if (b == '\r') {
            col = 0u;
            ++pos;
            continue;
        }
        if (b == '\n') {
            col = 0u;
            if (++row >= rows) {
                scroll_up();
                row = rows - 1u;
            }
            ++pos;
            continue;
        }
        if (b == '\t') {
            uint32_t next_tab = (col + 8u) & ~7u;
            if (next_tab > cols) next_tab = cols;
            while (col < next_tab) {
                entry->cells[row * cols + col] = {' ', fg, bg, 1u, bold, underline, inverse};
                ++col;
            }
            ++pos;
            continue;
        }
        if (b < 0x20u || b == 0x7Fu) {
            ++pos;
            continue;
        }

        const uint32_t cp = inlay_utf8_next(src, len, &pos);
        uint8_t w = term_codepoint_width(cp);
        if (w == 0u) continue;
        if (w == 2u && cols == 1u) w = 1u;

        if (w == 2u && col + 1u >= cols) {
            col = 0u;
            if (++row >= rows) {
                scroll_up();
                row = rows - 1u;
            }
        } else if (col >= cols) {
            col = 0u;
            if (++row >= rows) {
                scroll_up();
                row = rows - 1u;
            }
        }

        const uint32_t idx = row * cols + col;
        entry->cells[idx] = {cp, fg, bg, w, bold, underline, inverse};
        if (w == 2u && col + 1u < cols) {
            entry->cells[idx + 1u] = {' ', fg, bg, 0u, bold, underline, inverse};
            col += 2u;
        } else {
            col += 1u;
        }
    }

    entry->cols = cols;
    entry->rows = rows;
    entry->cursor_col = static_cast<int32_t>(col);
    entry->cursor_row = static_cast<int32_t>(row);
    entry->content_hash = hash;
    entry->content_len = text.length;
}

// ============================================================================
// Native Table Sorting (Zero-JS-Tick Index Indirection, Stable O(N log N))
// ============================================================================

inline uint8_t to_lower_ascii(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32u) : c;
}

bool parse_numeric_cell(EtchStr s, double* out) noexcept {
    if (!s.chars || s.length == 0u) return false;
    uint32_t i = 0u;
    while (i < s.length && (s.chars[i] == ' ' || s.chars[i] == '\t')) ++i;
    char buf[64] = {};
    uint32_t out_n = 0u;
    if (i < s.length && (s.chars[i] == '+' || s.chars[i] == '-')) {
        buf[out_n++] = s.chars[i++];
    }
    while (i < s.length && (s.chars[i] == ' ' || s.chars[i] == '$')) ++i;
    // Skip UTF-8 currency prefixes (EUR, GBP, JPY) if present.
    if (i + 2u < s.length && static_cast<uint8_t>(s.chars[i]) == 0xE2u &&
        static_cast<uint8_t>(s.chars[i + 1u]) == 0x82u &&
        static_cast<uint8_t>(s.chars[i + 2u]) == 0xACu) {
        i += 3u;
    } else if (i + 1u < s.length && static_cast<uint8_t>(s.chars[i]) == 0xC2u &&
               (static_cast<uint8_t>(s.chars[i + 1u]) == 0xA3u ||
                static_cast<uint8_t>(s.chars[i + 1u]) == 0xA5u)) {
        i += 2u;
    }
    while (i < s.length && s.chars[i] == ' ') ++i;
    if (out_n == 0u && i < s.length && (s.chars[i] == '+' || s.chars[i] == '-')) {
        buf[out_n++] = s.chars[i++];
    }
    bool saw_digit = false;
    while (i < s.length && out_n + 1u < sizeof(buf)) {
        const char ch = s.chars[i];
        if (ch >= '0' && ch <= '9') {
            buf[out_n++] = ch;
            saw_digit = true;
            ++i;
        } else if (ch == ',') {
            ++i;
        } else if (ch == '.' || ch == 'e' || ch == 'E' ||
                   ((ch == '+' || ch == '-') && out_n > 0u &&
                    (buf[out_n - 1u] == 'e' || buf[out_n - 1u] == 'E'))) {
            buf[out_n++] = ch;
            ++i;
        } else {
            break;
        }
    }
    if (!saw_digit) return false;
    buf[out_n] = '\0';
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == buf || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

int compare_table_rows(const InlaySurface* s, const TableSpec& spec,
                       uint32_t sort_col, bool numeric_col, bool sort_desc,
                       uint32_t row_a, uint32_t row_b) noexcept {
    const EtchStr a = inlay_ui_slice(s, s->table_cells[spec.cell_offset + row_a * spec.col_count + sort_col]);
    const EtchStr b = inlay_ui_slice(s, s->table_cells[spec.cell_offset + row_b * spec.col_count + sort_col]);
    int cmp = 0;
    if (numeric_col) {
        double da = 0.0, db = 0.0;
        const bool ok_a = parse_numeric_cell(a, &da);
        const bool ok_b = parse_numeric_cell(b, &db);
        if (ok_a && ok_b) {
            cmp = (da < db) ? -1 : ((da > db) ? 1 : 0);
        } else if (ok_a != ok_b) {
            cmp = ok_a ? -1 : 1;
        }
    }
    if (cmp == 0) {
        const uint32_t n = a.length < b.length ? a.length : b.length;
        for (uint32_t i = 0u; i < n; ++i) {
            const uint8_t ca = to_lower_ascii(static_cast<uint8_t>(a.chars[i]));
            const uint8_t cb = to_lower_ascii(static_cast<uint8_t>(b.chars[i]));
            if (ca != cb) {
                cmp = (ca < cb) ? -1 : 1;
                break;
            }
        }
        if (cmp == 0 && a.length != b.length) {
            cmp = (a.length < b.length) ? -1 : 1;
        }
    }
    if (sort_desc) cmp = -cmp;
    if (cmp == 0) cmp = (row_a < row_b) ? -1 : ((row_a > row_b) ? 1 : 0);
    return cmp;
}

uint64_t compute_table_data_hash(const InlaySurface* s, const TableSpec& spec, uint32_t numeric_mask) noexcept {
    uint64_t h = 14695981039346656037ull;
    const uint32_t hdr[3] = {spec.row_count, spec.col_count, numeric_mask};
    h = inlay_fnv1a_64(hdr, sizeof(hdr), h);
    const uint32_t total = spec.row_count * spec.col_count;
    for (uint32_t i = 0u; i < total; ++i) {
        const TextSlice sl = s->table_cells[spec.cell_offset + i];
        h = inlay_fnv1a_64(&sl.len, sizeof(sl.len), h);
        if (sl.len > 0u) {
            h = inlay_fnv1a_64(s->text + sl.off, sl.len, h);
        }
    }
    return h;
}

void rebuild_table_sort_order(const InlaySurface* s, TableSortCache* cache, const TableSpec& spec,
                              uint32_t sort_col, bool numeric_col, bool sort_desc,
                              bool do_sort, uint64_t data_hash) noexcept {
    const uint32_t n = spec.row_count;
    if (n > 0u && !inlay_grow_pod(cache->row_order, cache->row_cap, n, 32u, kTableRowHardCap)) return;
    for (uint32_t i = 0u; i < n; ++i) cache->row_order[i] = i;

    if (do_sort && n > 1u && sort_col < spec.col_count) {
        auto* scratch = static_cast<uint32_t*>(SDL_malloc(size_t(n) * sizeof(uint32_t)));
        if (scratch) {
            uint32_t* src = cache->row_order;
            uint32_t* dst = scratch;
            for (uint32_t width = 1u; width < n; width <<= 1u) {
                for (uint32_t left = 0u; left < n; left += (width << 1u)) {
                    const uint32_t mid = (left + width < n) ? (left + width) : n;
                    const uint32_t right = (mid + width < n) ? (mid + width) : n;
                    uint32_t i = left;
                    uint32_t j = mid;
                    uint32_t k = left;
                    while (i < mid && j < right) {
                        if (compare_table_rows(s, spec, sort_col, numeric_col, sort_desc, src[i], src[j]) <= 0)
                            dst[k++] = src[i++];
                        else
                            dst[k++] = src[j++];
                    }
                    while (i < mid) dst[k++] = src[i++];
                    while (j < right) dst[k++] = src[j++];
                }
                uint32_t* tmp = src;
                src = dst;
                dst = tmp;
            }
            if (src != cache->row_order) {
                std::memcpy(cache->row_order, src, size_t(n) * sizeof(uint32_t));
            }
            SDL_free(scratch);
        }
    }

    cache->row_count = n;
    cache->sort_col = do_sort ? sort_col : UINT32_MAX;
    cache->sort_desc = sort_desc;
    cache->data_hash = data_hash;
}

// ============================================================================
// UTF-8 Helper for Avatar Initials
// ============================================================================

uint32_t encode_utf8_cp(uint32_t cp, char* out) noexcept {
    if (cp >= 'a' && cp <= 'z') cp -= 32u;
    if (cp < 0x80u) {
        out[0] = static_cast<char>(cp);
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (cp >> 6));
        out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out[0] = static_cast<char>(0xE0u | (cp >> 12));
        out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = static_cast<char>(0xF0u | (cp >> 18));
        out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 4u;
    }
    return 0u;
}

uint32_t extract_avatar_initials(EtchStr explicit_init, EtchStr name, char out_buf[12], uint32_t* out_cp_count) noexcept {
    uint32_t cps[2] = {};
    uint32_t count = 0u;
    if (explicit_init.chars && explicit_init.length > 0u) {
        size_t pos = 0u;
        while (pos < explicit_init.length && count < 2u) {
            const uint32_t cp = inlay_utf8_next(explicit_init.chars, explicit_init.length, &pos);
            if (cp > 0x20u && cp != 0xFFFDu) cps[count++] = cp;
        }
    } else if (name.chars && name.length > 0u) {
        size_t pos = 0u;
        bool in_word = false;
        uint32_t first_cp = 0u;
        uint32_t last_word_cp = 0u;
        uint32_t word_count = 0u;
        while (pos < name.length) {
            const uint32_t cp = inlay_utf8_next(name.chars, name.length, &pos);
            const bool is_sep = (cp <= 0x20u || cp == '-' || cp == '_' || cp == '.');
            if (is_sep) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                if (word_count == 0u) first_cp = cp;
                else last_word_cp = cp;
                ++word_count;
            }
        }
        if (word_count >= 1u) cps[count++] = first_cp;
        if (word_count >= 2u) cps[count++] = last_word_cp;
    }
    if (count == 0u) {
        out_buf[0] = '?';
        *out_cp_count = 1u;
        return 1u;
    }
    uint32_t bytes = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        bytes += encode_utf8_cp(cps[i], out_buf + bytes);
    }
    *out_cp_count = count;
    return bytes;
}

// ============================================================================
// Parse Implementations (Called During JS Tree Build)
// ============================================================================

bool parse_doc_widget(InlaySurface* s, JSValueConst props, bool has_props,
                      InlayUiNode& node, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->doc_specs, s->doc_spec_count,
                                             s->doc_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    const InlayDocViewDesc def_vd = inlay_doc_view_desc_default();
    DocWidgetSpec spec = {};
    spec.auto_height = (st.flags2 & Nf2HExplicit) == 0u;
    spec.block_padding_px = def_vd.block_padding_px;
    spec.block_gap_px = def_vd.block_gap_px;
    spec.on_link_handler = INLAY_UI_NONE;
    spec.on_sel_handler = INLAY_UI_NONE;
    spec.on_find_handler = INLAY_UI_NONE;
    spec.on_task_toggle_handler = INLAY_UI_NONE;

    if (has_props) {
        spec.document_name = inlay_ui_string_slice_prop(s, props, "document");
        if (JS_HasException(s->js)) return false;
        const EtchStr md = spec.document_name.len ? EtchStr{} : inlay_ui_string_prop(s, props, "markdown");
        if (JS_HasException(s->js)) return false;
        if (md.length > 0u) {
            node.text = md;
        } else if (!spec.document_name.len && node.text.length == 0u) {
            const EtchStr tx = inlay_ui_string_prop(s, props, "text");
            if (JS_HasException(s->js)) return false;
            if (tx.length > 0u) {
                node.text = tx;
            } else {
                const EtchStr val = inlay_ui_string_prop(s, props, "value");
                if (JS_HasException(s->js)) return false;
                if (val.length > 0u) node.text = val;
            }
        }

        float fs = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "fontSize", &fs, false, 6.0, 128.0)) {
            st.font_size = fs;
            st.flags |= NfHasSize;
        }
        if (JS_HasException(s->js)) return false;

        inlay_ui_opt_bool_prop(s, props, "stickToBottom", &spec.stick_to_bottom);
        if (!inlay_ui_opt_bool_prop(s, props, "showLineNumbers", &spec.show_line_numbers)) {
            inlay_ui_opt_bool_prop(s, props, "lineNumbers", &spec.show_line_numbers);
        }
        inlay_ui_opt_bool_prop(s, props, "hideCopyButton", &spec.hide_copy_button);
        inlay_ui_opt_bool_prop(s, props, "autoHeight", &spec.auto_height);
        inlay_ui_opt_bool_prop(s, props, "streamingRepair", &spec.streaming_repair);

        inlay_ui_opt_number_prop(s, props, "blockPadding", &spec.block_padding_px, false, 0.0, 256.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_number_prop(s, props, "blockGap", &spec.block_gap_px, false, 0.0, 256.0);
        if (JS_HasException(s->js)) return false;

        if (!inlay_ui_parse_event_prop(s, props, "onLinkClick", &spec.on_link_action, &spec.on_link_handler)) {
            inlay_ui_parse_event_prop(s, props, "onLink", &spec.on_link_action, &spec.on_link_handler);
        }
        if (JS_HasException(s->js)) return false;
        spec.find_query = inlay_ui_string_slice_prop(s, props, "findQuery");
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onSelectionChange", &spec.on_sel_action, &spec.on_sel_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onFindResult", &spec.on_find_action, &spec.on_find_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onTaskToggle", &spec.on_task_toggle_action, &spec.on_task_toggle_handler);
        if (JS_HasException(s->js)) return false;
        spec.on_diagram_node_handler = INLAY_UI_NONE;
        inlay_ui_parse_event_prop(s, props, "onDiagramNodeClick", &spec.on_diagram_node_action, &spec.on_diagram_node_handler);
        if (JS_HasException(s->js)) return false;
    } else {
        spec.on_diagram_node_handler = INLAY_UI_NONE;
    }

    s->doc_specs[idx] = spec;
    return true;
}

bool parse_terminal_widget(InlaySurface* s, JSValueConst props, bool has_props,
                           InlayUiNode& node, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->term_specs, s->term_spec_count,
                                             s->term_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    TermWidgetSpec spec = {};
    spec.cols = 80u;
    spec.rows = 24u;
    spec.cursor_col = -1;
    spec.cursor_row = -1;
    spec.has_cursor = false;
    spec.cursor_visible = true;
    spec.line_height_px = 0.f;
    spec.on_cell_click_handler = INLAY_UI_NONE;
    spec.on_link_click_handler = INLAY_UI_NONE;
    spec.on_resize_handler = INLAY_UI_NONE;
    spec.on_sel_handler = INLAY_UI_NONE;
    spec.on_wheel_handler = INLAY_UI_NONE;
    spec.on_input_handler = INLAY_UI_NONE;
    spec.on_paste_handler = INLAY_UI_NONE;
    spec.on_mouse_handler = INLAY_UI_NONE;

    if (has_props) {
        spec.buffer_name = inlay_ui_string_slice_prop(s, props, "buffer");
        if (JS_HasException(s->js)) return false;
        spec.focusable = (spec.buffer_name.len > 0u);

        const EtchStr ansi = spec.buffer_name.len ? EtchStr{} : inlay_ui_string_prop(s, props, "ansi");
        if (JS_HasException(s->js)) return false;
        if (ansi.length > 0u) {
            node.text = ansi;
        } else if (spec.buffer_name.len == 0u) {
            const JSValue lines_v = JS_GetPropertyStr(s->js, props, "lines");
            if (JS_IsArray(lines_v)) {
                if (!copy_terminal_lines(s, lines_v, &node.text)) {
                    JS_FreeValue(s->js, lines_v);
                    return false;
                }
            } else if (node.text.length == 0u) {
                const EtchStr tx = inlay_ui_string_prop(s, props, "text");
                if (JS_HasException(s->js)) {
                    JS_FreeValue(s->js, lines_v);
                    return false;
                }
                if (tx.length > 0u) node.text = tx;
            }
            JS_FreeValue(s->js, lines_v);
        }

        const TextSlice title = inlay_ui_string_slice_prop(s, props, "title");
        if (JS_HasException(s->js)) return false;
        if (title.len > 0u) {
            auto* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return false;
            cold->placeholder = title;
        }

        bool show_chrome = false;
        if (inlay_ui_opt_bool_prop(s, props, "showChrome", &show_chrome)) {
            st.int_value = show_chrome ? 1 : -1;
        } else {
            st.int_value = 0;
        }

        inlay_ui_opt_uint_prop(s, props, "cols", 512u, &spec.cols);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_uint_prop(s, props, "rows", 512u, &spec.rows);
        if (JS_HasException(s->js)) return false;
        if (spec.cols == 0u) spec.cols = 1u;
        if (spec.rows == 0u) spec.rows = 1u;

        float fs = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "fontSize", &fs, false, 6.0, 96.0)) {
            st.font_size = fs;
            st.flags |= NfHasSize;
        }
        if (JS_HasException(s->js)) return false;

        float cw = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "cellWidth", &cw, false, 2.0, 256.0)) {
            st.value = cw;
            st.flags2 |= Nf2HasValue;
        }
        if (JS_HasException(s->js)) return false;

        inlay_ui_opt_number_prop(s, props, "lineHeight", &spec.line_height_px, false, 6.0, 256.0);
        if (JS_HasException(s->js)) return false;

        bool set_cc = inlay_ui_opt_int_prop(s, props, "cursorCol", -1, 4096, &spec.cursor_col);
        if (JS_HasException(s->js)) return false;
        bool set_cr = inlay_ui_opt_int_prop(s, props, "cursorRow", -1, 4096, &spec.cursor_row);
        if (JS_HasException(s->js)) return false;
        if (set_cc || set_cr) {
            if (set_cc && !set_cr && spec.cursor_col >= 0) spec.cursor_row = 0;
            if (set_cr && !set_cc && spec.cursor_row >= 0) spec.cursor_col = 0;
            spec.has_cursor = (spec.cursor_col >= 0 && spec.cursor_row >= 0);
        }

        parse_color_prop(s, props, "cursorColor", &spec.cursor_color);
        if (!st.bg.set) parse_color_prop(s, props, "bg", &st.bg);
        if (!st.color.set) parse_color_prop(s, props, "color", &st.color);
        inlay_ui_opt_bool_prop(s, props, "autoFit", &spec.auto_fit);
        inlay_ui_opt_bool_prop(s, props, "focusable", &spec.focusable);
        inlay_ui_opt_bool_prop(s, props, "interactiveSelection", &spec.interactive_selection);
        parse_color_prop(s, props, "searchColor", &spec.search_color);
        parse_color_prop(s, props, "searchActiveColor", &spec.search_active_color);
        if (inlay_ui_opt_bool_prop(s, props, "cursorVisible", &spec.cursor_visible)) {
            spec.has_cursor_visible = true;
        }
        const EtchStr cs = inlay_ui_string_prop(s, props, "cursorShape");
        if (JS_HasException(s->js)) return false;
        if (cs.length > 0u) {
            spec.has_cursor_shape = true;
            if (inlay_ui_equal(cs, "block")) spec.cursor_shape = uint8_t(ETCH_TERMINAL_CURSOR_BLOCK);
            else if (inlay_ui_equal(cs, "bar") || inlay_ui_equal(cs, "beam")) spec.cursor_shape = uint8_t(ETCH_TERMINAL_CURSOR_BAR);
            else if (inlay_ui_equal(cs, "hollow")) spec.cursor_shape = uint8_t(ETCH_TERMINAL_CURSOR_HOLLOW);
            else spec.cursor_shape = uint8_t(ETCH_TERMINAL_CURSOR_UNDERLINE);
        }
        {
            const JSValue sm_v = JS_GetPropertyStr(s->js, props, "searchMatches");
            if (JS_IsArray(sm_v)) {
                uint32_t sm_len = 0u;
                if (read_array_length(s, sm_v, &sm_len) && sm_len > 0u) {
                    const uint32_t take = sm_len > 256u ? 256u : sm_len;
                    const uint32_t off = inlay_ui_alloc_pool(s, s->term_search_matches, s->term_search_match_count,
                                                             s->term_search_match_cap, take, 8192u);
                    if (off == INLAY_UI_NONE) { JS_FreeValue(s->js, sm_v); return false; }
                    spec.search_off = off;
                    spec.search_cnt = take;
                    for (uint32_t mi = 0u; mi < take; ++mi) {
                        TermSearchMatchItem item = {};
                        const JSValue mv = JS_GetPropertyUint32(s->js, sm_v, mi);
                        if (JS_IsObject(mv) && !JS_IsNull(mv)) {
                            inlay_ui_opt_uint_prop(s, mv, "row", 1000000u, &item.row);
                            inlay_ui_opt_uint_prop(s, mv, "startCol", 4096u, &item.start_col);
                            inlay_ui_opt_uint_prop(s, mv, "endCol", 4096u, &item.end_col);
                            item.active = inlay_ui_bool_prop(s, mv, "active");
                        }
                        JS_FreeValue(s->js, mv);
                        s->term_search_matches[off + mi] = item;
                    }
                }
            }
            JS_FreeValue(s->js, sm_v);
        }
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onCellClick", &spec.on_cell_click_action, &spec.on_cell_click_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onLinkClick", &spec.on_link_click_action, &spec.on_link_click_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onResize", &spec.on_resize_action, &spec.on_resize_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onSelectionChange", &spec.on_sel_action, &spec.on_sel_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onWheel", &spec.on_wheel_action, &spec.on_wheel_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_bool_prop(s, props, "captureInput", &spec.capture_input);
        inlay_ui_opt_bool_prop(s, props, "bracketedPaste", &spec.bracketed_paste);
        inlay_ui_opt_bool_prop(s, props, "mouseReporting", &spec.mouse_reporting);
        inlay_ui_opt_bool_prop(s, props, "appCursor", &spec.app_cursor);
        {
            const JSValue pal_v = JS_GetPropertyStr(s->js, props, "palette");
            if (JS_IsArray(pal_v)) {
                uint32_t plen = 0u;
                if (read_array_length(s, pal_v, &plen) && plen >= 16u) {
                    bool ok_pal = true;
                    for (uint32_t pi = 0u; pi < 16u; ++pi) {
                        const JSValue cv = JS_GetPropertyUint32(s->js, pal_v, pi);
                        size_t clen = 0u;
                        const char* pcs = JS_ToCStringLen(s->js, &clen, cv);
                        if (pcs) {
                            const ColorRef cr = inlay_ui_parse_color(pcs, clen);
                            if (cr.set && cr.token == TokNone) spec.palette[pi] = cr.rgba;
                            else ok_pal = false;
                            JS_FreeCString(s->js, pcs);
                        } else {
                            ok_pal = false;
                        }
                        JS_FreeValue(s->js, cv);
                    }
                    spec.has_palette = ok_pal;
                }
            }
            JS_FreeValue(s->js, pal_v);
        }
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onInput", &spec.on_input_action, &spec.on_input_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onPaste", &spec.on_paste_action, &spec.on_paste_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onMouse", &spec.on_mouse_action, &spec.on_mouse_handler);
        if (JS_HasException(s->js)) return false;
        if (has_event(spec.on_input_action, spec.on_input_handler) ||
            has_event(spec.on_paste_action, spec.on_paste_handler)) {
            spec.capture_input = true;
            spec.focusable = true;
        }
        if (has_event(spec.on_mouse_action, spec.on_mouse_handler)) {
            spec.mouse_reporting = true;
        }
    }

    s->term_specs[idx] = spec;
    return true;
}

bool parse_virtual_list_widget(InlaySurface* s, JSValueConst props, bool has_props,
                               NodeStyle& st, uint32_t path_id) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->vlist_specs, s->vlist_spec_count,
                                             s->vlist_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    VirtualListSpec spec = {};
    spec.path_id = path_id;
    spec.item_height_px = 36.f;
    spec.row_gap_px = (st.flags & NfHasGap) ? std::fmax(0.f, st.gap) : 0.f;
    spec.overscan_rows = 8u;
    spec.scroll_fade_px = 16.f;
    spec.stick_to_bottom = false;
    spec.show_scrollbar = true;
    spec.on_range_handler = INLAY_UI_NONE;

    if (has_props) {
        bool has_count = inlay_ui_opt_uint_prop(s, props, "count", 10000000u, &spec.total_count);
        if (JS_HasException(s->js)) return false;
        if (!has_count) {
            has_count = inlay_ui_opt_uint_prop(s, props, "itemCount", 10000000u, &spec.total_count);
            if (JS_HasException(s->js)) return false;
        }

        bool has_win_start = inlay_ui_opt_uint_prop(s, props, "windowStart", 10000000u, &spec.window_start);
        if (JS_HasException(s->js)) return false;
        if (!has_win_start) {
            has_win_start = inlay_ui_opt_uint_prop(s, props, "firstIndex", 10000000u, &spec.window_start);
            if (JS_HasException(s->js)) return false;
        }

        inlay_ui_opt_number_prop(s, props, "itemHeight", &spec.item_height_px, false, 8.0, 2048.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_number_prop(s, props, "rowGap", &spec.row_gap_px, false, 0.0, 256.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_uint_prop(s, props, "overscan", 256u, &spec.overscan_rows);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_number_prop(s, props, "scrollFade", &spec.scroll_fade_px, false, 0.0, 128.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_bool_prop(s, props, "stickToBottom", &spec.stick_to_bottom);
        inlay_ui_opt_bool_prop(s, props, "showScrollbar", &spec.show_scrollbar);

        if (!inlay_ui_parse_event_prop(s, props, "onRange", &spec.on_range_action, &spec.on_range_handler)) {
            if (!inlay_ui_parse_event_prop(s, props, "onRangeChange", &spec.on_range_action, &spec.on_range_handler)) {
                inlay_ui_parse_event_prop(s, props, "onVisibleRange", &spec.on_range_action, &spec.on_range_handler);
            }
        }
        if (JS_HasException(s->js)) return false;

        if (!has_event(st.action, st.handler)) {
            if (!inlay_ui_parse_event_prop(s, props, "onSelect", &st.action, &st.handler)) {
                inlay_ui_parse_event_prop(s, props, "onItemClick", &st.action, &st.handler);
            }
            if (JS_HasException(s->js)) return false;
        }

        // Optional 0-JS-tick host model mode: model: "boundModelName"
        const TextSlice model_name = inlay_ui_string_slice_prop(s, props, "model");
        if (JS_HasException(s->js)) return false;
        if (model_name.len > 0u) {
            auto* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return false;
            cold->placeholder = model_name;
            st.flags2 |= Nf2Leaf;
            int32_t sel_idx = -1;
            if (inlay_ui_opt_int_prop(s, props, "selectedRow", -1, 100000000, &sel_idx) ||
                inlay_ui_opt_int_prop(s, props, "selectedIndex", -1, 100000000, &sel_idx)) {
                st.int_value = sel_idx;
                st.flags2 |= Nf2HasMin;
            }
            if (JS_HasException(s->js)) return false;
        } else {
            // Detect windowed renderItem callback mode (inlay_ui.cpp coordinates child generation via s->vlist_cache).
            const JSValue render_fn = JS_GetPropertyStr(s->js, props, "renderItem");
            if (JS_IsFunction(s->js, render_fn)) {
                st.flags2 |= Nf2HasValue;
                if (!has_win_start) {
                    if (const auto* win = find_vlist_cache(s, path_id)) {
                        spec.window_start = win->first_index;
                    }
                }
            }
            JS_FreeValue(s->js, render_fn);

            // Optional 0-JS-tick data mode: items: (string | { title/label, subtitle/desc, badge, meta })[]
            const JSValue items_v = JS_GetPropertyStr(s->js, props, "items");
            if (JS_IsArray(items_v)) {
                uint32_t arr_len = 0u;
                if (read_array_length(s, items_v, &arr_len)) {
                    constexpr uint32_t kMaxDataItems = kTableCellHardCap / 4u;
                    const uint32_t data_count = arr_len > kMaxDataItems ? kMaxDataItems : arr_len;
                    if (data_count > 0u) {
                        const uint32_t cell_off = inlay_ui_alloc_pool(
                            s, s->table_cells, s->table_cell_count, s->table_cell_cap,
                            data_count * 4u, kTableCellHardCap);
                        if (cell_off == INLAY_UI_NONE) {
                            JS_FreeValue(s->js, items_v);
                            return false;
                        }
                        auto* cold = inlay_ui_ensure_cold(s, st);
                        if (!cold) {
                            JS_FreeValue(s->js, items_v);
                            return false;
                        }
                        cold->item_offset = cell_off;
                        st.int_value = static_cast<int32_t>(data_count);
                        st.flags2 |= Nf2Selected;

                        for (uint32_t i = 0u; i < data_count; ++i) {
                            if ((i & 63u) == 0u && inlay_ui_check_watchdog(s)) {
                                JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
                                JS_FreeValue(s->js, items_v);
                                return false;
                            }
                            const JSValue iv = JS_GetPropertyUint32(s->js, items_v, i);
                            TextSlice title = {};
                            TextSlice subtitle = {};
                            TextSlice badge = {};
                            TextSlice meta = {};
                            if (JS_IsObject(iv) && !JS_IsNull(iv)) {
                                title = inlay_ui_string_slice_prop(s, iv, "title");
                                if (!title.len && !JS_HasException(s->js))
                                    title = inlay_ui_string_slice_prop(s, iv, "label");
                                if (!JS_HasException(s->js)) {
                                    subtitle = inlay_ui_string_slice_prop(s, iv, "subtitle");
                                    if (!subtitle.len && !JS_HasException(s->js))
                                        subtitle = inlay_ui_string_slice_prop(s, iv, "desc");
                                }
                                if (!JS_HasException(s->js))
                                    badge = inlay_ui_string_slice_prop(s, iv, "badge");
                                if (!JS_HasException(s->js))
                                    meta = inlay_ui_string_slice_prop(s, iv, "meta");
                            } else if (!JS_IsUndefined(iv) && !JS_IsNull(iv)) {
                                title = inlay_ui_copy_text_slice(s, iv);
                            }
                            JS_FreeValue(s->js, iv);
                            if (JS_HasException(s->js)) {
                                JS_FreeValue(s->js, items_v);
                                return false;
                            }
                            s->table_cells[cell_off + i * 4u + 0u] = title;
                            s->table_cells[cell_off + i * 4u + 1u] = subtitle;
                            s->table_cells[cell_off + i * 4u + 2u] = badge;
                            s->table_cells[cell_off + i * 4u + 3u] = meta;
                        }
                    } else {
                        st.int_value = 0;
                        st.flags2 |= Nf2Selected;
                    }
                    if (!has_count) spec.total_count = data_count;
                }
            }
            JS_FreeValue(s->js, items_v);
        }
    }

    s->vlist_specs[idx] = spec;
    return true;
}

bool parse_table_widget(InlaySurface* s, JSValueConst props, bool has_props,
                        NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->table_specs, s->table_spec_count,
                                             s->table_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    TableSpec spec = {};
    spec.header_height_px = 36.f;
    spec.row_height_px = 34.f;
    spec.cell_padding_x = 12.f;
    spec.selected_row = -1;
    spec.sort_col = -1;
    spec.zebra = true;
    spec.on_sort_handler = INLAY_UI_NONE;
    uint32_t numeric_mask = 0u;

    if (has_props) {
        inlay_ui_opt_number_prop(s, props, "headerHeight", &spec.header_height_px, false, 18.0, 256.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_number_prop(s, props, "rowHeight", &spec.row_height_px, false, 18.0, 256.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_number_prop(s, props, "cellPaddingX", &spec.cell_padding_x, false, 0.0, 128.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_int_prop(s, props, "selectedRow", -1, 1000000, &spec.selected_row);
        if (JS_HasException(s->js)) return false;

        if (inlay_ui_opt_int_prop(s, props, "sortColumn", 0, int32_t(ETCH_MAX_TABLE_COLUMNS) - 1, &spec.sort_col) ||
            inlay_ui_opt_int_prop(s, props, "sortCol", 0, int32_t(ETCH_MAX_TABLE_COLUMNS) - 1, &spec.sort_col)) {
            spec.has_sort = true;
        }
        if (JS_HasException(s->js)) return false;
        if (!inlay_ui_opt_bool_prop(s, props, "sortDesc", &spec.sort_desc)) {
            inlay_ui_opt_bool_prop(s, props, "sortDescending", &spec.sort_desc);
        }
        if (!inlay_ui_opt_bool_prop(s, props, "striped", &spec.zebra)) {
            inlay_ui_opt_bool_prop(s, props, "zebra", &spec.zebra);
        }
        if (!inlay_ui_opt_bool_prop(s, props, "showColumnLines", &spec.show_column_lines)) {
            inlay_ui_opt_bool_prop(s, props, "columnLines", &spec.show_column_lines);
        }
        inlay_ui_opt_bool_prop(s, props, "horizontalScroll", &spec.horizontal_scroll);

        parse_color_prop(s, props, "headerBg", &spec.header_bg);
        parse_color_prop(s, props, "zebraBg", &spec.zebra_bg);
        parse_color_prop(s, props, "hoverRowBg", &spec.hover_row_bg);
        parse_color_prop(s, props, "selectedRowBg", &spec.selected_row_bg);

        inlay_ui_parse_event_prop(s, props, "onSort", &spec.on_sort_action, &spec.on_sort_handler);
        if (JS_HasException(s->js)) return false;
        if (!has_event(st.action, st.handler)) {
            if (!inlay_ui_parse_event_prop(s, props, "onRowClick", &st.action, &st.handler)) {
                inlay_ui_parse_event_prop(s, props, "onSelect", &st.action, &st.handler);
            }
            if (JS_HasException(s->js)) return false;
        }

        // Parse columns (up to ETCH_MAX_TABLE_COLUMNS = 16).
        const JSValue cols_v = JS_GetPropertyStr(s->js, props, "columns");
        if (JS_IsArray(cols_v)) {
            uint32_t raw_cols = 0u;
            if (read_array_length(s, cols_v, &raw_cols) && raw_cols > 0u) {
                const uint32_t col_count =
                    raw_cols > uint32_t(ETCH_MAX_TABLE_COLUMNS) ? uint32_t(ETCH_MAX_TABLE_COLUMNS) : raw_cols;
                const uint32_t col_off = inlay_ui_alloc_pool(
                    s, s->table_cols, s->table_col_count, s->table_col_cap, col_count, kTableColHardCap);
                if (col_off == INLAY_UI_NONE) {
                    JS_FreeValue(s->js, cols_v);
                    return false;
                }
                spec.col_offset = col_off;
                spec.col_count = col_count;

                for (uint32_t c = 0u; c < col_count; ++c) {
                    TableColSpec cs = {};
                    cs.width_px = 1.f;
                    cs.min_width_px = 48.f;
                    cs.max_width_px = 4096.f;
                    cs.align_x = uint8_t(ETCH_ALIGN_X_LEFT);
                    cs.sizing = uint8_t(ETCH_TABLE_COL_STRETCH);
                    cs.resizable = true;
                    cs.sortable = true;

                    const JSValue cv = JS_GetPropertyUint32(s->js, cols_v, c);
                    if (JS_IsObject(cv) && !JS_IsNull(cv)) {
                        cs.title = inlay_ui_string_slice_prop(s, cv, "title");
                        if (!cs.title.len && !JS_HasException(s->js))
                            cs.title = inlay_ui_string_slice_prop(s, cv, "label");
                        if (!JS_HasException(s->js))
                            cs.key = inlay_ui_string_slice_prop(s, cv, "key");
                        if (!cs.title.len) cs.title = cs.key;
                        if (!cs.key.len) cs.key = cs.title;

                        const bool is_num = inlay_ui_bool_prop(s, cv, "numeric");
                        if (is_num) numeric_mask |= (1u << c);

                        float w = 0.f;
                        if (inlay_ui_opt_number_prop(s, cv, "width", &w, false, 1.0, 4096.0)) {
                            cs.sizing = uint8_t(ETCH_TABLE_COL_FIXED);
                            cs.width_px = w;
                            if (w < cs.min_width_px) cs.min_width_px = w;
                        }
                        if (!JS_HasException(s->js)) {
                            inlay_ui_opt_number_prop(s, cv, "minWidth", &cs.min_width_px, false, 0.0, 4096.0);
                        }
                        if (!JS_HasException(s->js)) {
                            inlay_ui_opt_number_prop(s, cv, "maxWidth", &cs.max_width_px, false, 0.0, 8192.0);
                        }
                        if (cs.max_width_px < cs.min_width_px) cs.max_width_px = cs.min_width_px;
                        if (cs.sizing == uint8_t(ETCH_TABLE_COL_FIXED) && cs.width_px > cs.max_width_px) {
                            cs.max_width_px = cs.width_px;
                        }

                        EtchAlignX ax = is_num ? ETCH_ALIGN_X_RIGHT : ETCH_ALIGN_X_LEFT;
                        peek_str_prop(s, cv, "align", [&](const char* ch, size_t n) noexcept {
                            return parse_align_x_token(ch, n, &ax);
                        });
                        cs.align_x = static_cast<uint8_t>(ax);

                        inlay_ui_opt_bool_prop(s, cv, "sortable", &cs.sortable);
                        inlay_ui_opt_bool_prop(s, cv, "resizable", &cs.resizable);
                    } else if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
                        cs.title = inlay_ui_copy_text_slice(s, cv);
                        cs.key = cs.title;
                    }
                    JS_FreeValue(s->js, cv);
                    if (JS_HasException(s->js)) {
                        JS_FreeValue(s->js, cols_v);
                        return false;
                    }
                    s->table_cols[col_off + c] = cs;
                }
            }
        }
        JS_FreeValue(s->js, cols_v);

        // Parse rows (array of arrays or array of objects keyed by col.key).
        if (spec.col_count > 0u) {
            const JSValue rows_v = JS_GetPropertyStr(s->js, props, "rows");
            if (JS_IsArray(rows_v)) {
                uint32_t raw_rows = 0u;
                if (read_array_length(s, rows_v, &raw_rows) && raw_rows > 0u) {
                    const uint32_t max_rows_cells = kTableCellHardCap / spec.col_count;
                    const uint32_t max_rows = max_rows_cells > kTableRowHardCap ? kTableRowHardCap : max_rows_cells;
                    const uint32_t row_count = raw_rows > max_rows ? max_rows : raw_rows;
                    const uint32_t cell_off = inlay_ui_alloc_pool(
                        s, s->table_cells, s->table_cell_count, s->table_cell_cap,
                        row_count * spec.col_count, kTableCellHardCap);
                    if (cell_off == INLAY_UI_NONE) {
                        JS_FreeValue(s->js, rows_v);
                        return false;
                    }
                    spec.cell_offset = cell_off;
                    spec.row_count = row_count;

                    JSAtom col_atoms[ETCH_MAX_TABLE_COLUMNS] = {};
                    for (uint32_t c = 0u; c < spec.col_count; ++c) {
                        const EtchStr key = inlay_ui_slice(s, s->table_cols[spec.col_offset + c].key);
                        col_atoms[c] = key.length > 0u
                            ? JS_NewAtomLen(s->js, key.chars, key.length)
                            : JS_ATOM_NULL;
                    }

                    bool ok = true;
                    for (uint32_t r = 0u; r < row_count && ok; ++r) {
                        if ((r & 63u) == 0u && inlay_ui_check_watchdog(s)) {
                            JS_ThrowInternalError(s->js, "UI conversion deadline exceeded");
                            ok = false;
                            break;
                        }
                        const JSValue rv = JS_GetPropertyUint32(s->js, rows_v, r);
                        const bool is_arr = JS_IsArray(rv);
                        const bool is_obj = !is_arr && JS_IsObject(rv) && !JS_IsNull(rv);
                        for (uint32_t c = 0u; c < spec.col_count && ok; ++c) {
                            TextSlice cell = {};
                            if (is_arr) {
                                const JSValue cv = JS_GetPropertyUint32(s->js, rv, c);
                                if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
                                    cell = inlay_ui_copy_text_slice(s, cv);
                                    if (JS_HasException(s->js)) ok = false;
                                }
                                JS_FreeValue(s->js, cv);
                            } else if (is_obj && col_atoms[c] != JS_ATOM_NULL) {
                                const JSValue cv = JS_GetProperty(s->js, rv, col_atoms[c]);
                                if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
                                    cell = inlay_ui_copy_text_slice(s, cv);
                                    if (JS_HasException(s->js)) ok = false;
                                }
                                JS_FreeValue(s->js, cv);
                            } else if (c == 0u && !JS_IsUndefined(rv) && !JS_IsNull(rv)) {
                                cell = inlay_ui_copy_text_slice(s, rv);
                                if (JS_HasException(s->js)) ok = false;
                            }
                            s->table_cells[cell_off + r * spec.col_count + c] = cell;
                        }
                        JS_FreeValue(s->js, rv);
                    }

                    for (uint32_t c = 0u; c < spec.col_count; ++c) {
                        if (col_atoms[c] != JS_ATOM_NULL) JS_FreeAtom(s->js, col_atoms[c]);
                    }
                    if (!ok) {
                        JS_FreeValue(s->js, rows_v);
                        return false;
                    }
                }
            }
            JS_FreeValue(s->js, rows_v);
        }
    }

    st.int_value = static_cast<int32_t>(numeric_mask & 0xFFFFu);
    s->table_specs[idx] = spec;
    return true;
}

bool parse_tree_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    st.int_value = 0; // 0 = default (show indent guide), 1 = explicit true, -1 = explicit false
    if (has_props) {
        bool guide = true;
        if (inlay_ui_opt_bool_prop(s, props, "showIndentGuide", &guide)) {
            st.int_value = guide ? 1 : -1;
        }
        const TextSlice model_name = inlay_ui_string_slice_prop(s, props, "model");
        if (JS_HasException(s->js)) return false;
        if (model_name.len > 0u) {
            auto* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return false;
            cold->placeholder = model_name;
            st.flags2 |= Nf2Leaf;
        }
        float rh = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "rowHeight", &rh, false, 16.0, 128.0)) {
            st.value = rh;
            st.flags2 |= Nf2HasValue;
        }
        if (JS_HasException(s->js)) return false;
        float indent = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "indent", &indent, false, 4.0, 64.0)) {
            auto* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return false;
            cold->min_val = indent;
            st.flags2 |= Nf2HasMin;
        }
        if (JS_HasException(s->js)) return false;
        if (!has_event(st.action, st.handler)) {
            if (!inlay_ui_parse_event_prop(s, props, "onSelect", &st.action, &st.handler)) {
                inlay_ui_parse_event_prop(s, props, "onClick", &st.action, &st.handler);
            }
            if (JS_HasException(s->js)) return false;
        }
    }
    return true;
}

bool parse_tree_item_widget(InlaySurface* s, JSValueConst props, bool has_props,
                            InlayUiNode& node, NodeStyle& st) {
    // Bit layout of st.int_value for TREE_ITEM:
    //   bit 0: showIndentGuide explicitly set
    //   bit 1: showIndentGuide value
    //   bit 2: leaf explicitly set
    //   bit 3: onToggle bound (action in cold->placeholder, handler in cold->sprite_id)
    st.int_value = 0;
    if (!has_props) return true;

    if (node.text.length == 0u) {
        const EtchStr lbl = inlay_ui_string_prop(s, props, "label");
        if (JS_HasException(s->js)) return false;
        if (lbl.length > 0u) {
            node.text = lbl;
        } else {
            const EtchStr tx = inlay_ui_string_prop(s, props, "text");
            if (JS_HasException(s->js)) return false;
            if (tx.length > 0u) node.text = tx;
        }
    }

    bool def_open = false;
    if (inlay_ui_opt_bool_prop(s, props, "defaultOpen", &def_open)) {
        if (def_open) st.flags2 |= Nf2DefaultOpen;
        else st.flags2 &= uint16_t(~Nf2DefaultOpen);
    }

    bool is_open = false;
    if (inlay_ui_opt_bool_prop(s, props, "open", &is_open)) {
        st.flags2 |= Nf2HasOpen;
        if (is_open) st.flags |= NfOpen;
        else st.flags &= ~NfOpen;
    }

    bool sel = false;
    if (inlay_ui_opt_bool_prop(s, props, "selected", &sel)) {
        if (sel) st.flags2 |= Nf2Selected;
        else st.flags2 &= uint16_t(~Nf2Selected);
    }

    bool leaf = false;
    if (inlay_ui_opt_bool_prop(s, props, "leaf", &leaf)) {
        st.int_value |= (1 << 2);
        if (leaf) st.flags2 |= Nf2Leaf;
        else st.flags2 &= uint16_t(~Nf2Leaf);
    }

    uint32_t icon_u = 0u;
    if (inlay_ui_opt_uint_prop(s, props, "iconId", ETCH_MAX_VECTOR_ICONS - 1u, &icon_u)) {
        st.icon_id = static_cast<uint16_t>(icon_u);
        st.flags2 |= Nf2HasIcon;
    }
    if (JS_HasException(s->js)) return false;

    bool guide = true;
    if (inlay_ui_opt_bool_prop(s, props, "showIndentGuide", &guide)) {
        st.int_value |= (1 << 0);
        if (guide) st.int_value |= (1 << 1);
    }

    float rh = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "rowHeight", &rh, false, 16.0, 128.0)) {
        st.value = rh;
        st.flags2 |= Nf2HasValue;
    }
    if (JS_HasException(s->js)) return false;

    float indent = 0.f;
    if (inlay_ui_opt_number_prop(s, props, "indent", &indent, false, 4.0, 64.0)) {
        auto* cold = inlay_ui_ensure_cold(s, st);
        if (!cold) return false;
        cold->min_val = indent;
        st.flags2 |= Nf2HasMin;
    }
    if (JS_HasException(s->js)) return false;

    if (!has_event(st.action, st.handler)) {
        if (!inlay_ui_parse_event_prop(s, props, "onSelect", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "onClick", &st.action, &st.handler);
        }
        if (JS_HasException(s->js)) return false;
    }

    TextSlice tog_act = {};
    uint32_t tog_h = INLAY_UI_NONE;
    if (inlay_ui_parse_event_prop(s, props, "onToggle", &tog_act, &tog_h)) {
        auto* cold = inlay_ui_ensure_cold(s, st);
        if (!cold) return false;
        cold->placeholder = tog_act;
        cold->sprite_id = tog_h;
        st.int_value |= (1 << 3);
    }
    if (JS_HasException(s->js)) return false;

    return true;
}

bool parse_menu_widget(InlaySurface* s, JSValueConst props, bool has_props,
                       NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->menu_specs, s->menu_spec_count,
                                             s->menu_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    MenuSpec spec = {};
    spec.on_close_handler = INLAY_UI_NONE;
    spec.on_open_change_handler = INLAY_UI_NONE;

    if (has_props) {
        spec.label = inlay_ui_string_slice_prop(s, props, "label");
        if (JS_HasException(s->js)) return false;
        if (!spec.label.len) {
            spec.label = inlay_ui_string_slice_prop(s, props, "text");
            if (JS_HasException(s->js)) return false;
        }

        if (inlay_ui_opt_bool_prop(s, props, "open", &spec.open)) {
            spec.has_open = true;
        }

        float bblur = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "backdropBlur", &bblur, false, 0.0, 128.0)) {
            auto* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return false;
            cold->backdrop_blur = bblur;
        }
        if (JS_HasException(s->js)) return false;

        if (!has_event(st.action, st.handler)) {
            inlay_ui_parse_event_prop(s, props, "onSelect", &st.action, &st.handler);
            if (JS_HasException(s->js)) return false;
        }
        inlay_ui_parse_event_prop(s, props, "onClose", &spec.on_close_action, &spec.on_close_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onOpenChange", &spec.on_open_change_action, &spec.on_open_change_handler);
        if (JS_HasException(s->js)) return false;

        const JSValue items_v = JS_GetPropertyStr(s->js, props, "items");
        if (JS_IsArray(items_v)) {
            uint32_t raw_items = 0u;
            if (read_array_length(s, items_v, &raw_items) && raw_items > 0u) {
                const uint32_t item_count = raw_items > 256u ? 256u : raw_items;
                const uint32_t item_off = inlay_ui_alloc_pool(
                    s, s->menu_items, s->menu_item_count, s->menu_item_cap, item_count, kMenuItemHardCap);
                if (item_off == INLAY_UI_NONE) {
                    JS_FreeValue(s->js, items_v);
                    return false;
                }
                spec.item_offset = item_off;
                spec.item_count = item_count;

                for (uint32_t i = 0u; i < item_count; ++i) {
                    MenuItemSpec mi = {};
                    mi.handler = INLAY_UI_NONE;
                    mi.icon_id = uint16_t(ETCH_INVALID_ICON);

                    const JSValue iv = JS_GetPropertyUint32(s->js, items_v, i);
                    if (JS_IsObject(iv) && !JS_IsNull(iv)) {
                        mi.separator = inlay_ui_bool_prop(s, iv, "separator") ||
                                       inlay_ui_bool_prop(s, iv, "divider");
                        if (!mi.separator) {
                            peek_str_prop(s, iv, "type", [&](const char* ch, size_t n) noexcept {
                                if (inlay_ui_str_eq(ch, n, "separator") || inlay_ui_str_eq(ch, n, "divider")) {
                                    mi.separator = true;
                                    return true;
                                }
                                return false;
                            });
                        }
                        mi.label = inlay_ui_string_slice_prop(s, iv, "label");
                        if (!mi.label.len && !JS_HasException(s->js))
                            mi.label = inlay_ui_string_slice_prop(s, iv, "text");
                        if (!JS_HasException(s->js))
                            mi.shortcut = inlay_ui_string_slice_prop(s, iv, "shortcut");
                        if (!JS_HasException(s->js))
                            mi.value = inlay_ui_string_slice_prop(s, iv, "value");

                        uint32_t ic = 0u;
                        if (!JS_HasException(s->js) &&
                            inlay_ui_opt_uint_prop(s, iv, "iconId", ETCH_MAX_VECTOR_ICONS - 1u, &ic)) {
                            mi.icon_id = static_cast<uint16_t>(ic);
                            mi.has_icon = true;
                        }
                        mi.checked = inlay_ui_bool_prop(s, iv, "checked");
                        mi.disabled = inlay_ui_bool_prop(s, iv, "disabled");
                        mi.danger = inlay_ui_bool_prop(s, iv, "danger");

                        if (!JS_HasException(s->js)) {
                            if (!inlay_ui_parse_event_prop(s, iv, "onClick", &mi.action, &mi.handler)) {
                                inlay_ui_parse_event_prop(s, iv, "action", &mi.action, &mi.handler);
                            }
                        }
                    } else if (!JS_IsUndefined(iv) && !JS_IsNull(iv)) {
                        const TextSlice sl = inlay_ui_copy_text_slice(s, iv);
                        const EtchStr sv = inlay_ui_slice(s, sl);
                        if (inlay_ui_equal(sv, "---") || inlay_ui_equal(sv, "-")) {
                            mi.separator = true;
                        } else {
                            mi.label = sl;
                            mi.value = sl;
                        }
                    }
                    JS_FreeValue(s->js, iv);
                    if (JS_HasException(s->js)) {
                        JS_FreeValue(s->js, items_v);
                        return false;
                    }
                    s->menu_items[item_off + i] = mi;
                }
            }
        }
        JS_FreeValue(s->js, items_v);
    }

    s->menu_specs[idx] = spec;
    return true;
}

bool parse_avatar_widget(InlaySurface* s, JSValueConst props, bool has_props,
                         InlayUiNode& node, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->avatar_specs, s->avatar_spec_count,
                                             s->avatar_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    AvatarSpec spec = {};
    spec.sprite_id = ETCH_INVALID_SPRITE;
    spec.size_px = 32.f;
    spec.status = AvatarStatusNone;

    if (has_props) {
        const EtchStr name = inlay_ui_string_prop(s, props, "name");
        if (JS_HasException(s->js)) return false;
        if (name.length > 0u) node.text = name;

        spec.initials = inlay_ui_string_slice_prop(s, props, "initials");
        if (JS_HasException(s->js)) return false;

        uint32_t sp = 0u;
        if (inlay_ui_opt_uint_prop(s, props, "spriteId", UINT32_MAX, &sp) && sp != ETCH_INVALID_SPRITE) {
            spec.sprite_id = sp;
            spec.has_sprite = true;
        }
        if (JS_HasException(s->js)) return false;

        const JSValue img_v = JS_GetPropertyStr(s->js, props, "image");
        if (JS_IsNumber(img_v)) {
            double dv = 0.0;
            if (JS_ToFloat64(s->js, &dv, img_v) == 0 && std::isfinite(dv) && dv > 0.0 && dv <= double(UINT32_MAX)) {
                spec.sprite_id = static_cast<uint32_t>(dv);
                spec.has_sprite = true;
            }
        } else if (JS_IsString(img_v)) {
            spec.src = inlay_ui_copy_text_slice(s, img_v);
        }
        JS_FreeValue(s->js, img_v);
        if (JS_HasException(s->js)) return false;

        if (!spec.src.len) {
            spec.src = inlay_ui_string_slice_prop(s, props, "src");
            if (JS_HasException(s->js)) return false;
        }

        // size: "xs"|"sm"|"md"|"lg"|"xl"|"2xl" or numeric px
        const JSValue sz_v = JS_GetPropertyStr(s->js, props, "size");
        if (JS_IsString(sz_v)) {
            size_t n = 0u;
            const char* ch = JS_ToCStringLen(s->js, &n, sz_v);
            if (ch) {
                if (inlay_ui_str_eq(ch, n, "xs")) spec.size_px = 20.f;
                else if (inlay_ui_str_eq(ch, n, "sm")) spec.size_px = 24.f;
                else if (inlay_ui_str_eq(ch, n, "md")) spec.size_px = 32.f;
                else if (inlay_ui_str_eq(ch, n, "lg")) spec.size_px = 40.f;
                else if (inlay_ui_str_eq(ch, n, "xl")) spec.size_px = 56.f;
                else if (inlay_ui_str_eq(ch, n, "2xl")) spec.size_px = 72.f;
                else if (n > 0u && n < 32u) {
                    char buf[32] = {};
                    std::memcpy(buf, ch, n);
                    char* end = nullptr;
                    const float px = std::strtof(buf, &end);
                    if (end != buf && *end == '\0' && std::isfinite(px) && px >= 12.f && px <= 256.f) {
                        spec.size_px = px;
                    }
                }
                JS_FreeCString(s->js, ch);
            }
        } else if (!JS_IsUndefined(sz_v) && !JS_IsNull(sz_v)) {
            float px = spec.size_px;
            if (inlay_ui_opt_number_prop(s, props, "size", &px, false, 12.0, 256.0)) {
                spec.size_px = px;
            }
        }
        JS_FreeValue(s->js, sz_v);
        if (JS_HasException(s->js)) return false;

        peek_str_prop(s, props, "status", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "online")) { spec.status = AvatarStatusOnline; return true; }
            if (inlay_ui_str_eq(ch, n, "busy") || inlay_ui_str_eq(ch, n, "dnd")) {
                spec.status = AvatarStatusBusy; return true;
            }
            if (inlay_ui_str_eq(ch, n, "away") || inlay_ui_str_eq(ch, n, "idle")) {
                spec.status = AvatarStatusAway; return true;
            }
            if (inlay_ui_str_eq(ch, n, "offline")) { spec.status = AvatarStatusOffline; return true; }
            if (inlay_ui_str_eq(ch, n, "none")) { spec.status = AvatarStatusNone; return true; }
            return false;
        });

        parse_color_prop(s, props, "statusColor", &spec.status_color);
        if (parse_color_prop(s, props, "ringColor", &spec.ring_color)) spec.has_ring = true;
        if (inlay_ui_bool_prop(s, props, "ring")) spec.has_ring = true;
        if (!st.bg.set) parse_color_prop(s, props, "bg", &st.bg);
        if (!st.color.set) parse_color_prop(s, props, "color", &st.color);
    }

    s->avatar_specs[idx] = spec;
    return true;
}

bool parse_toast_widget(InlaySurface* s, JSValueConst props, bool has_props,
                        NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->toast_specs, s->toast_spec_count,
                                             s->toast_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    ToastSpec spec = {};
    spec.variant = ToastInfo;
    spec.position = ToastBottomRight;
    spec.open = true;
    spec.dismissible = true;
    spec.action_handler = INLAY_UI_NONE;
    spec.dismiss_handler = INLAY_UI_NONE;
    st.int_value = 1; // 1 = "default" accent styling, 0 = explicit "info" blue

    if (has_props) {
        inlay_ui_opt_bool_prop(s, props, "open", &spec.open);
        inlay_ui_opt_bool_prop(s, props, "dismissible", &spec.dismissible);
        inlay_ui_opt_uint_prop(s, props, "stackIndex", 32u, &spec.stack_index);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_uint_prop(s, props, "durationMs", 600000u, &spec.duration_ms);
        if (JS_HasException(s->js)) return false;

        spec.title = inlay_ui_string_slice_prop(s, props, "title");
        if (JS_HasException(s->js)) return false;
        spec.description = inlay_ui_string_slice_prop(s, props, "description");
        if (JS_HasException(s->js)) return false;
        if (!spec.description.len) {
            spec.description = inlay_ui_string_slice_prop(s, props, "text");
            if (JS_HasException(s->js)) return false;
        }
        spec.action_label = inlay_ui_string_slice_prop(s, props, "actionLabel");
        if (JS_HasException(s->js)) return false;

        peek_str_prop(s, props, "variant", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "default")) { spec.variant = ToastInfo; st.int_value = 1; return true; }
            if (inlay_ui_str_eq(ch, n, "info")) { spec.variant = ToastInfo; st.int_value = 0; return true; }
            if (inlay_ui_str_eq(ch, n, "success")) { spec.variant = ToastSuccess; st.int_value = 0; return true; }
            if (inlay_ui_str_eq(ch, n, "warning") || inlay_ui_str_eq(ch, n, "warn")) {
                spec.variant = ToastWarning; st.int_value = 0; return true;
            }
            if (inlay_ui_str_eq(ch, n, "danger") || inlay_ui_str_eq(ch, n, "error")) {
                spec.variant = ToastDanger; st.int_value = 0; return true;
            }
            return false;
        });

        peek_str_prop(s, props, "position", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "bottom-right") || inlay_ui_str_eq(ch, n, "bottomRight")) {
                spec.position = ToastBottomRight; return true;
            }
            if (inlay_ui_str_eq(ch, n, "bottom-left") || inlay_ui_str_eq(ch, n, "bottomLeft")) {
                spec.position = ToastBottomLeft; return true;
            }
            if (inlay_ui_str_eq(ch, n, "bottom-center") || inlay_ui_str_eq(ch, n, "bottomCenter")) {
                spec.position = ToastBottomCenter; return true;
            }
            if (inlay_ui_str_eq(ch, n, "top-right") || inlay_ui_str_eq(ch, n, "topRight")) {
                spec.position = ToastTopRight; return true;
            }
            if (inlay_ui_str_eq(ch, n, "top-left") || inlay_ui_str_eq(ch, n, "topLeft")) {
                spec.position = ToastTopLeft; return true;
            }
            if (inlay_ui_str_eq(ch, n, "top-center") || inlay_ui_str_eq(ch, n, "topCenter")) {
                spec.position = ToastTopCenter; return true;
            }
            return false;
        });

        float bblur = 0.f;
        if (inlay_ui_opt_number_prop(s, props, "backdropBlur", &bblur, false, 0.0, 128.0)) {
            auto* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return false;
            cold->backdrop_blur = bblur;
        }
        if (JS_HasException(s->js)) return false;

        inlay_ui_parse_event_prop(s, props, "onAction", &spec.action_name, &spec.action_handler);
        if (JS_HasException(s->js)) return false;
        if (!inlay_ui_parse_event_prop(s, props, "onClose", &spec.dismiss_action, &spec.dismiss_handler)) {
            inlay_ui_parse_event_prop(s, props, "onDismiss", &spec.dismiss_action, &spec.dismiss_handler);
        }
        if (JS_HasException(s->js)) return false;
    }

    s->toast_specs[idx] = spec;
    return true;
}

bool parse_accordion_widget(InlaySurface* s, JSValueConst props, bool has_props,
                            InlayUiNode& node, NodeStyle& st) {
    if (!has_props) return true;

    const EtchStr title = inlay_ui_string_prop(s, props, "title");
    if (JS_HasException(s->js)) return false;
    if (title.length > 0u) {
        node.text = title;
    } else if (node.text.length == 0u) {
        const EtchStr lbl = inlay_ui_string_prop(s, props, "label");
        if (JS_HasException(s->js)) return false;
        if (lbl.length > 0u) node.text = lbl;
    }

    bool def_open = false;
    if (inlay_ui_opt_bool_prop(s, props, "defaultOpen", &def_open)) {
        if (def_open) st.flags2 |= Nf2DefaultOpen;
        else st.flags2 &= uint16_t(~Nf2DefaultOpen);
    }

    bool is_open = false;
    if (inlay_ui_opt_bool_prop(s, props, "open", &is_open)) {
        st.flags2 |= Nf2HasOpen;
        if (is_open) st.flags |= NfOpen;
        else st.flags &= ~NfOpen;
    }

    bool disabled = false;
    if (inlay_ui_opt_bool_prop(s, props, "disabled", &disabled)) {
        if (disabled) st.flags |= NfDisabled;
        else st.flags &= ~NfDisabled;
    }

    uint32_t ic = 0u;
    if (inlay_ui_opt_uint_prop(s, props, "iconId", ETCH_MAX_VECTOR_ICONS - 1u, &ic)) {
        st.icon_id = static_cast<uint16_t>(ic);
        st.flags2 |= Nf2HasIcon;
    }
    if (JS_HasException(s->js)) return false;

    if (!has_event(st.action, st.handler)) {
        if (!inlay_ui_parse_event_prop(s, props, "onChange", &st.action, &st.handler)) {
            inlay_ui_parse_event_prop(s, props, "onToggle", &st.action, &st.handler);
        }
        if (JS_HasException(s->js)) return false;
    }
    return true;
}

bool parse_skeleton_widget(InlaySurface* s, JSValueConst props, bool has_props,
                           NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->skeleton_specs, s->skeleton_spec_count,
                                             s->skeleton_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    SkeletonSpec spec = {};
    spec.shape = SkeletonRect;
    spec.lines = 3u;
    spec.line_gap_px = 8.f;
    spec.animated = true;

    if (has_props) {
        auto read_shape = [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "rect") || inlay_ui_str_eq(ch, n, "box")) {
                spec.shape = SkeletonRect; return true;
            }
            if (inlay_ui_str_eq(ch, n, "circle") || inlay_ui_str_eq(ch, n, "round") ||
                inlay_ui_str_eq(ch, n, "avatar")) {
                spec.shape = SkeletonCircle; return true;
            }
            if (inlay_ui_str_eq(ch, n, "text") || inlay_ui_str_eq(ch, n, "lines")) {
                spec.shape = SkeletonText; return true;
            }
            return false;
        };
        if (!peek_str_prop(s, props, "variant", read_shape)) {
            peek_str_prop(s, props, "shape", read_shape);
        }

        uint32_t lines = 3u;
        if (inlay_ui_opt_uint_prop(s, props, "lines", 32u, &lines)) {
            spec.lines = static_cast<uint8_t>(lines > 0u ? lines : 1u);
        }
        if (JS_HasException(s->js)) return false;

        inlay_ui_opt_number_prop(s, props, "lineGap", &spec.line_gap_px, false, 0.0, 64.0);
        if (JS_HasException(s->js)) return false;

        if (!inlay_ui_opt_bool_prop(s, props, "animate", &spec.animated)) {
            inlay_ui_opt_bool_prop(s, props, "animated", &spec.animated);
        }

        parse_color_prop(s, props, "shimmerColor", &spec.shimmer_color);
    }

    s->skeleton_specs[idx] = spec;
    return true;
}

// ============================================================================
// Draw Implementations (Zero Idle JS Ticks)
// ============================================================================

void draw_doc_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                     uint32_t id, uint32_t parent_id, const EtchTheme& theme,
                     bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->doc_spec_count) return;
    DocWidgetSpec& spec = s->doc_specs[st.ext_idx];
    InlayDoc* document = nullptr;
    DocCacheEntry* entry = nullptr;
    if (spec.document_name.len) {
        const EtchStr name = inlay_ui_slice(s, spec.document_name);
        const uint32_t i = inlay_ui_content_index(s, INLAY_UI_DOC, name);
        if (i < s->content_count) document = static_cast<InlayDoc*>(s->content[i].resource);
    } else {
        entry = find_or_add_doc_cache(s, spec);
        if (entry) document = entry->doc;
    }
    if (!document) return;

    // Sync markdown content with streaming append fast-path when unchanged prefix matches.
    if (entry && (spec.content_hash != entry->content_hash || node.text.length != entry->content_len)) {
        bool updated = true;
        if (node.text.length == 0u) {
            inlay_doc_clear(entry->doc);
        } else {
            const EtchStr existing = inlay_doc_source(entry->doc);
            if (existing.length > 0u && node.text.length > existing.length &&
                existing.chars && node.text.chars &&
                std::memcmp(existing.chars, node.text.chars, existing.length) == 0) {
                const EtchStr delta = {
                    node.text.chars + existing.length,
                    node.text.length - existing.length,
                    false
                };
                updated = inlay_doc_append_utf8(entry->doc, delta);
            } else {
                updated = inlay_doc_set_utf8(entry->doc, node.text);
            }
        }
        if (updated) {
            entry->content_hash = spec.content_hash;
            entry->content_len = node.text.length;
        }
    }

    auto box = etch_box_desc_default();
    box.width = etch_sizing_grow(0.f, 0.f);
    box.height = spec.auto_height ? etch_sizing_fit(0.f, 0.f) : etch_sizing_grow(0.f, 0.f);
    inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
    box.axis = ETCH_AXIS_COL;

    const float pad_x = box.padding.left + box.padding.right;
    const float pad_y = box.padding.top + box.padding.bottom;
    const EtchRect prev_b = etch_element_bounds(id);

    float eff_w = 0.f;
    if (box.width.kind == ETCH_SIZING_FIXED && box.width.value > 0.f) {
        eff_w = box.width.value - pad_x;
    } else if (prev_b.w > pad_x + 32.f) {
        eff_w = prev_b.w - pad_x;
    } else {
        const EtchRect par_b = parent_id ? etch_element_bounds(parent_id) : EtchRect{};
        eff_w = (par_b.w > pad_x + 32.f) ? (par_b.w - pad_x) : 640.f;
        if (prev_b.w < 1.f) etch_request_redraw(s->etch);
    }
    eff_w = std::fmax(32.f, eff_w);

    float eff_h = 0.f;
    if (!spec.auto_height) {
        if (box.height.kind == ETCH_SIZING_FIXED && box.height.value > 0.f) {
            eff_h = std::fmax(1.f, box.height.value - pad_y);
        } else if (box.height.kind == ETCH_SIZING_GROW || box.height.kind == ETCH_SIZING_PERCENT) {
            if (prev_b.h > pad_y + 1.f) {
                eff_h = prev_b.h - pad_y;
            } else {
                eff_h = 320.f;
                if (prev_b.h < 1.f) etch_request_redraw(s->etch);
            }
        }
    }

    InlayDocViewDesc vd = inlay_doc_view_desc_default();
    vd.id = etch_id_index(id, 0xD0C0u);
    vd.width_px = eff_w;
    vd.height_px = eff_h;
    vd.font_size_px = std::fmin(128.f, std::fmax(6.f, (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 15.f));
    vd.block_padding_px = spec.block_padding_px;
    vd.block_gap_px = spec.block_gap_px;
    vd.stick_to_bottom = spec.stick_to_bottom;
    vd.show_code_line_numbers = spec.show_line_numbers;
    vd.hide_code_copy_button = spec.hide_copy_button;
    vd.streaming_repair = spec.streaming_repair;
    vd.image_resolver_fn = s->image_resolver_fn;
    vd.image_resolver_user_data = s->image_resolver_user_data;
    vd.code_highlight_fn = s->doc_hl_fn;
    vd.code_highlight_user_data = s->doc_hl_user_data;
    vd.custom_fence_estimate_fn = s->doc_fence_est;
    vd.custom_fence_render_fn = s->doc_fence_ren;
    vd.custom_fence_user_data = s->doc_fence_user_data;

    struct DocTaskCtx {
        InlaySurface* s;
        const DocWidgetSpec* spec;
        WidgetEvents* events;
    };
    DocTaskCtx task_ctx = {s, &spec, events};
    if (events && has_event(spec.on_task_toggle_action, spec.on_task_toggle_handler)) {
        vd.task_toggle_fn = [](uint32_t block_index, uint32_t item_index, bool checked, void* user) {
            auto* tc = static_cast<DocTaskCtx*>(user);
            char buf[96];
            const int n = std::snprintf(buf, sizeof(buf),
                "{\"blockIndex\":%u,\"itemIndex\":%u,\"checked\":%s}",
                block_index, item_index, checked ? "true" : "false");
            if (n > 0) {
                queue_slice_event(tc->s, tc->events, tc->spec->on_task_toggle_action,
                                  tc->spec->on_task_toggle_handler,
                                  inlay_ui_frame_text(tc->s, {buf, uint32_t(n), false}), true);
            }
        };
        vd.task_toggle_user_data = &task_ctx;
    }

    InlayDocStyle custom_style = inlay_doc_style_default();
    if (st.color.set || (st.flags & NfHasFontId)) {
        if (st.color.set) {
            custom_style.text_color = inlay_ui_resolve_color(st.color, theme, theme.text_body);
        }
        if (st.flags & NfHasFontId) {
            custom_style.font_id = st.font_id;
        }
        vd.style = &custom_style;
    }

    etch_begin_box(&box);
    const InlayDocResult res = inlay_doc_view(document, &vd);
    etch_end_box();

    if (res.clicked_link.length > 0u && has_event(spec.on_link_action, spec.on_link_handler)) {
        const char* kind_str = (res.clicked_kind == INLAY_INLINE_LINK) ? "link"
                             : (res.clicked_kind == INLAY_INLINE_CODE) ? "code"
                                                                       : "text";
        const uint32_t cap = frame_json_cap(res.clicked_link.length * 2u, 96u);
        auto jb = frame_json_begin(s->etch, cap);
        frame_json_lit(jb, "{\"url\":");
        frame_json_quoted(jb, res.clicked_link);
        frame_json_lit(jb, ",\"text\":");
        frame_json_quoted(jb, res.clicked_link);
        frame_json_lit(jb, ",\"kind\":\"");
        frame_json_lit(jb, kind_str);
        frame_json_lit(jb, "\"}");
        const EtchStr payload = frame_json_finish(jb);
        if (payload.length > 0u) {
            queue_slice_event(s, events, spec.on_link_action, spec.on_link_handler, payload);
        }
    }
    if (res.clicked_diagram_node.length > 0u && has_event(spec.on_diagram_node_action, spec.on_diagram_node_handler)) {
        const uint32_t cap = frame_json_cap(res.clicked_diagram_node.length, 80u);
        auto jb = frame_json_begin(s->etch, cap);
        frame_json_lit(jb, "{\"nodeId\":");
        frame_json_quoted(jb, res.clicked_diagram_node);
        char bbuf[40];
        const int bn = std::snprintf(bbuf, sizeof(bbuf), ",\"blockIndex\":%u}", res.clicked_block_index);
        if (bn > 0) frame_json_lit(jb, bbuf);
        const EtchStr payload = frame_json_finish(jb);
        if (payload.length > 0u) {
            queue_slice_event(s, events, spec.on_diagram_node_action, spec.on_diagram_node_handler, payload, true);
        }
    }

    const bool track_sel = events && has_event(spec.on_sel_action, spec.on_sel_handler);
    const bool track_find = (spec.find_query.len > 0u) || (events && has_event(spec.on_find_action, spec.on_find_handler));
    if (track_sel || track_find) {
        DocViewTrackSlot* slot = nullptr;
        for (uint32_t ti = 0u; ti < s->doc_track_slot_count; ++ti) {
            if (s->doc_track_slots[ti].element_id == id) { slot = &s->doc_track_slots[ti]; break; }
        }
        if (!slot && inlay_grow_pod(s->doc_track_slots, s->doc_track_slot_cap, s->doc_track_slot_count + 1u, 8u, 64u)) {
            slot = &s->doc_track_slots[s->doc_track_slot_count++];
            *slot = {id, false, 0u, 0u, 0u, 0u, s->frame_counter};
        }
        if (slot) {
            slot->last_used_frame = s->frame_counter;
            if (track_sel) {
                const InlayDocSelection sel = inlay_doc_view_selection(document, vd.id);
                if (sel.active != slot->sel_active ||
                    (sel.active && (sel.anchor != slot->sel_anchor || sel.focus != slot->sel_focus))) {
                    slot->sel_active = sel.active;
                    slot->sel_anchor = sel.anchor;
                    slot->sel_focus = sel.focus;
                    EtchRect srect = {};
                    if (sel.active) (void)inlay_doc_view_selection_rect(document, vd.id, &srect);
                    char plain_buf[4096] = {};
                    char md_buf[4096] = {};
                    uint32_t plen = 0u, mlen = 0u;
                    if (sel.active) {
                        plen = inlay_doc_view_copy_plain_text(document, vd.id, plain_buf, sizeof(plain_buf));
                        if (plen >= sizeof(plain_buf)) plen = sizeof(plain_buf) - 1u;
                        mlen = inlay_doc_view_copy_markdown(document, vd.id, md_buf, sizeof(md_buf));
                        if (mlen >= sizeof(md_buf)) mlen = sizeof(md_buf) - 1u;
                    }
                    const uint32_t cap = frame_json_cap(plen + mlen, 160u);
                    auto jb = frame_json_begin(s->etch, cap);
                    frame_json_lit(jb, "{\"active\":");
                    frame_json_lit(jb, sel.active ? "true" : "false");
                    frame_json_lit(jb, ",\"text\":");
                    frame_json_quoted(jb, {plain_buf, plen, false});
                    frame_json_lit(jb, ",\"markdown\":");
                    frame_json_quoted(jb, {md_buf, mlen, false});
                    char rbuf[128];
                    const int rn = std::snprintf(rbuf, sizeof(rbuf),
                        ",\"rect\":{\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f}}",
                        double(srect.x), double(srect.y), double(srect.w), double(srect.h));
                    if (rn > 0) frame_json_raw(jb, rbuf, uint32_t(rn));
                    const EtchStr payload = frame_json_finish(jb);
                    if (payload.length > 0u) {
                        queue_slice_event(s, events, spec.on_sel_action, spec.on_sel_handler, payload);
                    }
                }
            }
            if (track_find) {
                const EtchStr q = inlay_ui_slice(s, spec.find_query);
                const uint64_t qh = q.length > 0u ? inlay_fnv1a_64(q.chars, q.length) : 0u;
                if (qh != slot->last_find_hash) {
                    slot->last_find_hash = qh;
                    uint32_t count = 0u;
                    int32_t active_idx = -1;
                    if (q.length > 0u) {
                        InlayDocSelection matches[64] = {};
                        count = inlay_doc_view_find_all(document, vd.id, q, matches, 64u);
                        if (count > 0u) {
                            active_idx = 0;
                            const uint32_t lo = matches[0].anchor < matches[0].focus ? matches[0].anchor : matches[0].focus;
                            inlay_doc_view_set_selection(document, vd.id, matches[0].anchor, matches[0].focus);
                            (void)inlay_doc_view_reveal_text(document, vd.id, lo, ETCH_ALIGN_Y_CENTER);
                        }
                    } else {
                        inlay_doc_view_clear_selection(document, vd.id);
                    }
                    slot->last_find_count = count;
                    if (events && has_event(spec.on_find_action, spec.on_find_handler)) {
                        char buf[64];
                        const int n = std::snprintf(buf, sizeof(buf), "{\"count\":%u,\"activeIndex\":%d}", count, active_idx);
                        if (n > 0) {
                            queue_slice_event(s, events, spec.on_find_action, spec.on_find_handler,
                                              inlay_ui_frame_text(s, {buf, uint32_t(n), false}));
                        }
                    }
                }
            }
        }
    }
}

void draw_terminal_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                          uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->term_spec_count) return;
    const TermWidgetSpec& spec = s->term_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    TermCacheEntry* entry = find_or_add_term_cache(s, id);
    if (!entry) return;

    bool live_mode = false;
    InlayTerminalSnapshot snap = inlay_terminal_snapshot_default();
    if (spec.buffer_name.len > 0u) {
        const EtchStr bname = inlay_ui_slice(s, spec.buffer_name);
        for (uint32_t bi = 0u; bi < s->terminal_count; ++bi) {
            if (inlay_ui_equal(bname, s->terminals[bi].name) && s->terminals[bi].read_fn) {
                if (s->terminals[bi].read_fn(&snap, s->terminals[bi].user) &&
                    snap.cells && snap.columns > 0u && snap.rows > 0u) {
                    live_mode = true;
                }
                break;
            }
        }
    }

    if (!live_mode) {
        const uint32_t cols = spec.cols > 0u ? spec.cols : 80u;
        const uint32_t rows = spec.rows > 0u ? spec.rows : 24u;
        const uint64_t h = inlay_fnv1a_64(node.text.chars, node.text.length);
        if (!entry->cells || entry->content_hash != h || entry->content_len != node.text.length ||
            entry->cols != cols || entry->rows != rows) {
            parse_terminal_ansi(entry, node.text, cols, rows, h);
        }
        if (!entry->cells) return;
    }

    const bool dark = inlay_is_dark_theme(theme);
    const EtchColor default_bg = dark ? etch_rgba8(14, 16, 22, 255) : etch_rgba8(20, 22, 30, 255);
    const EtchColor default_fg = etch_rgba8(226, 232, 240, 255);
    const EtchColor term_bg = resolve_ref(st.bg, theme, default_bg);
    const EtchColor term_fg = resolve_ref(st.color, theme, default_fg);

    const float font_px = std::fmin(96.f, std::fmax(6.f, (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.f));
    const uint16_t font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_MONO);
    const float measured_m = etch_measure_text(ETCH_STR_LIT("M"), font_id, font_px, 0.f).x;
    const float cell_w = ((st.flags2 & Nf2HasValue) && st.value > 0.f)
        ? st.value
        : (measured_m > 2.f ? std::round(measured_m * 2.f) * 0.5f : font_px * 0.6f);
    const float line_h = spec.line_height_px > 0.f ? spec.line_height_px : std::round(font_px * 1.42f);

    const TextSlice title_sl = cold ? cold->placeholder : TextSlice{};
    const bool show_chrome = (st.int_value > 0) || (st.int_value == 0 && title_sl.len > 0u);
    const EtchPadding body_pad = (st.flags & NfHasPadding) ? st.padding : etch_padding_xy(12.f, 10.f);

    auto frame = etch_box_desc_default();
    frame.width = etch_sizing_fit(0.f, 0.f);
    frame.height = etch_sizing_fit(0.f, 0.f);
    frame.background = term_bg;
    frame.border_color = dark ? etch_rgba8(255, 255, 255, 22) : etch_rgba8(15, 23, 42, 36);
    frame.border_width = 1.f;
    frame.radius = etch_corners_all(theme.radius_panel_px);
    frame.continuous_corners = true;
    frame.clip_children = true;
    frame.elevation = ETCH_ELEVATION_SM;
    frame.top_rim_alpha = dark ? 0.10f : 0.0f;
    inlay_ui_populate_box_desc(s, st, theme, id, frame, use_layer);
    frame.axis = ETCH_AXIS_COL;
    frame.padding = etch_padding_all(0.f);

    etch_begin_box(&frame);

    if (show_chrome) {
        auto bar = etch_box_desc_default();
        bar.id = etch_id_index(id, 0x7E40u);
        bar.axis = ETCH_AXIS_ROW;
        bar.width = etch_sizing_grow(0.f, 0.f);
        bar.height = etch_sizing_fixed(30.f);
        bar.padding = etch_padding_xy(12.f, 0.f);
        bar.gap = 7.f;
        bar.align_y = ETCH_ALIGN_Y_CENTER;
        bar.background = etch_rgba8(255, 255, 255, 7);
        bar.border_sides = {0.f, 0.f, 0.f, 1.f};
        bar.border_color = etch_rgba8(255, 255, 255, 16);
        etch_begin_box(&bar);

        const EtchColor dots[3] = {
            etch_rgba8(255, 95, 86, 210),
            etch_rgba8(255, 189, 46, 210),
            etch_rgba8(39, 201, 63, 210)
        };
        for (uint32_t d = 0u; d < 3u; ++d) {
            auto dot = etch_box_desc_default();
            dot.width = etch_sizing_fixed(10.f);
            dot.height = etch_sizing_fixed(10.f);
            dot.radius = etch_corners_all(5.f);
            dot.background = dots[d];
            etch_box(&dot);
        }

        if (title_sl.len > 0u) {
            auto sp = etch_box_desc_default();
            sp.width = etch_sizing_fixed(6.f);
            sp.height = etch_sizing_fixed(1.f);
            etch_box(&sp);

            auto td = etch_text_desc_default();
            td.font_id = uint16_t(ETCH_FONT_MONO);
            td.font_size_px = 11.5f;
            td.color = etch_rgba8(158, 168, 188, 225);
            td.wrap = false;
            td.ellipsis = true;
            etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, title_sl)), &td);
        }
        etch_end_box();
    }

    auto body = etch_box_desc_default();
    body.id = etch_id_index(id, 0x7E41u);
    body.axis = ETCH_AXIS_COL;
    body.width = (frame.width.kind == ETCH_SIZING_FIT) ? etch_sizing_fit(0.f, 0.f) : etch_sizing_grow(0.f, 0.f);
    body.height = (frame.height.kind == ETCH_SIZING_FIT) ? etch_sizing_fit(0.f, 0.f) : etch_sizing_grow(0.f, 0.f);
    body.padding = body_pad;
    body.clip_children = true;
    body.scroll_x = !spec.auto_fit && (frame.width.kind != ETCH_SIZING_FIT);
    body.scroll_y = !spec.auto_fit && (frame.height.kind != ETCH_SIZING_FIT);
    etch_begin_box(&body);

    auto tg = etch_terminal_grid_desc_default();
    tg.id = etch_id_index(id, 0x7E4Au);
    if (live_mode) {
        tg.cells = snap.cells;
        tg.cell_count = snap.cell_count;
        tg.columns = snap.columns;
        tg.rows = snap.rows;
        tg.first_row = snap.first_row;
        tg.cursor_column = snap.cursor_column;
        tg.cursor_row = snap.cursor_row;
        tg.cursor_shape = spec.has_cursor_shape ? static_cast<EtchTerminalCursorShape>(spec.cursor_shape) : snap.cursor_shape;
        tg.cursor_visible = spec.has_cursor_visible ? spec.cursor_visible : snap.cursor_visible;
        tg.selection_start_col = snap.selection_start_col;
        tg.selection_start_row = snap.selection_start_row;
        tg.selection_end_col = snap.selection_end_col;
        tg.selection_end_row = snap.selection_end_row;
        tg.selection_block = snap.selection_block;
        tg.selection_color = snap.selection_color;
        tg.hovered_hyperlink_id = snap.hovered_hyperlink_id;
        tg.inline_images = snap.inline_images;
        tg.inline_image_count = snap.inline_image_count;
    } else {
        tg.cells = entry->cells;
        tg.cell_count = size_t(entry->cols) * size_t(entry->rows);
        tg.columns = entry->cols;
        tg.rows = entry->rows;
        tg.first_row = 0u;
        tg.cursor_column = spec.has_cursor ? spec.cursor_col : -1;
        tg.cursor_row = spec.has_cursor ? spec.cursor_row : -1;
        if (spec.has_cursor_shape) tg.cursor_shape = static_cast<EtchTerminalCursorShape>(spec.cursor_shape);
        if (spec.has_cursor_visible) tg.cursor_visible = spec.cursor_visible;
    }
    tg.cell_width_px = cell_w;
    tg.line_height_px = line_h;
    tg.font_size_px = font_px;
    tg.font_id = font_id;
    tg.foreground = term_fg;
    tg.background = term_bg;
    tg.cursor_color = resolve_ref(spec.cursor_color, theme, theme.accent_primary);
    tg.cursor_motion = etch_spring_spec_snappy();
    tg.auto_fit_container = spec.auto_fit;
    tg.focusable = spec.focusable;
    tg.interactive_selection = spec.interactive_selection;
    if (spec.has_palette) {
        tg.ansi_palette = spec.palette;
        tg.ansi_palette_count = 16u;
    }
    tg.capture_keyboard_input = spec.capture_input;
    tg.bracketed_paste = spec.bracketed_paste;
    tg.mouse_reporting = spec.mouse_reporting;
    if (spec.search_color.set) tg.search_match_color = inlay_ui_resolve_color(spec.search_color, theme, tg.search_match_color);
    if (spec.search_active_color.set) tg.search_active_color = inlay_ui_resolve_color(spec.search_active_color, theme, tg.search_active_color);
    if (spec.search_cnt > 0u && spec.search_off + spec.search_cnt <= s->term_search_match_count) {
        auto* matches = static_cast<EtchTerminalSearchMatch*>(etch_frame_arena_alloc(
            s->etch, size_t(spec.search_cnt) * sizeof(EtchTerminalSearchMatch), alignof(EtchTerminalSearchMatch)));
        if (matches) {
            for (uint32_t mi = 0u; mi < spec.search_cnt; ++mi) {
                const auto& m = s->term_search_matches[spec.search_off + mi];
                matches[mi] = {m.row, m.start_col, m.end_col, m.active};
            }
            tg.search_matches = matches;
            tg.search_match_count = spec.search_cnt;
        }
    }
    const EtchTerminalGridResult res = etch_terminal_grid_ex(&tg);

    if (events) {
        if ((res.clicked || res.right_clicked) && has_event(spec.on_cell_click_action, spec.on_cell_click_handler)) {
            char payload[96];
            const int n = std::snprintf(payload, sizeof(payload),
                "{\"col\":%d,\"row\":%d,\"right\":%s}",
                res.clicked_col, res.clicked_row, res.right_clicked ? "true" : "false");
            if (n > 0) {
                queue_slice_event(s, events, spec.on_cell_click_action, spec.on_cell_click_handler,
                                  inlay_ui_frame_text(s, {payload, uint32_t(n), false}));
            }
        }
        if (res.clicked_hyperlink_id != 0u && has_event(spec.on_link_click_action, spec.on_link_click_handler)) {
            char payload[96];
            const int n = std::snprintf(payload, sizeof(payload),
                "{\"hyperlinkId\":%u,\"col\":%d,\"row\":%d}",
                res.clicked_hyperlink_id, res.clicked_col, res.clicked_row);
            if (n > 0) {
                queue_slice_event(s, events, spec.on_link_click_action, spec.on_link_click_handler,
                                  inlay_ui_frame_text(s, {payload, uint32_t(n), false}));
            }
        }
        if (res.selection_changed && has_event(spec.on_sel_action, spec.on_sel_handler)) {
            char payload[128];
            const int n = std::snprintf(payload, sizeof(payload),
                "{\"hasSelection\":%s,\"startCol\":%d,\"startRow\":%d,\"endCol\":%d,\"endRow\":%d}",
                res.has_selection ? "true" : "false",
                res.sel_start_col, res.sel_start_row, res.sel_end_col, res.sel_end_row);
            if (n > 0) {
                queue_slice_event(s, events, spec.on_sel_action, spec.on_sel_handler,
                                  inlay_ui_frame_text(s, {payload, uint32_t(n), false}));
            }
        }
        if (std::fabs(res.wheel_delta_y) > 1e-4f && has_event(spec.on_wheel_action, spec.on_wheel_handler)) {
            char payload[48];
            const int n = std::snprintf(payload, sizeof(payload), "{\"deltaY\":%.3f}", double(res.wheel_delta_y));
            if (n > 0) {
                queue_slice_event(s, events, spec.on_wheel_action, spec.on_wheel_handler,
                                  inlay_ui_frame_text(s, {payload, uint32_t(n), false}));
            }
        }
        if (res.visible_columns > 0u && res.visible_rows > 0u &&
            (res.visible_columns != entry->last_vis_cols || res.visible_rows != entry->last_vis_rows)) {
            entry->last_vis_cols = res.visible_columns;
            entry->last_vis_rows = res.visible_rows;
            if (has_event(spec.on_resize_action, spec.on_resize_handler)) {
                char payload[64];
                const int n = std::snprintf(payload, sizeof(payload),
                    "{\"cols\":%u,\"rows\":%u}", res.visible_columns, res.visible_rows);
                if (n > 0) {
                    queue_slice_event(s, events, spec.on_resize_action, spec.on_resize_handler,
                                      inlay_ui_frame_text(s, {payload, uint32_t(n), false}));
                }
            }
        }
        if ((res.typed_text.length > 0u || res.key_pressed != 0u) &&
            has_event(spec.on_input_action, spec.on_input_handler)) {
            char vt_buf[64] = {};
            uint32_t vt_len = 0u;
            if (res.key_pressed != 0u) {
                vt_len = etch_terminal_encode_key(res.key_pressed, res.key_mods, spec.app_cursor, vt_buf, sizeof(vt_buf));
                if (vt_len >= sizeof(vt_buf)) vt_len = sizeof(vt_buf) - 1u;
            }
            const EtchStr vt_str = vt_len > 0u ? EtchStr{vt_buf, vt_len, false} : res.typed_text;
            const uint32_t cap = frame_json_cap(res.typed_text.length + vt_str.length, 128u);
            auto jb = frame_json_begin(s->etch, cap);
            frame_json_lit(jb, "{\"text\":");
            frame_json_quoted(jb, res.typed_text);
            char mid[96];
            const int mn = std::snprintf(mid, sizeof(mid),
                ",\"keyCode\":%u,\"mods\":%u,\"repeat\":%s,\"vt\":",
                res.key_pressed, uint32_t(res.key_mods), res.key_repeat ? "true" : "false");
            if (mn > 0) frame_json_raw(jb, mid, uint32_t(mn));
            frame_json_quoted(jb, vt_str);
            frame_json_lit(jb, "}");
            const EtchStr payload = frame_json_finish(jb);
            if (payload.length > 0u) {
                queue_slice_event(s, events, spec.on_input_action, spec.on_input_handler, payload);
            }
        }
        if (res.pasted_text.length > 0u && has_event(spec.on_paste_action, spec.on_paste_handler)) {
            const uint32_t cap = frame_json_cap(res.pasted_text.length, 32u);
            auto jb = frame_json_begin(s->etch, cap);
            frame_json_lit(jb, "{\"text\":");
            frame_json_quoted(jb, res.pasted_text);
            frame_json_lit(jb, "}");
            const EtchStr payload = frame_json_finish(jb);
            if (payload.length > 0u) {
                queue_slice_event(s, events, spec.on_paste_action, spec.on_paste_handler, payload);
            }
        }
        if ((res.mouse_down || res.mouse_released || res.mouse_moved) &&
            has_event(spec.on_mouse_action, spec.on_mouse_handler)) {
            char sgr[48] = {};
            const bool pressed = res.mouse_down || (res.mouse_moved && res.mouse_button != 0u);
            const uint32_t slen = etch_terminal_encode_mouse_sgr(res.mouse_button, res.hovered_col, res.hovered_row,
                                                                 etch_key_mods(), pressed, res.mouse_moved,
                                                                 sgr, sizeof(sgr));
            const uint32_t cap = frame_json_cap(slen, 140u);
            auto jb = frame_json_begin(s->etch, cap);
            char head[128];
            const int hn = std::snprintf(head, sizeof(head),
                "{\"button\":%u,\"col\":%d,\"row\":%d,\"down\":%s,\"released\":%s,\"moved\":%s,\"sgr\":",
                uint32_t(res.mouse_button), res.hovered_col, res.hovered_row,
                res.mouse_down ? "true" : "false",
                res.mouse_released ? "true" : "false",
                res.mouse_moved ? "true" : "false");
            if (hn > 0) frame_json_raw(jb, head, uint32_t(hn));
            frame_json_quoted(jb, {sgr, slen < sizeof(sgr) ? slen : (sizeof(sgr) - 1u), false});
            frame_json_lit(jb, "}");
            const EtchStr payload = frame_json_finish(jb);
            if (payload.length > 0u) {
                queue_slice_event(s, events, spec.on_mouse_action, spec.on_mouse_handler, payload);
            }
        }
    }

    etch_end_box();
    etch_end_box();
}

void draw_virtual_list_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                              uint32_t id, const EtchTheme& theme, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->vlist_spec_count) return;
    const VirtualListSpec& spec = s->vlist_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    const bool model_mode = (st.flags2 & Nf2Leaf) != 0u && cold && cold->placeholder.len > 0u;
    const bool data_mode = !model_mode && (st.flags2 & Nf2Selected) != 0u;
    const bool has_render_item = !model_mode && (st.flags2 & Nf2HasValue) != 0u;

    const InlayModel* model = nullptr;
    if (model_mode) {
        const uint32_t ci = inlay_ui_content_index(s, INLAY_UI_VIRTUAL_LIST, inlay_ui_slice(s, cold->placeholder));
        if (ci < s->content_count) model = static_cast<const InlayModel*>(s->content[ci].resource);
    }

    uint32_t child_count = 0u;
    if (!model_mode && !data_mode) {
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            ++child_count;
        }
    }

    const uint32_t total_count = model_mode
        ? (model ? inlay_model_count(model) : 0u)
        : (spec.total_count > 0u
            ? spec.total_count
            : (data_mode ? static_cast<uint32_t>(st.int_value > 0 ? st.int_value : 0) : child_count));

    auto vd = etch_virtual_list_desc_default();
    vd.id = id;
    vd.item_count = total_count;
    vd.item_height_px = spec.item_height_px > 0.f ? spec.item_height_px : 36.f;
    vd.row_gap_px = spec.row_gap_px;
    vd.overscan_rows = spec.overscan_rows;
    vd.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
    vd.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(320.f));
    if (st.flags & NfHasPadding) vd.padding = st.padding;
    if (st.bg.set) vd.background = inlay_ui_resolve_color(st.bg, theme, theme.bg_surface);
    if (st.border_color.set) vd.border_color = inlay_ui_resolve_color(st.border_color, theme, theme.border_subtle);
    if (st.flags & NfHasBorderWidth) vd.border_width = std::fmax(0.f, st.border_width);
    if (st.flags & NfRadiusSet) vd.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_panel_px);
    vd.show_scrollbar = spec.show_scrollbar;
    vd.scroll_fade_px = spec.scroll_fade_px;
    vd.stick_to_bottom = spec.stick_to_bottom;

    const double stride = double(vd.item_height_px) +
                          double(vd.row_gap_px > 0.f ? std::round(vd.row_gap_px) : 0.f);
    ModelSelectionEntry* sel = model_mode ? find_or_add_model_selection(s, id) : nullptr;
    bool has_post_scroll_target = false;
    double post_scroll_target_y = 0.0;
    if (model_mode && model && sel) {
        const uint64_t cur_rev = inlay_model_revision(model);
        if (sel->last_model == model && sel->last_model_rev != cur_rev) {
            if (!(vd.stick_to_bottom && etch_virtual_list_is_at_bottom(id))) {
                const double cur_y = etch_virtual_list_scroll_offset(id);
                const double adj_y = cur_y > double(vd.padding.top) ? (cur_y - double(vd.padding.top)) : 0.0;
                const uint32_t first_visible =
                    stride > 0.0 ? static_cast<uint32_t>(std::floor(adj_y / stride)) : 0u;
                int64_t row_delta = 0;
                if (inlay_ui_model_replay_scroll_delta(model, sel->last_model_rev,
                                                       first_visible, &row_delta)) {
                    const double target_y = std::fmax(0.0, cur_y + double(row_delta) * stride);
                    etch_set_virtual_list_scroll_offset(id, target_y, true);
                    has_post_scroll_target = true;
                    post_scroll_target_y = target_y;
                }
            }
            sel->last_model_rev = cur_rev;
        } else if (sel->last_model != model) {
            sel->last_model = model;
            sel->last_model_rev = cur_rev;
        }
    }

    const EtchVirtualListSlice slice = etch_begin_virtual_list(&vd);
    if (has_post_scroll_target && post_scroll_target_y > slice.scroll_y + 0.5 &&
        post_scroll_target_y <= slice.max_scroll_y + 0.5) {
        etch_set_virtual_list_scroll_offset(id, post_scroll_target_y, true);
        etch_request_redraw(s->etch);
    }

    if (model_mode) {
        const bool controlled_sel = (st.flags2 & Nf2HasMin) != 0u;
        const bool dark = inlay_is_dark_theme(theme);
        const EtchColor hover_bg = (cold && cold->hover_bg.set)
            ? inlay_ui_resolve_color(cold->hover_bg, theme, theme.surface_hover)
            : (dark ? etch_rgba8(255, 255, 255, 10) : etch_rgba8(15, 23, 42, 8));
        const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.5f;
        const EtchColor primary_fg = resolve_ref(st.color, theme, theme.text_primary);
        const bool has_select_ev = has_event(st.action, st.handler);

        for (uint32_t i = slice.first_index; i < slice.end_index; ++i) {
            InlayModelRow mrow = {};
            const bool has_row = model && inlay_ui_model_read(model, i, &mrow);
            const bool is_selected = controlled_sel
                ? (st.int_value >= 0 && static_cast<uint32_t>(st.int_value) == i)
                : (sel && sel->has_selection &&
                   (has_row && mrow.row_id != 0u ? (sel->selected_row_id == mrow.row_id) : (sel->selected_index == i)));

            const uint32_t row_key = (has_row && mrow.row_id != 0u)
                ? static_cast<uint32_t>(mrow.row_id ^ (mrow.row_id >> 32))
                : (0x710000u + i);
            const uint32_t row_id = etch_id_index(id, row_key);
            auto row = etch_box_desc_default();
            row.id = row_id;
            row.axis = ETCH_AXIS_ROW;
            row.width = etch_sizing_grow(0.f, 0.f);
            row.height = etch_sizing_fixed(vd.item_height_px);
            row.padding = etch_padding_xy(12.f, 0.f);
            row.gap = 10.f;
            row.align_y = ETCH_ALIGN_Y_CENTER;
            row.background = is_selected ? theme.surface_selected : EtchColor{};
            row.hover_background = is_selected ? theme.surface_selected : hover_bg;
            row.border_sides = {0.f, 0.f, 0.f, 1.f};
            row.border_color = theme.border_subtle;
            row.focusable = true;
            etch_begin_box(&row);

            const EtchStr label_ft = (has_row && mrow.label.length > 0u) ? inlay_ui_frame_text(s, mrow.label) : EtchStr{};
            const EtchStr detail_ft = (has_row && mrow.detail.length > 0u) ? inlay_ui_frame_text(s, mrow.detail) : EtchStr{};
            const EtchStr badge_ft = (has_row && mrow.badge.length > 0u) ? inlay_ui_frame_text(s, mrow.badge) : EtchStr{};

            etch_emit_a11y_node(row_id, ETCH_A11Y_LIST_ITEM, label_ft, detail_ft,
                                is_selected ? uint32_t(ETCH_A11Y_STATE_SELECTED) : 0u, 3u, 0.f, {});

            if (has_row) {
                auto left = etch_box_desc_default();
                left.axis = ETCH_AXIS_COL;
                left.width = etch_sizing_grow(0.f, 0.f);
                left.height = etch_sizing_fit(0.f, 0.f);
                left.gap = 1.f;
                left.shrink_width = true;
                etch_begin_box(&left);

                if (label_ft.length > 0u) {
                    auto td = etch_text_desc_default();
                    td.font_size_px = font_px;
                    td.font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SANS);
                    td.color = primary_fg;
                    td.wrap = false;
                    td.ellipsis = true;
                    etch_text(label_ft, &td);
                }
                if (detail_ft.length > 0u) {
                    auto sd = etch_text_desc_default();
                    sd.font_size_px = std::fmax(10.f, font_px - 2.f);
                    sd.color = theme.text_muted;
                    sd.wrap = false;
                    sd.ellipsis = true;
                    etch_text(detail_ft, &sd);
                }
                etch_end_box();

                if (badge_ft.length > 0u) {
                    auto b = etch_box_desc_default();
                    b.padding = etch_padding_xy(7.f, 2.f);
                    b.background = theme.surface_subtle;
                    b.border_width = 1.f;
                    b.border_color = theme.border_subtle;
                    b.radius = etch_corners_all(theme.radius_pill_px);
                    b.align_y = ETCH_ALIGN_Y_CENTER;
                    etch_begin_box(&b);
                    auto bt = etch_text_desc_default();
                    bt.font_size_px = 11.f;
                    bt.font_id = uint16_t(ETCH_FONT_MEDIUM);
                    bt.color = theme.text_secondary;
                    bt.wrap = false;
                    etch_text(badge_ft, &bt);
                    etch_end_box();
                }
            }
            etch_end_box();

            if (etch_hovered(row_id)) etch_set_cursor(ETCH_CURSOR_HAND);
            if (is_activated(row_id)) {
                if (!controlled_sel && sel) {
                    sel->has_selection = true;
                    sel->selected_row_id = has_row ? mrow.row_id : 0u;
                    sel->selected_index = i;
                    etch_request_redraw(s->etch);
                }
                if (has_select_ev) {
                    const uint32_t cap = frame_json_cap(label_ft.length, 64u);
                    auto jb = frame_json_begin(s->etch, cap);
                    frame_json_lit(jb, "{\"index\":");
                    frame_json_u32(jb, i);
                    frame_json_lit(jb, ",\"rowId\":");
                    frame_json_u64(jb, has_row ? mrow.row_id : 0u);
                    frame_json_lit(jb, ",\"label\":");
                    frame_json_quoted(jb, label_ft);
                    frame_json_lit(jb, "}");
                    queue_slice_event(s, events, st.action, st.handler, frame_json_finish(jb), true);
                }
            }
        }
        etch_end_virtual_list();
        return;
    }

    if (data_mode) {
        const uint32_t data_count = st.int_value > 0 ? static_cast<uint32_t>(st.int_value) : 0u;
        const uint32_t data_off = cold ? cold->item_offset : 0u;
        const bool dark = inlay_is_dark_theme(theme);
        const EtchColor hover_bg = (cold && cold->hover_bg.set)
            ? inlay_ui_resolve_color(cold->hover_bg, theme, theme.surface_hover)
            : (dark ? etch_rgba8(255, 255, 255, 10) : etch_rgba8(15, 23, 42, 8));
        const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.5f;
        const EtchColor primary_fg = resolve_ref(st.color, theme, theme.text_primary);
        const bool clickable = has_event(st.action, st.handler);

        for (uint32_t i = slice.first_index; i < slice.end_index; ++i) {
            const uint32_t row_id = etch_id_index(id, 0x710000u + i);
            auto row = etch_box_desc_default();
            row.id = row_id;
            row.axis = ETCH_AXIS_ROW;
            row.width = etch_sizing_grow(0.f, 0.f);
            row.height = etch_sizing_fixed(vd.item_height_px);
            row.padding = etch_padding_xy(12.f, 0.f);
            row.gap = 10.f;
            row.align_y = ETCH_ALIGN_Y_CENTER;
            row.hover_background = hover_bg;
            row.border_sides = {0.f, 0.f, 0.f, 1.f};
            row.border_color = theme.border_subtle;
            row.focusable = clickable;
            etch_begin_box(&row);

            if (i < data_count) {
                const TextSlice title_sl = s->table_cells[data_off + i * 4u + 0u];
                const TextSlice sub_sl   = s->table_cells[data_off + i * 4u + 1u];
                const TextSlice badge_sl = s->table_cells[data_off + i * 4u + 2u];
                const TextSlice meta_sl  = s->table_cells[data_off + i * 4u + 3u];

                auto left = etch_box_desc_default();
                left.axis = ETCH_AXIS_COL;
                left.width = etch_sizing_grow(0.f, 0.f);
                left.height = etch_sizing_fit(0.f, 0.f);
                left.gap = 1.f;
                left.shrink_width = true;
                etch_begin_box(&left);

                if (title_sl.len > 0u) {
                    auto td = etch_text_desc_default();
                    td.font_size_px = font_px;
                    td.font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SANS);
                    td.color = primary_fg;
                    td.wrap = false;
                    td.ellipsis = true;
                    etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, title_sl)), &td);
                }
                if (sub_sl.len > 0u) {
                    auto sd = etch_text_desc_default();
                    sd.font_size_px = std::fmax(10.f, font_px - 2.f);
                    sd.color = theme.text_muted;
                    sd.wrap = false;
                    sd.ellipsis = true;
                    etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, sub_sl)), &sd);
                }
                etch_end_box();

                if (badge_sl.len > 0u) {
                    auto b = etch_box_desc_default();
                    b.padding = etch_padding_xy(7.f, 2.f);
                    b.background = theme.surface_subtle;
                    b.border_width = 1.f;
                    b.border_color = theme.border_subtle;
                    b.radius = etch_corners_all(theme.radius_pill_px);
                    b.align_y = ETCH_ALIGN_Y_CENTER;
                    etch_begin_box(&b);
                    auto bt = etch_text_desc_default();
                    bt.font_size_px = 11.f;
                    bt.font_id = uint16_t(ETCH_FONT_MEDIUM);
                    bt.color = theme.text_secondary;
                    bt.wrap = false;
                    etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, badge_sl)), &bt);
                    etch_end_box();
                }

                if (meta_sl.len > 0u) {
                    auto mt = etch_text_desc_default();
                    mt.font_size_px = 11.5f;
                    mt.font_id = uint16_t(ETCH_FONT_MONO);
                    mt.opentype_flags = ETCH_OT_TABULAR_NUMS;
                    mt.color = theme.text_muted;
                    mt.wrap = false;
                    etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, meta_sl)), &mt);
                }
            }
            etch_end_box();

            if (clickable) {
                if (etch_hovered(row_id)) etch_set_cursor(ETCH_CURSOR_HAND);
                if (is_activated(row_id)) {
                    auto jb = frame_json_begin(s->etch, 32u);
                    frame_json_lit(jb, "{\"index\":");
                    frame_json_u32(jb, i);
                    frame_json_lit(jb, "}");
                    queue_slice_event(s, events, st.action, st.handler, frame_json_finish(jb), true);
                }
            }
        }
        etch_end_virtual_list();
        return;
    }

    // Windowed child / renderItem mode (keyed by root-relative spec.path_id so separate panes never collide).
    const bool has_on_range = has_event(spec.on_range_action, spec.on_range_handler);
    const uint32_t cache_key = spec.path_id ? spec.path_id : id;
    VirtualListWinEntry* win = (has_render_item || has_on_range)
        ? find_or_add_vlist_cache(s, cache_key, spec.window_start, spec.window_start + child_count)
        : nullptr;
    if (win) {
        win->total_count = total_count;
        win->item_height_px = vd.item_height_px;
        win->scroll_y = slice.scroll_y;
    }

    uint32_t* child_indices = nullptr;
    if (child_count > 0u) {
        child_indices = static_cast<uint32_t*>(
            etch_frame_arena_alloc(s->etch, size_t(child_count) * sizeof(uint32_t), alignof(uint32_t)));
        if (child_indices) {
            uint32_t k = 0u;
            for (uint32_t at = node.first_child; at != INLAY_UI_NONE && k < child_count; at = s->nodes[at].next_sibling) {
                child_indices[k++] = at;
            }
        }
    }

    const uint32_t win_end = spec.window_start + child_count;
    const bool prev_col = s->draw_parent_col;
    s->draw_parent_col = true;
    for (uint32_t row_idx = slice.first_index; row_idx < slice.end_index; ++row_idx) {
        if (child_indices && row_idx >= spec.window_start && row_idx < win_end) {
            inlay_ui_draw_node(s, child_indices[row_idx - spec.window_start], id, theme, events);
        } else {
            auto ph = etch_box_desc_default();
            ph.width = etch_sizing_grow(0.f, 0.f);
            ph.height = etch_sizing_fixed(vd.item_height_px);
            etch_box(&ph);
        }
    }
    s->draw_parent_col = prev_col;

    etch_end_virtual_list();

    // If scrolling escapes materialized child window, request one bounded window refill.
    const bool escaped = (slice.end_index > slice.first_index) &&
                         (slice.first_index < spec.window_start || slice.end_index > win_end);
    if (escaped && total_count > 0u && win && (has_render_item || has_on_range)) {
        constexpr uint32_t kPrefetchWindow = 256u;
        uint32_t buf = spec.overscan_rows > 0u ? spec.overscan_rows * 2u : 16u;
        const uint32_t vis = slice.end_index - slice.first_index;
        if (vis >= kPrefetchWindow) {
            buf = 0u;
        } else if (buf * 2u > kPrefetchWindow - vis) {
            buf = (kPrefetchWindow - vis) / 2u;
        }
        const uint32_t want_first = (slice.first_index > buf) ? (slice.first_index - buf) : 0u;
        const uint32_t want_end = (slice.end_index + buf < total_count) ? (slice.end_index + buf) : total_count;
        if (win->first_index != want_first || win->end_index != want_end) {
            win->first_index = want_first;
            win->end_index = want_end;
            if (has_on_range) {
                auto jb = frame_json_begin(s->etch, 128u);
                frame_json_lit(jb, "{\"first\":");
                frame_json_u32(jb, want_first);
                frame_json_lit(jb, ",\"end\":");
                frame_json_u32(jb, want_end);
                frame_json_lit(jb, ",\"visibleFirst\":");
                frame_json_u32(jb, slice.first_index);
                frame_json_lit(jb, ",\"visibleEnd\":");
                frame_json_u32(jb, slice.end_index);
                frame_json_lit(jb, ",\"total\":");
                frame_json_u32(jb, total_count);
                frame_json_lit(jb, "}");
                const EtchStr payload = frame_json_finish(jb);
                if (payload.length > 0u) {
                    queue_slice_event(s, events, spec.on_range_action, spec.on_range_handler, payload);
                }
            } else if (has_render_item) {
                etch_request_redraw(s->etch);
            }
        }
    }
}

void draw_table_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                       const EtchTheme& theme, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->table_spec_count) return;
    const TableSpec& spec = s->table_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    if (spec.col_count == 0u) {
        auto empty = etch_box_desc_default();
        empty.id = id;
        empty.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
        empty.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(spec.header_height_px + spec.row_height_px));
        empty.background = resolve_ref(st.bg, theme, theme.bg_table);
        empty.border_color = resolve_ref(st.border_color, theme, theme.border_line);
        empty.border_width = 1.f;
        empty.radius = etch_corners_all(theme.radius_table_px);
        etch_box(&empty);
        return;
    }

    const uint32_t numeric_mask = static_cast<uint32_t>(st.int_value) & 0xFFFFu;
    EtchTableColumnDesc cols[ETCH_MAX_TABLE_COLUMNS] = {};
    for (uint32_t c = 0u; c < spec.col_count; ++c) {
        const TableColSpec& cs = s->table_cols[spec.col_offset + c];
        cols[c] = etch_table_column_desc_default();
        cols[c].id = etch_id_index(id, 0x7AB0u + c);
        cols[c].title = inlay_ui_frame_text(s, inlay_ui_slice(s, cs.title));
        cols[c].sizing = static_cast<EtchTableColumnSizing>(cs.sizing);
        cols[c].width = cs.width_px;
        cols[c].min_width = cs.min_width_px;
        cols[c].max_width = cs.max_width_px;
        cols[c].align_x = static_cast<EtchAlignX>(cs.align_x);
        cols[c].resizable = cs.resizable;
        cols[c].sortable = cs.sortable;
    }

    const float cell_font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.f;
    const float auto_h = std::fmin(
        420.f,
        spec.header_height_px + float(spec.row_count > 0u ? spec.row_count : 1u) * spec.row_height_px + 4.f);

    auto td = etch_table_desc_default();
    td.id = id;
    td.columns = cols;
    td.column_count = spec.col_count;
    td.item_count = spec.row_count;
    td.header_height_px = spec.header_height_px;
    td.row_height_px = spec.row_height_px;
    td.cell_padding_x = spec.cell_padding_x;
    td.header_font_size_px = std::fmax(10.f, cell_font_px - 1.f);
    td.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
    td.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(auto_h));
    if (st.flags & NfHasPadding) td.padding = st.padding;
    if (st.bg.set) td.background = inlay_ui_resolve_color(st.bg, theme, theme.bg_table);
    if (spec.header_bg.set) td.header_background = inlay_ui_resolve_color(spec.header_bg, theme, theme.bg_elevated);
    if (!spec.zebra) td.zebra_color = etch_color_transparent();
    else if (spec.zebra_bg.set) td.zebra_color = inlay_ui_resolve_color(spec.zebra_bg, theme, theme.zebra_row);
    if (spec.hover_row_bg.set) td.hover_row_color = inlay_ui_resolve_color(spec.hover_row_bg, theme, theme.selection_hover);
    if (spec.selected_row_bg.set) td.selected_row_color = inlay_ui_resolve_color(spec.selected_row_bg, theme, theme.surface_selected);
    if (st.border_color.set) td.border_color = inlay_ui_resolve_color(st.border_color, theme, theme.border_line);
    if (st.flags & NfHasBorderWidth) td.border_width = std::fmax(0.f, st.border_width);
    if (st.flags & NfRadiusSet) td.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_table_px);
    td.show_scrollbar = true;
    td.show_column_lines = spec.show_column_lines;
    td.horizontal_scroll = spec.horizontal_scroll;
    td.keyboard_navigation = true;
    td.scroll_fade_px = 12.f;
    TableSortCache* cache = find_or_add_table_sort_cache(s, id);
    bool synced_controlled_sort = false;
    if (spec.has_sort && spec.sort_col >= 0 && uint32_t(spec.sort_col) < spec.col_count) {
        const uint32_t want_col = static_cast<uint32_t>(spec.sort_col);
        td.default_sort_column = want_col;
        td.default_sort_descending = spec.sort_desc;
        if (cache && (!cache->controlled_applied ||
                      cache->last_controlled_col != spec.sort_col ||
                      cache->last_controlled_desc != spec.sort_desc)) {
            etch_set_table_sort(id, want_col, spec.sort_desc);
            cache->controlled_applied = true;
            cache->last_controlled_col = spec.sort_col;
            cache->last_controlled_desc = spec.sort_desc;
            synced_controlled_sort = true;
        }
    } else if (cache) {
        cache->controlled_applied = false;
        cache->last_controlled_col = -1;
        cache->last_controlled_desc = false;
    }

    const EtchTableResult res = etch_begin_table(&td);

    // Maintain cached index-indirection sort order (0 JS ticks when sorting natively).
    const bool sort_col_valid = res.sort_column < spec.col_count &&
                                s->table_cols[spec.col_offset + res.sort_column].sortable;
    const uint64_t data_hash = compute_table_data_hash(s, spec, numeric_mask);
    const uint32_t target_sort_col = sort_col_valid ? res.sort_column : UINT32_MAX;
    if (cache && (cache->data_hash != data_hash || cache->row_count != spec.row_count ||
                  cache->sort_col != target_sort_col || cache->sort_desc != res.sort_descending)) {
        const bool is_num = sort_col_valid && ((numeric_mask & (1u << res.sort_column)) != 0u);
        rebuild_table_sort_order(s, cache, spec, res.sort_column, is_num, res.sort_descending,
                                 sort_col_valid, data_hash);
    }

    const EtchColor cell_fg = resolve_ref(st.color, theme, theme.text_primary);
    const EtchColor sel_bg = resolve_ref(spec.selected_row_bg, theme, theme.surface_selected);
    const uint16_t base_font = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SANS);

    uint32_t peek_disp = res.slice.first_index;
    uint32_t disp_row = 0u;
    int32_t clicked_orig_row = -1;

    while (peek_disp < res.slice.end_index) {
        const uint32_t orig_row =
            (cache && cache->row_order && peek_disp < cache->row_count) ? cache->row_order[peek_disp] : peek_disp;
        const EtchColor row_bg = (spec.selected_row >= 0 && int32_t(orig_row) == spec.selected_row)
            ? sel_bg
            : EtchColor{};
        if (!etch_table_next_row(&disp_row, row_bg, {})) break;

        for (uint32_t c = 0u; c < spec.col_count && etch_table_next_column(); ++c) {
            const TextSlice cell_sl = s->table_cells[spec.cell_offset + orig_row * spec.col_count + c];
            const bool is_num = (numeric_mask & (1u << c)) != 0u;
            auto tx = etch_text_desc_default();
            tx.font_size_px = cell_font_px;
            tx.font_id = is_num ? uint16_t(ETCH_FONT_MONO) : base_font;
            if (is_num) tx.opentype_flags = ETCH_OT_TABULAR_NUMS;
            tx.color = cell_fg;
            tx.align_x = static_cast<EtchAlignX>(s->table_cols[spec.col_offset + c].align_x);
            tx.wrap = false;
            tx.ellipsis = true;
            etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, cell_sl)), &tx);
        }

        const EtchTableRowResult rr = etch_table_row_result(id, disp_row);
        if (rr.clicked) clicked_orig_row = static_cast<int32_t>(orig_row);
        ++peek_disp;
    }

    etch_end_table();

    if (res.sort_changed && !synced_controlled_sort && has_event(spec.on_sort_action, spec.on_sort_handler)) {
        const EtchStr key = (res.sort_column < spec.col_count)
            ? inlay_ui_slice(s, s->table_cols[spec.col_offset + res.sort_column].key)
            : EtchStr{};
        auto jb = frame_json_begin(s->etch, frame_json_cap(key.length, 80u));
        frame_json_lit(jb, "{\"column\":");
        frame_json_u32(jb, res.sort_column);
        frame_json_lit(jb, ",\"key\":");
        frame_json_quoted(jb, key);
        frame_json_lit(jb, ",\"descending\":");
        frame_json_bool(jb, res.sort_descending);
        frame_json_lit(jb, "}");
        const EtchStr payload = frame_json_finish(jb);
        if (payload.length > 0u) {
            queue_slice_event(s, events, spec.on_sort_action, spec.on_sort_handler, payload);
        }
    }

    if (clicked_orig_row >= 0 && has_event(st.action, st.handler)) {
        auto jb = frame_json_begin(s->etch, 32u);
        frame_json_lit(jb, "{\"row\":");
        frame_json_i32(jb, clicked_orig_row);
        frame_json_lit(jb, "}");
        queue_slice_event(s, events, st.action, st.handler, frame_json_finish(jb), true);
    }
}

void draw_tree_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                      uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    const bool model_mode = (st.flags2 & Nf2Leaf) != 0u && cold && cold->placeholder.len > 0u;

    if (model_mode) {
        const InlayModel* model = nullptr;
        const uint32_t ci = inlay_ui_content_index(s, INLAY_UI_VIRTUAL_LIST, inlay_ui_slice(s, cold->placeholder));
        if (ci < s->content_count) model = static_cast<const InlayModel*>(s->content[ci].resource);

        const uint32_t total = model ? inlay_model_count(model) : 0u;
        const float row_h = (st.flags2 & Nf2HasValue) && st.value > 0.f ? st.value : 28.f;
        const float indent_px = (st.flags2 & Nf2HasMin) && cold && cold->min_val > 0.f ? cold->min_val : 16.f;
        const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.f;
        const uint16_t font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SANS);
        const EtchColor fg_col = resolve_ref(st.color, theme, theme.text_primary);
        const EtchColor hov_col = (cold && cold->hover_bg.set)
            ? inlay_ui_resolve_color(cold->hover_bg, theme, theme.surface_hover)
            : theme.surface_hover;
        const bool has_select_ev = has_event(st.action, st.handler);
        ModelSelectionEntry* sel = find_or_add_model_selection(s, id);

        const auto draw_row = [&](uint32_t i) {
            InlayModelRow mrow = {};
            const bool has_row = model && inlay_ui_model_read(model, i, &mrow);
            const bool is_selected = sel && sel->has_selection &&
                (has_row && mrow.row_id != 0u ? (sel->selected_row_id == mrow.row_id) : (sel->selected_index == i));
            const uint32_t depth = has_row ? (mrow.depth > 32u ? 32u : mrow.depth) : 0u;
            const uint32_t row_id = etch_id_index(id, 0x730000u + i);

            auto row = etch_box_desc_default();
            row.id = row_id;
            row.axis = ETCH_AXIS_ROW;
            row.width = etch_sizing_grow(0.f, 0.f);
            row.height = etch_sizing_fixed(row_h);
            row.padding = {8.f + float(depth) * indent_px, 8.f, 0.f, 0.f};
            row.gap = 6.f;
            row.align_y = ETCH_ALIGN_Y_CENTER;
            row.radius = etch_corners_all(theme.radius_small_px);
            row.background = is_selected ? theme.surface_selected : EtchColor{};
            row.hover_background = is_selected ? theme.surface_selected : hov_col;
            row.focusable = true;
            etch_begin_box(&row);

            const EtchStr label_ft = (has_row && mrow.label.length > 0u) ? inlay_ui_frame_text(s, mrow.label) : EtchStr{};
            const EtchStr detail_ft = (has_row && mrow.detail.length > 0u) ? inlay_ui_frame_text(s, mrow.detail) : EtchStr{};
            const EtchStr badge_ft = (has_row && mrow.badge.length > 0u) ? inlay_ui_frame_text(s, mrow.badge) : EtchStr{};
            const uint32_t a11y_states = (is_selected ? uint32_t(ETCH_A11Y_STATE_SELECTED) : 0u) |
                                         (has_row && mrow.has_children && mrow.expanded ? uint32_t(ETCH_A11Y_STATE_EXPANDED) : 0u);
            etch_emit_a11y_node(row_id, ETCH_A11Y_TREE_ITEM, label_ft, detail_ft, a11y_states, 3u, 0.f, {});

            auto chev = etch_box_desc_default();
            chev.width = etch_sizing_fixed(12.f);
            chev.height = etch_sizing_fixed(row_h);
            etch_begin_box(&chev);
            if (has_row && mrow.has_children) {
                const float cy = row_h * 0.5f;
                const EtchVec2 pts_col[3] = {{4.f, cy - 3.5f}, {8.f, cy}, {4.f, cy + 3.5f}};
                const EtchVec2 pts_exp[3] = {{2.5f, cy - 2.f}, {6.f, cy + 2.f}, {9.5f, cy - 2.f}};
                auto pl = etch_polyline_desc_default();
                pl.points = mrow.expanded ? pts_exp : pts_col;
                pl.point_count = 3u;
                pl.color = theme.text_muted;
                pl.thickness = 1.4f;
                etch_draw_polyline(&pl);
            }
            etch_end_box();

            if (label_ft.length > 0u) {
                auto td = etch_text_desc_default();
                td.font_size_px = font_px;
                td.font_id = font_id;
                td.color = fg_col;
                td.wrap = false;
                td.ellipsis = true;
                etch_text(label_ft, &td);
            }
            if (detail_ft.length > 0u) {
                auto sd = etch_text_desc_default();
                sd.font_size_px = std::fmax(10.f, font_px - 1.5f);
                sd.color = theme.text_muted;
                sd.wrap = false;
                sd.ellipsis = true;
                etch_text(detail_ft, &sd);
            }
            if (badge_ft.length > 0u) {
                auto b = etch_box_desc_default();
                b.padding = etch_padding_xy(6.f, 1.f);
                b.background = theme.surface_subtle;
                b.radius = etch_corners_all(theme.radius_pill_px);
                etch_begin_box(&b);
                auto bt = etch_text_desc_default();
                bt.font_size_px = 10.5f;
                bt.color = theme.text_secondary;
                bt.wrap = false;
                etch_text(badge_ft, &bt);
                etch_end_box();
            }
            etch_end_box();

            if (etch_hovered(row_id)) etch_set_cursor(ETCH_CURSOR_HAND);
            if (is_activated(row_id)) {
                if (sel) {
                    sel->has_selection = true;
                    sel->selected_row_id = has_row ? mrow.row_id : 0u;
                    sel->selected_index = i;
                    etch_request_redraw(s->etch);
                }
                if (has_select_ev) {
                    const uint32_t cap = frame_json_cap(label_ft.length, 96u);
                    auto jb = frame_json_begin(s->etch, cap);
                    frame_json_lit(jb, "{\"index\":");
                    frame_json_u32(jb, i);
                    frame_json_lit(jb, ",\"rowId\":");
                    frame_json_u64(jb, has_row ? mrow.row_id : 0u);
                    frame_json_lit(jb, ",\"depth\":");
                    frame_json_u32(jb, depth);
                    frame_json_lit(jb, ",\"label\":");
                    frame_json_quoted(jb, label_ft);
                    frame_json_lit(jb, ",\"expanded\":");
                    frame_json_bool(jb, has_row && mrow.expanded);
                    frame_json_lit(jb, "}");
                    queue_slice_event(s, events, st.action, st.handler, frame_json_finish(jb), true);
                }
            }
        };

        if (has_explicit_h(s, st, cold)) {
            auto vd = etch_virtual_list_desc_default();
            vd.id = id;
            vd.item_count = total;
            vd.item_height_px = row_h;
            vd.row_gap_px = (st.flags & NfHasGap) ? std::fmax(0.f, st.gap) : 1.f;
            vd.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
            vd.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_fixed(240.f));
            if (st.flags & NfHasPadding) vd.padding = st.padding;
            else vd.padding = etch_padding_all(4.f);
            if (st.bg.set) vd.background = inlay_ui_resolve_color(st.bg, theme, theme.bg_surface);
            if (st.border_color.set) vd.border_color = inlay_ui_resolve_color(st.border_color, theme, theme.border_subtle);
            if (st.flags & NfHasBorderWidth) vd.border_width = std::fmax(0.f, st.border_width);
            if (st.flags & NfRadiusSet) vd.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_panel_px);
            const EtchVirtualListSlice slice = etch_begin_virtual_list(&vd);
            etch_emit_a11y_node(id, ETCH_A11Y_TREE, {}, {}, 0u, 0u, 0.f, {});
            for (uint32_t i = slice.first_index; i < slice.end_index; ++i) draw_row(i);
            etch_end_virtual_list();
        } else {
            auto box = etch_box_desc_default();
            box.width = etch_sizing_grow(0.f, 0.f);
            box.height = etch_sizing_fit(0.f, 0.f);
            box.padding = etch_padding_all(4.f);
            box.gap = 1.f;
            inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
            box.axis = ETCH_AXIS_COL;
            etch_begin_box(&box);
            etch_emit_a11y_node(id, ETCH_A11Y_TREE, {}, {}, 0u, 0u, 0.f, {});
            const uint32_t limit = total < 512u ? total : 512u;
            for (uint32_t i = 0u; i < limit; ++i) draw_row(i);
            etch_end_box();
        }
        return;
    }

    auto box = etch_box_desc_default();
    box.width = etch_sizing_grow(0.f, 0.f);
    box.height = etch_sizing_fit(0.f, 0.f);
    box.padding = etch_padding_all(4.f);
    box.gap = 1.f;
    inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
    box.axis = ETCH_AXIS_COL;

    const bool show_guide = (st.int_value >= 0);
    const bool previous = s->hide_tree_guide;
    s->hide_tree_guide = !show_guide;

    etch_begin_box(&box);
    EtchA11yOverrideDesc ov = {};
    ov.element_id = id;
    ov.role = ETCH_A11Y_TREE;
    ov.has_role = true;
    etch_set_a11y_override(&ov);

    const bool prev_col = s->draw_parent_col;
    s->draw_parent_col = true;
    for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
        inlay_ui_draw_node(s, at, id, theme, events);
    }
    s->draw_parent_col = prev_col;
    etch_end_box();

    s->hide_tree_guide = previous;
}

void draw_tree_item_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                           uint32_t id, const EtchTheme& theme, WidgetEvents* events) {
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    const bool controlled = (st.flags2 & Nf2HasOpen) != 0u;
    const bool want_open = (st.flags & NfOpen) != 0u;

    // Sync controlled open state ONLY when mismatched (avoids redundant request_redraw when idle).
    if (controlled && etch_is_tree_open(id) != want_open) {
        etch_set_tree_open(id, want_open);
    }

    const bool leaf_explicit = (st.int_value & (1 << 2)) != 0;
    const bool guide_explicit = (st.int_value & (1 << 0)) != 0;

    auto td = etch_tree_node_desc_default();
    td.id = id;
    td.label = inlay_ui_frame_text(s, node.text);
    td.icon_id = (st.flags2 & Nf2HasIcon) ? st.icon_id : uint16_t(ETCH_INVALID_ICON);
    td.default_open = controlled ? want_open : ((st.flags2 & Nf2DefaultOpen) != 0u);
    td.selected = (st.flags2 & Nf2Selected) != 0u;
    td.leaf = leaf_explicit ? ((st.flags2 & Nf2Leaf) != 0u) : (node.first_child == INLAY_UI_NONE);
    td.show_indent_guide = guide_explicit ? ((st.int_value & (1 << 1)) != 0) : !s->hide_tree_guide;
    td.row_height_px = (st.flags2 & Nf2HasValue) && st.value > 0.f ? st.value : 28.f;
    if ((st.flags2 & Nf2HasMin) && cold && cold->min_val > 0.f) td.indent_px = cold->min_val;
    if ((st.flags & NfHasSize) && st.font_size > 0.f) td.font_size_px = st.font_size;
    if (st.flags & NfHasFontId) td.font_id = st.font_id;
    if (st.color.set) td.text_color = inlay_ui_resolve_color(st.color, theme, theme.text_secondary);
    if (st.bg.set) td.selected_bg = inlay_ui_resolve_color(st.bg, theme, theme.surface_selected);
    if (cold && cold->hover_bg.set) td.hover_bg = inlay_ui_resolve_color(cold->hover_bg, theme, theme.surface_active);

    const EtchTreeNodeResult tr = etch_begin_tree_node(&td);
    if (tr.hovered) etch_set_cursor(ETCH_CURSOR_HAND);

    if (tr.open) {
        const bool prev_col = s->draw_parent_col;
        s->draw_parent_col = true;
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            inlay_ui_draw_node(s, at, id, theme, events);
        }
        s->draw_parent_col = prev_col;
        etch_end_tree_node();
    }

    if (tr.clicked && has_event(st.action, st.handler)) {
        auto jb = frame_json_begin(s->etch, frame_json_cap(node.text.length, 64u));
        frame_json_lit(jb, "{\"label\":");
        frame_json_quoted(jb, node.text);
        frame_json_lit(jb, ",\"open\":");
        frame_json_bool(jb, tr.open);
        frame_json_lit(jb, "}");
        const EtchStr payload = frame_json_finish(jb);
        if (payload.length > 0u) {
            queue_slice_event(s, events, st.action, st.handler, payload, true);
        }
    }

    if (tr.toggled && (st.int_value & (1 << 3)) != 0 && cold) {
        auto jb = frame_json_begin(s->etch, 32u);
        frame_json_lit(jb, "{\"open\":");
        frame_json_bool(jb, tr.open);
        frame_json_lit(jb, "}");
        queue_slice_event(s, events, cold->placeholder, cold->sprite_id, frame_json_finish(jb));
    }
}

void draw_menu_items(InlaySurface* s, uint32_t popup_id, const MenuSpec& spec,
                     const NodeStyle& st, const EtchTheme& theme, WidgetEvents* events) {
    for (uint32_t i = 0u; i < spec.item_count; ++i) {
        const MenuItemSpec& mi = s->menu_items[spec.item_offset + i];
        if (mi.separator) {
            etch_menu_separator();
            continue;
        }

        const bool tint_danger = mi.danger && !mi.disabled;
        if (tint_danger) {
            EtchTheme danger_thm = theme;
            danger_thm.text_primary = theme.danger;
            etch_push_theme(&danger_thm);
        }

        auto mid = etch_menu_item_desc_default();
        mid.id = etch_id_index(popup_id, 0xA000u + i);
        mid.label = inlay_ui_frame_text(s, inlay_ui_slice(s, mi.label));
        mid.shortcut_hint = inlay_ui_frame_text(s, inlay_ui_slice(s, mi.shortcut));
        mid.icon_id = mi.has_icon ? mi.icon_id : uint16_t(ETCH_INVALID_ICON);
        mid.checked = mi.checked;
        mid.disabled = mi.disabled;
        const bool activated = etch_menu_item(&mid);

        if (tint_danger) etch_pop_theme();

        if (activated) {
            const EtchStr val = mi.value.len ? inlay_ui_slice(s, mi.value) : inlay_ui_slice(s, mi.label);
            const EtchStr lbl = inlay_ui_slice(s, mi.label);
            auto jb = frame_json_begin(s->etch, frame_json_cap(val.length + lbl.length, 64u));
            frame_json_lit(jb, "{\"index\":");
            frame_json_u32(jb, i);
            frame_json_lit(jb, ",\"value\":");
            frame_json_quoted(jb, val);
            frame_json_lit(jb, ",\"label\":");
            frame_json_quoted(jb, lbl);
            frame_json_lit(jb, "}");
            const EtchStr payload = frame_json_finish(jb);
            if (payload.length > 0u) {
                if (has_event(mi.action, mi.handler)) {
                    queue_slice_event(s, events, mi.action, mi.handler, payload, true);
                }
                if (has_event(st.action, st.handler)) {
                    queue_slice_event(s, events, st.action, st.handler, payload, false);
                }
            }
        }
    }
}

void draw_menu_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                      uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->menu_spec_count) return;
    const MenuSpec& spec = s->menu_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    const uint32_t trigger_id = etch_id_index(id, 0xE101u);
    const uint32_t popup_id = etch_id_index(id, 0xE102u);
    const bool was_open = etch_is_popup_open(popup_id);

    if (spec.has_open) {
        if (spec.open && !was_open) {
            const EtchRect tb = etch_element_bounds(trigger_id);
            etch_open_popup(popup_id, {tb.x, tb.y + tb.h});
        } else if (!spec.open && was_open) {
            etch_close_popup(popup_id);
        }
    }

    bool emitted_open_change = false;
    auto handle_trigger = [&]() {
        if (was_open) {
            etch_close_popup(popup_id);
            if (has_event(spec.on_open_change_action, spec.on_open_change_handler)) {
                queue_slice_event(s, events, spec.on_open_change_action, spec.on_open_change_handler, ETCH_STR_LIT("false"));
                emitted_open_change = true;
            }
        } else {
            if (!spec.has_open || spec.open) {
                const EtchRect tb = etch_element_bounds(trigger_id);
                etch_open_popup(popup_id, {tb.x, tb.y + tb.h});
            }
            if (has_event(spec.on_open_change_action, spec.on_open_change_handler)) {
                queue_slice_event(s, events, spec.on_open_change_action, spec.on_open_change_handler, ETCH_STR_LIT("true"));
                emitted_open_change = true;
            }
        }
    };

    if (node.first_child != INLAY_UI_NONE) {
        auto trig = etch_box_desc_default();
        trig.axis = (st.flags & NfColumn) ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        trig.width = etch_sizing_fit(0.f, 0.f);
        trig.height = etch_sizing_fit(0.f, 0.f);
        trig.focusable = (st.flags & NfDisabled) == 0u;
        inlay_ui_populate_box_desc(s, st, theme, trigger_id, trig, use_layer);
        etch_begin_box(&trig);
        const bool prev_col = s->draw_parent_col;
        s->draw_parent_col = (trig.axis == ETCH_AXIS_COL);
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            inlay_ui_draw_node(s, at, trigger_id, theme, events);
        }
        s->draw_parent_col = prev_col;
        etch_end_box();

        if ((st.flags & NfDisabled) == 0u) {
            if (etch_hovered(trigger_id)) etch_set_cursor(ETCH_CURSOR_HAND);
            if (is_activated(trigger_id) || subtree_clicked(s, node.first_child, trigger_id)) {
                handle_trigger();
            }
        }
    } else {
        auto btn = etch_button_desc_default();
        btn.id = trigger_id;
        const EtchStr lbl = spec.label.len ? inlay_ui_slice(s, spec.label)
                          : node.text.length ? node.text
                                             : ETCH_STR_LIT("Menu");
        btn.label = inlay_ui_frame_text(s, lbl);
        btn.variant = static_cast<EtchButtonVariant>(st.button_variant);
        btn.disabled = (st.flags & NfDisabled) != 0u;
        if (has_explicit_w(s, st, cold)) btn.width = inlay_ui_resolve_sizing(s, st, cold, false, btn.width);
        if (has_explicit_h(s, st, cold)) btn.height = inlay_ui_resolve_sizing(s, st, cold, true, btn.height);
        if (st.flags & NfHasPadding) btn.padding = st.padding;
        if ((st.flags & NfHasSize) && st.font_size > 0.f) btn.font_size_px = st.font_size;
        if (st.flags & NfHasFontId) btn.font_id = st.font_id;
        if (st.flags & NfRadiusSet) btn.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_control_px);
        if (etch_button(&btn)) {
            handle_trigger();
        }
    }

    auto pd = etch_popup_desc_default();
    pd.id = popup_id;
    pd.trigger_id = trigger_id;
    pd.width = etch_sizing_fit(168.f, 360.f);
    pd.height = etch_sizing_fit(0.f, 420.f);
    pd.padding = etch_padding_all(4.f);
    pd.gap = 2.f;
    pd.attach_to_trigger = true;
    pd.flip_on_overflow = true;
    pd.backdrop_blur = (cold && cold->backdrop_blur > 0.f) ? cold->backdrop_blur : 12.f;
    pd.radius = etch_corners_all(theme.radius_panel_px);
    if (st.bg.set) pd.background = inlay_ui_resolve_color(st.bg, theme, theme.bg_popup);
    if (st.border_color.set) pd.border_color = inlay_ui_resolve_color(st.border_color, theme, theme.border_control);

    if (etch_begin_popup(&pd)) {
        draw_menu_items(s, popup_id, spec, st, theme, events);
        etch_end_popup();
    }

    if (was_open && !etch_is_popup_open(popup_id)) {
        if (has_event(spec.on_close_action, spec.on_close_handler)) {
            queue_slice_event(s, events, spec.on_close_action, spec.on_close_handler, ETCH_STR_LIT("false"));
        }
        if (!emitted_open_change && has_event(spec.on_open_change_action, spec.on_open_change_handler)) {
            queue_slice_event(s, events, spec.on_open_change_action, spec.on_open_change_handler, ETCH_STR_LIT("false"));
        }
    }
}

void draw_context_menu_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                              uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->menu_spec_count) return;
    const MenuSpec& spec = s->menu_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    const uint32_t target_id = etch_id_index(id, 0xC701u);
    const uint32_t popup_id = etch_id_index(id, 0xC702u);

    uint32_t eff_target = 0u;
    if (node.first_child != INLAY_UI_NONE) {
        eff_target = target_id;
        auto wrap = etch_box_desc_default();
        wrap.axis = (st.flags & NfColumn) ? ETCH_AXIS_COL : ETCH_AXIS_ROW;
        wrap.width = etch_sizing_fit(0.f, 0.f);
        wrap.height = etch_sizing_fit(0.f, 0.f);
        inlay_ui_populate_box_desc(s, st, theme, target_id, wrap, use_layer);
        etch_begin_box(&wrap);
        const bool prev_col = s->draw_parent_col;
        s->draw_parent_col = (wrap.axis == ETCH_AXIS_COL);
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            inlay_ui_draw_node(s, at, target_id, theme, events);
        }
        s->draw_parent_col = prev_col;
        etch_end_box();
    }

    const bool was_open = etch_is_popup_open(popup_id);
    const bool child_rclick = (node.first_child != INLAY_UI_NONE) &&
                              subtree_right_clicked(s, node.first_child, target_id);
    const bool rclicked = eff_target
        ? (etch_right_clicked(eff_target) || child_rclick)
        : etch_right_clicked_anywhere();
    const bool custom_popup = (cold && cold->backdrop_blur > 0.f) || st.bg.set || st.border_color.set;

    if (custom_popup) {
        if (rclicked) {
            etch_open_popup(popup_id, etch_pointer_pos());
        }
        auto pd = etch_popup_desc_default();
        pd.id = popup_id;
        pd.trigger_id = eff_target;
        pd.width = etch_sizing_fit(168.f, 360.f);
        pd.height = etch_sizing_fit(0.f, 420.f);
        pd.padding = etch_padding_all(4.f);
        pd.gap = 2.f;
        pd.attach_to_trigger = false;
        pd.flip_on_overflow = true;
        pd.backdrop_blur = (cold && cold->backdrop_blur > 0.f) ? cold->backdrop_blur : 12.f;
        pd.radius = etch_corners_all(theme.radius_panel_px);
        if (st.bg.set) pd.background = inlay_ui_resolve_color(st.bg, theme, theme.bg_popup);
        if (st.border_color.set) pd.border_color = inlay_ui_resolve_color(st.border_color, theme, theme.border_control);
        if (etch_begin_popup(&pd)) {
            draw_menu_items(s, popup_id, spec, st, theme, events);
            etch_end_popup();
        }
    } else {
        if (child_rclick) {
            etch_open_popup(popup_id, etch_pointer_pos());
        }
        if (etch_begin_context_menu(popup_id, eff_target)) {
            draw_menu_items(s, popup_id, spec, st, theme, events);
            etch_end_context_menu();
        }
    }

    if (was_open && !etch_is_popup_open(popup_id) && has_event(spec.on_close_action, spec.on_close_handler)) {
        queue_slice_event(s, events, spec.on_close_action, spec.on_close_handler, ETCH_STR_LIT("false"));
    }
}

void draw_avatar_widget(InlaySurface* s, uint32_t index, const InlayUiNode& node, const NodeStyle& st,
                        uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->avatar_spec_count) return;
    const AvatarSpec& spec = s->avatar_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    float sz = std::fmin(256.f, std::fmax(12.f, spec.size_px));
    if ((st.flags2 & Nf2WExplicit) && st.w_kind == ETCH_SIZING_FIXED && st.w_val >= 12.f) {
        sz = std::fmin(256.f, st.w_val);
    }
    const float rad = sz * 0.5f;
    const bool focusable = (st.flags & NfDisabled) == 0u &&
        ((st.flags & NfClickable) != 0u ||
         (cold && has_event(cold->on_focus_action, cold->on_focus_handler)));

    auto root = etch_box_desc_default();
    root.id = id;
    root.width = etch_sizing_fixed(sz);
    root.height = etch_sizing_fixed(sz);
    root.align_x = ETCH_ALIGN_X_CENTER;
    root.align_y = ETCH_ALIGN_Y_CENTER;
    root.focusable = focusable;
    if (cold && (cold->cflags & CfHasMargin)) root.margin = cold->margin;
    if (st.flags & NfHasAlignSelf) root.align_self = static_cast<EtchAlignSelf>(st.align_self);
    if ((st.flags & NfHasOpacity) && !use_layer) root.opacity = st.opacity;
    etch_begin_box(&root);

    const EtchColor ring_col = spec.has_ring
        ? resolve_ref(spec.ring_color, theme, theme.accent)
        : (st.border_color.set ? inlay_ui_resolve_color(st.border_color, theme, theme.border_subtle)
                               : etch_rgba8(255, 255, 255, inlay_is_dark_theme(theme) ? 30 : 40));
    const float ring_w = spec.has_ring ? 2.f
                       : (st.flags & NfHasBorderWidth) ? std::fmax(0.f, st.border_width)
                                                       : 1.f;

    if (spec.has_sprite && spec.sprite_id != ETCH_INVALID_SPRITE) {
        auto img = etch_image_desc_default();
        img.id = etch_id_index(id, 0xA701u);
        img.sprite_id = spec.sprite_id;
        img.width = etch_sizing_fixed(sz);
        img.height = etch_sizing_fixed(sz);
        img.radius = etch_corners_all(rad);
        img.border_color = ring_col;
        img.border_width = ring_w;
        img.fit = ETCH_IMAGE_FIT_COVER;
        etch_image(&img);
    } else {
        const EtchStr init_in = inlay_ui_slice(s, spec.initials);
        const EtchStr seed = node.text.length > 0u ? node.text : init_in;
        const uint32_t h = inlay_fnv1a_32(seed.chars, seed.length);
        const float hue = float(h % 360u);
        const EtchColor auto_bg = etch_color_oklch(inlay_is_dark_theme(theme) ? 0.54f : 0.56f, 0.145f, hue, 1.0f);
        const EtchColor disc_bg = resolve_ref(st.bg, theme, auto_bg);
        const EtchColor fg = resolve_ref(st.color, theme, etch_color_white(255u));

        char init_buf[12] = {};
        uint32_t cp_count = 1u;
        const uint32_t init_bytes = extract_avatar_initials(init_in, node.text, init_buf, &cp_count);

        auto disc = etch_box_desc_default();
        disc.id = etch_id_index(id, 0xA702u);
        disc.width = etch_sizing_fixed(sz);
        disc.height = etch_sizing_fixed(sz);
        disc.align_x = ETCH_ALIGN_X_CENTER;
        disc.align_y = ETCH_ALIGN_Y_CENTER;
        disc.background = disc_bg;
        disc.border_color = ring_col;
        disc.border_width = ring_w;
        disc.radius = etch_corners_all(rad);
        disc.clip_children = true;
        disc.pointer_passthrough = true;
        etch_begin_box(&disc);

        auto td = etch_text_desc_default();
        td.font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SEMIBOLD);
        td.font_size_px = std::fmax(9.f, std::round(sz * (cp_count > 1u ? 0.37f : 0.43f)));
        td.color = fg;
        td.wrap = false;
        etch_text(inlay_ui_frame_text(s, {init_buf, init_bytes, false}), &td);

        etch_end_box();
    }

    if (spec.status != AvatarStatusNone) {
        const float dot_sz = std::fmax(8.f, std::round(sz * 0.27f));
        const float halo_w = std::fmax(1.5f, std::round(sz * 0.06f));
        EtchColor dot_col = theme.text_muted;
        switch (spec.status) {
            case AvatarStatusOnline:  dot_col = etch_rgba8(34, 197, 94, 255); break;
            case AvatarStatusBusy:    dot_col = theme.danger; break;
            case AvatarStatusAway:    dot_col = theme.warning; break;
            case AvatarStatusOffline: dot_col = theme.text_muted; break;
            default: break;
        }
        dot_col = resolve_ref(spec.status_color, theme, dot_col);

        auto dot = etch_box_desc_default();
        dot.id = etch_id_index(id, 0xA703u);
        dot.width = etch_sizing_fixed(dot_sz);
        dot.height = etch_sizing_fixed(dot_sz);
        dot.radius = etch_corners_all(dot_sz * 0.5f);
        dot.background = dot_col;
        dot.border_color = theme.bg_surface;
        dot.border_width = halo_w;
        dot.border_align = ETCH_BORDER_OUTSIDE;
        dot.attach_to = ETCH_ATTACH_PARENT;
        dot.anchor_parent = ETCH_ANCHOR_BOTTOM_RIGHT;
        dot.anchor_element = ETCH_ANCHOR_BOTTOM_RIGHT;
        dot.floating_offset = {0.5f, 0.5f};
        dot.z_index = 2;
        dot.pointer_passthrough = true;
        etch_box(&dot);
    }

    etch_end_box();

    inlay_ui_dispatch_box_interaction(s, index, id, events);
}

void draw_toast_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                       uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->toast_spec_count) return;
    const ToastSpec& spec = s->toast_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    const EtchPresenceResult pres = etch_presence(etch_id_index(id, 0x70A5u), spec.open, etch_spring_spec_snappy());
    if (!pres.visible) {
        for (uint32_t ti = 0u; ti < s->toast_timer_count; ++ti) {
            if (s->toast_timers[ti].element_id == id) {
                s->toast_timers[ti].opened_ms = 0.0;
                s->toast_timers[ti].fired = false;
                break;
            }
        }
        return;
    }

    if (spec.open && spec.duration_ms > 0u) {
        ToastTimerSlot* tslot = nullptr;
        for (uint32_t ti = 0u; ti < s->toast_timer_count; ++ti) {
            if (s->toast_timers[ti].element_id == id) { tslot = &s->toast_timers[ti]; break; }
        }
        if (!tslot && inlay_grow_pod(s->toast_timers, s->toast_timer_cap, s->toast_timer_count + 1u, 8u, 64u)) {
            tslot = &s->toast_timers[s->toast_timer_count++];
            *tslot = {id, 0.0, false, s->frame_counter};
        }
        if (tslot) {
            tslot->last_used_frame = s->frame_counter;
            const double now = inlay_ui_now_ms(s);
            if (tslot->opened_ms <= 0.0) {
                tslot->opened_ms = now;
                tslot->fired = false;
            }
            if (!tslot->fired && (now - tslot->opened_ms) >= double(spec.duration_ms)) {
                tslot->fired = true;
                if (has_event(spec.dismiss_action, spec.dismiss_handler)) {
                    queue_slice_event(s, events, spec.dismiss_action, spec.dismiss_handler, ETCH_STR_LIT("false"), true);
                } else if (has_event(st.action, st.handler)) {
                    queue_slice_event(s, events, st.action, st.handler, ETCH_STR_LIT("false"), true);
                }
            } else if (!tslot->fired) {
                etch_request_redraw(s->etch);
            }
        }
    }

    constexpr float kInset = 20.f;
    EtchAnchor anchor_parent = ETCH_ANCHOR_BOTTOM_RIGHT;
    EtchAnchor anchor_elem = ETCH_ANCHOR_BOTTOM_RIGHT;
    EtchVec2 base_off = {-kInset, -kInset};
    float slide_dir = 1.f;

    switch (spec.position) {
        case ToastBottomRight:
            anchor_parent = ETCH_ANCHOR_BOTTOM_RIGHT;
            anchor_elem = ETCH_ANCHOR_BOTTOM_RIGHT;
            base_off = {-kInset, -kInset};
            slide_dir = 1.f;
            break;
        case ToastBottomLeft:
            anchor_parent = ETCH_ANCHOR_BOTTOM_LEFT;
            anchor_elem = ETCH_ANCHOR_BOTTOM_LEFT;
            base_off = {kInset, -kInset};
            slide_dir = 1.f;
            break;
        case ToastBottomCenter:
            anchor_parent = ETCH_ANCHOR_BOTTOM_CENTER;
            anchor_elem = ETCH_ANCHOR_BOTTOM_CENTER;
            base_off = {0.f, -kInset};
            slide_dir = 1.f;
            break;
        case ToastTopRight:
            anchor_parent = ETCH_ANCHOR_TOP_RIGHT;
            anchor_elem = ETCH_ANCHOR_TOP_RIGHT;
            base_off = {-kInset, kInset};
            slide_dir = -1.f;
            break;
        case ToastTopLeft:
            anchor_parent = ETCH_ANCHOR_TOP_LEFT;
            anchor_elem = ETCH_ANCHOR_TOP_LEFT;
            base_off = {kInset, kInset};
            slide_dir = -1.f;
            break;
        case ToastTopCenter:
            anchor_parent = ETCH_ANCHOR_TOP_CENTER;
            anchor_elem = ETCH_ANCHOR_TOP_CENTER;
            base_off = {0.f, kInset};
            slide_dir = -1.f;
            break;
        default:
            break;
    }
    if (spec.stack_index > 0u) {
        base_off.y += float(spec.stack_index) * 74.f * (-slide_dir);
    }

    EtchColor accent_col = theme.accent;
    switch (spec.variant) {
        case ToastSuccess: accent_col = etch_rgba8(34, 197, 94, 255); break;
        case ToastWarning: accent_col = theme.warning; break;
        case ToastDanger:  accent_col = theme.danger; break;
        case ToastInfo:
        default:
            accent_col = (st.int_value == 1) ? theme.accent : etch_rgba8(59, 130, 246, 255);
            break;
    }

    const bool anim = pres.opacity < 0.999f || pres.exiting;
    if (anim) {
        auto layer = etch_layer_desc_default();
        layer.opacity = pres.opacity;
        layer.translate = {0.f, (1.f - pres.opacity) * 10.f * slide_dir};
        layer.animating = true;
        etch_push_layer(&layer);
    }

    const bool dark = inlay_is_dark_theme(theme);
    const EtchStr title_str = inlay_ui_slice(s, spec.title);
    const EtchStr desc_str = spec.description.len ? inlay_ui_slice(s, spec.description)
                           : (title_str.length == 0u ? node.text : EtchStr{});

    auto card = etch_box_desc_default();
    card.axis = ETCH_AXIS_ROW;
    card.width = etch_sizing_fit(280.f, 420.f);
    card.height = etch_sizing_fit(0.f, 0.f);
    card.padding = etch_padding_xy(14.f, 12.f);
    card.gap = 12.f;
    card.align_y = ETCH_ALIGN_Y_CENTER;
    card.background = etch_rgba8(theme.bg_elevated.r, theme.bg_elevated.g, theme.bg_elevated.b, dark ? 234u : 242u);
    card.border_color = theme.border_control.a ? theme.border_control : theme.border_subtle;
    card.border_width = 1.f;
    card.radius = etch_corners_all(theme.radius_panel_px);
    card.continuous_corners = true;
    card.elevation = ETCH_ELEVATION_LG;
    card.backdrop_blur = (cold && cold->backdrop_blur > 0.f) ? cold->backdrop_blur : 14.f;
    card.top_rim_alpha = dark ? 0.14f : 0.0f;
    card.attach_to = ETCH_ATTACH_ROOT;
    card.anchor_parent = anchor_parent;
    card.anchor_element = anchor_elem;
    card.floating_offset = base_off;
    card.z_index = 120;
    card.clamp_to_viewport = true;
    inlay_ui_populate_box_desc(s, st, theme, id, card, anim || use_layer);
    card.axis = ETCH_AXIS_ROW;
    if (pres.exiting) card.pointer_passthrough = true;

    etch_begin_box(&card);

    // Semantic vertical accent pill.
    auto bar = etch_box_desc_default();
    bar.width = etch_sizing_fixed(3.5f);
    bar.height = etch_sizing_fixed((title_str.length > 0u && desc_str.length > 0u) ? 32.f : 18.f);
    bar.radius = etch_corners_all(2.f);
    bar.background = accent_col;
    etch_box(&bar);

    // Message body stack.
    auto body = etch_box_desc_default();
    body.axis = ETCH_AXIS_COL;
    body.width = etch_sizing_grow(0.f, 0.f);
    body.height = etch_sizing_fit(0.f, 0.f);
    body.gap = 2.f;
    body.shrink_width = true;
    etch_begin_box(&body);

    if (title_str.length > 0u) {
        auto td = etch_text_desc_default();
        td.font_id = uint16_t(ETCH_FONT_SEMIBOLD);
        td.font_size_px = 13.5f;
        td.color = resolve_ref(st.color, theme, theme.text_primary);
        td.wrap = true;
        etch_text(inlay_ui_frame_text(s, title_str), &td);
    }
    if (desc_str.length > 0u) {
        auto dd = etch_text_desc_default();
        dd.font_id = uint16_t(ETCH_FONT_SANS);
        dd.font_size_px = 12.5f;
        dd.color = title_str.length > 0u ? theme.text_secondary : resolve_ref(st.color, theme, theme.text_primary);
        dd.wrap = true;
        etch_text(inlay_ui_frame_text(s, desc_str), &dd);
    }
    {
        const bool prev_col = s->draw_parent_col;
        s->draw_parent_col = true;
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            inlay_ui_draw_node(s, at, id, theme, events);
        }
        s->draw_parent_col = prev_col;
    }
    etch_end_box();

    if (spec.action_label.len > 0u) {
        auto act_btn = etch_button_desc_default();
        act_btn.id = etch_id_index(id, 0x70A1u);
        act_btn.label = inlay_ui_frame_text(s, inlay_ui_slice(s, spec.action_label));
        act_btn.variant = ETCH_BUTTON_DEFAULT;
        act_btn.height = etch_sizing_fixed(26.f);
        act_btn.padding = etch_padding_xy(10.f, 0.f);
        act_btn.font_size_px = 12.f;
        if (etch_button(&act_btn) && !pres.exiting) {
            queue_slice_event(s, events, spec.action_name, spec.action_handler, ETCH_STR_LIT("true"), true);
        }
    }

    const bool has_dismiss = has_event(spec.dismiss_action, spec.dismiss_handler);
    if (spec.dismissible || has_dismiss) {
        const uint32_t close_id = etch_id_index(id, 0x70A2u);
        auto cb = etch_box_desc_default();
        cb.id = close_id;
        cb.width = etch_sizing_fixed(22.f);
        cb.height = etch_sizing_fixed(22.f);
        cb.radius = etch_corners_all(5.f);
        cb.hover_background = dark ? etch_rgba8(255, 255, 255, 18) : etch_rgba8(15, 23, 42, 14);
        cb.focusable = !pres.exiting;
        cb.pointer_passthrough = pres.exiting;
        etch_begin_box(&cb);

        EtchA11yOverrideDesc ov = {};
        ov.element_id = close_id;
        ov.role = ETCH_A11Y_BUTTON;
        ov.label = ETCH_STR_LIT("Dismiss");
        ov.actions_mask = pres.exiting ? 0u : 3u;
        ov.has_role = true;
        ov.has_label = true;
        ov.has_actions_mask = true;
        etch_set_a11y_override(&ov);

        const EtchColor x_col = (!pres.exiting && etch_hovered(close_id)) ? theme.text_primary : theme.text_muted;
        auto l1 = etch_line_draw_desc_default();
        l1.from = {7.f, 7.f};
        l1.to = {15.f, 15.f};
        l1.color = x_col;
        l1.thickness = 1.5f;
        etch_draw_line(&l1);

        auto l2 = etch_line_draw_desc_default();
        l2.from = {15.f, 7.f};
        l2.to = {7.f, 15.f};
        l2.color = x_col;
        l2.thickness = 1.5f;
        etch_draw_line(&l2);

        etch_end_box();

        if (!pres.exiting && etch_hovered(close_id)) etch_set_cursor(ETCH_CURSOR_HAND);
        if (!pres.exiting && is_activated(close_id)) {
            if (has_dismiss) {
                queue_slice_event(s, events, spec.dismiss_action, spec.dismiss_handler, ETCH_STR_LIT("false"), true);
            } else if (has_event(st.action, st.handler)) {
                queue_slice_event(s, events, st.action, st.handler, ETCH_STR_LIT("false"), true);
            }
        }
    }

    etch_end_box();

    if (anim) etch_pop_layer();
}

void draw_accordion_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                           uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    const bool controlled = (st.flags2 & Nf2HasOpen) != 0u;
    const bool disabled = (st.flags & NfDisabled) != 0u;
    const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 14.f;
    const uint16_t font_id = (st.flags & NfHasFontId) ? st.font_id : uint16_t(ETCH_FONT_SEMIBOLD);

    if (!controlled && !disabled) {
        auto cd = etch_collapsible_desc_default();
        cd.id = id;
        cd.title = inlay_ui_frame_text(s, node.text);
        cd.icon_id = (st.flags2 & Nf2HasIcon) ? st.icon_id : uint16_t(ETCH_INVALID_ICON);
        cd.default_open = (st.flags2 & Nf2DefaultOpen) != 0u;
        cd.header_height_px = 38.f;
        cd.font_size_px = font_px;
        cd.font_id = font_id;
        cd.header_padding = etch_padding_xy(12.f, 0.f);
        cd.body_padding = (st.flags & NfHasPadding) ? st.padding : etch_padding_xy(14.f, 12.f);
        cd.body_gap = (st.flags & NfHasGap) ? st.gap : 8.f;
        if (st.flags & NfRadiusSet) cd.radius = inlay_ui_resolve_corners(st, cold, theme, theme.radius_control_px);
        if (st.bg.set) {
            cd.header_bg = inlay_ui_resolve_color(st.bg, theme, theme.surface_subtle);
            cd.body_bg = cd.header_bg;
        }
        if (cold && cold->hover_bg.set) {
            cd.header_hover_bg = inlay_ui_resolve_color(cold->hover_bg, theme, theme.surface_subtle_hover);
        }
        if (st.border_color.set) cd.border_color = inlay_ui_resolve_color(st.border_color, theme, theme.border_subtle);
        if (st.flags & NfHasBorderWidth) cd.border_width = std::fmax(0.f, st.border_width);
        cd.text_color = resolve_ref(st.color, theme, theme.text_primary);
        cd.spring_spec = etch_spring_spec_snappy();

        const EtchCollapsibleResult cr = etch_begin_collapsible(&cd);
        if (cr.visible) {
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = true;
            for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
                inlay_ui_draw_node(s, at, id, theme, events);
            }
            s->draw_parent_col = prev_col;
            etch_end_collapsible();
        }
        if (cr.toggled && has_event(st.action, st.handler)) {
            auto jb = frame_json_begin(s->etch, 24u);
            frame_json_lit(jb, "{\"open\":");
            frame_json_bool(jb, cr.open);
            frame_json_lit(jb, "}");
            queue_slice_event(s, events, st.action, st.handler, frame_json_finish(jb));
        }
        return;
    }

    // Controlled (or disabled) Accordion with animated chevron + etch_begin_collapse.
    const bool is_open = controlled ? ((st.flags & NfOpen) != 0u) : ((st.flags2 & Nf2DefaultOpen) != 0u);
    const uint32_t hdr_id = etch_id_index(id, 0xACC1u);
    const uint32_t body_id = etch_id_index(id, 0xACC2u);
    const float chev_t = std::fmin(1.f, std::fmax(0.f, etch_spring(
        etch_id_index(id, 0xACC3u), is_open ? 1.f : 0.f, etch_spring_spec_snappy())));

    const EtchColor hdr_bg = resolve_ref(st.bg, theme, theme.surface_subtle);
    const EtchColor hdr_hover = (cold && cold->hover_bg.set)
        ? inlay_ui_resolve_color(cold->hover_bg, theme, theme.surface_subtle_hover)
        : theme.surface_subtle_hover;
    const EtchColor text_col = disabled ? theme.text_disabled : resolve_ref(st.color, theme, theme.text_primary);
    const EtchColor icon_col = disabled ? theme.text_disabled : theme.accent;

    auto outer = etch_box_desc_default();
    outer.axis = ETCH_AXIS_COL;
    outer.width = etch_sizing_grow(0.f, 0.f);
    outer.height = etch_sizing_fit(0.f, 0.f);
    outer.background = is_open ? theme.surface_subtle_body : EtchColor{};
    outer.border_color = theme.border_subtle;
    outer.border_width = 1.f;
    outer.radius = etch_corners_all(theme.radius_control_px);
    outer.continuous_corners = true;
    outer.clip_children = true;
    inlay_ui_populate_box_desc(s, st, theme, id, outer, use_layer);
    outer.axis = ETCH_AXIS_COL;
    outer.padding = etch_padding_all(0.f);
    outer.gap = 0.f;

    etch_begin_box(&outer);

    const bool hov = !disabled && etch_hovered(hdr_id);
    auto hdr = etch_box_desc_default();
    hdr.id = hdr_id;
    hdr.axis = ETCH_AXIS_ROW;
    hdr.width = etch_sizing_grow(0.f, 0.f);
    hdr.height = etch_sizing_fixed(38.f);
    hdr.padding = etch_padding_xy(12.f, 0.f);
    hdr.gap = 8.f;
    hdr.align_y = ETCH_ALIGN_Y_CENTER;
    hdr.background = hov ? hdr_hover : hdr_bg;
    hdr.focusable = !disabled;
    etch_begin_box(&hdr);

    // Animated chevron rotating 0 -> 90 deg smoothly.
    auto chev = etch_box_desc_default();
    chev.width = etch_sizing_fixed(14.f);
    chev.height = etch_sizing_fixed(38.f);
    etch_begin_box(&chev);
    const float angle = chev_t * 1.57079633f;
    const float cs = std::cos(angle);
    const float sn = std::sin(angle);
    const EtchVec2 base_pts[3] = {{-2.f, -4.f}, {2.f, 0.f}, {-2.f, 4.f}};
    EtchVec2 pts[3] = {};
    for (uint32_t i = 0u; i < 3u; ++i) {
        pts[i] = {7.f + base_pts[i].x * cs - base_pts[i].y * sn,
                  19.f + base_pts[i].x * sn + base_pts[i].y * cs};
    }
    auto pl = etch_polyline_desc_default();
    pl.points = pts;
    pl.point_count = 3u;
    pl.color = icon_col;
    pl.thickness = 1.5f;
    etch_draw_polyline(&pl);
    etch_end_box();

    if ((st.flags2 & Nf2HasIcon) && st.icon_id != ETCH_INVALID_ICON) {
        auto ic = etch_icon_desc_default();
        ic.icon_id = st.icon_id;
        ic.size_px = 14.f;
        ic.color = icon_col;
        etch_icon(&ic);
    }

    auto td = etch_text_desc_default();
    td.font_size_px = font_px;
    td.font_id = font_id;
    td.color = text_col;
    td.wrap = false;
    etch_text(inlay_ui_frame_text(s, node.text), &td);

    etch_end_box();

    if (!disabled) {
        if (hov) etch_set_cursor(ETCH_CURSOR_HAND);
        if (is_activated(hdr_id) && has_event(st.action, st.handler)) {
            auto jb = frame_json_begin(s->etch, 24u);
            frame_json_lit(jb, "{\"open\":");
            frame_json_bool(jb, !is_open);
            frame_json_lit(jb, "}");
            queue_slice_event(s, events, st.action, st.handler, frame_json_finish(jb), true);
        }
    }

    auto col = etch_collapse_desc_default();
    col.id = body_id;
    col.open = is_open;
    col.spring = etch_spring_spec_snappy();
    col.fade_content = true;
    col.width = etch_sizing_grow(0.f, 0.f);
    if (etch_begin_collapse(&col)) {
        auto body = etch_box_desc_default();
        body.axis = ETCH_AXIS_COL;
        body.width = etch_sizing_grow(0.f, 0.f);
        body.height = etch_sizing_fit(0.f, 0.f);
        body.padding = (st.flags & NfHasPadding) ? st.padding : etch_padding_xy(14.f, 12.f);
        body.gap = (st.flags & NfHasGap) ? st.gap : 8.f;
        etch_begin_box(&body);
        const bool prev_col = s->draw_parent_col;
        s->draw_parent_col = true;
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            inlay_ui_draw_node(s, at, id, theme, events);
        }
        s->draw_parent_col = prev_col;
        etch_end_box();
        etch_end_collapse();
    }

    etch_end_box();
}

void draw_skeleton_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                          const EtchTheme& theme, bool use_layer) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->skeleton_spec_count) return;
    const SkeletonSpec& spec = s->skeleton_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    const EtchColor c0 = resolve_ref(st.bg, theme, theme.surface_subtle);
    const EtchColor c1 = resolve_ref(spec.shimmer_color, theme, theme.surface_subtle_hover);
    const float pulse = spec.animated ? eval_skeleton_pulse(s, id) : 0.f;
    const EtchColor fill = spec.animated ? etch_color_lerp_oklab(c0, c1, pulse) : c0;

    if (spec.shape == SkeletonCircle) {
        float d = 40.f;
        if ((st.flags2 & Nf2WExplicit) && st.w_kind == ETCH_SIZING_FIXED && st.w_val > 0.f) d = st.w_val;
        else if ((st.flags2 & Nf2HExplicit) && st.h_kind == ETCH_SIZING_FIXED && st.h_val > 0.f) d = st.h_val;

        auto box = etch_box_desc_default();
        box.width = etch_sizing_fixed(d);
        box.height = etch_sizing_fixed(d);
        box.radius = etch_corners_all(d * 0.5f);
        inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
        if ((st.flags & NfRadiusSet) == 0u) box.radius = etch_corners_all(d * 0.5f);
        box.background = fill;
        etch_box(&box);
        return;
    }

    if (spec.shape == SkeletonText) {
        auto outer = etch_box_desc_default();
        outer.axis = ETCH_AXIS_COL;
        outer.width = etch_sizing_grow(0.f, 0.f);
        outer.height = etch_sizing_fit(0.f, 0.f);
        outer.gap = (st.flags & NfHasGap) ? st.gap : spec.line_gap_px;
        inlay_ui_populate_box_desc(s, st, theme, id, outer, use_layer);
        outer.axis = ETCH_AXIS_COL;
        outer.background = etch_color_transparent();
        outer.border_width = 0.f;
        outer.height = etch_sizing_fit(0.f, 0.f);

        etch_begin_box(&outer);
        const float line_h = ((st.flags2 & Nf2HExplicit) && st.h_kind == ETCH_SIZING_FIXED && st.h_val > 0.f)
            ? st.h_val
            : 13.f;
        const EtchCorners bar_r = (st.flags & NfRadiusSet)
            ? inlay_ui_resolve_corners(st, cold, theme, 4.f)
            : etch_corners_all(4.f);
        const uint32_t count = spec.lines > 0u ? uint32_t(spec.lines) : 1u;

        for (uint32_t i = 0u; i < count; ++i) {
            auto bar = etch_box_desc_default();
            bar.id = etch_id_index(id, 0x5CE0u + i);
            bar.width = (count > 1u && i + 1u == count) ? etch_sizing_percent(0.65f) : etch_sizing_grow(0.f, 0.f);
            bar.height = etch_sizing_fixed(line_h);
            bar.radius = bar_r;
            bar.background = fill;
            etch_box(&bar);
        }
        etch_end_box();
        return;
    }

    // Default: SkeletonRect
    auto box = etch_box_desc_default();
    box.width = etch_sizing_grow(0.f, 0.f);
    box.height = etch_sizing_fixed(20.f);
    box.radius = etch_corners_all(theme.radius_small_px);
    box.continuous_corners = true;
    inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
    box.background = fill;
    etch_box(&box);
}

// ============================================================================
// Round 2 Desktop & Design-System Widgets
// (DockSpace / DockPane / Sheet / HoverCard / RadioGroup / ToggleGroup /
//  ColorPicker / Chart / ViewZone)
// ============================================================================

static void rgb_to_hsv(EtchColor c, float* out_h, float* out_s, float* out_v) noexcept {
    const float r = float(c.r) / 255.f;
    const float g = float(c.g) / 255.f;
    const float b = float(c.b) / 255.f;
    const float mx = std::fmax(r, std::fmax(g, b));
    const float mn = std::fmin(r, std::fmin(g, b));
    const float d = mx - mn;
    float h = 0.f;
    if (d > 1e-5f) {
        if (mx == r) h = 60.f * std::fmod(((g - b) / d) + 6.f, 6.f);
        else if (mx == g) h = 60.f * (((b - r) / d) + 2.f);
        else h = 60.f * (((r - g) / d) + 4.f);
    }
    *out_h = h;
    *out_s = (mx > 1e-5f) ? (d / mx) : 0.f;
    *out_v = mx;
}

static EtchColor hsv_to_rgb(float h, float s, float v) noexcept {
    const float hh = std::fmod(h < 0.f ? (h + 360.f) : h, 360.f);
    const float ss = std::fmin(1.f, std::fmax(0.f, s));
    const float vv = std::fmin(1.f, std::fmax(0.f, v));
    const float c = vv * ss;
    const float x = c * (1.f - std::fabs(std::fmod(hh / 60.f, 2.f) - 1.f));
    const float m = vv - c;
    float r = 0.f, g = 0.f, b = 0.f;
    if (hh < 60.f) { r = c; g = x; }
    else if (hh < 120.f) { r = x; g = c; }
    else if (hh < 180.f) { g = c; b = x; }
    else if (hh < 240.f) { g = x; b = c; }
    else if (hh < 300.f) { r = x; b = c; }
    else { r = c; b = x; }
    auto to_u8 = [](float f) noexcept -> uint8_t {
        const float cl = std::fmin(1.f, std::fmax(0.f, f));
        return static_cast<uint8_t>(std::lround(cl * 255.f));
    };
    return etch_rgba8(to_u8(r + m), to_u8(g + m), to_u8(b + m), 255u);
}

bool parse_choice_items(InlaySurface* s, JSValueConst arr, uint32_t* out_off, uint32_t* out_cnt) {
    *out_off = 0u;
    *out_cnt = 0u;
    if (!JS_IsArray(arr)) return true;
    uint32_t raw_len = 0u;
    if (!read_array_length(s, arr, &raw_len) || raw_len == 0u) return true;
    const uint32_t count = raw_len > 32u ? 32u : raw_len;
    const uint32_t off = inlay_ui_alloc_pool(s, s->choice_items, s->choice_item_count,
                                             s->choice_item_cap, count, 4096u);
    if (off == INLAY_UI_NONE) return false;
    for (uint32_t i = 0u; i < count; ++i) {
        ChoiceItemSpec item = {};
        item.icon_id = uint16_t(ETCH_INVALID_ICON);
        const JSValue iv = JS_GetPropertyUint32(s->js, arr, i);
        if (JS_IsObject(iv) && !JS_IsNull(iv)) {
            item.label = inlay_ui_string_slice_prop(s, iv, "label");
            if (!item.label.len && !JS_HasException(s->js)) item.label = inlay_ui_string_slice_prop(s, iv, "text");
            if (!JS_HasException(s->js)) item.value = inlay_ui_string_slice_prop(s, iv, "value");
            if (!item.value.len) item.value = item.label;
            if (!item.label.len) item.label = item.value;
            if (!JS_HasException(s->js)) item.description = inlay_ui_string_slice_prop(s, iv, "description");
            item.disabled = inlay_ui_bool_prop(s, iv, "disabled");
            uint32_t ic = 0u;
            if (!JS_HasException(s->js) && inlay_ui_opt_uint_prop(s, iv, "iconId", ETCH_MAX_VECTOR_ICONS - 1u, &ic)) {
                item.icon_id = static_cast<uint16_t>(ic);
                item.has_icon = true;
            }
        } else if (!JS_IsUndefined(iv) && !JS_IsNull(iv)) {
            const TextSlice sl = inlay_ui_copy_text_slice(s, iv);
            item.label = sl;
            item.value = sl;
        }
        JS_FreeValue(s->js, iv);
        if (JS_HasException(s->js)) return false;
        s->choice_items[off + i] = item;
    }
    *out_off = off;
    *out_cnt = count;
    return true;
}

bool parse_dock_space_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->dock_space_specs, s->dock_space_spec_count,
                                             s->dock_space_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    DockSpaceSpec spec = {};
    spec.tab_bar_height = 30.f;
    spec.divider_size = 4.f;
    spec.default_split_ratio = 0.5f;
    spec.on_active_handler = INLAY_UI_NONE;
    spec.on_layout_handler = INLAY_UI_NONE;
    spec.on_tear_off_handler = INLAY_UI_NONE;
    if (has_props) {
        inlay_ui_opt_number_prop(s, props, "tabBarHeight", &spec.tab_bar_height, false, 18.0, 72.0);
        inlay_ui_opt_number_prop(s, props, "dividerSize", &spec.divider_size, false, 1.0, 24.0);
        inlay_ui_opt_number_prop(s, props, "defaultSplitRatio", &spec.default_split_ratio, false, 0.1, 0.9);
        peek_str_prop(s, props, "defaultSplit", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "horizontal") || inlay_ui_str_eq(ch, n, "row")) {
                spec.has_default_split = true; spec.default_split_vertical = false; return true;
            }
            if (inlay_ui_str_eq(ch, n, "vertical") || inlay_ui_str_eq(ch, n, "col")) {
                spec.has_default_split = true; spec.default_split_vertical = true; return true;
            }
            return false;
        });
        spec.layout_json = inlay_ui_string_slice_prop(s, props, "layoutJson");
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onActiveChange", &spec.on_active_action, &spec.on_active_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onLayoutChange", &spec.on_layout_action, &spec.on_layout_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onTearOff", &spec.on_tear_off_action, &spec.on_tear_off_handler);
        if (JS_HasException(s->js)) return false;
    }
    s->dock_space_specs[idx] = spec;
    return true;
}

bool parse_dock_pane_widget(InlaySurface* s, JSValueConst props, bool has_props, InlayUiNode& node, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->dock_pane_specs, s->dock_pane_spec_count,
                                             s->dock_pane_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    DockPaneSpec spec = {};
    spec.closable = true;
    spec.icon_id = uint16_t(ETCH_INVALID_ICON);
    spec.on_close_handler = INLAY_UI_NONE;
    spec.on_rclick_handler = INLAY_UI_NONE;
    if (has_props) {
        const JSValue pv = JS_GetPropertyStr(s->js, props, "paneId");
        if (JS_IsNumber(pv)) {
            uint32_t pid_u = 0u;
            if (JS_ToUint32(s->js, &pid_u, pv) == 0 && pid_u != 0u) spec.pane_id = pid_u;
        } else if (JS_IsString(pv)) {
            size_t plen = 0u;
            const char* pstr = JS_ToCStringLen(s->js, &plen, pv);
            if (pstr && plen > 0u) {
                uint32_t h = 2166136261u;
                for (size_t bi = 0u; bi < plen; ++bi) {
                    h ^= static_cast<uint8_t>(pstr[bi]);
                    h *= 16777619u;
                }
                spec.pane_id = h ? h : 1u;
            }
            if (pstr) JS_FreeCString(s->js, pstr);
        }
        JS_FreeValue(s->js, pv);
        if (JS_HasException(s->js)) return false;
        spec.title = inlay_ui_string_slice_prop(s, props, "title");
        if (!spec.title.len && !JS_HasException(s->js)) spec.title = inlay_ui_string_slice_prop(s, props, "label");
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_bool_prop(s, props, "closable", &spec.closable);
        spec.pinned = inlay_ui_bool_prop(s, props, "pinned");
        spec.dirty = inlay_ui_bool_prop(s, props, "dirty");
        spec.split_second = inlay_ui_bool_prop(s, props, "splitSecond");
        parse_color_prop(s, props, "badgeColor", &spec.badge_color);
        uint32_t ic = 0u;
        if (inlay_ui_opt_uint_prop(s, props, "iconId", ETCH_MAX_VECTOR_ICONS - 1u, &ic)) {
            spec.icon_id = static_cast<uint16_t>(ic);
            spec.has_icon = true;
        }
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onClose", &spec.on_close_action, &spec.on_close_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onRightClick", &spec.on_rclick_action, &spec.on_rclick_handler);
        if (JS_HasException(s->js)) return false;
    }
    if (spec.pane_id == 0u) spec.pane_id = node.id ? node.id : (idx + 1u);
    s->dock_pane_specs[idx] = spec;
    return true;
}

bool parse_sheet_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->sheet_specs, s->sheet_spec_count,
                                             s->sheet_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    SheetSpec spec = {};
    spec.open = false;
    spec.dismiss_on_backdrop = true;
    spec.dismiss_on_escape = true;
    spec.show_handle = true;
    spec.edge = uint8_t(ETCH_SHEET_RIGHT);
    spec.extent_px = 360.f;
    spec.on_close_handler = INLAY_UI_NONE;
    if (has_props) {
        inlay_ui_opt_bool_prop(s, props, "open", &spec.open);
        inlay_ui_opt_bool_prop(s, props, "dismissOnBackdrop", &spec.dismiss_on_backdrop);
        inlay_ui_opt_bool_prop(s, props, "dismissOnEscape", &spec.dismiss_on_escape);
        inlay_ui_opt_bool_prop(s, props, "showHandle", &spec.show_handle);
        inlay_ui_opt_number_prop(s, props, "extent", &spec.extent_px, false, 120.0, 2048.0);
        peek_str_prop(s, props, "edge", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "bottom")) { spec.edge = uint8_t(ETCH_SHEET_BOTTOM); return true; }
            if (inlay_ui_str_eq(ch, n, "left")) { spec.edge = uint8_t(ETCH_SHEET_LEFT); return true; }
            if (inlay_ui_str_eq(ch, n, "right")) { spec.edge = uint8_t(ETCH_SHEET_RIGHT); return true; }
            return false;
        });
        inlay_ui_parse_event_prop(s, props, "onClose", &spec.on_close_action, &spec.on_close_handler);
        if (JS_HasException(s->js)) return false;
    }
    s->sheet_specs[idx] = spec;
    return true;
}

bool parse_hover_card_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->hover_card_specs, s->hover_card_spec_count,
                                             s->hover_card_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    HoverCardSpec spec = {};
    spec.open_delay_ms = 260.f;
    spec.close_delay_ms = 180.f;
    spec.width_px = 280.f;
    if (has_props) {
        inlay_ui_opt_number_prop(s, props, "openDelayMs", &spec.open_delay_ms, false, 0.0, 5000.0);
        inlay_ui_opt_number_prop(s, props, "closeDelayMs", &spec.close_delay_ms, false, 0.0, 5000.0);
        inlay_ui_opt_number_prop(s, props, "cardWidth", &spec.width_px, false, 80.0, 1024.0);
        if (JS_HasException(s->js)) return false;
    }
    s->hover_card_specs[idx] = spec;
    return true;
}

bool parse_radio_group_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->radio_group_specs, s->radio_group_spec_count,
                                             s->radio_group_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    RadioGroupSpec spec = {};
    spec.on_change_handler = INLAY_UI_NONE;
    if (has_props) {
        spec.value = inlay_ui_string_slice_prop(s, props, "value");
        if (JS_HasException(s->js)) return false;
        peek_str_prop(s, props, "direction", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "row") || inlay_ui_str_eq(ch, n, "horizontal")) {
                spec.horizontal = true; return true;
            }
            return false;
        });
        const JSValue items_v = JS_GetPropertyStr(s->js, props, "items");
        const bool ok_items = parse_choice_items(s, items_v, &spec.item_off, &spec.item_cnt);
        JS_FreeValue(s->js, items_v);
        if (!ok_items || JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onChange", &spec.on_change_action, &spec.on_change_handler);
        if (JS_HasException(s->js)) return false;
    }
    s->radio_group_specs[idx] = spec;
    return true;
}

bool parse_toggle_group_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->toggle_group_specs, s->toggle_group_spec_count,
                                             s->toggle_group_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    ToggleGroupSpec spec = {};
    spec.on_change_handler = INLAY_UI_NONE;
    if (has_props) {
        spec.multiple = inlay_ui_bool_prop(s, props, "multiple");
        peek_str_prop(s, props, "variant", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "outline")) { spec.outline = true; return true; }
            return false;
        });
        const JSValue items_v = JS_GetPropertyStr(s->js, props, "items");
        const bool ok_items = parse_choice_items(s, items_v, &spec.item_off, &spec.item_cnt);
        JS_FreeValue(s->js, items_v);
        if (!ok_items || JS_HasException(s->js)) return false;

        if (spec.multiple) {
            const JSValue vals_v = JS_GetPropertyStr(s->js, props, "values");
            if (JS_IsArray(vals_v)) {
                uint32_t vlen = 0u;
                if (read_array_length(s, vals_v, &vlen)) {
                    for (uint32_t vi = 0u; vi < vlen && vi < 32u; ++vi) {
                        const JSValue vv = JS_GetPropertyUint32(s->js, vals_v, vi);
                        const EtchStr vs = inlay_ui_copy_text(s, vv);
                        JS_FreeValue(s->js, vv);
                        for (uint32_t ii = 0u; ii < spec.item_cnt && ii < 32u; ++ii) {
                            const EtchStr iv = inlay_ui_slice(s, s->choice_items[spec.item_off + ii].value);
                            if (iv.length == vs.length && (!iv.length || std::memcmp(iv.chars, vs.chars, iv.length) == 0)) {
                                spec.selected_mask |= (1u << ii);
                            }
                        }
                    }
                }
            }
            JS_FreeValue(s->js, vals_v);
        } else {
            spec.value = inlay_ui_string_slice_prop(s, props, "value");
        }
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onChange", &spec.on_change_action, &spec.on_change_handler);
        if (JS_HasException(s->js)) return false;
    }
    s->toggle_group_specs[idx] = spec;
    return true;
}

bool parse_color_picker_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->color_picker_specs, s->color_picker_spec_count,
                                             s->color_picker_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    ColorPickerSpec spec = {};
    spec.value = etch_rgba8(59, 130, 246, 255);
    spec.on_change_handler = INLAY_UI_NONE;
    if (has_props) {
        ColorRef cr = {};
        if (parse_color_prop(s, props, "value", &cr) && cr.set && cr.token == TokNone) {
            spec.value = cr.rgba;
            spec.has_value = true;
        }
        const JSValue pre_v = JS_GetPropertyStr(s->js, props, "presets");
        if (JS_IsArray(pre_v)) {
            uint32_t plen = 0u;
            if (read_array_length(s, pre_v, &plen)) {
                for (uint32_t i = 0u; i < plen && spec.preset_count < 16u; ++i) {
                    const JSValue pv = JS_GetPropertyUint32(s->js, pre_v, i);
                    const char* ps = JS_ToCString(s->js, pv);
                    if (ps) {
                        const ColorRef pcr = inlay_ui_parse_color(ps, std::strlen(ps));
                        if (pcr.set && pcr.token == TokNone) spec.presets[spec.preset_count++] = pcr.rgba;
                        JS_FreeCString(s->js, ps);
                    }
                    JS_FreeValue(s->js, pv);
                }
            }
        }
        JS_FreeValue(s->js, pre_v);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onChange", &spec.on_change_action, &spec.on_change_handler);
        if (JS_HasException(s->js)) return false;
    }
    if (spec.preset_count == 0u) {
        const EtchColor defs[6] = {
            etch_rgba8(239, 68, 68, 255), etch_rgba8(245, 158, 11, 255),
            etch_rgba8(34, 197, 94, 255), etch_rgba8(59, 130, 246, 255),
            etch_rgba8(168, 85, 247, 255), etch_rgba8(236, 72, 153, 255)
        };
        for (uint32_t i = 0u; i < 6u; ++i) spec.presets[i] = defs[i];
        spec.preset_count = 6u;
    }
    s->color_picker_specs[idx] = spec;
    return true;
}

bool parse_chart_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->chart_specs, s->chart_spec_count,
                                             s->chart_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;
    ChartSpec spec = {};
    spec.variant = 0u;
    spec.height_px = 140.f;
    spec.show_grid = true;
    spec.show_labels = true;
    if (has_props) {
        peek_str_prop(s, props, "variant", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "area")) { spec.variant = 1u; return true; }
            if (inlay_ui_str_eq(ch, n, "line")) { spec.variant = 2u; return true; }
            spec.variant = 0u; return true;
        });
        parse_color_prop(s, props, "seriesColor", &spec.color);
        if (!spec.color.set) parse_color_prop(s, props, "color", &spec.color);
        inlay_ui_opt_number_prop(s, props, "chartHeight", &spec.height_px, false, 40.0, 1024.0);
        inlay_ui_opt_bool_prop(s, props, "showGrid", &spec.show_grid);
        inlay_ui_opt_bool_prop(s, props, "showLabels", &spec.show_labels);

        const JSValue vals_v = JS_GetPropertyStr(s->js, props, "values");
        const JSValue lbls_v = JS_GetPropertyStr(s->js, props, "labels");
        if (JS_IsArray(vals_v)) {
            uint32_t vlen = 0u;
            if (read_array_length(s, vals_v, &vlen) && vlen > 0u) {
                const uint32_t count = vlen > 256u ? 256u : vlen;
                const uint32_t off = inlay_ui_alloc_pool(s, s->chart_points, s->chart_point_count,
                                                         s->chart_point_cap, count, 16384u);
                if (off == INLAY_UI_NONE) {
                    JS_FreeValue(s->js, lbls_v);
                    JS_FreeValue(s->js, vals_v);
                    return false;
                }
                spec.pt_off = off;
                spec.pt_cnt = count;
                for (uint32_t i = 0u; i < count; ++i) {
                    ChartPointSpec pt = {};
                    const JSValue pv = JS_GetPropertyUint32(s->js, vals_v, i);
                    if (JS_IsObject(pv) && !JS_IsNull(pv)) {
                        inlay_ui_opt_number_prop(s, pv, "value", &pt.value, true);
                        pt.label = inlay_ui_string_slice_prop(s, pv, "label");
                    } else {
                        double d = 0.0;
                        if (JS_ToFloat64(s->js, &d, pv) == 0 && std::isfinite(d)) pt.value = static_cast<float>(d);
                    }
                    JS_FreeValue(s->js, pv);
                    if (!pt.label.len && JS_IsArray(lbls_v)) {
                        const JSValue lv = JS_GetPropertyUint32(s->js, lbls_v, i);
                        if (!JS_IsUndefined(lv) && !JS_IsNull(lv)) pt.label = inlay_ui_copy_text_slice(s, lv);
                        JS_FreeValue(s->js, lv);
                    }
                    s->chart_points[off + i] = pt;
                }
            }
        }
        JS_FreeValue(s->js, lbls_v);
        JS_FreeValue(s->js, vals_v);
        if (JS_HasException(s->js)) return false;
    }
    s->chart_specs[idx] = spec;
    return true;
}

bool parse_view_zone_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    uint32_t zid = 0u;
    if (has_props) {
        inlay_ui_opt_uint_prop(s, props, "zoneId", UINT32_MAX, &zid);
        if (JS_HasException(s->js)) return false;
    }
    st.int_value = static_cast<int32_t>(zid);
    return true;
}

void draw_dock_space_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                            uint32_t id, const EtchTheme& theme, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->dock_space_spec_count) return;
    const DockSpaceSpec& spec = s->dock_space_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    uint32_t pane_nodes[32];
    uint32_t pane_count = 0u;
    bool any_split_second = false;
    for (uint32_t at = node.first_child; at != INLAY_UI_NONE && pane_count < 32u; at = s->nodes[at].next_sibling) {
        if (s->nodes[at].kind != INLAY_UI_DOCK_PANE) continue;
        const uint32_t px = s->styles[at].ext_idx;
        if (px == INLAY_UI_NONE || px >= s->dock_pane_spec_count) continue;
        pane_nodes[pane_count++] = at;
        if (s->dock_pane_specs[px].split_second) any_split_second = true;
    }

    DockSyncEntry* sync = nullptr;
    for (uint32_t i = 0u; i < s->dock_sync_cache_count; ++i) {
        if (s->dock_sync_cache[i].element_id == id) { sync = &s->dock_sync_cache[i]; break; }
    }
    if (!sync && inlay_grow_pod(s->dock_sync_cache, s->dock_sync_cache_cap, s->dock_sync_cache_count + 1u, 8u, 32u)) {
        sync = &s->dock_sync_cache[s->dock_sync_cache_count++];
        *sync = {};
        sync->element_id = id;
        sync->last_used_frame = s->frame_counter;
    }
    if (sync) sync->last_used_frame = s->frame_counter;

    auto make_pane_desc = [&](const DockPaneSpec& ps) -> EtchDockPaneDesc {
        auto pd = etch_dock_pane_desc_default();
        pd.id = ps.pane_id;
        pd.title = inlay_ui_frame_text(s, inlay_ui_slice(s, ps.title));
        if (ps.has_icon) pd.icon_id = ps.icon_id;
        pd.closable = ps.closable;
        pd.pinned = ps.pinned;
        pd.dirty = ps.dirty;
        if (ps.badge_color.set) pd.badge_color = inlay_ui_resolve_color(ps.badge_color, theme, EtchColor{});
        return pd;
    };

    const EtchStr lay_json = inlay_ui_slice(s, spec.layout_json);
    const uint64_t lay_hash = lay_json.length > 0u ? inlay_fnv1a_64(lay_json.chars, lay_json.length) : 0u;

    if (!etch_dock_space_initialized(id) || (sync && !sync->initialized)) {
        const uint32_t root = etch_dock_reset_root(id);
        uint32_t leaf1 = root, leaf2 = root;
        if (spec.has_default_split && any_split_second) {
            etch_dock_split_node(id, root, spec.default_split_vertical ? ETCH_AXIS_COL : ETCH_AXIS_ROW,
                                 spec.default_split_ratio, &leaf1, &leaf2, etch_id_index(id, 0xD0C1u), 120.f, 120.f);
        }
        for (uint32_t i = 0u; i < pane_count; ++i) {
            const auto& ps = s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx];
            const EtchDockPaneDesc pd = make_pane_desc(ps);
            etch_dock_add_pane(id, (ps.split_second ? leaf2 : leaf1), &pd);
        }
        if (lay_json.length > 0u) {
            (void)etch_dock_load_layout_json(id, lay_json);
            if (sync) sync->applied_layout_hash = lay_hash;
        }
        if (sync) {
            sync->initialized = true;
            sync->known_pane_count = pane_count;
            for (uint32_t i = 0u; i < pane_count; ++i) {
                sync->known_panes[i] = s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx].pane_id;
            }
        }
    } else if (sync) {
        // Apply explicit layoutJson prop only when it changes.
        if (lay_hash != 0u && lay_hash != sync->applied_layout_hash) {
            (void)etch_dock_load_layout_json(id, lay_json);
            sync->applied_layout_hash = lay_hash;
        }
        // Incremental reconciliation: preserve user split trees when panes are added or removed.
        for (uint32_t k = 0u; k < sync->known_pane_count; ++k) {
            const uint32_t old_pid = sync->known_panes[k];
            bool still_present = false;
            for (uint32_t i = 0u; i < pane_count; ++i) {
                if (s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx].pane_id == old_pid) {
                    still_present = true;
                    break;
                }
            }
            if (!still_present) {
                etch_dock_set_pane_open(id, old_pid, false);
            }
        }
        uint32_t target_leaf = etch_dock_focused_leaf(id);
        if (target_leaf == 0u) {
            for (uint32_t k = 0u; k < sync->known_pane_count; ++k) {
                const uint32_t lf = etch_dock_find_pane_leaf(id, sync->known_panes[k]);
                if (lf != 0u) { target_leaf = lf; break; }
            }
        }
        for (uint32_t i = 0u; i < pane_count; ++i) {
            const auto& ps = s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx];
            bool was_known = false;
            for (uint32_t k = 0u; k < sync->known_pane_count; ++k) {
                if (sync->known_panes[k] == ps.pane_id) { was_known = true; break; }
            }
            if (!was_known) {
                const uint32_t existing_leaf = etch_dock_find_pane_leaf(id, ps.pane_id);
                if (existing_leaf != 0u) {
                    etch_dock_set_pane_open(id, ps.pane_id, true);
                } else {
                    if (target_leaf == 0u) target_leaf = etch_dock_reset_root(id);
                    const EtchDockPaneDesc pd = make_pane_desc(ps);
                    etch_dock_add_pane(id, target_leaf, &pd);
                }
            }
        }
        sync->known_pane_count = pane_count;
        for (uint32_t i = 0u; i < pane_count; ++i) {
            sync->known_panes[i] = s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx].pane_id;
        }
    }

    for (uint32_t i = 0u; i < pane_count; ++i) {
        const auto& ps = s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx];
        etch_dock_set_pane_pinned(id, ps.pane_id, ps.pinned);
        etch_dock_set_pane_dirty(id, ps.pane_id, ps.dirty);
        if (ps.badge_color.set) {
            etch_dock_set_pane_badge(id, ps.pane_id, inlay_ui_resolve_color(ps.badge_color, theme, EtchColor{}));
        }
    }

    auto dd = etch_dock_space_desc_default();
    dd.id = id;
    dd.width = inlay_ui_resolve_sizing(s, st, cold, false, etch_sizing_grow(0.f, 0.f));
    dd.height = inlay_ui_resolve_sizing(s, st, cold, true, etch_sizing_grow(0.f, 0.f));
    dd.tab_bar_height_px = spec.tab_bar_height;
    dd.divider_size_px = spec.divider_size;
    if (st.bg.set) dd.background = inlay_ui_resolve_color(st.bg, theme, dd.background);
    etch_begin_dock_space(&dd);
    while (const uint32_t active_pid = etch_next_dock_pane()) {
        for (uint32_t i = 0u; i < pane_count; ++i) {
            const uint32_t pni = pane_nodes[i];
            const auto& ps = s->dock_pane_specs[s->styles[pni].ext_idx];
            if (ps.pane_id != active_pid) continue;
            const bool prev_col = s->draw_parent_col;
            s->draw_parent_col = true;
            for (uint32_t ch = s->nodes[pni].first_child; ch != INLAY_UI_NONE; ch = s->nodes[ch].next_sibling) {
                inlay_ui_draw_node(s, ch, id, theme, events);
            }
            s->draw_parent_col = prev_col;
            break;
        }
    }
    etch_end_dock_space();

    if (events) {
        for (uint32_t i = 0u; i < pane_count; ++i) {
            const auto& ps = s->dock_pane_specs[s->styles[pane_nodes[i]].ext_idx];
            if (ps.closable && !etch_dock_is_pane_open(id, ps.pane_id) &&
                has_event(ps.on_close_action, ps.on_close_handler)) {
                char buf[48];
                const int n = std::snprintf(buf, sizeof(buf), "{\"paneId\":%u}", ps.pane_id);
                if (n > 0) queue_slice_event(s, events, ps.on_close_action, ps.on_close_handler,
                                             inlay_ui_frame_text(s, {buf, uint32_t(n), false}), true);
                etch_dock_set_pane_open(id, ps.pane_id, true);
            }
            if (etch_dock_pane_right_clicked(id, ps.pane_id) &&
                has_event(ps.on_rclick_action, ps.on_rclick_handler)) {
                char buf[48];
                const int n = std::snprintf(buf, sizeof(buf), "{\"paneId\":%u}", ps.pane_id);
                if (n > 0) queue_slice_event(s, events, ps.on_rclick_action, ps.on_rclick_handler,
                                             inlay_ui_frame_text(s, {buf, uint32_t(n), false}), true);
            }
        }
        const uint32_t focused_leaf = etch_dock_focused_leaf(id);
        const uint32_t active_pid = etch_dock_active_pane(id, focused_leaf);
        if (sync && active_pid != 0u && active_pid != sync->last_active_pane) {
            const bool had_prev = sync->last_active_pane != 0u;
            sync->last_active_pane = active_pid;
            if (had_prev && has_event(spec.on_active_action, spec.on_active_handler)) {
                char buf[64];
                const int n = std::snprintf(buf, sizeof(buf), "{\"paneId\":%u,\"leaf\":%u}", active_pid, focused_leaf);
                if (n > 0) queue_slice_event(s, events, spec.on_active_action, spec.on_active_handler,
                                             inlay_ui_frame_text(s, {buf, uint32_t(n), false}));
            }
        }
        if (has_event(spec.on_tear_off_action, spec.on_tear_off_handler)) {
            EtchDockTearOffEvent tear = {};
            while (etch_dock_poll_tear_off(&tear)) {
                if (tear.dock_id != id) continue;
                const uint32_t cap = frame_json_cap(tear.title.length, 112u);
                auto jb = frame_json_begin(s->etch, cap);
                char head[96];
                const int hn = std::snprintf(head, sizeof(head),
                    "{\"paneId\":%u,\"x\":%.2f,\"y\":%.2f,\"title\":",
                    tear.pane_id, double(tear.screen_pos.x), double(tear.screen_pos.y));
                if (hn > 0) frame_json_raw(jb, head, uint32_t(hn));
                frame_json_quoted(jb, tear.title);
                frame_json_lit(jb, "}");
                const EtchStr payload = frame_json_finish(jb);
                if (payload.length > 0u) {
                    queue_slice_event(s, events, spec.on_tear_off_action, spec.on_tear_off_handler, payload);
                }
            }
        }
        if (sync && has_event(spec.on_layout_action, spec.on_layout_handler)) {
            char lbuf[4096];
            const uint32_t llen = etch_dock_save_layout_json(id, lbuf, sizeof(lbuf));
            if (llen > 0u && llen < sizeof(lbuf)) {
                const uint64_t lh = inlay_fnv1a_64(lbuf, llen);
                if (lh != sync->last_layout_hash) {
                    const bool had_prev = sync->last_layout_hash != 0u;
                    sync->last_layout_hash = lh;
                    if (had_prev) {
                        const uint32_t cap = frame_json_cap(llen, 16u);
                        auto jb = frame_json_begin(s->etch, cap);
                        frame_json_quoted(jb, {lbuf, llen, false});
                        const EtchStr payload = frame_json_finish(jb);
                        if (payload.length > 0u) {
                            queue_slice_event(s, events, spec.on_layout_action, spec.on_layout_handler, payload);
                        }
                    }
                }
            }
        }
    }
}

void draw_sheet_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                       uint32_t id, const EtchTheme& theme, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->sheet_spec_count) return;
    const SheetSpec& spec = s->sheet_specs[st.ext_idx];
    bool open_val = spec.open;
    auto sd = etch_sheet_desc_default();
    sd.id = id;
    sd.edge = static_cast<EtchSheetEdge>(spec.edge);
    sd.extent_px = spec.extent_px;
    sd.open_state = &open_val;
    sd.dismiss_on_backdrop = spec.dismiss_on_backdrop;
    sd.dismiss_on_escape = spec.dismiss_on_escape;
    sd.show_handle = spec.show_handle;
    if (st.bg.set) sd.background = inlay_ui_resolve_color(st.bg, theme, sd.background);
    if (st.border_color.set) sd.border_color = inlay_ui_resolve_color(st.border_color, theme, sd.border_color);
    if (etch_begin_sheet(&sd)) {
        const bool prev_col = s->draw_parent_col;
        s->draw_parent_col = true;
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            inlay_ui_draw_node(s, at, id, theme, events);
        }
        s->draw_parent_col = prev_col;
        etch_end_sheet();
    }
    if (spec.open && !open_val && has_event(spec.on_close_action, spec.on_close_handler)) {
        queue_slice_event(s, events, spec.on_close_action, spec.on_close_handler, ETCH_STR_LIT("false"), true);
    }
}

void draw_hover_card_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                            uint32_t id, const EtchTheme& theme, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->hover_card_spec_count) return;
    const HoverCardSpec& spec = s->hover_card_specs[st.ext_idx];
    const uint32_t trigger_node = node.first_child;
    if (trigger_node == INLAY_UI_NONE) return;

    const uint32_t trig_id = etch_id_index(id, 0x4C01u);
    const uint32_t card_id = etch_id_index(id, 0x4C02u);

    auto trig = etch_box_desc_default();
    trig.id = trig_id;
    trig.width = etch_sizing_fit(0.f, 0.f);
    trig.height = etch_sizing_fit(0.f, 0.f);
    etch_begin_box(&trig);
    inlay_ui_draw_node(s, trigger_node, id, theme, events);
    etch_end_box();

    HoverCardStateSlot* hslot = nullptr;
    for (uint32_t i = 0u; i < s->hover_card_state_count; ++i) {
        if (s->hover_card_states[i].element_id == id) { hslot = &s->hover_card_states[i]; break; }
    }
    if (!hslot && inlay_grow_pod(s->hover_card_states, s->hover_card_state_cap, s->hover_card_state_count + 1u, 8u, 64u)) {
        hslot = &s->hover_card_states[s->hover_card_state_count++];
        *hslot = {id, false, 0.0, 0.0, s->frame_counter};
    }
    bool open = false;
    if (hslot) {
        hslot->last_used_frame = s->frame_counter;
        const bool hov = etch_hovered(trig_id) || (hslot->open && etch_hovered(card_id));
        const double now = inlay_ui_now_ms(s);
        if (hov) {
            hslot->hover_leave_ms = 0.0;
            if (!hslot->open) {
                if (hslot->hover_enter_ms <= 0.0) hslot->hover_enter_ms = now;
                if ((now - hslot->hover_enter_ms) >= double(spec.open_delay_ms)) {
                    hslot->open = true;
                } else {
                    etch_request_redraw(s->etch);
                }
            }
        } else {
            hslot->hover_enter_ms = 0.0;
            if (hslot->open) {
                if (hslot->hover_leave_ms <= 0.0) hslot->hover_leave_ms = now;
                if ((now - hslot->hover_leave_ms) >= double(spec.close_delay_ms)) {
                    hslot->open = false;
                } else {
                    etch_request_redraw(s->etch);
                }
            }
        }
        open = hslot->open;
    }

    const uint32_t content_first = s->nodes[trigger_node].next_sibling;
    if (content_first == INLAY_UI_NONE) return;
    const EtchPresenceResult pres = etch_presence(etch_id_index(id, 0x4C03u), open, etch_spring_spec_snappy());
    if (!pres.visible) return;

    const bool anim = pres.opacity < 0.999f || pres.exiting;
    if (anim) {
        auto layer = etch_layer_desc_default();
        layer.opacity = pres.opacity;
        layer.translate = {0.f, (1.f - pres.opacity) * 4.f};
        layer.animating = true;
        etch_push_layer(&layer);
    }
    auto card = etch_box_desc_default();
    card.id = card_id;
    card.axis = ETCH_AXIS_COL;
    card.width = etch_sizing_fixed(spec.width_px);
    card.height = etch_sizing_fit(0.f, 0.f);
    card.padding = etch_padding_all(12.f);
    card.gap = 6.f;
    card.background = resolve_ref(st.bg, theme, theme.bg_elevated);
    card.border_color = resolve_ref(st.border_color, theme, theme.border_subtle);
    card.border_width = 1.f;
    card.radius = etch_corners_all(theme.radius_panel_px);
    card.elevation = ETCH_ELEVATION_LG;
    card.attach_to = ETCH_ATTACH_ELEMENT;
    card.attach_element_id = trig_id;
    card.anchor_parent = ETCH_ANCHOR_BOTTOM_LEFT;
    card.anchor_element = ETCH_ANCHOR_TOP_LEFT;
    card.floating_offset = {0.f, 6.f};
    card.z_index = 110;
    card.clamp_to_viewport = true;
    card.flip_on_overflow = true;
    etch_begin_box(&card);
    const bool prev_col = s->draw_parent_col;
    s->draw_parent_col = true;
    for (uint32_t at = content_first; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
        inlay_ui_draw_node(s, at, id, theme, events);
    }
    s->draw_parent_col = prev_col;
    etch_end_box();
    if (anim) etch_pop_layer();
}

void draw_radio_group_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                             const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->radio_group_spec_count) return;
    const RadioGroupSpec& spec = s->radio_group_specs[st.ext_idx];
    const EtchStr cur_val = inlay_ui_slice(s, spec.value);
    const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.5f;

    auto wrap = etch_box_desc_default();
    wrap.axis = spec.horizontal ? ETCH_AXIS_ROW : ETCH_AXIS_COL;
    wrap.width = etch_sizing_fit(0.f, 0.f);
    wrap.height = etch_sizing_fit(0.f, 0.f);
    wrap.gap = (st.flags & NfHasGap) ? st.gap : (spec.horizontal ? 14.f : 8.f);
    inlay_ui_populate_box_desc(s, st, theme, id, wrap, use_layer);
    wrap.axis = spec.horizontal ? ETCH_AXIS_ROW : ETCH_AXIS_COL;
    etch_begin_box(&wrap);

    for (uint32_t i = 0u; i < spec.item_cnt; ++i) {
        const ChoiceItemSpec& item = s->choice_items[spec.item_off + i];
        const EtchStr iv = inlay_ui_slice(s, item.value);
        const EtchStr il = inlay_ui_slice(s, item.label);
        const bool selected = (cur_val.length == iv.length) &&
                              (!cur_val.length || std::memcmp(cur_val.chars, iv.chars, cur_val.length) == 0);
        const bool dis = item.disabled || ((st.flags & NfDisabled) != 0u);
        const uint32_t rid = etch_id_index(id, 0xA100u + i);

        auto row = etch_box_desc_default();
        row.id = rid;
        row.axis = ETCH_AXIS_ROW;
        row.width = etch_sizing_fit(0.f, 0.f);
        row.height = etch_sizing_fit(0.f, 0.f);
        row.gap = 8.f;
        row.align_y = ETCH_ALIGN_Y_CENTER;
        row.focusable = !dis;
        etch_begin_box(&row);

        etch_emit_a11y_node(rid, ETCH_A11Y_BUTTON, inlay_ui_frame_text(s, il), {},
                            selected ? uint32_t(ETCH_A11Y_STATE_SELECTED) : 0u, dis ? 0u : 3u, 0.f, {});

        auto dot_outer = etch_box_desc_default();
        dot_outer.width = etch_sizing_fixed(16.f);
        dot_outer.height = etch_sizing_fixed(16.f);
        dot_outer.radius = etch_corners_all(8.f);
        dot_outer.border_width = selected ? 2.f : 1.5f;
        dot_outer.border_color = dis ? theme.border_subtle : (selected ? theme.accent : theme.border_control);
        dot_outer.align_x = ETCH_ALIGN_X_CENTER;
        dot_outer.align_y = ETCH_ALIGN_Y_CENTER;
        etch_begin_box(&dot_outer);
        if (selected) {
            auto dot_inner = etch_box_desc_default();
            dot_inner.width = etch_sizing_fixed(8.f);
            dot_inner.height = etch_sizing_fixed(8.f);
            dot_inner.radius = etch_corners_all(4.f);
            dot_inner.background = dis ? theme.text_disabled : theme.accent;
            etch_box(&dot_inner);
        }
        etch_end_box();

        auto col = etch_box_desc_default();
        col.axis = ETCH_AXIS_COL;
        col.width = etch_sizing_fit(0.f, 0.f);
        col.height = etch_sizing_fit(0.f, 0.f);
        col.gap = 1.f;
        etch_begin_box(&col);
        auto td = etch_text_desc_default();
        td.font_size_px = font_px;
        td.color = dis ? theme.text_disabled : resolve_ref(st.color, theme, theme.text_primary);
        td.wrap = false;
        etch_text(inlay_ui_frame_text(s, il), &td);
        if (item.description.len > 0u) {
            auto dd = etch_text_desc_default();
            dd.font_size_px = std::fmax(10.5f, font_px - 2.f);
            dd.color = theme.text_muted;
            dd.wrap = false;
            etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, item.description)), &dd);
        }
        etch_end_box();
        etch_end_box();

        if (!dis) {
            if (etch_hovered(rid)) etch_set_cursor(ETCH_CURSOR_HAND);
            if (is_activated(rid) && !selected && has_event(spec.on_change_action, spec.on_change_handler)) {
                auto jb = frame_json_begin(s->etch, frame_json_cap(iv.length, 8u));
                frame_json_quoted(jb, iv);
                queue_slice_event(s, events, spec.on_change_action, spec.on_change_handler, frame_json_finish(jb), true);
            }
        }
    }
    etch_end_box();
}

void draw_toggle_group_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                              const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->toggle_group_spec_count) return;
    const ToggleGroupSpec& spec = s->toggle_group_specs[st.ext_idx];
    const EtchStr cur_val = inlay_ui_slice(s, spec.value);
    const float font_px = (st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.f;

    auto wrap = etch_box_desc_default();
    wrap.axis = ETCH_AXIS_ROW;
    wrap.width = etch_sizing_fit(0.f, 0.f);
    wrap.height = etch_sizing_fit(0.f, 0.f);
    wrap.padding = etch_padding_all(2.f);
    wrap.gap = 2.f;
    wrap.background = spec.outline ? EtchColor{} : theme.surface_subtle;
    wrap.border_width = 1.f;
    wrap.border_color = theme.border_subtle;
    wrap.radius = etch_corners_all(theme.radius_control_px);
    inlay_ui_populate_box_desc(s, st, theme, id, wrap, use_layer);
    wrap.axis = ETCH_AXIS_ROW;
    etch_begin_box(&wrap);

    for (uint32_t i = 0u; i < spec.item_cnt; ++i) {
        const ChoiceItemSpec& item = s->choice_items[spec.item_off + i];
        const EtchStr iv = inlay_ui_slice(s, item.value);
        const EtchStr il = inlay_ui_slice(s, item.label);
        const bool active = spec.multiple
            ? (i < 32u && (spec.selected_mask & (1u << i)) != 0u)
            : ((cur_val.length == iv.length) && (!cur_val.length || std::memcmp(cur_val.chars, iv.chars, cur_val.length) == 0));
        const bool dis = item.disabled || ((st.flags & NfDisabled) != 0u);
        const uint32_t tid = etch_id_index(id, 0xB200u + i);

        auto seg = etch_box_desc_default();
        seg.id = tid;
        seg.axis = ETCH_AXIS_ROW;
        seg.width = etch_sizing_fit(0.f, 0.f);
        seg.height = etch_sizing_fixed(28.f);
        seg.padding = etch_padding_xy(10.f, 0.f);
        seg.gap = 5.f;
        seg.align_y = ETCH_ALIGN_Y_CENTER;
        seg.radius = etch_corners_all(std::fmax(2.f, theme.radius_control_px - 2.f));
        seg.background = active ? theme.surface_selected : EtchColor{};
        seg.hover_background = active ? theme.surface_selected : theme.surface_hover;
        seg.focusable = !dis;
        etch_begin_box(&seg);

        if (item.has_icon && item.icon_id != ETCH_INVALID_ICON) {
            auto ic = etch_icon_desc_default();
            ic.icon_id = item.icon_id;
            ic.size_px = 13.f;
            ic.color = active ? theme.accent : theme.text_secondary;
            etch_icon(&ic);
        }
        auto td = etch_text_desc_default();
        td.font_size_px = font_px;
        td.font_id = active ? uint16_t(ETCH_FONT_MEDIUM) : uint16_t(ETCH_FONT_SANS);
        td.color = dis ? theme.text_disabled : (active ? theme.text_primary : theme.text_secondary);
        td.wrap = false;
        etch_text(inlay_ui_frame_text(s, il), &td);
        etch_end_box();

        if (!dis) {
            if (etch_hovered(tid)) etch_set_cursor(ETCH_CURSOR_HAND);
            if (is_activated(tid) && has_event(spec.on_change_action, spec.on_change_handler)) {
                if (spec.multiple) {
                    const uint32_t next_mask = (i < 32u) ? (spec.selected_mask ^ (1u << i)) : spec.selected_mask;
                    auto jb = frame_json_begin(s->etch, 512u);
                    frame_json_lit(jb, "[");
                    bool first = true;
                    for (uint32_t j = 0u; j < spec.item_cnt && j < 32u; ++j) {
                        if ((next_mask & (1u << j)) == 0u) continue;
                        if (!first) frame_json_lit(jb, ",");
                        first = false;
                        frame_json_quoted(jb, inlay_ui_slice(s, s->choice_items[spec.item_off + j].value));
                    }
                    frame_json_lit(jb, "]");
                    queue_slice_event(s, events, spec.on_change_action, spec.on_change_handler, frame_json_finish(jb), true);
                } else if (!active) {
                    auto jb = frame_json_begin(s->etch, frame_json_cap(iv.length, 8u));
                    frame_json_quoted(jb, iv);
                    queue_slice_event(s, events, spec.on_change_action, spec.on_change_handler, frame_json_finish(jb), true);
                }
            }
        }
    }
    etch_end_box();
}

void draw_color_picker_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                              const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->color_picker_spec_count) return;
    const ColorPickerSpec& spec = s->color_picker_specs[st.ext_idx];

    ColorPickerSlot* cslot = nullptr;
    for (uint32_t i = 0u; i < s->color_picker_state_count; ++i) {
        if (s->color_picker_states[i].element_id == id) { cslot = &s->color_picker_states[i]; break; }
    }
    if (!cslot && inlay_grow_pod(s->color_picker_states, s->color_picker_state_cap, s->color_picker_state_count + 1u, 4u, 32u)) {
        cslot = &s->color_picker_states[s->color_picker_state_count++];
        *cslot = {id, 217.f, 0.76f, 0.96f, false, s->frame_counter};
    }
    if (cslot) {
        cslot->last_used_frame = s->frame_counter;
        if (!cslot->initialized || spec.has_value) {
            rgb_to_hsv(spec.value, &cslot->h, &cslot->s, &cslot->v);
            cslot->initialized = true;
        }
    }
    float cur_h = cslot ? cslot->h : 217.f;
    float cur_s = cslot ? cslot->s : 0.76f;
    float cur_v = cslot ? cslot->v : 0.96f;
    bool changed = false;

    const float pad_w = 200.f;
    const float pad_h = 120.f;
    auto card = etch_box_desc_default();
    card.axis = ETCH_AXIS_COL;
    card.width = etch_sizing_fixed(pad_w + 20.f);
    card.height = etch_sizing_fit(0.f, 0.f);
    card.padding = etch_padding_all(10.f);
    card.gap = 8.f;
    card.background = theme.bg_surface;
    card.border_color = theme.border_subtle;
    card.border_width = 1.f;
    card.radius = etch_corners_all(theme.radius_panel_px);
    inlay_ui_populate_box_desc(s, st, theme, id, card, use_layer);
    card.axis = ETCH_AXIS_COL;
    etch_begin_box(&card);

    // 1. SV 2D Pad
    const uint32_t sv_id = etch_id_index(id, 0xC001u);
    auto sv_box = etch_box_desc_default();
    sv_box.id = sv_id;
    sv_box.width = etch_sizing_fixed(pad_w);
    sv_box.height = etch_sizing_fixed(pad_h);
    sv_box.radius = etch_corners_all(6.f);
    sv_box.clip_children = true;
    sv_box.focusable = true;
    etch_begin_box(&sv_box);
    {
        const EtchColor hue_col = hsv_to_rgb(cur_h, 1.f, 1.f);
        auto r1 = etch_rect_draw_desc_default();
        r1.rect = {0.f, 0.f, pad_w, pad_h};
        r1.fill = etch_rgba8(255, 255, 255, 255);
        r1.fill_end = hue_col;
        r1.gradient_dir = {1.f, 0.f};
        etch_draw_rect(&r1);
        auto r2 = etch_rect_draw_desc_default();
        r2.rect = {0.f, 0.f, pad_w, pad_h};
        r2.fill = etch_rgba8(0, 0, 0, 0);
        r2.fill_end = etch_rgba8(0, 0, 0, 255);
        r2.gradient_dir = {0.f, 1.f};
        etch_draw_rect(&r2);
        const float cx = cur_s * pad_w;
        const float cy = (1.f - cur_v) * pad_h;
        auto dot = etch_rect_draw_desc_default();
        dot.rect = {cx - 5.f, cy - 5.f, 10.f, 10.f};
        dot.radius = etch_corners_all(5.f);
        dot.fill = hsv_to_rgb(cur_h, cur_s, cur_v);
        dot.border_color = etch_rgba8(255, 255, 255, 255);
        dot.border_width = 2.f;
        etch_draw_rect(&dot);
    }
    etch_end_box();
    if (etch_pressed(sv_id)) {
        const EtchRect b = etch_element_bounds(sv_id);
        const EtchVec2 p = etch_pointer_pos();
        if (b.w > 1.f && b.h > 1.f) {
            cur_s = std::fmin(1.f, std::fmax(0.f, (p.x - b.x) / b.w));
            cur_v = std::fmin(1.f, std::fmax(0.f, 1.f - (p.y - b.y) / b.h));
            changed = true;
        }
    }

    // 2. Hue Slider Bar
    const uint32_t hue_id = etch_id_index(id, 0xC002u);
    auto hue_box = etch_box_desc_default();
    hue_box.id = hue_id;
    hue_box.width = etch_sizing_fixed(pad_w);
    hue_box.height = etch_sizing_fixed(14.f);
    hue_box.radius = etch_corners_all(7.f);
    hue_box.clip_children = true;
    hue_box.focusable = true;
    etch_begin_box(&hue_box);
    {
        const EtchColor stops[7] = {
            etch_rgba8(255, 0, 0, 255), etch_rgba8(255, 255, 0, 255),
            etch_rgba8(0, 255, 0, 255), etch_rgba8(0, 255, 255, 255),
            etch_rgba8(0, 0, 255, 255), etch_rgba8(255, 0, 255, 255),
            etch_rgba8(255, 0, 0, 255)
        };
        const float seg_w = pad_w / 6.f;
        for (int seg = 0; seg < 6; ++seg) {
            auto sr = etch_rect_draw_desc_default();
            sr.rect = {float(seg) * seg_w, 0.f, seg_w + 1.f, 14.f};
            sr.fill = stops[seg];
            sr.fill_end = stops[seg + 1];
            sr.gradient_dir = {1.f, 0.f};
            etch_draw_rect(&sr);
        }
        const float hx = (cur_h / 360.f) * pad_w;
        auto thumb = etch_rect_draw_desc_default();
        thumb.rect = {std::fmin(pad_w - 10.f, std::fmax(0.f, hx - 5.f)), 1.f, 10.f, 12.f};
        thumb.radius = etch_corners_all(5.f);
        thumb.fill = hsv_to_rgb(cur_h, 1.f, 1.f);
        thumb.border_color = etch_rgba8(255, 255, 255, 255);
        thumb.border_width = 2.f;
        etch_draw_rect(&thumb);
    }
    etch_end_box();
    if (etch_pressed(hue_id)) {
        const EtchRect b = etch_element_bounds(hue_id);
        const EtchVec2 p = etch_pointer_pos();
        if (b.w > 1.f) {
            cur_h = std::fmin(359.9f, std::fmax(0.f, ((p.x - b.x) / b.w) * 360.f));
            changed = true;
        }
    }

    // 3. Preset Swatches
    auto sw_row = etch_box_desc_default();
    sw_row.axis = ETCH_AXIS_ROW;
    sw_row.width = etch_sizing_fixed(pad_w);
    sw_row.height = etch_sizing_fit(0.f, 0.f);
    sw_row.gap = 6.f;
    sw_row.wrap = true;
    etch_begin_box(&sw_row);
    for (uint8_t pi = 0u; pi < spec.preset_count; ++pi) {
        const uint32_t sw_id = etch_id_index(id, 0xC100u + pi);
        auto sw = etch_box_desc_default();
        sw.id = sw_id;
        sw.width = etch_sizing_fixed(20.f);
        sw.height = etch_sizing_fixed(20.f);
        sw.radius = etch_corners_all(5.f);
        sw.background = spec.presets[pi];
        sw.border_width = 1.f;
        sw.border_color = theme.border_subtle;
        sw.focusable = true;
        etch_box(&sw);
        if (etch_hovered(sw_id)) etch_set_cursor(ETCH_CURSOR_HAND);
        if (is_activated(sw_id)) {
            rgb_to_hsv(spec.presets[pi], &cur_h, &cur_s, &cur_v);
            changed = true;
        }
    }
    etch_end_box();

    // 4. Preview + Hex Readout
    const EtchColor final_col = hsv_to_rgb(cur_h, cur_s, cur_v);
    char hex_buf[16];
    std::snprintf(hex_buf, sizeof(hex_buf), "#%02X%02X%02X", final_col.r, final_col.g, final_col.b);
    auto foot = etch_box_desc_default();
    foot.axis = ETCH_AXIS_ROW;
    foot.width = etch_sizing_fixed(pad_w);
    foot.height = etch_sizing_fit(0.f, 0.f);
    foot.gap = 8.f;
    foot.align_y = ETCH_ALIGN_Y_CENTER;
    etch_begin_box(&foot);
    auto prev = etch_box_desc_default();
    prev.width = etch_sizing_fixed(22.f);
    prev.height = etch_sizing_fixed(22.f);
    prev.radius = etch_corners_all(5.f);
    prev.background = final_col;
    prev.border_width = 1.f;
    prev.border_color = theme.border_subtle;
    etch_box(&prev);
    auto htd = etch_text_desc_default();
    htd.font_id = uint16_t(ETCH_FONT_MONO);
    htd.font_size_px = 12.f;
    htd.color = theme.text_primary;
    etch_text(inlay_ui_frame_text(s, {hex_buf, 7u, false}), &htd);
    etch_end_box();

    etch_end_box();

    if (changed) {
        if (cslot) { cslot->h = cur_h; cslot->s = cur_s; cslot->v = cur_v; }
        if (has_event(spec.on_change_action, spec.on_change_handler)) {
            char payload[16];
            std::snprintf(payload, sizeof(payload), "\"#%02X%02X%02X\"", final_col.r, final_col.g, final_col.b);
            queue_slice_event(s, events, spec.on_change_action, spec.on_change_handler,
                              inlay_ui_frame_text(s, {payload, 9u, false}), true);
        }
    }
}

void draw_chart_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                       const EtchTheme& theme, bool use_layer) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->chart_spec_count) return;
    const ChartSpec& spec = s->chart_specs[st.ext_idx];
    const EtchColor series_col = resolve_ref(spec.color, theme, theme.accent);

    auto outer = etch_box_desc_default();
    outer.axis = ETCH_AXIS_COL;
    outer.width = etch_sizing_grow(0.f, 0.f);
    outer.height = etch_sizing_fit(0.f, 0.f);
    outer.padding = etch_padding_xy(10.f, 8.f);
    outer.gap = 6.f;
    outer.background = theme.bg_surface;
    outer.border_color = theme.border_subtle;
    outer.border_width = 1.f;
    outer.radius = etch_corners_all(theme.radius_panel_px);
    inlay_ui_populate_box_desc(s, st, theme, id, outer, use_layer);
    outer.axis = ETCH_AXIS_COL;
    etch_begin_box(&outer);

    const uint32_t plot_id = etch_id_index(id, 0xC4A0u);
    const EtchRect prev_b = etch_element_bounds(plot_id);
    const float plot_w = prev_b.w > 16.f ? prev_b.w : 280.f;
    const float plot_h = spec.height_px;

    auto plot = etch_box_desc_default();
    plot.id = plot_id;
    plot.width = etch_sizing_grow(0.f, 0.f);
    plot.height = etch_sizing_fixed(plot_h);
    plot.clip_children = true;
    etch_begin_box(&plot);

    if (spec.show_grid) {
        for (int g = 1; g <= 3; ++g) {
            auto gl = etch_line_draw_desc_default();
            const float gy = plot_h * (float(g) / 4.f);
            gl.from = {0.f, gy};
            gl.to = {plot_w, gy};
            gl.color = etch_rgba8(theme.border_subtle.r, theme.border_subtle.g, theme.border_subtle.b, 110u);
            gl.thickness = 1.f;
            etch_draw_line(&gl);
        }
    }

    if (spec.pt_cnt > 0u && spec.pt_off + spec.pt_cnt <= s->chart_point_count) {
        float min_v = 0.f, max_v = 0.f;
        for (uint32_t i = 0u; i < spec.pt_cnt; ++i) {
            const float v = s->chart_points[spec.pt_off + i].value;
            if (i == 0u || v > max_v) max_v = v;
            if (v < min_v) min_v = v;
        }
        if (max_v <= min_v) max_v = min_v + 1.f;
        const float range = max_v - min_v;

        if (spec.variant == 0u) {
            const float slot_w = plot_w / float(spec.pt_cnt);
            const float bar_w = std::fmax(2.f, slot_w * 0.68f);
            const float pad_x = (slot_w - bar_w) * 0.5f;
            for (uint32_t i = 0u; i < spec.pt_cnt; ++i) {
                const float norm = (s->chart_points[spec.pt_off + i].value - min_v) / range;
                const float bh = std::fmax(2.f, norm * (plot_h - 8.f));
                auto br = etch_rect_draw_desc_default();
                br.rect = {float(i) * slot_w + pad_x, plot_h - bh, bar_w, bh};
                br.radius = {3.f, 3.f, 0.f, 0.f};
                br.fill = series_col;
                etch_draw_rect(&br);
            }
        } else {
            auto* pts = static_cast<EtchVec2*>(etch_frame_arena_alloc(
                s->etch, size_t(spec.pt_cnt) * sizeof(EtchVec2), alignof(EtchVec2)));
            if (pts) {
                for (uint32_t i = 0u; i < spec.pt_cnt; ++i) {
                    const float norm = (s->chart_points[spec.pt_off + i].value - min_v) / range;
                    const float x = (spec.pt_cnt > 1u)
                        ? (4.f + (plot_w - 8.f) * (float(i) / float(spec.pt_cnt - 1u)))
                        : (plot_w * 0.5f);
                    const float y = (plot_h - 6.f) - norm * (plot_h - 14.f);
                    pts[i] = {x, y};
                }
                if (spec.variant == 1u && spec.pt_cnt >= 2u) {
                    EtchAreaSeriesDesc as = {};
                    as.points = pts;
                    as.point_count = spec.pt_cnt;
                    as.baseline_y = plot_h;
                    as.fill_top = etch_rgba8(series_col.r, series_col.g, series_col.b, 92u);
                    as.fill_bottom = etch_rgba8(series_col.r, series_col.g, series_col.b, 8u);
                    as.line_color = series_col;
                    as.line_thickness = 2.f;
                    as.smooth = true;
                    etch_draw_area_series(&as);
                } else if (spec.pt_cnt >= 2u) {
                    auto pl = etch_polyline_desc_default();
                    pl.points = pts;
                    pl.point_count = spec.pt_cnt;
                    pl.color = series_col;
                    pl.thickness = 2.f;
                    etch_draw_polyline(&pl);
                }
            }
        }
    }
    etch_end_box();

    if (spec.show_labels && spec.pt_cnt > 0u) {
        bool any_lbl = false;
        for (uint32_t i = 0u; i < spec.pt_cnt; ++i) {
            if (s->chart_points[spec.pt_off + i].label.len > 0u) { any_lbl = true; break; }
        }
        if (any_lbl) {
            auto lrow = etch_box_desc_default();
            lrow.axis = ETCH_AXIS_ROW;
            lrow.width = etch_sizing_grow(0.f, 0.f);
            lrow.height = etch_sizing_fit(0.f, 0.f);
            lrow.justify = ETCH_JUSTIFY_SPACE_BETWEEN;
            etch_begin_box(&lrow);
            const uint32_t step = spec.pt_cnt > 8u ? ((spec.pt_cnt + 7u) / 8u) : 1u;
            for (uint32_t i = 0u; i < spec.pt_cnt; i += step) {
                auto td = etch_text_desc_default();
                td.font_size_px = 10.5f;
                td.color = theme.text_muted;
                td.wrap = false;
                etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, s->chart_points[spec.pt_off + i].label)), &td);
            }
            etch_end_box();
        }
    }
    etch_end_box();
}

// ============================================================================
// Portal Declarative Bridge & Built-in <DiffView> (Split + Unified + Word Diff)
// ============================================================================

bool parse_portal_widget(InlaySurface* s, JSValueConst props, bool has_props,
                         InlayUiNode& node, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->portal_specs, s->portal_spec_count,
                                             s->portal_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    PortalWidgetSpec spec = {};
    spec.zoom = 1.f;
    spec.find_forward = true;
    spec.on_state_handler = INLAY_UI_NONE;
    spec.on_msg_handler = INLAY_UI_NONE;
    spec.on_nav_handler = INLAY_UI_NONE;

    if (has_props) {
        node.text = inlay_ui_string_prop(s, props, "name");
        if (!node.text.length && !JS_HasException(s->js)) node.text = inlay_ui_string_prop(s, props, "portal");
        if (!node.text.length && !JS_HasException(s->js)) node.text = inlay_ui_string_prop(s, props, "text");
        if (JS_HasException(s->js)) return false;

        spec.url = inlay_ui_string_slice_prop(s, props, "url");
        if (JS_HasException(s->js)) return false;
        spec.html = inlay_ui_string_slice_prop(s, props, "html");
        if (JS_HasException(s->js)) return false;
        if (inlay_ui_opt_number_prop(s, props, "zoom", &spec.zoom, false, 0.25, 5.0)) spec.has_zoom = true;
        if (JS_HasException(s->js)) return false;

        const JSValue fq_v = JS_GetPropertyStr(s->js, props, "findQuery");
        if (!JS_IsUndefined(fq_v) && !JS_IsNull(fq_v)) {
            spec.has_find = true;
            spec.find_query = inlay_ui_copy_text_slice(s, fq_v);
        }
        JS_FreeValue(s->js, fq_v);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_bool_prop(s, props, "findMatchCase", &spec.find_match_case);
        inlay_ui_opt_bool_prop(s, props, "findForward", &spec.find_forward);

        spec.nav_action = inlay_ui_string_slice_prop(s, props, "navAction");
        if (JS_HasException(s->js)) return false;
        spec.eval_script = inlay_ui_string_slice_prop(s, props, "evalScript");
        if (JS_HasException(s->js)) return false;
        spec.init_script = inlay_ui_string_slice_prop(s, props, "initScript");
        if (JS_HasException(s->js)) return false;
        spec.post_message = inlay_ui_string_slice_prop(s, props, "postMessage");
        if (JS_HasException(s->js)) return false;

        inlay_ui_parse_event_prop(s, props, "onStateChange", &spec.on_state_action, &spec.on_state_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onMessage", &spec.on_msg_action, &spec.on_msg_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onNavigate", &spec.on_nav_action, &spec.on_nav_handler);
        if (JS_HasException(s->js)) return false;
    }

    s->portal_specs[idx] = spec;
    return true;
}

void draw_portal_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                        uint32_t id, const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    const uint32_t pi = inlay_ui_content_index(s, INLAY_UI_PORTAL, node.text);
    auto* portal = (pi < s->content_count) ? static_cast<InlayPortal*>(s->content[pi].resource) : nullptr;
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    const PortalWidgetSpec* spec = (st.ext_idx != INLAY_UI_NONE && st.ext_idx < s->portal_spec_count)
        ? &s->portal_specs[st.ext_idx] : nullptr;

    if (portal && spec) {
        PortalSyncSlot* slot = nullptr;
        for (uint32_t si = 0u; si < s->portal_sync_slot_count; ++si) {
            if (s->portal_sync_slots[si].element_id == id) { slot = &s->portal_sync_slots[si]; break; }
        }
        if (!slot && inlay_grow_pod(s->portal_sync_slots, s->portal_sync_slot_cap, s->portal_sync_slot_count + 1u, 4u, 32u)) {
            slot = &s->portal_sync_slots[s->portal_sync_slot_count++];
            *slot = {};
            slot->element_id = id;
            slot->applied_zoom = 1.f;
        }
        if (slot) {
            slot->last_used_frame = s->frame_counter;
            const EtchStr init_s = inlay_ui_slice(s, spec->init_script);
            if (init_s.length > 0u) {
                const uint64_t h = inlay_fnv1a_64(init_s.chars, init_s.length);
                if (h != slot->init_hash) {
                    slot->init_hash = h;
                    (void)inlay_portal_add_init_script(portal, init_s);
                }
            }
            const EtchStr url_s = inlay_ui_slice(s, spec->url);
            if (url_s.length > 0u) {
                const uint64_t h = inlay_fnv1a_64(url_s.chars, url_s.length);
                if (h != slot->url_hash) {
                    slot->url_hash = h;
                    (void)inlay_portal_load_url(portal, url_s);
                    if (events && has_event(spec->on_nav_action, spec->on_nav_handler)) {
                        const uint32_t cap = frame_json_cap(url_s.length, 24u);
                        auto jb = frame_json_begin(s->etch, cap);
                        frame_json_lit(jb, "{\"url\":");
                        frame_json_quoted(jb, url_s);
                        frame_json_lit(jb, "}");
                        const EtchStr payload = frame_json_finish(jb);
                        if (payload.length > 0u) queue_slice_event(s, events, spec->on_nav_action, spec->on_nav_handler, payload);
                    }
                }
            }
            const EtchStr html_s = inlay_ui_slice(s, spec->html);
            if (html_s.length > 0u) {
                const uint64_t h = inlay_fnv1a_64(html_s.chars, html_s.length);
                if (h != slot->html_hash) {
                    slot->html_hash = h;
                    (void)inlay_portal_load_html(portal, html_s);
                }
            }
            if (spec->has_zoom && (!slot->has_applied_zoom || std::fabs(slot->applied_zoom - spec->zoom) > 1e-3f)) {
                slot->has_applied_zoom = true;
                slot->applied_zoom = spec->zoom;
                (void)inlay_portal_set_zoom(portal, spec->zoom);
            }
            if (spec->has_find) {
                const EtchStr fq = inlay_ui_slice(s, spec->find_query);
                uint64_t fh = fq.length > 0u ? inlay_fnv1a_64(fq.chars, fq.length) : 1ull;
                fh = (fh ^ (spec->find_match_case ? 3u : 0u) ^ (spec->find_forward ? 5u : 0u)) * 1099511628211ull;
                if (fh != slot->find_hash) {
                    slot->find_hash = fh;
                    if (fq.length > 0u) (void)inlay_portal_find_in_page(portal, fq, spec->find_forward, spec->find_match_case);
                    else (void)inlay_portal_stop_find(portal);
                }
            }
            const EtchStr nav_s = inlay_ui_slice(s, spec->nav_action);
            if (nav_s.length > 0u) {
                const uint64_t h = inlay_fnv1a_64(nav_s.chars, nav_s.length);
                if (h != slot->nav_hash) {
                    slot->nav_hash = h;
                    uint32_t base_len = nav_s.length;
                    for (uint32_t ni = 0u; ni < nav_s.length; ++ni) {
                        if (nav_s.chars[ni] == '#' || nav_s.chars[ni] == ':') { base_len = ni; break; }
                    }
                    if (inlay_ui_str_eq(nav_s.chars, base_len, "back")) (void)inlay_portal_go_back(portal);
                    else if (inlay_ui_str_eq(nav_s.chars, base_len, "forward")) (void)inlay_portal_go_forward(portal);
                    else if (inlay_ui_str_eq(nav_s.chars, base_len, "reload")) (void)inlay_portal_reload(portal);
                    else if (inlay_ui_str_eq(nav_s.chars, base_len, "devtools")) (void)inlay_portal_open_devtools(portal);
                }
            }
            const EtchStr eval_s = inlay_ui_slice(s, spec->eval_script);
            if (eval_s.length > 0u) {
                const uint64_t h = inlay_fnv1a_64(eval_s.chars, eval_s.length);
                if (h != slot->eval_hash) {
                    slot->eval_hash = h;
                    (void)inlay_portal_eval_script(portal, eval_s);
                }
            }
            const EtchStr post_s = inlay_ui_slice(s, spec->post_message);
            if (post_s.length > 0u) {
                const uint64_t h = inlay_fnv1a_64(post_s.chars, post_s.length);
                if (h != slot->post_hash) {
                    slot->post_hash = h;
                    (void)inlay_portal_post_message(portal, post_s);
                }
            }
            if (events && has_event(spec->on_msg_action, spec->on_msg_handler)) {
                for (uint32_t mi = 0u; mi < 8u; ++mi) {
                    const uint32_t sz = inlay_portal_peek_message_size(portal);
                    if (sz == 0u || sz > 32768u) break;
                    auto* mbuf = static_cast<char*>(etch_frame_arena_alloc(s->etch, size_t(sz) + 1u, 1u));
                    if (!mbuf) break;
                    const uint32_t got = inlay_portal_poll_message(portal, mbuf, sz + 1u);
                    if (got == 0u) break;
                    const EtchStr raw = {mbuf, got < sz ? got : sz, false};
                    const char c0 = raw.length > 0u ? raw.chars[0] : '\0';
                    const bool looks_json = (c0 == '{' || c0 == '[' || c0 == '"' || c0 == '-' || (c0 >= '0' && c0 <= '9') ||
                                             inlay_ui_equal(raw, "true") || inlay_ui_equal(raw, "false") || inlay_ui_equal(raw, "null"));
                    if (looks_json) {
                        queue_slice_event(s, events, spec->on_msg_action, spec->on_msg_handler, raw);
                    } else {
                        auto jb = frame_json_begin(s->etch, frame_json_cap(raw.length, 8u));
                        frame_json_quoted(jb, raw);
                        const EtchStr qm = frame_json_finish(jb);
                        if (qm.length > 0u) queue_slice_event(s, events, spec->on_msg_action, spec->on_msg_handler, qm);
                    }
                }
            }
            if (events && has_event(spec->on_state_action, spec->on_state_handler)) {
                InlayPortalState pst = inlay_portal_state_default();
                InlayPortalFindResult fres = inlay_portal_find_result_default();
                if (inlay_portal_state(portal, &pst)) {
                    (void)inlay_portal_find_status(portal, &fres);
                    uint64_t sh = 14695981039346656037ull;
                    sh = (sh ^ (pst.is_loading ? 1u : 0u) ^ (pst.can_go_back ? 2u : 0u) ^ (pst.can_go_forward ? 4u : 0u)) * 1099511628211ull;
                    sh = (sh ^ static_cast<uint32_t>(std::lround(pst.zoom_factor * 1000.f))) * 1099511628211ull;
                    sh = (sh ^ (fres.active ? 8u : 0u) ^ (fres.active_match << 4) ^ (fres.total_matches << 16)) * 1099511628211ull;
                    if (pst.title.length > 0u) sh ^= inlay_fnv1a_64(pst.title.chars, pst.title.length);
                    if (pst.url.length > 0u) sh ^= inlay_fnv1a_64(pst.url.chars, pst.url.length) * 31ull;
                    if (pst.favicon_url.length > 0u) sh ^= inlay_fnv1a_64(pst.favicon_url.chars, pst.favicon_url.length) * 131ull;
                    if (sh != slot->last_state_hash) {
                        slot->last_state_hash = sh;
                        const uint32_t cap = frame_json_cap(pst.title.length + pst.url.length + pst.favicon_url.length, 256u);
                        auto jb = frame_json_begin(s->etch, cap);
                        char head[160];
                        const int hn = std::snprintf(head, sizeof(head),
                            "{\"loading\":%s,\"canGoBack\":%s,\"canGoForward\":%s,\"zoom\":%.3f,\"title\":",
                            pst.is_loading ? "true" : "false",
                            pst.can_go_back ? "true" : "false",
                            pst.can_go_forward ? "true" : "false",
                            double(pst.zoom_factor));
                        if (hn > 0) frame_json_raw(jb, head, uint32_t(hn));
                        frame_json_quoted(jb, pst.title);
                        frame_json_lit(jb, ",\"url\":");
                        frame_json_quoted(jb, pst.url);
                        frame_json_lit(jb, ",\"faviconUrl\":");
                        frame_json_quoted(jb, pst.favicon_url);
                        char tail[128];
                        const int tn = std::snprintf(tail, sizeof(tail),
                            ",\"find\":{\"active\":%s,\"activeMatch\":%u,\"totalMatches\":%u}}",
                            fres.active ? "true" : "false", fres.active_match, fres.total_matches);
                        if (tn > 0) frame_json_raw(jb, tail, uint32_t(tn));
                        const EtchStr payload = frame_json_finish(jb);
                        if (payload.length > 0u) queue_slice_event(s, events, spec->on_state_action, spec->on_state_handler, payload);
                    }
                }
            }
        }
    }

    auto box = etch_box_desc_default();
    inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
    const auto prev_b = etch_element_bounds(id);
    const bool fixed_w = (box.width.kind == ETCH_SIZING_FIXED && box.width.value > 0.f);
    const bool fixed_h = (box.height.kind == ETCH_SIZING_FIXED && box.height.value > 0.f);
    if ((!fixed_w && prev_b.w <= 0.f) || (!fixed_h && prev_b.h <= 0.f)) {
        etch_request_redraw(s->etch);
    }
    float eff_w = fixed_w ? box.width.value : prev_b.w;
    float eff_h = fixed_h ? box.height.value : prev_b.h;
    if (eff_w > 0.f) eff_w = std::fmax(0.f, eff_w - box.padding.left - box.padding.right);
    if (eff_h > 0.f) eff_h = std::fmax(0.f, eff_h - box.padding.top - box.padding.bottom);
    const float portal_r = (st.flags & NfRadiusSet)
        ? inlay_ui_resolve_corners(st, cold, theme, 0.f).tl
        : 0.f;
    etch_begin_box(&box);
    if (portal && eff_w > 0.f && eff_h > 0.f) {
        auto pv = inlay_portal_view_desc_default();
        pv.id = etch_id_index(id, 0x9077u);
        pv.width = eff_w;
        pv.height = eff_h;
        pv.radius = portal_r;
        inlay_portal_view(portal, &pv);
    }
    etch_end_box();
}

void free_diff_cache_entry(DiffViewCacheEntry& e) noexcept {
    if (e.left_buf) { etch_destroy_text_buffer(e.left_buf); e.left_buf = nullptr; }
    if (e.right_buf) { etch_destroy_text_buffer(e.right_buf); e.right_buf = nullptr; }
    if (e.unified_buf) { etch_destroy_text_buffer(e.unified_buf); e.unified_buf = nullptr; }
    SDL_free(e.left_meta);    e.left_meta = nullptr;
    SDL_free(e.right_meta);   e.right_meta = nullptr;
    SDL_free(e.unified_meta); e.unified_meta = nullptr;
    SDL_free(e.left_hls);     e.left_hls = nullptr;     e.left_hl_count = 0u;
    SDL_free(e.right_hls);    e.right_hls = nullptr;    e.right_hl_count = 0u;
    SDL_free(e.unified_hls);  e.unified_hls = nullptr;  e.unified_hl_count = 0u;
    SDL_free(e.markers);      e.markers = nullptr;      e.marker_count = 0u;
    SDL_free(e.folds);        e.folds = nullptr;        e.fold_count = 0u;
    SDL_free(e.hunks);        e.hunks = nullptr;        e.hunk_count = 0u;
    e.active_hunk = 0u;
    e.split_line_count = 0u;
    e.unified_line_count = 0u;
    e.added_lines = 0u;
    e.removed_lines = 0u;
}

bool parse_diff_view_widget(InlaySurface* s, JSValueConst props, bool has_props, NodeStyle& st) {
    const uint32_t idx = inlay_ui_alloc_pool(s, s->diff_specs, s->diff_spec_count,
                                             s->diff_spec_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return false;
    st.ext_idx = idx;

    DiffViewSpec spec = {};
    spec.mode = 0u;
    spec.context_lines = 3u;
    spec.collapse_unchanged = false;
    spec.word_diff = true;
    spec.show_overview_ruler = true;
    spec.show_line_numbers = true;
    spec.editable = false;
    spec.font_size = 13.f;
    spec.on_gutter_click_handler = INLAY_UI_NONE;
    spec.on_gutter_range_handler = INLAY_UI_NONE;
    spec.on_change_handler = INLAY_UI_NONE;
    spec.on_hunk_handler = INLAY_UI_NONE;

    if (has_props) {
        spec.old_text = inlay_ui_string_slice_prop(s, props, "oldText");
        if (!spec.old_text.len && !JS_HasException(s->js)) spec.old_text = inlay_ui_string_slice_prop(s, props, "original");
        if (JS_HasException(s->js)) return false;
        spec.new_text = inlay_ui_string_slice_prop(s, props, "newText");
        if (!spec.new_text.len && !JS_HasException(s->js)) spec.new_text = inlay_ui_string_slice_prop(s, props, "modified");
        if (JS_HasException(s->js)) return false;
        spec.language = inlay_ui_string_slice_prop(s, props, "language");
        if (JS_HasException(s->js)) return false;
        spec.file_path = inlay_ui_string_slice_prop(s, props, "filePath");
        if (!spec.file_path.len && !JS_HasException(s->js)) spec.file_path = inlay_ui_string_slice_prop(s, props, "title");
        if (JS_HasException(s->js)) return false;

        peek_str_prop(s, props, "mode", [&](const char* ch, size_t n) noexcept {
            if (inlay_ui_str_eq(ch, n, "unified") || inlay_ui_str_eq(ch, n, "inline")) {
                spec.mode = 1u;
                return true;
            }
            spec.mode = 0u;
            return true;
        });
        inlay_ui_opt_uint_prop(s, props, "contextLines", 64u, &spec.context_lines);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_bool_prop(s, props, "collapseUnchanged", &spec.collapse_unchanged);
        inlay_ui_opt_bool_prop(s, props, "wordDiff", &spec.word_diff);
        inlay_ui_opt_bool_prop(s, props, "showOverviewRuler", &spec.show_overview_ruler);
        inlay_ui_opt_bool_prop(s, props, "showLineNumbers", &spec.show_line_numbers);
        inlay_ui_opt_bool_prop(s, props, "editable", &spec.editable);
        if (inlay_ui_opt_number_prop(s, props, "fontSize", &spec.font_size, false, 8.0, 48.0)) spec.has_font_size = true;
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onGutterClick", &spec.on_gutter_click_action, &spec.on_gutter_click_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onGutterRange", &spec.on_gutter_range_action, &spec.on_gutter_range_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onChange", &spec.on_change_action, &spec.on_change_handler);
        if (JS_HasException(s->js)) return false;
        inlay_ui_parse_event_prop(s, props, "onHunkAction", &spec.on_hunk_action, &spec.on_hunk_handler);
        if (JS_HasException(s->js)) return false;

        const JSValue ha_v = JS_GetPropertyStr(s->js, props, "hunkActions");
        if (JS_IsArray(ha_v)) {
            uint32_t raw_len = 0u;
            if (read_array_length(s, ha_v, &raw_len) && raw_len > 0u) {
                const uint32_t take = raw_len > 8u ? 8u : raw_len;
                for (uint32_t i = 0u; i < take; ++i) {
                    const JSValue el = JS_GetPropertyUint32(s->js, ha_v, i);
                    if (JS_IsObject(el) && !JS_IsNull(el)) {
                        TextSlice aid = inlay_ui_string_slice_prop(s, el, "id");
                        TextSlice lbl = inlay_ui_string_slice_prop(s, el, "label");
                        if (!lbl.len) lbl = aid;
                        if (!aid.len) aid = lbl;
                        if (aid.len > 0u) {
                            spec.hunk_actions[spec.hunk_action_cnt++] = {aid, lbl};
                        }
                    }
                    JS_FreeValue(s->js, el);
                }
            }
        }
        JS_FreeValue(s->js, ha_v);
        if (JS_HasException(s->js)) return false;

        const JSValue ann_v = JS_GetPropertyStr(s->js, props, "annotations");
        if (JS_IsArray(ann_v)) {
            uint32_t raw_len = 0u;
            if (read_array_length(s, ann_v, &raw_len) && raw_len > 0u) {
                const uint32_t take = raw_len > 64u ? 64u : raw_len;
                const uint32_t off = inlay_ui_alloc_pool(s, s->diff_annotations, s->diff_annotation_count,
                                                         s->diff_annotation_cap, take, 2048u);
                if (off == INLAY_UI_NONE) { JS_FreeValue(s->js, ann_v); return false; }
                spec.ann_off = off;
                spec.ann_cnt = take;
                for (uint32_t i = 0u; i < take; ++i) {
                    DiffAnnotationItem it = {};
                    it.is_new_side = true;
                    it.height = 54.f;
                    const JSValue el = JS_GetPropertyUint32(s->js, ann_v, i);
                    if (JS_IsObject(el) && !JS_IsNull(el)) {
                        inlay_ui_opt_uint_prop(s, el, "line", 10000000u, &it.line);
                        inlay_ui_opt_number_prop(s, el, "height", &it.height, false, 24.0, 1024.0);
                        peek_str_prop(s, el, "side", [&](const char* ch, size_t n) noexcept {
                            if (inlay_ui_str_eq(ch, n, "old") || inlay_ui_str_eq(ch, n, "left")) {
                                it.is_new_side = false; return true;
                            }
                            it.is_new_side = true; return true;
                        });
                        it.author = inlay_ui_string_slice_prop(s, el, "author");
                        it.body = inlay_ui_string_slice_prop(s, el, "body");
                        parse_color_prop(s, el, "color", &it.color);
                    }
                    JS_FreeValue(s->js, el);
                    s->diff_annotations[off + i] = it;
                }
            }
        }
        JS_FreeValue(s->js, ann_v);
        if (JS_HasException(s->js)) return false;
    }

    s->diff_specs[idx] = spec;
    return true;
}

void build_diff_cache(DiffViewCacheEntry& ce, EtchStr old_text, EtchStr new_text,
                      const DiffViewSpec& spec, bool dark) {
    free_diff_cache_entry(ce);
    struct LineView { const char* ptr; uint32_t len; uint64_t hash; };
    auto split_lines = [](EtchStr txt, LineView*& out_arr, uint32_t& out_cnt) {
        out_arr = nullptr; out_cnt = 0u;
        if (!txt.chars || txt.length == 0u) return;
        uint32_t lines = 1u;
        for (uint32_t i = 0u; i < txt.length; ++i) {
            if (txt.chars[i] == '\n') ++lines;
        }
        out_arr = static_cast<LineView*>(SDL_malloc(size_t(lines) * sizeof(LineView)));
        if (!out_arr) return;
        uint32_t s = 0u, idx = 0u;
        for (uint32_t i = 0u; i <= txt.length; ++i) {
            if (i == txt.length || txt.chars[i] == '\n') {
                uint32_t e = i;
                if (e > s && txt.chars[e - 1u] == '\r') --e;
                const uint32_t ln = e - s;
                out_arr[idx++] = {txt.chars + s, ln, inlay_fnv1a_64(txt.chars + s, ln)};
                s = i + 1u;
            }
        }
        out_cnt = idx;
    };
    LineView* old_l = nullptr; uint32_t n_old = 0u;
    LineView* new_l = nullptr; uint32_t n_new = 0u;
    split_lines(old_text, old_l, n_old);
    split_lines(new_text, new_l, n_new);

    auto line_eq = [](const LineView& a, const LineView& b) noexcept -> bool {
        return a.hash == b.hash && a.len == b.len && (a.len == 0u || std::memcmp(a.ptr, b.ptr, a.len) == 0);
    };

    struct DiffOp { int8_t op; uint32_t oi; uint32_t ni; };
    const uint32_t max_ops = n_old + n_new + 4u;
    auto* ops = static_cast<DiffOp*>(SDL_malloc(size_t(max_ops) * sizeof(DiffOp)));
    uint32_t op_cnt = 0u;
    if (ops) {
        uint32_t pref = 0u;
        while (pref < n_old && pref < n_new && line_eq(old_l[pref], new_l[pref])) ++pref;
        uint32_t suff = 0u;
        while (suff < (n_old - pref) && suff < (n_new - pref) &&
               line_eq(old_l[n_old - 1u - suff], new_l[n_new - 1u - suff])) ++suff;

        for (uint32_t i = 0u; i < pref; ++i) ops[op_cnt++] = {0, i, i};

        const uint32_t mo = n_old - pref - suff;
        const uint32_t mn = n_new - pref - suff;
        if (mo == 0u) {
            for (uint32_t j = 0u; j < mn; ++j) ops[op_cnt++] = {1, 0u, pref + j};
        } else if (mn == 0u) {
            for (uint32_t i = 0u; i < mo; ++i) ops[op_cnt++] = {-1, pref + i, 0u};
        } else if (uint64_t(mo) * uint64_t(mn) <= 262144ull) {
            const uint32_t stride = mn + 1u;
            auto* dp = static_cast<uint16_t*>(SDL_calloc(size_t(mo + 1u) * size_t(stride), sizeof(uint16_t)));
            if (dp) {
                for (uint32_t i = mo; i-- > 0u;) {
                    for (uint32_t j = mn; j-- > 0u;) {
                        if (line_eq(old_l[pref + i], new_l[pref + j])) {
                            dp[i * stride + j] = uint16_t(dp[(i + 1u) * stride + (j + 1u)] + 1u);
                        } else {
                            const uint16_t d1 = dp[(i + 1u) * stride + j];
                            const uint16_t d2 = dp[i * stride + (j + 1u)];
                            dp[i * stride + j] = d1 >= d2 ? d1 : d2;
                        }
                    }
                }
                uint32_t i = 0u, j = 0u;
                while (i < mo || j < mn) {
                    if (i < mo && j < mn && line_eq(old_l[pref + i], new_l[pref + j])) {
                        ops[op_cnt++] = {0, pref + i, pref + j}; ++i; ++j;
                    } else if (j < mn && (i == mo || dp[i * stride + (j + 1u)] > dp[(i + 1u) * stride + j])) {
                        ops[op_cnt++] = {1, 0u, pref + j}; ++j;
                    } else {
                        ops[op_cnt++] = {-1, pref + i, 0u}; ++i;
                    }
                }
                SDL_free(dp);
            }
        } else {
            uint32_t i = 0u, j = 0u;
            while (i < mo || j < mn) {
                if (i < mo && j < mn && line_eq(old_l[pref + i], new_l[pref + j])) {
                    ops[op_cnt++] = {0, pref + i, pref + j}; ++i; ++j;
                } else {
                    bool matched = false;
                    for (uint32_t k = 1u; k <= 32u && !matched; ++k) {
                        if (i + k < mo && j < mn && line_eq(old_l[pref + i + k], new_l[pref + j])) {
                            for (uint32_t t = 0u; t < k; ++t) ops[op_cnt++] = {-1, pref + i + t, 0u};
                            i += k; matched = true;
                        } else if (j + k < mn && i < mo && line_eq(old_l[pref + i], new_l[pref + j + k])) {
                            for (uint32_t t = 0u; t < k; ++t) ops[op_cnt++] = {1, 0u, pref + j + t};
                            j += k; matched = true;
                        }
                    }
                    if (!matched) {
                        if (i < mo) ops[op_cnt++] = {-1, pref + i++, 0u};
                        if (j < mn) ops[op_cnt++] = {1, 0u, pref + j++};
                    }
                }
            }
        }
        for (uint32_t i = 0u; i < suff; ++i) {
            ops[op_cnt++] = {0, n_old - suff + i, n_new - suff + i};
        }
    }

    struct StrBuf {
        char* data = nullptr; uint32_t len = 0u; uint32_t cap = 0u;
        void push(const char* p, uint32_t n) {
            if (n == 0u) return;
            if (!inlay_grow_pod(data, cap, len + n + 1u, 256u, 8u * 1024u * 1024u)) return;
            std::memcpy(data + len, p, n);
            len += n;
            data[len] = '\0';
        }
        void push_line(const char* p, uint32_t n, bool has_prev) {
            if (has_prev) push("\n", 1u);
            if (n > 0u) push(p, n);
        }
    };

    const EtchColor word_del_bg = dark ? etch_rgba8(239, 68, 68, 95) : etch_rgba8(220, 38, 38, 62);
    const EtchColor word_add_bg = dark ? etch_rgba8(34, 197, 94, 92) : etch_rgba8(22, 163, 74, 58);
    const EtchColor ov_del_col = dark ? etch_rgba8(248, 113, 113, 210) : etch_rgba8(220, 38, 38, 195);
    const EtchColor ov_add_col = dark ? etch_rgba8(74, 222, 128, 210) : etch_rgba8(22, 163, 74, 195);

    auto push_hl = [](EtchCodeInlineHighlight*& arr, uint32_t& cnt, uint32_t& cap,
                      uint32_t line, uint32_t sc, uint32_t ec, EtchColor bg) {
        if (ec <= sc) return;
        if (!inlay_grow_pod(arr, cap, cnt + 1u, 16u, 16384u)) return;
        EtchCodeInlineHighlight h = etch_code_inline_highlight_default();
        h.line_index = line;
        h.start_col_byte = sc;
        h.end_col_byte = ec;
        h.background = bg;
        h.radius_px = 2.f;
        arr[cnt++] = h;
    };
    auto push_marker = [](EtchCodeOverviewMarker*& arr, uint32_t& cnt, uint32_t& cap,
                          uint32_t line, EtchColor col, uint8_t lane) {
        if (!inlay_grow_pod(arr, cap, cnt + 1u, 16u, 16384u)) return;
        EtchCodeOverviewMarker m = etch_code_overview_marker_default();
        m.line_index = line;
        m.color = col;
        m.lane = lane;
        arr[cnt++] = m;
    };

    uint32_t left_hl_cap = 0u, right_hl_cap = 0u, uni_hl_cap = 0u, mk_cap = 0u, hunk_cap = 0u;
    uint32_t split_cap = 0u, uni_cap = 0u;

    StrBuf lb, rb, ub;
    uint32_t p = 0u;
    while (p < op_cnt) {
        if (ops[p].op == 0) {
            const auto& lv = old_l[ops[p].oi];
            const uint32_t si = ce.split_line_count;
            if (inlay_grow_pod(ce.left_meta, split_cap, si + 1u, 32u, 262144u)) {
                auto* rm = static_cast<DiffLineMeta*>(SDL_realloc(ce.right_meta, size_t(split_cap) * sizeof(DiffLineMeta)));
                if (rm) ce.right_meta = rm;
            }
            if (ce.left_meta && ce.right_meta && si < split_cap) {
                ce.left_meta[si] = {0, ops[p].oi + 1u};
                ce.right_meta[si] = {0, ops[p].ni + 1u};
                lb.push_line(lv.ptr, lv.len, si > 0u);
                rb.push_line(lv.ptr, lv.len, si > 0u);
                ++ce.split_line_count;
            }
            const uint32_t ui = ce.unified_line_count;
            if (inlay_grow_pod(ce.unified_meta, uni_cap, ui + 1u, 32u, 262144u)) {
                ce.unified_meta[ui] = {0, ops[p].ni + 1u};
                ub.push_line(lv.ptr, lv.len, ui > 0u);
                ++ce.unified_line_count;
            }
            ++p;
            continue;
        }
        uint32_t del_start = p;
        while (p < op_cnt && ops[p].op == -1) ++p;
        uint32_t del_end = p;
        uint32_t add_start = p;
        while (p < op_cnt && ops[p].op == 1) ++p;
        uint32_t add_end = p;
        const uint32_t dc = del_end - del_start;
        const uint32_t ac = add_end - add_start;
        ce.removed_lines += dc;
        ce.added_lines += ac;
        const uint32_t rows = dc > ac ? dc : ac;
        const uint32_t hunk_split_start = ce.split_line_count;
        const uint32_t hunk_uni_start = ce.unified_line_count;
        const uint32_t hunk_old_start = (dc > 0u) ? (ops[del_start].oi + 1u)
            : ((del_start > 0u && ops[del_start - 1u].op == 0) ? (ops[del_start - 1u].oi + 2u) : 1u);
        const uint32_t hunk_new_start = (ac > 0u) ? (ops[add_start].ni + 1u)
            : ((del_start > 0u && ops[del_start - 1u].op == 0) ? (ops[del_start - 1u].ni + 2u) : 1u);

        for (uint32_t r = 0u; r < rows; ++r) {
            const uint32_t si = ce.split_line_count;
            if (inlay_grow_pod(ce.left_meta, split_cap, si + 1u, 32u, 262144u)) {
                auto* rm = static_cast<DiffLineMeta*>(SDL_realloc(ce.right_meta, size_t(split_cap) * sizeof(DiffLineMeta)));
                if (rm) ce.right_meta = rm;
            }
            if (!ce.left_meta || !ce.right_meta || si >= split_cap) break;
            if (r < dc) {
                const auto& dl = old_l[ops[del_start + r].oi];
                ce.left_meta[si] = {-1, ops[del_start + r].oi + 1u};
                lb.push_line(dl.ptr, dl.len, si > 0u);
                if (spec.mode == 0u) push_marker(ce.markers, ce.marker_count, mk_cap, si, ov_del_col, 0u);
            } else {
                ce.left_meta[si] = {2, 0u};
                lb.push_line("", 0u, si > 0u);
            }
            if (r < ac) {
                const auto& al = new_l[ops[add_start + r].ni];
                ce.right_meta[si] = {1, ops[add_start + r].ni + 1u};
                rb.push_line(al.ptr, al.len, si > 0u);
                if (spec.mode == 0u) push_marker(ce.markers, ce.marker_count, mk_cap, si, ov_add_col, 1u);
            } else {
                ce.right_meta[si] = {2, 0u};
                rb.push_line("", 0u, si > 0u);
            }
            if (spec.word_diff && r < dc && r < ac) {
                const auto& dl = old_l[ops[del_start + r].oi];
                const auto& al = new_l[ops[add_start + r].ni];
                uint32_t cp = 0u;
                while (cp < dl.len && cp < al.len && dl.ptr[cp] == al.ptr[cp]) ++cp;
                uint32_t cs = 0u;
                while (cs < (dl.len - cp) && cs < (al.len - cp) &&
                       dl.ptr[dl.len - 1u - cs] == al.ptr[al.len - 1u - cs]) ++cs;
                if (dl.len > cp + cs) push_hl(ce.left_hls, ce.left_hl_count, left_hl_cap, si, cp, dl.len - cs, word_del_bg);
                if (al.len > cp + cs) push_hl(ce.right_hls, ce.right_hl_count, right_hl_cap, si, cp, al.len - cs, word_add_bg);
            }
            ++ce.split_line_count;
        }

        const uint32_t uni_del_base = ce.unified_line_count;
        for (uint32_t r = 0u; r < dc; ++r) {
            const uint32_t ui = ce.unified_line_count;
            if (!inlay_grow_pod(ce.unified_meta, uni_cap, ui + 1u, 32u, 262144u)) break;
            const auto& dl = old_l[ops[del_start + r].oi];
            ce.unified_meta[ui] = {-1, ops[del_start + r].oi + 1u};
            ub.push_line(dl.ptr, dl.len, ui > 0u);
            if (spec.mode == 1u) push_marker(ce.markers, ce.marker_count, mk_cap, ui, ov_del_col, 0u);
            ++ce.unified_line_count;
        }
        const uint32_t uni_add_base = ce.unified_line_count;
        for (uint32_t r = 0u; r < ac; ++r) {
            const uint32_t ui = ce.unified_line_count;
            if (!inlay_grow_pod(ce.unified_meta, uni_cap, ui + 1u, 32u, 262144u)) break;
            const auto& al = new_l[ops[add_start + r].ni];
            ce.unified_meta[ui] = {1, ops[add_start + r].ni + 1u};
            ub.push_line(al.ptr, al.len, ui > 0u);
            if (spec.mode == 1u) push_marker(ce.markers, ce.marker_count, mk_cap, ui, ov_add_col, 1u);
            ++ce.unified_line_count;
        }
        if (spec.word_diff && dc > 0u && ac > 0u) {
            const uint32_t pairs = dc < ac ? dc : ac;
            for (uint32_t r = 0u; r < pairs; ++r) {
                const auto& dl = old_l[ops[del_start + r].oi];
                const auto& al = new_l[ops[add_start + r].ni];
                uint32_t cp = 0u;
                while (cp < dl.len && cp < al.len && dl.ptr[cp] == al.ptr[cp]) ++cp;
                uint32_t cs = 0u;
                while (cs < (dl.len - cp) && cs < (al.len - cp) &&
                       dl.ptr[dl.len - 1u - cs] == al.ptr[al.len - 1u - cs]) ++cs;
                if (dl.len > cp + cs) push_hl(ce.unified_hls, ce.unified_hl_count, uni_hl_cap, uni_del_base + r, cp, dl.len - cs, word_del_bg);
                if (al.len > cp + cs) push_hl(ce.unified_hls, ce.unified_hl_count, uni_hl_cap, uni_add_base + r, cp, al.len - cs, word_add_bg);
            }
        }
        if (inlay_grow_pod(ce.hunks, hunk_cap, ce.hunk_count + 1u, 16u, 4096u)) {
            ce.hunks[ce.hunk_count++] = {
                hunk_split_start, ce.split_line_count,
                hunk_uni_start, ce.unified_line_count,
                hunk_old_start, dc, hunk_new_start, ac
            };
        }
    }

    if (spec.collapse_unchanged) {
        const DiffLineMeta* meta = (spec.mode == 1u) ? ce.unified_meta : ce.left_meta;
        const uint32_t total = (spec.mode == 1u) ? ce.unified_line_count : ce.split_line_count;
        uint32_t fcap = 0u;
        uint32_t i = 0u;
        while (i < total) {
            if (meta && meta[i].kind == 0) {
                uint32_t j = i;
                while (j < total && meta[j].kind == 0) ++j;
                const uint32_t fs = (i == 0u) ? 0u : (i + spec.context_lines);
                const uint32_t fe = (j == total) ? (j > 0u ? j - 1u : 0u) : (j > spec.context_lines + 1u ? j - spec.context_lines - 1u : 0u);
                if (fe > fs + 1u && inlay_grow_pod(ce.folds, fcap, ce.fold_count + 1u, 8u, 512u)) {
                    ce.folds[ce.fold_count++] = {fs, fe, true};
                }
                i = j;
            } else {
                ++i;
            }
        }
    }

    ce.left_buf = etch_create_text_buffer({lb.data ? lb.data : "", lb.len, false}, lb.len + 64u);
    ce.right_buf = etch_create_text_buffer({rb.data ? rb.data : "", rb.len, false}, rb.len + 64u);
    ce.unified_buf = etch_create_text_buffer({ub.data ? ub.data : "", ub.len, false}, ub.len + 64u);
    SDL_free(lb.data);
    SDL_free(rb.data);
    SDL_free(ub.data);
    SDL_free(ops);
    SDL_free(old_l);
    SDL_free(new_l);
}

void draw_diff_view_widget(InlaySurface* s, const NodeStyle& st, uint32_t id,
                           const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->diff_spec_count) return;
    const DiffViewSpec& spec = s->diff_specs[st.ext_idx];
    const EtchStr old_s = inlay_ui_slice(s, spec.old_text);
    const EtchStr new_s = inlay_ui_slice(s, spec.new_text);

    uint64_t dh = inlay_fnv1a_64(old_s.chars, old_s.length);
    dh ^= inlay_fnv1a_64(new_s.chars, new_s.length) * 1099511628211ull;
    dh ^= uint64_t(spec.mode) | (uint64_t(spec.collapse_unchanged ? 1u : 0u) << 2) |
          (uint64_t(spec.word_diff ? 1u : 0u) << 3) | (uint64_t(spec.context_lines) << 4);

    DiffViewCacheEntry* ce = nullptr;
    for (uint32_t i = 0u; i < s->diff_cache_count; ++i) {
        if (s->diff_cache[i].element_id == id) { ce = &s->diff_cache[i]; break; }
    }
    if (!ce) {
        if (s->diff_cache_count < 64u && inlay_grow_pod(s->diff_cache, s->diff_cache_cap, s->diff_cache_count + 1u, 8u, 64u)) {
            ce = &s->diff_cache[s->diff_cache_count++];
            *ce = {};
            ce->element_id = id;
        } else if (s->diff_cache_count > 0u) {
            uint32_t victim = 0u;
            for (uint32_t i = 1u; i < s->diff_cache_count; ++i) {
                if (s->diff_cache[i].last_used_frame < s->diff_cache[victim].last_used_frame) victim = i;
            }
            ce = &s->diff_cache[victim];
            free_diff_cache_entry(*ce);
            *ce = {};
            ce->element_id = id;
        }
    }
    if (!ce) return;
    ce->last_used_frame = s->frame_counter;
    const bool dark = inlay_is_dark_theme(theme);
    if (!ce->left_buf || ce->diff_hash != dh || ce->mode != spec.mode ||
        ce->collapse_unchanged != spec.collapse_unchanged || ce->word_diff != spec.word_diff ||
        ce->context_lines != spec.context_lines) {
        build_diff_cache(*ce, old_s, new_s, spec, dark);
        ce->diff_hash = dh;
        ce->mode = spec.mode;
        ce->collapse_unchanged = spec.collapse_unchanged;
        ce->word_diff = spec.word_diff;
        ce->context_lines = spec.context_lines;
    }

    const uint32_t uni_ed_id = etch_id_index(id, 0xDF10u);
    const uint32_t left_id = etch_id_index(id, 0xDF01u);
    const uint32_t right_id = etch_id_index(id, 0xDF02u);

    auto outer = etch_box_desc_default();
    outer.axis = ETCH_AXIS_COL;
    outer.width = etch_sizing_grow(0.f, 0.f);
    outer.height = etch_sizing_grow(0.f, 0.f);
    outer.background = theme.bg_surface;
    outer.border_color = theme.border_subtle;
    outer.border_width = 1.f;
    outer.radius = etch_corners_all(theme.radius_panel_px);
    outer.clip_children = true;
    inlay_ui_populate_box_desc(s, st, theme, id, outer, use_layer);
    outer.axis = ETCH_AXIS_COL;
    etch_begin_box(&outer);

    const EtchStr fp = inlay_ui_slice(s, spec.file_path);
    if (ce->hunk_count > 0u && ce->active_hunk >= ce->hunk_count) ce->active_hunk = 0u;
    const bool show_header = (fp.length > 0u || ce->hunk_count > 0u || spec.hunk_action_cnt > 0u);
    if (show_header) {
        auto hdr = etch_box_desc_default();
        hdr.axis = ETCH_AXIS_ROW;
        hdr.width = etch_sizing_grow(0.f, 0.f);
        hdr.height = etch_sizing_fixed(32.f);
        hdr.padding = etch_padding_xy(12.f, 0.f);
        hdr.gap = 8.f;
        hdr.align_y = ETCH_ALIGN_Y_CENTER;
        hdr.background = theme.bg_chrome;
        hdr.border_sides = {0.f, 0.f, 0.f, 1.f};
        hdr.border_color = theme.border_subtle;
        etch_begin_box(&hdr);

        auto ftd = etch_text_desc_default();
        ftd.font_id = uint16_t(ETCH_FONT_MONO);
        ftd.font_size_px = 12.f;
        ftd.color = theme.text_primary;
        ftd.wrap = false;
        ftd.ellipsis = true;
        etch_text(fp.length > 0u ? inlay_ui_frame_text(s, fp) : ETCH_STR_LIT("Diff"), &ftd);

        auto sp = etch_box_desc_default();
        sp.width = etch_sizing_grow(0.f, 0.f);
        sp.height = etch_sizing_fixed(1.f);
        etch_box(&sp);

        char abuf[24], rbuf[24];
        const int an = std::snprintf(abuf, sizeof(abuf), "+%u", ce->added_lines);
        const int rn = std::snprintf(rbuf, sizeof(rbuf), "-%u", ce->removed_lines);
        if (an > 0) {
            auto td = etch_text_desc_default();
            td.font_id = uint16_t(ETCH_FONT_MONO);
            td.font_size_px = 11.5f;
            td.color = dark ? etch_rgb_hex(0x4ADE80u, 255u) : etch_rgb_hex(0x15803Du, 255u);
            td.wrap = false;
            etch_text(inlay_ui_frame_text(s, {abuf, uint32_t(an), false}), &td);
        }
        if (rn > 0) {
            auto td = etch_text_desc_default();
            td.font_id = uint16_t(ETCH_FONT_MONO);
            td.font_size_px = 11.5f;
            td.color = dark ? etch_rgb_hex(0xF87171u, 255u) : etch_rgb_hex(0xB91C1Cu, 255u);
            td.wrap = false;
            etch_text(inlay_ui_frame_text(s, {rbuf, uint32_t(rn), false}), &td);
        }

        if (ce->hunk_count > 0u) {
            auto jump_to_hunk = [&](uint32_t hi) {
                if (hi >= ce->hunk_count) return;
                const auto& hk = ce->hunks[hi];
                if (spec.mode == 1u) {
                    etch_code_editor_scroll_to_line(uni_ed_id, hk.uni_start, ETCH_ALIGN_Y_TOP);
                } else {
                    etch_code_editor_scroll_to_line(left_id, hk.split_start, ETCH_ALIGN_Y_TOP);
                    etch_code_editor_scroll_to_line(right_id, hk.split_start, ETCH_ALIGN_Y_TOP);
                }
                etch_request_redraw(s->etch);
            };
            auto pbtn = etch_button_desc_default();
            pbtn.id = etch_id_index(id, 0xDF21u);
            pbtn.variant = ETCH_BUTTON_GHOST;
            pbtn.label = ETCH_STR_LIT("Prev");
            pbtn.font_size_px = 11.f;
            pbtn.padding = etch_padding_xy(6.f, 2.f);
            if (etch_button(&pbtn)) {
                ce->active_hunk = (ce->active_hunk == 0u) ? (ce->hunk_count - 1u) : (ce->active_hunk - 1u);
                jump_to_hunk(ce->active_hunk);
            }
            char hbuf[32];
            const int hn = std::snprintf(hbuf, sizeof(hbuf), "%u/%u", ce->active_hunk + 1u, ce->hunk_count);
            if (hn > 0) {
                auto htd = etch_text_desc_default();
                htd.font_id = uint16_t(ETCH_FONT_MONO);
                htd.font_size_px = 11.f;
                htd.color = theme.text_muted;
                htd.wrap = false;
                etch_text(inlay_ui_frame_text(s, {hbuf, uint32_t(hn), false}), &htd);
            }
            auto nbtn = etch_button_desc_default();
            nbtn.id = etch_id_index(id, 0xDF22u);
            nbtn.variant = ETCH_BUTTON_GHOST;
            nbtn.label = ETCH_STR_LIT("Next");
            nbtn.font_size_px = 11.f;
            nbtn.padding = etch_padding_xy(6.f, 2.f);
            if (etch_button(&nbtn)) {
                ce->active_hunk = (ce->active_hunk + 1u) % ce->hunk_count;
                jump_to_hunk(ce->active_hunk);
            }
            for (uint32_t ai = 0u; ai < spec.hunk_action_cnt; ++ai) {
                auto abtn = etch_button_desc_default();
                abtn.id = etch_id_index(id, 0xDF30u + ai);
                abtn.label = inlay_ui_frame_text(s, inlay_ui_slice(s, spec.hunk_actions[ai].label));
                abtn.font_size_px = 11.f;
                abtn.padding = etch_padding_xy(7.f, 2.f);
                if (etch_button(&abtn) && events && has_event(spec.on_hunk_action, spec.on_hunk_handler)) {
                    const auto& hk = ce->hunks[ce->active_hunk];
                    const EtchStr aid = inlay_ui_slice(s, spec.hunk_actions[ai].id);
                    auto jb = frame_json_begin(s->etch, frame_json_cap(aid.length, 160u));
                    char p1[64];
                    const int n1 = std::snprintf(p1, sizeof(p1), "{\"hunkIndex\":%u,\"actionId\":", ce->active_hunk);
                    if (n1 > 0) frame_json_lit(jb, p1);
                    frame_json_quoted(jb, aid);
                    char p2[128];
                    const int n2 = std::snprintf(p2, sizeof(p2),
                        ",\"oldStart\":%u,\"oldCount\":%u,\"newStart\":%u,\"newCount\":%u}",
                        hk.old_start, hk.old_count, hk.new_start, hk.new_count);
                    if (n2 > 0) frame_json_lit(jb, p2);
                    const EtchStr payload = frame_json_finish(jb);
                    if (payload.length > 0u) {
                        queue_slice_event(s, events, spec.on_hunk_action, spec.on_hunk_handler, payload, true);
                    }
                }
            }
        }
        etch_end_box();
    }

    const EtchColor row_add_bg = dark ? etch_rgba8(34, 197, 94, 30) : etch_rgba8(22, 163, 74, 24);
    const EtchColor row_del_bg = dark ? etch_rgba8(239, 68, 68, 32) : etch_rgba8(220, 38, 38, 24);
    const EtchColor row_pad_bg = dark ? etch_rgba8(255, 255, 255, 10) : etch_rgba8(15, 23, 42, 10);
    const EtchColor bar_add = dark ? etch_rgba8(74, 222, 128, 220) : etch_rgba8(22, 163, 74, 210);
    const EtchColor bar_del = dark ? etch_rgba8(248, 113, 113, 220) : etch_rgba8(220, 38, 38, 210);

    struct DiffDecoCtx {
        const DiffLineMeta* meta;
        uint32_t count;
        const EditorFoldItem* folds;
        uint32_t fold_count;
        EtchColor add_bg, del_bg, pad_bg, add_bar, del_bar, fold_col;
    };
    auto diff_deco_fn = [](uint32_t line_idx, EtchCodeLineDecoration* out, void* user) -> bool {
        const auto* dc = static_cast<const DiffDecoCtx*>(user);
        if (!dc->meta || line_idx >= dc->count) return false;
        const int8_t k = dc->meta[line_idx].kind;
        const uint32_t orig = dc->meta[line_idx].orig_line;
        if (k == 2) {
            out->row_bg = dc->pad_bg;
            out->hide_line_number = true;
            return true;
        }
        if (orig > 0u) {
            out->has_display_line_number = true;
            out->display_line_number = orig;
        }
        if (k == 1) {
            out->row_bg = dc->add_bg;
            out->gutter_bar_color = dc->add_bar;
            out->gutter_tag = ETCH_STR_LIT("+");
            out->gutter_tag_color = dc->add_bar;
            return true;
        }
        if (k == -1) {
            out->row_bg = dc->del_bg;
            out->gutter_bar_color = dc->del_bar;
            out->gutter_tag = ETCH_STR_LIT("-");
            out->gutter_tag_color = dc->del_bar;
            return true;
        }
        for (uint32_t fi = 0u; fi < dc->fold_count; ++fi) {
            if (dc->folds[fi].start == line_idx) {
                out->gutter_tag = dc->folds[fi].collapsed ? ETCH_STR_LIT(">") : ETCH_STR_LIT("v");
                out->gutter_tag_color = dc->fold_col;
                break;
            }
        }
        return orig > 0u;
    };

    auto toggle_fold_on_gutter = [&](int32_t clicked_line) -> bool {
        if (clicked_line < 0 || !ce->folds || ce->fold_count == 0u) return false;
        const uint32_t uline = static_cast<uint32_t>(clicked_line);
        for (uint32_t fi = 0u; fi < ce->fold_count; ++fi) {
            if (ce->folds[fi].start == uline) {
                ce->folds[fi].collapsed = !ce->folds[fi].collapsed;
                etch_request_redraw(s->etch);
                return true;
            }
        }
        return false;
    };

    const EtchStr lang = spec.language.len > 0u ? inlay_ui_frame_text(s, inlay_ui_slice(s, spec.language)) : EtchStr{};
    const float font_px = spec.has_font_size ? spec.font_size : ((st.flags & NfHasSize) && st.font_size > 0.f ? st.font_size : 13.f);

    auto emit_gutter_events = [&](const EtchCodeEditorResult& er, const DiffLineMeta* meta, uint32_t mcnt, const char* side) {
        const bool toggled = toggle_fold_on_gutter(er.clicked_gutter_line);
        if (!events) return;
        if (!toggled && er.clicked_gutter_line >= 0 && has_event(spec.on_gutter_click_action, spec.on_gutter_click_handler)) {
            const uint32_t li = static_cast<uint32_t>(er.clicked_gutter_line);
            const uint32_t orig = (meta && li < mcnt) ? meta[li].orig_line : (li + 1u);
            char buf[96];
            const int n = std::snprintf(buf, sizeof(buf),
                "{\"line\":%d,\"side\":\"%s\",\"origLine\":%u}", er.clicked_gutter_line, side, orig);
            if (n > 0) queue_slice_event(s, events, spec.on_gutter_click_action, spec.on_gutter_click_handler,
                                         inlay_ui_frame_text(s, {buf, uint32_t(n), false}), true);
        }
        if (er.gutter_drag_start_line >= 0 && er.gutter_drag_end_line >= 0 &&
            has_event(spec.on_gutter_range_action, spec.on_gutter_range_handler)) {
            char buf[128];
            const int n = std::snprintf(buf, sizeof(buf),
                "{\"startLine\":%d,\"endLine\":%d,\"dragging\":%s,\"side\":\"%s\"}",
                er.gutter_drag_start_line, er.gutter_drag_end_line, er.gutter_dragging ? "true" : "false", side);
            if (n > 0) queue_slice_event(s, events, spec.on_gutter_range_action, spec.on_gutter_range_handler,
                                         inlay_ui_frame_text(s, {buf, uint32_t(n), false}));
        }
    };

    auto apply_annotations = [&](uint32_t ed_id, bool want_new_side, bool any_side) {
        if (spec.ann_cnt == 0u || spec.ann_off + spec.ann_cnt > s->diff_annotation_count) {
            etch_code_editor_clear_view_zones(ed_id);
            return;
        }
        EtchCodeViewZone zones[64] = {};
        uint32_t zcnt = 0u;
        for (uint32_t ai = 0u; ai < spec.ann_cnt && zcnt < 64u; ++ai) {
            const auto& an = s->diff_annotations[spec.ann_off + ai];
            if (!any_side && an.is_new_side != want_new_side) continue;
            EtchCodeViewZone z = etch_code_view_zone_default();
            z.zone_id = ai + 1u;
            z.after_line = an.line;
            z.height_px = an.height;
            z.fill_color = an.color.set ? inlay_ui_resolve_color(an.color, theme, theme.bg_elevated) : theme.bg_elevated;
            zones[zcnt++] = z;
        }
        if (zcnt > 0u) etch_code_editor_set_view_zones(ed_id, zones, zcnt);
        else etch_code_editor_clear_view_zones(ed_id);
    };

    auto draw_annotation_zones = [&](uint32_t ed_id, bool want_new_side, bool any_side) {
        if (spec.ann_cnt == 0u || spec.ann_off + spec.ann_cnt > s->diff_annotation_count) return;
        for (uint32_t ai = 0u; ai < spec.ann_cnt; ++ai) {
            const auto& an = s->diff_annotations[spec.ann_off + ai];
            if (!any_side && an.is_new_side != want_new_side) continue;
            if (!etch_code_editor_begin_view_zone(ed_id, ai + 1u)) continue;
            auto card = etch_box_desc_default();
            card.axis = ETCH_AXIS_COL;
            card.width = etch_sizing_grow(0.f, 0.f);
            card.height = etch_sizing_grow(0.f, 0.f);
            card.padding = etch_padding_xy(10.f, 6.f);
            card.gap = 3.f;
            card.border_sides = {3.f, 0.f, 0.f, 0.f};
            card.border_color = an.color.set ? inlay_ui_resolve_color(an.color, theme, theme.accent) : theme.accent;
            card.background = theme.bg_elevated;
            etch_begin_box(&card);
            if (an.author.len > 0u) {
                auto at = etch_text_desc_default();
                at.font_id = uint16_t(ETCH_FONT_SEMIBOLD);
                at.font_size_px = 11.5f;
                at.color = theme.accent;
                at.wrap = false;
                etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, an.author)), &at);
            }
            if (an.body.len > 0u) {
                auto bt = etch_text_desc_default();
                bt.font_size_px = 12.f;
                bt.color = theme.text_primary;
                bt.wrap = true;
                etch_text(inlay_ui_frame_text(s, inlay_ui_slice(s, an.body)), &bt);
            }
            etch_end_box();
            etch_code_editor_end_view_zone();
        }
    };

    if (spec.mode == 1u) {
        const uint32_t ed_id = uni_ed_id;
        auto ed = etch_code_editor_desc_default();
        ed.id = ed_id;
        ed.buffer = ce->unified_buf;
        ed.width = etch_sizing_grow(0.f, 0.f);
        ed.height = etch_sizing_grow(0.f, 0.f);
        ed.font_size_px = font_px;
        ed.read_only = true;
        ed.show_line_numbers = spec.show_line_numbers;
        ed.show_overview_ruler = spec.show_overview_ruler;
        ed.gutter_tag_width_px = 16.f;
        ed.border_width = 0.f;
        ed.language = lang;
        if (s->code_tok_fn) { ed.tokenizer = s->code_tok_fn; ed.tokenizer_user_data = s->code_tok_user_data; }
        auto* dc = static_cast<DiffDecoCtx*>(etch_frame_arena_alloc(s->etch, sizeof(DiffDecoCtx), alignof(DiffDecoCtx)));
        if (dc) {
            *dc = {ce->unified_meta, ce->unified_line_count, ce->folds, ce->fold_count,
                   row_add_bg, row_del_bg, row_pad_bg, bar_add, bar_del, theme.text_muted};
            ed.line_decorator = diff_deco_fn;
            ed.line_decorator_user_data = dc;
        }
        if (ce->unified_hl_count > 0u) etch_code_editor_set_inline_highlights(ed_id, ce->unified_hls, ce->unified_hl_count);
        else etch_code_editor_clear_inline_highlights(ed_id);
        if (spec.show_overview_ruler && ce->marker_count > 0u) etch_code_editor_set_overview_markers(ed_id, ce->markers, ce->marker_count);
        else etch_code_editor_clear_overview_markers(ed_id);
        for (uint32_t fi = 0u; fi < ce->fold_count; ++fi) {
            etch_code_editor_set_fold(ed_id, ce->folds[fi].start, ce->folds[fi].end, ce->folds[fi].collapsed);
        }
        apply_annotations(ed_id, true, true);
        const EtchCodeEditorResult er = etch_code_editor(&ed);
        draw_annotation_zones(ed_id, true, true);
        emit_gutter_events(er, ce->unified_meta, ce->unified_line_count, "unified");
    } else {
        const EtchVec2 lso = etch_code_editor_scroll_offset(left_id);
        const EtchVec2 rso = etch_code_editor_scroll_offset(right_id);
        const bool lx_chg = std::fabs(lso.x - ce->last_left_scroll_x) > 0.5f;
        const bool rx_chg = std::fabs(rso.x - ce->last_right_scroll_x) > 0.5f;
        const bool ly_chg = std::fabs(lso.y - ce->last_left_scroll_y) > 0.5f;
        const bool ry_chg = std::fabs(rso.y - ce->last_right_scroll_y) > 0.5f;
        if (lx_chg || ly_chg) {
            const EtchVec2 tgt = {lx_chg ? lso.x : rso.x, ly_chg ? lso.y : rso.y};
            etch_code_editor_set_scroll_offset(right_id, tgt, true);
            ce->last_left_scroll_x = tgt.x;  ce->last_right_scroll_x = tgt.x;
            ce->last_left_scroll_y = tgt.y;  ce->last_right_scroll_y = tgt.y;
        } else if (rx_chg || ry_chg) {
            const EtchVec2 tgt = {rx_chg ? rso.x : lso.x, ry_chg ? rso.y : lso.y};
            etch_code_editor_set_scroll_offset(left_id, tgt, true);
            ce->last_left_scroll_x = tgt.x;  ce->last_right_scroll_x = tgt.x;
            ce->last_left_scroll_y = tgt.y;  ce->last_right_scroll_y = tgt.y;
        }

        auto row = etch_box_desc_default();
        row.axis = ETCH_AXIS_ROW;
        row.width = etch_sizing_grow(0.f, 0.f);
        row.height = etch_sizing_grow(0.f, 0.f);
        etch_begin_box(&row);

        auto led = etch_code_editor_desc_default();
        led.id = left_id;
        led.buffer = ce->left_buf;
        led.width = etch_sizing_grow(0.f, 0.f);
        led.height = etch_sizing_grow(0.f, 0.f);
        led.font_size_px = font_px;
        led.read_only = true;
        led.show_line_numbers = spec.show_line_numbers;
        led.show_overview_ruler = false;
        led.show_scrollbar = false;
        led.gutter_tag_width_px = 14.f;
        led.border_width = 0.f;
        led.language = lang;
        if (s->code_tok_fn) { led.tokenizer = s->code_tok_fn; led.tokenizer_user_data = s->code_tok_user_data; }
        auto* ldc = static_cast<DiffDecoCtx*>(etch_frame_arena_alloc(s->etch, sizeof(DiffDecoCtx), alignof(DiffDecoCtx)));
        if (ldc) {
            *ldc = {ce->left_meta, ce->split_line_count, ce->folds, ce->fold_count,
                    row_add_bg, row_del_bg, row_pad_bg, bar_add, bar_del, theme.text_muted};
            led.line_decorator = diff_deco_fn;
            led.line_decorator_user_data = ldc;
        }
        if (ce->left_hl_count > 0u) etch_code_editor_set_inline_highlights(left_id, ce->left_hls, ce->left_hl_count);
        else etch_code_editor_clear_inline_highlights(left_id);
        for (uint32_t fi = 0u; fi < ce->fold_count; ++fi) {
            etch_code_editor_set_fold(left_id, ce->folds[fi].start, ce->folds[fi].end, ce->folds[fi].collapsed);
        }
        apply_annotations(left_id, false, false);
        const EtchCodeEditorResult ler = etch_code_editor(&led);
        draw_annotation_zones(left_id, false, false);
        emit_gutter_events(ler, ce->left_meta, ce->split_line_count, "old");

        auto div = etch_box_desc_default();
        div.width = etch_sizing_fixed(1.f);
        div.height = etch_sizing_grow(0.f, 0.f);
        div.background = theme.border_line;
        etch_box(&div);

        auto red = etch_code_editor_desc_default();
        red.id = right_id;
        red.buffer = ce->right_buf;
        red.width = etch_sizing_grow(0.f, 0.f);
        red.height = etch_sizing_grow(0.f, 0.f);
        red.font_size_px = font_px;
        red.read_only = !spec.editable;
        red.show_line_numbers = spec.show_line_numbers;
        red.show_overview_ruler = spec.show_overview_ruler;
        red.gutter_tag_width_px = 14.f;
        red.border_width = 0.f;
        red.language = lang;
        if (s->code_tok_fn) { red.tokenizer = s->code_tok_fn; red.tokenizer_user_data = s->code_tok_user_data; }
        auto* rdc = static_cast<DiffDecoCtx*>(etch_frame_arena_alloc(s->etch, sizeof(DiffDecoCtx), alignof(DiffDecoCtx)));
        if (rdc) {
            *rdc = {ce->right_meta, ce->split_line_count, ce->folds, ce->fold_count,
                    row_add_bg, row_del_bg, row_pad_bg, bar_add, bar_del, theme.text_muted};
            red.line_decorator = diff_deco_fn;
            red.line_decorator_user_data = rdc;
        }
        if (ce->right_hl_count > 0u) etch_code_editor_set_inline_highlights(right_id, ce->right_hls, ce->right_hl_count);
        else etch_code_editor_clear_inline_highlights(right_id);
        if (spec.show_overview_ruler && ce->marker_count > 0u) etch_code_editor_set_overview_markers(right_id, ce->markers, ce->marker_count);
        else etch_code_editor_clear_overview_markers(right_id);
        for (uint32_t fi = 0u; fi < ce->fold_count; ++fi) {
            etch_code_editor_set_fold(right_id, ce->folds[fi].start, ce->folds[fi].end, ce->folds[fi].collapsed);
        }
        apply_annotations(right_id, true, false);
        const EtchCodeEditorResult rer = etch_code_editor(&red);
        draw_annotation_zones(right_id, true, false);
        emit_gutter_events(rer, ce->right_meta, ce->split_line_count, "new");
        if (spec.editable && rer.changed && events && has_event(spec.on_change_action, spec.on_change_handler)) {
            const uint32_t lc = etch_text_buffer_line_count(ce->right_buf);
            char* flat = nullptr;
            uint32_t flen = 0u, fcap = 0u;
            bool first_out = true;
            for (uint32_t li = 0u; li < lc; ++li) {
                if (ce->right_meta && li < ce->split_line_count && ce->right_meta[li].kind == 2) continue;
                char lbuf[4096];
                const uint32_t ls = etch_text_buffer_line_start(ce->right_buf, li);
                const uint32_t le = etch_text_buffer_line_end(ce->right_buf, li);
                const uint32_t got = etch_text_buffer_copy_range(ce->right_buf, ls, le, lbuf, sizeof(lbuf));
                const uint32_t need = flen + (first_out ? 0u : 1u) + got + 1u;
                if (!inlay_grow_pod(flat, fcap, need, 256u, 512u * 1024u)) break;
                if (!first_out) flat[flen++] = '\n';
                if (got > 0u) { std::memcpy(flat + flen, lbuf, got); flen += got; }
                flat[flen] = '\0';
                first_out = false;
            }
            const EtchStr edited = {flat ? flat : "", flen, false};
            auto jb = frame_json_begin(s->etch, frame_json_cap(edited.length, 32u));
            frame_json_quoted(jb, edited);
            const EtchStr payload = frame_json_finish(jb);
            SDL_free(flat);
            if (payload.length > 0u) {
                queue_slice_event(s, events, spec.on_change_action, spec.on_change_handler, payload);
            }
        }

        etch_end_box();
    }

    etch_end_box();
}

} // namespace

// ============================================================================
// Exported Extended Widget Module Entrypoints (inlay_ui_internal.h)
// ============================================================================

bool inlay_ui_is_ext_widget(InlayUiKind kind) noexcept {
    switch (kind) {
        case INLAY_UI_DOC:
        case INLAY_UI_TERMINAL:
        case INLAY_UI_VIRTUAL_LIST:
        case INLAY_UI_TABLE:
        case INLAY_UI_TREE:
        case INLAY_UI_TREE_ITEM:
        case INLAY_UI_MENU:
        case INLAY_UI_CONTEXT_MENU:
        case INLAY_UI_AVATAR:
        case INLAY_UI_TOAST:
        case INLAY_UI_ACCORDION:
        case INLAY_UI_SKELETON:
        case INLAY_UI_PORTAL:
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

void inlay_ui_reset_widget_specs(InlaySurface* s) noexcept {
    if (!s) return;
    s->doc_spec_count = 0u;
    s->term_spec_count = 0u;
    s->vlist_spec_count = 0u;
    s->table_spec_count = 0u;
    s->table_col_count = 0u;
    s->table_cell_count = 0u;
    s->menu_spec_count = 0u;
    s->menu_item_count = 0u;
    s->avatar_spec_count = 0u;
    s->toast_spec_count = 0u;
    s->skeleton_spec_count = 0u;
    s->dnd_spec_count = 0u;
    s->input_rich_spec_count = 0u;
    s->input_highlight_count = 0u;
    s->editor_rich_spec_count = 0u;
    s->editor_hint_count = 0u;
    s->editor_inline_hl_count = 0u;
    s->editor_marker_count = 0u;
    s->editor_line_deco_count = 0u;
    s->editor_fold_count = 0u;
    s->editor_view_zone_count = 0u;
    s->term_search_match_count = 0u;
    s->dock_space_spec_count = 0u;
    s->dock_pane_spec_count = 0u;
    s->sheet_spec_count = 0u;
    s->hover_card_spec_count = 0u;
    s->choice_item_count = 0u;
    s->radio_group_spec_count = 0u;
    s->toggle_group_spec_count = 0u;
    s->color_picker_spec_count = 0u;
    s->chart_spec_count = 0u;
    s->chart_point_count = 0u;
    s->portal_spec_count = 0u;
    s->diff_spec_count = 0u;
    s->diff_annotation_count = 0u;
}

void inlay_ui_free_spec_pools(InlaySurface* s) noexcept {
    if (!s) return;
#define INLAY_FREE_POOL(ptr, cnt, cap) do { SDL_free(s->ptr); s->ptr = nullptr; s->cnt = 0u; s->cap = 0u; } while (0)
    INLAY_FREE_POOL(doc_specs, doc_spec_count, doc_spec_cap);
    INLAY_FREE_POOL(term_specs, term_spec_count, term_spec_cap);
    INLAY_FREE_POOL(vlist_specs, vlist_spec_count, vlist_spec_cap);
    INLAY_FREE_POOL(table_specs, table_spec_count, table_spec_cap);
    INLAY_FREE_POOL(table_cols, table_col_count, table_col_cap);
    INLAY_FREE_POOL(table_cells, table_cell_count, table_cell_cap);
    INLAY_FREE_POOL(menu_specs, menu_spec_count, menu_spec_cap);
    INLAY_FREE_POOL(menu_items, menu_item_count, menu_item_cap);
    INLAY_FREE_POOL(avatar_specs, avatar_spec_count, avatar_spec_cap);
    INLAY_FREE_POOL(toast_specs, toast_spec_count, toast_spec_cap);
    INLAY_FREE_POOL(skeleton_specs, skeleton_spec_count, skeleton_spec_cap);
    INLAY_FREE_POOL(dnd_specs, dnd_spec_count, dnd_spec_cap);
    INLAY_FREE_POOL(input_rich_specs, input_rich_spec_count, input_rich_spec_cap);
    INLAY_FREE_POOL(input_highlights, input_highlight_count, input_highlight_cap);
    INLAY_FREE_POOL(editor_rich_specs, editor_rich_spec_count, editor_rich_spec_cap);
    INLAY_FREE_POOL(editor_hints, editor_hint_count, editor_hint_cap);
    INLAY_FREE_POOL(editor_inline_hls, editor_inline_hl_count, editor_inline_hl_cap);
    INLAY_FREE_POOL(editor_markers, editor_marker_count, editor_marker_cap);
    INLAY_FREE_POOL(editor_line_decos, editor_line_deco_count, editor_line_deco_cap);
    INLAY_FREE_POOL(editor_folds, editor_fold_count, editor_fold_cap);
    INLAY_FREE_POOL(editor_view_zones, editor_view_zone_count, editor_view_zone_cap);
    INLAY_FREE_POOL(term_search_matches, term_search_match_count, term_search_match_cap);
    INLAY_FREE_POOL(dock_space_specs, dock_space_spec_count, dock_space_spec_cap);
    INLAY_FREE_POOL(dock_pane_specs, dock_pane_spec_count, dock_pane_spec_cap);
    INLAY_FREE_POOL(sheet_specs, sheet_spec_count, sheet_spec_cap);
    INLAY_FREE_POOL(hover_card_specs, hover_card_spec_count, hover_card_spec_cap);
    INLAY_FREE_POOL(choice_items, choice_item_count, choice_item_cap);
    INLAY_FREE_POOL(radio_group_specs, radio_group_spec_count, radio_group_spec_cap);
    INLAY_FREE_POOL(toggle_group_specs, toggle_group_spec_count, toggle_group_spec_cap);
    INLAY_FREE_POOL(color_picker_specs, color_picker_spec_count, color_picker_spec_cap);
    INLAY_FREE_POOL(chart_specs, chart_spec_count, chart_spec_cap);
    INLAY_FREE_POOL(chart_points, chart_point_count, chart_point_cap);
    INLAY_FREE_POOL(portal_specs, portal_spec_count, portal_spec_cap);
    INLAY_FREE_POOL(diff_specs, diff_spec_count, diff_spec_cap);
    INLAY_FREE_POOL(diff_annotations, diff_annotation_count, diff_annotation_cap);
#undef INLAY_FREE_POOL
}

void inlay_ui_move_spec_pools(InlaySurface* dst, InlaySurface* src) noexcept {
    inlay_ui_free_spec_pools(dst);
#define INLAY_MOVE_POOL(ptr, cnt, cap) do { \
    dst->ptr = src->ptr; dst->cnt = src->cnt; dst->cap = src->cap; \
    src->ptr = nullptr; src->cnt = 0u; src->cap = 0u; } while (0)
    INLAY_MOVE_POOL(doc_specs, doc_spec_count, doc_spec_cap);
    INLAY_MOVE_POOL(term_specs, term_spec_count, term_spec_cap);
    INLAY_MOVE_POOL(vlist_specs, vlist_spec_count, vlist_spec_cap);
    INLAY_MOVE_POOL(table_specs, table_spec_count, table_spec_cap);
    INLAY_MOVE_POOL(table_cols, table_col_count, table_col_cap);
    INLAY_MOVE_POOL(table_cells, table_cell_count, table_cell_cap);
    INLAY_MOVE_POOL(menu_specs, menu_spec_count, menu_spec_cap);
    INLAY_MOVE_POOL(menu_items, menu_item_count, menu_item_cap);
    INLAY_MOVE_POOL(avatar_specs, avatar_spec_count, avatar_spec_cap);
    INLAY_MOVE_POOL(toast_specs, toast_spec_count, toast_spec_cap);
    INLAY_MOVE_POOL(skeleton_specs, skeleton_spec_count, skeleton_spec_cap);
    INLAY_MOVE_POOL(dnd_specs, dnd_spec_count, dnd_spec_cap);
    INLAY_MOVE_POOL(input_rich_specs, input_rich_spec_count, input_rich_spec_cap);
    INLAY_MOVE_POOL(input_highlights, input_highlight_count, input_highlight_cap);
    INLAY_MOVE_POOL(editor_rich_specs, editor_rich_spec_count, editor_rich_spec_cap);
    INLAY_MOVE_POOL(editor_hints, editor_hint_count, editor_hint_cap);
    INLAY_MOVE_POOL(editor_inline_hls, editor_inline_hl_count, editor_inline_hl_cap);
    INLAY_MOVE_POOL(editor_markers, editor_marker_count, editor_marker_cap);
    INLAY_MOVE_POOL(editor_line_decos, editor_line_deco_count, editor_line_deco_cap);
    INLAY_MOVE_POOL(editor_folds, editor_fold_count, editor_fold_cap);
    INLAY_MOVE_POOL(editor_view_zones, editor_view_zone_count, editor_view_zone_cap);
    INLAY_MOVE_POOL(term_search_matches, term_search_match_count, term_search_match_cap);
    INLAY_MOVE_POOL(dock_space_specs, dock_space_spec_count, dock_space_spec_cap);
    INLAY_MOVE_POOL(dock_pane_specs, dock_pane_spec_count, dock_pane_spec_cap);
    INLAY_MOVE_POOL(sheet_specs, sheet_spec_count, sheet_spec_cap);
    INLAY_MOVE_POOL(hover_card_specs, hover_card_spec_count, hover_card_spec_cap);
    INLAY_MOVE_POOL(choice_items, choice_item_count, choice_item_cap);
    INLAY_MOVE_POOL(radio_group_specs, radio_group_spec_count, radio_group_spec_cap);
    INLAY_MOVE_POOL(toggle_group_specs, toggle_group_spec_count, toggle_group_spec_cap);
    INLAY_MOVE_POOL(color_picker_specs, color_picker_spec_count, color_picker_spec_cap);
    INLAY_MOVE_POOL(chart_specs, chart_spec_count, chart_spec_cap);
    INLAY_MOVE_POOL(chart_points, chart_point_count, chart_point_cap);
    INLAY_MOVE_POOL(portal_specs, portal_spec_count, portal_spec_cap);
    INLAY_MOVE_POOL(diff_specs, diff_spec_count, diff_spec_cap);
    INLAY_MOVE_POOL(diff_annotations, diff_annotation_count, diff_annotation_cap);
#undef INLAY_MOVE_POOL
}

void inlay_ui_trim_document_cache(InlaySurface* s) noexcept {
    const uint32_t limit = s->doc_spec_count > kRetainedDocs ? s->doc_spec_count : kRetainedDocs;
    for (uint32_t i = s->doc_cache_count; i > 0 && s->doc_cache_count > limit; --i) {
        auto& entry = s->doc_cache[i - 1u];
        if (entry.last_used_frame == s->frame_counter) continue;
        inlay_doc_destroy(entry.doc);
        entry = s->doc_cache[--s->doc_cache_count];
    }
}

void inlay_ui_free_widget_caches(InlaySurface* s) noexcept {
    if (!s) return;
    purge_model_selections(s);

    if (s->doc_cache) {
        for (uint32_t i = 0u; i < s->doc_cache_count; ++i) {
            if (s->doc_cache[i].doc) {
                inlay_doc_destroy(s->doc_cache[i].doc);
                s->doc_cache[i].doc = nullptr;
            }
        }
        SDL_free(s->doc_cache);
        s->doc_cache = nullptr;
    }
    s->doc_cache_count = 0u;
    s->doc_cache_cap = 0u;

    if (s->diff_cache) {
        for (uint32_t i = 0u; i < s->diff_cache_count; ++i) {
            free_diff_cache_entry(s->diff_cache[i]);
        }
        SDL_free(s->diff_cache);
        s->diff_cache = nullptr;
    }
    s->diff_cache_count = 0u;
    s->diff_cache_cap = 0u;

    SDL_free(s->doc_track_slots);   s->doc_track_slots = nullptr;   s->doc_track_slot_count = 0u;   s->doc_track_slot_cap = 0u;
    SDL_free(s->portal_sync_slots); s->portal_sync_slots = nullptr; s->portal_sync_slot_count = 0u; s->portal_sync_slot_cap = 0u;

    if (s->term_cache) {
        for (uint32_t i = 0u; i < s->term_cache_count; ++i) {
            SDL_free(s->term_cache[i].cells);
            s->term_cache[i].cells = nullptr;
        }
        SDL_free(s->term_cache);
        s->term_cache = nullptr;
    }
    s->term_cache_count = 0u;
    s->term_cache_cap = 0u;

    SDL_free(s->vlist_cache);
    s->vlist_cache = nullptr;
    s->vlist_cache_count = 0u;
    s->vlist_cache_cap = 0u;

    if (s->table_sort_cache) {
        for (uint32_t i = 0u; i < s->table_sort_cache_count; ++i) {
            SDL_free(s->table_sort_cache[i].row_order);
            s->table_sort_cache[i].row_order = nullptr;
        }
        SDL_free(s->table_sort_cache);
        s->table_sort_cache = nullptr;
    }
    s->table_sort_cache_count = 0u;
    s->table_sort_cache_cap = 0u;

    SDL_free(s->skeleton_cache);      s->skeleton_cache = nullptr;      s->skeleton_cache_count = 0u;      s->skeleton_cache_cap = 0u;
    SDL_free(s->dock_sync_cache);     s->dock_sync_cache = nullptr;     s->dock_sync_cache_count = 0u;     s->dock_sync_cache_cap = 0u;
    SDL_free(s->hover_card_states);   s->hover_card_states = nullptr;   s->hover_card_state_count = 0u;    s->hover_card_state_cap = 0u;
    SDL_free(s->toast_timers);        s->toast_timers = nullptr;        s->toast_timer_count = 0u;         s->toast_timer_cap = 0u;
    SDL_free(s->color_picker_states); s->color_picker_states = nullptr; s->color_picker_state_count = 0u;  s->color_picker_state_cap = 0u;
    SDL_free(s->editor_scroll_states);s->editor_scroll_states = nullptr;s->editor_scroll_state_count = 0u; s->editor_scroll_state_cap = 0u;
    SDL_free(s->drag_hover_states);   s->drag_hover_states = nullptr;   s->drag_hover_state_count = 0u;    s->drag_hover_state_cap = 0u;

    inlay_ui_free_spec_pools(s);
}

bool inlay_ui_parse_ext_widget(InlaySurface* s, JSValueConst props,
                               InlayUiKind kind, InlayUiNode& node, NodeStyle& st,
                               uint32_t path_id) {
    const bool has_props = JS_IsObject(props) && !JS_IsNull(props);
    switch (kind) {
        case INLAY_UI_DOC:
            if (!parse_doc_widget(s, props, has_props, node, st)) return false;
            s->doc_specs[st.ext_idx].path_id = path_id;
            s->doc_specs[st.ext_idx].cache_index = INLAY_UI_NONE;
            return true;
        case INLAY_UI_TERMINAL:     return parse_terminal_widget(s, props, has_props, node, st);
        case INLAY_UI_VIRTUAL_LIST: return parse_virtual_list_widget(s, props, has_props, st, path_id);
        case INLAY_UI_TABLE:        return parse_table_widget(s, props, has_props, st);
        case INLAY_UI_TREE:         return parse_tree_widget(s, props, has_props, st);
        case INLAY_UI_TREE_ITEM:    return parse_tree_item_widget(s, props, has_props, node, st);
        case INLAY_UI_MENU:
        case INLAY_UI_CONTEXT_MENU: return parse_menu_widget(s, props, has_props, st);
        case INLAY_UI_AVATAR:       return parse_avatar_widget(s, props, has_props, node, st);
        case INLAY_UI_TOAST:        return parse_toast_widget(s, props, has_props, st);
        case INLAY_UI_ACCORDION:    return parse_accordion_widget(s, props, has_props, node, st);
        case INLAY_UI_SKELETON:     return parse_skeleton_widget(s, props, has_props, st);
        case INLAY_UI_PORTAL:       return parse_portal_widget(s, props, has_props, node, st);
        case INLAY_UI_DOCK_SPACE:   return parse_dock_space_widget(s, props, has_props, st);
        case INLAY_UI_DOCK_PANE:    return parse_dock_pane_widget(s, props, has_props, node, st);
        case INLAY_UI_SHEET:        return parse_sheet_widget(s, props, has_props, st);
        case INLAY_UI_HOVER_CARD:   return parse_hover_card_widget(s, props, has_props, st);
        case INLAY_UI_RADIO_GROUP:  return parse_radio_group_widget(s, props, has_props, st);
        case INLAY_UI_TOGGLE_GROUP: return parse_toggle_group_widget(s, props, has_props, st);
        case INLAY_UI_COLOR_PICKER: return parse_color_picker_widget(s, props, has_props, st);
        case INLAY_UI_CHART:        return parse_chart_widget(s, props, has_props, st);
        case INLAY_UI_VIEW_ZONE:    return parse_view_zone_widget(s, props, has_props, st);
        case INLAY_UI_DIFF_VIEW:    return parse_diff_view_widget(s, props, has_props, st);
        default:                    return true;
    }
}

void inlay_ui_draw_ext_widget(InlaySurface* s, uint32_t index, uint32_t id, uint32_t parent_id,
                              const EtchTheme& theme, bool use_layer, WidgetEvents* events) {
    const InlayUiNode& node = s->nodes[index];
    const NodeStyle& st = s->styles[index];
    switch (node.kind) {
        case INLAY_UI_DOC:
            draw_doc_widget(s, node, st, id, parent_id, theme, use_layer, events);
            break;
        case INLAY_UI_TERMINAL:
            draw_terminal_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_VIRTUAL_LIST:
            draw_virtual_list_widget(s, node, st, id, theme, events);
            break;
        case INLAY_UI_TABLE:
            draw_table_widget(s, st, id, theme, events);
            break;
        case INLAY_UI_TREE:
            draw_tree_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_TREE_ITEM:
            draw_tree_item_widget(s, node, st, id, theme, events);
            break;
        case INLAY_UI_MENU:
            draw_menu_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_CONTEXT_MENU:
            draw_context_menu_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_AVATAR:
            draw_avatar_widget(s, index, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_TOAST:
            draw_toast_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_ACCORDION:
            draw_accordion_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_SKELETON:
            draw_skeleton_widget(s, st, id, theme, use_layer);
            break;
        case INLAY_UI_PORTAL:
            draw_portal_widget(s, node, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_DOCK_SPACE:
            draw_dock_space_widget(s, node, st, id, theme, events);
            break;
        case INLAY_UI_DOCK_PANE:
            break;
        case INLAY_UI_SHEET:
            draw_sheet_widget(s, node, st, id, theme, events);
            break;
        case INLAY_UI_HOVER_CARD:
            draw_hover_card_widget(s, node, st, id, theme, events);
            break;
        case INLAY_UI_RADIO_GROUP:
            draw_radio_group_widget(s, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_TOGGLE_GROUP:
            draw_toggle_group_widget(s, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_COLOR_PICKER:
            draw_color_picker_widget(s, st, id, theme, use_layer, events);
            break;
        case INLAY_UI_CHART:
            draw_chart_widget(s, st, id, theme, use_layer);
            break;
        case INLAY_UI_VIEW_ZONE:
            break;
        case INLAY_UI_DIFF_VIEW:
            draw_diff_view_widget(s, st, id, theme, use_layer, events);
            break;
        default:
            break;
    }
}

#endif // INLAY_ENABLE_SCRIPT
