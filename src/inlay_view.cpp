#include "inlay_internal.h"
#include <cstring>
#include <cmath>
#include <cstdlib>
namespace {

void details_range(EtchStr source,const InlayBlock& b,uint32_t* summary,uint32_t* summary_end,uint32_t* body,uint32_t* end) {
 *end=b.content_offset+b.content_length;*summary=b.content_offset;
 while(*summary<*end && !(*summary+9<=*end && !std::memcmp(source.chars+*summary,"<summary>",9)))++*summary;
 if(*summary<*end)*summary+=9;
 *summary_end=*summary;
 while(*summary_end<*end && source.chars[*summary_end]!='<')++*summary_end;
 *body=*summary_end;while(*body<*end && source.chars[*body]!='\n')++*body;
 if(*body<*end)++*body;
 for(uint32_t pos=*body;pos+10<=*end;++pos)if(!std::memcmp(source.chars+pos,"</details>",10)) {*end=pos;break;}
}
float block_height(uint32_t index,void* user) {
 const auto& m=*static_cast<InlayMeasure*>(user);
 // Etch can request a row between a document mutation and the next view build.
 if(m.revision!=m.doc->revision || index>=m.count)return 28;
 auto b=*inlay_doc_block(m.doc,m.rows[index].block);
 if(m.heights[index]>0)return m.heights[index];
 b.content_offset=m.rows[index].offset;b.content_length=m.rows[index].length;
 const EtchStr source=inlay_doc_source(m.doc);
 if(b.kind==INLAY_DETAILS) {
  if(!m.open[m.rows[index].block])return m.rows[index].first ? 56.f : 1.f;
  const auto& range=m.details[m.rows[index].block];const uint32_t body=range.body,end=range.end;
  const uint32_t start=b.content_offset>body ? b.content_offset : body;
  const uint32_t stop=b.content_offset+b.content_length<end ? b.content_offset+b.content_length : end;
  if(stop<=start)return m.rows[index].first ? 56.f : 1.f;
  b.content_offset=start;b.content_length=stop-start;
 }
 if(b.kind==INLAY_THEMATIC_BREAK)return 16;
 const float font=b.kind==INLAY_HEADING ? m.font+float(7-b.level)*2 : m.font;
 const float available=std::fmax(m.width-32,32.f);
 float height=0;uint32_t pos=b.content_offset,end=pos+b.content_length;
 while(pos<end) {
  uint32_t next=pos;while(next<end && source.chars[next]!='\n')++next;
  const EtchVec2 size=etch_measure_text({source.chars+pos,next-pos,true},uint16_t(b.kind==INLAY_CODE_BLOCK ? ETCH_FONT_MONO : ETCH_FONT_SANS),font,0);
  height+=(b.kind==INLAY_CODE_BLOCK ? 1.f : std::fmax(1.f,std::ceil(size.x/available)))*(font*1.5f)+4+(b.kind==INLAY_TABLE ? 8 : 0);
  pos=next<end ? next+1 : end;
 }
 return std::fmax(height,font*1.5f)+24+(b.kind==INLAY_CODE_BLOCK ? 64 : 0);
}
EtchStr slice(EtchStr source,uint32_t start,uint32_t n) noexcept {return {source.chars+start,n,false};}
bool cache_inline(InlayMeasure* m,EtchStr source) {
 const uint32_t tail=m->width==0 ? 0 : m->parsed_tail;
 while(m->token_count && m->tokens[m->token_count-1].offset>=tail)--m->token_count;
 uint32_t pos=tail;
 while(pos<source.length) {
  uint32_t end=pos;while(end<source.length && source.chars[end]!='\n')++end;
  uint32_t bracket=pos,paren=pos;bool no_code=false,no_bold=false;
  while(pos<end) {
   const uint32_t start=pos;InlayInline token={pos,1,0,0,0};
   if(source.chars[pos]=='`' && !no_code) {
    uint32_t close=pos+1;while(close<end && source.chars[close]!='`')++close;
    if(close<end) {token={pos+1,close-pos-1,0,0,1};pos=close+1;}else no_code=true;
   } else if(source.chars[pos]=='[') {
    if(bracket<=pos) {bracket=pos+1;while(bracket<end && source.chars[bracket]!=']')++bracket;}
    if(bracket+1<end && source.chars[bracket+1]=='(') {
     const uint32_t url=bracket+2;
     if(paren<url) {paren=url;while(paren<end && source.chars[paren]!=')')++paren;}
     if(paren<end) {token={pos+1,bracket-pos-1,url,paren-url,2};pos=paren+1;}
    }
   } else if(!no_bold && pos+1<end && source.chars[pos]=='*' && source.chars[pos+1]=='*') {
    uint32_t close=pos+2;while(close+1<end && !(source.chars[close]=='*' && source.chars[close+1]=='*'))++close;
    if(close+1<end) {token={pos+2,close-pos-2,0,0,3};pos=close+2;}else no_bold=true;
   }
   if(pos==start) {
    ++pos;while(pos<end && source.chars[pos]!='`' && source.chars[pos]!='[' && source.chars[pos]!='*')++pos;
    token.length=pos-start;
   }
   if(m->token_count==m->token_capacity) {
    const uint32_t cap=m->token_capacity ? m->token_capacity*2 : 64;
    auto* memory=std::realloc(m->tokens,size_t(cap)*sizeof(InlayInline));if(!memory)return false;
    m->tokens=static_cast<InlayInline*>(memory);m->token_capacity=cap;
   }
   m->tokens[m->token_count++]=token;
  }
  pos=end<source.length ? end+1 : end;
 }
 return true;
}
uint32_t finish_inline_row(const InlayMeasure* m,uint32_t stop) {
 // A link destination has no rendered width. Keep it with the label's last row.
 uint32_t low=0,high=m->token_count;
 while(low<high) {const uint32_t mid=low+(high-low)/2;if(m->tokens[mid].offset<=stop)low=mid+1;else high=mid;}
 if(low) {
  const auto& t=m->tokens[low-1];
  if(t.kind==2 && stop>=t.offset+t.length && stop<=t.url_offset+t.url_length)
   return t.url_offset+t.url_length+1;
 }
 return stop;
}
void rich_line(InlayMeasure* m,EtchStr line,uint32_t base_id,float font,InlayDocResult* result,uint32_t range_start=0,uint32_t range_end=UINT32_MAX) {
 const auto source=inlay_doc_source(m->doc);const uint32_t offset=uint32_t(line.chars-source.chars);
 const uint32_t start=offset+range_start,end=offset+(range_end<line.length ? range_end : line.length);
 uint32_t low=0,high=m->token_count;
 while(low<high) {const uint32_t mid=low+(high-low)/2;const auto& t=m->tokens[mid];if(t.offset+t.length<=start)low=mid+1;else high=mid;}
 uint32_t count=0;for(uint32_t i=low;i<m->token_count && m->tokens[i].offset<end;++i)++count;
 if(!count)return;
 auto* spans=static_cast<EtchTextSpan*>(etch_frame_arena_alloc(etch_current_context(),size_t(count)*sizeof(EtchTextSpan),alignof(EtchTextSpan)));if(!spans)return;
 for(uint32_t i=0;i<count;++i) {
  const auto& t=m->tokens[low+i];auto span=etch_text_span_default();span.font_size_px=font;
  const uint32_t a=t.offset>start ? t.offset : start,b=t.offset+t.length<end ? t.offset+t.length : end;
  span.text={source.chars+a,b-a,false};
  if(t.kind==1) {span.font_id=ETCH_FONT_MONO;span.background=etch_rgba8(42,53,68,255);span.padding_x=4;span.bg_radius=4;}
  if(t.kind==2) {span.color=etch_rgba8(80,190,240,255);span.underline=true;span.id=etch_id_index(base_id,t.offset+1);if(etch_clicked(span.id))result->clicked_link={source.chars+t.url_offset,t.url_length,false};}
  if(t.kind==3)span.font_id=ETCH_FONT_BOLD;
  spans[i]=span;
 }
 auto desc=etch_rich_text_desc_default();desc.id=base_id;desc.selectable=true;desc.font_size_px=font;desc.line_height_px=font*1.5f;
 etch_text_spans(spans,count,&desc);
}
void code_line(EtchStr line,uint32_t id,float font) {
 if(!line.length) {auto spacer=etch_box_desc_default();spacer.height=etch_sizing_fixed(font*1.5f);etch_box(&spacer);return;}
 EtchTextSpan* spans=nullptr;uint32_t count=0,pos=0;
 for(uint32_t pass=0;pass<2;++pass) {
 count=0;pos=0;
 while(pos<line.length) {
  uint32_t start=pos;auto span=etch_text_span_default();span.font_id=ETCH_FONT_MONO;span.font_size_px=font;
  const char c=line.chars[pos];
  if(uint8_t(c)>=0x80) {++pos;while(pos<line.length && (uint8_t(line.chars[pos])&0xc0)==0x80)++pos;}
  else if(c=='/' && pos+1<line.length && line.chars[pos+1]=='/') {pos=line.length;span.color=etch_rgba8(125,145,160,255);}
  else if(c=='"' || c=='\'') {
   ++pos;while(pos<line.length) {const char next=line.chars[pos++];if(next=='\\' && pos<line.length)++pos;else if(next==c)break;}
   span.color=etch_rgba8(170,215,145,255);
  } else if(c>='0' && c<='9') {++pos;while(pos<line.length && ((line.chars[pos]>='0' && line.chars[pos]<='9') || line.chars[pos]=='.'))++pos;span.color=etch_rgba8(225,170,120,255);}
  else {
   ++pos;while(pos<line.length && ((line.chars[pos]>='a' && line.chars[pos]<='z') || (line.chars[pos]>='A' && line.chars[pos]<='Z') || line.chars[pos]=='_'))++pos;
   const char* keywords[]={"const","auto","int","float","return","if","else","for","while","bool","void"};
   for(const char* word:keywords)if(std::strlen(word)==pos-start && !std::memcmp(word,line.chars+start,pos-start))span.color=etch_rgba8(125,175,245,255);
  }
  span.text={line.chars+start,pos-start,false};if(spans)spans[count]=span;++count;
 }
 if(pass==0) {spans=static_cast<EtchTextSpan*>(etch_frame_arena_alloc(etch_current_context(),size_t(count)*sizeof(EtchTextSpan),alignof(EtchTextSpan)));if(!spans)return;}
 }
 auto desc=etch_rich_text_desc_default();desc.id=id;desc.selectable=true;desc.font_id=ETCH_FONT_MONO;desc.font_size_px=font;desc.line_height_px=font*1.5f;desc.wrap=false;etch_text_spans(spans,count,&desc);
}
void block_lines(InlayMeasure* measure,const InlayBlock& b,EtchStr source,uint32_t id,float font,InlayDocResult* result) {
 uint32_t pos=b.content_offset,end=pos+b.content_length,line_index=0;
 while(pos<end) {
  uint32_t next=pos;while(next<end && source.chars[next]!='\n')++next;
  uint32_t logical=pos,logical_end=next;
  uint32_t content_start=b.offset;while(content_start<source.length && content_start-b.offset<3 && source.chars[content_start]==' ')++content_start;
  if(b.kind==INLAY_BLOCKQUOTE || b.kind==INLAY_ALERT)content_start+=2;
  if(b.kind==INLAY_HEADING)content_start+=b.level+1;
  const bool continuation=pos>content_start && source.chars[pos-1]!='\n';
  if(b.kind==INLAY_HEADING && !continuation) {uint32_t trim=b.offset;while(trim<logical_end && source.chars[trim]==' ')++trim;trim+=b.level+1;if(logical<trim)logical=trim;}
  EtchStr line=slice(source,logical,logical_end-logical);const uint32_t line_id=etch_id_index(id,++line_index);
  const auto render_rich=[&](EtchStr text) {const uint32_t offset=uint32_t(text.chars-source.chars);rich_line(measure,text,line_id,font,result,pos>offset ? pos-offset : 0,next>offset ? next-offset : 0);};
  if(b.kind==INLAY_CODE_BLOCK)code_line(line,line_id,font);
  else if(b.kind==INLAY_TABLE) {
   bool delimiter=true;for(uint32_t j=0;j<line.length;++j)if(!std::strchr("|:- \t\r",line.chars[j]))delimiter=false;
   if(!delimiter) {
    auto row=etch_box_desc_default();row.id=line_id;row.width=etch_sizing_fixed(measure->width-24);row.height=etch_sizing_fit(0,4096);row.scroll_y=true;row.clip_children=true;row.gap=12;row.padding=etch_padding_all(4);row.background=line_index==1 ? etch_rgba8(32,46,60,255) : etch_rgba8(20,29,42,255);etch_begin_box(&row);
    uint32_t start=line.length && line.chars[0]=='|' ? 1u : 0u,cell_index=0,cell_count=0;
    for(uint32_t cursor=start;cursor<line.length;) {++cell_count;while(cursor<line.length && line.chars[cursor]!='|')++cursor;if(cursor<line.length)++cursor;}
    const float cell_width=std::fmax(1.f,(measure->width-32-12*float(cell_count-1))/float(cell_count));
    while(start<line.length) {
     uint32_t stop=start;while(stop<line.length && line.chars[stop]!='|')++stop;
     auto cell=etch_box_desc_default();cell.width=etch_sizing_fixed(cell_width);cell.axis=ETCH_AXIS_COL;etch_begin_box(&cell);
     const uint32_t cell_offset=logical+start;
     rich_line(measure,{line.chars+start,stop-start,false},etch_id_index(line_id,++cell_index),font,result,pos>cell_offset ? pos-cell_offset : 0,next>cell_offset ? next-cell_offset : 0);etch_end_box();
     start=stop<line.length ? stop+1 : line.length;
    }
    etch_end_box();
   }
  } else {
   if((b.kind==INLAY_BLOCKQUOTE || b.kind==INLAY_ALERT) && !continuation) {
    if(line.length>=2 && line.chars[0]=='>' && line.chars[1]==' ') {line.chars+=2;line.length-=2;}
    if(b.kind==INLAY_ALERT && logical==b.offset+2 && line.length>=3 && line.chars[0]=='[' && line.chars[1]=='!') {line.chars+=2;line.length-=2;if(line.chars[line.length-1]==']')--line.length;}
   }
   if((b.kind==INLAY_LIST || b.kind==INLAY_TASK_LIST) && !continuation) {
    uint32_t indent=0;while(indent<line.length && indent<3 && line.chars[indent]==' ')++indent;line.chars+=indent;line.length-=indent;
    auto row=etch_box_desc_default();row.width=etch_sizing_fixed(measure->width-24);row.gap=8;etch_begin_box(&row);
    EtchStr marker=ETCH_STR_LIT("•");uint32_t trim=0;
    if(line.length>=6 && line.chars[0]=='-' && line.chars[2]=='[') {marker=line.chars[3]==' ' ? ETCH_STR_LIT("☐") : ETCH_STR_LIT("☑");trim=6;}
    else if(line.length>=2 && (line.chars[0]=='-' || line.chars[0]=='*' || line.chars[0]=='+'))trim=2;
    else {while(trim<line.length && line.chars[trim]!=' ')++trim;if(trim<line.length)++trim;}
    if(pos==logical)etch_text(marker,nullptr);
    line.chars+=trim;line.length-=trim;render_rich(line);etch_end_box();
   } else render_rich(line);
  }
  pos=next<end ? next+1 : end;
 }
}

}
extern "C" InlayDocResult inlay_doc_view(InlayDoc* doc,const InlayDocViewDesc* input) {
 InlayDocResult result={};if(!doc || !input || !input->id || !std::isfinite(input->width_px) || input->width_px<32 ||
  !std::isfinite(input->height_px) || input->height_px<1 || !std::isfinite(input->font_size_px) || input->font_size_px<6 || input->font_size_px>128 || (etch_current_context()!=doc->context || !etch_frame_in_progress(etch_current_context())))return result;
 const auto& view=*input;InlayMeasure* measure=nullptr;
 for(auto& slot:doc->views)if(slot.id==view.id) {measure=&slot;break;}
 if(!measure)for(auto& slot:doc->views)if(!slot.id) {measure=&slot;break;}
 if(!measure)return result;
 const bool reflow=measure->width!=view.width_px || measure->font!=view.font_size_px;
 if(reflow || measure->revision!=doc->revision) {
  if(doc->block_count>measure->open_capacity) {
   uint32_t cap=measure->open_capacity ? measure->open_capacity : 64;while(cap<doc->block_count)cap*=2;
   auto* ranges=static_cast<InlayDetails*>(std::realloc(measure->details,size_t(cap)*sizeof(InlayDetails)));if(!ranges)return result;
   measure->details=ranges;
   auto* data=static_cast<bool*>(std::realloc(measure->open,cap*sizeof(bool)));if(!data)return result;
   for(uint32_t i=measure->open_capacity;i<cap;++i)data[i]=true;
   measure->open=data;measure->open_capacity=cap;
  }
  if(measure->width==0)for(uint32_t i=0;i<measure->open_capacity;++i)measure->open[i]=true;
  const auto source=inlay_doc_source(doc);
  for(uint32_t block=measure->width==0 ? 0 : measure->frozen_count;block<doc->block_count;++block)if(doc->blocks[block].kind==INLAY_DETAILS) {
   auto& r=measure->details[block];details_range(source,doc->blocks[block],&r.summary,&r.summary_end,&r.body,&r.end);
  }
  if(!cache_inline(measure,source))return result;
  measure->parsed_tail=doc->tail_start;
  // Split prose for bounded wrapping; code and table rows retain logical lines.
  const uint32_t limit=uint32_t(std::fmin(2048.f,std::fmax(32.f,(view.width_px-48)/(view.font_size_px+12)*24)));
  uint32_t needed=0;
  for(uint32_t block=0;block<doc->block_count;++block) {
   const auto& b=doc->blocks[block];uint32_t pos=b.content_offset,end=pos+b.content_length;
   do {uint32_t stop=(b.kind==INLAY_DETAILS && !measure->open[block]) ? end : pos;while(stop<end && doc->source[stop]!='\n' && (b.kind==INLAY_CODE_BLOCK || b.kind==INLAY_TABLE || stop-pos<limit))++stop;
    if(b.kind!=INLAY_CODE_BLOCK && b.kind!=INLAY_TABLE)stop=finish_inline_row(measure,stop);
    while(stop<end && (uint8_t(doc->source[stop])&0xc0)==0x80)++stop;
    if(stop<end && doc->source[stop]=='\n')++stop;
    ++needed;pos=stop;
   } while(pos<end);
  }
  if(needed>measure->capacity) {
   uint32_t cap=measure->capacity ? measure->capacity : 64;while(cap<needed)cap*=2;
   auto* rows=static_cast<InlayRow*>(std::malloc(size_t(cap)*sizeof(InlayRow)));
   auto* heights=static_cast<float*>(std::calloc(cap,sizeof(float)));
   if(!rows || !heights) {std::free(rows);std::free(heights);return result;}
   if(measure->count) {std::memcpy(rows,measure->rows,size_t(measure->count)*sizeof(InlayRow));std::memcpy(heights,measure->heights,size_t(measure->count)*sizeof(float));}
   std::free(measure->rows);std::free(measure->heights);measure->rows=rows;measure->heights=heights;measure->capacity=cap;
  }
  uint32_t row=0;
  for(uint32_t block=0;block<doc->block_count;++block) {
   const auto& b=doc->blocks[block];uint32_t pos=b.content_offset,end=pos+b.content_length;
   do {uint32_t stop=(b.kind==INLAY_DETAILS && !measure->open[block]) ? end : pos;while(stop<end && doc->source[stop]!='\n' && (b.kind==INLAY_CODE_BLOCK || b.kind==INLAY_TABLE || stop-pos<limit))++stop;
    if(b.kind!=INLAY_CODE_BLOCK && b.kind!=INLAY_TABLE)stop=finish_inline_row(measure,stop);
    while(stop<end && (uint8_t(doc->source[stop])&0xc0)==0x80)++stop;
    if(stop<end && doc->source[stop]=='\n')++stop;
    const InlayRow next={block,pos,stop-pos,pos==b.content_offset,b.kind};
    const bool same=!reflow && block<measure->frozen_count && row<measure->count && measure->rows[row].block==block && measure->rows[row].offset==pos && measure->rows[row].length==stop-pos && measure->rows[row].kind==b.kind;
    if(!same)measure->heights[row]=0;
    measure->rows[row++]=next;pos=stop;
   } while(pos<end);
  }
  measure->count=row;measure->frozen_count=doc->frozen_count;
  // Rebuild Etch's estimates from persistent measurements, preserving frozen rows.
  const bool existed=measure->id!=0;
  measure->doc=doc;measure->id=view.id;measure->width=view.width_px;measure->font=view.font_size_px;measure->revision=doc->revision;
  if(existed)etch_virtual_list_invalidate_heights(view.id);
 }
 auto list=etch_virtual_list_desc_default();list.id=view.id;list.item_count=measure->count;list.variable_height=true;
 list.width=etch_sizing_fixed(view.width_px);list.height=etch_sizing_fixed(view.height_px);list.scroll_fade_px=20;
 list.measure_height_fn=block_height;list.measure_user_data=measure;
 const auto visible=etch_begin_virtual_list(&list);result.visible_first=visible.first_index;result.visible_end=visible.end_index;result.total_rows=measure->count;
 const EtchStr source=inlay_doc_source(doc);
 for(uint32_t i=visible.first_index;i<visible.end_index;++i) {
  auto b=*inlay_doc_block(doc,measure->rows[i].block);
  b.content_offset=measure->rows[i].offset;b.content_length=measure->rows[i].length;const uint32_t row_id=etch_id_index(etch_id_index(view.id,b.content_offset+1),b.content_length+1);
  if(b.kind==INLAY_DETAILS && !measure->rows[i].first && block_height(i,measure)<=1) {
   auto spacer=etch_box_desc_default();spacer.height=etch_sizing_fixed(1);etch_box(&spacer);continue;
  }
  auto box=etch_box_desc_default();box.id=row_id;box.axis=ETCH_AXIS_COL;box.width=etch_sizing_fixed(view.width_px);box.height=etch_sizing_fit(0,0);
  box.padding=etch_padding_all(12);box.gap=4;box.clip_children=true;
  if(b.kind==INLAY_CODE_BLOCK || b.kind==INLAY_ALERT || b.kind==INLAY_BLOCKQUOTE) {box.background=etch_rgba8(24,33,48,255);box.radius=etch_corners_all(8);}
  etch_begin_box(&box);
  if(b.kind==INLAY_THEMATIC_BREAK) {auto rule=etch_box_desc_default();rule.width=etch_sizing_grow(0,0);rule.height=etch_sizing_fixed(1);rule.background=etch_rgba8(80,95,110,255);etch_box(&rule);}
  else if(b.kind==INLAY_DETAILS) {
   const auto& range=measure->details[measure->rows[i].block];const uint32_t summary=range.summary,summary_end=range.summary_end,body=range.body,end=range.end;
   if(measure->rows[i].first) {
    auto button=etch_button_desc_default();button.id=etch_id_index(row_id,900);button.label=summary_end>summary ? slice(source,summary,summary_end-summary) : ETCH_STR_LIT("Details");
    if(etch_button(&button)) {
     measure->open[measure->rows[i].block]=!measure->open[measure->rows[i].block];measure->revision=doc->revision-1;
     for(uint32_t j=i;j<measure->count && measure->rows[j].block==measure->rows[i].block;++j)measure->heights[j]=0;
     etch_virtual_list_invalidate_heights(view.id);
    }
   }
   if(measure->open[measure->rows[i].block]) {
    const uint32_t start=b.content_offset>body ? b.content_offset : body,stop=b.content_offset+b.content_length<end ? b.content_offset+b.content_length : end;
    if(stop>start) {b.kind=INLAY_PARAGRAPH;b.content_offset=start;b.content_length=stop-start;block_lines(measure,b,source,row_id,view.font_size_px,&result);}
   }
  } else if(b.kind==INLAY_CODE_BLOCK) {
   if(measure->rows[i].first) {
   auto button=etch_button_desc_default();button.id=etch_id_index(row_id,1);button.label=ETCH_STR_LIT("Copy code");
   if(etch_button(&button)) {
    const auto& full=*inlay_doc_block(doc,measure->rows[i].block);
    char* copy=static_cast<char*>(etch_frame_arena_alloc(etch_current_context(),size_t(full.content_length)+1,1));
    if(copy) {std::memcpy(copy,source.chars+full.content_offset,full.content_length);copy[full.content_length]=0;etch_set_clipboard_text(copy);}
   }
   if(b.language_length)etch_text(slice(source,b.language_offset,b.language_length),nullptr);
   }
   block_lines(measure,b,source,etch_id_index(row_id,2),view.font_size_px,&result);
  } else block_lines(measure,b,source,row_id,b.kind==INLAY_HEADING ? view.font_size_px+float(7-b.level)*2 : view.font_size_px,&result);
  etch_end_box();
  const EtchRect previous=etch_element_bounds(row_id);
  if(previous.h>0 && std::fabs(previous.h-measure->heights[i])>.01f) {measure->heights[i]=previous.h;etch_virtual_list_set_item_height(view.id,i,previous.h);}
 }
 etch_end_virtual_list();return result;
}
