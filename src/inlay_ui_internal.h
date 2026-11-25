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
    TextSlice on_key_action;
    uint32_t on_key_handler;
    bool global_keys;
    bool consume_keys;
    TextSlice on_text_action;
    uint32_t on_text_handler;
    uint32_t dnd_idx; // Index into InlaySurface::dnd_specs or INLAY_UI_NONE
};

// ============================================================================
// Cold Extended Widget Specs (Per-Rebuild Pools) & Persistent Caches
// ============================================================================

struct NodeDndSpec {
    bool draggable;
    TextSlice drag_kind;
    TextSlice drag_label;
    TextSlice drag_data;
    TextSlice drop_accept;
    TextSlice on_drop_action;
    uint32_t on_drop_handler;
    TextSlice on_file_drop_action;
    uint32_t on_file_drop_handler;
    TextSlice on_drag_hover_action;
    uint32_t on_drag_hover_handler;
};

struct InputHighlightItem {
    uint32_t offset;
    uint32_t length;
    ColorRef color;
    ColorRef bg;
    ColorRef border_color;
    float radius;
    bool atomic;
};

struct InputRichSpec {
    uint32_t hl_offset;
    uint32_t hl_count;
    TextSlice ghost_text;
    ColorRef ghost_color;
    bool accept_ghost_on_tab;
    TextSlice on_accept_ghost_action;
    uint32_t on_accept_ghost_handler;
    TextSlice on_paste_image_action;
    uint32_t on_paste_image_handler;
    TextSlice on_caret_action;
    uint32_t on_caret_handler;
};

struct EditorInlayHintItem {
    uint32_t line;
    uint32_t col;
    TextSlice text;
    ColorRef color;
    ColorRef bg;
    ColorRef border_color;
    float radius;
};

struct EditorInlineHlItem {
    uint32_t line;
    uint32_t start_col;
    uint32_t end_col;
    ColorRef bg;
    ColorRef border_color;
    float radius;
};

struct EditorOverviewItem {
    uint32_t line;
    ColorRef color;
    uint8_t lane;
};

struct EditorLineDecoItem {
    uint32_t line;
    ColorRef row_bg;
    ColorRef gutter_bar;
    ColorRef gutter_tag_color;
    TextSlice gutter_tag;
};

struct EditorFoldItem {
    uint32_t start;
    uint32_t end;
    bool collapsed;
};

struct EditorViewZoneItem {
    uint32_t id;
    uint32_t after_line;
    float height;
    bool above_line;
    ColorRef fill_color;
};

struct CodeEditorRichSpec {
    bool auto_height;
    bool show_overview_ruler;
    bool has_show_line_numbers;
    bool show_line_numbers;
    bool has_scroll_x;
    bool has_scroll_y;
    float scroll_x;
    float scroll_y;
    float gutter_tag_width;
    TextSlice sticky_header;
    uint32_t hint_off, hint_cnt;
    uint32_t hl_off, hl_cnt;
    uint32_t marker_off, marker_cnt;
    uint32_t deco_off, deco_cnt;
    uint32_t fold_off, fold_cnt;
    uint32_t vz_off, vz_cnt;
    TextSlice on_scroll_action;
    uint32_t on_scroll_handler;
    TextSlice on_gutter_click_action;
    uint32_t on_gutter_click_handler;
    TextSlice on_gutter_range_action;
    uint32_t on_gutter_range_handler;
    TextSlice language;
    bool has_highlight_brackets;
    bool highlight_brackets;
    bool has_indent_guides;
    bool indent_guides;
    bool has_diag_tooltips;
    bool diag_tooltips;
    bool show_minimap;
    TextSlice on_cursor_action;
    uint32_t on_cursor_handler;
};

struct DocWidgetSpec {
    uint64_t content_hash;
    uint32_t path_id, cache_index;
    TextSlice document_name;
    bool show_line_numbers;
    bool hide_copy_button;
    bool stick_to_bottom;
    bool auto_height;
    bool streaming_repair;
    float block_padding_px;
    float block_gap_px;
    TextSlice on_link_action;
    uint32_t on_link_handler;
    TextSlice find_query;
    TextSlice on_sel_action;
    uint32_t on_sel_handler;
    TextSlice on_find_action;
    uint32_t on_find_handler;
    TextSlice on_task_toggle_action;
    uint32_t on_task_toggle_handler;
    TextSlice on_diagram_node_action;
    uint32_t on_diagram_node_handler;
};

