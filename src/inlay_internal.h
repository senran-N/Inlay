#pragma once
#include <inlay/inlay_c.h>
struct InlayRow {
 uint32_t block,offset,length,table_header,table_columns;
 float estimate;
 bool first,zebra,separator,last_in_block;
 InlayBlockKind kind;
};
enum InlayInlineStyle : uint32_t {
 InlayInlineCode=1u<<0, InlayInlineLink=1u<<1, InlayInlineMath=1u<<2,
 InlayInlineBold=1u<<3, InlayInlineEmphasis=1u<<4, InlayInlineStrike=1u<<5,
 InlayInlineHighlight=1u<<6, InlayInlineFootnote=1u<<7, InlayInlineImage=1u<<8
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
struct InlayMeasure {
 InlayDoc* doc;
 uint32_t id;
 float width,font;
 InlayDocViewDesc view;
 InlayDocStyle style;
 double total_height;
 uint32_t revision;
 bool layout_settled;
 InlayRow* rows;
 float* heights;
 uint32_t count,capacity,frozen_count;
 InlayDetails* details;
 bool* open;
 bool* code_wrap;
 uint32_t open_capacity;
 bool doc_sel_active,doc_sel_dragging;
 uint32_t doc_sel_anchor,doc_sel_focus,doc_sel_drag_active_id,doc_sel_last_active;
 InlayInline* tokens;
 uint32_t token_count,token_capacity,parsed_tail;
};
struct InlayDoc {
 EtchContext* context;
 char* source;
 InlayBlock* blocks;
 uint32_t source_size,source_capacity,block_count,block_capacity,frozen_count,tail_start;
 uint32_t revision;
 InlayMeasure views[16];
 char utf8_carry[4];uint8_t utf8_carry_len;
};
void inlay_diagram_draw(const InlayDiagramResult& result,const InlayDiagramDesc& input);
