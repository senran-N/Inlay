#pragma once
#include "inlay_internal.h"
#include <SDL3/SDL_stdinc.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Zero-dependency line-preserving JSX -> __jsx(...) transpiler (src/inlay_ui_jsx.cpp).
// Fast-path: returns true with *out_len == 0 when no JSX syntax candidate is present
// (caller executes src directly with zero copy/allocation overhead).
bool inlay_jsx_transpile(const char* src, size_t src_len,
                         const char* filename,
                         char** io_buf, size_t* io_cap, size_t* out_len,
                         char* err_buf, size_t err_cap);

#if defined(INLAY_ENABLE_SCRIPT) && INLAY_ENABLE_SCRIPT
#include <SDL3/SDL_timer.h>
#include <quickjs.h>

inline constexpr uint32_t INLAY_UI_NONE = UINT32_MAX;
inline constexpr uint32_t INLAY_UI_INPUT_CAPACITY = 8192u;
inline constexpr float INLAY_UI_DEG_TO_RAD = 3.14159265358979323846f / 180.0f;

// ============================================================================
// Live Theme Tokens, ColorRef, Transitions & Hot/Cold Node Styling
// ============================================================================

enum ThemeToken : uint8_t {
    TokNone = 0, // Use literal ColorRef::rgba
    TokTransparent,
    TokBgCanvas,
    TokBgSurface,
    TokBgElevated,
    TokBgChrome,
    TokBgPopup,
    TokBgTooltip,
    TokBgInput,
    TokBgTable,
    TokBgNodeCanvas,
    TokSurfaceControl,
    TokSurfaceHover,
    TokSurfaceActive,
    TokSurfaceDisabled,
    TokSurfaceSubtle,
    TokSurfaceSubtleHover,
    TokSurfaceSubtleBody,
    TokSurfaceSelected,
    TokTextPrimary,
    TokTextSecondary,
    TokTextBody,
    TokTextCode,
    TokTextMuted,
    TokTextDisabled,
    TokAccent,
    TokAccentSecondary,
    TokAccentPrimary,
    TokAccentHover,
    TokFocusRing,
    TokDanger,
    TokWarning,
    TokSuccess,
    TokInfo,
    TokBorderSubtle,
    TokBorderLine,
    TokBorderInput,
    TokBorderNode,
    TokBorderControl,
    TokBorderTooltip,
    TokSelection,
    TokSelectionHover,
    TokZebraRow,
    TokActiveLine,
    TokBackdrop,
    TokShadow,
    TokOnPrimary,
    TokOnError,
    TokSyntaxComment,
    TokSyntaxString,
    TokSyntaxNumber,
    TokSyntaxKeyword
};

struct ColorRef {
    EtchColor rgba;    // Literal RGBA when token == TokNone
    uint8_t token;     // ThemeToken (0 = literal rgba)
    uint8_t alpha_mul; // 255 = 100% alpha; 0..254 scales resolved alpha (e.g. bg-accent/15 -> 38)
    bool set;
    uint8_t pad;
};
static_assert(sizeof(ColorRef) == 8);

inline ColorRef inlay_color_ref_rgba(EtchColor c, uint8_t alpha_mul = 255u) noexcept {
    return {c, uint8_t(TokNone), alpha_mul, true, 0u};
}

inline ColorRef inlay_color_ref_token(ThemeToken tok, uint8_t alpha_mul = 255u) noexcept {
    return {{}, uint8_t(tok), alpha_mul, true, 0u};
}

enum RadiusToken : uint8_t {
    RadPx = 0,
    RadPanel = 1,
    RadControl = 2,
    RadInput = 3,
    RadSmall = 4,
    RadPill = 5
};

enum TransitionPropBits : uint16_t {
    TransNone      = 0u,
    TransBg        = 1u << 0,
    TransColor     = 1u << 1,
    TransBorder    = 1u << 2,
    TransOpacity   = 1u << 3,
    TransTransform = 1u << 4,
    TransLayout    = 1u << 5,
    TransAll       = 0x3Fu
};