struct TermSearchMatchItem {
    uint32_t row;
    uint32_t start_col;
    uint32_t end_col;
    bool active;
};

struct TermWidgetSpec {
    uint32_t cols;
    uint32_t rows;
    int32_t cursor_col;
    int32_t cursor_row;
    bool has_cursor;
    bool auto_fit;
    bool focusable;
    bool interactive_selection;
    bool has_cursor_shape;
    uint8_t cursor_shape;
    bool cursor_visible;
    bool has_cursor_visible;
    float line_height_px;
    ColorRef cursor_color;
    ColorRef search_color;
    ColorRef search_active_color;
    uint32_t search_off;
    uint32_t search_cnt;
    TextSlice buffer_name;
    TextSlice on_cell_click_action;
    uint32_t on_cell_click_handler;
    TextSlice on_link_click_action;
    uint32_t on_link_click_handler;
    TextSlice on_resize_action;
    uint32_t on_resize_handler;
    TextSlice on_sel_action;
    uint32_t on_sel_handler;
    TextSlice on_wheel_action;
    uint32_t on_wheel_handler;
    bool capture_input;
    bool bracketed_paste;
    bool mouse_reporting;
    bool app_cursor;
    bool has_palette;
    EtchColor palette[16];
    TextSlice on_input_action;
    uint32_t on_input_handler;
    TextSlice on_paste_action;
    uint32_t on_paste_handler;
    TextSlice on_mouse_action;
    uint32_t on_mouse_handler;
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
    uint32_t stack_index;
    uint32_t duration_ms;
    TextSlice action_name;
    uint32_t action_handler;
    TextSlice dismiss_action;
    uint32_t dismiss_handler;
};

struct DockSpaceSpec {
    float tab_bar_height;
    float divider_size;
    float default_split_ratio;
    bool default_split_vertical;
    bool has_default_split;
    TextSlice on_active_action;
    uint32_t on_active_handler;
    TextSlice layout_json;
    TextSlice on_layout_action;
    uint32_t on_layout_handler;
    TextSlice on_tear_off_action;
    uint32_t on_tear_off_handler;
};

struct DockPaneSpec {
    uint32_t pane_id;
    TextSlice title;
    uint16_t icon_id;
    bool has_icon;
    bool closable;
    bool pinned;
    bool dirty;
    bool split_second;
    ColorRef badge_color;
    TextSlice on_close_action;
    uint32_t on_close_handler;
    TextSlice on_rclick_action;
    uint32_t on_rclick_handler;
};

struct SheetSpec {
    bool open;
    bool dismiss_on_backdrop;
    bool dismiss_on_escape;
    bool show_handle;
    uint8_t edge; // EtchSheetEdge
    float extent_px;
    TextSlice on_close_action;
    uint32_t on_close_handler;
};

struct HoverCardSpec {
    float open_delay_ms;
    float close_delay_ms;
    float width_px;
};

struct ChoiceItemSpec {
    TextSlice label;
    TextSlice value;
    TextSlice description;
    uint16_t icon_id;
    bool has_icon;
    bool disabled;
};

struct RadioGroupSpec {
    uint32_t item_off;
    uint32_t item_cnt;
    TextSlice value;
    bool horizontal;
    TextSlice on_change_action;
    uint32_t on_change_handler;
};

struct ToggleGroupSpec {
    uint32_t item_off;
    uint32_t item_cnt;
    TextSlice value;
    uint32_t selected_mask;
    bool multiple;
    bool outline;
    TextSlice on_change_action;
    uint32_t on_change_handler;
};

struct ColorPickerSpec {
    EtchColor value;
    bool has_value;
    EtchColor presets[16];
    uint8_t preset_count;
    TextSlice on_change_action;
    uint32_t on_change_handler;
};

struct ChartPointSpec {
    float value;
    TextSlice label;
};

struct ChartSpec {
    uint8_t variant; // 0=bar, 1=area, 2=line
    uint32_t pt_off;
    uint32_t pt_cnt;
    ColorRef color;
    float height_px;
    bool show_grid;
    bool show_labels;
};

