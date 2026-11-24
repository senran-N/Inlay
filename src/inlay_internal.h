#pragma once
#include <inlay/inlay_c.h>
#include <SDL3/SDL_stdinc.h>
#include <cstdint>
#include <cstring>

inline bool inlay_is_dark_theme(const EtchTheme& theme) noexcept {
 return (uint32_t(theme.bg_canvas.r) * 299u + uint32_t(theme.bg_canvas.g) * 587u + uint32_t(theme.bg_canvas.b) * 114u) < 128000u;
}

template <typename T>
inline bool inlay_grow_pod(T*& ptr, uint32_t& cap, uint32_t needed,
                           uint32_t init_cap, uint32_t hard_cap) noexcept {
 if (needed <= cap) return true;
 if (needed > hard_cap) return false;
 uint32_t next = cap ? cap : (init_cap ? init_cap : 16u);
 if (next > hard_cap) next = hard_cap;
 while (next < needed) {
  next = (next > hard_cap / 2u) ? hard_cap : (next * 2u);
 }
 if (size_t(next) > SIZE_MAX / sizeof(T)) return false;
 void* mem = SDL_realloc(ptr, size_t(next) * sizeof(T));
 if (!mem) return false;
 ptr = static_cast<T*>(mem);
 cap = next;
 return true;
}

inline uint32_t inlay_fnv1a_32(const void* data, size_t len,
                               uint32_t seed = 2166136261u) noexcept {
 uint32_t h = seed;
 const auto* p = static_cast<const uint8_t*>(data);
 for (size_t i = 0; i < len; ++i)
  h = (h ^ uint32_t(p[i])) * 16777619u;
 return h;
}

inline uint64_t inlay_fnv1a_64(const void* data, size_t len,
                               uint64_t seed = 14695981039346656037ull) noexcept {
 uint64_t h = seed;
 const auto* p = static_cast<const uint8_t*>(data);
 for (size_t i = 0; i < len; ++i)
  h = (h ^ uint64_t(p[i])) * 1099511628211ull;
 return h;
}

inline uint32_t inlay_utf8_next(const char* s, size_t len, size_t* io_pos) noexcept {
 const size_t pos = *io_pos;
 if (pos >= len) return 0u;
 const uint8_t b0 = static_cast<uint8_t>(s[pos]);
 if (b0 < 0x80u) {
  *io_pos = pos + 1u;
  return b0;
 }
 uint32_t n = 0u, cp = 0u, min_cp = 0u;
 if (b0 >= 0xC2u && b0 <= 0xDFu) {
  n = 2u; cp = b0 & 0x1Fu; min_cp = 0x80u;
 } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
  n = 3u; cp = b0 & 0x0Fu; min_cp = 0x800u;
 } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
  n = 4u; cp = b0 & 0x07u; min_cp = 0x10000u;
 } else {
  *io_pos = pos + 1u;
  return 0xFFFDu;
 }
 if (pos + n > len) {
  *io_pos = pos + 1u;
  return 0xFFFDu;
 }
 for (uint32_t i = 1u; i < n; ++i) {
  const uint8_t bx = static_cast<uint8_t>(s[pos + i]);
  if ((bx & 0xC0u) != 0x80u) {
   *io_pos = pos + 1u;
   return 0xFFFDu;
  }
  cp = (cp << 6) | uint32_t(bx & 0x3Fu);
 }
 if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
  *io_pos = pos + 1u;
  return 0xFFFDu;
 }
 *io_pos = pos + n;
 return cp;
}

template <typename T>
inline bool inlay_read_abi_struct(const T* input, T* out) noexcept {
 if(!input || !out)return false;
 const uint32_t sz=input->struct_size;
 if(sz==0u || sz==uint32_t(sizeof(T))) {
  *out=*input;
  out->struct_size=uint32_t(sizeof(T));
  return true;
 }
 if(sz<uint32_t(sizeof(uint32_t)) || sz>uint32_t(sizeof(T)))return false;
 std::memcpy(out,input,sz);
 out->struct_size=uint32_t(sizeof(T));
 return true;
}

enum InlayAstKind : uint8_t {
 InlayAstDoc=0,
 InlayAstQuote,
 InlayAstAlert,
 InlayAstUl,
 InlayAstOl,
 InlayAstLi,
 InlayAstHeading,
 InlayAstParagraph,
 InlayAstCode,
 InlayAstTable,
 InlayAstThematicBreak,
 InlayAstDetails,
 InlayAstMathBlock,
 InlayAstImage,
 InlayAstFootnote
};

