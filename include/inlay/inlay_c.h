#ifndef INLAY_C_H
#define INLAY_C_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <etch/etch_c.h>
#define INLAY_VERSION_MAJOR 0
#define INLAY_VERSION_MINOR 3
#define INLAY_VERSION_PATCH 0
#define INLAY_STRING_DETAIL(x) #x
#define INLAY_STRING(x) INLAY_STRING_DETAIL(x)
#define INLAY_VERSION_STRING INLAY_STRING(INLAY_VERSION_MAJOR) "." INLAY_STRING(INLAY_VERSION_MINOR) "." INLAY_STRING(INLAY_VERSION_PATCH)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct InlayDoc InlayDoc;
typedef enum InlayBlockKind {
 INLAY_PARAGRAPH, INLAY_HEADING, INLAY_CODE_BLOCK, INLAY_BLOCKQUOTE,
 INLAY_ALERT, INLAY_LIST, INLAY_TASK_LIST, INLAY_TABLE, INLAY_THEMATIC_BREAK, INLAY_DETAILS, INLAY_MATH_BLOCK
} InlayBlockKind;
typedef struct InlayBlock {
 InlayBlockKind kind;
 uint32_t offset, length, content_offset, content_length, level;
 uint32_t language_offset, language_length;
 bool complete;
} InlayBlock;
typedef struct InlayDocConfig {uint32_t initial_source_bytes, initial_blocks;} InlayDocConfig;
typedef struct InlayDocViewDesc {uint32_t id;float width_px,height_px,font_size_px;} InlayDocViewDesc;
typedef struct InlayDocResult {EtchStr clicked_link;uint32_t visible_first,visible_end,total_rows;} InlayDocResult;
typedef struct InlayMathDesc {EtchVec2 pos;float font_size_px;EtchColor color;} InlayMathDesc;
typedef struct InlayMathResult {float width,height,baseline;bool valid;} InlayMathResult;
InlayMathDesc inlay_math_desc_default(void);
InlayMathResult inlay_draw_math(EtchStr tex,const InlayMathDesc* desc);
InlayMathResult inlay_measure_math(EtchStr tex,float font_size_px);
typedef struct InlayDiagramDesc {EtchVec2 pos;float font_size_px,node_gap_px,rank_gap_px;uint32_t canvas_id;float viewport_width_px,viewport_height_px;} InlayDiagramDesc;
typedef struct InlayDiagramNode {EtchStr id,label;EtchRect bounds;uint32_t rank;} InlayDiagramNode;
typedef struct InlayDiagramEdge {uint32_t from,to;bool reversed;EtchStr label;} InlayDiagramEdge;
/* Layout spans belong to the active Etch frame arena and expire at frame end. */
typedef struct InlayDiagramResult {const InlayDiagramNode* nodes;const InlayDiagramEdge* edges;uint32_t node_count,edge_count;float width,height;bool valid,horizontal,sequence;} InlayDiagramResult;
InlayDiagramDesc inlay_diagram_desc_default(void);
InlayDiagramResult inlay_diagram_layout(EtchStr source,const InlayDiagramDesc* desc);
InlayDiagramResult inlay_diagram_view(EtchStr source,const InlayDiagramDesc* desc);
InlayDocConfig inlay_doc_config_default(void);
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

typedef struct InlaySurface InlaySurface;
typedef struct InlaySurfaceDesc {size_t memory_limit_bytes,max_stack_bytes;float max_tick_ms;uint32_t max_nodes,max_text_bytes;} InlaySurfaceDesc;
typedef enum InlayUiKind {INLAY_UI_BOX,INLAY_UI_TEXT,INLAY_UI_BUTTON} InlayUiKind;
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
InlaySurfaceStats inlay_surface_stats(const InlaySurface*);
EtchStr inlay_surface_error(const InlaySurface*);
/* Native nodes/text survive until the next script/state/event evaluation. */
const InlayUiNode* inlay_surface_nodes(const InlaySurface*,uint32_t* count);
#ifdef __cplusplus
}
#endif
#endif