enum TransitionEasing : uint8_t {
    TransEaseOut         = 0,
    TransEaseLinear      = 1,
    TransEaseIn          = 2,
    TransEaseInOut       = 3,
    TransEaseCubicBezier = 4,
    TransEaseSpring      = 5 // bezier[0]=response, bezier[1]=damping_ratio
};

struct TransitionSpec {
    uint16_t prop_mask;
    uint8_t easing;
    float duration_sec;
    float delay_sec;
    float bezier[4];
};

// Offset/length slice inside InlaySurface::text pool (keeps NodeStyle compact & 4B-aligned).
struct TextSlice {
    uint32_t off;
    uint32_t len;
};

struct ShadowSpec {
    float x, y, blur, spread;
    ColorRef color;
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
    ColorRef stop_colors[8];
    float stop_pos[8];
};

struct TransformSpec {
    float tx, ty;
    float sx, sy;
    float rotation_rad;
};

enum NodeFlags : uint32_t {
    NfNone            = 0u,
    NfColumn          = 1u << 0,
    NfHasPadding      = 1u << 1,
    NfHasGap          = 1u << 2,
    NfHasSize         = 1u << 3,
    NfHasBorderWidth  = 1u << 4,
    NfHasAlignX       = 1u << 5,
    NfHasAlignY       = 1u << 6,
    NfHasJustify      = 1u << 7,
    NfHasAlignSelf    = 1u << 8,
    NfHasFlexGrow     = 1u << 9,
    NfHasFlexShrink   = 1u << 10,
    NfHasWrap         = 1u << 11,
    NfWrapFlag        = 1u << 12,
    NfHasOpacity      = 1u << 13,
    NfHasFontId       = 1u << 14,
    NfHasScrollX      = 1u << 15,
    NfScrollX         = 1u << 16,
    NfHasScrollY      = 1u << 17,
    NfScrollY         = 1u << 18,
    NfHasClipChildren = 1u << 19,
    NfClipChildren    = 1u << 20,
    NfDisabled        = 1u << 21,
    NfChecked         = 1u << 22,
    NfIndeterminate   = 1u << 23,
    NfOpen            = 1u << 24,
    NfEllipsis        = 1u << 25,
    NfShrinkWidth     = 1u << 26,
    NfShrinkHeight    = 1u << 27,
    NfDisplayMath     = 1u << 28,
    NfAnimateLayout   = 1u << 29,
    NfRadiusSet       = 1u << 30,
    NfClickable       = 1u << 31
};

enum NodeFlags2 : uint16_t {
    Nf2None          = 0u,
    Nf2HasValue      = 1u << 0,
    Nf2HasMin        = 1u << 1,
    Nf2HasMax        = 1u << 2,
    Nf2HasStep       = 1u << 3,
    Nf2HasSpacerSize = 1u << 4,
    Nf2HasSprite     = 1u << 5,
    Nf2HasIcon       = 1u << 6,
    Nf2WExplicit     = 1u << 7,
    Nf2HExplicit     = 1u << 8,
    Nf2Selected      = 1u << 9,
    Nf2Leaf          = 1u << 10,
    Nf2DefaultOpen   = 1u << 11,
    Nf2HasOpen       = 1u << 12
};

// Compact hot per-node style (128 B = 2 cache lines).
struct NodeStyle {
    float w_val, h_val;
    EtchPadding padding;
    float gap, font_size, border_width, opacity, radius_px;
    ColorRef bg, color, border_color;
    TextSlice action;
    uint32_t handler;
    uint32_t cold_idx;   // Index into InlaySurface::cold_visuals or INLAY_UI_NONE
    uint32_t ext_idx;    // Index into kind-specific cold widget spec pool or INLAY_UI_NONE
    int32_t int_value;   // Active tab / combobox / tree depth / generic integer state
    float value;         // Progress/slider value, split ratio, or spacer size
    float flex_grow;
    float flex_shrink;
    uint32_t flags;      // NodeFlags
    uint16_t flags2;     // NodeFlags2
    uint16_t font_id;
    uint16_t icon_id;
    uint16_t max_lines;
    uint8_t w_kind;      // EtchSizingKind
    uint8_t h_kind;      // EtchSizingKind
    uint8_t radius_tok;  // RadiusToken
    uint8_t align_x;     // EtchAlignX
    uint8_t align_y;     // EtchAlignY
    uint8_t justify;     // EtchJustify
    uint8_t align_self;  // EtchAlignSelf
    uint8_t button_variant; // EtchButtonVariant
    uint8_t image_fit;   // EtchImageFit
    uint8_t item_count;
    uint8_t pad_[2];
};
static_assert(sizeof(NodeStyle) == 128);

