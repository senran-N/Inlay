#ifndef INLAY_C_H
#define INLAY_C_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <etch/etch_c.h>
#define INLAY_VERSION_MAJOR 0
#define INLAY_VERSION_MINOR 10
#define INLAY_VERSION_PATCH 1
#define INLAY_STRING_DETAIL(x) #x
#define INLAY_STRING(x) INLAY_STRING_DETAIL(x)
#define INLAY_VERSION_STRING INLAY_STRING(INLAY_VERSION_MAJOR) "." INLAY_STRING(INLAY_VERSION_MINOR) "." INLAY_STRING(INLAY_VERSION_PATCH)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct InlayDoc InlayDoc;
typedef enum InlayBlockKind {
 INLAY_PARAGRAPH, INLAY_HEADING, INLAY_CODE_BLOCK, INLAY_BLOCKQUOTE,
 INLAY_ALERT, INLAY_LIST, INLAY_TASK_LIST, INLAY_TABLE, INLAY_THEMATIC_BREAK, INLAY_DETAILS, INLAY_MATH_BLOCK,
 INLAY_IMAGE, INLAY_FOOTNOTE
} InlayBlockKind;
typedef struct InlayBlock {
 InlayBlockKind kind;
 uint32_t offset, length, content_offset, content_length, level;
 uint32_t language_offset, language_length;
 bool complete;
} InlayBlock;
typedef struct InlayDocConfig {uint32_t initial_source_bytes, initial_blocks;} InlayDocConfig;
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
typedef struct InlayMathDesc {EtchVec2 pos;float font_size_px;EtchColor color;} InlayMathDesc;
typedef struct InlayMathResult {float width,height,baseline;bool valid;} InlayMathResult;
InlayMathDesc inlay_math_desc_default(void);
InlayMathResult inlay_draw_math(EtchStr tex,const InlayMathDesc* desc);
InlayMathResult inlay_measure_math(EtchStr tex,float font_size_px);
typedef struct InlayDiagramDesc {EtchVec2 pos;float font_size_px,node_gap_px,rank_gap_px;uint32_t canvas_id;float viewport_width_px,viewport_height_px;} InlayDiagramDesc;
typedef struct InlayDiagramNode {EtchStr id,label;EtchRect bounds;uint32_t rank;} InlayDiagramNode;
typedef struct InlayDiagramEdge {uint32_t from,to;bool reversed,dashed;EtchStr label;} InlayDiagramEdge;
/* Layout spans belong to the active Etch frame arena and expire at frame end. */
typedef struct InlayDiagramResult {const InlayDiagramNode* nodes;const InlayDiagramEdge* edges;uint32_t node_count,edge_count;float width,height;bool valid,horizontal,sequence;} InlayDiagramResult;
InlayDiagramDesc inlay_diagram_desc_default(void);
InlayDiagramResult inlay_diagram_layout(EtchStr source,const InlayDiagramDesc* desc);
InlayDiagramResult inlay_diagram_view(EtchStr source,const InlayDiagramDesc* desc);
InlayDocConfig inlay_doc_config_default(void);
InlayDocStyle inlay_doc_style_default(void);
InlayDocViewDesc inlay_doc_view_desc_default(void);
InlayDoc* inlay_doc_create(EtchContext*,const InlayDocConfig*);
void inlay_doc_destroy(InlayDoc*);
void inlay_doc_clear(InlayDoc*);
bool inlay_doc_set_utf8(InlayDoc*,EtchStr);
bool inlay_doc_append_utf8(InlayDoc*,EtchStr);
uint32_t inlay_doc_block_count(const InlayDoc*);
const InlayBlock* inlay_doc_block(const InlayDoc*,uint32_t index);
EtchStr inlay_doc_source(const InlayDoc*);
InlayDocResult inlay_doc_view(InlayDoc*,const InlayDocViewDesc*);
/* Total height in logical pixels. Uses matching view measurements or estimates
   with default flow spacing. May be called outside a frame. */
float inlay_doc_measure_height(InlayDoc*,uint32_t view_id,float width_px,float font_size_px);
bool inlay_doc_view_is_at_bottom(uint32_t view_id);
void inlay_doc_view_scroll_to_bottom(uint32_t view_id,bool stop_momentum);

typedef struct InlaySurface InlaySurface;
typedef struct InlaySurfaceDesc {size_t memory_limit_bytes,max_stack_bytes;float max_tick_ms;uint32_t max_nodes,max_text_bytes;} InlaySurfaceDesc;
typedef enum InlayUiKind {
 INLAY_UI_BOX, INLAY_UI_TEXT, INLAY_UI_BUTTON, INLAY_UI_INPUT, INLAY_UI_TOGGLE,
 INLAY_UI_SCROLL, INLAY_UI_PROGRESS, INLAY_UI_SLIDER, INLAY_UI_BADGE, INLAY_UI_DIVIDER, INLAY_UI_IMAGE
} InlayUiKind;
typedef struct InlayUiNode {InlayUiKind kind;uint32_t id,first_child,next_sibling;EtchStr text;} InlayUiNode;
typedef struct InlaySurfaceViewDesc {uint32_t id;float width_px,height_px;} InlaySurfaceViewDesc;
/* last_reload_ms measures engine reload; total includes filesystem read latency. */
typedef struct InlaySurfaceStats {uint64_t js_ticks,reloads;uint32_t node_count;double last_tick_ms,last_reload_ms,last_reload_total_ms;bool has_error;} InlaySurfaceStats;
/* Callbacks may set state, but must not destroy or recursively dispatch the surface.
   The host calls poll_hmr periodically while idle; timers advance when view is built. */
