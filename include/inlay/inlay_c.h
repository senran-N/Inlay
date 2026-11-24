#ifndef INLAY_C_H
#define INLAY_C_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <etch/etch_c.h>
#define INLAY_VERSION_MAJOR 0
#define INLAY_VERSION_MINOR 14
#define INLAY_VERSION_PATCH 0
#define INLAY_STRING_DETAIL(x) #x
#define INLAY_STRING(x) INLAY_STRING_DETAIL(x)
#define INLAY_VERSION_STRING INLAY_STRING(INLAY_VERSION_MAJOR) "." INLAY_STRING(INLAY_VERSION_MINOR) "." INLAY_STRING(INLAY_VERSION_PATCH)
#define INLAY_ABI_VERSION 2u
#ifndef INLAY_API
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(INLAY_SHARED)
#if defined(INLAY_BUILD) || defined(INLAY_EXPORTS)
#define INLAY_API __declspec(dllexport)
#else
#define INLAY_API __declspec(dllimport)
#endif
#else
#define INLAY_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define INLAY_API __attribute__((visibility("default")))
#else
#define INLAY_API
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
INLAY_API uint32_t inlay_abi_version(void);

typedef struct InlayDoc InlayDoc;
typedef enum InlayBlockKind {
 INLAY_PARAGRAPH, INLAY_HEADING, INLAY_CODE_BLOCK, INLAY_BLOCKQUOTE,
 INLAY_ALERT, INLAY_LIST, INLAY_TASK_LIST, INLAY_TABLE, INLAY_THEMATIC_BREAK, INLAY_DETAILS, INLAY_MATH_BLOCK,
 INLAY_IMAGE, INLAY_FOOTNOTE
} InlayBlockKind;
typedef enum InlayAlertKind {
 INLAY_ALERT_NONE = 0,
 INLAY_ALERT_NOTE = 1,
 INLAY_ALERT_TIP = 2,
 INLAY_ALERT_IMPORTANT = 3,
 INLAY_ALERT_WARNING = 4,
 INLAY_ALERT_CAUTION = 5
} InlayAlertKind;
typedef enum InlayBlockFlags {
 INLAY_BLOCK_FLAG_NONE      = 0u,
 INLAY_BLOCK_FLAG_ORDERED   = 1u << 0,
 INLAY_BLOCK_FLAG_TASK      = 1u << 1,
 INLAY_BLOCK_FLAG_CHECKED   = 1u << 2,
 INLAY_BLOCK_FLAG_IN_QUOTE  = 1u << 3,
 INLAY_BLOCK_FLAG_IN_ALERT  = 1u << 4,
 INLAY_BLOCK_FLAG_IN_LIST   = 1u << 5,
 INLAY_BLOCK_FLAG_SYNTHETIC = 1u << 6
} InlayBlockFlags;
typedef struct InlayBlock {
 InlayBlockKind kind;
 uint32_t offset, length, content_offset, content_length, level;
 uint32_t language_offset, language_length;
 bool complete;
 int32_t parent_index;       /* -1 for top-level blocks, or parent AST index for the current parse */
 uint16_t depth;             /* total container nesting depth (0 = top-level) */
 uint16_t quote_depth;       /* enclosing blockquote/alert nesting depth */
 uint16_t list_depth;        /* enclosing list nesting depth (0 = not inside a list) */
 uint8_t alert_kind;         /* InlayAlertKind */
 uint8_t flags;              /* InlayBlockFlags bitmask */
 uint32_t list_ordinal;      /* 1-based ordinal for ordered list items, 0 for unordered */
 uint32_t task_mark_offset;  /* source offset of ' '/'x'/'X' in [ ], or UINT32_MAX */
} InlayBlock;
typedef struct InlayDocConfig {
 uint32_t struct_size;
 uint32_t initial_source_bytes, initial_blocks;
} InlayDocConfig;
typedef enum InlayInlineKind {
 INLAY_INLINE_TEXT=0,
 INLAY_INLINE_CODE=1,
 INLAY_INLINE_LINK=2,
 INLAY_INLINE_IMAGE=3
} InlayInlineKind;
/* Called for code and links. Return true to apply edits. Replacement text must
   live through the view call and any use of clicked_link. url is empty for
   code; an identified code span reports its displayed text on click.
   The callback must not mutate the document. */
typedef bool (*InlayInlineDecoratorFn)(InlayInlineKind kind,EtchStr text,EtchStr url,
                                      EtchTextSpan* inout_span,void* user_data);
typedef struct InlayDocStyle {
 uint32_t struct_size;
 EtchColor background,text_color,muted_color,heading_color,link_color,
  code_bg,code_border,quote_bg,quote_border,table_header_bg,
  table_border,highlight_bg,selection_color;
 bool has_background;
 uint16_t font_id,bold_font_id,mono_font_id;
 float line_height_scale;
 float heading_scale[6];
 float code_font_scale; /* Optical scale for inline code and code blocks (0 = 1.0). */
 /* Extended styling fields (zero = default for full ABI backward compatibility). */
 EtchColor inline_code_bg;
 EtchColor inline_code_border;
 EtchColor inline_code_color;
 EtchColor code_color;
 EtchColor code_header_bg;
 float inline_code_padding_x; /* 0 = default 4.5f */
 float inline_code_padding_y; /* <= 0 = default 0.0f */
 float inline_code_radius_px; /* 0 = default 4.0f */
 bool inline_code_no_border;  /* Omit inline code border even if code_border is set. */
 EtchColor alert_colors[5];   /* NOTE, TIP, IMPORTANT, WARNING, CAUTION (alpha 0 = semantic default). */
 float alert_bg_alpha;        /* 0 = default 0.09f subtle wash; < 0 disables wash. */
 EtchColor table_bg;
 EtchColor table_zebra_bg;
 EtchColor table_header_color;
} InlayDocStyle;
typedef struct InlayImageResult {
 struct SDL_GPUTexture* texture;
 float width_px,height_px,radius_px;
} InlayImageResult;
typedef bool (*InlayImageResolverFn)(EtchStr alt,EtchStr src,InlayImageResult* out_image,void* user_data);
typedef void (*InlayTaskToggleFn)(uint32_t block_index,uint32_t item_index,bool checked,void* user_data);
/* Return > 0 to claim a fenced code block (other than built-in mermaid) as a
   single unsplit custom row with that initial estimated height in logical px.
   Return <= 0 to keep standard code-block rendering. Must not mutate the doc. */