enum ColdFlags : uint32_t {
    CfNone                 = 0u,
    CfHasMargin            = 1u << 0,
    CfHasBorderSides       = 1u << 1,
    CfHasRowGap            = 1u << 2,
    CfHasAspectRatio       = 1u << 3,
    CfHasLineHeight        = 1u << 4,
    CfHasLetterSpacing     = 1u << 5,
    CfHasElevation         = 1u << 6,
    CfInsetShadowBool      = 1u << 7,
    CfPerCornerRadius      = 1u << 8,
    CfHasContinuousCorners = 1u << 9,
    CfContinuousCorners    = 1u << 10,
    CfHasBackdropSat       = 1u << 11,
    CfHasBackdropBright    = 1u << 12,
    CfHasTopRimAlpha       = 1u << 13,
    CfHasHoverScale        = 1u << 14,
    CfHasPressScale        = 1u << 15,
    CfHasHoverOpacity      = 1u << 16,
    CfHasTransform         = 1u << 17,
    CfHasAttachTo          = 1u << 18,
    CfHasAnchorElement     = 1u << 19,
    CfHasAnchorParent      = 1u << 20,
    CfHasFloatingOffset    = 1u << 21,
    CfHasZIndex            = 1u << 22,
    CfPointerPassthrough   = 1u << 23,
    CfHasClampToViewport   = 1u << 24,
    CfClampToViewport      = 1u << 25,
    CfFlipOnOverflow       = 1u << 26,
    CfHasBlendMode         = 1u << 27,
    CfHasAlphaMask         = 1u << 28,
    CfHasTransition        = 1u << 29,
    CfHasOpenType          = 1u << 30,
    CfHasCursor            = 1u << 31
};

// Cold visual/layout/compositing properties allocated lazily only when a node uses them.
struct ColdVisualStyle {
    uint32_t cflags;
    float w_min, w_max, h_min, h_max;
    EtchPadding margin;
    EtchPadding border_sides;
    float row_gap;
    float aspect_ratio;
    float line_height;
    float letter_spacing;
    uint16_t grid_cols;
    uint16_t grid_rows;
    uint16_t col_span;
    uint16_t row_span;
    int16_t z_index;
    EtchVec2 floating_offset;
    EtchAttachTo attach_to;
    EtchAnchor anchor_element;
    EtchAnchor anchor_parent;
    EtchCursor cursor;
    // Per-corner radii, shadows, gradients
    EtchCorners radius_vals;
    uint8_t radius_tok[4];
    EtchElevation elevation;
    uint32_t shadow_offset;
    uint8_t shadow_count;
    uint32_t gradient_idx;
    // Optical vibrancy & rim highlight
    float backdrop_blur;
    float backdrop_saturation;
    float backdrop_brightness;
    float backdrop_noise;
    float top_rim_alpha;
    // Hover / press / focus / dark-mode state overrides
    ColorRef hover_bg;
    ColorRef hover_border_color;
    ColorRef hover_text_color;
    ColorRef press_bg;
    ColorRef press_border_color;
    ColorRef focus_border_color;
    ColorRef focus_ring_color;
    ColorRef dark_bg;
    ColorRef dark_border_color;
    ColorRef dark_text_color;
    float hover_scale;
    float press_scale;
    float hover_opacity;
    // 2D affine transform
    TransformSpec transform;
    // Etch 0.28 compositing, motion & OpenType features
    EtchBlendMode blend_mode;
    EtchAlphaMaskDesc alpha_mask;
    TransitionSpec transition;
    uint32_t opentype_flags;
    // Secondary widget properties
    TextSlice placeholder;
    TextSlice src;
    ColorRef track_color;
    float min_val, max_val, step;
    uint32_t sprite_id;
    uint32_t item_offset;
    TextSlice on_hover_action;
    uint32_t on_hover_handler;
    TextSlice on_focus_action;
    uint32_t on_focus_handler;
};

