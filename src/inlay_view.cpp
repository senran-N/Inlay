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
bool diagram_block(const InlayDoc* doc,const InlayBlock& block) {
 return block.kind==INLAY_CODE_BLOCK && block.language_length==7 && !std::memcmp(doc->source+block.language_offset,"mermaid",7);
}
float custom_fence_estimate(const InlayDocViewDesc& view,const InlayDoc* doc,const InlayBlock& block,float available_width) {
 if(block.kind!=INLAY_CODE_BLOCK || !view.custom_fence_estimate_fn || !view.custom_fence_render_fn || diagram_block(doc,block))return 0.f;
 const EtchStr lang={doc->source+block.language_offset,block.language_length,false};
 const EtchStr body={doc->source+block.content_offset,block.content_length,false};
 const float h=view.custom_fence_estimate_fn(lang,body,available_width,view.font_size_px,view.custom_fence_user_data);
 return (std::isfinite(h) && h>0.f) ? std::fmin(h,4096.f) : 0.f;
}
struct NativeDraw {EtchStr source;InlayDiagramResult diagram_result;float font;bool diagram;EtchTheme theme;EtchColor text_color;};
void draw_native(EtchRect,void* user) {
 const auto& draw=*static_cast<NativeDraw*>(user);
 etch_push_theme(&draw.theme);
 if(draw.diagram){auto desc=inlay_diagram_desc_default();desc.font_size_px=draw.font;inlay_diagram_draw(draw.diagram_result,desc);}
 else {auto desc=inlay_math_desc_default();desc.font_size_px=draw.font;if(draw.text_color.a!=0)desc.color=draw.text_color;inlay_draw_math(draw.source,&desc);}
 etch_pop_theme();
}
void native_box(EtchStr source,float font,bool diagram,uint32_t id,float max_width,EtchColor text_color={}) {
 auto* ctx=etch_current_context();
 auto* copy=source.length ? static_cast<char*>(etch_frame_arena_alloc(ctx,source.length,1)) : nullptr;
 if(source.length && !copy){etch_text(source,nullptr);return;}
 if(copy)std::memcpy(copy,source.chars,source.length);
 const EtchStr copied={copy,source.length,false};
 float width=0,height=0;
 InlayDiagramResult diagram_result={};
 if(diagram){auto desc=inlay_diagram_desc_default();desc.font_size_px=font;diagram_result=inlay_diagram_layout(copied,&desc);if(!diagram_result.valid){etch_text(copied,nullptr);return;}width=diagram_result.width;height=diagram_result.height;}
 else {const auto metrics=inlay_measure_math(copied,font);if(!metrics.valid){etch_text(copied,nullptr);return;}width=metrics.width;height=metrics.height;}
 auto box=etch_box_desc_default();box.id=id;box.width=etch_sizing_fixed(std::fmin(width,max_width));box.height=etch_sizing_fixed(std::fmin(height+8,4096.f));box.scroll_y=height+8>4096;box.scroll_x=width>max_width;box.clip_children=true;
 etch_begin_box(&box);
 auto content=etch_box_desc_default();content.width=etch_sizing_fixed(width);content.height=etch_sizing_fixed(height+8);etch_begin_box(&content);
 auto* draw=static_cast<NativeDraw*>(etch_frame_arena_alloc(ctx,sizeof(NativeDraw),alignof(NativeDraw)));
 if(draw){
  *draw={copied,diagram_result,font,diagram,etch_get_theme(ctx),text_color};
  etch_draw_after_layout(draw_native,draw,false);
 }
 etch_end_box();etch_end_box();
}
EtchStr slice(EtchStr source,uint32_t start,uint32_t n) noexcept {return {source.chars+start,n,false};}
InlayDocStyle resolved_style(const InlayDocViewDesc& view) noexcept {
 return view.style ? *view.style : inlay_doc_style_default();
}
float effective_line_scale(const InlayDocStyle& style) noexcept {
 return style.line_height_scale>0.f ? style.line_height_scale : 1.5f;
}
float effective_heading_font(const InlayDocViewDesc& view,const InlayDocStyle& style,uint32_t level) noexcept {
 if(level>=1 && level<=6 && style.heading_scale[level-1]>0.f)return view.font_size_px*style.heading_scale[level-1];
 return view.font_size_px+float(7-level)*2.f;
}
uint16_t effective_sans_font(const InlayDocStyle& style) noexcept {
 return style.font_id!=ETCH_INVALID_FONT ? style.font_id : ETCH_FONT_SANS;
}
uint16_t effective_bold_font(const InlayDocStyle& style) noexcept {
 return style.bold_font_id!=ETCH_INVALID_FONT ? style.bold_font_id : ETCH_FONT_BOLD;
}
uint16_t effective_mono_font(const InlayDocStyle& style) noexcept {
 return style.mono_font_id!=ETCH_INVALID_FONT ? style.mono_font_id : ETCH_FONT_MONO;
}
struct ResolvedImage {
 struct SDL_GPUTexture* texture;
 float width,height,radius;
 bool valid;
};
ResolvedImage resolve_doc_image(const InlayDocViewDesc& view,EtchStr alt,EtchStr src,float max_width) {
 ResolvedImage out={};
 if(!view.image_resolver_fn)return out;
 InlayImageResult img={};
 if(!view.image_resolver_fn(alt,src,&img,view.image_resolver_user_data))return out;
 if(!img.texture || !std::isfinite(img.width_px) || !std::isfinite(img.height_px) || img.width_px<=0.f || img.height_px<=0.f)return out;
 float w=img.width_px,h=img.height_px;
 if(w>max_width && max_width>0.f) {
  const float s=max_width/w;
  w=max_width;
  h*=s;
 }
 out.texture=img.texture;out.width=w;out.height=h;
 out.radius=std::isfinite(img.radius_px) && img.radius_px>0.f ? img.radius_px : 0.f;
 out.valid=true;
 return out;
}
void draw_image_fallback(EtchStr alt,uint32_t id,float font,const InlayDocStyle& style,const EtchTheme& theme) {
 const EtchStr label=alt.length ? alt : ETCH_STR_LIT("image");
 auto pill=etch_box_desc_default();pill.id=id;pill.padding=etch_padding_xy(8,4);
 pill.background=theme.surface_subtle;pill.border_color=theme.border_subtle;pill.border_width=1;
 pill.radius=etch_corners_all(theme.radius_small_px);pill.align_y=ETCH_ALIGN_Y_CENTER;
 etch_begin_box(&pill);
 auto td=etch_text_desc_default();
 td.font_size_px=std::fmax(11.f,font*0.85f);
 td.font_id=effective_sans_font(style);
 td.color=style.muted_color.a!=0 ? style.muted_color : theme.text_muted;
 td.wrap=false;
 auto* ctx=etch_current_context();
 char* buf=static_cast<char*>(etch_frame_arena_alloc(ctx,size_t(label.length)+2,1));
 if(buf) {
  buf[0]='[';std::memcpy(buf+1,label.chars,label.length);buf[label.length+1]=']';
  etch_text({buf,label.length+2,false},&td);
 } else etch_text(label,&td);
 etch_end_box();
}
struct PendingTaskToggle {
 uint32_t source_offset,block_index,item_index;
 bool checked,triggered;
};
float block_padding(const InlayDocViewDesc& view,InlayBlockKind kind) {
 return kind==INLAY_CODE_BLOCK || kind==INLAY_ALERT || kind==INLAY_BLOCKQUOTE ? 12.f : view.block_padding_px;
}
bool details_body(const InlayMeasure& m,const InlayRow& row) {
 const auto& range=m.details[row.block];
 return m.open[row.block] && row.offset+row.length>range.body && row.offset<range.end;
}
bool skipped_row(const InlayMeasure& m,const InlayRow& row) {
 return row.separator || (row.kind==INLAY_DETAILS && !row.first && !details_body(m,row));
}
// Zero in the height cache denotes a row that has not been measured yet.
constexpr float collapsed_height=-1.f;
// Delimiters are temporary tokens during the line scan. Compact the line after
// matching so the cache contains only disjoint, styled source ranges.
constexpr uint32_t inline_open=1u<<10,inline_close=1u<<11;
bool append_inline(InlayMeasure* m,InlayInline token) {
 if(m->token_count==m->token_capacity) {
  const uint32_t cap=m->token_capacity ? m->token_capacity*2 : 64;
  auto* memory=std::realloc(m->tokens,size_t(cap)*sizeof(InlayInline));if(!memory)return false;
  m->tokens=static_cast<InlayInline*>(memory);m->token_capacity=cap;
 }
 m->tokens[m->token_count++]=token;return true;
}
bool inline_space(char c) {return c==' ' || c=='\t' || c=='\r';}
bool inline_line(InlayMeasure* m,const char* text,uint32_t pos,uint32_t end) {
 const uint32_t first=m->token_count,line_start=pos;
 uint32_t opens[4],depth=0,bracket=pos,paren=pos;
 bool no_code=false,no_dollar=false,no_paren_math=false;
 while(pos<end) {
  const uint32_t start=pos;InlayInline token={pos,1,0,0,0};
  const char c=text[pos];
  if(c=='$' && !no_dollar) {
   uint32_t close=pos+1;while(close<end && text[close]!='$')++close;
   if(close<end){token={pos+1,close-pos-1,pos,close+1-pos,InlayInlineMath};pos=close+1;}else no_dollar=true;
  } else if(!no_paren_math && c=='\\' && pos+1<end && text[pos+1]=='(') {
   uint32_t close=pos+2;while(close+1<end && !(text[close]=='\\' && text[close+1]==')'))++close;
   if(close+1<end){token={pos+2,close-pos-2,pos,close+2-pos,InlayInlineMath};pos=close+2;}else no_paren_math=true;
  } else if(c=='`' && !no_code) {
   uint32_t close=pos+1;while(close<end && text[close]!='`')++close;
   if(close<end){token={pos+1,close-pos-1,0,0,InlayInlineCode};pos=close+1;}else no_code=true;
  } else if(c=='!' && pos+1<end && text[pos+1]=='[') {
   if(bracket<=pos+1){bracket=pos+2;while(bracket<end && text[bracket]!=']')++bracket;}
   if(bracket+1<end && text[bracket+1]=='(') {
    const uint32_t url=bracket+2;
    if(paren<url){paren=url;while(paren<end && text[paren]!=')')++paren;}
    if(paren<end){token={pos+2,bracket-(pos+2),url,paren-url,InlayInlineImage};pos=paren+1;}
   }
  } else if(c=='[') {
   if(pos+1<end && text[pos+1]=='^') {
    uint32_t fn_end=pos+2;
    while(fn_end<end && text[fn_end]!=']' && !inline_space(text[fn_end]) && text[fn_end]!='[')++fn_end;
    if(fn_end<end && fn_end>pos+2 && text[fn_end]==']') {
     token={pos+2,fn_end-(pos+2),pos,fn_end+1-pos,InlayInlineFootnote};pos=fn_end+1;
    }
   }
   if(pos==start) {
    if(bracket<=pos){bracket=pos+1;while(bracket<end && text[bracket]!=']')++bracket;}
    if(bracket+1<end && text[bracket+1]=='(') {
     const uint32_t url=bracket+2;
     if(paren<url){paren=url;while(paren<end && text[paren]!=')')++paren;}
     if(paren<end){token={pos+1,bracket-pos-1,url,paren-url,InlayInlineLink};pos=paren+1;}
    }
   }
  } else if(c=='*' || c=='~' || c=='=') {
   uint32_t stop=pos+1;while(stop<end && text[stop]==c)++stop;
   const uint32_t run=stop-pos;
   const bool can_close=pos>line_start && !inline_space(text[pos-1]);
   if(can_close || (c=='*' && run<=3) || ((c=='~' || c=='=') && run==2)) {
    const bool can_open=stop<end && !inline_space(text[stop]);
    while(pos<stop) {
     const uint32_t left=stop-pos;
     uint32_t flag=c=='~' ? InlayInlineStrike : c=='=' ? InlayInlineHighlight : left>=2 ? InlayInlineBold : InlayInlineEmphasis;
     uint32_t match=depth;
     if(can_close)for(uint32_t i=depth;i>0;--i) {
      const auto& open=m->tokens[opens[i-1]];
      if(text[open.offset]==c && (open.length==left || (left>=3 && open.length<left))
         && open.offset+open.length<start) {
       match=i-1;flag=open.flags;break;
      }
     }
     if(match==depth && (left>3 || ((c=='~' || c=='=') && left!=2))) {
      if(!append_inline(m,{pos,left,0,0,0}))return false;
      break;
     }
     const uint32_t width=flag==InlayInlineEmphasis ? 1u : 2u;
     uint32_t flags=0;
     if(match<depth) {
      // Both styles in a triple opener start at the same visible position.
      // The first closer determines which style is nested.
      if(c=='*' && match+1<depth) {
       auto& open=m->tokens[opens[match]];
       auto& next=m->tokens[opens[match+1]];
       if(open.offset+open.length==next.offset && open.length+next.length==3 &&
          text[next.offset]=='*') {
        const uint32_t outer_width=next.length;
        next.offset=open.offset+outer_width;next.length=open.length;
        open.length=outer_width;open.flags=next.flags;next.flags=flag;
        ++match;
       }
      }
      while(depth>match+1)m->tokens[opens[--depth]].flags=0;
      m->tokens[opens[--depth]].flags=flag|inline_open;
      flags=flag|inline_close;
     } else if(can_open && depth<4) {
      bool already_open=false;
      for(uint32_t i=0;i<depth;++i)already_open |= m->tokens[opens[i]].flags==flag;
      if(!already_open){opens[depth++]=m->token_count;flags=flag;}
     }
     if(!append_inline(m,{pos,width,0,0,flags}))return false;
     pos+=width;
    }
    pos=stop;
    continue;
   }
   pos=stop;
   token.length=run;
  }
  if(pos==start) {
   if(c=='\\' && pos+1<end && std::strchr("\\|`*~[$=!",text[pos+1]))++pos;
   ++pos;
   while(pos<end && !std::strchr("`[*~$\\=!",text[pos]))++pos;
   token.length=pos-start;
  }
  if(!append_inline(m,token))return false;
 }
 while(depth)m->tokens[opens[--depth]].flags=0;
 uint32_t active=0,out=first;
 for(uint32_t i=first;i<m->token_count;++i) {
  auto t=m->tokens[i];
  if(t.flags&inline_open){active|=t.flags&~inline_open;continue;}
  if(t.flags&inline_close){active&=~(t.flags&~inline_close);continue;}
  if(!t.length && !(t.flags&(InlayInlineMath|InlayInlineImage)))continue;
  t.flags|=active;
  if(out>first && !(t.flags&(InlayInlineCode|InlayInlineLink|InlayInlineMath|InlayInlineFootnote|InlayInlineImage))) {
   auto& previous=m->tokens[out-1];
   if(previous.flags==t.flags && previous.offset+previous.length==t.offset) {
    previous.length+=t.length;continue;
   }
  }
  m->tokens[out++]=t;
 }
 m->token_count=out;return true;
}
void table_row_range(EtchStr* line,uint32_t* start) {
 while(*start<line->length && inline_space(line->chars[*start]))++*start;
 while(line->length>*start && inline_space(line->chars[line->length-1]))--line->length;
 if(*start<line->length && line->chars[*start]=='|')++*start;
}
uint32_t table_cell_end(EtchStr line,uint32_t pos) {
 while(pos<line.length && line.chars[pos]!='|') {
  if(line.chars[pos]=='\\' && pos+1<line.length &&
     (line.chars[pos+1]=='\\' || line.chars[pos+1]=='|'))++pos;
  ++pos;
 }
 return pos;
}
EtchStr table_line(EtchStr source,const InlayRow& row) {
 auto line=slice(source,row.offset,row.length);
 if(line.length && line.chars[line.length-1]=='\n')--line.length;
 return line;
}
uint32_t table_columns(EtchStr line) {
 uint32_t start=0,count=0;
 table_row_range(&line,&start);
 for(;start<line.length;start=table_cell_end(line,start)+1)++count;
 return count;
}
EtchAlignX table_alignment(EtchStr rule,uint32_t* start) {
 if(*start>=rule.length)return ETCH_ALIGN_X_LEFT;
 const uint32_t end=table_cell_end(rule,*start);
 uint32_t a=*start,b=end;
 while(a<b && inline_space(rule.chars[a]))++a;
 while(b>a && inline_space(rule.chars[b-1]))--b;
 *start=end+1;
 const bool right=a<b && rule.chars[b-1]==':';
 return right ? (rule.chars[a]==':' ? ETCH_ALIGN_X_CENTER : ETCH_ALIGN_X_RIGHT) : ETCH_ALIGN_X_LEFT;
}
float estimate_row(const InlayDocViewDesc& view,const InlayDocStyle& style,float width,const InlayBlock& b,
                   EtchStr text,bool first,bool last,bool diagram,uint32_t columns=1,EtchStr img_src={}) {
 const float padding=block_padding(view,b.kind);
 if(diagram)return 200;
 if(b.kind==INLAY_MATH_BLOCK)return view.font_size_px*4+32;
 if(b.kind==INLAY_THEMATIC_BREAK)return 1+2*padding;
 if(b.kind==INLAY_IMAGE) {
  const float avail=std::fmax(1.f,width-2*padding);
  const auto img=resolve_doc_image(view,text,img_src,avail);
  return (img.valid ? img.height : std::fmax(11.f,view.font_size_px*0.85f)*1.5f+8.f)+2*padding;
 }
 const float line_scale=effective_line_scale(style);
 const float font=b.kind==INLAY_HEADING ? effective_heading_font(view,style,b.level)
  : b.kind==INLAY_FOOTNOTE ? std::fmax(10.f,view.font_size_px*0.85f) : view.font_size_px;
 const float line_height=font*line_scale;
 if(b.kind==INLAY_TABLE) {
  const float cell_width=std::fmax(1.f,width-2*padding)/float(columns);
  const float available=std::fmax(1.f,cell_width-2*view.table_cell_padding_x);
  uint32_t start=0,longest=0;
  table_row_range(&text,&start);
  for(uint32_t i=0;i<columns && start<text.length;++i) {
   const uint32_t stop=table_cell_end(text,start);
   if(stop-start>longest)longest=stop-start;
   start=stop+1;
  }
  const float height=std::fmax(1.f,std::ceil(float(longest)*font*.5f/available))*line_height;
  return std::fmin(4096.f,height+2*view.table_cell_padding_y)+(float(first)+float(last))*padding;
 }
 const bool code=b.kind==INLAY_CODE_BLOCK;
 const float available=std::fmax(1.f,width-2*padding);
 float height=0;
 for(uint32_t pos=0;pos<text.length;) {
  uint32_t stop=pos;
  while(stop<text.length && text.chars[stop]!='\n')++stop;
  const float lines=code ? 1.f : std::fmax(1.f,std::ceil(float(stop-pos)*font*.5f/available));
  height+=lines*line_height+4;
  pos=stop+1;
 }
 const bool has_code_header=code && first && (!view.hide_code_copy_button || b.language_length || view.code_header_fn);
 const float header=has_code_header
  ? std::fmax((!view.hide_code_copy_button || view.code_header_fn) ? 32.f : 0.f,b.language_length ? line_height : 0.f)+4.f
  : (b.kind==INLAY_FOOTNOTE && first ? 8.f : 0.f);
 return std::fmax(0.f,height-4)+2*padding+header;
}
float initial_row_height(const InlayMeasure& m,const InlayRow& row) {
 if(skipped_row(m,row))return m.view.height_px==0 ? 0.f : 1.f;
 const auto& block=m.doc->blocks[row.block];
 if(row.kind==INLAY_DETAILS && row.first && !details_body(m,row))return 32+2*m.view.block_padding_px;
 const float custom_h=custom_fence_estimate(m.view,m.doc,block,std::fmax(1.f,m.width-2*m.view.block_padding_px));
 if(custom_h>0.f)return custom_h+2*m.view.block_padding_px;
 const EtchStr source={m.doc->source,m.doc->source_size,false};
 const EtchStr img_src=block.kind==INLAY_IMAGE ? slice(source,block.language_offset,block.language_length) : EtchStr{};
 return estimate_row(m.view,m.style,m.width,block,table_line(source,row),
                     row.first,row.last_in_block,diagram_block(m.doc,block),
                     row.kind==INLAY_TABLE ? m.rows[row.table_header].table_columns : 1,img_src);
}
float block_height(uint32_t index,void* user) {
 const auto& m=*static_cast<InlayMeasure*>(user);
 // Etch can request a row between a document mutation and the next view build.
 if(m.revision!=m.doc->revision || index>=m.count)return 28;
 const float height=m.heights[index];
 if(height==collapsed_height)return 0;
 return height==0 ? m.rows[index].estimate : height;
}
bool cache_inline(InlayMeasure* m,EtchStr source) {
 const uint32_t tail=m->width==0 ? 0 : m->parsed_tail;
 while(m->token_count && m->tokens[m->token_count-1].offset>=tail)--m->token_count;
 const uint32_t frozen=m->token_count;
 uint32_t low=0,high=m->doc->block_count;
 while(low<high) {
  const uint32_t mid=low+(high-low)/2;
  const auto& b=m->doc->blocks[mid];
  if(b.offset+b.length<=tail)low=mid+1;else high=mid;
 }
 for(uint32_t i=low;i<m->doc->block_count;++i) {
  const auto& b=m->doc->blocks[i];
  if(b.kind==INLAY_CODE_BLOCK || b.kind==INLAY_MATH_BLOCK || b.kind==INLAY_IMAGE)continue;
  const uint32_t last=b.offset+b.length;
  const uint32_t start_pos=b.kind==INLAY_FOOTNOTE ? b.content_offset : b.offset;
  for(uint32_t pos=start_pos>tail ? start_pos : tail;pos<last;) {
   uint32_t end=pos;while(end<last && source.chars[end]!='\n')++end;
   if(b.kind==INLAY_TABLE) {
    auto line=slice(source,pos,end-pos);
    uint32_t start=0;table_row_range(&line,&start);
    while(start<line.length) {
     const uint32_t stop=table_cell_end(line,start);
     if(!inline_line(m,source.chars,pos+start,pos+stop)){m->token_count=frozen;return false;}
     start=stop+1;
    }
   } else if(!inline_line(m,source.chars,pos,end)){m->token_count=frozen;return false;}
   pos=end<last ? end+1 : end;
  }
 }
 return true;
}
uint32_t finish_inline_row(const InlayMeasure* m,uint32_t pos,uint32_t stop,uint32_t limit,bool decorate) {
 uint32_t low=0,high=m->token_count;
 while(low<high) {const uint32_t mid=low+(high-low)/2;if(m->tokens[mid].offset<=stop)low=mid+1;else high=mid;}
 if(low<m->token_count){const auto& t=m->tokens[low];if((t.flags&InlayInlineMath) && stop>t.url_offset && stop<t.url_offset+t.url_length)return t.url_offset+t.url_length;}
 if(low) {
  const auto& t=m->tokens[low-1];
  if((t.flags&InlayInlineMath) && stop>t.url_offset && stop<t.url_offset+t.url_length)return t.url_offset+t.url_length;
  if((t.flags&InlayInlineFootnote) && stop>t.url_offset && stop<t.url_offset+t.url_length)return t.url_offset+t.url_length;
  const uint32_t text_end=t.offset+t.length;
  const bool link=(t.flags&InlayInlineLink)!=0,image=(t.flags&InlayInlineImage)!=0;
  if(link || image || (t.flags&InlayInlineCode)) {
   const uint32_t token_end=(link || image) ? t.url_offset+t.url_length+1 : text_end+1;
   const uint32_t token_start=image ? t.offset-2 : t.offset-1;
   if(stop<token_end) {
    // Keep closing syntax with the visible text, including hidden link destinations.
    if(stop>=text_end || t.length+2<=limit)return token_end;
    // Let Etch wrap the span, starting oversized tokens on a fresh row.
    if(token_start>pos)return token_start;
    if(decorate || token_end-pos<=16384)return token_end;
   }
  }
 }
 return stop;
}
EtchStr table_text(EtchStr text,EtchContext* ctx) {
 uint32_t first=0;
 while(first+1<text.length) {
  if(text.chars[first]=='\\') {
   if(text.chars[first+1]=='|')break;
   if(text.chars[first+1]=='\\')++first;
  }
  ++first;
 }
 if(first+1>=text.length)return text;
 auto* copy=static_cast<char*>(etch_frame_arena_alloc(ctx,text.length,1));
 if(!copy)return text;
 std::memcpy(copy,text.chars,first);
 uint32_t count=first;
 for(uint32_t i=first;i<text.length;++i) {
  if(text.chars[i]=='\\' && i+1<text.length) {
   if(text.chars[i+1]=='|')++i;
   else if(text.chars[i+1]=='\\')copy[count++]=text.chars[i++];
  }
  copy[count++]=text.chars[i];
 }
 return {copy,count,false};
}
struct InlineRowBox {
 const EtchBoxDesc* pending;
 bool suppressed;
};
void begin_inline_row(InlineRowBox& row) {
 if(row.pending) {
  etch_begin_box(row.pending);
  row.pending=nullptr;
 }
}
float rich_line(InlayMeasure* m,EtchStr line,uint32_t base_id,float font,InlayDocResult* result,const EtchTheme& theme,
                const InlayDocStyle& style,uint32_t range_start,uint32_t range_end,float width,bool estimate,InlineRowBox& row_box,
                bool table=false,bool header=false,EtchAlignX align=ETCH_ALIGN_X_LEFT,EtchColor color_override={}) {
 const EtchStr source={m->doc->source,m->doc->source_size,false};const uint32_t offset=uint32_t(line.chars-source.chars);
 const uint32_t start=offset+range_start,end=offset+(range_end<line.length ? range_end : line.length);
 uint32_t low=0,high=m->token_count;
 while(low<high) {const uint32_t mid=low+(high-low)/2;const auto& t=m->tokens[mid];if(t.offset+t.length<=start)low=mid+1;else high=mid;}
 uint32_t count=0;for(uint32_t i=low;i<m->token_count && m->tokens[i].offset<end;++i)++count;
 if(!count)return 0;
 auto* ctx=etch_current_context();
 bool math=false,has_image=false;
 const uint16_t sans_font=effective_sans_font(style);
 const uint16_t bold_font=effective_bold_font(style);
 const uint16_t mono_font=effective_mono_font(style);
 const uint16_t base_font=header ? (style.bold_font_id!=ETCH_INVALID_FONT ? style.bold_font_id : static_cast<uint16_t>(ETCH_FONT_SEMIBOLD)) : sans_font;
 float line_height=font*effective_line_scale(style);
 auto* spans=static_cast<EtchTextSpan*>(etch_frame_arena_alloc(ctx,size_t(count)*sizeof(EtchTextSpan),alignof(EtchTextSpan)));if(!spans)return 0;
 uint32_t emitted=count;
 for(uint32_t i=0;i<count;++i) {
  const auto& t=m->tokens[low+i];auto span=etch_text_span_default();span.font_size_px=font;
  const uint32_t a=t.offset>start ? t.offset : start,b=t.offset+t.length<end ? t.offset+t.length : end;
  span.text={source.chars+a,b-a,false};
  if(table)span.text=table_text(span.text,ctx);
  if(t.flags&InlayInlineEmphasis)span.font_id=ETCH_FONT_ITALIC;
  if(t.flags&InlayInlineBold)span.font_id=bold_font;
  span.strikethrough=(t.flags&InlayInlineStrike)!=0;
  if(t.flags&InlayInlineHighlight) {
   span.background=style.highlight_bg.a!=0 ? style.highlight_bg : EtchColor{theme.accent.r,theme.accent.g,theme.accent.b,56};
   span.padding_x=4;span.bg_radius=theme.radius_small_px;
  }
  if(t.flags&InlayInlineCode) {
   span.font_id=mono_font;
   span.background=style.code_bg.a!=0 ? style.code_bg : theme.surface_subtle;
   if(style.code_border.a!=0)span.border_color=style.code_border;
   span.color=theme.text_code;
   span.padding_x=4;span.bg_radius=theme.radius_small_px;
  }
  if(t.flags&InlayInlineFootnote) {
   const EtchColor fn_col=style.link_color.a!=0 ? style.link_color : theme.accent;
   span.color=fn_col;
   span.background={fn_col.r,fn_col.g,fn_col.b,32};
   span.border_color={fn_col.r,fn_col.g,fn_col.b,96};
   span.font_size_px=std::fmax(10.f,font*0.78f);
   span.padding_x=4;span.padding_y=1;span.bg_radius=theme.radius_small_px;
   span.id=etch_id_index(base_id,t.offset+1);
   line_height=std::fmax(line_height,etch_font_metrics(base_font,span.font_size_px).line_height+2*span.padding_y);
   if(span.id && etch_clicked(span.id))result->clicked_link=slice(source,t.url_offset,t.url_length);
  }
  if(t.flags&(InlayInlineLink|InlayInlineCode)) {
   const bool link=(t.flags&InlayInlineLink)!=0;
   EtchStr url={};
   if(link) {
    url=slice(source,t.url_offset,t.url_length);
    span.color=style.link_color.a!=0 ? style.link_color : theme.accent;
    span.underline=true;span.id=etch_id_index(base_id,t.offset+1);
   }
   EtchStr clicked_text=span.text;
   if(m->view.inline_decorator_fn) {
    const bool continuation=a>t.offset;
    const bool split=continuation || b<t.offset+t.length;
    const EtchStr full_text=split ? slice(source,t.offset,t.length) : span.text;
    auto decorated=span;
    decorated.text=full_text;
    if(m->view.inline_decorator_fn(link ? INLAY_INLINE_LINK : INLAY_INLINE_CODE,full_text,url,
                                  &decorated,m->view.inline_decorator_user_data)) {
     const bool replaced=decorated.text.chars!=full_text.chars || decorated.text.length!=full_text.length;
     if(replaced && continuation) {
      // Etch skips empty spans. Keep indices aligned with the tokens for inline math.
      spans[i]=etch_text_span_default();
      --emitted;
      continue;
     }
     clicked_text=decorated.text;
     if(!replaced && split)decorated.text=span.text;
     if(continuation)decorated.icon_id=ETCH_INVALID_ICON;
     span=decorated;
     const float size=span.font_size_px>0 ? span.font_size_px : font;
     const uint16_t face=span.font_id==ETCH_INVALID_FONT ? base_font : span.font_id;
     const float icon=span.icon_id==ETCH_INVALID_ICON ? 0.f : (span.icon_size_px>0 ? span.icon_size_px : size*.9f);
     line_height=std::fmax(line_height,std::fmax(etch_font_metrics(face,size).line_height,icon)+2*std::fmax(0.f,span.padding_y));
    }
   }
   if(span.id && etch_clicked(span.id))result->clicked_link=link ? url : clicked_text;
  }
  math |= (t.flags&InlayInlineMath)!=0;
  has_image |= (t.flags&InlayInlineImage)!=0;
  spans[i]=span;
 }
 if(!emitted) {row_box.suppressed=true;return 0;}
 begin_inline_row(row_box);
 auto desc=etch_rich_text_desc_default();desc.id=base_id;desc.selectable=true;desc.font_id=base_font;
 desc.font_size_px=font;desc.line_height_px=line_height;desc.align_x=align;
 if(color_override.a!=0)desc.color=color_override;
 else if(style.text_color.a!=0)desc.color=style.text_color;
 if(style.selection_color.a!=0)desc.selection_color=style.selection_color;
 if(math || has_image) {
  auto row=etch_box_desc_default();row.width=etch_sizing_fixed(width);row.wrap=true;row.align_y=ETCH_ALIGN_Y_CENTER;row.gap=4;etch_begin_box(&row);
  for(uint32_t i=0;i<count;++i) {
   desc.id=etch_id_index(base_id,i+1);
   const auto& t=m->tokens[low+i];
   if(t.flags&InlayInlineMath)native_box(spans[i].text,font,false,desc.id,width,desc.color.a!=0 && !etch_color_is_theme_auto(desc.color) ? desc.color : EtchColor{});
   else if(t.flags&InlayInlineImage) {
    const EtchStr alt=slice(source,t.offset,t.length);
    const EtchStr src=slice(source,t.url_offset,t.url_length);
    const auto img=resolve_doc_image(m->view,alt,src,width);
    if(img.valid) {
     auto idesc=etch_image_desc_default();idesc.id=desc.id;idesc.texture=img.texture;
     idesc.width=etch_sizing_fixed(img.width);idesc.height=etch_sizing_fixed(img.height);
     idesc.radius=etch_corners_all(img.radius);
     etch_image(&idesc);
    } else draw_image_fallback(alt,desc.id,font,style,theme);
   } else etch_text_spans(spans+i,1,&desc);
  }
  etch_end_box();
 } else etch_text_spans(spans,count,&desc);
 if(!estimate)return 0;
 // Etch handles word wrapping. Use the styled advances for the initial row
 // estimate until the layout exposes the exact wrapped height.
 float advance=0;
 float extra_height=0;
 for(uint32_t i=0;i<count;++i) {
  const auto& t=m->tokens[low+i];
  if(t.flags&InlayInlineImage) {
   const EtchStr alt=slice(source,t.offset,t.length);
   const EtchStr src=slice(source,t.url_offset,t.url_length);
   const auto img=resolve_doc_image(m->view,alt,src,width);
   if(img.valid){advance+=img.width+4;extra_height=std::fmax(extra_height,img.height);}
   else advance+=etch_measure_text(alt.length ? alt : ETCH_STR_LIT("image"),sans_font,std::fmax(11.f,font*0.85f),0).x+24;
   continue;
  }
  const auto& span=spans[i];
  const float size=span.font_size_px>0 ? span.font_size_px : font;
  const uint16_t face=span.font_id==ETCH_INVALID_FONT ? base_font : span.font_id;
  advance+=etch_measure_text(span.text,face,size,span.letter_spacing).x+2*std::fmax(0.f,span.padding_x);
  if(span.icon_id!=ETCH_INVALID_ICON) {
   advance+=span.icon_size_px>0 ? span.icon_size_px : size*.9f;
   if(span.text.length)advance+=std::fmax(0.f,span.icon_gap_px);
  }
 }
 return std::fmax(extra_height,std::fmax(1.f,std::ceil(advance/width))*line_height);
}
bool ascii_ieq(EtchStr a,const char* lit) noexcept {
 const size_t n=std::strlen(lit);
 if(a.length!=n)return false;
 for(size_t i=0;i<n;++i) {
  char c=a.chars[i];
  if(c>='A' && c<='Z')c=char(c+'a'-'A');
  if(c!=lit[i])return false;
 }
 return true;
}
bool is_code_ident_start(char c) noexcept {
 return (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_';
}
bool is_code_ident_char(char c) noexcept {
 return is_code_ident_start(c) || (c>='0' && c<='9');
}
bool is_code_keyword(const char* s,uint32_t len) noexcept {
 static constexpr const char* kWords[]={
  "const","let","var","mut","fn","func","function","def","class","struct","enum","union",
  "trait","impl","interface","type","typedef","namespace","using","import","export","from","as",
  "pub","public","private","protected","static","extern","inline","constexpr","virtual","override",
  "final","noexcept","template","typename","auto","void","bool","char","int","long","short",
  "float","double","unsigned","signed","size_t","uint8_t","uint16_t","uint32_t","uint64_t",
  "int32_t","int64_t","usize","isize","u8","u16","u32","u64","i32","i64","f32","f64","str",
  "string","number","boolean","any","unknown","never","if","else","elif","for","while","loop",
  "do","switch","match","case","default","break","continue","return","yield","async","await",
  "try","catch","finally","throw","raise","with","in","of","is","not","and","or","true","false",
  "null","nullptr","nil","None","True","False","self","this","super","new","delete","sizeof",
  "alignof","where","unsafe","move","ref","pass","lambda","package","module","local","then","fi","done"
 };
 for(const char* word:kWords) {
  if(std::strlen(word)==len && !std::memcmp(word,s,len))return true;
 }
 return false;
}
void code_line(EtchStr line,EtchStr language,uint32_t id,float font,const EtchTheme& theme,const InlayDocStyle& style) {
 const uint16_t mono_font=effective_mono_font(style);
 const float line_height=font*effective_line_scale(style);
 if(!line.length) {auto spacer=etch_box_desc_default();spacer.height=etch_sizing_fixed(line_height);etch_box(&spacer);return;}
 if(ascii_ieq(language,"diff") || ascii_ieq(language,"patch")) {
  auto span=etch_text_span_default();
  span.font_id=mono_font;span.font_size_px=font;span.text=line;span.color=theme.text_code;
  if(line.length>=2 && line.chars[0]=='@' && line.chars[1]=='@')span.color=theme.accent;
  else if(line.length>=3 && ((line.chars[0]=='+' && line.chars[1]=='+' && line.chars[2]=='+') ||
                             (line.chars[0]=='-' && line.chars[1]=='-' && line.chars[2]=='-')))span.color=theme.syntax_keyword;
  else if(line.chars[0]=='+')span.color=theme.syntax_string;
  else if(line.chars[0]=='-')span.color=theme.danger;
  auto desc=etch_rich_text_desc_default();desc.id=id;desc.selectable=true;desc.font_id=mono_font;desc.font_size_px=font;desc.line_height_px=line_height;desc.wrap=false;
  if(style.selection_color.a!=0)desc.selection_color=style.selection_color;
  etch_text_spans(&span,1,&desc);
  return;
 }
 const bool c_like=ascii_ieq(language,"c") || ascii_ieq(language,"cpp") || ascii_ieq(language,"c++") ||
                   ascii_ieq(language,"cxx") || ascii_ieq(language,"cc") || ascii_ieq(language,"h") ||
                   ascii_ieq(language,"hpp") || ascii_ieq(language,"hlsl") || ascii_ieq(language,"glsl");
 EtchTextSpan* spans=nullptr;uint32_t count=0,pos=0;
 for(uint32_t pass=0;pass<2;++pass) {
 count=0;pos=0;
 while(pos<line.length) {
  uint32_t start=pos;auto span=etch_text_span_default();span.font_id=mono_font;span.font_size_px=font;span.color=theme.text_code;
  const char c=line.chars[pos];
  if(uint8_t(c)>=0x80) {++pos;while(pos<line.length && (uint8_t(line.chars[pos])&0xc0)==0x80)++pos;}
  else if((c=='/' && pos+1<line.length && line.chars[pos+1]=='/') ||
          (c=='-' && pos+1<line.length && line.chars[pos+1]=='-' && (pos+2==line.length || line.chars[pos+2]==' ' || line.chars[pos+2]=='-'))) {
   pos=line.length;span.color=theme.syntax_comment;
  } else if(c=='/' && pos+1<line.length && line.chars[pos+1]=='*') {
   pos+=2;while(pos+1<line.length && !(line.chars[pos]=='*' && line.chars[pos+1]=='/'))++pos;
   pos=(pos+1<line.length) ? pos+2 : line.length;
   span.color=theme.syntax_comment;
  } else if(c=='#') {
   if(c_like) {
    ++pos;while(pos<line.length && is_code_ident_char(line.chars[pos]))++pos;
    span.color=theme.syntax_keyword;
   } else {
    pos=line.length;span.color=theme.syntax_comment;
   }
  } else if(c=='"' || c=='\'' || c=='`') {
   ++pos;while(pos<line.length) {const char next=line.chars[pos++];if(next=='\\' && pos<line.length)++pos;else if(next==c)break;}
   span.color=theme.syntax_string;
  } else if(c>='0' && c<='9') {
   ++pos;
   if(c=='0' && pos<line.length && (line.chars[pos]=='x' || line.chars[pos]=='X' || line.chars[pos]=='b' || line.chars[pos]=='B')) {
    ++pos;
    while(pos<line.length && ((line.chars[pos]>='0' && line.chars[pos]<='9') || (line.chars[pos]>='a' && line.chars[pos]<='f') ||
                              (line.chars[pos]>='A' && line.chars[pos]<='F') || line.chars[pos]=='_'))++pos;
   } else {
    while(pos<line.length && ((line.chars[pos]>='0' && line.chars[pos]<='9') || line.chars[pos]=='.' || line.chars[pos]=='_'))++pos;
   }
   while(pos<line.length && (line.chars[pos]=='u' || line.chars[pos]=='U' || line.chars[pos]=='l' || line.chars[pos]=='L' || line.chars[pos]=='f' || line.chars[pos]=='F'))++pos;
   span.color=theme.syntax_number;
  } else if(is_code_ident_start(c)) {
   ++pos;while(pos<line.length && is_code_ident_char(line.chars[pos]))++pos;
   if(is_code_keyword(line.chars+start,pos-start))span.color=theme.syntax_keyword;
  } else {
   ++pos;
  }
  span.text={line.chars+start,pos-start,false};if(spans)spans[count]=span;++count;
 }
 if(pass==0) {spans=static_cast<EtchTextSpan*>(etch_frame_arena_alloc(etch_current_context(),size_t(count)*sizeof(EtchTextSpan),alignof(EtchTextSpan)));if(!spans)return;}
 }
 auto desc=etch_rich_text_desc_default();desc.id=id;desc.selectable=true;desc.font_id=mono_font;desc.font_size_px=font;desc.line_height_px=line_height;desc.wrap=false;
 if(style.selection_color.a!=0)desc.selection_color=style.selection_color;
 etch_text_spans(spans,count,&desc);
}
float block_lines(InlayMeasure* measure,const InlayBlock& b,EtchStr source,uint32_t id,float font,
                  InlayDocResult* result,const EtchTheme& theme,const InlayDocStyle& style,const InlayRow& info,bool estimate=false,
                  const EtchBoxDesc* pending_box=nullptr,PendingTaskToggle* toggle_out=nullptr,EtchColor color_override={}) {
 const auto& view=measure->view;
 const float padding=block_padding(view,b.kind);
 const float width=std::fmax(1.f,measure->width-2*padding);
 float height=0;
 InlineRowBox row_box={pending_box,false};
 uint32_t pos=b.content_offset,end=pos+b.content_length,line_index=0;
 while(pos<end) {
  uint32_t next=pos;while(next<end && source.chars[next]!='\n')++next;
  uint32_t logical=pos,logical_end=next;
  uint32_t content_start=b.offset;while(content_start<source.length && content_start-b.offset<3 && source.chars[content_start]==' ')++content_start;
  if(b.kind==INLAY_BLOCKQUOTE || b.kind==INLAY_ALERT)content_start+=2;
  if(b.kind==INLAY_HEADING)content_start+=b.level+1;
  if(b.kind==INLAY_FOOTNOTE)content_start=measure->doc->blocks[info.block].content_offset;
  const bool continuation=pos>content_start && source.chars[pos-1]!='\n';
  if(b.kind==INLAY_HEADING && !continuation) {uint32_t trim=b.offset;while(trim<logical_end && source.chars[trim]==' ')++trim;trim+=b.level+1;if(logical<trim)logical=trim;}
  EtchStr line=slice(source,logical,logical_end-logical);const uint32_t line_id=etch_id_index(id,++line_index);
  const auto render_rich=[&](EtchStr text,float avail_override=0.f) {
   const uint32_t offset=uint32_t(text.chars-source.chars);
   const float available=avail_override>0.f ? avail_override : (b.kind==INLAY_LIST || b.kind==INLAY_TASK_LIST ? std::fmax(1.f,width-8-std::fmax(14.f,font)) : width);
   const float line_height=rich_line(measure,text,line_id,font,result,theme,style,
                                     pos>offset ? pos-offset : 0,next>offset ? next-offset : 0,available,estimate,row_box,
                                     false,false,ETCH_ALIGN_X_LEFT,color_override);
   if(line_height>0)height+=(height>0 ? 4.f : 0.f)+line_height;
  };
  if(b.kind==INLAY_CODE_BLOCK)code_line(line,slice(source,b.language_offset,b.language_length),line_id,font,theme,style);
  else if(b.kind==INLAY_TABLE) {
   const auto& header=measure->rows[info.table_header];
   const uint32_t columns=header.table_columns;
   EtchStr rule={};uint32_t rule_start=0;
   if(info.table_header+1<measure->count && measure->rows[info.table_header+1].separator &&
      measure->rows[info.table_header+1].block==info.block) {
    rule=table_line(source,measure->rows[info.table_header+1]);table_row_range(&rule,&rule_start);
   }
   auto row=etch_box_desc_default();row.id=line_id;row.width=etch_sizing_fixed(width);
   row.height=etch_sizing_fit(0,4096);row.scroll_y=true;row.clip_children=true;
   const EtchColor hdr_bg=style.table_header_bg.a!=0 ? style.table_header_bg : theme.bg_elevated;
   row.background=info.first ? hdr_bg : info.zebra
    ? etch_color_blend_over(theme.bg_table,theme.zebra_row) : theme.bg_table;
   row.border_color=style.table_border.a!=0 ? style.table_border : theme.border_subtle;
   row.border_sides={1,1,info.first ? 1.f : 0.f,1};
   const float radius=view.table_radius_px;
   const float top=info.first ? radius : 0.f,bottom=info.last_in_block ? radius : 0.f;
   row.radius={top,top,bottom,bottom};etch_begin_box(&row);
   uint32_t start=0;table_row_range(&line,&start);
   const float cell_width=width/float(columns);
   const float pad_x=std::fmin(view.table_cell_padding_x,cell_width*.5f);
   float tallest=0;
   for(uint32_t column=0;column<columns;++column) {
    const auto align=table_alignment(rule,&rule_start);
    auto cell=etch_box_desc_default();cell.width=etch_sizing_fixed(cell_width);cell.axis=ETCH_AXIS_COL;
    cell.align_x=align;cell.height=etch_sizing_fit(0,0);
    cell.padding=etch_padding_xy(pad_x,view.table_cell_padding_y);etch_begin_box(&cell);
    if(start<line.length) {
     const uint32_t stop=table_cell_end(line,start);
     const float cell_height=rich_line(measure,{line.chars+start,stop-start,false},etch_id_index(line_id,column+1),
                                      font,result,theme,style,0,stop-start,std::fmax(1.f,cell_width-2*pad_x),
                                      estimate,row_box,true,info.first,align,color_override);
     tallest=std::fmax(tallest,cell_height);
     start=stop+1;
    }
    etch_end_box();
   }
   etch_end_box();
   if(estimate)return std::fmin(4096.f,tallest+2*view.table_cell_padding_y)+
    (float(info.first)+float(info.last_in_block))*padding;
  } else {
   if((b.kind==INLAY_BLOCKQUOTE || b.kind==INLAY_ALERT) && !continuation) {
    if(line.length>=2 && line.chars[0]=='>' && line.chars[1]==' ') {line.chars+=2;line.length-=2;}
    if(b.kind==INLAY_ALERT && logical==b.offset+2 && line.length>=3 && line.chars[0]=='[' && line.chars[1]=='!') {line.chars+=2;line.length-=2;if(line.chars[line.length-1]==']')--line.length;}
   }
   if((b.kind==INLAY_LIST || b.kind==INLAY_TASK_LIST) && !continuation) {
    begin_inline_row(row_box);
    uint32_t indent=0;while(indent<line.length && indent<3 && line.chars[indent]==' ')++indent;line.chars+=indent;line.length-=indent;
    auto row=etch_box_desc_default();row.width=etch_sizing_fixed(width);row.gap=8;row.align_y=ETCH_ALIGN_Y_CENTER;etch_begin_box(&row);
    EtchStr marker=ETCH_STR_LIT("•");uint32_t trim=0;
    const bool task=line.length>=6 && line.chars[0]=='-' && line.chars[2]=='[';
    if(task)trim=6;
    else if(line.length>=2 && (line.chars[0]=='-' || line.chars[0]=='*' || line.chars[0]=='+'))trim=2;
    else {while(trim<line.length && line.chars[trim]!=' ')++trim;if(trim<line.length)++trim;}
    if(pos==logical) {
     if(task) {
      bool checked=line.chars[3]!=' ';
      const uint32_t check_offset=uint32_t((line.chars+3)-source.chars);
      auto cb=etch_checkbox_desc_default();
      cb.id=etch_id_index(line_id,0x5441u);
      cb.label={};cb.checked=&checked;
      cb.box_size_px=std::fmax(14.f,font*0.95f);
      cb.font_size_px=font;cb.font_id=effective_sans_font(style);
      if(style.link_color.a!=0)cb.accent=style.link_color;
      if(etch_checkbox(&cb) && toggle_out) {
       uint32_t item_index=0;
       const auto& full_block=measure->doc->blocks[info.block];
       for(uint32_t scan=full_block.content_offset;scan<pos;) {
        uint32_t lend=scan;while(lend<pos && source.chars[lend]!='\n')++lend;
        uint32_t ind=0;while(scan+ind<lend && ind<3 && source.chars[scan+ind]==' ')++ind;
        if(lend-(scan+ind)>=6 && source.chars[scan+ind]=='-' && source.chars[scan+ind+2]=='[')++item_index;
        scan=lend<pos ? lend+1 : pos;
       }
       *toggle_out={check_offset,info.block,item_index,checked,true};
      }
     } else {
      auto md=etch_text_desc_default();md.font_size_px=font;md.font_id=effective_sans_font(style);md.wrap=false;
      if(style.text_color.a!=0)md.color=style.text_color;
      etch_text(marker,&md);
     }
    }
    line.chars+=trim;line.length-=trim;render_rich(line);etch_end_box();
   } else render_rich(line);
  }
  pos=next<end ? next+1 : end;
 }
 if(pending_box) {
  if(row_box.pending && row_box.suppressed)return collapsed_height;
  begin_inline_row(row_box);
  etch_end_box();
 }
 return estimate ? height+2*padding : 0.f;
}

uint32_t row_end(const InlayMeasure* m,const InlayDocViewDesc& view,float row_width,const InlayBlock& b,uint32_t block,uint32_t pos,uint32_t limit,bool decorate) {
 const auto* doc=m->doc;
 const uint32_t end=b.content_offset+b.content_length;
 if((b.kind==INLAY_DETAILS && !m->open[block]) || b.kind==INLAY_MATH_BLOCK || b.kind==INLAY_IMAGE || b.kind==INLAY_FOOTNOTE ||
    diagram_block(doc,b) || custom_fence_estimate(view,doc,b,std::fmax(1.f,row_width-2*view.block_padding_px))>0.f)return end;
 uint32_t stop=pos;
 if(b.kind==INLAY_CODE_BLOCK) {
  for(uint32_t line=0;line<16 && stop<end;++line) {
   while(stop<end && doc->source[stop]!='\n')++stop;
   if(stop<end)++stop;
  }
  return stop;
 }
 while(stop<end && doc->source[stop]!='\n' && (b.kind==INLAY_TABLE || stop-pos<limit))++stop;
 while(stop<end && (uint8_t(doc->source[stop])&0xc0)==0x80)++stop;
 if(b.kind!=INLAY_TABLE && stop<end)stop=finish_inline_row(m,pos,stop,limit,decorate);
 if(stop<end && doc->source[stop]=='\n')++stop;
 return stop;
}

bool prepare_view(InlayDoc* doc,InlayMeasure* measure,const InlayDocViewDesc& view,float row_width,bool reflow) {
 if(doc->block_count>measure->open_capacity) {
  uint32_t cap=measure->open_capacity ? measure->open_capacity : 64;while(cap<doc->block_count)cap*=2;
  auto* ranges=static_cast<InlayDetails*>(std::realloc(measure->details,size_t(cap)*sizeof(InlayDetails)));if(!ranges)return false;
  measure->details=ranges;
  auto* data=static_cast<bool*>(std::realloc(measure->open,cap*sizeof(bool)));if(!data)return false;
  for(uint32_t i=measure->open_capacity;i<cap;++i)data[i]=true;
  measure->open=data;measure->open_capacity=cap;
 }
 if(measure->width==0)for(uint32_t i=0;i<measure->open_capacity;++i)measure->open[i]=true;
 measure->doc=doc;
 const EtchStr source={doc->source,doc->source_size,false};
 for(uint32_t block=measure->width==0 ? 0 : measure->frozen_count;block<doc->block_count;++block)if(doc->blocks[block].kind==INLAY_DETAILS) {
  auto& r=measure->details[block];details_range(source,doc->blocks[block],&r.summary,&r.summary_end,&r.body,&r.end);
 }
 if(!cache_inline(measure,source))return false;
 // Split prose for bounded wrapping and keep logical lines in code and tables.
 const uint32_t limit=uint32_t(std::fmin(2048.f,std::fmax(32.f,(row_width-48)/(view.font_size_px+12)*24)));
 const bool decorate=view.inline_decorator_fn!=nullptr;
 uint32_t needed=0;
 for(uint32_t block=0;block<doc->block_count;++block) {
  const auto& b=doc->blocks[block];uint32_t pos=b.content_offset,end=pos+b.content_length;
  do {const uint32_t stop=row_end(measure,view,row_width,b,block,pos,limit,decorate);
   ++needed;pos=stop;
  } while(pos<end);
 }
 if(needed>measure->capacity) {
  uint32_t cap=measure->capacity ? measure->capacity : 64;while(cap<needed)cap*=2;
  auto* rows=static_cast<InlayRow*>(std::malloc(size_t(cap)*sizeof(InlayRow)));
  auto* heights=static_cast<float*>(std::calloc(cap,sizeof(float)));
  if(!rows || !heights) {std::free(rows);std::free(heights);return false;}
  if(measure->count) {std::memcpy(rows,measure->rows,size_t(measure->count)*sizeof(InlayRow));std::memcpy(heights,measure->heights,size_t(measure->count)*sizeof(float));}
  std::free(measure->rows);std::free(measure->heights);measure->rows=rows;measure->heights=heights;measure->capacity=cap;
 }
 uint32_t row=0;
 for(uint32_t block=0;block<doc->block_count;++block) {
  const auto& b=doc->blocks[block];uint32_t pos=b.content_offset,end=pos+b.content_length;
  const uint32_t header=row;
  uint32_t last=row;
  bool zebra=false,need_separator=b.kind==INLAY_TABLE;
  do {const uint32_t stop=row_end(measure,view,row_width,b,block,pos,limit,decorate);
   const bool first=pos==b.content_offset;
   const bool separator=need_separator && !first && inlay_table_rule(source.chars+pos,stop-pos);
   InlayRow next={block,pos,stop-pos,header,0,0,first,zebra,separator,false,b.kind};
   if(first && b.kind==INLAY_TABLE) {
    const uint32_t columns=table_columns(table_line(source,next));
    next.table_columns=columns ? columns : 1;
   }
   if(!first)need_separator=false;
   if(!separator){last=row;if(!first)zebra=!zebra;}
   const bool same=!reflow && block<measure->frozen_count && row<measure->count && measure->rows[row].block==block && measure->rows[row].offset==pos && measure->rows[row].length==stop-pos && measure->rows[row].kind==b.kind;
   if(same)next.estimate=measure->rows[row].estimate;
   else measure->heights[row]=0;
   measure->rows[row++]=next;pos=stop;
  } while(pos<end);
  measure->rows[last].last_in_block=true;
 }
 measure->count=row;measure->frozen_count=doc->frozen_count;
 // Rebuild Etch's estimates from persistent measurements, preserving frozen rows.
 measure->id=view.id;measure->width=row_width;measure->font=view.font_size_px;measure->revision=doc->revision;
 measure->parsed_tail=doc->tail_start;measure->style=resolved_style(view);measure->view=view;measure->view.style=&measure->style;
 measure->total_height=0;
 for(uint32_t i=0;i<measure->count;++i) {
  if(measure->rows[i].estimate==0)measure->rows[i].estimate=initial_row_height(*measure,measure->rows[i]);
  measure->total_height+=block_height(i,measure);
  if(view.height_px==0 && i && measure->rows[i].first)measure->total_height+=view.block_gap_px;
 }
 return true;
}
bool valid_dimensions(uint32_t id,float width,float font) {
 return id && std::isfinite(width) && width>=32 && std::isfinite(font) && font>=6 && font<=128;
}
bool valid_spacing(float value) {return std::isfinite(value) && value>=0;}
bool valid_style(const InlayDocStyle* s) noexcept {
 if(!s)return true;
 if(!std::isfinite(s->line_height_scale) || s->line_height_scale<0.f || s->line_height_scale>10.f)return false;
 for(float hs:s->heading_scale)if(!std::isfinite(hs) || hs<0.f || hs>10.f)return false;
 return true;
}
bool style_reflow_changed(const InlayDocStyle& a,const InlayDocStyle& b) noexcept {
 if(a.font_id!=b.font_id || a.bold_font_id!=b.bold_font_id || a.mono_font_id!=b.mono_font_id ||
    a.line_height_scale!=b.line_height_scale)return true;
 for(int i=0;i<6;++i)if(a.heading_scale[i]!=b.heading_scale[i])return true;
 return false;
}
}