struct PortalWidgetSpec {
    TextSlice url;
    TextSlice html;
    float zoom;
    bool has_zoom;
    TextSlice find_query;
    bool has_find;
    bool find_match_case;
    bool find_forward;
    TextSlice nav_action;
    TextSlice eval_script;
    TextSlice init_script;
    TextSlice post_message;
    TextSlice on_state_action;
    uint32_t on_state_handler;
    TextSlice on_msg_action;
    uint32_t on_msg_handler;
    TextSlice on_nav_action;
    uint32_t on_nav_handler;
};

struct PortalSyncSlot {
    uint32_t element_id;
    uint64_t url_hash;
    uint64_t html_hash;
    uint64_t find_hash;
    uint64_t nav_hash;
    uint64_t eval_hash;
    uint64_t init_hash;
    uint64_t post_hash;
    float applied_zoom;
    bool has_applied_zoom;
    uint64_t last_state_hash;
    uint64_t last_used_frame;
};

struct DocViewTrackSlot {
    uint32_t element_id;
    bool sel_active;
    uint32_t sel_anchor;
    uint32_t sel_focus;
    uint64_t last_find_hash;
    uint32_t last_find_count;
    uint64_t last_used_frame;
};

struct DiffAnnotationItem {
    uint32_t line;
    bool is_new_side;
    float height;
    TextSlice author;
    TextSlice body;
    ColorRef color;
};

struct DiffHunkActionItem {
    TextSlice id;
    TextSlice label;
};

struct DiffViewSpec {
    TextSlice old_text;
    TextSlice new_text;
    TextSlice language;
    TextSlice file_path;
    uint8_t mode; // 0 = split, 1 = unified
    uint32_t context_lines;
    bool collapse_unchanged;
    bool word_diff;
    bool show_overview_ruler;
    bool show_line_numbers;
    bool editable;
    float font_size;
    bool has_font_size;
    uint32_t ann_off;
    uint32_t ann_cnt;
    DiffHunkActionItem hunk_actions[8];
    uint32_t hunk_action_cnt;
    TextSlice on_gutter_click_action;
    uint32_t on_gutter_click_handler;
    TextSlice on_gutter_range_action;
    uint32_t on_gutter_range_handler;
    TextSlice on_change_action;
    uint32_t on_change_handler;
    TextSlice on_hunk_action;
    uint32_t on_hunk_handler;
};

struct DiffLineMeta {
    int8_t kind; // 0=same, 1=add, -1=del, 2=pad
    uint32_t orig_line;
};

struct DiffHunk {
    uint32_t split_start;
    uint32_t split_end;
    uint32_t uni_start;
    uint32_t uni_end;
    uint32_t old_start;
    uint32_t old_count;
    uint32_t new_start;
    uint32_t new_count;
};

struct DiffViewCacheEntry {
    uint32_t element_id;
    uint64_t diff_hash;
    uint8_t mode;
    bool collapse_unchanged;
    bool word_diff;
    uint32_t context_lines;
    EtchTextBuffer* left_buf;
    EtchTextBuffer* right_buf;
    EtchTextBuffer* unified_buf;
    DiffLineMeta* left_meta;
    DiffLineMeta* right_meta;
    DiffLineMeta* unified_meta;
    uint32_t split_line_count;
    uint32_t unified_line_count;
    EtchCodeInlineHighlight* left_hls;
    uint32_t left_hl_count;
    EtchCodeInlineHighlight* right_hls;
    uint32_t right_hl_count;
    EtchCodeInlineHighlight* unified_hls;
    uint32_t unified_hl_count;
    EtchCodeOverviewMarker* markers;
    uint32_t marker_count;
    EditorFoldItem* folds;
    uint32_t fold_count;
    DiffHunk* hunks;
    uint32_t hunk_count;
    uint32_t active_hunk;
    uint32_t added_lines;
    uint32_t removed_lines;
    float last_left_scroll_x;
    float last_right_scroll_x;
    float last_left_scroll_y;
    float last_right_scroll_y;
    uint64_t last_used_frame;
};

