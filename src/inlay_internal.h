#pragma once
#include <inlay/inlay_c.h>
struct InlayRow {uint32_t block,offset,length;bool first;InlayBlockKind kind=INLAY_PARAGRAPH;};
struct InlayInline {uint32_t offset,length,url_offset,url_length;uint8_t kind;};
struct InlayDetails {uint32_t summary,summary_end,body,end;};
struct InlayMeasure {InlayDoc* doc;uint32_t id;float width,font;uint32_t revision;InlayRow* rows;float* heights;uint32_t count,capacity,frozen_count;InlayDetails* details;bool* open;uint32_t open_capacity;InlayInline* tokens;uint32_t token_count,token_capacity,parsed_tail;};
struct InlayDoc {
 EtchContext* context;
 char* source;
 InlayBlock* blocks;
 uint32_t source_size,source_capacity,block_count,block_capacity,frozen_count,tail_start;
 uint32_t revision;
 InlayMeasure views[16];
 char utf8_carry[4];uint8_t utf8_carry_len;
};