typedef float (*InlayCustomFenceEstimateFn)(EtchStr language,EtchStr body,
                                            float available_width_px,float font_size_px,
                                            void* user_data);
/* Renders a custom fenced block previously claimed by custom_fence_estimate_fn.
   Must not mutate the document. */
typedef void (*InlayCustomFenceRenderFn)(uint32_t block_index,uint32_t row_id,
                                         EtchStr language,EtchStr body,
                                         float available_width_px,float font_size_px,
                                         void* user_data);
/* Optional extra controls rendered on the right side of a standard code block's
   header strip (before the built-in Copy button when visible). */
typedef void (*InlayCodeHeaderFn)(uint32_t block_index,uint32_t row_id,
                                  EtchStr language,EtchStr full_code,
                                  void* user_data);
typedef struct InlayDocViewDesc {
 uint32_t struct_size;
 uint32_t id;
 float width_px,height_px,font_size_px; /* Zero height builds an auto-height view. */
 float block_padding_px,block_gap_px;
 float table_cell_padding_x,table_cell_padding_y,table_radius_px;
 bool hide_code_copy_button;
 InlayInlineDecoratorFn inline_decorator_fn;
 void* inline_decorator_user_data;
 const InlayDocStyle* style;
 InlayImageResolverFn image_resolver_fn;
 void* image_resolver_user_data;
 InlayTaskToggleFn task_toggle_fn;
 void* task_toggle_user_data;
 bool stick_to_bottom;
 InlayCustomFenceEstimateFn custom_fence_estimate_fn;
 InlayCustomFenceRenderFn custom_fence_render_fn;
 void* custom_fence_user_data;
 InlayCodeHeaderFn code_header_fn;
 void* code_header_user_data;
 bool show_code_line_numbers;
 InlayCodeHeaderFn diagram_header_fn;
 void* diagram_header_user_data;
 uint32_t image_epoch; /* Bump to reflow image sizes after async texture loads complete. */
} InlayDocViewDesc;
typedef struct InlayDocResult {
 EtchStr clicked_link;
 uint32_t visible_first,visible_end,total_rows;
 float measured_height_px;
 bool at_bottom;
 InlayInlineKind clicked_kind;
 EtchRect clicked_bounds;
 uint32_t clicked_block_index;
 uint16_t clicked_mods;
} InlayDocResult;
typedef struct InlayDocSelection {
 uint32_t anchor;
 uint32_t focus;
 uint32_t total_bytes;
 bool active;
} InlayDocSelection;
typedef struct InlayMathDesc {
 uint32_t struct_size;
 EtchVec2 pos;
 float font_size_px;
 EtchColor color;
} InlayMathDesc;
typedef enum InlayMathError {
 INLAY_MATH_ERROR_NONE = 0,
 INLAY_MATH_ERROR_INVALID_SYNTAX = 1,
 INLAY_MATH_ERROR_UNSUPPORTED_COMMAND = 2,
 INLAY_MATH_ERROR_DEPTH_LIMIT = 3
} InlayMathError;
typedef struct InlayMathResult {
 float width,height,baseline;
 bool valid;
 uint32_t error_offset; /* Zero-based UTF-8 byte position. May equal source length at EOF. */
 InlayMathError error;
} InlayMathResult;
INLAY_API InlayMathDesc inlay_math_desc_default(void);
INLAY_API InlayMathResult inlay_draw_math(EtchStr tex,const InlayMathDesc* desc);
INLAY_API InlayMathResult inlay_measure_math(EtchStr tex,float font_size_px);
INLAY_API uint32_t inlay_math_export_svg(EtchStr tex,const InlayMathDesc* desc,char* out_buf,uint32_t cap);
INLAY_API uint32_t inlay_math_export_mathml(EtchStr tex,char* out_buf,uint32_t cap);
typedef enum InlayDiagramDiagnosticKind {
 INLAY_DIAGRAM_UNSUPPORTED_DIRECTIVE, INLAY_DIAGRAM_UNSUPPORTED_KIND,
 INLAY_DIAGRAM_INVALID_DIRECTION, INLAY_DIAGRAM_INVALID_SYNTAX
} InlayDiagramDiagnosticKind;
/* Synchronous during parsing. Ranges are in source UTF-8 bytes and may be
   empty at EOF. The callback must not reenter rendering. */