extern "C" float inlay_doc_measure_height(InlayDoc* doc,uint32_t view_id,float width_px,float font_size_px) {
 if(!doc || !valid_dimensions(view_id,width_px,font_size_px))return 0;
 const InlayMeasure* matched=nullptr;
 for(auto& slot:doc->views) {
  if(slot.id!=view_id || slot.view.width_px!=width_px || slot.font!=font_size_px)continue;
  matched=&slot;
  if(slot.width && slot.revision==doc->revision) {
   if(!slot.view.height_px && etch_current_context()==doc->context &&
      (slot.layout_settled || !etch_frame_in_progress(doc->context))) {
    const float height=etch_element_bounds(view_id).h;
    if(height>0) {
     slot.layout_settled=true;
     return height;
    }
   }
   return float(slot.total_height);
  }
  break;
 }
 auto view=matched ? matched->view : inlay_doc_view_desc_default();
 const auto style=matched ? matched->style : inlay_doc_style_default();
 if(!matched)view.height_px=0;
 view.font_size_px=font_size_px;
 const float width=view.height_px>0 && width_px>48 ? width_px-12 : width_px;
 const EtchStr source={doc->source,doc->source_size,false};
 double total=0;
 for(uint32_t i=0;i<doc->block_count;++i) {
  const auto& b=doc->blocks[i];
  if(i && view.height_px==0)total+=view.block_gap_px;
  if(b.kind!=INLAY_TABLE) {
   const float custom_h=custom_fence_estimate(view,doc,b,std::fmax(1.f,width-2*view.block_padding_px));
   if(custom_h>0.f) {
    total+=custom_h+2*view.block_padding_px;
    continue;
   }
   const EtchStr img_src=b.kind==INLAY_IMAGE ? slice(source,b.language_offset,b.language_length) : EtchStr{};
   total+=estimate_row(view,style,width,b,slice(source,b.content_offset,b.content_length),true,true,diagram_block(doc,b),1,img_src);
   continue;
  }
  uint32_t pos=b.content_offset;
  const uint32_t end=pos+b.content_length;
  bool first=true,rule=false;
  uint32_t columns=1;
  while(pos<end) {
   uint32_t stop=pos;
   while(stop<end && source.chars[stop]!='\n')++stop;
   const auto line=slice(source,pos,stop-pos);
   const uint32_t next=stop<end ? stop+1 : end;
   if(first) {
    const uint32_t count=table_columns(line);
    if(count)columns=count;
   }
   const bool separator=rule && inlay_table_rule(line.chars,line.length);
   const bool last=next==end || (first && !std::memchr(source.chars+next,'\n',end-next-1) &&
                                inlay_table_rule(source.chars+next,end-next));
   if(separator)total+=view.height_px==0 ? 0.f : 1.f;
   else total+=estimate_row(view,style,width,b,line,first,last,false,columns);
   rule=first;first=false;pos=next;
  }
 }
 return float(total);
}