struct DockSyncEntry {
    uint32_t element_id;
    uint32_t known_panes[32];
    uint32_t known_pane_count;
    bool initialized;
    uint64_t applied_layout_hash;
    uint64_t last_layout_hash;
    uint32_t last_active_pane;
    uint64_t last_used_frame;
};

struct HoverCardStateSlot {
    uint32_t element_id;
    bool open;
    double hover_enter_ms;
    double hover_leave_ms;
    uint64_t last_used_frame;
};

struct ToastTimerSlot {
    uint32_t element_id;
    double opened_ms;
    bool fired;
    uint64_t last_used_frame;
};

struct ColorPickerSlot {
    uint32_t element_id;
    float h, s, v;
    bool initialized;
    uint64_t last_used_frame;
};

struct EditorScrollSlot {
    uint32_t element_id;
    float last_x, last_y;
    float applied_prop_x, applied_prop_y;
    bool had_prop_x, had_prop_y;
    uint32_t last_cursor_byte, last_anchor_byte;
    bool has_cursor_state;
    uint64_t last_used_frame;
};

struct DragHoverSlot {
    uint32_t element_id;
    bool hovering;
    uint64_t last_used_frame;
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
    uint32_t last_vis_cols;
    uint32_t last_vis_rows;
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
    InlaySlotMeasureFn measure_callback;
    EtchA11yRole a11y_role;
    char a11y_label[64];
    void* user;
};

struct ContentBinding {
    char name[64];
    InlayUiKind kind;
    void* resource;
};

struct TerminalBinding {
    char name[64];
    InlayTerminalReadFn read_fn;
    void* user;
};

struct StreamBinding {
    char name[64];
    InlayStream* stream;
    uint64_t last_seen_cursor;
};