// ============================================================================
// Cold Extended Widget Specs (Per-Rebuild Pools) & Persistent Caches
// ============================================================================

struct DocWidgetSpec {
    bool show_line_numbers;
    bool hide_copy_button;
    bool stick_to_bottom;
    bool auto_height;
    float block_padding_px;
    float block_gap_px;
    TextSlice on_link_action;
    uint32_t on_link_handler;
};

struct TermWidgetSpec {
    uint32_t cols;
    uint32_t rows;
    int32_t cursor_col;
    int32_t cursor_row;
    bool has_cursor;
    float line_height_px;
    ColorRef cursor_color;
};

struct VirtualListSpec {
    uint32_t path_id;
    uint32_t total_count;
    uint32_t window_start;
    float item_height_px;
    float row_gap_px;
    uint32_t overscan_rows;
    float scroll_fade_px;
    bool stick_to_bottom;
    bool show_scrollbar;
    TextSlice on_range_action;
    uint32_t on_range_handler;
};

struct TableColSpec {
    TextSlice title;
    TextSlice key;
    float width_px;
    float min_width_px;
    float max_width_px;
    uint8_t align_x; // EtchAlignX
    uint8_t sizing;  // EtchTableColumnSizing
    bool resizable;
    bool sortable;
};

struct TableSpec {
    uint32_t col_offset;
    uint32_t col_count;
    uint32_t cell_offset; // Row-major TextSlice array in InlaySurface::table_cells (row_count * col_count)
    uint32_t row_count;
    float header_height_px;
    float row_height_px;
    float cell_padding_x;
    int32_t selected_row;
    int32_t sort_col;
    bool sort_desc;
    bool has_sort;
    bool zebra;
    bool show_column_lines;
    bool horizontal_scroll;
    ColorRef header_bg;
    ColorRef zebra_bg;
    ColorRef hover_row_bg;
    ColorRef selected_row_bg;
    TextSlice on_sort_action;
    uint32_t on_sort_handler;
};

struct MenuItemSpec {
    TextSlice label;
    TextSlice shortcut;
    TextSlice value;
    TextSlice action;
    uint32_t handler;
    uint16_t icon_id;
    bool has_icon;
    bool checked;
    bool disabled;
    bool separator;
    bool danger;
};

struct MenuSpec {
    uint32_t item_offset;
    uint32_t item_count;
    uint32_t target_id; // For INLAY_UI_CONTEXT_MENU (0 = wrapped child/parent)
    TextSlice label;
    bool open;
    bool has_open;
    TextSlice on_close_action;
    uint32_t on_close_handler;
    TextSlice on_open_change_action;
    uint32_t on_open_change_handler;
};

enum AvatarStatus : uint8_t {
    AvatarStatusNone    = 0,
    AvatarStatusOnline  = 1,
    AvatarStatusBusy    = 2,
    AvatarStatusAway    = 3,
    AvatarStatusOffline = 4
};

struct AvatarSpec {
    TextSlice initials;
    TextSlice src;
    uint32_t sprite_id;
    float size_px;
    uint8_t status; // AvatarStatus
    ColorRef status_color;
    ColorRef ring_color;
    bool has_sprite;
    bool has_ring;
};

enum ToastVariant : uint8_t {
    ToastInfo    = 0,
    ToastSuccess = 1,
    ToastWarning = 2,
    ToastDanger  = 3
};

enum ToastPosition : uint8_t {
    ToastBottomRight  = 0,
    ToastBottomLeft   = 1,
    ToastBottomCenter = 2,
    ToastTopRight     = 3,
    ToastTopLeft      = 4,
    ToastTopCenter    = 5
};

