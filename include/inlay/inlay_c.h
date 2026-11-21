#ifndef INLAY_C_H
#define INLAY_C_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <etch/etch_c.h>
#define INLAY_VERSION_MAJOR 0
#define INLAY_VERSION_MINOR 11
#define INLAY_VERSION_PATCH 0
#define INLAY_STRING_DETAIL(x) #x
#define INLAY_STRING(x) INLAY_STRING_DETAIL(x)
#define INLAY_VERSION_STRING INLAY_STRING(INLAY_VERSION_MAJOR) "." INLAY_STRING(INLAY_VERSION_MINOR) "." INLAY_STRING(INLAY_VERSION_PATCH)
#define INLAY_ABI_VERSION 1u
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
 int32_t parent_index;       /* -1 for top-level blocks, or flat index of parent AST container */
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
 INLAY_INLINE_LINK=2
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
} InlayDocViewDesc;
typedef struct InlayDocResult {
 EtchStr clicked_link;
 uint32_t visible_first,visible_end,total_rows;
 float measured_height_px;
 bool at_bottom;
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
typedef struct InlayMathResult {float width,height,baseline;bool valid;} InlayMathResult;
INLAY_API InlayMathDesc inlay_math_desc_default(void);
INLAY_API InlayMathResult inlay_draw_math(EtchStr tex,const InlayMathDesc* desc);
INLAY_API InlayMathResult inlay_measure_math(EtchStr tex,float font_size_px);
typedef struct InlayDiagramDesc {
 uint32_t struct_size;
 EtchVec2 pos;
 float font_size_px,node_gap_px,rank_gap_px;
 uint32_t canvas_id;
 float viewport_width_px,viewport_height_px;
} InlayDiagramDesc;
typedef struct InlayDiagramNode {EtchStr id,label;EtchRect bounds;uint32_t rank;} InlayDiagramNode;
typedef struct InlayDiagramEdge {uint32_t from,to;bool reversed,dashed;EtchStr label;} InlayDiagramEdge;
/* Layout spans belong to the active Etch frame arena and expire at frame end. */
typedef struct InlayDiagramResult {const InlayDiagramNode* nodes;const InlayDiagramEdge* edges;uint32_t node_count,edge_count;float width,height;bool valid,horizontal,sequence;} InlayDiagramResult;
INLAY_API InlayDiagramDesc inlay_diagram_desc_default(void);
INLAY_API InlayDiagramResult inlay_diagram_layout(EtchStr source,const InlayDiagramDesc* desc);
INLAY_API InlayDiagramResult inlay_diagram_view(EtchStr source,const InlayDiagramDesc* desc);
INLAY_API InlayDocConfig inlay_doc_config_default(void);
INLAY_API InlayDocStyle inlay_doc_style_default(void);
INLAY_API InlayDocViewDesc inlay_doc_view_desc_default(void);
INLAY_API InlayDoc* inlay_doc_create(EtchContext*,const InlayDocConfig*);
INLAY_API void inlay_doc_destroy(InlayDoc*);
INLAY_API void inlay_doc_clear(InlayDoc*);
INLAY_API bool inlay_doc_set_utf8(InlayDoc*,EtchStr);
INLAY_API bool inlay_doc_append_utf8(InlayDoc*,EtchStr);
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
INLAY_API void inlay_doc_view_set_selection(InlayDoc* doc,uint32_t view_id,uint32_t anchor,uint32_t focus);
INLAY_API void inlay_doc_view_select_all(InlayDoc* doc,uint32_t view_id);
INLAY_API void inlay_doc_view_clear_selection(InlayDoc* doc,uint32_t view_id);
/* Copies selected clean plain text into out_buf (NUL-terminated if cap > 0). Returns required length excluding NUL. */
INLAY_API uint32_t inlay_doc_view_copy_plain_text(const InlayDoc* doc,uint32_t view_id,char* out_buf,uint32_t cap);
/* Copies selected range as balanced Markdown into out_buf (NUL-terminated if cap > 0). Returns required length excluding NUL. */
INLAY_API uint32_t inlay_doc_view_copy_markdown(const InlayDoc* doc,uint32_t view_id,char* out_buf,uint32_t cap);

typedef struct InlaySurface InlaySurface;
typedef struct InlaySurfaceDesc {
 uint32_t struct_size;
 size_t memory_limit_bytes,max_stack_bytes;
 float max_tick_ms;
 uint32_t max_nodes,max_text_bytes;
} InlaySurfaceDesc;
typedef enum InlayUiKind {
 INLAY_UI_BOX, INLAY_UI_TEXT, INLAY_UI_BUTTON, INLAY_UI_INPUT, INLAY_UI_TOGGLE,
 INLAY_UI_SCROLL, INLAY_UI_PROGRESS, INLAY_UI_SLIDER, INLAY_UI_BADGE, INLAY_UI_DIVIDER, INLAY_UI_IMAGE
} InlayUiKind;
typedef struct InlayUiNode {InlayUiKind kind;uint32_t id,first_child,next_sibling;EtchStr text;} InlayUiNode;
typedef struct InlaySurfaceViewDesc {
 uint32_t struct_size;
 uint32_t id;
 float width_px,height_px;
} InlaySurfaceViewDesc;
/* last_reload_ms measures engine reload; total includes filesystem read latency. */
typedef struct InlaySurfaceStats {uint64_t js_ticks,reloads;uint32_t node_count;double last_tick_ms,last_reload_ms,last_reload_total_ms;bool has_error;} InlaySurfaceStats;
/* Callbacks may set state, but must not destroy or recursively dispatch the surface.
   The host calls poll_hmr periodically while idle; timers advance when view is built. */
typedef void (*InlayActionFn)(InlaySurface*,EtchStr payload_json,void* user_data);
INLAY_API InlaySurfaceDesc inlay_surface_desc_default(void);
INLAY_API InlaySurfaceViewDesc inlay_surface_view_desc_default(void);
INLAY_API InlaySurface* inlay_surface_create(EtchContext*,const InlaySurfaceDesc*);
INLAY_API void inlay_surface_destroy(InlaySurface*);
INLAY_API bool inlay_surface_set_script(InlaySurface*,EtchStr script);
INLAY_API bool inlay_surface_set_state_json(InlaySurface*,EtchStr json);
INLAY_API EtchStr inlay_surface_state_json(const InlaySurface*);
INLAY_API bool inlay_surface_bind_action(InlaySurface*,EtchStr name,InlayActionFn,void* user_data);
INLAY_API bool inlay_surface_dispatch(InlaySurface*,EtchStr name,EtchStr payload_json);
INLAY_API bool inlay_surface_enable_hmr(InlaySurface*,const char* script_path);
INLAY_API bool inlay_surface_poll_hmr(InlaySurface*);
INLAY_API void inlay_surface_view(InlaySurface*,const InlaySurfaceViewDesc*);
/* Milliseconds until the next scheduled JS timer expires, 0 if due now, or -1 if none. */
INLAY_API int32_t inlay_surface_next_timer_ms(const InlaySurface*);
INLAY_API InlaySurfaceStats inlay_surface_stats(const InlaySurface*);
INLAY_API EtchStr inlay_surface_error(const InlaySurface*);
/* Native nodes/text survive until the next script/state/event evaluation. */
INLAY_API const InlayUiNode* inlay_surface_nodes(const InlaySurface*,uint32_t* count);
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