struct InlayAstNode {
 InlayAstKind kind;
 uint8_t alert_kind;
 uint8_t flags;
 uint8_t pad;
 int32_t parent;
 int32_t first_child;
 int32_t next_sibling;
 uint16_t depth;
 uint16_t quote_depth;
 uint16_t list_depth;
 uint16_t level;
 uint32_t ordinal;
 uint32_t block_index;
 uint32_t src_offset,src_length;
 uint32_t task_mark_offset;
};

struct InlayListItemMeta {
 uint32_t line_offset,line_length;
 uint32_t content_offset,content_length;
 uint32_t ordinal;
 uint32_t task_mark_offset;
 int32_t ast_index;
 uint16_t list_depth;
 uint8_t ordered,is_task,checked,pad;
};

struct InlayRefDef {
 uint32_t norm_label_off,norm_label_len; /* in doc->ast_text */
 uint32_t url_off,url_len;               /* in doc->source unless url_in_pool */
 bool url_in_pool;
};

struct InlayBlockMeta {
 int32_t ast_index;
 uint32_t list_item_first,list_item_count;
 uint32_t pool_offset,pool_length;       /* in doc->ast_text when INLAY_BLOCK_FLAG_SYNTHETIC */
};

struct InlayRow {
 uint32_t block,offset,length,table_header,table_columns;
 uint32_t code_start_line,code_total_lines;
 uint32_t doc_text_offset,doc_text_length;
 uint32_t list_item_index;               /* UINT32_MAX if none */
 float estimate,last_scroll_x,code_min_width;
 uint8_t code_lex_state;
 bool first,zebra,separator,last_in_block,is_custom_fence;
 InlayBlockKind kind;
};

enum InlayInlineStyle : uint32_t {
 InlayInlineCode=1u<<0, InlayInlineLink=1u<<1, InlayInlineMath=1u<<2,
 InlayInlineBold=1u<<3, InlayInlineEmphasis=1u<<4, InlayInlineStrike=1u<<5,
 InlayInlineHighlight=1u<<6, InlayInlineFootnote=1u<<7, InlayInlineImage=1u<<8,
 InlayInlineUrlInPool=1u<<9
};

struct InlayInline {uint32_t offset,length,url_offset,url_length,flags;};

inline bool inlay_table_rule(const char* text,uint32_t length) noexcept {
 if(length && text[length-1]=='\n')--length;
 uint32_t dashes=0;bool pipe=false;
 for(uint32_t i=0;i<length;++i) {
  const char c=text[i];
  if(c=='-')++dashes;
  else if(c=='|')pipe=true;
  else if(c!=':' && c!=' ' && c!='\t' && c!='\r')return false;
 }
 return pipe && dashes>=3;
}

struct InlayDetails {uint32_t summary,summary_end,body,end;};
struct InlayTableCols {uint32_t header_row,columns;float total_width;float widths[32];};

struct InlaySelSegment {
 uint32_t element_id;
 uint32_t row_index;
 uint32_t block_index;
 uint32_t doc_start,doc_len;
 uint16_t font_id;
 uint16_t pad;
 float font_size;
 float line_height;
 bool wrap;
};

struct InlayMeasure {
 InlayDoc* doc;
 uint32_t id;
 float width,font;
 InlayDocViewDesc view;
 InlayDocStyle style;
 double total_height;
 uint32_t revision;
 bool layout_settled;
 bool reveal_pending;
 uint32_t reveal_revision,reveal_offset;
 EtchAlignY reveal_align;
 InlayRow* rows;
 float* heights;
 uint32_t count,capacity,frozen_count;
 InlayDetails* details;
 bool* open;
 bool* code_wrap;
 float* code_scroll_x;
 bool* diagram_full_scale;
 uint64_t* copied_until_ms;
 uint32_t open_capacity;
 InlayTableCols* table_cols;
 uint32_t table_cols_count,table_cols_capacity;
 char* plain_text;
 uint32_t plain_len,plain_cap;
 InlaySelSegment* vis_segs;
 uint32_t vis_seg_count,vis_seg_cap;
 uint32_t* vis_row_ids;
 uint32_t vis_row_count,vis_row_cap;
 uint32_t* vis_ctrl_ids;
 uint32_t vis_ctrl_count,vis_ctrl_cap;
 uint32_t* vis_span_ids;
 uint32_t vis_span_count,vis_span_cap;
 bool doc_sel_active,doc_sel_dragging,doc_sel_pending_span,doc_sel_sync_needed;
 uint32_t doc_sel_anchor,doc_sel_focus,doc_sel_drag_active_id,doc_sel_last_active;
 EtchVec2 doc_sel_press_pos;
 InlayInline* tokens;
 uint32_t token_count,token_capacity,parsed_tail;
};

