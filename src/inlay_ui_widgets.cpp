#include "inlay_ui_internal.h"

#if defined(INLAY_ENABLE_SCRIPT) && INLAY_ENABLE_SCRIPT
#include <SDL3/SDL_keycode.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kDocCacheCap = 16u;
constexpr uint32_t kTermCacheCap = 8u;
constexpr uint32_t kVListCacheCap = 16u;
constexpr uint32_t kTableSortCacheCap = 8u;
constexpr uint32_t kSkeletonCacheCap = 64u;
constexpr uint32_t kTableCellHardCap = 262144u;
constexpr uint32_t kTableRowHardCap = 65536u;
constexpr uint32_t kTableColHardCap = 16384u;
constexpr uint32_t kMenuItemHardCap = 16384u;
constexpr uint32_t kMaxTreeGuideDepth = 16u;
constexpr uint32_t kMaxEventJsonBytes = 65536u;

// Per-draw-pass tree indent-guide inheritance stack (Tree -> nested TreeItems).
bool g_tree_guide_stack[kMaxTreeGuideDepth] = {};
uint32_t g_tree_guide_depth = 0u;

bool current_tree_guide() noexcept {
    return g_tree_guide_depth > 0u ? g_tree_guide_stack[g_tree_guide_depth - 1u] : true;
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

DocCacheEntry* find_or_add_doc_cache(InlaySurface* s, uint32_t id) noexcept {
    for (uint32_t i = 0u; i < s->doc_cache_count; ++i) {
        if (s->doc_cache[i].element_id == id) {
            s->doc_cache[i].last_used_frame = s->frame_counter;
            return &s->doc_cache[i];
        }
    }
    DocCacheEntry* entry = nullptr;
    if (s->doc_cache_count < kDocCacheCap) {
        if (!inlay_grow_pod(s->doc_cache, s->doc_cache_cap, s->doc_cache_count + 1u, 4u, kDocCacheCap))
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
    const InlayDocConfig cfg = inlay_doc_config_default();
    entry->element_id = id;
    entry->doc = inlay_doc_create(s->etch, &cfg);
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

    if (has_props) {
        const EtchStr md = inlay_ui_string_prop(s, props, "markdown");
        if (JS_HasException(s->js)) return false;
        if (md.length > 0u) {
            node.text = md;
        } else if (node.text.length == 0u) {
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

        inlay_ui_opt_number_prop(s, props, "blockPadding", &spec.block_padding_px, false, 0.0, 256.0);
        if (JS_HasException(s->js)) return false;
        inlay_ui_opt_number_prop(s, props, "blockGap", &spec.block_gap_px, false, 0.0, 256.0);
        if (JS_HasException(s->js)) return false;

        if (!inlay_ui_parse_event_prop(s, props, "onLinkClick", &spec.on_link_action, &spec.on_link_handler)) {
            inlay_ui_parse_event_prop(s, props, "onLink", &spec.on_link_action, &spec.on_link_handler);
        }
        if (JS_HasException(s->js)) return false;
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
    spec.line_height_px = 0.f;

    if (has_props) {
        const EtchStr ansi = inlay_ui_string_prop(s, props, "ansi");
        if (JS_HasException(s->js)) return false;
        if (ansi.length > 0u) {
            node.text = ansi;
        } else {
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
    const DocWidgetSpec& spec = s->doc_specs[st.ext_idx];
    DocCacheEntry* entry = find_or_add_doc_cache(s, id);
    if (!entry || !entry->doc) return;

    // Sync markdown content with streaming append fast-path when unchanged prefix matches.
    const uint64_t h = inlay_fnv1a_64(node.text.chars, node.text.length);
    if (h != entry->content_hash || node.text.length != entry->content_len) {
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
                inlay_doc_append_utf8(entry->doc, delta);
            } else {
                inlay_doc_set_utf8(entry->doc, node.text);
            }
        }
        entry->content_hash = h;
        entry->content_len = node.text.length;
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
    const InlayDocResult res = inlay_doc_view(entry->doc, &vd);
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
}

void draw_terminal_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                          uint32_t id, const EtchTheme& theme, bool use_layer) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->term_spec_count) return;
    const TermWidgetSpec& spec = s->term_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    TermCacheEntry* entry = find_or_add_term_cache(s, id);
    if (!entry) return;

    const uint32_t cols = spec.cols > 0u ? spec.cols : 80u;
    const uint32_t rows = spec.rows > 0u ? spec.rows : 24u;
    const uint64_t h = inlay_fnv1a_64(node.text.chars, node.text.length);
    if (!entry->cells || entry->content_hash != h || entry->content_len != node.text.length ||
        entry->cols != cols || entry->rows != rows) {
        parse_terminal_ansi(entry, node.text, cols, rows, h);
    }
    if (!entry->cells) return;

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
    body.scroll_x = (frame.width.kind != ETCH_SIZING_FIT);
    body.scroll_y = (frame.height.kind != ETCH_SIZING_FIT);
    etch_begin_box(&body);

    auto tg = etch_terminal_grid_desc_default();
    tg.id = etch_id_index(id, 0x7E4Au);
    tg.cells = entry->cells;
    tg.cell_count = size_t(entry->cols) * size_t(entry->rows);
    tg.columns = entry->cols;
    tg.rows = entry->rows;
    tg.first_row = 0u;
    tg.cell_width_px = cell_w;
    tg.line_height_px = line_h;
    tg.font_size_px = font_px;
    tg.font_id = font_id;
    tg.foreground = term_fg;
    tg.background = term_bg;
    tg.cursor_column = spec.has_cursor ? spec.cursor_col : -1;
    tg.cursor_row = spec.has_cursor ? spec.cursor_row : -1;
    tg.cursor_color = resolve_ref(spec.cursor_color, theme, theme.accent_primary);
    tg.cursor_motion = etch_spring_spec_snappy();
    etch_terminal_grid(&tg);

    etch_end_box();
    etch_end_box();
}

void draw_virtual_list_widget(InlaySurface* s, const InlayUiNode& node, const NodeStyle& st,
                              uint32_t id, const EtchTheme& theme, WidgetEvents* events) {
    if (st.ext_idx == INLAY_UI_NONE || st.ext_idx >= s->vlist_spec_count) return;
    const VirtualListSpec& spec = s->vlist_specs[st.ext_idx];
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);

    const bool data_mode = (st.flags2 & Nf2Selected) != 0u;
    const bool has_render_item = (st.flags2 & Nf2HasValue) != 0u;

    uint32_t child_count = 0u;
    if (!data_mode) {
        for (uint32_t at = node.first_child; at != INLAY_UI_NONE; at = s->nodes[at].next_sibling) {
            ++child_count;
        }
    }

    const uint32_t total_count = spec.total_count > 0u
        ? spec.total_count
        : (data_mode ? static_cast<uint32_t>(st.int_value > 0 ? st.int_value : 0) : child_count);

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

    const EtchVirtualListSlice slice = etch_begin_virtual_list(&vd);

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
        constexpr uint32_t kMaxWindow = 256u;
        uint32_t buf = spec.overscan_rows > 0u ? spec.overscan_rows * 2u : 16u;
        const uint32_t vis = slice.end_index - slice.first_index;
        if (vis >= kMaxWindow) {
            buf = 0u;
        } else if (buf * 2u > kMaxWindow - vis) {
            buf = (kMaxWindow - vis) / 2u;
        }
        const uint32_t want_first = (slice.first_index > buf) ? (slice.first_index - buf) : 0u;
        uint32_t want_end = (slice.end_index + buf < total_count) ? (slice.end_index + buf) : total_count;
        if (want_end - want_first > kMaxWindow) want_end = want_first + kMaxWindow;
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
    auto box = etch_box_desc_default();
    box.width = etch_sizing_grow(0.f, 0.f);
    box.height = etch_sizing_fit(0.f, 0.f);
    box.padding = etch_padding_all(4.f);
    box.gap = 1.f;
    inlay_ui_populate_box_desc(s, st, theme, id, box, use_layer);
    box.axis = ETCH_AXIS_COL;

    const bool show_guide = (st.int_value >= 0);
    const bool pushed = g_tree_guide_depth < kMaxTreeGuideDepth;
    if (pushed) g_tree_guide_stack[g_tree_guide_depth++] = show_guide;

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

    if (pushed) --g_tree_guide_depth;
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
    td.show_indent_guide = guide_explicit ? ((st.int_value & (1 << 1)) != 0) : current_tree_guide();
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
    if (!pres.visible) return;

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
}

void inlay_ui_free_widget_caches(InlaySurface* s) noexcept {
    if (!s) return;

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

    SDL_free(s->skeleton_cache);
    s->skeleton_cache = nullptr;
    s->skeleton_cache_count = 0u;
    s->skeleton_cache_cap = 0u;

    SDL_free(s->doc_specs);      s->doc_specs = nullptr;      s->doc_spec_count = 0u;      s->doc_spec_cap = 0u;
    SDL_free(s->term_specs);     s->term_specs = nullptr;     s->term_spec_count = 0u;     s->term_spec_cap = 0u;
    SDL_free(s->vlist_specs);    s->vlist_specs = nullptr;    s->vlist_spec_count = 0u;    s->vlist_spec_cap = 0u;
    SDL_free(s->table_specs);    s->table_specs = nullptr;    s->table_spec_count = 0u;    s->table_spec_cap = 0u;
    SDL_free(s->table_cols);     s->table_cols = nullptr;     s->table_col_count = 0u;     s->table_col_cap = 0u;
    SDL_free(s->table_cells);    s->table_cells = nullptr;    s->table_cell_count = 0u;    s->table_cell_cap = 0u;
    SDL_free(s->menu_specs);     s->menu_specs = nullptr;     s->menu_spec_count = 0u;     s->menu_spec_cap = 0u;
    SDL_free(s->menu_items);     s->menu_items = nullptr;     s->menu_item_count = 0u;     s->menu_item_cap = 0u;
    SDL_free(s->avatar_specs);   s->avatar_specs = nullptr;   s->avatar_spec_count = 0u;   s->avatar_spec_cap = 0u;
    SDL_free(s->toast_specs);    s->toast_specs = nullptr;    s->toast_spec_count = 0u;    s->toast_spec_cap = 0u;
    SDL_free(s->skeleton_specs); s->skeleton_specs = nullptr; s->skeleton_spec_count = 0u; s->skeleton_spec_cap = 0u;
}

bool inlay_ui_parse_ext_widget(InlaySurface* s, JSValueConst props,
                               InlayUiKind kind, InlayUiNode& node, NodeStyle& st,
                               uint32_t path_id) {
    const bool has_props = JS_IsObject(props) && !JS_IsNull(props);
    switch (kind) {
        case INLAY_UI_DOC:          return parse_doc_widget(s, props, has_props, node, st);
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
            draw_terminal_widget(s, node, st, id, theme, use_layer);
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
        default:
            break;
    }
}

#endif // INLAY_ENABLE_SCRIPT