typedef void (*InlayDiagramDiagnosticFn)(InlayDiagramDiagnosticKind,uint32_t byte_offset,uint32_t byte_length,void* user_data);
typedef struct InlayDiagramDesc {
 uint32_t struct_size;
 EtchVec2 pos;
 float font_size_px,node_gap_px,rank_gap_px;
 uint32_t canvas_id;
 float viewport_width_px,viewport_height_px;
 float max_width_px;      /* 0 = unconstrained; > 0 auto-fits natively in layout space. */
 float min_scale;         /* 0 = default 0.62f. */
 bool has_custom_fonts;   /* When true, uses font_id / bold_font_id instead of defaults. */
 uint16_t font_id;
 uint16_t bold_font_id;
 InlayDiagramDiagnosticFn diagnostic;
 void* diagnostic_user_data;
} InlayDiagramDesc;
typedef struct InlayDiagramNode {EtchStr id,label;EtchRect bounds;uint32_t rank;} InlayDiagramNode;
typedef struct InlayDiagramEdge {uint32_t from,to;bool reversed,dashed;EtchStr label;} InlayDiagramEdge;
/* Layout spans belong to the active Etch frame arena and expire at frame end. */
typedef struct InlayDiagramResult {
 const InlayDiagramNode* nodes;
 const InlayDiagramEdge* edges;
 uint32_t node_count,edge_count;
 float width,height;
 bool valid,horizontal,sequence;
 float scale;          /* 1.0f unscaled, < 1.0f natively auto-fitted. */
 float natural_width;  /* Unscaled layout width before auto-fit. */
 float natural_height; /* Unscaled layout height before auto-fit. */
 uint32_t warning_count; /* Unsupported directives skipped during parsing. */
} InlayDiagramResult;
INLAY_API InlayDiagramDesc inlay_diagram_desc_default(void);
INLAY_API InlayDiagramResult inlay_diagram_layout(EtchStr source,const InlayDiagramDesc* desc);
INLAY_API InlayDiagramResult inlay_diagram_view(EtchStr source,const InlayDiagramDesc* desc);
INLAY_API uint32_t inlay_diagram_export_svg(EtchStr source,const InlayDiagramDesc* desc,char* out_buf,uint32_t cap);
INLAY_API InlayDocConfig inlay_doc_config_default(void);
INLAY_API InlayDocStyle inlay_doc_style_default(void);
INLAY_API InlayDocViewDesc inlay_doc_view_desc_default(void);
INLAY_API InlayDoc* inlay_doc_create(EtchContext*,const InlayDocConfig*);
INLAY_API void inlay_doc_destroy(InlayDoc*);
INLAY_API void inlay_doc_clear(InlayDoc*);
INLAY_API bool inlay_doc_set_utf8(InlayDoc*,EtchStr);
INLAY_API bool inlay_doc_append_utf8(InlayDoc*,EtchStr);
/* Replace [begin, end) in the current source. Offsets and replacement text
   must be complete UTF-8. A range edit requires an empty stream carry and
   invalidates rendered text offsets and selections. */
INLAY_API bool inlay_doc_replace_utf8(InlayDoc*,uint32_t begin,uint32_t end,EtchStr);
/* Compare for equality to check whether the current document has changed. */
INLAY_API uint32_t inlay_doc_revision(const InlayDoc*);
/* Host-defined annotation IDs are nonzero and local to a document. Ranges are
   half-open UTF-8 source bytes. Inserts on either edge stay outside a range.
   A point follows an insertion at its position. A replacement overlapping a
   range becomes part of it. A successful set_utf8 or clear removes all anchors. */
typedef struct InlayDocAnchor { uint64_t id; uint32_t begin,end; } InlayDocAnchor;
INLAY_API bool inlay_doc_set_anchor(InlayDoc*,InlayDocAnchor anchor);
INLAY_API bool inlay_doc_get_anchor(const InlayDoc*,uint64_t id,InlayDocAnchor* out);
INLAY_API bool inlay_doc_remove_anchor(InlayDoc*,uint64_t id);
/* Enumerate in ID order for persistence alongside the matching source. */
INLAY_API uint32_t inlay_doc_anchor_count(const InlayDoc*);
INLAY_API bool inlay_doc_anchor_at(const InlayDoc*,uint32_t index,InlayDocAnchor* out);
typedef enum InlayDocStreamStatus {
 INLAY_DOC_STREAM_INVALID, INLAY_DOC_STREAM_READY, INLAY_DOC_STREAM_PARTIAL_UTF8,
 INLAY_DOC_STREAM_OPEN_FENCE
} InlayDocStreamStatus;
/* End-of-stream check. The returned status leaves the document available for
   display or further appends. An open fence is valid CommonMark at EOF. */
INLAY_API InlayDocStreamStatus inlay_doc_stream_status(const InlayDoc*);
INLAY_API uint32_t inlay_doc_block_count(const InlayDoc*);
INLAY_API const InlayBlock* inlay_doc_block(const InlayDoc*,uint32_t index);
INLAY_API EtchStr inlay_doc_source(const InlayDoc*);
INLAY_API InlayDocResult inlay_doc_view(InlayDoc*,const InlayDocViewDesc*);
/* Total height in logical pixels. Uses matching view measurements or estimates
   with default flow spacing. May be called outside a frame. */
INLAY_API float inlay_doc_measure_height(InlayDoc*,uint32_t view_id,float width_px,float font_size_px);
INLAY_API bool inlay_doc_view_is_at_bottom(uint32_t view_id);
INLAY_API void inlay_doc_view_scroll_to_bottom(uint32_t view_id,bool stop_momentum);
INLAY_API InlayDocSelection inlay_doc_view_selection(const InlayDoc* doc,uint32_t view_id);
/* Search the current rendered view's plain text, starting at a byte offset.
   Returns an inactive selection when the view is stale or there is no match. */