struct ToastSpec {
    TextSlice title;
    TextSlice description;
    TextSlice action_label;
    uint8_t variant;  // ToastVariant
    uint8_t position; // ToastPosition
    bool open;
    bool dismissible;
    TextSlice action_name;
    uint32_t action_handler;
    TextSlice dismiss_action;
    uint32_t dismiss_handler;
};

enum SkeletonShape : uint8_t {
    SkeletonRect   = 0,
    SkeletonCircle = 1,
    SkeletonText   = 2
};

struct SkeletonSpec {
    uint8_t shape; // SkeletonShape
    uint8_t lines; // Number of text bars for SkeletonText
    float line_gap_px;
    bool animated;
    ColorRef shimmer_color;
};

// Persistent caches surviving across JS rebuilds (bounded LRU; freed on surface destroy).
struct DocCacheEntry {
    uint32_t element_id;
    InlayDoc* doc;
    uint64_t content_hash;
    uint32_t content_len;
    uint64_t last_used_frame;
};

struct TermCacheEntry {
    uint32_t element_id;
    EtchTerminalCell* cells;
    uint32_t cell_cap;
    uint32_t cols;
    uint32_t rows;
    int32_t cursor_col;
    int32_t cursor_row;
    uint64_t content_hash;
    uint32_t content_len;
    uint64_t last_used_frame;
};

struct VirtualListWinEntry {
    uint32_t element_id;
    uint32_t first_index;
    uint32_t end_index;
    uint32_t total_count;
    float item_height_px;
    double scroll_y;
    uint64_t last_used_frame;
};

struct TableSortCache {
    uint32_t element_id;
    uint32_t* row_order;
    uint32_t row_count;
    uint32_t row_cap;
    uint32_t sort_col;
    bool sort_desc;
    bool controlled_applied;
    int32_t last_controlled_col;
    bool last_controlled_desc;
    uint64_t data_hash;
    uint64_t last_used_frame;
};

struct SkeletonPulseSlot {
    uint32_t element_id;
    float target;
    bool initialized;
    bool reduced;
    uint64_t last_used_frame;
};

struct ColorTweenSlot {
    uint32_t id_prop_hash;
    EtchColor start_col;
    EtchColor target_col;
    EtchColor current_col;
    double start_ms;
    float duration_sec;
    float delay_sec;
    uint8_t easing;
    float bezier[4];
    uint64_t last_frame;
    bool active;
};

struct ClsCacheEntry {
    uint64_t hash;
    uint32_t str_off;
    uint32_t last_hit;
    uint16_t str_len;
    uint8_t key_flags; // bit0=init_col, bit1=default_col_kind, bit2=has_cold, bit7=valid
    uint8_t pad_;
    NodeStyle style;
    ColdVisualStyle cold;
};

// ============================================================================
// Runtime Surface Struct & Frame Event Queue
// ============================================================================

struct ActionBinding {
    char name[64];
    InlayActionFn callback;
    void* user;
};

struct SlotBinding {
    char name[64];
    InlaySlotRenderFn callback;
    void* user;
};

struct TimerEntry {
    JSValue function;
    double due;
    uint32_t id;
};

struct WidgetEvent {
    WidgetEvent* next;
    EtchStr name;
    EtchStr payload;
    uint32_t handler;
    bool is_click;
};

struct WidgetEvents {
    WidgetEvent* first;
    WidgetEvent* last;
};