typedef void (*InlayActionFn)(InlaySurface*,EtchStr payload_json,void* user_data);
InlaySurfaceDesc inlay_surface_desc_default(void);
InlaySurfaceViewDesc inlay_surface_view_desc_default(void);
InlaySurface* inlay_surface_create(EtchContext*,const InlaySurfaceDesc*);
void inlay_surface_destroy(InlaySurface*);
bool inlay_surface_set_script(InlaySurface*,EtchStr script);
bool inlay_surface_set_state_json(InlaySurface*,EtchStr json);
EtchStr inlay_surface_state_json(const InlaySurface*);
bool inlay_surface_bind_action(InlaySurface*,EtchStr name,InlayActionFn,void* user_data);
bool inlay_surface_dispatch(InlaySurface*,EtchStr name,EtchStr payload_json);
bool inlay_surface_enable_hmr(InlaySurface*,const char* script_path);
bool inlay_surface_poll_hmr(InlaySurface*);
void inlay_surface_view(InlaySurface*,const InlaySurfaceViewDesc*);
/* Milliseconds until the next scheduled JS timer expires, 0 if due now, or -1 if none. */
int32_t inlay_surface_next_timer_ms(const InlaySurface*);
InlaySurfaceStats inlay_surface_stats(const InlaySurface*);
EtchStr inlay_surface_error(const InlaySurface*);
/* Native nodes/text survive until the next script/state/event evaluation. */
const InlayUiNode* inlay_surface_nodes(const InlaySurface*,uint32_t* count);
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
} InlayPortalBackendVTable;
typedef struct InlayPortalDesc {
 uint32_t width,height;
 InlayPortalMode mode;
 const InlayPortalBackendVTable* backend; /* NULL selects the built-in mock. */
 void* backend_user_data;
} InlayPortalDesc;
typedef struct InlayPortalViewDesc {uint32_t id;float width,height,radius;} InlayPortalViewDesc;
typedef struct InlayPortalStats {uint64_t uploaded_bytes;uint32_t uploaded_rects;bool has_texture;} InlayPortalStats;
InlayPortalDesc inlay_portal_desc_default(void);
InlayPortal* inlay_portal_create(EtchContext*,const InlayPortalDesc*);
/* resize/destroy fail while a view may still be queued. After submitting the
   host frame to the GPU (or discarding ALL its draws), acknowledge completion. */
bool inlay_portal_destroy(InlayPortal*);
void inlay_portal_frame_submitted(InlayPortal*);
bool inlay_portal_load_url(InlayPortal*,EtchStr);
bool inlay_portal_load_html(InlayPortal*,EtchStr);
bool inlay_portal_resize(InlayPortal*,uint32_t width,uint32_t height);
bool inlay_portal_poll(InlayPortal*);
void inlay_portal_view(InlayPortal*,const InlayPortalViewDesc*);
/* Call after end_frame to synchronize final bounds. Hidden views send empty bounds. */
bool inlay_portal_sync_geometry(InlayPortal*);
bool inlay_portal_inject_pointer(InlayPortal*,EtchVec2 screen_pos,bool down);
void inlay_portal_inject_wheel(InlayPortal*,EtchVec2);
void inlay_portal_inject_key(InlayPortal*,uint32_t key,bool down,uint16_t mods);
void inlay_portal_inject_text(InlayPortal*,EtchStr);
bool inlay_portal_query_ime_rect(InlayPortal*,EtchRect* out_screen);
InlayPortalStats inlay_portal_stats(const InlayPortal*);
const InlayPortalBackendVTable* inlay_portal_mock_backend(void);
/* Host policy: call after geometry sync. Disable routing while a modal, popup or
   another surface covers the portal; rectangle visibility cannot prove occlusion.
   Disabling releases capture. Keyboard/text/wheel require portal focus. */
void inlay_portal_set_input_enabled(InlayPortal*,bool enabled);
bool inlay_portal_has_focus(const InlayPortal*);
/* Deterministic mock animation clock. No wall-clock polling or idle animation. */
bool inlay_portal_mock_advance(InlayPortal*,uint32_t phase);
/* Optional Windows native UNDERLAY backend. The host supplies a dedicated HWND
   BELOW its alpha-composited Etch surface, with the same client origin, and owns
   message pumping, native keyboard and IME dispatch. Create and destroy the
   backend on the window's STA thread; the backend balances its own COM calls.
   No OSR texture support. NULL on unsupported/disabled builds. Paths are UTF-8.
   Options and strings need only survive create; native window outlives backend. */
typedef struct InlayPortalWebView2Options {
 void* underlay_hwnd;
 const char* loader_path; /* NULL or empty searches for WebView2Loader.dll. */
 const char* user_data_path;
void (*completed)(void* user_data,int32_t hresult); /* async; no callback after destroy */
void* user_data;
} InlayPortalWebView2Options;
const InlayPortalBackendVTable* inlay_portal_webview2_backend(void);
#ifdef __cplusplus
}
#endif
#endif