INLAY_API InlayDocSelection inlay_doc_view_find_text(const InlayDoc*,uint32_t view_id,EtchStr query,uint32_t from_byte);
/* Queue a reveal of a plain-text byte offset in a fixed-height document view. */
INLAY_API bool inlay_doc_view_reveal_text(InlayDoc*,uint32_t view_id,uint32_t byte_offset,EtchAlignY align);
INLAY_API void inlay_doc_view_set_selection(InlayDoc* doc,uint32_t view_id,uint32_t anchor,uint32_t focus);
INLAY_API void inlay_doc_view_select_all(InlayDoc* doc,uint32_t view_id);
INLAY_API void inlay_doc_view_clear_selection(InlayDoc* doc,uint32_t view_id);
/* Copies selected clean plain text into out_buf (NUL-terminated if cap > 0). Returns required length excluding NUL. */
INLAY_API uint32_t inlay_doc_view_copy_plain_text(const InlayDoc* doc,uint32_t view_id,char* out_buf,uint32_t cap);
/* Copies selected range as balanced Markdown into out_buf (NUL-terminated if cap > 0). Returns required length excluding NUL. */
INLAY_API uint32_t inlay_doc_view_copy_markdown(const InlayDoc* doc,uint32_t view_id,char* out_buf,uint32_t cap);
/* Exports semantic HTML5 for the full document into out_buf (NUL-terminated if cap > 0). Returns required length excluding NUL. */
INLAY_API uint32_t inlay_doc_export_html(const InlayDoc* doc,char* out_buf,uint32_t cap);
/* Copies selected range as semantic HTML5 into out_buf (NUL-terminated if cap > 0). Returns required length excluding NUL. */
INLAY_API uint32_t inlay_doc_view_copy_html(const InlayDoc* doc,uint32_t view_id,char* out_buf,uint32_t cap);
/* Document heading outline enumeration and multi-match search index. */
typedef struct InlayDocOutlineEntry {
 uint32_t block_index,source_offset;
 uint8_t level;
 EtchStr text;
} InlayDocOutlineEntry;
INLAY_API uint32_t inlay_doc_outline_count(const InlayDoc* doc);
INLAY_API bool inlay_doc_outline_at(const InlayDoc* doc,uint32_t index,InlayDocOutlineEntry* out);
INLAY_API uint32_t inlay_doc_view_find_all(const InlayDoc* doc,uint32_t view_id,EtchStr query,InlayDocSelection* out_matches,uint32_t max_matches);
/* Reports Mermaid cross-frame layout cache statistics for a document. */
INLAY_API void inlay_doc_diagram_cache_stats(const InlayDoc* doc,uint32_t* out_entries,uint64_t* out_hits,uint64_t* out_misses);

typedef struct InlaySurface InlaySurface;
typedef struct InlaySurfaceDesc {
 uint32_t struct_size;
 size_t memory_limit_bytes,max_stack_bytes;
 float max_tick_ms;
 uint32_t max_nodes,max_text_bytes;
 bool disable_jsx; /* When false (default), scripts with JSX syntax auto-transpile cleanly. */
} InlaySurfaceDesc;
typedef enum InlayUiKind {
 INLAY_UI_BOX, INLAY_UI_TEXT, INLAY_UI_BUTTON, INLAY_UI_INPUT, INLAY_UI_TOGGLE,
 INLAY_UI_SCROLL, INLAY_UI_PROGRESS, INLAY_UI_SLIDER, INLAY_UI_BADGE, INLAY_UI_DIVIDER, INLAY_UI_IMAGE,
 INLAY_UI_CARD, INLAY_UI_SPACER, INLAY_UI_CHECKBOX, INLAY_UI_TABS, INLAY_UI_COMBOBOX,
 INLAY_UI_MODAL, INLAY_UI_POPOVER, INLAY_UI_TOOLTIP, INLAY_UI_SPLIT,
 INLAY_UI_MATH, INLAY_UI_DIAGRAM, INLAY_UI_SLOT,
 INLAY_UI_DOC, INLAY_UI_TERMINAL, INLAY_UI_VIRTUAL_LIST, INLAY_UI_TABLE,
 INLAY_UI_TREE, INLAY_UI_TREE_ITEM, INLAY_UI_MENU, INLAY_UI_CONTEXT_MENU,
 INLAY_UI_AVATAR, INLAY_UI_TOAST, INLAY_UI_ACCORDION, INLAY_UI_SKELETON, INLAY_UI_CODE_EDITOR, INLAY_UI_FORM
} InlayUiKind;
typedef struct InlayUiNode {InlayUiKind kind;uint32_t id,first_child,next_sibling;EtchStr text;} InlayUiNode;
typedef struct InlaySurfaceViewDesc {
 uint32_t struct_size;
 uint32_t id;
 float width_px,height_px; /* <= 0 grows to fill parent container (etch_sizing_grow). */
} InlaySurfaceViewDesc;
/* last_reload_ms measures engine reload; total includes filesystem read latency. */
typedef struct InlaySurfaceStats {
 uint64_t js_ticks,reloads;
 uint32_t node_count;
 double last_tick_ms,last_reload_ms,last_reload_total_ms;
 bool has_error;
 uint32_t cached_documents;
 uint64_t document_cache_misses;
} InlaySurfaceStats;
/* Callbacks may set state, but must not destroy or recursively dispatch the surface.
   The host calls poll_hmr periodically while idle; timers advance when view is built. */
typedef void (*InlayActionFn)(InlaySurface*,EtchStr payload_json,void* user_data);
typedef enum InlayAsyncActionKind {
 INLAY_ASYNC_ACTION_NONE, INLAY_ASYNC_ACTION_START, INLAY_ASYNC_ACTION_CANCEL
} InlayAsyncActionKind;
typedef struct InlayAsyncActionEvent {
 uint32_t struct_size;
 InlayAsyncActionKind kind;
 uint32_t request_id;
 EtchStr name, payload_json;
} InlayAsyncActionEvent;
/* Host-bound native content bridge mounted inside declarative ui.slot() nodes.
   Invoked during inlay_surface_view inside the slot's Etch layout box. Must not
   destroy or recursively dispatch the surface. */
typedef void (*InlaySlotRenderFn)(InlaySurface*,uint32_t element_id,float width_px,float height_px,EtchStr props_json,void* user_data);
typedef bool (*InlaySlotMeasureFn)(InlaySurface*,uint32_t element_id,float available_w,float available_h,EtchStr props_json,EtchVec2* out_size,void* user_data);