struct InlaySurface {
    EtchContext* etch;
    InlaySurfaceDesc desc;
    JSRuntime* runtime;
    JSContext* js;
    InlayUiNode* nodes;
    NodeStyle* styles;
    uint32_t node_count, node_capacity;
    char* text;
    uint32_t text_used;
    // Per-rebuild auxiliary visual & item pools (allocated via inlay_grow_pod / SDL_realloc)
    ColdVisualStyle* cold_visuals;
    uint32_t cold_visual_count, cold_visual_cap;
    ShadowSpec* shadow_pool;
    uint32_t shadow_pool_count, shadow_pool_cap;
    GradientSpec* gradient_pool;
    uint32_t gradient_pool_count, gradient_pool_cap;
    EtchStr* item_pool;
    uint32_t item_pool_count, item_pool_cap;
    // Per-rebuild extended widget spec pools
    DocWidgetSpec* doc_specs;
    uint32_t doc_spec_count, doc_spec_cap;
    TermWidgetSpec* term_specs;
    uint32_t term_spec_count, term_spec_cap;
    VirtualListSpec* vlist_specs;
    uint32_t vlist_spec_count, vlist_spec_cap;
    TableSpec* table_specs;
    uint32_t table_spec_count, table_spec_cap;
    TableColSpec* table_cols;
    uint32_t table_col_count, table_col_cap;
    TextSlice* table_cells;
    uint32_t table_cell_count, table_cell_cap;
    MenuSpec* menu_specs;
    uint32_t menu_spec_count, menu_spec_cap;
    MenuItemSpec* menu_items;
    uint32_t menu_item_count, menu_item_cap;
    AvatarSpec* avatar_specs;
    uint32_t avatar_spec_count, avatar_spec_cap;
    ToastSpec* toast_specs;
    uint32_t toast_spec_count, toast_spec_cap;
    SkeletonSpec* skeleton_specs;
    uint32_t skeleton_spec_count, skeleton_spec_cap;
    // Persistent widget & style caches
    DocCacheEntry* doc_cache;
    uint32_t doc_cache_count, doc_cache_cap;
    TermCacheEntry* term_cache;
    uint32_t term_cache_count, term_cache_cap;
    VirtualListWinEntry* vlist_cache;
    uint32_t vlist_cache_count, vlist_cache_cap;
    TableSortCache* table_sort_cache;
    uint32_t table_sort_cache_count, table_sort_cache_cap;
    SkeletonPulseSlot* skeleton_cache;
    uint32_t skeleton_cache_count, skeleton_cache_cap;
    ColorTweenSlot* color_tweens;
    uint32_t color_tween_count, color_tween_cap;
    // Persistent 2-way parsed `cls` utility cache (theme-independent ColorRef tokens)
    ClsCacheEntry* cls_cache;
    char* cls_cache_strs;
    uint32_t cls_cache_str_used;
    uint32_t cls_cache_clock;
    // Reusable JSX transpilation scratch buffer (owned via SDL_realloc / SDL_free)
    char* jsx_buf;
    size_t jsx_cap;
    // State, source, edit scratch
    char* state;
    char* json_scratch;
    char edit_buffer[INLAY_UI_INPUT_CAPACITY];
    char* source;
    uint32_t source_capacity, state_size, state_capacity;
    char error[1024], path[1024];
    ActionBinding actions[32];
    uint32_t action_count;
    SlotBinding slots[32];
    uint32_t slot_count;
    JSValue* handlers;
    uint32_t handler_count, handler_capacity;
    TimerEntry timers[32];
    uint32_t next_timer;
    int64_t modified;
    uint64_t file_size, deadline, frame_counter;
    double frequency;
    bool running, drawing, draw_parent_col, timed_out, pending_state, state_dirty, loaded, hmr_file_error;
    JSValue rejections[32];
    uint32_t rejection_count;
    InlaySurfaceStats stats;
};

// ============================================================================
// Inline Shared Utilities
// ============================================================================

inline bool inlay_ui_str_eq(const char* chars, size_t len, const char* lit) noexcept {
    const size_t n = std::strlen(lit);
    return len == n && (n == 0 || (chars && !std::memcmp(chars, lit, n)));
}

inline bool inlay_ui_equal(EtchStr value, const char* lit) noexcept {
    return inlay_ui_str_eq(value.chars, value.length, lit);
}

inline EtchStr inlay_ui_slice(const InlaySurface* s, TextSlice sl) noexcept {
    return sl.len ? EtchStr{s->text + sl.off, sl.len, false} : EtchStr{};
}

inline EtchStr inlay_ui_frame_text(InlaySurface* s, EtchStr text) noexcept {
    return text.length ? etch_frame_arena_push_str(s->etch, text.chars, text.length) : EtchStr{};
}