extern "C" bool inlay_doc_view_is_at_bottom(uint32_t view_id) {
 return view_id ? etch_virtual_list_is_at_bottom(view_id) : true;
}

extern "C" void inlay_doc_view_scroll_to_bottom(uint32_t view_id,bool stop_momentum) {
 if(view_id)etch_virtual_list_scroll_to_bottom(view_id,stop_momentum);
}

extern "C" InlayDocResult inlay_doc_view(InlayDoc* doc,const InlayDocViewDesc* input) {
 InlayDocResult result={};
 if(!doc || !input || !valid_dimensions(input->id,input->width_px,input->font_size_px) ||
    !std::isfinite(input->height_px) || input->height_px<0 || (input->height_px>0 && input->height_px<1) ||
    !valid_spacing(input->block_padding_px) || !valid_spacing(input->block_gap_px) ||
    !valid_spacing(input->table_cell_padding_x) || !valid_spacing(input->table_cell_padding_y) ||
    !valid_spacing(input->table_radius_px) || !valid_style(input->style) ||
    etch_current_context()!=doc->context || !etch_frame_in_progress(doc->context))return result;
 const auto& view=*input;
 const InlayDocStyle style=resolved_style(view);
 InlayMeasure* measure=nullptr;
 for(auto& slot:doc->views)if(slot.id==view.id) {measure=&slot;break;}
 if(!measure)for(auto& slot:doc->views)if(!slot.id) {measure=&slot;break;}
 if(!measure)return result;
 const bool flow=view.height_px==0;
 const float row_width=!flow && view.width_px>48 ? view.width_px-12 : view.width_px;
 const auto& old=measure->view;
 const bool reflow=measure->width!=row_width || measure->font!=view.font_size_px ||
   flow!=(old.height_px==0) || old.block_padding_px!=view.block_padding_px || old.block_gap_px!=view.block_gap_px ||
   old.table_cell_padding_x!=view.table_cell_padding_x || old.table_cell_padding_y!=view.table_cell_padding_y ||
   old.hide_code_copy_button!=view.hide_code_copy_button || old.inline_decorator_fn!=view.inline_decorator_fn ||
   old.inline_decorator_user_data!=view.inline_decorator_user_data ||
   old.image_resolver_fn!=view.image_resolver_fn || old.image_resolver_user_data!=view.image_resolver_user_data ||
   old.custom_fence_estimate_fn!=view.custom_fence_estimate_fn ||
   old.custom_fence_render_fn!=view.custom_fence_render_fn ||
   old.custom_fence_user_data!=view.custom_fence_user_data ||
   old.code_header_fn!=view.code_header_fn ||
   old.code_header_user_data!=view.code_header_user_data ||
   (measure->width!=0 && style_reflow_changed(measure->style,style));
 const bool refresh=reflow || measure->revision!=doc->revision;
 float previous_height=flow && !refresh ? etch_element_bounds(view.id).h : 0.f;
 // A refresh prepares new measurements before Etch updates the element bounds.
 measure->layout_settled=previous_height>0;
 if(refresh) {
  if(!prepare_view(doc,measure,view,row_width,reflow))return result;
  if(!flow)etch_virtual_list_invalidate_heights(view.id);
 }
 measure->style=style;measure->view=view;measure->view.style=&measure->style;
 result.total_rows=measure->count;
 if(flow) {
  auto box=etch_box_desc_default();box.id=view.id;box.axis=ETCH_AXIS_COL;
  box.width=etch_sizing_fixed(view.width_px);box.height=etch_sizing_fit(0,0);box.gap=view.block_gap_px;
  if(style.has_background || style.background.a!=0)box.background=style.background;
  etch_begin_box(&box);
  result.visible_end=measure->count;
  result.at_bottom=true;
 } else {
  auto list=etch_virtual_list_desc_default();list.id=view.id;list.item_count=measure->count;list.variable_height=true;
  list.width=etch_sizing_fixed(view.width_px);list.height=etch_sizing_fixed(view.height_px);list.scroll_fade_px=20;
  list.measure_height_fn=block_height;list.measure_user_data=measure;
  list.stick_to_bottom=view.stick_to_bottom;
  if(style.has_background || style.background.a!=0)list.background=style.background;
  const auto visible=etch_begin_virtual_list(&list);
  result.visible_first=visible.first_index;result.visible_end=visible.end_index;
  result.at_bottom=visible.at_bottom;
 }
 const auto theme=etch_get_theme(doc->context);
 const EtchStr source={doc->source,doc->source_size,false};
 PendingTaskToggle pending_toggle={};
 bool group_open=false;
 for(uint32_t i=result.visible_first;i<result.visible_end;++i) {
  const auto& info=measure->rows[i];
  auto b=doc->blocks[info.block];
  b.content_offset=info.offset;b.content_length=info.length;
  const uint32_t row_id=etch_id_index(etch_id_index(view.id,info.block+1),info.offset-doc->blocks[info.block].content_offset+1);
  if(flow && info.first) {
   if(group_open)etch_end_box();
   auto group=etch_box_desc_default();group.axis=ETCH_AXIS_COL;
   group.width=etch_sizing_fixed(row_width);group.height=etch_sizing_fit(0,0);
   etch_begin_box(&group);group_open=true;
  }
  if(skipped_row(*measure,info)) {
   if(!flow) {
    auto spacer=etch_box_desc_default();spacer.height=etch_sizing_fixed(1);
    if(info.separator && !measure->rows[info.table_header].last_in_block) {
     spacer.width=etch_sizing_fixed(row_width);
     spacer.padding=etch_padding_xy(view.block_padding_px,0);
     etch_begin_box(&spacer);
     auto seam=etch_box_desc_default();seam.width=etch_sizing_grow(0,0);
     seam.height=etch_sizing_fixed(1);seam.background=style.table_border.a!=0 ? style.table_border : theme.border_subtle;
     etch_box(&seam);
     etch_end_box();
    } else etch_box(&spacer);
   }
   continue;
  }
  const bool is_custom_fence=b.kind==INLAY_CODE_BLOCK && info.first &&
   custom_fence_estimate(view,doc,doc->blocks[info.block],std::fmax(1.f,row_width-2*view.block_padding_px))>0.f;
  const float padding=is_custom_fence ? view.block_padding_px : block_padding(view,b.kind);
  auto box=etch_box_desc_default();box.id=row_id;box.axis=ETCH_AXIS_COL;box.width=etch_sizing_fixed(row_width);box.height=etch_sizing_fit(0,0);
  box.padding=etch_padding_all(padding);box.gap=4;box.clip_children=true;
  if(b.kind==INLAY_TABLE) {
   box.padding={padding,padding,info.first ? padding : 0.f,info.last_in_block ? padding : 0.f};
   box.gap=0;
  }
  if(b.kind==INLAY_CODE_BLOCK && !is_custom_fence) {
   box.background=style.code_bg.a!=0 ? style.code_bg : theme.bg_elevated;
   box.border_color=style.code_border.a!=0 ? style.code_border : theme.border_subtle;
   box.border_width=1;box.radius=etch_corners_all(theme.radius_panel_px);
  } else if(b.kind==INLAY_ALERT || b.kind==INLAY_BLOCKQUOTE) {
   box.background=style.quote_bg.a!=0 ? style.quote_bg : theme.bg_elevated;
   box.border_color=style.quote_border.a!=0 ? style.quote_border : (b.kind==INLAY_ALERT ? theme.accent : theme.border_subtle);
   box.border_width=1;box.radius=etch_corners_all(theme.radius_panel_px);
  }
  // A split inline continuation may emit no spans after decoration.
  const EtchBoxDesc* pending_box=!info.first && view.inline_decorator_fn && source.chars[info.offset-1]!='\n' &&
    b.kind!=INLAY_CODE_BLOCK && b.kind!=INLAY_TABLE && b.kind!=INLAY_THEMATIC_BREAK && b.kind!=INLAY_IMAGE && b.kind!=INLAY_FOOTNOTE ? &box : nullptr;
  if(!pending_box)etch_begin_box(&box);
  float styled_height=0;
  if(b.kind==INLAY_THEMATIC_BREAK) {auto rule=etch_box_desc_default();rule.width=etch_sizing_grow(0,0);rule.height=etch_sizing_fixed(1);rule.background=theme.border_line;etch_box(&rule);}
  else if(b.kind==INLAY_MATH_BLOCK || diagram_block(doc,b)) {
   native_box(slice(source,b.content_offset,b.content_length),view.font_size_px,b.kind!=INLAY_MATH_BLOCK,etch_id_index(row_id,7),std::fmax(1.f,row_width-2*padding),style.text_color);
  } else if(b.kind==INLAY_IMAGE) {
   const auto& full=doc->blocks[info.block];
   const EtchStr alt=slice(source,full.content_offset,full.content_length);
   const EtchStr src=slice(source,full.language_offset,full.language_length);
   const float avail=std::fmax(1.f,row_width-2*padding);
   const auto img=resolve_doc_image(view,alt,src,avail);
   if(img.valid) {
    auto idesc=etch_image_desc_default();idesc.id=etch_id_index(row_id,1);idesc.texture=img.texture;
    idesc.width=etch_sizing_fixed(img.width);idesc.height=etch_sizing_fixed(img.height);
    idesc.radius=etch_corners_all(img.radius);
    etch_image(&idesc);
   } else draw_image_fallback(alt,etch_id_index(row_id,1),view.font_size_px,style,theme);
  } else if(b.kind==INLAY_FOOTNOTE) {
   const float fn_font=std::fmax(10.f,view.font_size_px*0.85f);
   const EtchColor muted=style.muted_color.a!=0 ? style.muted_color : theme.text_muted;
   if(info.first) {
    auto div=etch_box_desc_default();div.width=etch_sizing_grow(0,0);div.height=etch_sizing_fixed(1);
    div.background=theme.border_subtle;etch_box(&div);
   }
   auto fn_row=etch_box_desc_default();fn_row.width=etch_sizing_fixed(std::fmax(1.f,row_width-2*padding));
   fn_row.gap=8;fn_row.align_y=ETCH_ALIGN_Y_TOP;etch_begin_box(&fn_row);
   if(info.first && b.language_length) {
    auto lbl=etch_text_desc_default();lbl.font_size_px=fn_font;lbl.font_id=effective_sans_font(style);
    lbl.color=style.link_color.a!=0 ? style.link_color : muted;lbl.wrap=false;
    char* lbuf=static_cast<char*>(etch_frame_arena_alloc(doc->context,size_t(b.language_length)+3,1));
    if(lbuf) {
     lbuf[0]='[';std::memcpy(lbuf+1,source.chars+b.language_offset,b.language_length);
     lbuf[b.language_length+1]=']';lbuf[b.language_length+2]=':';
     etch_text({lbuf,b.language_length+3,false},&lbl);
    } else etch_text(slice(source,b.language_offset,b.language_length),&lbl);
   }
   auto fn_body=etch_box_desc_default();fn_body.axis=ETCH_AXIS_COL;
   fn_body.width=etch_sizing_grow(0,0);fn_body.height=etch_sizing_fit(0,0);fn_body.gap=4;
   etch_begin_box(&fn_body);
   styled_height=block_lines(measure,b,source,row_id,fn_font,&result,theme,style,info,false,pending_box,nullptr,muted);
   etch_end_box();
   etch_end_box();
  } else if(b.kind==INLAY_DETAILS) {
   const auto& range=measure->details[info.block];const uint32_t summary=range.summary,summary_end=range.summary_end,body=range.body,end=range.end;
   if(info.first) {
    auto button=etch_button_desc_default();button.id=etch_id_index(row_id,900);button.label=summary_end>summary ? slice(source,summary,summary_end-summary) : ETCH_STR_LIT("Details");
    if(etch_button(&button)) {
     measure->open[info.block]=!measure->open[info.block];measure->revision=doc->revision-1;
     for(uint32_t j=i;j<measure->count && measure->rows[j].block==info.block;++j) {
      measure->heights[j]=0;
      measure->rows[j].estimate=0;
     }
     etch_request_redraw(doc->context);
    }
   }
   if(measure->open[info.block]) {
    const uint32_t start=b.content_offset>body ? b.content_offset : body,stop=b.content_offset+b.content_length<end ? b.content_offset+b.content_length : end;
    if(stop>start) {
     b.kind=INLAY_PARAGRAPH;b.content_offset=start;b.content_length=stop-start;
     styled_height=block_lines(measure,b,source,row_id,view.font_size_px,&result,theme,style,info,false,pending_box);
    }
   }
  } else if(is_custom_fence) {
   const auto& full=doc->blocks[info.block];
   const EtchStr lang=slice(source,full.language_offset,full.language_length);
   const EtchStr body=slice(source,full.content_offset,full.content_length);
   view.custom_fence_render_fn(info.block,row_id,lang,body,std::fmax(1.f,row_width-2*padding),view.font_size_px,view.custom_fence_user_data);
  } else if(b.kind==INLAY_CODE_BLOCK) {
   if(info.first && (!view.hide_code_copy_button || b.language_length || view.code_header_fn)) {
    const auto& full=doc->blocks[info.block];
    const EtchStr lang=slice(source,full.language_offset,full.language_length);
    const EtchStr full_code=slice(source,full.content_offset,full.content_length);
    auto hdr=etch_box_desc_default();
    hdr.width=etch_sizing_fixed(std::fmax(1.f,row_width-2*padding));
    hdr.height=etch_sizing_fit(0,0);
    hdr.align_y=ETCH_ALIGN_Y_CENTER;
    hdr.gap=8;
    etch_begin_box(&hdr);
    if(lang.length) {
     auto ld=etch_text_desc_default();ld.font_id=effective_mono_font(style);ld.font_size_px=std::fmax(11.f,view.font_size_px*0.82f);
     ld.color=style.muted_color.a!=0 ? style.muted_color : theme.text_muted;ld.wrap=false;
     etch_text(lang,&ld);
    }
    auto sp=etch_box_desc_default();sp.width=etch_sizing_grow(0,0);sp.height=etch_sizing_fixed(1);sp.shrink_width=true;
    etch_box(&sp);
    if(view.code_header_fn) {
     view.code_header_fn(info.block,row_id,lang,full_code,view.code_header_user_data);
    }
    if(!view.hide_code_copy_button) {
     auto button=etch_button_desc_default();button.id=etch_id_index(row_id,1);button.label=ETCH_STR_LIT("Copy code");
     if(etch_button(&button)) {
      char* copy=static_cast<char*>(etch_frame_arena_alloc(doc->context,size_t(full.content_length)+1,1));
      if(copy) {std::memcpy(copy,source.chars+full.content_offset,full.content_length);copy[full.content_length]=0;etch_set_clipboard_text(copy);}
     }
    }
    etch_end_box();
   }
   auto scroll=etch_box_desc_default();scroll.id=etch_id_index(row_id,2);
   scroll.width=etch_sizing_fixed(std::fmax(1.f,row_width-2*padding));scroll.height=etch_sizing_fit(0,0);
   scroll.scroll_x=true;scroll.clip_children=true;
   etch_begin_box(&scroll);
   auto content=etch_box_desc_default();content.axis=ETCH_AXIS_COL;
   content.width=etch_sizing_fit(0,0);content.height=etch_sizing_fit(0,0);content.gap=4;
   etch_begin_box(&content);
   block_lines(measure,b,source,scroll.id,view.font_size_px,&result,theme,style,info);
   etch_end_box();etch_end_box();
  } else {
   const bool estimate=view.inline_decorator_fn && (measure->heights[i]==collapsed_height ||
                       (measure->heights[i]==0 && (refresh || etch_element_bounds(row_id).h<=0)));
   const float font=b.kind==INLAY_HEADING ? effective_heading_font(view,style,b.level) : view.font_size_px;
   const EtchColor color_ov=b.kind==INLAY_HEADING ? style.heading_color
    : (b.kind==INLAY_BLOCKQUOTE ? style.muted_color : EtchColor{});
   styled_height=block_lines(measure,b,source,row_id,font,&result,theme,style,info,estimate,pending_box,&pending_toggle,color_ov);
  }
  if(!pending_box)etch_end_box();
  const float cached=measure->heights[i];
  if(measure->revision==doc->revision && styled_height!=cached &&
     (styled_height!=0 || cached==collapsed_height)) {
   const float before=block_height(i,measure);
   measure->heights[i]=styled_height;
   const float after=block_height(i,measure);
   measure->total_height+=after-before;
   if(!flow)etch_virtual_list_set_item_height(view.id,i,std::fmax(1.f,after));
   if(styled_height==collapsed_height || cached==collapsed_height) {
    previous_height=0;
    measure->layout_settled=false;
   }
  }
  if(styled_height==collapsed_height)continue;
  if(!reflow && cached!=collapsed_height && measure->revision==doc->revision) {
   const EtchRect previous=etch_element_bounds(row_id);
   if(previous.h>0 && std::fabs(previous.h-measure->heights[i])>.01f) {
    measure->total_height+=previous.h-block_height(i,measure);
    measure->heights[i]=previous.h;
    if(!flow)etch_virtual_list_set_item_height(view.id,i,previous.h);
   }
  }
 }
 if(flow) {
  if(group_open)etch_end_box();
  etch_end_box();
 } else etch_end_virtual_list();
 result.measured_height_px=previous_height>0 ? previous_height : float(measure->total_height);
 if(pending_toggle.triggered && pending_toggle.source_offset<doc->source_size) {
  doc->source[pending_toggle.source_offset]=pending_toggle.checked ? 'x' : ' ';
  etch_request_redraw(doc->context);
  if(view.task_toggle_fn) {
   view.task_toggle_fn(pending_toggle.block_index,pending_toggle.item_index,pending_toggle.checked,view.task_toggle_user_data);
  }
 }
 return result;
}