typedef enum InlayActionPermission {
 INLAY_ACTION_READ = 0,
 INLAY_ACTION_WRITE = 1,
 INLAY_ACTION_DESTRUCTIVE = 2
} InlayActionPermission;
typedef struct InlayActionPolicy {
 uint32_t struct_size;
 InlayActionPermission permission;
 bool require_approval;
 EtchStr required_keys_csv; /* Comma-separated JSON keys required in object payload, or empty. */
} InlayActionPolicy;
typedef bool (*InlayActionAuditFn)(InlaySurface*,EtchStr action_name,InlayActionPermission permission,
 bool requires_approval,EtchStr payload_json,void* user_data);
INLAY_API InlayActionPolicy inlay_action_policy_default(void);

/* Reusable host-owned row/tree data source for VirtualList & Tree (0 full-JSON marshaling). */
typedef struct InlayModel InlayModel;
typedef struct InlayModelRow {
 uint64_t row_id;
 uint32_t depth;
 bool has_children;
 bool expanded;
 EtchStr label;
 EtchStr detail;
 EtchStr badge;
} InlayModelRow;
typedef bool (*InlayModelReadFn)(uint32_t index,InlayModelRow* out_row,void* user_data);
typedef enum InlayModelChangeKind {
 INLAY_MODEL_CHANGE_NONE = 0,
 INLAY_MODEL_CHANGE_UPDATE = 1,
 INLAY_MODEL_CHANGE_INSERT = 2,
 INLAY_MODEL_CHANGE_REMOVE = 3,
 INLAY_MODEL_CHANGE_MOVE = 4,
 INLAY_MODEL_CHANGE_RESET = 5
} InlayModelChangeKind;
typedef struct InlayModelChange {
 InlayModelChangeKind kind;
 uint32_t first_row,count,dst_row;
 uint64_t revision;
} InlayModelChange;
INLAY_API InlayModel* inlay_model_create(InlayModelReadFn read_fn,void* user_data);
INLAY_API void inlay_model_destroy(InlayModel*);
INLAY_API void inlay_model_bind_context(InlayModel*,EtchContext*);
INLAY_API void inlay_model_set_count(InlayModel*,uint32_t count);
INLAY_API void inlay_model_notify_change(InlayModel*,uint32_t first_row,uint32_t count);
INLAY_API void inlay_model_notify_insert(InlayModel*,uint32_t index,uint32_t count);
INLAY_API void inlay_model_notify_remove(InlayModel*,uint32_t index,uint32_t count);
INLAY_API void inlay_model_notify_move(InlayModel*,uint32_t from_index,uint32_t to_index,uint32_t count);
INLAY_API bool inlay_model_last_change(const InlayModel*,InlayModelChange* out);
INLAY_API uint32_t inlay_model_count(const InlayModel*);
INLAY_API uint64_t inlay_model_revision(const InlayModel*);
INLAY_API InlaySurfaceDesc inlay_surface_desc_default(void);
INLAY_API InlaySurfaceViewDesc inlay_surface_view_desc_default(void);
INLAY_API InlaySurface* inlay_surface_create(EtchContext*,const InlaySurfaceDesc*);
INLAY_API void inlay_surface_destroy(InlaySurface*);
INLAY_API bool inlay_surface_set_script(InlaySurface*,EtchStr script);
INLAY_API bool inlay_surface_set_state_json(InlaySurface*,EtchStr json);
INLAY_API EtchStr inlay_surface_state_json(const InlaySurface*);
INLAY_API bool inlay_surface_bind_action(InlaySurface*,EtchStr name,InlayActionFn,void* user_data);
/* Exposes a host-approved name to request(name, payload) in script. */
INLAY_API bool inlay_surface_allow_async_action(InlaySurface*,EtchStr name);
/* Poll on the owner thread after script updates. Copy the borrowed start event
   slices before starting a worker. A cancel event carries only its request_id. */
INLAY_API InlayAsyncActionEvent inlay_surface_poll_async_action(InlaySurface*);
/* Complete on the owner thread. A stale ID or invalid JSON returns false. */
INLAY_API bool inlay_surface_complete_async_action(InlaySurface*,uint32_t request_id,bool success,EtchStr result_json);
/* Close permanently and drain pending cancel events before destroying the surface. */
INLAY_API void inlay_surface_close_async_actions(InlaySurface*);
INLAY_API bool inlay_surface_bind_slot(InlaySurface*,EtchStr name,InlaySlotRenderFn,void* user_data);
INLAY_API bool inlay_surface_bind_slot_ex(InlaySurface*,EtchStr name,InlaySlotRenderFn,InlaySlotMeasureFn,
 EtchA11yRole a11y_role,EtchStr a11y_label,void* user_data);
INLAY_API bool inlay_surface_set_action_policy(InlaySurface*,EtchStr name,const InlayActionPolicy* policy);
INLAY_API void inlay_surface_set_action_audit_hook(InlaySurface*,InlayActionAuditFn fn,void* user_data);
INLAY_API bool inlay_surface_bind_model(InlaySurface*,EtchStr name,InlayModel*);
INLAY_API uint64_t inlay_surface_memo_reused_nodes(const InlaySurface*);
INLAY_API uint32_t inlay_surface_export_dts(const InlaySurface*,char* out_buf,uint32_t cap);
/* ui.doc({document: name}) borrows a document from the same Etch context.
   Pass NULL to unbind. Unbind before destroying a document on a live surface. */
INLAY_API bool inlay_surface_bind_document(InlaySurface*,EtchStr name,InlayDoc*);
/* Borrow a host-owned Etch editor buffer. Pass NULL to unbind, then finish a frame
   without the editor before freeing the buffer, so its native selection and accessibility snapshots expire.
   The buffer retains text and undo history across script and view rebuilds. */