inline double inlay_ui_now_ms(const InlaySurface* s) noexcept {
    return double(SDL_GetPerformanceCounter()) * 1000.0 / s->frequency;
}

inline void inlay_ui_init_node_style(NodeStyle& st) noexcept {
    st = {};
    st.font_size = 15.f;
    st.opacity = 1.f;
    st.handler = INLAY_UI_NONE;
    st.cold_idx = INLAY_UI_NONE;
    st.ext_idx = INLAY_UI_NONE;
    st.flex_shrink = 1.f;
}

template <typename T>
inline uint32_t inlay_ui_alloc_pool(InlaySurface* s, T*& pool, uint32_t& count, uint32_t& cap,
                                    uint32_t need, uint32_t hard_cap) {
    if (need == 0u) return 0u;
    if (need > hard_cap || count > hard_cap - need) {
        JS_ThrowInternalError(s->js, "UI auxiliary pool capacity exceeded");
        return INLAY_UI_NONE;
    }
    if (!inlay_grow_pod(pool, cap, count + need, 32u, hard_cap)) {
        JS_ThrowInternalError(s->js, "UI auxiliary pool allocation failed");
        return INLAY_UI_NONE;
    }
    const uint32_t offset = count;
    count += need;
    return offset;
}

inline const ColdVisualStyle* inlay_ui_get_cold(const InlaySurface* s, const NodeStyle& st) noexcept {
    return (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count)
        ? &s->cold_visuals[st.cold_idx]
        : nullptr;
}

inline ColorRef inlay_ui_eff_bg(const NodeStyle& st, const ColdVisualStyle* cold, bool dark) noexcept {
    return (dark && cold && cold->dark_bg.set) ? cold->dark_bg : st.bg;
}

inline ColorRef inlay_ui_eff_border(const NodeStyle& st, const ColdVisualStyle* cold, bool dark) noexcept {
    return (dark && cold && cold->dark_border_color.set) ? cold->dark_border_color : st.border_color;
}

inline ColorRef inlay_ui_eff_text(const NodeStyle& st, const ColdVisualStyle* cold, bool dark) noexcept {
    return (dark && cold && cold->dark_text_color.set) ? cold->dark_text_color : st.color;
}

inline ColdVisualStyle* inlay_ui_ensure_cold(InlaySurface* s, NodeStyle& st) {
    if (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count)
        return &s->cold_visuals[st.cold_idx];
    const uint32_t idx = inlay_ui_alloc_pool(s, s->cold_visuals, s->cold_visual_count,
                                             s->cold_visual_cap, 1u, s->desc.max_nodes);
    if (idx == INLAY_UI_NONE) return nullptr;
    auto& c = s->cold_visuals[idx];
    c = {};
    c.col_span = 1u;
    c.row_span = 1u;
    c.gradient_idx = INLAY_UI_NONE;
    c.backdrop_saturation = 1.f;
    c.backdrop_brightness = 1.f;
    c.hover_scale = 1.f;
    c.press_scale = 1.f;
    c.hover_opacity = 1.f;
    c.transform = {0.f, 0.f, 1.f, 1.f, 0.f};
    c.on_hover_handler = INLAY_UI_NONE;
    c.on_focus_handler = INLAY_UI_NONE;
    st.cold_idx = idx;
    return &c;
}

// ============================================================================
// Cross-Module Function Contracts
// ============================================================================

// Core VM / JS property & layout helpers (src/inlay_ui.cpp)
TextSlice inlay_ui_copy_text_slice(InlaySurface* s, JSValueConst value);
EtchStr inlay_ui_copy_text(InlaySurface* s, JSValueConst value);
TextSlice inlay_ui_string_slice_prop(InlaySurface* s, JSValueConst obj, const char* name);
EtchStr inlay_ui_string_prop(InlaySurface* s, JSValueConst obj, const char* name);
bool inlay_ui_opt_number_prop(InlaySurface* s, JSValueConst obj, const char* name, float* out,
                              bool unbounded = false, double min_v = -1.0, double max_v = 16384.0);