struct TimerEntry {
    JSValue function;
    double due;
    double interval_ms;
    uint32_t id;
    bool is_raf;
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
    NodeDndSpec* dnd_specs;
    uint32_t dnd_spec_count, dnd_spec_cap;
    InputRichSpec* input_rich_specs;
    uint32_t input_rich_spec_count, input_rich_spec_cap;
    InputHighlightItem* input_highlights;
    uint32_t input_highlight_count, input_highlight_cap;
    CodeEditorRichSpec* editor_rich_specs;
    uint32_t editor_rich_spec_count, editor_rich_spec_cap;
    EditorInlayHintItem* editor_hints;
    uint32_t editor_hint_count, editor_hint_cap;
    EditorInlineHlItem* editor_inline_hls;
    uint32_t editor_inline_hl_count, editor_inline_hl_cap;
    EditorOverviewItem* editor_markers;
    uint32_t editor_marker_count, editor_marker_cap;
    EditorLineDecoItem* editor_line_decos;
    uint32_t editor_line_deco_count, editor_line_deco_cap;
    EditorFoldItem* editor_folds;
    uint32_t editor_fold_count, editor_fold_cap;
    EditorViewZoneItem* editor_view_zones;
    uint32_t editor_view_zone_count, editor_view_zone_cap;
    TermSearchMatchItem* term_search_matches;
    uint32_t term_search_match_count, term_search_match_cap;
    DockSpaceSpec* dock_space_specs;
    uint32_t dock_space_spec_count, dock_space_spec_cap;
    DockPaneSpec* dock_pane_specs;
    uint32_t dock_pane_spec_count, dock_pane_spec_cap;
    SheetSpec* sheet_specs;
    uint32_t sheet_spec_count, sheet_spec_cap;
    HoverCardSpec* hover_card_specs;
    uint32_t hover_card_spec_count, hover_card_spec_cap;
    ChoiceItemSpec* choice_items;
    uint32_t choice_item_count, choice_item_cap;
    RadioGroupSpec* radio_group_specs;
    uint32_t radio_group_spec_count, radio_group_spec_cap;
    ToggleGroupSpec* toggle_group_specs;
    uint32_t toggle_group_spec_count, toggle_group_spec_cap;
    ColorPickerSpec* color_picker_specs;
    uint32_t color_picker_spec_count, color_picker_spec_cap;
    ChartSpec* chart_specs;
    uint32_t chart_spec_count, chart_spec_cap;
    ChartPointSpec* chart_points;
    uint32_t chart_point_count, chart_point_cap;
    PortalWidgetSpec* portal_specs;
    uint32_t portal_spec_count, portal_spec_cap;
    DiffViewSpec* diff_specs;
    uint32_t diff_spec_count, diff_spec_cap;
    DiffAnnotationItem* diff_annotations;
    uint32_t diff_annotation_count, diff_annotation_cap;
    // Persistent widget & style caches
    DocCacheEntry* doc_cache;
    uint32_t doc_cache_count, doc_cache_cap;
    DocViewTrackSlot* doc_track_slots;
    uint32_t doc_track_slot_count, doc_track_slot_cap;
    PortalSyncSlot* portal_sync_slots;
    uint32_t portal_sync_slot_count, portal_sync_slot_cap;
    DiffViewCacheEntry* diff_cache;
    uint32_t diff_cache_count, diff_cache_cap;
    TermCacheEntry* term_cache;
    uint32_t term_cache_count, term_cache_cap;
    VirtualListWinEntry* vlist_cache;
    uint32_t vlist_cache_count, vlist_cache_cap;
    TableSortCache* table_sort_cache;
    uint32_t table_sort_cache_count, table_sort_cache_cap;
    SkeletonPulseSlot* skeleton_cache;
    uint32_t skeleton_cache_count, skeleton_cache_cap;
    DockSyncEntry* dock_sync_cache;
    uint32_t dock_sync_cache_count, dock_sync_cache_cap;
    HoverCardStateSlot* hover_card_states;
    uint32_t hover_card_state_count, hover_card_state_cap;
    ToastTimerSlot* toast_timers;
    uint32_t toast_timer_count, toast_timer_cap;
    ColorPickerSlot* color_picker_states;
    uint32_t color_picker_state_count, color_picker_state_cap;
    EditorScrollSlot* editor_scroll_states;
    uint32_t editor_scroll_state_count, editor_scroll_state_cap;
    DragHoverSlot* drag_hover_states;
    uint32_t drag_hover_state_count, drag_hover_state_cap;
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
    ActionBinding* actions;
    uint32_t action_count, action_cap;
    SlotBinding slots[32];
    uint32_t slot_count;
    ContentBinding* content;
    uint32_t content_count, content_cap;
    TerminalBinding* terminals;
    uint32_t terminal_count, terminal_cap;
    StreamBinding* streams;
    uint32_t stream_count, stream_cap;
    InlayImageResolverFn image_resolver_fn;
    void* image_resolver_user_data;
    InlayConsoleFn console_fn;
    void* console_user_data;
    EtchSyntaxTokenizeFn code_tok_fn;
    void* code_tok_user_data;
    InlayCodeHighlightFn doc_hl_fn;
    void* doc_hl_user_data;
    InlayCustomFenceEstimateFn doc_fence_est;
    InlayCustomFenceRenderFn doc_fence_ren;
    void* doc_fence_user_data;
    JSValue* handlers;
    uint32_t handler_count, handler_capacity;
    TimerEntry timers[64];
    uint32_t next_timer;
    int64_t modified;
    uint64_t file_size, deadline, frame_counter;
    double frequency;
    bool running, drawing, draw_parent_col, timed_out, pending_state, state_dirty, loaded, hmr_file_error;
    bool hide_tree_guide;
    float last_view_width_px;
    float current_container_width_px;
    uint64_t memo_reused_nodes;
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

inline uint32_t inlay_ui_content_index(const InlaySurface* s, InlayUiKind kind, EtchStr name) noexcept {
    for (uint32_t i = 0; i < s->content_count; ++i) {
        if (s->content[i].kind == kind && inlay_ui_equal(name, s->content[i].name)) return i;
    }
    return s->content_count;
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
    c.on_key_handler = INLAY_UI_NONE;
    c.on_text_handler = INLAY_UI_NONE;
    c.dnd_idx = INLAY_UI_NONE;
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
void inlay_ui_free_spec_pools(InlaySurface* s) noexcept;
void inlay_ui_move_spec_pools(InlaySurface* dst, InlaySurface* src) noexcept;
void inlay_ui_free_widget_caches(InlaySurface* s) noexcept;
void inlay_ui_trim_document_cache(InlaySurface* s) noexcept;

#endif // INLAY_ENABLE_SCRIPT