INLAY_API bool inlay_surface_bind_text_buffer(InlaySurface*,EtchStr name,EtchTextBuffer*);
INLAY_API bool inlay_surface_set_image_resolver(InlaySurface*,InlayImageResolverFn,void* user_data);
INLAY_API bool inlay_surface_dispatch(InlaySurface*,EtchStr name,EtchStr payload_json);
INLAY_API bool inlay_surface_enable_hmr(InlaySurface*,const char* script_path);
INLAY_API bool inlay_surface_poll_hmr(InlaySurface*);
/* Renders the reactive surface inside the active Etch frame. width_px <= 0 or
   height_px <= 0 grows to fill the parent container (etch_sizing_grow) on that axis. */
INLAY_API void inlay_surface_view(InlaySurface*,const InlaySurfaceViewDesc*);
/* Milliseconds until the next scheduled JS timer expires, 0 if due now, or -1 if none. */
INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface*);
INLAY_API InlaySurfaceStats inlay_surface_stats(const InlaySurface*);
INLAY_API EtchStr inlay_surface_error(const InlaySurface*);
/* Native nodes/text survive until the next script/state/event evaluation. */
INLAY_API const InlayUiNode* inlay_surface_nodes(const InlaySurface*,uint32_t* count);

typedef struct InlayInbox InlayInbox;
typedef struct InlayInboxDesc {
 uint32_t struct_size;
 uint32_t max_messages, max_bytes;
 uint32_t wake_event_type; /* Host-registered SDL user event, or 0 for polling. */
} InlayInboxDesc;
typedef struct InlayInboxMessage {
 uint32_t struct_size;
 uint32_t kind, id, payload_size;
} InlayInboxMessage;
typedef enum InlayInboxStatus {
 INLAY_INBOX_OK, INLAY_INBOX_EMPTY, INLAY_INBOX_FULL, INLAY_INBOX_CLOSED,
 INLAY_INBOX_BUFFER_TOO_SMALL, INLAY_INBOX_INVALID, INLAY_INBOX_NO_MEMORY
} InlayInboxStatus;
INLAY_API InlayInboxDesc inlay_inbox_desc_default(void);
INLAY_API InlayInbox* inlay_inbox_create(const InlayInboxDesc*);
/* Thread-safe. A successful post copies the bytes. An empty-to-nonempty
   transition pushes the configured event, which carries no inbox pointer.
   Event delivery depends on the host's SDL event queue and filters. */
INLAY_API InlayInboxStatus inlay_inbox_post(InlayInbox*,uint32_t kind,uint32_t id,const void* payload,uint32_t size);
/* Single consumer. BUFFER_TOO_SMALL returns the required size in the message
   header and leaves the front message queued. */
INLAY_API InlayInboxStatus inlay_inbox_take(InlayInbox*,InlayInboxMessage*,void* payload,uint32_t capacity);
/* If the consumer stops at a frame budget, poll this before sleeping. */
INLAY_API uint32_t inlay_inbox_pending(InlayInbox*);
/* Close rejects new posts. The consumer can still drain queued messages.
   Join producers and stop the consumer before destroying the inbox. */
INLAY_API void inlay_inbox_close(InlayInbox*);
INLAY_API void inlay_inbox_destroy(InlayInbox*);
/* Drains up to max_messages from inbox and dispatches UTF-8 JSON payloads to surface. */
INLAY_API uint32_t inlay_inbox_drain_to_surface(InlayInbox*,InlaySurface*,uint32_t max_messages);

/* Structured AI conversation & tool-result stream with stable 64-bit IDs and resumable cursor. */
typedef struct InlayStream InlayStream;
typedef enum InlayStreamRole {
 INLAY_STREAM_ROLE_USER = 0,
 INLAY_STREAM_ROLE_ASSISTANT = 1,
 INLAY_STREAM_ROLE_TOOL = 2,
 INLAY_STREAM_ROLE_SYSTEM = 3
} InlayStreamRole;
typedef enum InlayStreamItemKind {
 INLAY_STREAM_ITEM_MARKDOWN = 0,
 INLAY_STREAM_ITEM_TOOL_CALL = 1,
 INLAY_STREAM_ITEM_TOOL_RESULT = 2,
 INLAY_STREAM_ITEM_CITATION = 3,
 INLAY_STREAM_ITEM_ATTACHMENT = 4,
 INLAY_STREAM_ITEM_PROGRESS = 5
} InlayStreamItemKind;
typedef enum InlayStreamStatus {
 INLAY_STREAM_STATUS_PENDING = 0,
 INLAY_STREAM_STATUS_RUNNING = 1,
 INLAY_STREAM_STATUS_DONE = 2,
 INLAY_STREAM_STATUS_ERROR = 3,
 INLAY_STREAM_STATUS_CANCELLED = 4
} InlayStreamStatus;
typedef struct InlayStreamItem {
 uint64_t id;
 uint64_t message_id;
 InlayStreamRole role;
 InlayStreamItemKind kind;
 InlayStreamStatus status;
 float progress;
 uint64_t revision;
 EtchStr title;
 EtchStr body;
 EtchStr metadata_json;
} InlayStreamItem;
INLAY_API InlayStream* inlay_stream_create(void);
INLAY_API void inlay_stream_destroy(InlayStream*);
INLAY_API bool inlay_stream_upsert(InlayStream*,const InlayStreamItem* item);
INLAY_API bool inlay_stream_append_body(InlayStream*,uint64_t id,EtchStr delta_utf8);
INLAY_API bool inlay_stream_remove(InlayStream*,uint64_t id);
INLAY_API uint32_t inlay_stream_count(const InlayStream*);
INLAY_API bool inlay_stream_get_by_id(const InlayStream*,uint64_t id,InlayStreamItem* out_item);
INLAY_API bool inlay_stream_at(const InlayStream*,uint32_t index,InlayStreamItem* out_item);
INLAY_API uint64_t inlay_stream_cursor(const InlayStream*);
INLAY_API uint32_t inlay_stream_export_json(const InlayStream*,char* out_buf,uint32_t cap);
INLAY_API uint32_t inlay_stream_export_since_json(const InlayStream*,uint64_t since_cursor,char* out_buf,uint32_t cap);
INLAY_API bool inlay_stream_import_json(InlayStream*,EtchStr json);
INLAY_API void inlay_stream_set_max_bytes(InlayStream*,size_t max_bytes);