bool inlay_ui_bool_prop(InlaySurface* s, JSValueConst obj, const char* name);
bool inlay_ui_opt_bool_prop(InlaySurface* s, JSValueConst obj, const char* name, bool* out);
bool inlay_ui_opt_uint_prop(InlaySurface* s, JSValueConst obj, const char* name,
                            uint32_t max_value, uint32_t* out);
bool inlay_ui_opt_int_prop(InlaySurface* s, JSValueConst obj, const char* name,
                           int32_t min_value, int32_t max_value, int32_t* out);
uint32_t inlay_ui_register_handler(InlaySurface* s, JSValueConst fn);
bool inlay_ui_parse_event_prop(InlaySurface* s, JSValueConst props, const char* name,
                               TextSlice* out_action, uint32_t* out_handler);
bool inlay_ui_check_watchdog(InlaySurface* s) noexcept;
void inlay_ui_queue_event(InlaySurface* s, WidgetEvents* events, EtchStr name, uint32_t handler,
                          EtchStr payload, bool is_click = false);
void inlay_ui_dispatch_box_interaction(InlaySurface* s, uint32_t index, uint32_t id, WidgetEvents* events);
EtchSizing inlay_ui_resolve_sizing(const InlaySurface* s, const NodeStyle& st, const ColdVisualStyle* cold,
                                   bool vertical, EtchSizing fallback) noexcept;
EtchCorners inlay_ui_resolve_corners(const NodeStyle& st, const ColdVisualStyle* cold,
                                     const EtchTheme& theme, float fallback) noexcept;
void inlay_ui_populate_box_desc(InlaySurface* s, const NodeStyle& st, const EtchTheme& theme,
                                uint32_t id, EtchBoxDesc& box, bool use_layer) noexcept;
void inlay_ui_draw_node(InlaySurface* s, uint32_t index, uint32_t parent,
                        const EtchTheme& theme, WidgetEvents* events);

// Style, utility classes (`cls`/`className`), tokens & transitions (src/inlay_ui_style.cpp)
ColorRef inlay_ui_parse_color(const char* chars, size_t len) noexcept;
EtchColor inlay_ui_resolve_color(ColorRef ref, const EtchTheme& theme, EtchColor fallback) noexcept;
bool inlay_ui_parse_transition(const char* chars, size_t len, TransitionSpec* out) noexcept;
bool inlay_ui_parse_blend_mode(const char* chars, size_t len, EtchBlendMode* out) noexcept;
bool inlay_ui_parse_opentype_flags(const char* chars, size_t len, uint32_t* io_flags) noexcept;
bool inlay_ui_parse_alpha_mask(InlaySurface* s, JSValueConst val, EtchAlphaMaskDesc* out);
void inlay_ui_apply_cls(InlaySurface* s, const char* cls, size_t cls_len,
                        InlayUiKind kind, NodeStyle& st);
EtchColor inlay_ui_tween_color(InlaySurface* s, uint32_t element_id, uint16_t prop_bit,
                               EtchColor target, const TransitionSpec& trans) noexcept;
float inlay_ui_tween_scalar(InlaySurface* s, uint32_t element_id, uint16_t prop_bit,
                            float target, const TransitionSpec& trans) noexcept;

// Extended native & composite widgets (src/inlay_ui_widgets.cpp)
bool inlay_ui_is_ext_widget(InlayUiKind kind) noexcept;
bool inlay_ui_parse_ext_widget(InlaySurface* s, JSValueConst props,
                               InlayUiKind kind, InlayUiNode& node, NodeStyle& st,
                               uint32_t path_id);
void inlay_ui_draw_ext_widget(InlaySurface* s, uint32_t index, uint32_t id, uint32_t parent_id,
                              const EtchTheme& theme, bool use_layer, WidgetEvents* events);
void inlay_ui_reset_widget_specs(InlaySurface* s) noexcept;
void inlay_ui_free_widget_caches(InlaySurface* s) noexcept;

#endif // INLAY_ENABLE_SCRIPT