struct InlayDoc {
 EtchContext* context;
 char* source;
 InlayBlock* blocks;
 InlayBlockMeta* block_meta;
 uint32_t source_size,source_capacity,block_count,block_capacity,frozen_count,tail_start;
 InlayAstNode* ast_nodes;
 uint32_t ast_node_count,ast_node_capacity,frozen_ast_count;
 InlayListItemMeta* list_items;
 uint32_t list_item_count,list_item_capacity,frozen_list_item_count;
 char* ast_text;
 uint32_t ast_text_size,ast_text_capacity,frozen_ast_text_size;
 InlayRefDef* ref_defs;
 uint32_t ref_def_count,ref_def_capacity;
 bool has_ref_defs;
 bool parse_pending;
 uint32_t parse_from;
 uint32_t revision;
 InlayDocAnchor* anchors;
 uint32_t anchor_count,anchor_capacity;
 InlayMeasure* views;
 uint32_t view_count,view_capacity;
 char utf8_carry[4];uint8_t utf8_carry_len;
 struct InlayDiagramCache* diagram_cache;
};

void inlay_doc_free_diagram_cache(InlayDoc* doc) noexcept;

const InlayRefDef* inlay_doc_find_ref_def(const InlayDoc* doc,const char* label,uint32_t len) noexcept;
void inlay_doc_parse_pending(InlayDoc* doc) noexcept;

enum InlayDiagramShape : uint8_t {
 InlayShapeRect=0, InlayShapeRound=1, InlayShapeDiamond=2, InlayShapeCircle=3
};
enum InlayDiagramEdgeStyle : uint8_t {
 InlayEdgeStyleNone=0u,
 InlayEdgeStyleNoArrow=1u<<0,
 InlayEdgeStyleThick=1u<<1,
 InlayEdgeStyleBidir=1u<<2
};
struct InlayDiagramSubgraph {
 EtchStr title;
 EtchRect bounds;
 int32_t parent;
 bool has_nodes;
};
struct InlayDiagramEdgePath {
 uint32_t point_offset,point_count;
 int32_t feedback_lane;
};
struct InlayDiagramMeta {
 const InlayDiagramShape* shapes;
 const uint8_t* edge_styles;
 const InlayDiagramSubgraph* subgraphs;
 uint32_t subgraph_count;
 const InlayDiagramEdgePath* edge_paths;
 const EtchVec2* waypoints;
 uint32_t waypoint_count;
 bool reversed_dir;
};
struct InlayMathPrepared {
 const void* root;
 InlayMathResult metrics;
};
bool inlay_math_prepare(EtchStr tex,float font_size_px,bool display_style,InlayMathPrepared* out) noexcept;
void inlay_math_draw_prepared(const InlayMathPrepared& prep,EtchVec2 pos,EtchColor color) noexcept;
InlayDiagramResult inlay_diagram_layout_ex(EtchStr source,const InlayDiagramDesc* desc,InlayDiagramMeta* out_meta) noexcept;
void inlay_diagram_draw_ex(const InlayDiagramResult& result,const InlayDiagramDesc& input,const InlayDiagramMeta* meta) noexcept;
void inlay_diagram_draw(const InlayDiagramResult& result,const InlayDiagramDesc& input);

struct InlayDCompClipGeom {
 float offset_x,offset_y;
 float width,height;
 float left,top,right,bottom;
 float tl_r,tr_r,br_r,bl_r;
 bool visible,clipped,need_clip;
};
InlayDCompClipGeom inlay_dcomp_compute_clip(EtchRect bounds,EtchRect visible_bounds,float radius_logical,float dpi_scale) noexcept;