/* Verifiable Generative UI declarative JSON schema validator & safe surface renderer. */
typedef struct InlayGenUiValidation {
 bool valid;
 uint32_t node_count;
 uint32_t error_node_index;
 uint32_t error_offset;
 char error_message[256];
} InlayGenUiValidation;
INLAY_API bool inlay_genui_validate_json(EtchStr spec_json,uint32_t schema_version,InlayGenUiValidation* out);
INLAY_API bool inlay_genui_render_to_surface(InlaySurface*,EtchStr spec_json,uint32_t schema_version,InlayGenUiValidation* out);
INLAY_API uint32_t inlay_genui_list_actions(EtchStr spec_json,char* out_buf,uint32_t cap);

/* Design Tokens Format (DTCG JSON) import/export mapped directly to EtchTheme. */
INLAY_API bool inlay_tokens_import_theme(EtchStr dtcg_json,bool dark_mode,EtchTheme* out_theme);
INLAY_API uint32_t inlay_tokens_export_theme(const EtchTheme* theme,char* out_buf,uint32_t cap);

/* Non-blocking async image loader with bounded LRU cache, cancellation & UI-thread upload. */
typedef struct InlayImageLoader InlayImageLoader;
typedef enum InlayImageLoadStatus {
 INLAY_IMAGE_STATUS_IDLE = 0,
 INLAY_IMAGE_STATUS_LOADING = 1,
 INLAY_IMAGE_STATUS_READY = 2,
 INLAY_IMAGE_STATUS_ERROR = 3
} InlayImageLoadStatus;
typedef struct InlayImageLoaderDesc {
 uint32_t struct_size;
 uint32_t max_entries;
 size_t max_vram_bytes;
} InlayImageLoaderDesc;
INLAY_API InlayImageLoaderDesc inlay_image_loader_desc_default(void);
INLAY_API InlayImageLoader* inlay_image_loader_create(EtchContext*,const InlayImageLoaderDesc*);
INLAY_API void inlay_image_loader_destroy(InlayImageLoader*);
INLAY_API bool inlay_image_loader_request_file(InlayImageLoader*,EtchStr key,const char* path,bool srgb);
INLAY_API bool inlay_image_loader_request_memory(InlayImageLoader*,EtchStr key,const void* data,size_t size_bytes,bool srgb);
INLAY_API void inlay_image_loader_cancel(InlayImageLoader*,EtchStr key);
INLAY_API uint32_t inlay_image_loader_poll(InlayImageLoader*,uint32_t max_uploads);
INLAY_API InlayImageLoadStatus inlay_image_loader_status(const InlayImageLoader*,EtchStr key);
INLAY_API uint32_t inlay_image_loader_epoch(const InlayImageLoader*);
INLAY_API bool inlay_image_loader_resolve(EtchStr src,InlayImageResult* out,void* loader_user_data);

/* Windowless portal. Backends own returned pixel spans until the next callback;
   all callbacks run synchronously on the host UI thread. Context outlives portal.
   Backend callbacks must not destroy or reenter the portal. */
typedef struct InlayPortal InlayPortal;
typedef enum InlayPortalMode {INLAY_PORTAL_MODE_TEXTURE,INLAY_PORTAL_MODE_UNDERLAY} InlayPortalMode;
typedef struct InlayPortalDirtyRect {
 uint32_t x,y,width,height,row_pitch_bytes;
 const void* rgba_pixels;
} InlayPortalDirtyRect;
typedef struct InlayPortalBackendVTable {
 void* (*create)(void* user_data,uint32_t width,uint32_t height);
 void (*destroy)(void* backend);
 bool (*load_url)(void* backend,EtchStr url);
 bool (*load_html)(void* backend,EtchStr html);
 bool (*resize)(void* backend,uint32_t width,uint32_t height);
 uint32_t (*poll_dirty_rects)(void* backend,InlayPortalDirtyRect* rects,uint32_t capacity);
 void (*inject_pointer)(void* backend,EtchVec2 local_pos,bool down);
 void (*inject_wheel)(void* backend,EtchVec2 delta);
 void (*inject_key)(void* backend,uint32_t key,bool down,uint16_t mods);
 void (*inject_text)(void* backend,EtchStr text);
 bool (*query_ime_rect)(void* backend,EtchRect* out_local);
 void (*set_underlay_bounds)(void* backend,EtchRect bounds,EtchRect visible_bounds);
 /* Poll must retain/coalesce dirt until accepted=true; failed uploads retry it. */
 void (*ack_dirty_rects)(void* backend,bool accepted);
 /* Optional corner radius (logical px) for hardware-clipped underlay backends. */
 void (*set_underlay_radius)(void* backend,float radius_px);
} InlayPortalBackendVTable;
typedef struct InlayPortalDesc {
 uint32_t struct_size;
 uint32_t width,height;
 InlayPortalMode mode;
 const InlayPortalBackendVTable* backend; /* NULL selects the built-in mock. */
 void* backend_user_data;
} InlayPortalDesc;
typedef struct InlayPortalViewDesc {
 uint32_t struct_size;
 uint32_t id;
 float width,height,radius;
} InlayPortalViewDesc;
typedef struct InlayPortalStats {uint64_t uploaded_bytes;uint32_t uploaded_rects;bool has_texture;} InlayPortalStats;
INLAY_API InlayPortalDesc inlay_portal_desc_default(void);
INLAY_API InlayPortalViewDesc inlay_portal_view_desc_default(void);
INLAY_API InlayPortal* inlay_portal_create(EtchContext*,const InlayPortalDesc*);
/* resize/destroy fail while a view may still be queued. After submitting the
   host frame to the GPU (or discarding ALL its draws), acknowledge completion. */
