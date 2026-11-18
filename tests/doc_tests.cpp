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
uint32_t doc_row_id(uint32_t view,const InlayRow& row) {
 return etch_id_index(etch_id_index(view,row.offset+1),row.length+1);
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
 const uint32_t id=doc_row_id(view.id,row),scroll_id=etch_id_index(id,2);
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
 const auto bounds=[&](uint32_t index){return etch_element_bounds(doc_row_id(view.id,cached.rows[index]));};
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
 const auto bold_cell=etch_id_index(doc_row_id(view.id,cached.rows[2]),1);
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
 const auto italic_text=etch_element_bounds(etch_id_index(doc_row_id(view.id,cached.rows[5]),1));
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
  const auto row=etch_element_bounds(doc_row_id(view.id,doc->views[0].rows[0]));
  const auto native=etch_element_bounds(doc_row_id(view.id,doc->views[0].rows[2]));
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
 const auto row_id=etch_id_index(etch_id_index(view.id,rr.offset+1),rr.length+1);
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
 inlay_doc_destroy(reference);inlay_doc_destroy(streamed);etch_destroy_context(ctx);
 ok &= flow_document(nullptr);
 std::printf("32KB streaming equivalence: %s\n",ok ? "PASS" : "FAIL");return ok ? 0 : 1;
}
