#include <inlay/inlay.h>
#include <cstdio>
#include <SDL3/SDL_surface.h>
#include "../src/inlay_internal.h"
#include <cmath>
#include <cstring>
namespace {
bool str_equal(EtchStr text,EtchStr expected) {
 return text.length==expected.length && !std::memcmp(text.chars,expected.chars,text.length);
}
uint32_t doc_row_id(const InlayDoc* doc,uint32_t view,const InlayRow& row) {
 return etch_id_index(etch_id_index(view,row.block+1),row.offset-doc->blocks[row.block].content_offset+1);
}
bool contains_label(EtchContext* ctx,EtchStr text) {
 for(uint32_t i=0;i<etch_a11y_node_count(ctx,nullptr);++i) {
  EtchA11yNode node={};
  if(etch_get_a11y_node(i,&node,ctx,nullptr) && str_equal(node.label,text))return true;
 }
 return false;
}
bool markdown_and_scroll(EtchContext* ctx,InlayDoc* doc) {
 const auto input=ETCH_STR_LIT(
  "**bold with `code` and [link](https://example.org) and *emphasis* and ~~strike~~**\n\n"
  "***both*** and ~~*both again*~~\n"
  "**one****two** and ***nested** outside*\n"
  "***solo** remains\n"
  "Literal runs a~b a~~~b a****b\n\n"
  "| Key | Value |\n| --- | --- |\n| x\\|y | `a\\|b` |\n| **strong** | *soft* |\n\n"
  "```cpp\nconst auto first = \"abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz\";\n"
  "return abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz;\n```\n");
 inlay_doc_clear(doc);
 bool ok=true;
 auto view=inlay_doc_view_desc_default();view.id=etch_id("rich-markdown");view.width_px=320;view.height_px=720;
 auto f=etch_frame_desc_default();f.viewport_w=360;f.viewport_h=720;
 const auto frame=[&](){
  etch_begin_frame(ctx,&f);inlay_doc_view(doc,&view);etch_end_frame(ctx);
 };
 for(uint32_t pos=0;pos<input.length;) {
  const uint32_t left=input.length-pos,n=left<7 ? left : 7;
  ok &= inlay_doc_append_utf8(doc,{input.chars+pos,n,false});
  pos+=n;frame();
 }
 for(uint32_t i=0;i<3;++i)frame();
 const InlayMeasure* cached=nullptr;
 for(const auto& m:doc->views)if(m.id==view.id)cached=&m;
 if(!cached)return false;
 uint32_t previous_end=0,found=0;
 for(uint32_t i=0;i<cached->token_count;++i) {
  const auto& t=cached->tokens[i];
  ok &= t.offset>=previous_end;
  previous_end=t.offset+t.length;
  const EtchStr text={input.chars+t.offset,t.length,false};
  if(str_equal(text,ETCH_STR_LIT("code")) && t.flags==(InlayInlineCode|InlayInlineBold))found|=1;
  if(str_equal(text,ETCH_STR_LIT("link")) && t.flags==(InlayInlineLink|InlayInlineBold) &&
     str_equal({input.chars+t.url_offset,t.url_length,false},ETCH_STR_LIT("https://example.org")))found|=2;
  if(str_equal(text,ETCH_STR_LIT("emphasis")) && t.flags==(InlayInlineEmphasis|InlayInlineBold))found|=4;
  if(str_equal(text,ETCH_STR_LIT("strike")) && t.flags==(InlayInlineStrike|InlayInlineBold))found|=8;
  if(str_equal(text,ETCH_STR_LIT("both")) && t.flags==(InlayInlineBold|InlayInlineEmphasis))found|=16;
  if(str_equal(text,ETCH_STR_LIT("both again")) && t.flags==(InlayInlineStrike|InlayInlineEmphasis))found|=32;
  if(str_equal(text,ETCH_STR_LIT("one")) && t.flags==InlayInlineBold)found|=64;
  if(str_equal(text,ETCH_STR_LIT("two")) && t.flags==InlayInlineBold)found|=128;
  if(str_equal(text,ETCH_STR_LIT("nested")) && t.flags==(InlayInlineBold|InlayInlineEmphasis))found|=256;
  if(str_equal(text,ETCH_STR_LIT(" outside")) && t.flags==InlayInlineEmphasis)found|=512;
  if(str_equal(text,ETCH_STR_LIT("*")) && !t.flags)found|=1024;
  if(str_equal(text,ETCH_STR_LIT("solo")) && t.flags==InlayInlineBold)found|=2048;
  if(str_equal(text,ETCH_STR_LIT("Literal runs a~b a~~~b a****b")) && !t.flags)found|=4096;
 }
 ok &= found==8191 && str_equal(inlay_doc_source(doc),input);
 ok &= contains_label(ctx,ETCH_STR_LIT(" x|y ")) && contains_label(ctx,ETCH_STR_LIT(" a|b "));
 uint32_t table_rows=0;
 for(uint32_t i=0;i<cached->count;++i) {
  const auto& table=cached->rows[i];
  if(table.kind!=INLAY_TABLE)continue;
  ok &= table.first==(table_rows==0) && table.separator==(table_rows==1) &&
        table.zebra==(table_rows==3);
  ++table_rows;
 }
 ok &= table_rows==4;
 const auto& row=cached->rows[cached->count-1];
 const uint32_t id=doc_row_id(doc,view.id,row),scroll_id=etch_id_index(id,2);
 const uint32_t copy_id=etch_id_index(id,1),code_id=etch_id_index(scroll_id,1);
 const uint32_t next_line_id=etch_id_index(scroll_id,2);
 const auto card=etch_element_bounds(id),pane=etch_element_bounds(scroll_id);
 const auto copy=etch_element_bounds(copy_id),line=etch_element_bounds(code_id),next=etch_element_bounds(next_line_id);
 ok &= row.kind==INLAY_CODE_BLOCK && row.first && std::fabs(card.w-308)<.1f;
 ok &= pane.w>0 && pane.w<card.w && copy.y+copy.h<=pane.y && line.w>pane.w && next.y>line.y;
 etch_set_scroll_offset(scroll_id,{100,0},true);
 frame();frame();
 const auto scrolled_line=etch_element_bounds(code_id),scrolled_next=etch_element_bounds(next_line_id);
 ok &= etch_scroll_offset(scroll_id).x>0 && scrolled_line.x<line.x-50 && scrolled_next.x<next.x-50;
 ok &= std::fabs(etch_element_bounds(copy_id).x-copy.x)<.1f;
 if(!ok)std::printf("Markdown/scroll: styles=%u, card=%.1f, pane=%.1f, line=%.1f, x=%.1f / %.1f\n",
                    found,card.w,pane.w,line.w,line.x,scrolled_line.x);
 return ok;
}
bool sample_pixel(SDL_Surface* img,int x,int y,EtchColor expected,unsigned* brightness) {
 EtchColor pixel={};
 if(!SDL_ReadSurfacePixel(img,x,y,&pixel.r,&pixel.g,&pixel.b,&pixel.a))return false;
 *brightness=unsigned(pixel.r)+pixel.g+pixel.b;
 return std::abs(int(pixel.r)-expected.r)<=8 && std::abs(int(pixel.g)-expected.g)<=8 &&
        std::abs(int(pixel.b)-expected.b)<=8;
}
struct Chips {uint16_t icon;uint32_t code_id,link_id;bool code_seen,link_seen;};
bool decorate_chip(InlayInlineKind kind,EtchStr text,EtchStr url,EtchTextSpan* span,void* user) {
 auto& chips=*static_cast<Chips*>(user);
 if(kind==INLAY_INLINE_CODE) {
  chips.code_seen=str_equal(text,ETCH_STR_LIT("Chip")) && !url.length;
  span->id=chips.code_id;
 } else if(kind==INLAY_INLINE_LINK) {
  chips.link_seen=str_equal(text,ETCH_STR_LIT("Link")) && str_equal(url,ETCH_STR_LIT("https://example.org"));
  span->id=chips.link_id;
 } else return false;
 span->icon_id=chips.icon;span->icon_size_px=24;span->icon_gap_px=5;
 span->icon_color=etch_rgba8(255,16,220,255);
 span->background=etch_rgba8(40,55,80,255);span->hover_background=etch_rgba8(65,85,110,255);
 span->border_color=etch_rgba8(100,120,160,255);
 span->padding_x=8;span->padding_y=5;span->bg_radius=8;
 return true;
}
bool inline_decorator_row_boundaries(EtchContext* ctx,InlayDoc* doc) {
 struct Probe {InlayInlineKind kind;EtchStr text;uint32_t calls;bool matched;};
 Probe probe={};
 auto view=inlay_doc_view_desc_default();view.id=etch_id("inline-decorator-row-boundaries");
 view.width_px=64;view.height_px=0;
 view.inline_decorator_user_data=&probe;
 view.inline_decorator_fn=[](InlayInlineKind kind,EtchStr text,EtchStr,EtchTextSpan*,void* user) {
  auto& p=*static_cast<Probe*>(user);
  ++p.calls;p.matched &= kind==p.kind && str_equal(text,p.text);
  return false;
 };
 auto f=etch_frame_desc_default();f.viewport_w=240;f.viewport_h=400;
 const auto check=[&](EtchStr source,InlayInlineKind kind,EtchStr text) {
  if(!inlay_doc_set_utf8(doc,source))return false;
  probe={kind,text,0,true};
  etch_begin_frame(ctx,&f);
  const auto result=inlay_doc_view(doc,&view);
  etch_end_frame(ctx);
  const bool ok=result.total_rows==1 && probe.calls==1 && probe.matched;
  if(!ok)std::fprintf(stderr,"Inline decoration: kind=%u, text bytes=%u, rows=%u, calls=%u, full text=%d\n",
                      unsigned(kind),text.length,result.total_rows,probe.calls,probe.matched);
  return ok;
 };
 bool ok=check(ETCH_STR_LIT("aaaaaaaaaaaaaaaaaaaaaaaaaaaa `Chip`"),INLAY_INLINE_CODE,ETCH_STR_LIT("Chip"));
 ok &= check(ETCH_STR_LIT("aaaaaaaaaaaaaaaaaaaaaaaaaaaa [Link](https://example.org)"),
             INLAY_INLINE_LINK,ETCH_STR_LIT("Link"));
 const auto oversized=ETCH_STR_LIT("`abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz`");
 ok &= check(oversized,INLAY_INLINE_CODE,{oversized.chars+1,oversized.length-2,false});
 return ok;
}
bool inline_decorator_wrapped_code(EtchContext* ctx,InlayDoc* doc) {
 const auto source=ETCH_STR_LIT("`abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz`");
 const EtchStr original={source.chars+1,source.length-2,false};
 static const EtchStr replacement=ETCH_STR_LIT("REPLACED");
 static const auto foreground=etch_rgba8(32,212,144,255),background=etch_rgba8(61,39,108,255),
                   icon_color=etch_rgba8(255,16,220,255);
 struct Probe {uint16_t icon;bool replace;};
 Probe probe={etch_register_svg_stroke_icon(ctx,ETCH_STR_LIT("M 3 12 L 9 18 L 21 5"),nullptr),false};
 auto view=inlay_doc_view_desc_default();view.id=etch_id("inline-decorator-wrapped-code");
 view.width_px=64;view.height_px=0;
 view.inline_decorator_user_data=&probe;
 view.inline_decorator_fn=[](InlayInlineKind,EtchStr,EtchStr,EtchTextSpan* span,void* user) {
  const auto& p=*static_cast<Probe*>(user);
  span->id=etch_id("wrapped-code");span->color=foreground;span->background=background;
  if(p.replace)span->text=replacement;
  else {span->icon_id=p.icon;span->icon_color=icon_color;}
  return true;
 };
 auto f=etch_frame_desc_default();f.viewport_w=240;f.viewport_h=400;
 const auto frame=[&]() {
  etch_begin_frame(ctx,&f);
  const auto result=inlay_doc_view(doc,&view);
  etch_end_frame(ctx);
  return result;
 };
 bool ok=probe.icon!=ETCH_INVALID_ICON && inlay_doc_set_utf8(doc,source);
 const bool replacements[]={true,false};
 for(bool replace:replacements) {
  probe.replace=replace;
  f.pointer_pos={0,0};
  const auto result=frame();
  const auto expected=replace ? replacement : original;
  uint32_t labels=0,bytes=0,icons=0;
  bool labels_match=true,styled=true;
  EtchRect hit={};
  for(uint32_t i=0;i<etch_a11y_node_count(ctx,nullptr);++i) {
   EtchA11yNode node={};
   if(!etch_get_a11y_node(i,&node,ctx,nullptr) || node.role!=ETCH_A11Y_STATIC_TEXT)continue;
   ++labels;
   labels_match &= bytes+node.label.length<=expected.length &&
                   !std::memcmp(node.label.chars,expected.chars+bytes,node.label.length);
   bytes+=node.label.length;
   bool has_foreground=false,has_background=false;
   uint32_t row_icons=0;
   for(uint32_t j=0;j<etch_frame_prim_count(ctx);++j) {
    EtchPrim prim={};
    if(!etch_get_frame_prim(ctx,j,&prim) || prim.bounds.y<node.bounds.y ||
       prim.bounds.y>=node.bounds.y+node.bounds.h)continue;
    has_foreground |= prim.style.y==etch_color_pack_rgba8(foreground);
    if(prim.style.y==etch_color_pack_rgba8(background)) {
     has_background=true;
     hit={prim.bounds.x,prim.bounds.y,prim.bounds.z,prim.bounds.w};
    }
    if(prim.style.y==etch_color_pack_rgba8(icon_color))++row_icons;
   }
   styled &= has_foreground && has_background && row_icons==(!replace && labels==1 ? 1u : 0u);
   icons+=row_icons;
  }
  f.pointer_pos={hit.x+2,hit.y+hit.h*.5f};
  f.pointer_down=true;const auto pressed=frame();
  f.pointer_down=false;const auto released=frame();
  const bool clicked=str_equal(pressed.clicked_link,expected) || str_equal(released.clicked_link,expected);
  const bool pass=result.total_rows==1 && labels==1 &&
                  labels_match && bytes==expected.length && styled && clicked;
  if(!pass)std::fprintf(stderr,"Wrapped code (replace=%d): rows=%u, labels=%u, bytes=%u, icons=%u, styled=%d, clicked=%d\n",
                        replace,result.total_rows,labels,bytes,icons,styled,clicked);
  ok &= pass;
 }
 ok &= inlay_doc_set_utf8(doc,ETCH_STR_LIT("`one two three four five six`"));
 const auto wrapped=frame();
 ok &= wrapped.total_rows==1 && etch_element_bounds(view.id).h>view.font_size_px*1.5f+2*view.block_padding_px;
 return ok;
}
bool inline_decorator_replacement_height(EtchContext* ctx,InlayDoc* doc) {
 auto view=inlay_doc_view_desc_default();view.id=etch_id("inline-decorator-replacement-height");
 view.width_px=64;view.height_px=0;
 view.inline_decorator_fn=[](InlayInlineKind,EtchStr,EtchStr,EtchTextSpan* span,void*) {
  span->text=ETCH_STR_LIT("X");
  return true;
 };
 auto f=etch_frame_desc_default();f.viewport_w=240;f.viewport_h=400;
 bool ok=true;
 const auto settled_height=[&](EtchStr source) {
  ok &= inlay_doc_set_utf8(doc,source);
  InlayDocResult result={};
  for(uint32_t i=0;i<3;++i) {
   etch_begin_frame(ctx,&f);
   result=inlay_doc_view(doc,&view);
   etch_end_frame(ctx);
  }
  const float height=etch_element_bounds(view.id).h;
  const float measured=inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px);
  const bool pass=result.total_rows==1 && height>0 && result.measured_height_px==height && measured==height;
  if(!pass)std::fprintf(stderr,"Replacement height: bytes=%u, rows=%u, bounds=%.1f, view=%.1f, measure=%.1f\n",
                        source.length,result.total_rows,height,result.measured_height_px,measured);
  ok &= pass;
  return height;
 };
 const float short_height=settled_height(ETCH_STR_LIT("`short`"));
 char code[16385+2];code[0]='`';
 const uint32_t lengths[]={800,16385};
 for(uint32_t length:lengths) {
  std::memset(code+1,'a',length);code[length+1]='`';
  const float long_height=settled_height({code,length+2,false});
  if(long_height!=short_height)std::fprintf(stderr,"Code replacement (%u bytes): height=%.1f, short=%.1f\n",
                                            length,long_height,short_height);
  ok &= long_height==short_height;
 }
 const char link_end[]="](https://example.org)";
 char link[801+sizeof(link_end)-1];link[0]='[';
 std::memset(link+1,'a',800);std::memcpy(link+801,link_end,sizeof(link_end)-1);
 const float link_height=settled_height({link,sizeof(link),false});
 if(link_height!=short_height)std::fprintf(stderr,"Link replacement: height=%.1f, short=%.1f\n",link_height,short_height);
 ok &= link_height==short_height;
 return ok;
}
bool flow_height_after_refresh(EtchContext* ctx,InlayDoc* doc) {
 auto view=inlay_doc_view_desc_default();view.id=etch_id("flow-height-refresh");view.height_px=0;
 auto f=etch_frame_desc_default();f.viewport_w=800;f.viewport_h=480;
 const auto measure=[&]() {return inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px);};
 const auto check_frame=[&](const char* change) {
  etch_begin_frame(ctx,&f);
  const auto result=inlay_doc_view(doc,&view);
  const float during=measure();
  etch_end_frame(ctx);
  const float after=measure();
  const bool ok=result.measured_height_px>0 && std::fabs(during-result.measured_height_px)<.1f &&
                std::fabs(after-result.measured_height_px)<.1f;
  if(!ok)std::printf("Flow height after %s: view=%.1f, during=%.1f, after=%.1f\n",
                     change,result.measured_height_px,during,after);
  return ok;
 };
 bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT("one\ntwo\nthree"));
 etch_begin_frame(ctx,&f);inlay_doc_view(doc,&view);etch_end_frame(ctx);
 const float original=etch_element_bounds(view.id).h;
 ok &= inlay_doc_set_utf8(doc,ETCH_STR_LIT("one"));
 ok &= check_frame("document replacement");
 const float shorter=etch_element_bounds(view.id).h;
 ok &= shorter>0 && shorter<original;
 view.block_padding_px+=8;
 ok &= check_frame("padding change");
 return ok && etch_element_bounds(view.id).h>shorter;
}
bool flow_document(const char* png) {
 auto cfg=etch_context_desc_default();cfg.headless=true;
 auto* ctx=etch_create_context(&cfg);if(!ctx)return false;
 auto* doc=inlay_doc_create(ctx,nullptr);
 bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT(
  "| Word | Word | Word |\n| :--- | :---: | ---: |\n| Word | Word | Word |\n| end | two | three |\n\n"
  "`Chip` [Link](https://example.org)\n\n*Waffle words*\n\n```cpp\nint x;\n```\n"));
 Chips chips={etch_register_svg_stroke_icon(ctx,ETCH_STR_LIT("M 3 12 L 9 18 L 21 5"),nullptr),
              etch_id("flow-code-chip"),etch_id("flow-link-chip"),false,false};
 auto view=inlay_doc_view_desc_default();view.id=etch_id("flow-document");view.height_px=0;
 view.width_px=340;view.block_padding_px=0;view.block_gap_px=11;view.table_cell_padding_x=10;
 view.table_cell_padding_y=6;view.table_radius_px=7;view.hide_code_copy_button=true;
 auto f=etch_frame_desc_default();f.viewport_w=380;f.viewport_h=360;
 const auto frame=[&]() {
  etch_begin_frame(ctx,&f);
  auto parent=etch_box_desc_default();parent.id=etch_id("flow-parent");parent.axis=ETCH_AXIS_COL;
  parent.width=etch_sizing_fixed(360);parent.height=etch_sizing_fit(0,0);parent.gap=3;
  etch_begin_box(&parent);
  const auto result=inlay_doc_view(doc,&view);
  auto footer=etch_box_desc_default();footer.id=etch_id("flow-footer");
  footer.width=etch_sizing_fixed(20);footer.height=etch_sizing_fixed(5);etch_box(&footer);
  etch_end_box();etch_end_frame(ctx);
  return result;
 };
 const float estimate=inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px);
 const auto first=frame();
 const auto undecorated=etch_element_bounds(view.id);
 view.inline_decorator_fn=decorate_chip;view.inline_decorator_user_data=&chips;
 frame();
 const auto settled=frame();
 const auto flow=etch_element_bounds(view.id),footer=etch_element_bounds(etch_id("flow-footer"));
 const auto& cached=doc->views[0];
 const auto bounds=[&](uint32_t index){return etch_element_bounds(doc_row_id(doc,view.id,cached.rows[index]));};
 const auto header=bounds(0),body=bounds(2),last=bounds(3),italic=bounds(5);
 const auto code=etch_element_bounds(chips.code_id),link=etch_element_bounds(chips.link_id);
 ok &= estimate>0 && first.measured_height_px>0 && settled.total_rows==7 &&
       settled.visible_first==0 && settled.visible_end==settled.total_rows && flow.w==view.width_px &&
       std::fabs(flow.h-settled.measured_height_px)<.1f &&
       std::fabs(flow.h-inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px))<.1f &&
       std::fabs(flow.y+flow.h+3-footer.y)<.1f && flow.h>undecorated.h+8;
 ok &= cached.rows[1].separator && cached.rows[3].last_in_block && bounds(1).h==0 &&
       std::fabs(header.y+header.h-body.y)<.1f && std::fabs(body.y+body.h-last.y)<.1f &&
       std::fabs(last.y+last.h+view.block_gap_px-bounds(4).y)<.1f;
 const auto bold_cell=etch_id_index(doc_row_id(doc,view.id,cached.rows[2]),1);
 EtchRect cells[3]={};
 for(uint32_t i=0;i<3;++i)cells[i]=etch_element_bounds(etch_id_index(bold_cell,i+1));
 const float column=view.width_px/3;
 ok &= cells[0].w>10 && cells[1].w>10 && cells[2].w>10 &&
       std::fabs(cells[0].x-body.x-view.table_cell_padding_x)<1.5f &&
       std::fabs(cells[1].x+cells[1].w*.5f-body.x-column*1.5f)<1.5f &&
       std::fabs(cells[2].x+cells[2].w-body.x-body.w+view.table_cell_padding_x)<1.5f;
 ok &= chips.icon!=ETCH_INVALID_ICON && chips.code_seen && chips.link_seen && code.w>55 && link.h>=34 &&
       contains_label(ctx,ETCH_STR_LIT("Waffle words")) && italic.h>0 &&
       !contains_label(ctx,ETCH_STR_LIT("Copy code")) && contains_label(ctx,ETCH_STR_LIT("cpp"));
 const auto italic_width=etch_measure_text(ETCH_STR_LIT("Waffle words"),ETCH_FONT_ITALIC,view.font_size_px,0);
 const auto italic_text=etch_element_bounds(etch_id_index(doc_row_id(doc,view.id,cached.rows[5]),1));
 ok &= italic_width.x>0 && std::fabs(italic_width.x-italic_text.w)<2;
 if(!etch_is_font_loaded(ctx,ETCH_FONT_ITALIC)) {
  const auto fallback=etch_measure_text(ETCH_STR_LIT("Waffle words"),ETCH_FONT_SANS,view.font_size_px,0);
  ok &= std::fabs(italic_width.x-fallback.x)<.1f;
 }
 if(png) {
  const auto theme=etch_get_theme(ctx);
  ok &= etch_render_headless_png(ctx,380,360,png,etch_rgba8(2,220,120,255));
  auto* img=SDL_LoadPNG(png);
  if(img) {
   unsigned rgb=0,ink=0;
   const int x=int(header.x+header.w-4);
   const EtchColor clear=etch_rgba8(2,220,120,255);
   for(int y=int(header.y+8);y<int(last.y+last.h-8);++y) {
    EtchColor pixel={};
    ok &= SDL_ReadSurfacePixel(img,x,y,&pixel.r,&pixel.g,&pixel.b,&pixel.a) &&
          pixel.a>240 && (std::abs(int(pixel.r)-clear.r)>15 ||
                          std::abs(int(pixel.g)-clear.g)>15 || std::abs(int(pixel.b)-clear.b)>15);
   }
   ok &= sample_pixel(img,x,int(header.y+8),theme.bg_elevated,&rgb) &&
         sample_pixel(img,x,int(body.y+8),theme.bg_table,&rgb) &&
         sample_pixel(img,x,int(last.y+8),etch_color_blend_over(theme.bg_table,theme.zebra_row),&rgb);
   for(int y=int(code.y);y<int(code.y+code.h);++y)for(int px=int(code.x);px<int(code.x+32);++px) {
    uint8_t r=0,g=0,b=0,a=0;
    if(SDL_ReadSurfacePixel(img,px,y,&r,&g,&b,&a) && r>200 && b>160 && g<90)++ink;
   }
   ok &= ink>12;
   SDL_DestroySurface(img);
  } else ok=false;
 }
 const auto click=[&](EtchRect rect,EtchStr expected) {
  f.pointer_pos={rect.x+rect.w*.5f,rect.y+rect.h*.5f};
  f.pointer_down=true;const auto pressed=frame();
  f.pointer_down=false;const auto released=frame();
  return str_equal(pressed.clicked_link,expected) || str_equal(released.clicked_link,expected);
 };
 ok &= click(code,ETCH_STR_LIT("Chip"));
 ok &= click(link,ETCH_STR_LIT("https://example.org"));
 ok &= inlay_doc_set_utf8(doc,ETCH_STR_LIT("`Chip` [Link](https://example.org)"));
 view.width_px=130;view.inline_decorator_fn=nullptr;view.inline_decorator_user_data=nullptr;
 frame();
 const auto short_line=etch_element_bounds(view.id);
 view.inline_decorator_fn=decorate_chip;view.inline_decorator_user_data=&chips;
 const auto estimate_chips=frame();
 const auto wrapped=etch_element_bounds(view.id);
 ok &= wrapped.h>short_line.h*2 && estimate_chips.measured_height_px>short_line.h*2;
 if(!ok)std::printf("Flow: rows=%u, height %.1f / %.1f / %.1f; table y %.1f/%.1f/%.1f; cell x %.1f/%.1f/%.1f; chip %.1fx%.1f; italic %.1f/%.1f\n",
  settled.total_rows,undecorated.h,flow.h,settled.measured_height_px,header.y,body.y,last.y,
  cells[0].x,cells[1].x,cells[2].x,code.w,code.h,italic_width.x,italic_text.w);
 inlay_doc_destroy(doc);etch_destroy_context(ctx);
 return ok;
}
bool theme_pixels(const char* dark_png,const char* light_png) {
 auto cfg=etch_context_desc_default();cfg.headless=true;
 auto* ctx=etch_create_context(&cfg);if(!ctx)return false;
 auto* doc=inlay_doc_create(ctx,nullptr);
 bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT(
  "> [!NOTE]\n> Check panel contrast\n\n```mermaid\nflowchart TD\nA[Plan] --> B[Run]\n```\n"));
 auto view=inlay_doc_view_desc_default();view.id=etch_id("theme-document");view.width_px=232;view.height_px=360;
 const char* paths[]={dark_png,light_png};
 const EtchTheme themes[]={etch_theme_dark(),etch_theme_light()};
 unsigned panel_light[2]={},node_light[2]={},native_light[2]={};
 for(uint32_t i=0;i<2;++i) {
  const auto& theme=themes[i];etch_set_theme(&themes[1-i],ctx);
  auto f=etch_frame_desc_default();f.viewport_w=520;f.viewport_h=360;
  etch_begin_frame(ctx,&f);
  etch_push_theme(&theme);
  auto layout=etch_box_desc_default();layout.width=etch_sizing_fixed(520);layout.gap=40;
  etch_begin_box(&layout);inlay_doc_view(doc,&view);
  auto box=etch_box_desc_default();box.id=etch_id("theme-diagram");
  box.width=etch_sizing_fixed(220);box.height=etch_sizing_fixed(220);etch_begin_box(&box);
  auto desc=inlay_diagram_desc_default();desc.pos={12,12};
  const auto graph=inlay_diagram_view(ETCH_STR_LIT("flowchart TD\nA[Plan] -->|ship| B[Run]\nB -. retry .-> A"),&desc);
  const auto node=graph.valid ? graph.nodes[0].bounds : EtchRect{};
  etch_end_box();etch_end_box();
  etch_pop_theme();etch_end_frame(ctx);
  ok &= graph.valid && etch_render_headless_png(ctx,520,360,paths[i],theme.bg_canvas);
  const auto row=etch_element_bounds(doc_row_id(doc,view.id,doc->views[0].rows[0]));
  const auto native=etch_element_bounds(doc_row_id(doc,view.id,doc->views[0].rows[2]));
  const auto diagram=etch_element_bounds(box.id);
  auto* img=SDL_LoadPNG(paths[i]);
  if(img) {
   ok &= sample_pixel(img,int(row.x+row.w-20),int(row.y+18),theme.bg_elevated,&panel_light[i]);
   ok &= sample_pixel(img,int(diagram.x+node.x+node.w-10),int(diagram.y+node.y+18),
                      theme.bg_elevated,&node_light[i]);
   ok &= sample_pixel(img,int(native.x+22),int(native.y+30),theme.bg_elevated,&native_light[i]);
   SDL_DestroySurface(img);
  } else ok=false;
 }
 ok &= panel_light[1]>panel_light[0]+300 && node_light[1]>node_light[0]+300 &&
       native_light[1]>native_light[0]+300;
 ok &= etch_frame_stats(ctx).draw_call_count<=3;
 inlay_doc_destroy(doc);etch_destroy_context(ctx);
 std::printf("Document/diagram theme pixels: %s (panel %u/%u, node %u/%u, deferred node %u/%u)\n",
             ok ? "PASS" : "FAIL",panel_light[0],panel_light[1],node_light[0],node_light[1],
             native_light[0],native_light[1]);
 return ok;
}
bool streaming_row_id_and_scroll(EtchContext* ctx,InlayDoc* doc) {
 auto view=inlay_doc_view_desc_default();view.id=etch_id("stream-row-id-scroll");
 view.width_px=240;view.height_px=300;
 auto f=etch_frame_desc_default();f.viewport_w=300;f.viewport_h=320;
 const auto frame=[&]() {
  etch_begin_frame(ctx,&f);inlay_doc_view(doc,&view);etch_end_frame(ctx);
 };
 bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT("Stream "));
 frame();
 const InlayMeasure* cached=nullptr;
 for(const auto& m:doc->views)if(m.id==view.id)cached=&m;
 if(!cached || !cached->count)return false;
 const uint32_t para_row_id=doc_row_id(doc,view.id,cached->rows[0]);
 for(int i=0;i<100;++i) {
  ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("a"));
  frame();
  ok &= cached->count>=1 && doc_row_id(doc,view.id,cached->rows[0])==para_row_id;
 }
 ok &= inlay_doc_set_utf8(doc,ETCH_STR_LIT(
  "```cpp\nconst char* wide = \"012345678901234567890123456789012345678901234567890123456789\";\n"));
 frame();frame();
 if(!cached->count)return false;
 const uint32_t code_row_id=doc_row_id(doc,view.id,cached->rows[0]);
 const uint32_t scroll_id=etch_id_index(code_row_id,2);
 etch_set_scroll_offset(scroll_id,{80,0},true);
 frame();
 const float initial_scroll=etch_scroll_offset(scroll_id).x;
 ok &= initial_scroll>40.f;
 for(int i=0;i<200;++i) {
  const EtchStr token=(i%25==24) ? ETCH_STR_LIT("\n") : ETCH_STR_LIT("x");
  ok &= inlay_doc_append_utf8(doc,token);
  frame();
  ok &= cached->count>=1 && doc_row_id(doc,view.id,cached->rows[0])==code_row_id;
  ok &= etch_scroll_offset(scroll_id).x>40.f;
 }
 if(!ok)std::fprintf(stderr,"Streaming row_id/scroll: para=%u code=%u initial_scroll=%.1f final_scroll=%.1f\n",
                     para_row_id,code_row_id,initial_scroll,etch_scroll_offset(scroll_id).x);
 return ok;
}
bool mermaid_single_pass_arena(EtchContext* ctx,InlayDoc* doc) {
 const auto diagram_src=ETCH_STR_LIT("flowchart TD\nA[Start] --> B[Middle]\nB --> C[Finish]\nC --> A");
 auto f=etch_frame_desc_default();f.viewport_w=400;f.viewport_h=400;
 etch_begin_frame(ctx,&f);etch_end_frame(ctx);
 const size_t baseline=etch_frame_stats(ctx).arena_used_bytes;
 etch_begin_frame(ctx,&f);
 const auto ddesc=inlay_diagram_desc_default();
 const auto layout=inlay_diagram_layout(diagram_src,&ddesc);
 etch_end_frame(ctx);
 const size_t single_layout_bytes=etch_frame_stats(ctx).arena_used_bytes-baseline;
 if(!layout.valid || !single_layout_bytes) {
  std::fprintf(stderr,"Mermaid single pass baseline failed: valid=%d bytes=%zu\n",layout.valid,single_layout_bytes);
  return false;
 }
 auto view=inlay_doc_view_desc_default();view.id=etch_id("mermaid-single-pass");
 view.width_px=360;view.height_px=0;
 bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT(
  "```mermaid\nflowchart TD\nA[Start] --> B[Middle]\nB --> C[Finish]\nC --> A\n```\n"));
 etch_begin_frame(ctx,&f);
 inlay_doc_view(doc,&view);
 // Mutate the document before end_frame to confirm deferred diagram draws use frame-arena copies.
 inlay_doc_clear(doc);
 etch_end_frame(ctx);
 const size_t used=etch_frame_stats(ctx).arena_used_bytes;
 ok &= used>single_layout_bytes && used<single_layout_bytes+single_layout_bytes/2;
 if(!ok)std::fprintf(stderr,"Mermaid single pass arena: used=%zu single_layout=%zu\n",used,single_layout_bytes);
 return ok;
}
bool has_prim_color(EtchContext* ctx,uint32_t rgba) {
 for(uint32_t i=0;i<etch_frame_prim_count(ctx);++i) {
  EtchPrim prim={};
  if(etch_get_frame_prim(ctx,i,&prim) && (prim.style.y==rgba || prim.style.z==rgba))return true;
 }
 return false;
}
bool doc_style_customization(EtchContext* ctx,InlayDoc* doc) {
 const InlayDocStyle def=inlay_doc_style_default();
 bool ok=def.font_id==ETCH_INVALID_FONT && def.bold_font_id==ETCH_INVALID_FONT &&
         def.mono_font_id==ETCH_INVALID_FONT && !def.has_background && def.line_height_scale==0.f;
 ok &= inlay_doc_set_utf8(doc,ETCH_STR_LIT(
  "# Styled Heading\n\n"
  "Body with ==highlighted== and [styled link](https://example.org) and `code`.\n\n"
  "> Quote block\n\n"
  "| H | V |\n| --- | --- |\n| A | B |\n\n"
  "```cpp\nint x = 1;\n```\n\n"
  "[^n1]: Muted footnote.\n"));
 InlayDocStyle style=inlay_doc_style_default();
 style.has_background=true;
 style.background=etch_rgba8(0,0,0,0);
 style.text_color=etch_rgba8(210,220,230,255);
 style.muted_color=etch_rgba8(130,140,150,255);
 style.heading_color=etch_rgba8(255,180,64,255);
 style.link_color=etch_rgba8(80,210,255,255);
 style.code_bg=etch_rgba8(24,32,48,180);
 style.code_border=etch_rgba8(70,95,130,255);
 style.quote_bg=etch_rgba8(30,40,36,190);
 style.quote_border=etch_rgba8(90,190,140,255);
 style.table_header_bg=etch_rgba8(55,40,75,200);
 style.table_border=etch_rgba8(120,90,160,255);
 style.highlight_bg=etch_rgba8(255,220,60,120);
 style.selection_color=etch_rgba8(90,140,255,100);
 style.font_id=ETCH_FONT_SANS;
 style.bold_font_id=ETCH_FONT_BOLD;
 style.mono_font_id=ETCH_FONT_MONO;
 style.line_height_scale=1.5f;
 style.heading_scale[0]=1.6f;

 auto view=inlay_doc_view_desc_default();
 view.id=etch_id("doc-style-custom");
 view.width_px=380;view.height_px=0;view.style=&style;
 auto f=etch_frame_desc_default();f.viewport_w=420;f.viewport_h=800;
 InlayDocResult res1={};
 for(int i=0;i<2;++i){etch_begin_frame(ctx,&f);res1=inlay_doc_view(doc,&view);etch_end_frame(ctx);}
 const float h1=res1.measured_height_px;
 ok &= h1>0 && std::fabs(inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px)-h1)<.1f;
 const bool code_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.code_bg)) && has_prim_color(ctx,etch_color_pack_rgba8(style.code_border));
 const bool quote_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.quote_bg)) && has_prim_color(ctx,etch_color_pack_rgba8(style.quote_border));
 const bool table_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.table_header_bg)) && has_prim_color(ctx,etch_color_pack_rgba8(style.table_border));
 const bool hl_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.highlight_bg));
 const bool head_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.heading_color));
 const bool link_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.link_color));
 const bool text_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.text_color));
 const bool muted_ok=has_prim_color(ctx,etch_color_pack_rgba8(style.muted_color));
 ok &= code_ok && quote_ok && table_ok && hl_ok && head_ok && link_ok && text_ok && muted_ok;

 style.line_height_scale=2.2f;
 style.heading_scale[0]=2.8f;
 InlayDocResult res2={};
 for(int i=0;i<2;++i){etch_begin_frame(ctx,&f);res2=inlay_doc_view(doc,&view);etch_end_frame(ctx);}
 ok &= res2.measured_height_px>h1+40.f;
 ok &= std::fabs(inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px)-res2.measured_height_px)<.1f;
 if(!ok)std::fprintf(stderr,"DocStyle check failed: h1=%.1f h2=%.1f code=%d quote=%d table=%d hl=%d head=%d link=%d text=%d muted=%d\n",
                     h1,res2.measured_height_px,code_ok,quote_ok,table_ok,hl_ok,head_ok,link_ok,text_ok,muted_ok);
 return ok;
}
bool doc_images_highlights_footnotes_tasks(EtchContext* ctx,InlayDoc* doc,InlayDoc* streamed) {
 uint32_t pixels[16*8];
 for(uint32_t& px:pixels)px=0xFFE08020u;
 auto tdesc=etch_texture_desc_default();
 tdesc.width=16;tdesc.height=8;tdesc.rgba_pixels=pixels;
 auto tex=etch_create_texture_rgba(ctx,&tdesc);
 if(!tex.texture)return false;

 const auto md=ETCH_STR_LIT(
  "![banner](hero.png)\n\n"
  "![missing](nope.png)\n\n"
  "See ==important== and [^fn1] and inline ![badge](hero.png)!\n\n"
  "- [ ] First task\n"
  "- [x] Second task\n\n"
  "[^fn1]: Footnote explanation line one.\n"
  "  Footnote explanation line two.\n");
 bool ok=inlay_doc_set_utf8(doc,md);
 inlay_doc_clear(streamed);
 for(uint32_t i=0;i<md.length;++i)ok &= inlay_doc_append_utf8(streamed,{md.chars+i,1,false});
 ok &= inlay_doc_block_count(doc)==5 && inlay_doc_block_count(streamed)==5;
 for(uint32_t i=0;i<5;++i) {
  const auto a=*inlay_doc_block(doc,i),b=*inlay_doc_block(streamed,i);
  ok &= a.kind==b.kind && a.offset==b.offset && a.length==b.length &&
        a.content_offset==b.content_offset && a.content_length==b.content_length &&
        a.language_offset==b.language_offset && a.language_length==b.language_length;
 }
 ok &= inlay_doc_block(doc,0)->kind==INLAY_IMAGE && inlay_doc_block(doc,1)->kind==INLAY_IMAGE &&
       inlay_doc_block(doc,3)->kind==INLAY_TASK_LIST && inlay_doc_block(doc,4)->kind==INLAY_FOOTNOTE;

 struct ImageCtx {struct SDL_GPUTexture* tex;uint32_t hits;};
 ImageCtx img_ctx={tex.texture,0};
 struct TaskCtx {uint32_t calls,block,item;bool checked;};
 TaskCtx task_ctx={0,999,999,false};

 auto view=inlay_doc_view_desc_default();
 view.id=etch_id("doc-v09-features");
 view.width_px=240;view.height_px=0;
 view.image_resolver_user_data=&img_ctx;
 view.image_resolver_fn=[](EtchStr,EtchStr src,InlayImageResult* out,void* user) {
  auto& c=*static_cast<ImageCtx*>(user);
  ++c.hits;
  if(!str_equal(src,ETCH_STR_LIT("hero.png")))return false;
  out->texture=c.tex;out->width_px=800.f;out->height_px=400.f;out->radius_px=8.f;
  return true;
 };
 view.task_toggle_user_data=&task_ctx;
 view.task_toggle_fn=[](uint32_t block,uint32_t item,bool checked,void* user) {
  auto& t=*static_cast<TaskCtx*>(user);
  ++t.calls;t.block=block;t.item=item;t.checked=checked;
 };

 auto f=etch_frame_desc_default();f.viewport_w=320;f.viewport_h=720;
 for(int i=0;i<2;++i){etch_begin_frame(ctx,&f);inlay_doc_view(doc,&view);etch_end_frame(ctx);}
 ok &= img_ctx.hits>=2;
 const InlayMeasure* found_view=nullptr;
 for(const auto& m:doc->views)if(m.id==view.id)found_view=&m;
 if(!found_view) {etch_destroy_loaded_texture(ctx,&tex);return false;}
 const auto& cached=*found_view;
 const uint32_t banner_id=etch_id_index(doc_row_id(doc,view.id,cached.rows[0]),1);
 const auto banner_bounds=etch_element_bounds(banner_id);
 const float expected_w=view.width_px-2*view.block_padding_px;
 ok &= std::fabs(banner_bounds.w-expected_w)<.5f && std::fabs(banner_bounds.h-expected_w*0.5f)<.5f;
 ok &= contains_label(ctx,ETCH_STR_LIT("[missing]"));
 ok &= contains_label(ctx,ETCH_STR_LIT("[fn1]:"));
 const InlayRow* fn_row=nullptr;
 for(uint32_t i=0;i<cached.count;++i)if(cached.rows[i].kind==INLAY_FOOTNOTE){fn_row=&cached.rows[i];break;}
 ok &= fn_row!=nullptr;
 const uint32_t fn_block_row_id=fn_row ? doc_row_id(doc,view.id,*fn_row) : 0u;
 const auto fn_l1=etch_element_bounds(etch_id_index(fn_block_row_id,1));
 const auto fn_l2=etch_element_bounds(etch_id_index(fn_block_row_id,2));
 ok &= fn_l1.h>0 && fn_l2.h>0 && fn_l2.y>=fn_l1.y+fn_l1.h;

 // Click the [^fn1] footnote reference badge
 const uint32_t para_row_id=doc_row_id(doc,view.id,cached.rows[2]);
 const uint32_t para_line_id=etch_id_index(para_row_id,1);
 uint32_t fn_offset=0;
 for(uint32_t i=0;i<cached.token_count;++i) {
  if(cached.tokens[i].flags&InlayInlineFootnote){fn_offset=cached.tokens[i].offset;break;}
 }
 ok &= fn_offset>0;
 const uint32_t fn_span_id=etch_id_index(para_line_id,fn_offset+1);
 const auto fn_bounds=etch_element_bounds(fn_span_id);
 if(fn_bounds.w>0 && fn_bounds.h>0) {
  f.pointer_pos={fn_bounds.x+fn_bounds.w*.5f,fn_bounds.y+fn_bounds.h*.5f};
  f.pointer_down=true;
  etch_begin_frame(ctx,&f);const auto p_res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
  f.pointer_down=false;
  etch_begin_frame(ctx,&f);const auto r_res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
  ok &= str_equal(p_res.clicked_link,ETCH_STR_LIT("[^fn1]")) ||
        str_equal(r_res.clicked_link,ETCH_STR_LIT("[^fn1]"));
 } else ok=false;

 // Click the first task-list checkbox (- [ ] First task)
 const uint32_t task_row_id=doc_row_id(doc,view.id,cached.rows[3]);
 const uint32_t cb_id=etch_id_index(etch_id_index(task_row_id,1),0x5441u);
 ok &= etch_perform_a11y_action(cb_id,ETCH_A11Y_ACT_PRESS,0,{},ctx,nullptr);
 f.pointer_pos={0,0};
 etch_begin_frame(ctx,&f);inlay_doc_view(doc,&view);etch_end_frame(ctx);
 ok &= task_ctx.calls==1 && task_ctx.block==3 && task_ctx.item==0 && task_ctx.checked;
 const auto updated_src=inlay_doc_source(doc);
 bool toggled_src=false;
 const char* needle="- [x] First task";const uint32_t nlen=uint32_t(std::strlen(needle));
 for(uint32_t i=0;i+nlen<=updated_src.length;++i)if(!std::memcmp(updated_src.chars+i,needle,nlen)){toggled_src=true;break;}
 ok &= toggled_src;

 etch_destroy_loaded_texture(ctx,&tex);
 if(!ok)std::fprintf(stderr,"v0.9 features failed: banner=%.1fx%.1f fn=%.1fx%.1f task=%u/%u/%u/%d\n",
                     banner_bounds.w,banner_bounds.h,fn_bounds.w,fn_bounds.h,
                     task_ctx.calls,task_ctx.block,task_ctx.item,task_ctx.checked);
 return ok;
}
bool doc_v010_sticky_custom_fence_and_syntax(EtchContext* ctx,InlayDoc* doc) {
 bool ok=true;
 // 1. Sticky-bottom virtual list stays pinned across appends, detaches on wheel-up, re-attaches via API.
 inlay_doc_clear(doc);
 for(int i=0;i<24;++i)ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("Paragraph line for sticky bottom streaming test.\n\n"));
 auto view=inlay_doc_view_desc_default();
 view.id=etch_id("v010-sticky");
 view.width_px=260;view.height_px=140;view.stick_to_bottom=true;
 auto f=etch_frame_desc_default();f.viewport_w=320;f.viewport_h=200;
 InlayDocResult res={};
 for(int i=0;i<4;++i) {
  etch_begin_frame(ctx,&f);res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
 }
 const bool s1_init=res.at_bottom && inlay_doc_view_is_at_bottom(view.id);
 ok &= s1_init;
 const double bottom_before=etch_virtual_list_scroll_offset(view.id);
 ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("Streamed delta paragraph one.\n\nStreamed delta paragraph two.\n\n"));
 for(int i=0;i<3;++i) {
  etch_begin_frame(ctx,&f);res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
 }
 const double bottom_after=etch_virtual_list_scroll_offset(view.id);
 const bool s1_grow=res.at_bottom && inlay_doc_view_is_at_bottom(view.id) && bottom_after>bottom_before;
 ok &= s1_grow;
 // Scroll up to detach sticky-bottom.
 const auto list_bounds=etch_element_bounds(view.id);
 f.pointer_pos={list_bounds.x+list_bounds.w*0.5f,list_bounds.y+list_bounds.h*0.5f};
 f.scroll_delta={0.0f,4.0f};
 etch_begin_frame(ctx,&f);res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
 f.scroll_delta={0.0f,0.0f};
 etch_begin_frame(ctx,&f);res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
 const bool s1_detach=!res.at_bottom && !inlay_doc_view_is_at_bottom(view.id);
 ok &= s1_detach;
 inlay_doc_view_scroll_to_bottom(view.id,true);
 etch_begin_frame(ctx,&f);res=inlay_doc_view(doc,&view);etch_end_frame(ctx);
 const bool s1_reattach=res.at_bottom && inlay_doc_view_is_at_bottom(view.id);
 ok &= s1_reattach;
 if(!(s1_init && s1_grow && s1_detach && s1_reattach)) {
  std::fprintf(stderr,"v0.10 sticky failed: init=%d grow=%d detach=%d reattach=%d before=%.1f after=%.1f cur=%.1f bounds=%.1fx%.1f\n",
               s1_init,s1_grow,s1_detach,s1_reattach,bottom_before,bottom_after,
               etch_virtual_list_scroll_offset(view.id),list_bounds.w,list_bounds.h);
 }

 // 2. Custom fence rendering hook (single unsplit row >16 lines) + Code header slot + multi-language syntax.
 struct Hooks {
  uint32_t est_calls=0,render_calls=0,header_calls=0;
  uint32_t last_custom_body_len=0;
  bool diff_header_seen=false;
 } hooks;
 const auto custom_src=ETCH_STR_LIT(
  "```omp-tool\nline1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10\n"
  "line11\nline12\nline13\nline14\nline15\nline16\nline17\nline18\nline19\nline20\n```\n\n"
  "```rust\n// rust comment\npub fn check() -> usize { let x = 0x2A; return x; }\n```\n\n"
  "```diff\n@@ -1,2 +1,2 @@\n-old_line\n+new_line\n```\n");
 ok &= inlay_doc_set_utf8(doc,custom_src);
 auto cview=inlay_doc_view_desc_default();
 cview.id=etch_id("v010-custom");cview.width_px=360;cview.height_px=0;
 cview.custom_fence_user_data=&hooks;
 cview.custom_fence_estimate_fn=[](EtchStr lang,EtchStr,float,float,void* user) -> float {
  auto& h=*static_cast<Hooks*>(user);
  ++h.est_calls;
  return str_equal(lang,ETCH_STR_LIT("omp-tool")) ? 56.f : 0.f;
 };
 cview.custom_fence_render_fn=[](uint32_t,uint32_t row_id,EtchStr lang,EtchStr body,float avail_w,float,void* user) {
  auto& h=*static_cast<Hooks*>(user);
  ++h.render_calls;
  if(str_equal(lang,ETCH_STR_LIT("omp-tool")))h.last_custom_body_len=body.length;
  auto card=etch_box_desc_default();
  card.id=etch_id_index(row_id,77);
  card.width=etch_sizing_fixed(avail_w);
  card.height=etch_sizing_fixed(72.f);
  etch_box(&card);
 };
 cview.code_header_user_data=&hooks;
 cview.code_header_fn=[](uint32_t,uint32_t row_id,EtchStr lang,EtchStr,void* user) {
  auto& h=*static_cast<Hooks*>(user);
  ++h.header_calls;
  if(str_equal(lang,ETCH_STR_LIT("diff")))h.diff_header_seen=true;
  auto pill=etch_box_desc_default();
  pill.id=etch_id_index(row_id,88);
  pill.width=etch_sizing_fixed(28.f);pill.height=etch_sizing_fixed(18.f);
  etch_box(&pill);
 };
 auto f2=etch_frame_desc_default();f2.viewport_w=420;f2.viewport_h=640;
 InlayDocResult cres={};
 for(int i=0;i<3;++i) {
  etch_begin_frame(ctx,&f2);cres=inlay_doc_view(doc,&cview);etch_end_frame(ctx);
 }
 // 3 blocks => exactly 3 rows (custom fence with 20 lines was NOT split into 16-line chunks).
 ok &= cres.total_rows==3 && hooks.est_calls>0 && hooks.render_calls>0 && hooks.last_custom_body_len>100;
 ok &= hooks.header_calls>0 && hooks.diff_header_seen;
 // Verify multi-language & diff syntax colors were emitted.
 const EtchTheme thm=etch_get_theme(ctx);
 const uint32_t col_kw=etch_color_pack_rgba8(thm.syntax_keyword);
 const uint32_t col_cm=etch_color_pack_rgba8(thm.syntax_comment);
 const uint32_t col_num=etch_color_pack_rgba8(thm.syntax_number);
 const uint32_t col_ok=etch_color_pack_rgba8(thm.syntax_string);
 const uint32_t col_err=etch_color_pack_rgba8(thm.danger);
 bool seen_kw=false,seen_cm=false,seen_num=false,seen_ok=false,seen_err=false;
 for(uint32_t j=0;j<etch_frame_prim_count(ctx);++j) {
  EtchPrim prim={};
  if(!etch_get_frame_prim(ctx,j,&prim))continue;
  if(prim.style.y==col_kw)seen_kw=true;
  if(prim.style.y==col_cm)seen_cm=true;
  if(prim.style.y==col_num)seen_num=true;
  if(prim.style.y==col_ok)seen_ok=true;
  if(prim.style.y==col_err)seen_err=true;
 }
 ok &= seen_kw && seen_cm && seen_num && seen_ok && seen_err;
 if(!ok)std::fprintf(stderr,"v0.10 features failed: rows=%u est=%u ren=%u hdr=%u colors=%d%d%d%d%d\n",
                     cres.total_rows,hooks.est_calls,hooks.render_calls,hooks.header_calls,
                     seen_kw,seen_cm,seen_num,seen_ok,seen_err);
 return ok;
}
}
int main(int argc,char** argv) {
 if(argc==4) {
  const bool theme=theme_pixels(argv[1],argv[2]);
  return flow_document(argv[3]) && theme ? 0 : 1;
 }
 auto config=etch_context_desc_default();config.headless=true;
 auto* ctx=etch_create_context(&config);if(!ctx)return 1;
 auto* reference=inlay_doc_create(ctx,nullptr);auto* streamed=inlay_doc_create(ctx,nullptr);
 if(!reference || !streamed)return 1;
 const char* section="# Heading 中文 😀\n\nA **bold** paragraph with `code` and [link](https://example.org).\n\n> [!NOTE]\n> Quote text\n\n- [x] task\n- [ ] other\n\n| Name | Value |\n| :--- | ---: |\n| A | 42 |\n\n```cpp\nint x = 1; // 中文\n```\n\n<details>\n<summary>Details</summary>\nMore text\n</details>\n\n---\n\n";
 char input[65536]={};size_t size=0;
 while(size+std::strlen(section)<32768) {std::memcpy(input+size,section,std::strlen(section));size+=std::strlen(section);}
 bool ok=inlay_doc_set_utf8(reference,{input,uint32_t(size),false});
 bool kinds[11]={};for(uint32_t i=0;i<inlay_doc_block_count(reference);++i)kinds[inlay_doc_block(reference,i)->kind]=true;
 ok &= kinds[INLAY_PARAGRAPH] && kinds[INLAY_HEADING] && kinds[INLAY_CODE_BLOCK] && kinds[INLAY_ALERT] && kinds[INLAY_TASK_LIST] && kinds[INLAY_TABLE] && kinds[INLAY_THEMATIC_BREAK] && kinds[INLAY_DETAILS];
 const uint32_t chunks[]={1,3,7,64};
 for(uint32_t chunk: chunks) {
  inlay_doc_clear(streamed);
  for(uint32_t pos=0;pos<size;) {
   const uint32_t n=uint32_t(size-pos)<chunk ? uint32_t(size-pos) : chunk;
   if(!inlay_doc_append_utf8(streamed,{input+pos,n,false})) {ok=false;break;}pos+=n;
  }
  ok &= inlay_doc_block_count(streamed)==inlay_doc_block_count(reference);
  const auto a=inlay_doc_source(reference),b=inlay_doc_source(streamed);
  ok &= a.length==b.length && std::memcmp(a.chars,b.chars,a.length)==0;
  for(uint32_t i=0;i<inlay_doc_block_count(reference) && i<inlay_doc_block_count(streamed);++i) {
   const auto x=*inlay_doc_block(reference,i),y=*inlay_doc_block(streamed,i);
   if(x.kind!=y.kind || x.offset!=y.offset || x.length!=y.length || x.content_offset!=y.content_offset || x.content_length!=y.content_length || x.complete!=y.complete || x.level!=y.level || x.language_length!=y.language_length) {
    std::fprintf(stderr,"chunk=%u block=%u kind=%u/%u offset=%u/%u length=%u/%u\n",chunk,i,unsigned(x.kind),unsigned(y.kind),x.offset,y.offset,x.length,y.length);ok=false;break;
   }
  }
 }
 inlay_doc_clear(streamed);
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("  ```cpp\nint x;\n  ```\n\nnext\n\n"));
 ok &= inlay_doc_block_count(streamed)==2 && inlay_doc_block(streamed,0)->complete && inlay_doc_block(streamed,1)->kind==INLAY_PARAGRAPH;
 const auto small=InlayDocConfig{4,1};auto* alias=inlay_doc_create(ctx,&small);
 ok &= alias && inlay_doc_set_utf8(alias,ETCH_STR_LIT("abc\n\n"));
 if(alias) {
  ok &= inlay_doc_append_utf8(alias,inlay_doc_source(alias));
  const auto source=inlay_doc_source(alias);ok &= source.length==10 && !std::memcmp(source.chars,"abc\n\nabc\n\n",10);
  inlay_doc_destroy(alias);
 }
 // A prefix ending mid-codepoint remains pending until the following append.
 inlay_doc_clear(streamed);
 ok &= inlay_doc_append_utf8(streamed,{"\xF0\x9F",2,true}) && inlay_doc_source(streamed).length==0;
 ok &= inlay_doc_append_utf8(streamed,{"\x98\x80",2,true}) && inlay_doc_source(streamed).length==4;
 ok &= !inlay_doc_append_utf8(streamed,{"\xED\xA0\x80",3,true});
 inlay_doc_clear(streamed);
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("# Frozen\n\nTail"));
 const auto frozen=*inlay_doc_block(streamed,0);
 for(int i=0;i<100;++i)ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT(" text"));
 const auto still_frozen=*inlay_doc_block(streamed,0);
 ok &= frozen.offset==still_frozen.offset && frozen.length==still_frozen.length && frozen.kind==still_frozen.kind && still_frozen.complete;
 const auto count=inlay_doc_block_count(streamed);
 ok &= !inlay_doc_append_utf8(streamed,{"\xC0\x80",2,true}) && inlay_doc_block_count(streamed)==count;
 // Persistent view rows preserve measured frozen content across tail growth.
 inlay_doc_clear(streamed);
 for(int i=0;i<300;++i)ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT("Long **frozen paragraph** with `inline code` and 中文.\n\n"));
 auto view=inlay_doc_view_desc_default();view.id=etch_id("stream_view");
 view.width_px=200;view.height_px=160;view.font_size_px=24;
 double scroll=0;
 for(int frame=0;frame<32;++frame) {
  if(frame==5)ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT("Tail append\n\n"));
  auto f=etch_frame_desc_default();f.viewport_w=300;f.viewport_h=200;etch_begin_frame(ctx,&f);
  const auto result=inlay_doc_view(streamed,&view);ok &= result.visible_end>result.visible_first;
  etch_end_frame(ctx);
  if(frame==2)etch_virtual_list_scroll_to_row(view.id,240,28,ETCH_ALIGN_Y_TOP);
  if(frame==4)scroll=etch_virtual_list_scroll_offset(view.id);
  if(frame==5)ok &= std::fabs(etch_virtual_list_scroll_offset(view.id)-scroll)<1;
 }
 ok &= scroll>0 && !etch_needs_redraw(ctx);
 ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT("wake")) && etch_needs_redraw(ctx);
 for(int frame=0;frame<32;++frame) {auto redraw_frame=etch_frame_desc_default();redraw_frame.viewport_w=300;redraw_frame.viewport_h=200;etch_begin_frame(ctx,&redraw_frame);inlay_doc_view(streamed,&view);etch_end_frame(ctx);}
 ok &= !etch_needs_redraw(ctx);
 inlay_doc_clear(streamed);ok &= etch_needs_redraw(ctx);
 // A details block has one header and retains every continuation row.
 inlay_doc_set_utf8(streamed,ETCH_STR_LIT("<details>\n<summary>Title</summary>\n"));
 for(int i=0;i<100;++i)inlay_doc_append_utf8(streamed,ETCH_STR_LIT("body **bold** text\n"));
 inlay_doc_append_utf8(streamed,ETCH_STR_LIT("</details>\n"));
 auto f=etch_frame_desc_default();f.viewport_w=300;f.viewport_h=200;etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);
 ok &= streamed->views[0].count>=100;
 uint32_t headers=0;for(uint32_t i=0;i<streamed->views[0].count;++i)headers+=streamed->views[0].rows[i].first ? 1u : 0u;
 ok &= headers==1;
 streamed->views[0].open[0]=false;streamed->views[0].revision=streamed->revision-1;
 etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);
 ok &= streamed->views[0].count==1;
 // Narrow, large-font content remains split below the GPU layout height limit.
 inlay_doc_clear(streamed);char narrow[4096];std::memset(narrow,'a',sizeof(narrow));
 ok &= inlay_doc_append_utf8(streamed,{narrow,sizeof(narrow),false});
 auto narrow_view=view;narrow_view.width_px=32;narrow_view.font_size_px=128;
 etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&narrow_view);etch_end_frame(ctx);
 ok &= streamed->views[0].count>100;
 for(uint32_t i=0;i<streamed->views[0].count;++i)ok &= streamed->views[0].rows[i].length<=35;

 // Space-separated prose wraps within the requested viewport width.
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("This ordinary paragraph has many short words and must wrap inside the narrow document viewport with every word readable."));
 for(int frame=0;frame<3;++frame) {etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);}
 const auto rr=streamed->views[0].rows[0];
 const auto row_id=doc_row_id(streamed,view.id,rr);
 const auto row_bounds=etch_element_bounds(row_id);
 const auto text_bounds=etch_element_bounds(etch_id_index(row_id,1));
 ok &= row_bounds.w<=view.width_px+.1f && text_bounds.h>view.font_size_px*1.5f;
 // Closing fences at EOF stay out of code content and can reopen on append.
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("```cpp\nint x;\n```"));
 ok &= inlay_doc_block(streamed,0)->content_length==7 && !inlay_doc_block(streamed,0)->complete;
 ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT("x"));
 ok &= inlay_doc_block(streamed,0)->content_length==11;
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("[x](https://example.org/"));
 char url[4096];std::memset(url,'a',sizeof(url));
 ok &= inlay_doc_append_utf8(streamed,{url,sizeof(url),false});
 ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT(")"));
 etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);
 ok &= streamed->views[0].count==1;
 // Oversized wrapped table rows scroll internally and leave the next block reachable.
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("| H | V |\n| --- | --- |\n| "));
 char large_cell[30000];for(uint32_t i=0;i<sizeof(large_cell);++i)large_cell[i]=i%5==4 ? ' ' : 'x';
 ok &= inlay_doc_append_utf8(streamed,{large_cell,sizeof(large_cell),false});
 ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT(" | end |\n\nTail"));
 for(int frame=0;frame<8;++frame) {etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);}
 ok &= streamed->views[0].count==4 && streamed->views[0].heights[2]<=4120;
 etch_virtual_list_scroll_to_row(view.id,3,28,ETCH_ALIGN_Y_BOTTOM);
 etch_begin_frame(ctx,&f);const auto tail_visible=inlay_doc_view(streamed,&view);etch_end_frame(ctx);
 ok &= tail_visible.visible_end==4;
 // A wide table cell remains in its logical row rather than creating extra cells.
 inlay_doc_clear(streamed);
 ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT("| Header | Value |\n| --- | --- |\n| "));
 char cell[4096];std::memset(cell,'x',sizeof(cell));
 ok &= inlay_doc_append_utf8(streamed,{cell,sizeof(cell),false});
 ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT(" | **end** |\n\nTail"));
 etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);
 auto& cached=streamed->views[0];
 ok &= cached.count==4 && cached.rows[2].length>4096;
 const uint32_t frozen_tail=cached.parsed_tail;
 uint32_t frozen_tokens=0;
 while(frozen_tokens<cached.token_count && cached.tokens[frozen_tokens].offset<frozen_tail)++frozen_tokens;
 InlayInline saved[16]={};
 ok &= frozen_tokens>0 && frozen_tokens<=16;
 if(frozen_tokens<=16)std::memcpy(saved,cached.tokens,frozen_tokens*sizeof(InlayInline));
 for(int append=0;append<8;++append) {
  ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT(" **more**"));
  etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&view);etch_end_frame(ctx);
  if(frozen_tokens<=16)ok &= !std::memcmp(saved,cached.tokens,frozen_tokens*sizeof(InlayInline));
 }
 // Offscreen diagrams must not consume frame scratch for native layout.
 inlay_doc_clear(streamed);
 for(uint32_t i=0;i<256;++i)ok &= inlay_doc_append_utf8(streamed,ETCH_STR_LIT("```mermaid\nflowchart TD\nA --> B\n```\n"));
 etch_begin_frame(ctx,&f);const auto many_diagrams=inlay_doc_view(streamed,&view);
 const auto remaining_diagram_desc=inlay_diagram_desc_default();
 ok &= many_diagrams.total_rows==256 && inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA --> B"),&remaining_diagram_desc).valid;
 etch_end_frame(ctx);ok &= etch_frame_stats(ctx).arena_used_bytes<1024u*1024u;
 // Math block delimiters retain streaming equivalence at every byte boundary.
 const auto math_source=ETCH_STR_LIT("$$\n\\frac{x}{y}\n$$\n\n\\[\nx^2\n\\]\n");
 inlay_doc_set_utf8(reference,math_source);inlay_doc_clear(streamed);
 for(uint32_t i=0;i<math_source.length;++i)ok &= inlay_doc_append_utf8(streamed,{math_source.chars+i,1,false});
 ok &= inlay_doc_block_count(reference)==2 && inlay_doc_block_count(streamed)==2;
 for(uint32_t i=0;i<2;++i){const auto a=*inlay_doc_block(reference,i),b=*inlay_doc_block(streamed,i);ok &= a.kind==INLAY_MATH_BLOCK && a.kind==b.kind && a.content_offset==b.content_offset && a.content_length==b.content_length;}
 const EtchStr closed_math[]={ETCH_STR_LIT("$$x^2$$\n\n# Next\n"),ETCH_STR_LIT("$$x^2$$ \t\n\n# Next\n"),ETCH_STR_LIT("\\[x^2\\] \n\n# Next\n"),ETCH_STR_LIT("  $$\nx^2\n  $$\n\n# Next\n")};
 for(const auto source:closed_math){inlay_doc_set_utf8(reference,source);inlay_doc_clear(streamed);for(uint32_t i=0;i<source.length;++i)ok &= inlay_doc_append_utf8(streamed,{source.chars+i,1,false});ok &= inlay_doc_block_count(reference)==2 && inlay_doc_block_count(streamed)==2 && inlay_doc_block(streamed,1)->kind==INLAY_HEADING;}
 // Inline math tokens and Mermaid blocks render in a virtualized native view.
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("Inline $x^2$ and \\(y_1\\).\n\n```mermaid\nflowchart LR\nA --> B\n```\n"));
 etch_begin_frame(ctx,&f);const auto native=inlay_doc_view(streamed,&view);etch_end_frame(ctx);
 bool math_token=false;for(uint32_t i=0;i<streamed->views[0].token_count;++i)math_token |= (streamed->views[0].tokens[i].flags&InlayInlineMath)!=0;
 ok &= math_token && native.total_rows>=2;
 // Row splits retain both bytes of parenthesized inline-math delimiters.
 ok &= inlay_doc_set_utf8(streamed,ETCH_STR_LIT("\\(abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\\)"));
 etch_begin_frame(ctx,&f);inlay_doc_view(streamed,&narrow_view);etch_end_frame(ctx);
 ok &= streamed->views[0].count==1 && streamed->views[0].rows[0].length==inlay_doc_source(streamed).length;
 // Native math reports baseline geometry and bounds recursive input.
 etch_begin_frame(ctx,&f);
 const auto atom_math=inlay_measure_math(ETCH_STR_LIT("x"),24);
 const auto fraction_math=inlay_measure_math(ETCH_STR_LIT("\\frac{x+1}{y}"),24);
 const auto matrix_math=inlay_measure_math(ETCH_STR_LIT("\\begin{matrix}a&b\\\\c&d\\end{matrix}"),24);
 const auto delimited_math=inlay_measure_math(ETCH_STR_LIT("\\left(\\frac{x}{y}\\right)"),24);
 ok &= delimited_math.valid && delimited_math.width>fraction_math.width*.2f;
 const auto bracket_math=inlay_measure_math(ETCH_STR_LIT("\\begin{bmatrix}a&b\\\\c&d\\end{bmatrix}"),24);
 const auto tall_script=inlay_measure_math(ETCH_STR_LIT("x^{\\begin{matrix}a\\\\b\\\\c\\\\d\\end{matrix}}"),24);
 ok &= bracket_math.valid && bracket_math.width>matrix_math.width && tall_script.valid && tall_script.height>atom_math.height;
 ok &= matrix_math.valid && matrix_math.width>atom_math.width && matrix_math.height>atom_math.height;
 const auto root_math=inlay_measure_math(ETCH_STR_LIT("\\sqrt{x}"),24);
 const auto script_math=inlay_measure_math(ETCH_STR_LIT("x_1^2"),24);
 ok &= atom_math.valid && fraction_math.valid && root_math.valid && script_math.valid;
 ok &= fraction_math.height>atom_math.height && fraction_math.baseline>atom_math.baseline;
 ok &= root_math.width>atom_math.width && script_math.height>atom_math.height;
 ok &= !inlay_measure_math(ETCH_STR_LIT("\\frac{x}"),24).valid;
 char recursive_math[1024];uint32_t recursive_size=0;
 for(int level=0;level<100;++level){std::memcpy(recursive_math+recursive_size,"\\sqrt",5);recursive_size+=5;}
 recursive_math[recursive_size++]='x';
 ok &= !inlay_measure_math({recursive_math,recursive_size,false},24).valid;
 const auto diagram_desc=inlay_diagram_desc_default();
 const auto sample_graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA[Stream] --> B[Parse]\nA --> C[Validate]\nB --> D[Render]\nC --> D\nD --> A"),&diagram_desc);
 ok &= sample_graph.valid;
 const auto graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA[Start] --> B[Middle]\nB --> C[Finish]\nC --> A"),&diagram_desc);
 ok &= graph.valid && graph.node_count==3 && graph.edge_count==3;
 for(uint32_t i=0;i<graph.node_count;++i)for(uint32_t j=i+1;j<graph.node_count;++j){const auto a=graph.nodes[i].bounds,b=graph.nodes[j].bounds;ok &= a.x+a.w<=b.x || b.x+b.w<=a.x || a.y+a.h<=b.y || b.y+b.h<=a.y;}
 const auto sequence=inlay_diagram_layout(ETCH_STR_LIT("sequenceDiagram\nparticipant A as Client\nparticipant B as Server\nA->>B:Request\nB-->>A:Reply"),&diagram_desc);
 ok &= sequence.valid && sequence.sequence && sequence.node_count==2 && sequence.edge_count==2;
 if(sequence.valid && sequence.edge_count==2)ok &= !sequence.edges[0].dashed && sequence.edges[1].dashed;
 const auto labeled=inlay_diagram_layout(ETCH_STR_LIT(
  "flowchart LR\nsubgraph Outer\nA -->|fetch data| B\nsubgraph Inner\n"
  "B -- parse data --> C -.-> D\nend\nD -. retry .-> A\nend"),&diagram_desc);
 ok &= labeled.valid && labeled.node_count==4 && labeled.edge_count==4;
 if(labeled.valid && labeled.edge_count==4) {
  ok &= str_equal(labeled.edges[0].label,ETCH_STR_LIT("fetch data")) && !labeled.edges[0].dashed;
  ok &= str_equal(labeled.edges[1].label,ETCH_STR_LIT("parse data")) && !labeled.edges[1].dashed;
  ok &= labeled.edges[2].dashed && !labeled.edges[2].label.length;
  ok &= labeled.edges[3].dashed && labeled.edges[3].reversed &&
        str_equal(labeled.edges[3].label,ETCH_STR_LIT("retry"));
 }
 ok &= !inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nsubgraph Open\nA --> B"),&diagram_desc).valid;
 const auto self_graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA --> A"),&diagram_desc);
 ok &= self_graph.valid && self_graph.width>=self_graph.nodes[0].bounds.w+40;
 const auto horizontal=inlay_diagram_layout(ETCH_STR_LIT("flowchart LR\nA --> B --> C"),&diagram_desc);
 ok &= horizontal.valid && horizontal.horizontal && horizontal.nodes[0].bounds.x<horizontal.nodes[2].bounds.x;
 bool feedback=false;for(uint32_t i=0;i<graph.edge_count;++i)feedback |= graph.edges[i].reversed;
 ok &= feedback && !inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA -->"),&diagram_desc).valid;
 etch_end_frame(ctx);
 ok &= markdown_and_scroll(ctx,streamed);
 ok &= inline_decorator_row_boundaries(ctx,streamed);
 ok &= inline_decorator_wrapped_code(ctx,streamed);
 ok &= inline_decorator_replacement_height(ctx,streamed);
 ok &= flow_height_after_refresh(ctx,streamed);
 ok &= streaming_row_id_and_scroll(ctx,streamed);
 ok &= mermaid_single_pass_arena(ctx,streamed);
 ok &= doc_style_customization(ctx,streamed);
 ok &= doc_images_highlights_footnotes_tasks(ctx,reference,streamed);
 ok &= doc_v010_sticky_custom_fence_and_syntax(ctx,streamed);
 inlay_doc_destroy(reference);inlay_doc_destroy(streamed);etch_destroy_context(ctx);
 ok &= flow_document(nullptr);
 std::printf("32KB streaming equivalence: %s\n",ok ? "PASS" : "FAIL");return ok ? 0 : 1;
}