INLAY_API bool inlay_portal_destroy(InlayPortal*);
INLAY_API void inlay_portal_frame_submitted(InlayPortal*);
INLAY_API bool inlay_portal_load_url(InlayPortal*,EtchStr);
INLAY_API bool inlay_portal_load_html(InlayPortal*,EtchStr);
INLAY_API bool inlay_portal_resize(InlayPortal*,uint32_t width,uint32_t height);
INLAY_API bool inlay_portal_poll(InlayPortal*);
INLAY_API void inlay_portal_view(InlayPortal*,const InlayPortalViewDesc*);
/* Call after end_frame to synchronize final bounds. Hidden views send empty bounds. */
INLAY_API bool inlay_portal_sync_geometry(InlayPortal*);
INLAY_API bool inlay_portal_inject_pointer(InlayPortal*,EtchVec2 screen_pos,bool down);
INLAY_API void inlay_portal_inject_wheel(InlayPortal*,EtchVec2);
INLAY_API void inlay_portal_inject_key(InlayPortal*,uint32_t key,bool down,uint16_t mods);
INLAY_API void inlay_portal_inject_text(InlayPortal*,EtchStr);
INLAY_API bool inlay_portal_query_ime_rect(InlayPortal*,EtchRect* out_screen);
INLAY_API InlayPortalStats inlay_portal_stats(const InlayPortal*);
INLAY_API const InlayPortalBackendVTable* inlay_portal_mock_backend(void);
/* Host policy: call after geometry sync. Disable routing while a modal, popup or
   another surface covers the portal; rectangle visibility cannot prove occlusion.
   Disabling releases capture. Keyboard/text/wheel require portal focus. */
INLAY_API void inlay_portal_set_input_enabled(InlayPortal*,bool enabled);
INLAY_API bool inlay_portal_has_focus(const InlayPortal*);
/* Deterministic mock animation clock. No wall-clock polling or idle animation. */
INLAY_API bool inlay_portal_mock_advance(InlayPortal*,uint32_t phase);

/* Portal navigation policy, origin allowlist, history & bidirectional host<->web message bridge. */
typedef enum InlayPortalNavDecision {
 INLAY_PORTAL_NAV_ALLOW = 0,
 INLAY_PORTAL_NAV_BLOCK = 1,
 INLAY_PORTAL_NAV_OPEN_EXTERNAL = 2
} InlayPortalNavDecision;
typedef InlayPortalNavDecision (*InlayPortalNavFn)(InlayPortal*,EtchStr url,bool is_new_window,void* user_data);
typedef void (*InlayPortalMessageFn)(InlayPortal*,EtchStr message_json,void* user_data);
typedef bool (*InlayPortalPermissionFn)(InlayPortal*,EtchStr origin,EtchStr permission_kind,void* user_data);
typedef struct InlayPortalPolicy {
 uint32_t struct_size;
 EtchStr allowed_origins_csv; /* Empty allows all origins; otherwise comma-separated scheme://host[:port]. */
 bool allow_external_links;
 bool allow_new_windows;
 InlayPortalNavFn on_navigate;
 InlayPortalMessageFn on_message;
 InlayPortalPermissionFn on_permission;
 void* user_data;
} InlayPortalPolicy;
INLAY_API InlayPortalPolicy inlay_portal_policy_default(void);
INLAY_API void inlay_portal_set_policy(InlayPortal*,const InlayPortalPolicy* policy);
INLAY_API bool inlay_portal_post_message(InlayPortal*,EtchStr message_json);
/* Returns front queued message byte length without consuming it (0 if empty). */
INLAY_API uint32_t inlay_portal_peek_message_size(const InlayPortal*);
/* Pops front message when cap > message_len; when out_buf==NULL or cap <= message_len,
   writes truncated NUL-terminated preview (if cap > 0), returns full required length, and
   keeps the message queued so callers never lose data on short buffers. */
INLAY_API uint32_t inlay_portal_poll_message(InlayPortal*,char* out_buf,uint32_t cap);
INLAY_API bool inlay_portal_go_back(InlayPortal*);
INLAY_API bool inlay_portal_go_forward(InlayPortal*);
INLAY_API bool inlay_portal_reload(InlayPortal*);
INLAY_API bool inlay_portal_can_go_back(const InlayPortal*);
INLAY_API bool inlay_portal_can_go_forward(const InlayPortal*);
INLAY_API EtchStr inlay_portal_current_url(const InlayPortal*);
INLAY_API bool inlay_portal_end_frame_sync(InlayPortal*,bool host_input_enabled);
/* Optional Windows native UNDERLAY backend. The host supplies a dedicated HWND
   BELOW its alpha-composited Etch surface, with the same client origin, and owns
   message pumping, native keyboard and IME dispatch. Create and destroy the
   backend on the window's STA thread; the backend balances its own COM calls.
   No OSR texture support. NULL on unsupported/disabled builds. Paths are UTF-8.
   Options and strings need only survive create; native window outlives backend. */
typedef struct InlayPortalWebView2Options {
 uint32_t struct_size;
 void* underlay_hwnd;
 const char* loader_path; /* NULL or empty searches for WebView2Loader.dll. */
 const char* user_data_path;
 void (*completed)(void* user_data,int32_t hresult); /* async; no callback after destroy */
 void* user_data;
 bool disable_dcomp; /* Force classic HWND controller + SetWindowRgn fallback. */
} InlayPortalWebView2Options;
INLAY_API InlayPortalWebView2Options inlay_portal_webview2_options_default(void);
INLAY_API const InlayPortalBackendVTable* inlay_portal_webview2_backend(void);
#ifdef __cplusplus
}
#endif
#endif
