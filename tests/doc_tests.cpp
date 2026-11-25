#include <inlay/inlay.h>
#include <cstdio>
#include <cstdlib>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include "../src/inlay_internal.h"
#include <cmath>
#include <cstring>
namespace {
bool inbox_regression() {
 if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) return false;
 const uint32_t wake_type = SDL_RegisterEvents(1);
 InlayInboxDesc desc = inlay_inbox_desc_default();
 desc.max_messages = 2u;
 desc.max_bytes = 12u;
 desc.wake_event_type = wake_type;
 InlayInbox* inbox = wake_type ? inlay_inbox_create(&desc) : nullptr;
 bool ok = inbox != nullptr;
 if (inbox) {
  SDL_Thread* worker = SDL_CreateThread([](void* user) -> int {
   auto* queue = static_cast<InlayInbox*>(user);
   SDL_Delay(20);
   char first[] = "abcdefg";
   const auto sent = inlay_inbox_post(queue, 1u, 10u, first, 7u);
   first[0] = 'X';
   return sent == INLAY_INBOX_OK &&
          inlay_inbox_post(queue, 1u, 11u, "hijk", 4u) == INLAY_INBOX_OK &&
          inlay_inbox_post(queue, 2u, 12u, nullptr, 0u) == INLAY_INBOX_FULL ? 0 : 1;
  }, "inlay inbox producer", inbox);
  bool woke = false;
  const uint64_t deadline = SDL_GetTicks() + 1000u;
  while (worker && SDL_GetTicks() < deadline) {
   SDL_Event event = {};
   if (SDL_WaitEventTimeout(&event, 20) && event.type == wake_type) {
    woke = true;
    break;
   }
  }
  int worker_status = -1;
  if (worker) SDL_WaitThread(worker, &worker_status);
  ok &= woke && worker_status == 0 && !SDL_HasEvent(wake_type);
  ok &= inlay_inbox_pending(inbox) == 2u;
  InlayInboxMessage message = {};
  char bytes[12] = {};
  ok &= inlay_inbox_take(inbox, &message, nullptr, 0u) == INLAY_INBOX_BUFFER_TOO_SMALL &&
        message.payload_size == 7u && message.id == 10u;
  ok &= inlay_inbox_take(inbox, &message, bytes, sizeof(bytes)) == INLAY_INBOX_OK &&
        message.kind == 1u && !std::memcmp(bytes, "abcdefg", 7u);
  ok &= inlay_inbox_post(inbox, 2u, 12u, "123456789", 9u) == INLAY_INBOX_FULL;
  ok &= inlay_inbox_post(inbox, 2u, 12u, "123456", 6u) == INLAY_INBOX_OK && !SDL_HasEvent(wake_type);
  ok &= inlay_inbox_take(inbox, &message, bytes, sizeof(bytes)) == INLAY_INBOX_OK &&
        message.id == 11u && !std::memcmp(bytes, "hijk", 4u);
  ok &= inlay_inbox_take(inbox, &message, bytes, sizeof(bytes)) == INLAY_INBOX_OK &&
        message.id == 12u && !std::memcmp(bytes, "123456", 6u);
  ok &= inlay_inbox_take(inbox, &message, nullptr, 0u) == INLAY_INBOX_EMPTY;
  ok &= inlay_inbox_post(inbox, 3u, 13u, nullptr, 0u) == INLAY_INBOX_OK && SDL_HasEvent(wake_type);
  inlay_inbox_close(inbox);
  ok &= inlay_inbox_post(inbox, 3u, 14u, nullptr, 0u) == INLAY_INBOX_CLOSED;
  ok &= inlay_inbox_take(inbox, &message, nullptr, 0u) == INLAY_INBOX_OK &&
        message.id == 13u && message.payload_size == 0u;
  ok &= inlay_inbox_take(inbox, &message, nullptr, 0u) == INLAY_INBOX_CLOSED;
  inlay_inbox_destroy(inbox);
  SDL_FlushEvent(wake_type);
 }
 SDL_QuitSubSystem(SDL_INIT_EVENTS);
 if (!ok) std::fprintf(stderr, "Inbox worker wake, FIFO, backpressure or close failed\n");
 return ok;
}
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
 for(uint32_t vi=0;vi<doc->view_count;++vi)if(doc->views[vi].id==view.id)cached=&doc->views[vi];
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
  const uint32_t native_row_id=doc_row_id(doc,view.id,doc->views[0].rows[2]);
  const auto native=etch_element_bounds(etch_id_index(native_row_id,7));
  const auto diagram=etch_element_bounds(box.id);
  const EtchColor expected_alert_bg=etch_color_blend_over(
   theme.bg_elevated,
   EtchColor{theme.accent.r,theme.accent.g,theme.accent.b,uint8_t(std::round(0.09f*255.f))});
  auto* img=SDL_LoadPNG(paths[i]);
  if(img) {
   ok &= sample_pixel(img,int(row.x+row.w-20),int(row.y+18),expected_alert_bg,&panel_light[i]);
   ok &= sample_pixel(img,int(diagram.x+node.x+node.w-10),int(diagram.y+node.y+18),
                      theme.bg_elevated,&node_light[i]);
   ok &= sample_pixel(img,int(native.x+22),int(native.y+18),theme.bg_elevated,&native_light[i]);
   SDL_DestroySurface(img);
  } else ok=false;
 }
 ok &= panel_light[1]>panel_light[0]+300 && node_light[1]>node_light[0]+300 &&
       native_light[1]>native_light[0]+300;
 ok &= etch_frame_stats(ctx).draw_call_count>0 && etch_frame_stats(ctx).draw_call_count<=32;
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
 for(uint32_t vi=0;vi<doc->view_count;++vi)if(doc->views[vi].id==view.id)cached=&doc->views[vi];
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
 if(!layout.valid || !single_layout_bytes || single_layout_bytes > 8192u) {
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
 ok &= used>single_layout_bytes && used<baseline+single_layout_bytes*4u;
 if(!ok)std::fprintf(stderr,"Mermaid single pass arena: used=%zu baseline=%zu single_layout=%zu\n",used,baseline,single_layout_bytes);
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
 for(uint32_t vi=0;vi<doc->view_count;++vi)if(doc->views[vi].id==view.id)found_view=&doc->views[vi];
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

bool phase4_md4c_selection_and_abi(EtchContext* ctx) {
 bool ok=true;
 // 1. ABI version & struct_size validation (default, 0, partial prefix, tiny corrupt, oversized)
 bool abi_ok=(inlay_abi_version()==INLAY_ABI_VERSION);
 {
  const auto def_c=inlay_doc_config_default();
  abi_ok &= def_c.struct_size==sizeof(InlayDocConfig);
  InlayDocConfig zero_c={};
  auto* d0=inlay_doc_create(ctx,&zero_c);
  abi_ok &= d0!=nullptr;
  inlay_doc_destroy(d0);
  InlayDocConfig part_c={};
  part_c.struct_size=uint32_t(offsetof(InlayDocConfig,initial_blocks));
  part_c.initial_source_bytes=2048u;
  auto* dp=inlay_doc_create(ctx,&part_c);
  abi_ok &= dp!=nullptr;
  inlay_doc_destroy(dp);
  InlayDocConfig tiny_c=def_c;tiny_c.struct_size=2u;
  InlayDocConfig big_c=def_c;big_c.struct_size=uint32_t(sizeof(InlayDocConfig))+4u;
  abi_ok &= inlay_doc_create(ctx,&tiny_c)==nullptr && inlay_doc_create(ctx,&big_c)==nullptr;
 }
 ok &= abi_ok;

 {
  InlayDoc* doc = inlay_doc_create(ctx, nullptr);
  if (!doc) return false;
  char blank[16384];
  std::memset(blank, '\n', sizeof(blank));
  const auto initial_blocks = doc->block_capacity;
  const auto revision = inlay_doc_revision(doc);
  bool sparse = inlay_doc_set_utf8(doc, {blank, sizeof(blank), false}) &&
                inlay_doc_revision(doc) == revision + 1u &&
                inlay_doc_append_utf8(doc, ETCH_STR_LIT("# Heading\n"));
  sparse &= inlay_doc_block_count(doc) == 1u && doc->block_capacity == initial_blocks;
  sparse &= inlay_doc_replace_utf8(doc, 0, 1, {blank, sizeof(blank), false});
  sparse &= inlay_doc_block_count(doc) == 1u && doc->block_capacity == initial_blocks;
  if (!sparse) std::fprintf(stderr, "Blank lines inflated native block storage or changed the AST\n");
  ok &= sparse;
  inlay_doc_destroy(doc);
 }

 {
  InlayDoc* doc = inlay_doc_create(ctx, nullptr);
  bool retained = doc && inlay_doc_set_utf8(doc, ETCH_STR_LIT("# Keep"));
  if (doc) {
   const auto source_is = [doc](EtchStr expected) {
    const EtchStr current = inlay_doc_source(doc);
    return current.length == expected.length && std::memcmp(current.chars, expected.chars, expected.length) == 0;
   };
   retained &= !inlay_doc_set_utf8(doc, ETCH_STR_LIT("\xff"));
   retained &= source_is(ETCH_STR_LIT("# Keep"));
   const EtchStr invalid[] = {ETCH_STR_LIT("\xe0\x80"), ETCH_STR_LIT("\xed\xa0"),
                              ETCH_STR_LIT("\xf0\x80"), ETCH_STR_LIT("\xf4\x90")};
   for (const EtchStr part : invalid) retained &= !inlay_doc_append_utf8(doc, part);
   retained &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("\xe4\xbd"));
   retained &= inlay_doc_stream_status(doc) == INLAY_DOC_STREAM_PARTIAL_UTF8;
   retained &= !inlay_doc_append_utf8(doc, ETCH_STR_LIT("X"));
   retained &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("\xa0"));
   retained &= inlay_doc_stream_status(doc) == INLAY_DOC_STREAM_READY;
   retained &= source_is(ETCH_STR_LIT("# Keep你"));
   const EtchStr own = inlay_doc_source(doc);
   retained &= inlay_doc_set_utf8(doc, {own.chars + 2u, own.length - 2u, false});
   retained &= source_is(ETCH_STR_LIT("Keep你"));
   retained &= inlay_doc_set_utf8(doc, ETCH_STR_LIT("```text\nCode\n"));
   retained &= inlay_doc_stream_status(doc) == INLAY_DOC_STREAM_OPEN_FENCE;
   retained &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("```\n"));
   retained &= inlay_doc_stream_status(doc) == INLAY_DOC_STREAM_READY;
   inlay_doc_destroy(doc);
  }
  if (!retained) std::fprintf(stderr, "UTF-8 replacement or chunk transaction failed\n");
  ok &= retained;
 }

 {
  InlayDoc* doc = inlay_doc_create(ctx, nullptr);
  bool found = doc != nullptr;
  if (doc) {
   for (uint32_t i = 0; i < 40u; ++i) {
    found &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("A short row.\n\n"));
   }
   found &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("**终点** with a match.\n\n"));
   auto view = inlay_doc_view_desc_default();
   view.id = etch_id("doc-search-reveal");
   view.width_px = 320.f;
   view.height_px = 96.f;
   auto frame = etch_frame_desc_default();
   frame.viewport_w = 400;
   frame.viewport_h = 180;
   const auto draw = [&]() {
    etch_begin_frame(ctx, &frame);
    const InlayDocResult result = inlay_doc_view(doc, &view);
    etch_end_frame(ctx);
    return result;
   };
   const auto initial = draw();
   const auto match = inlay_doc_view_find_text(doc, view.id, ETCH_STR_LIT("终点"), 0u);
   found &= initial.visible_first == 0u && match.active && match.focus - match.anchor == 6u;
   found &= !inlay_doc_view_find_text(doc, view.id, ETCH_STR_LIT("\xe7"), 0u).active;
   found &= !inlay_doc_view_find_text(doc, view.id, ETCH_STR_LIT("终点"), match.focus).active;
   inlay_doc_view_set_selection(doc, view.id, match.anchor, match.focus);
   char selected[16] = {};
   found &= inlay_doc_view_copy_plain_text(doc, view.id, selected, sizeof(selected)) == 6u &&
            std::strcmp(selected, "终点") == 0;
   found &= inlay_doc_view_reveal_text(doc, view.id, match.anchor, ETCH_ALIGN_Y_CENTER);
   const auto moved = draw();
   found &= moved.visible_first > 0u && moved.visible_end == moved.total_rows;
   found &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("Another row."));
   found &= !inlay_doc_view_find_text(doc, view.id, ETCH_STR_LIT("终点"), 0u).active;
   inlay_doc_destroy(doc);
  }
  if (!found) std::fprintf(stderr, "Document search or reveal failed\n");
  ok &= found;
 }

 // 2. md4c CommonMark+GFM Nested AST + Reference links + Autolinks + Streaming chunk equivalence
 const auto nested_md=ETCH_STR_LIT(
  "# Nested CommonMark\n\n"
  "See [Etch][etch-ref], [Inlay][], [Short], <https://commonmark.org> and https://example.org/auto.\n\n"
  "- Level 1 A\n"
  "  - Level 2 B\n"
  "    - Level 3 C\n"
  "  1. Ordered 1\n"
  "  2. Ordered 2\n"
  "- [ ] Root open task\n"
  "  - [x] Nested done task\n\n"
  "> Outer quote paragraph\n"
  ">\n"
  "> > Deep inner quote\n"
  ">\n"
  "> - Quoted bullet 1\n"
  ">   - Quoted bullet 2\n"
  ">\n"
  "> ```cpp\n"
  "> int quoted_val = 7;\n"
  "> ```\n"
  ">\n"
  "> | ColA | ColB |\n"
  "> | :--- | ---: |\n"
  "> | q1 | q2 |\n\n"
  "> [!WARNING]\n"
  "> Alert intro\n"
  ">\n"
  "> - Alert item\n\n"
  "> [!TIP]\n"
  "> Helpful tip\n\n"
  "> [!IMPORTANT]\n"
  "> Key detail\n\n"
  "> [!CAUTION]\n"
  "> Careful here\n\n"
  "[etch-ref]: https://etch.dev\n"
  "[inlay]: https://inlay.dev\n"
  "[short]: https://short.dev\n");

 auto* ref_doc=inlay_doc_create(ctx,nullptr);
 auto* st_doc=inlay_doc_create(ctx,nullptr);
 bool ast_ok=ref_doc && st_doc && inlay_doc_set_utf8(ref_doc,nested_md);
 if(ast_ok) {
  // Check streaming chunk equivalence (including forward reference link definitions at EOF!)
  const uint32_t chunks[]={1u,3u,7u,19u};
  for(uint32_t ch:chunks) {
   inlay_doc_clear(st_doc);
   for(uint32_t p=0;p<nested_md.length;) {
    const uint32_t n=(nested_md.length-p<ch) ? (nested_md.length-p) : ch;
    ast_ok &= inlay_doc_append_utf8(st_doc,{nested_md.chars+p,n,false});
    if (ch == 1u || ch == 7u) (void)inlay_doc_block_count(st_doc);
    p+=n;
   }
   ast_ok &= inlay_doc_block_count(st_doc)==inlay_doc_block_count(ref_doc);
   for(uint32_t i=0;i<inlay_doc_block_count(ref_doc) && i<inlay_doc_block_count(st_doc);++i) {
    const auto a=*inlay_doc_block(ref_doc,i),b=*inlay_doc_block(st_doc,i);
    ast_ok &= a.kind==b.kind && a.offset==b.offset && a.length==b.length &&
              a.content_offset==b.content_offset && a.content_length==b.content_length &&
              a.quote_depth==b.quote_depth && a.list_depth==b.list_depth &&
              a.alert_kind==b.alert_kind && a.flags==b.flags && a.complete==b.complete;
   }
   ast_ok &= st_doc->ref_def_count==ref_doc->ref_def_count && st_doc->list_item_count==ref_doc->list_item_count;
  }
  // Verify nested AST properties: depth-3 list, quote_depth==2, code-in-quote, table-in-quote, all alert kinds
  bool saw_depth3_list=false,saw_quote2=false,saw_code_in_quote=false,saw_table_in_quote=false;
  bool saw_warn=false,saw_tip=false,saw_imp=false,saw_caut=false;
  for(uint32_t k=0;k<ref_doc->list_item_count;++k) {
   if(ref_doc->list_items[k].list_depth>=3u)saw_depth3_list=true;
  }
  for(uint32_t i=0;i<inlay_doc_block_count(ref_doc);++i) {
   const auto& b=*inlay_doc_block(ref_doc,i);
   if(b.quote_depth>=2u)saw_quote2=true;
   if(b.kind==INLAY_CODE_BLOCK && b.quote_depth>=1u)saw_code_in_quote=true;
   if(b.kind==INLAY_TABLE && b.quote_depth>=1u)saw_table_in_quote=true;
   if(b.alert_kind==INLAY_ALERT_WARNING)saw_warn=true;
   if(b.alert_kind==INLAY_ALERT_TIP)saw_tip=true;
   if(b.alert_kind==INLAY_ALERT_IMPORTANT)saw_imp=true;
   if(b.alert_kind==INLAY_ALERT_CAUTION)saw_caut=true;
  }
  ast_ok &= saw_depth3_list && saw_quote2 && saw_code_in_quote && saw_table_in_quote &&
            saw_warn && saw_tip && saw_imp && saw_caut && ref_doc->ref_def_count==3u;

  // Render and verify reference links + autolinks resolved cleanly!
  auto view=inlay_doc_view_desc_default();
  view.id=etch_id("p4-nested");view.width_px=420;view.height_px=0;
  auto f=etch_frame_desc_default();f.viewport_w=460;f.viewport_h=1200;
  for(int k=0;k<2;++k){etch_begin_frame(ctx,&f);inlay_doc_view(ref_doc,&view);etch_end_frame(ctx);}
  uint32_t ref_mask=0;
  const auto& vm=ref_doc->views[0];
  for(uint32_t ti=0;ti<vm.token_count;++ti) {
   const auto& t=vm.tokens[ti];
   if(!(t.flags&InlayInlineLink))continue;
   const EtchStr txt={nested_md.chars+t.offset,t.length,false};
   const EtchStr url=(t.flags&InlayInlineUrlInPool) ? EtchStr{ref_doc->ast_text+t.url_offset,t.url_length,false}
                                                     : EtchStr{nested_md.chars+t.url_offset,t.url_length,false};
   if(str_equal(txt,ETCH_STR_LIT("Etch")) && str_equal(url,ETCH_STR_LIT("https://etch.dev")))ref_mask|=1u;
   if(str_equal(txt,ETCH_STR_LIT("Inlay")) && str_equal(url,ETCH_STR_LIT("https://inlay.dev")))ref_mask|=2u;
   if(str_equal(txt,ETCH_STR_LIT("Short")) && str_equal(url,ETCH_STR_LIT("https://short.dev")))ref_mask|=4u;
   if(str_equal(txt,ETCH_STR_LIT("https://commonmark.org")) && str_equal(url,ETCH_STR_LIT("https://commonmark.org")))ref_mask|=8u;
   if(str_equal(txt,ETCH_STR_LIT("https://example.org/auto")) && str_equal(url,ETCH_STR_LIT("https://example.org/auto")))ref_mask|=16u;
  }
  ast_ok &= (ref_mask==31u);
  if(!ast_ok) {
   std::fprintf(stderr,"Phase4 AST failed: d3=%d q2=%d cq=%d tq=%d alerts=%d%d%d%d refs=%u mask=%u\n",
                saw_depth3_list,saw_quote2,saw_code_in_quote,saw_table_in_quote,
                saw_warn,saw_tip,saw_imp,saw_caut,ref_doc->ref_def_count,ref_mask);
  }
 }
 ok &= ast_ok;

 // 3. Unified document character-level cross-block selection, single-block subrange, Ctrl+A, Ctrl+C, balanced Markdown
 bool sel_ok=true;
 {
  auto* sdoc=inlay_doc_create(ctx,nullptr);
  sel_ok &= sdoc!=nullptr && inlay_doc_set_utf8(sdoc,ETCH_STR_LIT(
   "# Alpha Heading\n\n"
   "Second **bold middle** line.\n\n"
   "```cpp\nint final_code = 99;\n```\n"));
  InlayDocStyle st=inlay_doc_style_default();
  st.selection_color=etch_rgba8(77,155,244,140);
  auto view=inlay_doc_view_desc_default();
  view.id=etch_id("p4-sel");view.width_px=420;view.height_px=0;view.style=&st;
  auto f=etch_frame_desc_default();f.viewport_w=460;f.viewport_h=600;
  const auto step=[&]() {
   etch_begin_frame(ctx,&f);
   inlay_doc_view(sdoc,&view);
   etch_end_frame(ctx);
  };
  step();step();
  const auto& m=sdoc->views[0];
  sel_ok &= m.vis_seg_count>=3u;
  if(m.vis_seg_count>=3u) {
   const EtchRect r0=etch_element_bounds(m.vis_segs[0].element_id); // "Alpha Heading"
   const EtchRect r2=etch_element_bounds(m.vis_segs[2].element_id); // "int final_code = 99;"
   // Cross-block drag: from ~middle of heading (r0) across full middle paragraph (r1) to ~middle of code line (r2)
   f.pointer_pos={r0.x+r0.w*0.5f,r0.y+r0.h*0.5f};
   f.pointer_down=true;
   step();
   f.pointer_pos={r2.x+r2.w*0.5f,r2.y+r2.h*0.5f};
   step();
   // Check crisp selection highlight rects were emitted across all 3 blocks!
   const uint32_t sel_rgba=etch_color_pack_rgba8(st.selection_color);
   bool hl0=false,hl1=false,hl2=false;
   const EtchRect r1=etch_element_bounds(m.vis_segs[1].element_id);
   for(uint32_t j=0;j<etch_frame_prim_count(ctx);++j) {
    EtchPrim pr={};
    if(!etch_get_frame_prim(ctx,j,&pr) || pr.style.y!=sel_rgba)continue;
    const float py=pr.bounds.y+pr.bounds.w*0.5f;
    if(py>=r0.y && py<=r0.y+r0.h)hl0=true;
    if(py>=r1.y && py<=r1.y+r1.h)hl1=true;
    if(py>=r2.y && py<=r2.y+r2.h)hl2=true;
   }
   f.pointer_down=false;
   step();
   const auto sel=inlay_doc_view_selection(sdoc,view.id);
   char cross_buf[256]={};
   const uint32_t cross_len=inlay_doc_view_copy_plain_text(sdoc,view.id,cross_buf,sizeof(cross_buf));
   // Trigger Ctrl+C and check clipboard matches!
   f.key_mods=ETCH_MOD_PRIMARY;f.key_pressed_code='c';
   step();
   f.key_mods=0;f.key_pressed_code=0;
   EtchArena clip_arena=etch_arena_create(4096);
   const EtchStr clip1=etch_get_clipboard_text(&clip_arena);
   const bool cross_ok=sel.active && sel.anchor>0 && sel.anchor<m.vis_segs[0].doc_len &&
                       sel.focus>m.vis_segs[2].doc_start && sel.focus<m.vis_segs[2].doc_start+m.vis_segs[2].doc_len &&
                       hl0 && hl1 && hl2 && cross_len>0 &&
                       std::strstr(cross_buf,"Second bold middle line.")!=nullptr &&
                       clip1.length==cross_len && !std::memcmp(clip1.chars,cross_buf,cross_len);
   // Balanced Markdown copy across partial heading + bold paragraph + partial code block!
   char md_buf[512]={};
   const uint32_t md_len=inlay_doc_view_copy_markdown(sdoc,view.id,md_buf,sizeof(md_buf));
   const bool md_ok=md_len>0 && std::strstr(md_buf,"**bold middle**")!=nullptr &&
                    std::strstr(md_buf,"```cpp\n")!=nullptr && std::strstr(md_buf,"\n```")!=nullptr;
   sel_ok &= cross_ok && md_ok;

   // Single-block subrange drag inside middle paragraph ("Second bold middle line.")
   f.pointer_pos={r1.x+r1.w*0.25f,r1.y+r1.h*0.5f};
   f.pointer_down=true;step();
   f.pointer_pos={r1.x+r1.w*0.65f,r1.y+r1.h*0.5f};
   step();
   f.pointer_down=false;step();
   const auto sub_sel=inlay_doc_view_selection(sdoc,view.id);
   char sub_buf[128]={};
   const uint32_t sub_len=inlay_doc_view_copy_plain_text(sdoc,view.id,sub_buf,sizeof(sub_buf));
   f.key_mods=ETCH_MOD_PRIMARY;f.key_pressed_code='c';step();
   f.key_mods=0;f.key_pressed_code=0;
   etch_arena_reset(&clip_arena);
   const EtchStr clip_sub=etch_get_clipboard_text(&clip_arena);
   const bool sub_ok=sub_sel.active && sub_sel.anchor>m.vis_segs[1].doc_start &&
                     sub_sel.focus<m.vis_segs[1].doc_start+m.vis_segs[1].doc_len &&
                     sub_len>0 && sub_len<m.vis_segs[1].doc_len &&
                     clip_sub.length==sub_len && !std::memcmp(clip_sub.chars,sub_buf,sub_len);
   sel_ok &= sub_ok;

   // Ctrl+A selects entire document + Ctrl+C copies entire document plain text!
   f.key_mods=ETCH_MOD_PRIMARY;f.key_pressed_code='a';step();
   f.key_mods=ETCH_MOD_PRIMARY;f.key_pressed_code='c';step();
   f.key_mods=0;f.key_pressed_code=0;
   const auto all_sel=inlay_doc_view_selection(sdoc,view.id);
   etch_arena_reset(&clip_arena);
   const EtchStr clip_all=etch_get_clipboard_text(&clip_arena);
   const bool all_ok=all_sel.active && all_sel.anchor==0 && all_sel.focus==all_sel.total_bytes &&
                     clip_all.length==all_sel.total_bytes &&
                     std::strstr(clip_all.chars,"Alpha Heading")!=nullptr &&
                     std::strstr(clip_all.chars,"Second bold middle line.")!=nullptr &&
                     std::strstr(clip_all.chars,"int final_code = 99;")!=nullptr;
   sel_ok &= all_ok;
   etch_arena_destroy(&clip_arena);
   if(!sel_ok) {
    std::fprintf(stderr,"Phase4 selection failed: cross=%d(hl=%d%d%d,a=%u,f=%u,clip=%u/%u) md=%d sub=%d(len=%u) all=%d(a=%u,f=%u/%u)\n",
                 cross_ok,hl0,hl1,hl2,sel.anchor,sel.focus,clip1.length,cross_len,md_ok,sub_ok,sub_len,
                 all_ok,all_sel.anchor,all_sel.focus,all_sel.total_bytes);
   }
  }
  inlay_doc_destroy(sdoc);
 }
 ok &= sel_ok;
 inlay_doc_destroy(ref_doc);
 inlay_doc_destroy(st_doc);
 if(!ok)std::fprintf(stderr,"Phase 4 check FAILED: abi=%d ast=%d sel=%d\n",abi_ok,ast_ok,sel_ok);
 return ok;
}

bool upgrade_v011_regression(EtchContext* ctx,const char* autofit_png) {
 bool ok=true;
 auto f=etch_frame_desc_default();f.viewport_w=520;f.viewport_h=900;

 // 1. Mermaid auto-fit, per-rank rank_gaps[], richer flowchart syntax (RL/BT, quoted labels, <br/>, ---/==>/<-->, directives)
 bool mermaid_ok=true;
 {
  etch_begin_frame(ctx,&f);
  const auto wide_lr=ETCH_STR_LIT(
   "flowchart LR\n"
   "classDef hot fill:#f96\n"
   "direction LR\n"
   "A[\"Input (raw)<br/>Stage 1\"] -->|very long descriptive edge label| B[Parse] ==> C[Validate]\n"
   "C <--> D[Emit] --- E[Done]\n"
   "class A hot\n"
   "style B stroke:#333\n"
   "linkStyle 0 stroke-width:2px\n");
  auto dd_nat=inlay_diagram_desc_default();
  const auto nat=inlay_diagram_layout(wide_lr,&dd_nat);
  mermaid_ok &= nat.valid && nat.horizontal && nat.node_count==5 && nat.edge_count==4 &&
                nat.scale==1.0f && nat.natural_width==nat.width && nat.natural_width>480.f;
  if(nat.valid && nat.node_count==5) {
   // Multiline <br/> node A is taller than single-line node B.
   mermaid_ok &= nat.nodes[0].bounds.h > nat.nodes[1].bounds.h + 8.f;
   // Per-rank gap: A->B gap (with long label) must be significantly wider than B->C and C->D gaps!
   const float gap01=nat.nodes[1].bounds.x - (nat.nodes[0].bounds.x + nat.nodes[0].bounds.w);
   const float gap12=nat.nodes[2].bounds.x - (nat.nodes[1].bounds.x + nat.nodes[1].bounds.w);
   const float gap23=nat.nodes[3].bounds.x - (nat.nodes[2].bounds.x + nat.nodes[2].bounds.w);
   mermaid_ok &= gap01 > gap12 + 30.f && std::fabs(gap12 - gap23) < 2.f;
  }
  // Native auto-fit to max_width_px = 420 (should scale < 1.0f and fit strictly <= 420)
  auto dd_fit=inlay_diagram_desc_default();
  dd_fit.max_width_px=420.f;
  dd_fit.min_scale=0.5f;
  const auto fit=inlay_diagram_layout(wide_lr,&dd_fit);
  mermaid_ok &= fit.valid && fit.scale<1.0f && fit.scale>=0.5f &&
                fit.natural_width>420.f && fit.width<=420.1f;

  // RL and BT directions
  const auto rl=inlay_diagram_layout(ETCH_STR_LIT("flowchart RL\nA[Start] --> B[End]"),&dd_nat);
  const auto bt=inlay_diagram_layout(ETCH_STR_LIT("flowchart BT\nA[Bottom] --> B[Top]"),&dd_nat);
  mermaid_ok &= rl.valid && rl.horizontal && rl.node_count==2 && rl.nodes[0].bounds.x > rl.nodes[1].bounds.x;
  mermaid_ok &= bt.valid && !bt.horizontal && bt.node_count==2 && bt.nodes[0].bounds.y > bt.nodes[1].bounds.y;
  etch_end_frame(ctx);

  // Mermaid in inlay_doc_view: streaming placeholder, exact first-frame height (no 200px jump), Fit/100% toggle,
  // diagram_header_fn, Copy->"Copied", and headless PNG zero-clipping check.
  auto* mdoc=inlay_doc_create(ctx,nullptr);
  mermaid_ok &= mdoc!=nullptr;
  if(mdoc) {
   // Streaming incomplete Mermaid renders cleanly as placeholder (no crash, no layout error).
   mermaid_ok &= inlay_doc_set_utf8(mdoc,ETCH_STR_LIT("```mermaid\nflowchart LR\nA[Stream] --> B["));
   uint32_t diag_hdr_calls=0;
   auto mv=inlay_doc_view_desc_default();
   mv.id=etch_id("v011-mermaid");mv.width_px=440;mv.height_px=0;
   mv.diagram_header_user_data=&diag_hdr_calls;
   mv.diagram_header_fn=[](uint32_t,uint32_t row_id,EtchStr lang,EtchStr,void* user) {
    if(str_equal(lang,ETCH_STR_LIT("mermaid")))++(*static_cast<uint32_t*>(user));
    auto b=etch_box_desc_default();
    b.id=etch_id_index(row_id,888);b.width=etch_sizing_fixed(18);b.height=etch_sizing_fixed(16);
    etch_box(&b);
   };
   etch_begin_frame(ctx,&f);inlay_doc_view(mdoc,&mv);etch_end_frame(ctx);
   mermaid_ok &= contains_label(ctx,ETCH_STR_LIT("Mermaid..."));

   // Complete wide flowchart LR: first frame measures exact height (not 200px!), fits <= inner_w, shows "100%" toggle!
   mermaid_ok &= inlay_doc_set_utf8(mdoc,ETCH_STR_LIT(
    "```mermaid\nflowchart LR\nA[Alpha Node] --> B[Beta Node] --> C[Gamma Node] --> D[Delta Node]\n```\n"));
   etch_begin_frame(ctx,&f);
   const auto mres1=inlay_doc_view(mdoc,&mv);
   etch_end_frame(ctx);
   const auto& mvm=mdoc->views[0];
   const uint32_t mrow_id=doc_row_id(mdoc,mv.id,mvm.rows[0]);
   const auto dbox_fit=etch_element_bounds(etch_id_index(mrow_id,7));
   mermaid_ok &= mres1.total_rows==1 && mvm.heights[0]>0.f && std::fabs(mvm.heights[0]-200.f)>10.f &&
                 diag_hdr_calls>0 && contains_label(ctx,ETCH_STR_LIT("Mermaid")) &&
                 contains_label(ctx,ETCH_STR_LIT("100%")) && contains_label(ctx,ETCH_STR_LIT("Copy")) &&
                 dbox_fit.w>0.f && dbox_fit.w<=mv.width_px-2.f*12.f+1.f;
   if(autofit_png) {
    // Render headless PNG and verify rightmost fitted node pixels are visibly drawn inside the box (zero clipping).
    const EtchTheme thm=etch_get_theme(ctx);
    mermaid_ok &= etch_render_headless_png(ctx,520,900,autofit_png,thm.bg_canvas);
    auto* img=SDL_LoadPNG(autofit_png);
    if(img) {
     uint32_t edge_ink=0;
     const int rx0=int(dbox_fit.x+dbox_fit.w-30);
     const int rx1=int(dbox_fit.x+dbox_fit.w-2);
     const int ry0=int(dbox_fit.y+4);
     const int ry1=int(dbox_fit.y+dbox_fit.h-4);
     for(int y=ry0;y<ry1;++y)for(int x=rx0;x<rx1;++x) {
      uint8_t r=0,g=0,b=0,a=0;
      if(SDL_ReadSurfacePixel(img,x,y,&r,&g,&b,&a) &&
         (std::abs(int(r)-thm.bg_elevated.r)>10 || std::abs(int(g)-thm.bg_elevated.g)>10 || std::abs(int(b)-thm.bg_elevated.b)>10))++edge_ink;
     }
     mermaid_ok &= edge_ink>15;
     SDL_DestroySurface(img);
    } else mermaid_ok=false;
   }
   // Click Copy -> "Copied", click 100% -> "Fit"
   const uint32_t copy_id=etch_id_index(mrow_id,1);
   const uint32_t fit_id=etch_id_index(mrow_id,4);
   mermaid_ok &= etch_perform_a11y_action(copy_id,ETCH_A11Y_ACT_PRESS,0,{},ctx,nullptr);
   etch_begin_frame(ctx,&f);inlay_doc_view(mdoc,&mv);etch_end_frame(ctx);
   etch_begin_frame(ctx,&f);inlay_doc_view(mdoc,&mv);etch_end_frame(ctx);
   mermaid_ok &= contains_label(ctx,ETCH_STR_LIT("Copied"));
   mermaid_ok &= etch_perform_a11y_action(fit_id,ETCH_A11Y_ACT_PRESS,0,{},ctx,nullptr);
   etch_begin_frame(ctx,&f);inlay_doc_view(mdoc,&mv);etch_end_frame(ctx);
   etch_begin_frame(ctx,&f);inlay_doc_view(mdoc,&mv);etch_end_frame(ctx);
   mermaid_ok &= contains_label(ctx,ETCH_STR_LIT("Fit")) && mvm.diagram_full_scale[0];
   inlay_doc_destroy(mdoc);
  }
 }
 ok &= mermaid_ok;

 // 2. Decoupled inline code pill styling (inline_code_no_border, padding_y=0, radius=4)
 bool inline_code_ok=true;
 {
  auto* doc=inlay_doc_create(ctx,nullptr);
  inline_code_ok &= doc && inlay_doc_set_utf8(doc,ETCH_STR_LIT("Check `pill` here.\n"));
  InlayDocStyle st=inlay_doc_style_default();
  st.code_border=etch_rgba8(220,30,30,255);
  st.inline_code_bg=etch_rgba8(25,55,95,255);
  st.inline_code_color=etch_rgba8(110,240,190,255);
  st.inline_code_padding_x=4.5f;
  st.inline_code_padding_y=0.f;
  st.inline_code_radius_px=4.f;
  st.inline_code_no_border=true;
  auto v=inlay_doc_view_desc_default();
  v.id=etch_id("v011-inline-code");v.width_px=320;v.height_px=0;v.style=&st;
  etch_begin_frame(ctx,&f);inlay_doc_view(doc,&v);etch_end_frame(ctx);
  inline_code_ok &= has_prim_color(ctx,etch_color_pack_rgba8(st.inline_code_bg)) &&
                    has_prim_color(ctx,etch_color_pack_rgba8(st.inline_code_color)) &&
                    !has_prim_color(ctx,etch_color_pack_rgba8(st.code_border));
  inlay_doc_destroy(doc);
 }
 ok &= inline_code_ok;

 // 3. Bold headings + Alert semantic border preservation with quote_border set + synthetic multi-block alert text
 bool heading_alert_ok=true;
 {
  auto* doc=inlay_doc_create(ctx,nullptr);
  heading_alert_ok &= doc && inlay_doc_set_utf8(doc,ETCH_STR_LIT(
   "# First Bold Heading\n\n"
   "Paragraph text.\n\n"
   "## Second Heading\n\n"
   "> [!WARNING]\n> Alert body\n>\n> - Nested alert item\n\n"
   "> Regular blockquote\n"));
  InlayDocStyle st=inlay_doc_style_default();
  st.quote_border=etch_rgba8(190,80,245,255);
  st.alert_colors[INLAY_ALERT_WARNING-1]=etch_rgba8(245,165,20,255);
  auto v=inlay_doc_view_desc_default();
  v.id=etch_id("v011-head-alert");v.width_px=380;v.height_px=0;v.style=&st;
  for(int k=0;k<2;++k){etch_begin_frame(ctx,&f);inlay_doc_view(doc,&v);etch_end_frame(ctx);}
  const auto& vm=doc->views[0];
  // Heading uses ETCH_FONT_SEMIBOLD by default (when bold_font_id == ETCH_INVALID_FONT)
  heading_alert_ok &= vm.vis_seg_count>=2 && vm.vis_segs[0].font_id==ETCH_FONT_SEMIBOLD &&
                      vm.vis_segs[1].font_id==ETCH_FONT_SANS;
  // Alert border keeps semantic warning color (NOT overwritten by style.quote_border!), while plain quote uses quote_border!
  heading_alert_ok &= has_prim_color(ctx,etch_color_pack_rgba8(st.alert_colors[INLAY_ALERT_WARNING-1])) &&
                      has_prim_color(ctx,etch_color_pack_rgba8(st.quote_border)) &&
                      contains_label(ctx,ETCH_STR_LIT("WARNING"));
  // Verify synthetic non-code alert block reads from doc->ast_text (NOT doc->source[0])!
  InlayDoc* sdoc=inlay_doc_create(ctx,nullptr);
  heading_alert_ok &= sdoc && inlay_doc_set_utf8(sdoc,ETCH_STR_LIT("WRONG_SOURCE_PREFIX\n\n> [!CAUTION]\n> Line 1\n"));
  if(sdoc && inlay_doc_block_count(sdoc)>=2) {
   const char* tag="[!CAUTION]";
   const uint32_t tlen=10;
   if(sdoc->ast_text_capacity < sdoc->ast_text_size + tlen) {
    auto* mem=static_cast<char*>(std::realloc(sdoc->ast_text, sdoc->ast_text_size + tlen + 64));
    if(mem){sdoc->ast_text=mem;sdoc->ast_text_capacity=sdoc->ast_text_size+tlen+64;}
   }
   if(sdoc->ast_text) {
    const uint32_t po=sdoc->ast_text_size;
    std::memcpy(sdoc->ast_text+po,tag,tlen);
    sdoc->ast_text_size+=tlen;
    sdoc->blocks[1].flags |= INLAY_BLOCK_FLAG_SYNTHETIC;
    sdoc->blocks[1].content_offset=0;
    sdoc->blocks[1].content_length=tlen;
    sdoc->block_meta[1].pool_offset=po;
    sdoc->block_meta[1].pool_length=tlen;
    ++sdoc->revision;
    auto sv=inlay_doc_view_desc_default();sv.id=etch_id("v011-synth-alert");sv.width_px=320;sv.height_px=0;
    etch_begin_frame(ctx,&f);inlay_doc_view(sdoc,&sv);etch_end_frame(ctx);
    heading_alert_ok &= contains_label(ctx,ETCH_STR_LIT("CAUTION"));
   }
  }
  inlay_doc_destroy(sdoc);
  inlay_doc_destroy(doc);
 }
 ok &= heading_alert_ok;

 // 4. Multi-chunk (>16 line) seamless code block borders + show_code_line_numbers reflow + code_header_bg
 bool code_seamless_ok=true;
 {
  auto* doc=inlay_doc_create(ctx,nullptr);
  const auto long_code=ETCH_STR_LIT(
   "```cpp\n"
   "int l01=1;\nint l02=2;\nint l03=3;\nint l04=4;\nint l05=5;\nint l06=6;\nint l07=7;\nint l08=8;\n"
   "int l09=9;\nint l10=10;\nint l11=11;\nint l12=12;\nint l13=13;\nint l14=14;\nint l15=15;\nint l16=16;\n"
   "int l17=17;\nint l18=18;\nint l19=19;\nint l20=20;\n"
   "```\n");
  code_seamless_ok &= doc && inlay_doc_set_utf8(doc,long_code);
  InlayDocStyle st=inlay_doc_style_default();
  st.code_border=etch_rgba8(60,110,170,255);
  st.code_header_bg=etch_rgba8(18,28,44,255);
  auto v=inlay_doc_view_desc_default();
  v.id=etch_id("v011-code-chunks");v.width_px=380;v.height_px=0;v.style=&st;v.show_code_line_numbers=false;
  for(int k=0;k<2;++k){etch_begin_frame(ctx,&f);inlay_doc_view(doc,&v);etch_end_frame(ctx);}
  const auto& vm=doc->views[0];
  code_seamless_ok &= vm.count==2 && has_prim_color(ctx,etch_color_pack_rgba8(st.code_header_bg));
  if(vm.count==2) {
   const uint32_t r1_id=doc_row_id(doc,v.id,vm.rows[0]);
   const uint32_t r2_id=doc_row_id(doc,v.id,vm.rows[1]);
   const auto l16=etch_element_bounds(etch_id_index(etch_id_index(r1_id,2),16));
   const auto l17=etch_element_bounds(etch_id_index(etch_id_index(r2_id,2),1));
   const float seam_gap=l17.y - (l16.y + l16.h);
   // Seamless spacing (~4px) instead of double 12px card padding (~24px)!
   code_seamless_ok &= l16.h>0.f && l17.h>0.f && seam_gap>=2.f && seam_gap<=8.f;
   // Verify no horizontal interior border rect of style.code_border sits across the seam between r1 and r2!
   const auto r1_b=etch_element_bounds(r1_id);
   const uint32_t border_rgba=etch_color_pack_rgba8(st.code_border);
   bool interior_h_border=false;
   for(uint32_t j=0;j<etch_frame_prim_count(ctx);++j) {
    EtchPrim pr={};
    if(!etch_get_frame_prim(ctx,j,&pr) || pr.style.y!=border_rgba)continue;
    if(pr.bounds.z > r1_b.w*0.5f && pr.bounds.w <= 3.f && std::fabs(pr.bounds.y - (r1_b.y + r1_b.h)) < 3.f) {
     interior_h_border=true;
    }
   }
   code_seamless_ok &= !interior_h_border;
  }
  // Toggle show_code_line_numbers triggers reflow immediately and emits line number "17"!
  v.show_code_line_numbers=true;
  etch_begin_frame(ctx,&f);inlay_doc_view(doc,&v);etch_end_frame(ctx);
  code_seamless_ok &= contains_label(ctx,ETCH_STR_LIT("17"));
  inlay_doc_destroy(doc);
 }
 ok &= code_seamless_ok;

 // 5. Inline math merged runs, table link URL stripping + wide horizontal scroll, structured click metadata, cached custom fence
 bool misc_ok=true;
 {
  auto* doc=inlay_doc_create(ctx,nullptr);
  misc_ok &= doc && inlay_doc_set_utf8(doc,ETCH_STR_LIT(
   "Start **bold** `tag` $x^2$ end.\n\n"
   "$$\na + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p\n$$\n\n"
   "| Item | Link |\n| --- | --- |\n| Short | [Go](https://example.org/extremely/long/url/that/must/not/inflate/table/column/width) |\n\n"
   "| C1 | C2 | C3 | C4 | C5 | C6 |\n| --- | --- | --- | --- | --- | --- |\n| AlphaCol | BetaCol | GammaCol | DeltaCol | EpsilonCol | ZetaCol |\n\n"
   "> ```nested-custom\n> payload\n> ```\n"));
  struct MiscState { uint32_t code_id, est_calls; float last_avail_w; } ms={etch_id("v011-click-tag"),0,0.f};
  auto v=inlay_doc_view_desc_default();
  v.id=etch_id("v011-misc");v.width_px=340;v.height_px=0;
  v.inline_decorator_user_data=&ms;
  v.inline_decorator_fn=[](InlayInlineKind kind,EtchStr text,EtchStr,EtchTextSpan* sp,void* user) {
   auto& s=*static_cast<MiscState*>(user);
   if(kind==INLAY_INLINE_CODE && str_equal(text,ETCH_STR_LIT("tag"))) { sp->id=s.code_id; return true; }
   return false;
  };
  v.custom_fence_user_data=&ms;
  v.custom_fence_estimate_fn=[](EtchStr lang,EtchStr,float avail_w,float,void* user) -> float {
   auto& s=*static_cast<MiscState*>(user);
   if(str_equal(lang,ETCH_STR_LIT("nested-custom"))) { ++s.est_calls; s.last_avail_w=avail_w; return 32.f; }
   return 0.f;
  };
  v.custom_fence_render_fn=[](uint32_t,uint32_t row_id,EtchStr,EtchStr,float avail_w,float,void*) {
   auto b=etch_box_desc_default();
   b.id=etch_id_index(row_id,55);b.width=etch_sizing_fixed(avail_w);b.height=etch_sizing_fixed(32.f);
   etch_box(&b);
  };
  for(int k=0;k<3;++k){etch_begin_frame(ctx,&f);inlay_doc_view(doc,&v);etch_end_frame(ctx);}
  const auto& vm=doc->views[0];
  // 1) Inline math merged run: "Start " + "bold" + " " + "tag" + " " merged into 1 vis_seg before $x^2$, and " end." = 1 vis_seg after!
  uint32_t row0_segs=0;
  for(uint32_t s=0;s<vm.vis_seg_count;++s) if(vm.vis_segs[s].block_index==0u) ++row0_segs;
  misc_ok &= row0_segs==2u;
  // 2) Table URL stripping + wide 6-column table horizontal scroll
  bool found_url_tbl=false,found_wide_tbl=false;
  for(uint32_t t=0;t<vm.table_cols_count;++t) {
   if(vm.table_cols[t].columns==2u) {
    found_url_tbl=true;
    // Link column ("Go") should NOT dominate over "Item"/"Short" (both roughly equal, well below 60% of 316px).
    misc_ok &= vm.table_cols[t].widths[1] < 185.f;
   }
   if(vm.table_cols[t].columns==6u) {
    found_wide_tbl=true;
    misc_ok &= vm.table_cols[t].total_width > (v.width_px - 2.f*v.block_padding_px);
   }
  }
  misc_ok &= found_url_tbl && found_wide_tbl;
  // 3) Nested custom fence received reduced width (< width_px - 2*padding) and was cached (not called on steady-state frames!)
  const uint32_t est_after_settle=ms.est_calls;
  etch_begin_frame(ctx,&f);inlay_doc_view(doc,&v);etch_end_frame(ctx);
  misc_ok &= ms.last_avail_w < (v.width_px - 2.f*v.block_padding_px - 10.f) && ms.est_calls==est_after_settle;
  // 4) Structured click metadata on decorated INLAY_INLINE_CODE with ETCH_MOD_CTRL
  const auto tag_bounds=etch_element_bounds(ms.code_id);
  misc_ok &= tag_bounds.w>0.f && tag_bounds.h>0.f;
  if(tag_bounds.w>0.f) {
   auto cf=f;
   cf.pointer_pos={tag_bounds.x+tag_bounds.w*0.5f,tag_bounds.y+tag_bounds.h*0.5f};
   cf.key_mods=ETCH_MOD_CTRL;
   cf.pointer_down=true;
   etch_begin_frame(ctx,&cf);const auto r_down=inlay_doc_view(doc,&v);etch_end_frame(ctx);
   cf.pointer_down=false;
   etch_begin_frame(ctx,&cf);const auto r_up=inlay_doc_view(doc,&v);etch_end_frame(ctx);
   const auto& hit=r_up.clicked_link.length ? r_up : r_down;
   misc_ok &= str_equal(hit.clicked_link,ETCH_STR_LIT("tag")) &&
              hit.clicked_kind==INLAY_INLINE_CODE &&
              hit.clicked_bounds.w>0.f &&
              hit.clicked_block_index==0u &&
              (hit.clicked_mods & ETCH_MOD_CTRL)!=0;
  }
  inlay_doc_destroy(doc);
 }
 ok &= misc_ok;

 if(!ok)std::fprintf(stderr,"v0.11 upgrade check FAILED: mermaid=%d inline_code=%d head_alert=%d code_seamless=%d misc=%d\n",
                     mermaid_ok,inline_code_ok,heading_alert_ok,code_seamless_ok,misc_ok);
 return ok;
}
}
int main(int argc,char** argv) {
 if(argc==4) {
  const bool theme=theme_pixels(argv[1],argv[2]);
  const bool flow=flow_document(argv[3]);
  // Also run headless PNG verification of narrow-width Mermaid flowchart LR auto-fit (stored under build/).
  char af_png[512]={};
  const char* slash=std::strrchr(argv[3],'/');
  const char* bslash=std::strrchr(argv[3],'\\');
  const char* sep=(bslash && (!slash || bslash>slash)) ? bslash : slash;
  if(sep && size_t(sep - argv[3]) + 32 < sizeof(af_png)) {
   const size_t dir_len=size_t(sep - argv[3]) + 1;
   std::memcpy(af_png,argv[3],dir_len);
   std::memcpy(af_png+dir_len,"document-mermaid-autofit.png",29);
  }
  auto cfg=etch_context_desc_default();cfg.headless=true;
  auto* ctx=etch_create_context(&cfg);
  const bool v11=ctx && upgrade_v011_regression(ctx,af_png[0] ? af_png : nullptr);
  if(ctx)etch_destroy_context(ctx);
  return flow && theme && v11 ? 0 : 1;
 }
 auto config=etch_context_desc_default();config.headless=true;
 auto* ctx=etch_create_context(&config);if(!ctx)return 1;
 auto* reference=inlay_doc_create(ctx,nullptr);auto* streamed=inlay_doc_create(ctx,nullptr);
 if(!reference || !streamed)return 1;
 const char* section="# Heading 中文 😀\n\nA **bold** paragraph with `code` and [link](https://example.org).\n\n> [!NOTE]\n> Quote text\n\n- [x] task\n- [ ] other\n\n| Name | Value |\n| :--- | ---: |\n| A | 42 |\n\n```cpp\nint x = 1; // 中文\n```\n\n<details>\n<summary>Details</summary>\nMore text\n</details>\n\n---\n\n";
 char input[65536]={};size_t size=0;
 while(size+std::strlen(section)<32768) {std::memcpy(input+size,section,std::strlen(section));size+=std::strlen(section);}
 bool ok=inlay_doc_set_utf8(reference,{input,uint32_t(size),false});
 ok &= inbox_regression();
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
 InlayDocConfig small=inlay_doc_config_default();small.initial_source_bytes=4;small.initial_blocks=1;
 auto* alias=inlay_doc_create(ctx,&small);
 ok &= alias && inlay_doc_set_utf8(alias,ETCH_STR_LIT("abc\n\n"));
 if(alias) {
  ok &= inlay_doc_append_utf8(alias,inlay_doc_source(alias));
  const auto source=inlay_doc_source(alias);ok &= source.length==10 && !std::memcmp(source.chars,"abc\n\nabc\n\n",10);
  inlay_doc_destroy(alias);
 }
 const auto same_edited_doc = [&](EtchStr expected) {
  if (!inlay_doc_set_utf8(reference, expected)) return false;
  const EtchStr source = inlay_doc_source(streamed);
  if (source.length != expected.length || (source.length && std::memcmp(source.chars, expected.chars, source.length))) return false;
  const uint32_t blocks = inlay_doc_block_count(reference);
  if (blocks != inlay_doc_block_count(streamed)) return false;
  for (uint32_t i = 0; i < blocks; ++i) {
   const InlayBlock& a = *inlay_doc_block(reference, i);
   const InlayBlock& b = *inlay_doc_block(streamed, i);
   if (a.kind != b.kind || a.offset != b.offset || a.length != b.length || a.content_offset != b.content_offset ||
       a.content_length != b.content_length || a.parent_index != b.parent_index || a.level != b.level ||
       a.complete != b.complete || a.flags != b.flags) return false;
  }
  return true;
 };
 bool edited = inlay_doc_set_utf8(streamed, ETCH_STR_LIT("# 中文\n\nFirst line\nTail"));
 edited &= inlay_doc_block_count(streamed) == 2u;
 const uint32_t original_revision = inlay_doc_revision(streamed);
 edited &= !inlay_doc_replace_utf8(streamed, 3u, 5u, {});
 edited &= !inlay_doc_replace_utf8(streamed, 2u, 5u, ETCH_STR_LIT("\xe4\xbd"));
 edited &= !inlay_doc_replace_utf8(streamed, 5u, 2u, {});
 edited &= !inlay_doc_replace_utf8(streamed, 0u, UINT32_MAX, {});
 edited &= inlay_doc_replace_utf8(streamed, 2u, 5u, ETCH_STR_LIT("中"));
 edited &= inlay_doc_revision(streamed) == original_revision;
 const EtchStr old_source = inlay_doc_source(streamed);
 edited &= inlay_doc_replace_utf8(streamed, old_source.length - 4u, old_source.length,
                                  {old_source.chars + 2u, 6u, false});
 edited &= inlay_doc_revision(streamed) != original_revision;
 edited &= same_edited_doc(ETCH_STR_LIT("# 中文\n\nFirst line\n中文"));
 edited &= inlay_doc_replace_utf8(streamed, 0u, 2u, {});
 edited &= same_edited_doc(ETCH_STR_LIT("中文\n\nFirst line\n中文"));
 edited &= inlay_doc_append_utf8(streamed, ETCH_STR_LIT("\xe4\xbd"));
 edited &= !inlay_doc_replace_utf8(streamed, 0u, 0u, ETCH_STR_LIT("x"));
 edited &= inlay_doc_append_utf8(streamed, ETCH_STR_LIT("\xa0"));
 edited &= same_edited_doc(ETCH_STR_LIT("中文\n\nFirst line\n中文你"));
 edited &= inlay_doc_replace_utf8(streamed, 0u, inlay_doc_source(streamed).length, {});
 edited &= same_edited_doc({});
 edited &= inlay_doc_set_utf8(streamed, ETCH_STR_LIT("- old item\n\nOutside\n\nLast"));
 edited &= inlay_doc_block_count(streamed) == 3u;
 edited &= inlay_doc_replace_utf8(streamed, 12u, 19u, ETCH_STR_LIT("- merged"));
 edited &= same_edited_doc(ETCH_STR_LIT("- old item\n\n- merged\n\nLast"));
 edited &= inlay_doc_set_utf8(streamed, ETCH_STR_LIT("abcd"));
 edited &= inlay_doc_set_anchor(streamed, {9u, 4u, 4u}) && inlay_doc_set_anchor(streamed, {2u, 1u, 3u});
 InlayDocAnchor anchor = {};
 const auto at = [&](uint64_t id, uint32_t a, uint32_t b) {
  return inlay_doc_get_anchor(streamed, id, &anchor) && anchor.id == id && anchor.begin == a && anchor.end == b;
 };
 edited &= inlay_doc_anchor_count(streamed) == 2u && inlay_doc_anchor_at(streamed, 0u, &anchor) && anchor.id == 2u;
 edited &= inlay_doc_append_utf8(streamed, ETCH_STR_LIT("\xe4\xbd")) && at(9u,4u,4u);
 edited &= inlay_doc_append_utf8(streamed, ETCH_STR_LIT("\xa0")) && at(9u,7u,7u);
 edited &= !inlay_doc_set_anchor(streamed, {2u,1u,5u}) && at(2u,1u,3u);
 edited &= inlay_doc_replace_utf8(streamed, 1u, 1u, ETCH_STR_LIT("X")) && at(2u,2u,4u);
 edited &= inlay_doc_replace_utf8(streamed, 4u, 4u, ETCH_STR_LIT("Y")) && at(2u,2u,4u);
 edited &= inlay_doc_replace_utf8(streamed, 3u, 3u, ETCH_STR_LIT("!")) && at(2u,2u,5u);
 edited &= inlay_doc_replace_utf8(streamed, 1u, 6u, ETCH_STR_LIT("中文")) && at(2u,1u,7u);
 edited &= inlay_doc_replace_utf8(streamed, 0u, inlay_doc_source(streamed).length, {}) && at(2u,0u,0u) && at(9u,0u,0u);
 edited &= inlay_doc_remove_anchor(streamed, 9u) && !inlay_doc_remove_anchor(streamed, 9u);
 edited &= !inlay_doc_set_utf8(streamed, ETCH_STR_LIT("\xff")) && at(2u,0u,0u);
 edited &= inlay_doc_set_utf8(streamed, ETCH_STR_LIT("fresh")) && inlay_doc_anchor_count(streamed) == 0u;
 if (!edited) std::fprintf(stderr, "Document range-edit transaction or parse equivalence failed\n");
 ok &= edited;
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
 const auto invalid_fraction = inlay_measure_math(ETCH_STR_LIT("\\frac{x}"),24);
 ok &= !invalid_fraction.valid && invalid_fraction.error == INLAY_MATH_ERROR_INVALID_SYNTAX &&
       invalid_fraction.error_offset == 8u;
 const auto unsupported_math = inlay_measure_math(ETCH_STR_LIT("\\foobar{x}"),24);
 ok &= !unsupported_math.valid && unsupported_math.error == INLAY_MATH_ERROR_UNSUPPORTED_COMMAND &&
       unsupported_math.error_offset == 0u;
 const auto bad_index = inlay_measure_math(ETCH_STR_LIT("α+\\sqrt[\\unknown]{x}"),24);
 ok &= !bad_index.valid && bad_index.error == INLAY_MATH_ERROR_UNSUPPORTED_COMMAND && bad_index.error_offset == 9u;
 const auto bad_brace = inlay_measure_math(ETCH_STR_LIT("π}"),24);
 ok &= !bad_brace.valid && bad_brace.error_offset == 2u;
 char recursive_math[1024];uint32_t recursive_size=0;
 for(int level=0;level<100;++level){std::memcpy(recursive_math+recursive_size,"\\sqrt",5);recursive_size+=5;}
 recursive_math[recursive_size++]='x';
 const auto depth_error = inlay_measure_math({recursive_math,recursive_size,false},24);
 ok &= !depth_error.valid && depth_error.error == INLAY_MATH_ERROR_DEPTH_LIMIT && depth_error.error_offset > 0u;
 const auto diagram_desc=inlay_diagram_desc_default();
 etch_end_frame(ctx);
 etch_begin_frame(ctx,&f);
 {
  struct DiagnosticProbe {
   InlayDiagramDiagnosticKind kinds[8];
   uint32_t offsets[8], lengths[8], count;
  } probe = {};
  auto desc = diagram_desc;
  desc.diagnostic_user_data = &probe;
  desc.diagnostic = [](InlayDiagramDiagnosticKind kind, uint32_t offset, uint32_t length, void* user) {
   auto& diagnostics = *static_cast<DiagnosticProbe*>(user);
   const uint32_t i = diagnostics.count++;
   if (i >= 8u) return;
   diagnostics.kinds[i] = kind;
   diagnostics.offsets[i] = offset;
   diagnostics.lengths[i] = length;
  };
  const auto source = ETCH_STR_LIT("flowchart TD\nstyle --> B\n  classDef accent fill:red; linkStyle 0 stroke:blue\nclick B call");
  const auto partial = inlay_diagram_layout(source, &desc);
  bool diagnostics_ok = partial.valid && partial.node_count == 2u && partial.edge_count == 1u &&
                        partial.warning_count == 3u && probe.count == 3u;
  const EtchStr directives[] = {ETCH_STR_LIT("classDef"), ETCH_STR_LIT("linkStyle"), ETCH_STR_LIT("click")};
  for (uint32_t i = 0; i < 3u; ++i) {
   diagnostics_ok &= probe.kinds[i] == INLAY_DIAGRAM_UNSUPPORTED_DIRECTIVE &&
    probe.offsets[i] <= source.length && probe.lengths[i] <= source.length - probe.offsets[i] &&
    str_equal({source.chars + probe.offsets[i], probe.lengths[i], false}, directives[i]);
  }
  diagnostics_ok &= !inlay_diagram_layout(ETCH_STR_LIT("journey\n"), &desc).valid;
  diagnostics_ok &= !inlay_diagram_layout(ETCH_STR_LIT("graph XX\n"), &desc).valid;
  diagnostics_ok &= !inlay_diagram_layout(ETCH_STR_LIT("sequenceDiagram\nparticipant ["), &desc).valid;
  diagnostics_ok &= probe.count == 6u && probe.kinds[3] == INLAY_DIAGRAM_UNSUPPORTED_KIND &&
   probe.offsets[3] == 0u && probe.lengths[3] == 7u && probe.kinds[4] == INLAY_DIAGRAM_INVALID_DIRECTION &&
   probe.offsets[4] == 6u && probe.lengths[4] == 2u && probe.kinds[5] == INLAY_DIAGRAM_INVALID_SYNTAX;
  if (!diagnostics_ok) std::fprintf(stderr, "Mermaid diagnostic classification or source spans failed\n");
  ok &= diagnostics_ok;
 }
 etch_end_frame(ctx);
 etch_begin_frame(ctx,&f);
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
 ok &= phase4_md4c_selection_and_abi(ctx);
 ok &= upgrade_v011_regression(ctx,nullptr);

 // I1 regression: ordered/nested lists, inline parser upgrades (_/multi-tick/currency/balanced URL/nested link),
 // dynamic >16 views, cross-chunk code lexer+hscroll sync, task revision bump, TeX math & Sugiyama diagrams.
 {
  bool i1=true;
  auto* idoc=inlay_doc_create(ctx,nullptr);
  i1 &= idoc!=nullptr;
  bool i1_list=true,i1_inline=true,i1_code=true,i1_hscroll=true,i1_task=true,i1_views=true,i1_math=true,i1_sug=true;
  if(idoc) {
   const auto md=ETCH_STR_LIT(
    "1. First item\n"
    "2. Second item\n"
    "  - Nested bullet\n\n"
    "Use _italic_, __bold__, foo_bar_baz, ``a `b` c``, Costs $5 and $10, $x^2$, "
    "[C](https://en.wikipedia.org/wiki/C_(programming_language)) and [**bold** link](https://example.org).\n\n"
    "- [ ] Toggle me\n\n"
    "```cpp\n"
    "/* start multiline comment\n"
    "line02\nline03\nline04\nline05\nline06\nline07\nline08\n"
    "line09\nline10\nline11\nline12\nline13\nline14\nline15\nline16\n"
    "still_in_comment_chunk_2_0123456789012345678901234567890123456789 */\n"
    "int after = 42;\n"
    "```\n");
   i1 &= inlay_doc_set_utf8(idoc,md);
   auto v=inlay_doc_view_desc_default();v.id=etch_id("i1-main");v.width_px=340;v.height_px=0;v.show_code_line_numbers=true;
   auto fr=etch_frame_desc_default();fr.viewport_w=400;fr.viewport_h=900;
   for(int k=0;k<2;++k){etch_begin_frame(ctx,&fr);inlay_doc_view(idoc,&v);etch_end_frame(ctx);}
   // Ordered list markers preserved ("1.", "2.")
   i1_list &= contains_label(ctx,ETCH_STR_LIT("1.")) && contains_label(ctx,ETCH_STR_LIT("2."));
   // Inline token checks
   const InlayMeasure& vm=idoc->views[0];
   uint32_t mask=0;
   bool currency_became_math=false;
   for(uint32_t ti=0;ti<vm.token_count;++ti) {
    const auto& t=vm.tokens[ti];
    const EtchStr txt={md.chars+t.offset,t.length,false};
    if(str_equal(txt,ETCH_STR_LIT("italic")) && t.flags==InlayInlineEmphasis)mask|=1u;
    if(str_equal(txt,ETCH_STR_LIT("bold")) && t.flags==InlayInlineBold)mask|=2u;
    if(str_equal(txt,ETCH_STR_LIT("a `b` c")) && t.flags==InlayInlineCode)mask|=4u;
    if(str_equal(txt,ETCH_STR_LIT("x^2")) && t.flags==InlayInlineMath)mask|=8u;
    if((t.flags&InlayInlineMath) && str_equal(txt,ETCH_STR_LIT("5 and ")))currency_became_math=true;
    if(str_equal(txt,ETCH_STR_LIT("C")) && (t.flags&InlayInlineLink) &&
       str_equal({md.chars+t.url_offset,t.url_length,false},ETCH_STR_LIT("https://en.wikipedia.org/wiki/C_(programming_language)")))mask|=16u;
    if(str_equal(txt,ETCH_STR_LIT("bold")) && t.flags==(InlayInlineLink|InlayInlineBold) &&
       str_equal({md.chars+t.url_offset,t.url_length,false},ETCH_STR_LIT("https://example.org")))mask|=32u;
    if(t.flags==0 && txt.length>=11) {
     for(uint32_t k=0;k+11<=txt.length;++k)if(!std::memcmp(txt.chars+k,"foo_bar_baz",11))mask|=64u;
    }
   }
   i1_inline &= (mask==127u) && !currency_became_math;
   // Code block chunk 2 carries lex_state=1 (inside /* ... */) and precomputed start_line=17!
   uint32_t code_chunks=0,chunk1_rid=0,chunk2_rid=0;
   bool saw_lex_carry=false;
   for(uint32_t ri=0;ri<vm.count;++ri) {
    if(vm.rows[ri].kind!=INLAY_CODE_BLOCK)continue;
    ++code_chunks;
    const uint32_t rid=doc_row_id(idoc,v.id,vm.rows[ri]);
    if(code_chunks==1)chunk1_rid=rid;
    if(code_chunks==2){chunk2_rid=rid;saw_lex_carry=(vm.rows[ri].code_lex_state==1u && vm.rows[ri].code_start_line==17u);}
   }
   i1_code &= code_chunks==2 && saw_lex_carry;
   // Shared horizontal scroll across 16-line code chunks
   if(chunk1_rid && chunk2_rid) {
    etch_set_scroll_offset(etch_id_index(chunk1_rid,2),{60.f,0.f},true);
    etch_begin_frame(ctx,&fr);inlay_doc_view(idoc,&v);etch_end_frame(ctx);
    etch_begin_frame(ctx,&fr);inlay_doc_view(idoc,&v);etch_end_frame(ctx);
    i1_hscroll &= etch_scroll_offset(etch_id_index(chunk2_rid,2)).x>30.f;
   } else i1_hscroll=false;
   // Task toggle bumps doc->revision
   uint32_t task_row_id=0;
   for(uint32_t ri=0;ri<vm.count;++ri)if(vm.rows[ri].kind==INLAY_TASK_LIST){task_row_id=doc_row_id(idoc,v.id,vm.rows[ri]);break;}
   const uint32_t rev_before=idoc->revision;
   if(task_row_id) {
    const uint32_t cb_id=etch_id_index(etch_id_index(task_row_id,1),0x5441u);
    i1_task &= etch_perform_a11y_action(cb_id,ETCH_A11Y_ACT_PRESS,0,{},ctx,nullptr);
    etch_begin_frame(ctx,&fr);inlay_doc_view(idoc,&v);etch_end_frame(ctx);
    i1_task &= idoc->revision>rev_before;
   } else i1_task=false;
   // Dynamic views pool (>16 views on the same document all work without dropping)
   etch_begin_frame(ctx,&fr);
   for(uint32_t k=0;k<20;++k) {
    auto mv=v;mv.id=etch_id_index(etch_id("i1-multi"),k+1);mv.width_px=200;mv.height_px=120;
    const auto mr=inlay_doc_view(idoc,&mv);
    i1_views &= mr.total_rows>0;
   }
   etch_end_frame(ctx);
   i1_views &= idoc->view_count>=21;

   // TeX math engine checks: display limits, \sqrt[n]{x}, \binom, cases, aligned, vmatrix, \vec{i}, accents, \langle/\rangle
   etch_begin_frame(ctx,&fr);
   InlayMathPrepared disp_sum={},text_sum={};
   i1_math &= inlay_math_prepare(ETCH_STR_LIT("\\sum_{i=1}^{n} x_i"),24,true,&disp_sum);
   i1_math &= inlay_math_prepare(ETCH_STR_LIT("\\sum_{i=1}^{n} x_i"),24,false,&text_sum);
   i1_math &= disp_sum.metrics.height>text_sum.metrics.height;
   const auto sq2=inlay_measure_math(ETCH_STR_LIT("\\sqrt{x}"),24);
   const auto sq3=inlay_measure_math(ETCH_STR_LIT("\\sqrt[3]{x}"),24);
   i1_math &= sq3.valid && sq3.width>=sq2.width;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("\\binom{n}{k}"),24).valid;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("\\begin{cases}x & x>0\\\\-x & x\\le 0\\end{cases}"),24).valid;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("\\begin{aligned}a+b &= c\\\\d &= e+f\\end{aligned}"),24).valid;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("\\begin{vmatrix}a&b\\\\c&d\\end{vmatrix}"),24).valid;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("\\vec{i}+\\tilde{x}+\\bar{y}+\\dot{z}+\\underline{u}"),24).valid;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("\\left\\langle \\psi \\mid \\phi \\right\\rangle"),24).valid;
   i1_math &= inlay_measure_math(ETCH_STR_LIT("x^{\\frac{a}{b}}_{\\frac{c}{d}}"),24).valid;

   // Sugiyama diagram checks: shapes, multi-rank virtual waypoints, nested subgraphs, separated feedback lanes
   InlayDiagramMeta meta={};
   const auto dg_desc=inlay_diagram_desc_default();
   const auto sug=inlay_diagram_layout_ex(ETCH_STR_LIT(
    "flowchart TD\n"
    "subgraph Outer\n"
    "A([Start]) --> B{Decide}\n"
    "subgraph Inner\n"
    "B --> C(Work)\n"
    "C --> D((Done))\n"
    "end\n"
    "end\n"
    "A --> D\n"
    "D -. retry1 .-> A\n"
    "C -. retry2 .-> A\n"),&dg_desc,&meta);
   i1_sug &= sug.valid && sug.node_count==4 && meta.subgraph_count==2 && meta.waypoint_count>=2;
   if(sug.valid && meta.shapes && meta.subgraphs && meta.edge_paths && meta.subgraph_count==2) {
    i1_sug &= meta.shapes[0]==InlayShapeRound && meta.shapes[1]==InlayShapeDiamond &&
          meta.shapes[2]==InlayShapeRound && meta.shapes[3]==InlayShapeCircle;
    const auto ob=meta.subgraphs[0].bounds,ib=meta.subgraphs[1].bounds;
    i1_sug &= ob.x<=ib.x && ob.y<=ib.y && ob.x+ob.w>=ib.x+ib.w && ob.y+ob.h>=ib.y+ib.h;
    i1_sug &= meta.edge_paths[4].feedback_lane==0 && meta.edge_paths[5].feedback_lane==1;
   }
   etch_end_frame(ctx);
   i1 &= i1_list && i1_inline && i1_code && i1_hscroll && i1_task && i1_views && i1_math && i1_sug;
   if(!i1)std::fprintf(stderr,"I1 failed: list=%d inline=%d(mask=%u,cur=%d) code=%d(chunks=%u,lex=%d) hscroll=%d(c2x=%.1f) task=%d views=%d math=%d sug=%d(v=%d,wp=%u,sg=%u)\n",
                       i1_list,i1_inline,mask,currency_became_math,i1_code,code_chunks,saw_lex_carry,
                       i1_hscroll,chunk2_rid?etch_scroll_offset(etch_id_index(chunk2_rid,2)).x:-1.f,
                       i1_task,i1_views,i1_math,i1_sug,sug.valid,meta.waypoint_count,meta.subgraph_count);
   inlay_doc_destroy(idoc);
  }
  if(!i1)std::fprintf(stderr,"I1 upgrades check FAILED\n");
  ok &= i1;
 }

 // Track I3 checks: #26 Mermaid layout cache, #22 semantic A11y, #23 HTML5 & SVG exports, #42 async image loader.
 {
  bool i3=true;
  bool i3_cache=true,i3_a11y=true,i3_html=true,i3_dsvg=true,i3_msvg=true,i3_loader=true;
  auto fr=etch_frame_desc_default();fr.viewport_w=760;fr.viewport_h=960;

  // 1. #26 Mermaid cross-frame layout cache & #22 Semantic A11y nodes
  if(auto* doc3=inlay_doc_create(ctx,nullptr)) {
   const EtchStr rich_md=ETCH_STR_LIT(
    "# Heading One\n\n"
    "## Heading Two\n\n"
    "Visit [Etch Docs](https://example.org/docs) and note `<tag>&\"q\"`.\n\n"
    "```cpp\nint x = 1 < 2;\n```\n\n"
    "> Quoted note\n\n"
    "- Item A\n- Item B\n\n"
    "| Col1 | Col2 |\n| --- | --- |\n| V1 | V2 |\n\n"
    "---\n\n"
    "![Logo](logo.png)\n\n"
    "$$\n\\frac{a+b}{c}\n$$\n\n"
    "```mermaid\nflowchart TD\nA[Start] --> B[End]\n```\n");
   i3 &= inlay_doc_set_utf8(doc3,rich_md);

   auto v=inlay_doc_view_desc_default();
   v.id=etch_id("i3-doc-view");v.width_px=640;v.height_px=0;

   uint32_t entries=0;uint64_t hits=0,misses=0;
   etch_begin_frame(ctx,&fr);inlay_doc_view(doc3,&v);etch_end_frame(ctx);
   inlay_doc_diagram_cache_stats(doc3,&entries,&hits,&misses);
   i3_cache &= entries==1u && misses==1ull && hits==0ull;

   // Second & third frames must hit cache without incrementing misses!
   etch_begin_frame(ctx,&fr);inlay_doc_view(doc3,&v);etch_end_frame(ctx);
   etch_begin_frame(ctx,&fr);inlay_doc_view(doc3,&v);etch_end_frame(ctx);
   inlay_doc_diagram_cache_stats(doc3,&entries,&hits,&misses);
   i3_cache &= entries==1u && misses==1ull && hits==2ull;

   // Verify #22 Semantic A11y nodes emitted on last frame
   const auto str_contains=[](EtchStr hay,const char* needle) noexcept -> bool {
    const size_t nlen=std::strlen(needle);
    if(!hay.chars || nlen==0u || hay.length<nlen)return false;
    for(uint32_t off=0u;off+nlen<=hay.length;++off) {
     if(std::memcmp(hay.chars+off,needle,nlen)==0)return true;
    }
    return false;
   };
   bool saw_doc=false,saw_h1=false,saw_h2=false,saw_link=false,saw_math=false,saw_fig=false;
   const uint32_t acount=etch_a11y_node_count(ctx,nullptr);
   for(uint32_t ai=0;ai<acount;++ai) {
    EtchA11yNode n={};
    if(!etch_get_a11y_node(ai,&n,ctx,nullptr))continue;
    if(n.role==ETCH_A11Y_DOCUMENT) saw_doc=true;
    if(n.role==ETCH_A11Y_HEADING) {
     if(std::fabs(n.value_num-1.0f)<0.01f && str_equal(n.label,ETCH_STR_LIT("Heading One"))) saw_h1=true;
     if(std::fabs(n.value_num-2.0f)<0.01f && str_equal(n.label,ETCH_STR_LIT("Heading Two"))) saw_h2=true;
    }
    if(n.role==ETCH_A11Y_LINK) {
     if(str_equal(n.label,ETCH_STR_LIT("Etch Docs")) &&
        str_equal(n.value_text,ETCH_STR_LIT("https://example.org/docs")) &&
        (n.actions_mask & etch_a11y_action_bit(ETCH_A11Y_ACT_PRESS))!=0u) saw_link=true;
    }
    if(n.role==ETCH_A11Y_MATH) {
     if(str_equal(n.label,ETCH_STR_LIT("Math formula")) &&
        str_contains(n.value_text,"\\frac{a+b}{c}")) saw_math=true;
    }
    if(n.role==ETCH_A11Y_FIGURE) {
     if(str_equal(n.label,ETCH_STR_LIT("Diagram")) &&
        str_contains(n.value_text,"Flowchart: 2 nodes, 1 edges")) saw_fig=true;
    }
   }
   i3_a11y &= saw_doc && saw_h1 && saw_h2 && saw_link && saw_math && saw_fig;

   // 2. #23 Semantic HTML5 export
   const uint32_t html_need=inlay_doc_export_html(doc3,nullptr,0u);
   i3_html &= html_need>128u;
   char* html_buf=static_cast<char*>(SDL_malloc(size_t(html_need)+1u));
   if(html_buf) {
    const uint32_t html_got=inlay_doc_export_html(doc3,html_buf,html_need+1u);
    i3_html &= html_got==html_need && html_buf[html_need]=='\0';
    i3_html &= std::strstr(html_buf,"<h1>Heading One</h1>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<h2>Heading Two</h2>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<a href=\"https://example.org/docs\">Etch Docs</a>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<code>&lt;tag&gt;&amp;&quot;q&quot;</code>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<pre><code class=\"language-cpp\">int x = 1 &lt; 2;")!=nullptr;
    i3_html &= std::strstr(html_buf,"<blockquote><p>Quoted note</p></blockquote>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<ul>")!=nullptr && std::strstr(html_buf,"<li>Item A</li>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<table>")!=nullptr && std::strstr(html_buf,"<th>Col1</th>")!=nullptr && std::strstr(html_buf,"<td>V1</td>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<hr>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<figure><img src=\"logo.png\" alt=\"Logo\"></figure>")!=nullptr;
    i3_html &= std::strstr(html_buf,"<div class=\"math\">")!=nullptr;
    i3_html &= std::strstr(html_buf,"<div class=\"mermaid\">")!=nullptr;
    SDL_free(html_buf);
   } else i3_html=false;

   // Clearing doc frees diagram cache
   inlay_doc_clear(doc3);
   inlay_doc_diagram_cache_stats(doc3,&entries,&hits,&misses);
   i3_cache &= entries==0u && hits==0ull && misses==0ull;
   inlay_doc_destroy(doc3);
  } else i3=false;

  // 3. #23 Standalone Vector SVG exports (both OUTSIDE and INSIDE an active Etch frame!)
  const EtchStr dg_src=ETCH_STR_LIT(
   "flowchart TD\n"
   "subgraph Group1\n"
   "A([Start]) --> B{Check}\n"
   "B -. retry .-> C((Done))\n"
   "end\n");
  const EtchStr seq_src=ETCH_STR_LIT("sequenceDiagram\nAlice->>Bob: Hello\nBob-->>Alice: Hi\n");
  const EtchStr math_src=ETCH_STR_LIT("\\frac{a+b}{\\sqrt{x^2+1}}");

  const auto check_diagram_svg=[](EtchStr src,bool expect_shapes) -> bool {
   const uint32_t need=inlay_diagram_export_svg(src,nullptr,nullptr,0u);
   if(need<64u)return false;
   char* buf=static_cast<char*>(SDL_malloc(size_t(need)+1u));
   if(!buf)return false;
   const uint32_t got=inlay_diagram_export_svg(src,nullptr,buf,need+1u);
   bool ok_s = got==need && buf[need]=='\0' &&
               std::strstr(buf,"<svg xmlns=\"http://www.w3.org/2000/svg\"")!=nullptr &&
               std::strstr(buf,"<defs>")!=nullptr &&
               std::strstr(buf,"</svg>")!=nullptr;
   if(expect_shapes) {
    ok_s &= std::strstr(buf,"<polygon")!=nullptr &&
            std::strstr(buf,"<circle")!=nullptr &&
            std::strstr(buf,"stroke-dasharray")!=nullptr;
   }
   SDL_free(buf);
   return ok_s;
  };
  const auto check_math_svg=[](EtchStr src) -> bool {
   const uint32_t need=inlay_math_export_svg(src,nullptr,nullptr,0u);
   if(need<64u)return false;
   char* buf=static_cast<char*>(SDL_malloc(size_t(need)+1u));
   if(!buf)return false;
   const uint32_t got=inlay_math_export_svg(src,nullptr,buf,need+1u);
   const bool ok_m = got==need && buf[need]=='\0' &&
                     std::strstr(buf,"<svg xmlns=\"http://www.w3.org/2000/svg\"")!=nullptr &&
                     std::strstr(buf,"<line")!=nullptr &&
                     std::strstr(buf,"<polyline")!=nullptr &&
                     std::strstr(buf,"<text")!=nullptr &&
                     std::strstr(buf,"</svg>")!=nullptr;
   SDL_free(buf);
   return ok_m;
  };
  // Outside active frame:
  i3_dsvg &= check_diagram_svg(dg_src,true) && check_diagram_svg(seq_src,false);
  i3_msvg &= check_math_svg(math_src);
  // Inside active frame:
  etch_begin_frame(ctx,&fr);
  i3_dsvg &= check_diagram_svg(dg_src,true);
  i3_msvg &= check_math_svg(math_src);
  etch_end_frame(ctx);

  // 4. #42 Non-blocking Async Image Loader (SVG + raster BMP + cancel + resolve + epoch)
  if(auto* loader=inlay_image_loader_create(ctx,nullptr)) {
   static const char kSvgBytes[]="<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\"><rect width=\"24\" height=\"24\" fill=\"#4f6ef7\"/></svg>";
   static const uint8_t kBmp1x1[58]={
    0x42,0x4D, 0x3A,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x36,0x00,0x00,0x00,
    0x28,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x01,0x00, 0x18,0x00,
    0x00,0x00,0x00,0x00, 0x04,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xFF,0x80,0x40,0x00
   };
   const uint32_t ep0=inlay_image_loader_epoch(loader);
   i3_loader &= inlay_image_loader_status(loader,ETCH_STR_LIT("icon.svg"))==INLAY_IMAGE_STATUS_IDLE;
   i3_loader &= inlay_image_loader_request_memory(loader,ETCH_STR_LIT("icon.svg"),kSvgBytes,sizeof(kSvgBytes)-1u,true);
   i3_loader &= inlay_image_loader_request_memory(loader,ETCH_STR_LIT("dot.bmp"),kBmp1x1,sizeof(kBmp1x1),true);
   i3_loader &= inlay_image_loader_request_memory(loader,ETCH_STR_LIT("cancel.bmp"),kBmp1x1,sizeof(kBmp1x1),true);
   inlay_image_loader_cancel(loader,ETCH_STR_LIT("cancel.bmp"));
   i3_loader &= inlay_image_loader_status(loader,ETCH_STR_LIT("cancel.bmp"))==INLAY_IMAGE_STATUS_IDLE;

   for(int iter=0;iter<200;++iter) {
    (void)inlay_image_loader_poll(loader,4u);
    if(inlay_image_loader_status(loader,ETCH_STR_LIT("icon.svg"))==INLAY_IMAGE_STATUS_READY &&
       inlay_image_loader_status(loader,ETCH_STR_LIT("dot.bmp"))==INLAY_IMAGE_STATUS_READY) break;
    SDL_Delay(5);
   }
   i3_loader &= inlay_image_loader_status(loader,ETCH_STR_LIT("icon.svg"))==INLAY_IMAGE_STATUS_READY;
   i3_loader &= inlay_image_loader_status(loader,ETCH_STR_LIT("dot.bmp"))==INLAY_IMAGE_STATUS_READY;
   i3_loader &= inlay_image_loader_status(loader,ETCH_STR_LIT("cancel.bmp"))==INLAY_IMAGE_STATUS_IDLE;
   i3_loader &= inlay_image_loader_epoch(loader)>=ep0+2u;

   InlayImageResult res_svg={},res_bmp={},res_cancel={};
   i3_loader &= inlay_image_loader_resolve(ETCH_STR_LIT("icon.svg"),&res_svg,loader) &&
                res_svg.texture!=nullptr && res_svg.width_px>0.f && res_svg.height_px>0.f;
   i3_loader &= inlay_image_loader_resolve(ETCH_STR_LIT("dot.bmp"),&res_bmp,loader) &&
                res_bmp.texture!=nullptr && res_bmp.width_px==1.f && res_bmp.height_px==1.f;
   i3_loader &= !inlay_image_loader_resolve(ETCH_STR_LIT("cancel.bmp"),&res_cancel,loader);

   // Off-thread invalid payload reports ERROR cleanly, and destroying with in-flight jobs leaks nothing.
   static const uint8_t kBadBytes[8]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
   i3_loader &= inlay_image_loader_request_memory(loader,ETCH_STR_LIT("bad.bin"),kBadBytes,sizeof(kBadBytes),true);
   for(int iter=0;iter<200;++iter) {
    (void)inlay_image_loader_poll(loader,1u);
    if(inlay_image_loader_status(loader,ETCH_STR_LIT("bad.bin"))==INLAY_IMAGE_STATUS_ERROR) break;
    SDL_Delay(5);
   }
   i3_loader &= inlay_image_loader_status(loader,ETCH_STR_LIT("bad.bin"))==INLAY_IMAGE_STATUS_ERROR;
   i3_loader &= inlay_image_loader_request_memory(loader,ETCH_STR_LIT("unpolled.svg"),kSvgBytes,sizeof(kSvgBytes)-1u,true);
   SDL_Delay(15);
   inlay_image_loader_destroy(loader);
  } else i3_loader=false;

  // 5. Wave-2 Track I3 checks: C01 O(delta) open code fence streaming, C02 outline & find_all,
  //    C03 scroll A11y actions, C04 copy_html, C05 MathML export.
  bool i3_fence=true,i3_outline_find_html=true,i3_mathml=true,i3_scroll_a11y=true;

  // C01: 64 KiB fenced code appended in 256-byte chunks with per-chunk parse checks -> exact equivalence with set_utf8!
  {
   auto* ref_f=inlay_doc_create(ctx,nullptr);
   auto* str_f=inlay_doc_create(ctx,nullptr);
   const size_t fence_cap=72u*1024u;
   char* fence_buf=static_cast<char*>(SDL_malloc(fence_cap));
   if(ref_f && str_f && fence_buf) {
    size_t flen=0u;
    const auto push_lit=[&](const char* s) noexcept {
     const size_t n=std::strlen(s);
     if(flen+n<=fence_cap){std::memcpy(fence_buf+flen,s,n);flen+=n;}
    };
    push_lit("# Fence Stream\n\nIntro paragraph.\n\n```cpp\n");
    const char* code_line="const int value = compute_delta(alpha, beta) + 42; // `inline` tick ok\n";
    while(flen < 65536u) push_lit(code_line);
    push_lit("    ```not-closing-4-spaces\n");
    push_lit("```\n\nTrailing paragraph.\n");

    i3_fence &= inlay_doc_set_utf8(ref_f,{fence_buf,static_cast<uint32_t>(flen),false});
    bool saw_open_fence=false;
    for(size_t pos=0u;pos<flen;) {
     const size_t n=(flen-pos<256u) ? (flen-pos) : 256u;
     i3_fence &= inlay_doc_append_utf8(str_f,{fence_buf+pos,static_cast<uint32_t>(n),false});
     const uint32_t bc=inlay_doc_block_count(str_f);
     if(pos>1024u && pos+512u<flen) {
      i3_fence &= bc==3u && inlay_doc_stream_status(str_f)==INLAY_DOC_STREAM_OPEN_FENCE;
      saw_open_fence=true;
     }
     pos+=n;
    }
    i3_fence &= saw_open_fence && inlay_doc_stream_status(str_f)==INLAY_DOC_STREAM_READY;
    const uint32_t rcnt=inlay_doc_block_count(ref_f);
    i3_fence &= rcnt==4u && inlay_doc_block_count(str_f)==rcnt;
    for(uint32_t bi=0u;bi<rcnt && bi<inlay_doc_block_count(str_f);++bi) {
     const auto a=*inlay_doc_block(ref_f,bi),b=*inlay_doc_block(str_f,bi);
     i3_fence &= a.kind==b.kind && a.offset==b.offset && a.length==b.length &&
                 a.content_offset==b.content_offset && a.content_length==b.content_length &&
                 a.level==b.level && a.language_offset==b.language_offset &&
                 a.language_length==b.language_length && a.complete==b.complete &&
                 a.flags==b.flags && a.quote_depth==b.quote_depth && a.list_depth==b.list_depth;
    }
    // Split closing fence across chunk boundaries ("  " -> "``" -> "`\n") + tilde fence equivalence.
    const EtchStr split_fence=ETCH_STR_LIT("~~~py\nx = 1\n  ~~~\n\nDone.\n");
    i3_fence &= inlay_doc_set_utf8(ref_f,split_fence);
    inlay_doc_clear(str_f);
    const uint32_t cuts[]={6u,6u,2u,2u,2u,split_fence.length-18u};
    uint32_t sp=0u;
    for(uint32_t cn:cuts) {
     i3_fence &= inlay_doc_append_utf8(str_f,{split_fence.chars+sp,cn,false});
     (void)inlay_doc_block_count(str_f);
     sp+=cn;
    }
    i3_fence &= inlay_doc_block_count(str_f)==inlay_doc_block_count(ref_f) && inlay_doc_block_count(ref_f)==2u;
    for(uint32_t bi=0u;bi<inlay_doc_block_count(ref_f) && bi<inlay_doc_block_count(str_f);++bi) {
     const auto a=*inlay_doc_block(ref_f,bi),b=*inlay_doc_block(str_f,bi);
     i3_fence &= a.kind==b.kind && a.offset==b.offset && a.length==b.length &&
                 a.content_offset==b.content_offset && a.content_length==b.content_length &&
                 a.complete==b.complete;
    }
   } else i3_fence=false;
   SDL_free(fence_buf);
   inlay_doc_destroy(ref_f);
   inlay_doc_destroy(str_f);
  }

  // C02 (outline + find_all), C03 (SCROLL_UP/SCROLL_DOWN on ETCH_A11Y_DOCUMENT), C04 (copy_html)
  if(auto* d2=inlay_doc_create(ctx,nullptr)) {
   const EtchStr md2=ETCH_STR_LIT(
    "# Intro Alpha\n\n"
    "Paragraph with **bold ALPHA**, *italic*, `code<1>`, and [Etch](https://etch.dev).\n\n"
    "## Architecture\n\n"
    "```cpp\nint alpha = a < b;\n```\n\n"
    "### Details\n\n"
    "Final Alpha note.\n");
   i3_outline_find_html &= inlay_doc_set_utf8(d2,md2);

   // Outline enumeration
   i3_outline_find_html &= inlay_doc_outline_count(d2)==3u;
   InlayDocOutlineEntry e0={},e1={},e2={},ebad={};
   i3_outline_find_html &= inlay_doc_outline_at(d2,0u,&e0) && e0.level==1u && e0.block_index==0u && e0.source_offset==0u &&
                           str_equal(e0.text,ETCH_STR_LIT("Intro Alpha"));
   i3_outline_find_html &= inlay_doc_outline_at(d2,1u,&e1) && e1.level==2u && e1.block_index==2u && e1.source_offset>0u &&
                           str_equal(e1.text,ETCH_STR_LIT("Architecture"));
   i3_outline_find_html &= inlay_doc_outline_at(d2,2u,&e2) && e2.level==3u && e2.block_index==4u && e2.source_offset>e1.source_offset &&
                           str_equal(e2.text,ETCH_STR_LIT("Details"));
   i3_outline_find_html &= !inlay_doc_outline_at(d2,3u,&ebad);

   // Render fixed-height view for C03 A11y scroll actions + C02 find_all + C04 copy_html
   auto v2=inlay_doc_view_desc_default();
   v2.id=etch_id("i3-w2-scroll-view");v2.width_px=480.f;v2.height_px=240.f;
   etch_begin_frame(ctx,&fr);inlay_doc_view(d2,&v2);etch_end_frame(ctx);

   // C03: Verify ETCH_A11Y_ACT_SCROLL_UP & ETCH_A11Y_ACT_SCROLL_DOWN on ETCH_A11Y_DOCUMENT
   bool saw_scroll_doc=false;
   const uint32_t expected_scroll_bits = etch_a11y_action_bit(ETCH_A11Y_ACT_SCROLL_UP) |
                                         etch_a11y_action_bit(ETCH_A11Y_ACT_SCROLL_DOWN);
   for(uint32_t ai=0u;ai<etch_a11y_node_count(ctx,nullptr);++ai) {
    EtchA11yNode an={};
    if(etch_get_a11y_node(ai,&an,ctx,nullptr) && an.role==ETCH_A11Y_DOCUMENT && an.id==v2.id) {
     saw_scroll_doc = (an.actions_mask & expected_scroll_bits) == expected_scroll_bits;
     break;
    }
   }
   i3_scroll_a11y &= saw_scroll_doc;

   // C02: find_all (case-insensitive across heading, paragraph, code, final paragraph -> 4 matches)
   const uint32_t total_matches=inlay_doc_view_find_all(d2,v2.id,ETCH_STR_LIT("alpha"),nullptr,0u);
   InlayDocSelection two_matches[2]={};
   const uint32_t total_cap2=inlay_doc_view_find_all(d2,v2.id,ETCH_STR_LIT("ALPHA"),two_matches,2u);
   InlayDocSelection all_matches[8]={};
   const uint32_t total_all=inlay_doc_view_find_all(d2,v2.id,ETCH_STR_LIT("Alpha"),all_matches,8u);
   i3_outline_find_html &= total_matches==4u && total_cap2==4u && total_all==4u;
   i3_outline_find_html &= two_matches[0].active && two_matches[1].active && two_matches[0].focus<=two_matches[1].anchor;
   for(uint32_t mi=0u;mi<4u;++mi) {
    i3_outline_find_html &= all_matches[mi].active && (all_matches[mi].focus - all_matches[mi].anchor)==5u;
    if(mi>0u) i3_outline_find_html &= all_matches[mi].anchor >= all_matches[mi-1u].focus;
   }
   i3_outline_find_html &= inlay_doc_view_find_all(d2,v2.id,ETCH_STR_LIT("\xe7"),all_matches,8u)==0u;

   // C04: copy_html on full selection + truncation check
   inlay_doc_view_select_all(d2,v2.id);
   const uint32_t ch_need=inlay_doc_view_copy_html(d2,v2.id,nullptr,0u);
   i3_outline_find_html &= ch_need>80u;
   char trunc_buf[8]={};
   i3_outline_find_html &= inlay_doc_view_copy_html(d2,v2.id,trunc_buf,static_cast<uint32_t>(sizeof(trunc_buf)))==ch_need && trunc_buf[7]=='\0';
   char* ch_buf=static_cast<char*>(SDL_malloc(size_t(ch_need)+1u));
   if(ch_buf) {
    const uint32_t ch_got=inlay_doc_view_copy_html(d2,v2.id,ch_buf,ch_need+1u);
    i3_outline_find_html &= ch_got==ch_need && ch_buf[ch_need]=='\0';
    i3_outline_find_html &= std::strstr(ch_buf,"<h1>Intro Alpha</h1>")!=nullptr;
    i3_outline_find_html &= std::strstr(ch_buf,"<strong>bold ALPHA</strong>")!=nullptr;
    i3_outline_find_html &= std::strstr(ch_buf,"<em>italic</em>")!=nullptr;
    i3_outline_find_html &= std::strstr(ch_buf,"<code>code&lt;1&gt;</code>")!=nullptr;
    i3_outline_find_html &= std::strstr(ch_buf,"<a href=\"https://etch.dev\">Etch</a>")!=nullptr;
    i3_outline_find_html &= std::strstr(ch_buf,"<h2>Architecture</h2>")!=nullptr;
    i3_outline_find_html &= std::strstr(ch_buf,"<pre><code class=\"language-cpp\">int alpha = a &lt; b;</code></pre>")!=nullptr;
    SDL_free(ch_buf);
   } else i3_outline_find_html=false;

   inlay_doc_destroy(d2);
  } else { i3_outline_find_html=false; i3_scroll_a11y=false; }

  // C05: Standard MathML export (outside and inside active frame)
  {
   const auto check_mathml=[](EtchStr tex,const char* must1,const char* must2,const char* must3) -> bool {
    const uint32_t need=inlay_math_export_mathml(tex,nullptr,0u);
    if(need<32u) return false;
    char short_b[8]={};
    if(inlay_math_export_mathml(tex,short_b,static_cast<uint32_t>(sizeof(short_b)))!=need || short_b[7]!='\0') return false;
    char* buf=static_cast<char*>(SDL_malloc(size_t(need)+1u));
    if(!buf) return false;
    const uint32_t got=inlay_math_export_mathml(tex,buf,need+1u);
    bool ok_ml = got==need && buf[need]=='\0' &&
                 std::strstr(buf,"<math xmlns=\"http://www.w3.org/1998/Math/MathML\"")!=nullptr &&
                 std::strstr(buf,"</math>")!=nullptr;
    if(must1) ok_ml &= std::strstr(buf,must1)!=nullptr;
    if(must2) ok_ml &= std::strstr(buf,must2)!=nullptr;
    if(must3) ok_ml &= std::strstr(buf,must3)!=nullptr;
    SDL_free(buf);
    return ok_ml;
   };
   // Outside active frame:
   i3_mathml &= check_mathml(ETCH_STR_LIT("\\frac{a+b}{\\sqrt{x^2+1}}"),"<mfrac>","<msqrt>","<msup>");
   i3_mathml &= check_mathml(ETCH_STR_LIT("\\sqrt[3]{y_i^2}"),"<mroot>","<msubsup>","<mn>3</mn>");
   i3_mathml &= check_mathml(ETCH_STR_LIT("\\begin{bmatrix}a&b\\\\c&d\\end{bmatrix}"),"<mtable>","<mtr>","<mtd>");
   i3_mathml &= check_mathml(ETCH_STR_LIT("a < b"),"<mo>&lt;</mo>","<mi>a</mi>","<mi>b</mi>");
   char bad_ml[8]={'X','X','X','X','X','X','X','X'};
   i3_mathml &= inlay_math_export_mathml(ETCH_STR_LIT("\\frac{x}"),bad_ml,static_cast<uint32_t>(sizeof(bad_ml)))==0u && bad_ml[0]=='\0';
   // Inside active frame:
   etch_begin_frame(ctx,&fr);
   i3_mathml &= check_mathml(ETCH_STR_LIT("\\frac{a+b}{\\sqrt{x^2+1}}"),"<mfrac>","<msqrt>","<msup>");
   etch_end_frame(ctx);
  }

  // C06: Pluggable code_highlight_fn + Mermaid stateDiagram-v2 & configurable node/edge capacity
  bool i3_hl_state = true;
  {
   struct CustomHlCtx { uint32_t calls; uint32_t last_line; };
   CustomHlCtx hl_ctx = {0u, 999u};
   const auto custom_hl = [](EtchStr lang, EtchStr line, uint32_t line_idx,
                             InlayCodeHighlightToken* out, uint32_t max_toks, void* user) -> uint32_t {
    auto* c = static_cast<CustomHlCtx*>(user);
    ++c->calls;
    c->last_line = line_idx;
    if (lang.length == 4u && std::memcmp(lang.chars, "rust", 4u) == 0 && line.length >= 2u && max_toks >= 1u) {
     out[0] = {0u, 2u, etch_rgba8(245, 158, 11, 255), 0u};
     return 1u;
    }
    return 0u;
   };
   InlayDoc* d3 = inlay_doc_create(ctx, nullptr);
   i3_hl_state &= d3 && inlay_doc_set_utf8(d3, ETCH_STR_LIT("```rust\nfn main() {}\nlet x = 42;\n```\n"));
   if (d3) {
    InlayDocViewDesc v3 = inlay_doc_view_desc_default();
    v3.id = etch_id("i3-custom-hl");
    v3.width_px = 420.f;
    v3.height_px = 0.f;
    v3.code_highlight_fn = custom_hl;
    v3.code_highlight_user_data = &hl_ctx;
    etch_begin_frame(ctx, &fr);
    const InlayDocResult r3 = inlay_doc_view(d3, &v3);
    // stateDiagram-v2 with composite state, start/end markers, and transition labels
    static const char kStateDiag[] =
        "stateDiagram-v2\n"
        "  direction LR\n"
        "  [*] --> Idle\n"
        "  state \"Executing Tool\" as Active {\n"
        "    Plan --> Run : dispatch\n"
        "  }\n"
        "  Idle --> Plan : prompt\n"
        "  Run --> [*] : complete\n";
    InlayDiagramDesc sd = inlay_diagram_desc_default();
    const InlayDiagramResult sres = inlay_diagram_view({kStateDiag, uint32_t(sizeof(kStateDiag) - 1u), true}, &sd);
    char ssvg[8192] = {};
    const uint32_t ssvg_len = inlay_diagram_export_svg({kStateDiag, uint32_t(sizeof(kStateDiag) - 1u), true}, &sd, ssvg, uint32_t(sizeof(ssvg)));
    // Configurable max_nodes > default 256 (300 nodes chain)
    char big_src[6144];
    uint32_t bp = uint32_t(std::snprintf(big_src, sizeof(big_src), "flowchart LR\n"));
    for (uint32_t ni = 0u; ni < 299u && bp + 24u < sizeof(big_src); ++ni) {
     bp += uint32_t(std::snprintf(big_src + bp, sizeof(big_src) - bp, "N%u-->N%u\n", ni, ni + 1u));
    }
    InlayDiagramDesc def_cap = inlay_diagram_desc_default();
    InlayDiagramDesc big_cap = inlay_diagram_desc_default();
    big_cap.max_nodes = 512u;
    big_cap.max_edges = 1024u;
    const InlayDiagramResult r_def = inlay_diagram_layout({big_src, bp, false}, &def_cap);
    const InlayDiagramResult r_big = inlay_diagram_layout({big_src, bp, false}, &big_cap);
    etch_end_frame(ctx);
    i3_hl_state &= r3.measured_height_px > 0.f && hl_ctx.calls == 2u && hl_ctx.last_line == 1u;
    i3_hl_state &= sres.valid && sres.horizontal && sres.node_count == 5u && sres.edge_count == 4u &&
                   ssvg_len > 0u && std::strstr(ssvg, "<circle") != nullptr;
    i3_hl_state &= !r_def.valid && r_big.valid && r_big.node_count == 300u && r_big.edge_count == 299u;
    inlay_doc_destroy(d3);
   }
  }

  // C07: Streaming unclosed inline auto-repair, CJK emphasis & autolink boundary, and selection_rect
  bool i3_r2 = true;
  {
   InlayDoc* d4 = inlay_doc_create(ctx, nullptr);
   i3_r2 &= d4 != nullptr;
   if (d4) {
    // 1. Streaming repair on incomplete bold / code / link / math
    const auto stream_md = ETCH_STR_LIT("Alpha **unclosed bold");
    i3_r2 &= inlay_doc_set_utf8(d4, stream_md);
    InlayDocViewDesc v4 = inlay_doc_view_desc_default();
    v4.id = etch_id("i3-r2-stream");
    v4.width_px = 420.f;
    v4.height_px = 0.f;
    v4.streaming_repair = true;
    etch_begin_frame(ctx, &fr);
    inlay_doc_view(d4, &v4);
    etch_end_frame(ctx);
    const InlayMeasure* m4 = nullptr;
    for (uint32_t vi = 0u; vi < d4->view_count; ++vi) if (d4->views[vi].id == v4.id) m4 = &d4->views[vi];
    bool saw_repaired_bold = false;
    if (m4) {
     for (uint32_t ti = 0u; ti < m4->token_count; ++ti) {
      const auto& t = m4->tokens[ti];
      const EtchStr txt = {stream_md.chars + t.offset, t.length, false};
      if ((t.flags & InlayInlineBold) && str_equal(txt, ETCH_STR_LIT("unclosed bold"))) saw_repaired_bold = true;
     }
    }
    i3_r2 &= saw_repaired_bold;

    // Incomplete code & partial link repair
    const auto stream_md2 = ETCH_STR_LIT("See [Orca](https://github.com/stably");
    i3_r2 &= inlay_doc_set_utf8(d4, stream_md2);
    etch_begin_frame(ctx, &fr);
    inlay_doc_view(d4, &v4);
    etch_end_frame(ctx);
    bool saw_repaired_link = false;
    if (m4) {
     for (uint32_t ti = 0u; ti < m4->token_count; ++ti) {
      const auto& t = m4->tokens[ti];
      const EtchStr txt = {stream_md2.chars + t.offset, t.length, false};
      if ((t.flags & InlayInlineLink) && str_equal(txt, ETCH_STR_LIT("Orca"))) saw_repaired_link = true;
     }
    }
    i3_r2 &= saw_repaired_link;

    // Toggle streaming_repair off -> strict CommonMark literal fallback (Orca is NOT a Markdown link)
    v4.streaming_repair = false;
    etch_begin_frame(ctx, &fr);
    inlay_doc_view(d4, &v4);
    etch_end_frame(ctx);
    bool saw_link_strict = false;
    if (m4) {
     for (uint32_t ti = 0u; ti < m4->token_count; ++ti) {
      const auto& t = m4->tokens[ti];
      const EtchStr txt = {stream_md2.chars + t.offset, t.length, false};
      if ((t.flags & InlayInlineLink) && str_equal(txt, ETCH_STR_LIT("Orca"))) saw_link_strict = true;
     }
    }
    i3_r2 &= !saw_link_strict;

    // 2. CJK emphasis + CJK autolink punctuation boundary + selection_rect
    const auto cjk_md = ETCH_STR_LIT("**中文（括号）**后和*斜体*，访问 https://orca.dev，打开应用");
    i3_r2 &= inlay_doc_set_utf8(d4, cjk_md);
    etch_begin_frame(ctx, &fr);
    inlay_doc_view(d4, &v4);
    etch_end_frame(ctx);
    bool cjk_bold = false, cjk_em = false, cjk_url = false;
    if (m4) {
     for (uint32_t ti = 0u; ti < m4->token_count; ++ti) {
      const auto& t = m4->tokens[ti];
      const EtchStr txt = {cjk_md.chars + t.offset, t.length, false};
      if ((t.flags & InlayInlineBold) && str_equal(txt, ETCH_STR_LIT("中文（括号）"))) cjk_bold = true;
      if ((t.flags & InlayInlineEmphasis) && str_equal(txt, ETCH_STR_LIT("斜体"))) cjk_em = true;
      if ((t.flags & InlayInlineLink) && str_equal(txt, ETCH_STR_LIT("https://orca.dev"))) cjk_url = true;
     }
    }
    i3_r2 &= cjk_bold && cjk_em && cjk_url;

    // 3. selection_rect screen bounding box
    inlay_doc_view_select_all(d4, v4.id);
    etch_begin_frame(ctx, &fr);
    inlay_doc_view(d4, &v4);
    etch_end_frame(ctx);
    EtchRect sel_rc = {}, cleared_rc = {};
    i3_r2 &= inlay_doc_view_selection_rect(d4, v4.id, &sel_rc) && sel_rc.w > 10.f && sel_rc.h > 8.f;
    inlay_doc_view_clear_selection(d4, v4.id);
    const bool cleared_ok = !inlay_doc_view_selection_rect(d4, v4.id, &cleared_rc);
    i3_r2 &= cleared_ok;
    if (!i3_r2) {
     std::fprintf(stderr, "i3_r2 diag: bold=%d link=%d strict=%d cjk=(%d,%d,%d) sel=(%.1f,%.1f) clr=%d vis_segs=%u\n",
                  saw_repaired_bold, saw_repaired_link, !saw_link_strict, cjk_bold, cjk_em, cjk_url,
                  sel_rc.w, sel_rc.h, cleared_ok, m4 ? m4->vis_seg_count : 0u);
    }

    inlay_doc_destroy(d4);
   }
  }

  // Round 3: Inline HTML tags + Mermaid classDiagram / erDiagram / pie / gantt
  bool i3_r3 = true;
  {
   InlayDoc* d5 = inlay_doc_create(ctx, nullptr);
   i3_r3 &= d5 != nullptr;
   if (d5) {
    const auto html_md = ETCH_STR_LIT(
        "Press <kbd>Ctrl</kbd>+<kbd>K</kbd><br>Water H<sub>2</sub>O and x<sup>2</sup> "
        "<mark>hi</mark> <u>under</u> <del>old</del> <b>bold</b> <i>em</i> <code>fn()</code> "
        "<img src=\"logo.png\" alt=\"Logo\">");
    i3_r3 &= inlay_doc_set_utf8(d5, html_md);
    EtchFrameDesc fr5 = etch_frame_desc_default();
    fr5.viewport_w = 640; fr5.viewport_h = 480;
    InlayDocViewDesc v5 = inlay_doc_view_desc_default();
    v5.id = etch_id("i3-r3-html");
    v5.width_px = 560.f;
    v5.height_px = 0.f;
    etch_begin_frame(ctx, &fr5);
    const InlayDocResult r5 = inlay_doc_view(d5, &v5);
    etch_end_frame(ctx);
    i3_r3 &= r5.measured_height_px > 20.f;

    const InlayMeasure* m5 = nullptr;
    for (uint32_t vi = 0u; vi < d5->view_count; ++vi) {
     if (d5->views[vi].id == v5.id) { m5 = &d5->views[vi]; break; }
    }
    uint32_t seen_flags = 0u;
    if (m5) {
     for (uint32_t ti = 0u; ti < m5->token_count; ++ti) seen_flags |= m5->tokens[ti].flags;
    }
    const uint32_t want_flags = InlayInlineKbd | InlayInlineBreak | InlayInlineSub | InlayInlineSup |
                                InlayInlineHighlight | InlayInlineUnderline | InlayInlineStrike |
                                InlayInlineBold | InlayInlineEmphasis | InlayInlineCode | InlayInlineImage;
    const bool flags_ok = (seen_flags & want_flags) == want_flags;
    i3_r3 &= flags_ok;

    // Mermaid Round 3 diagram types: classDiagram, erDiagram, pie, gantt
    const InlayDiagramDesc dd = inlay_diagram_desc_default();
    char svg_buf[16384] = {};

    const auto cls_src = ETCH_STR_LIT(
        "classDiagram\n"
        "class Animal {\n  +String name\n  +speak()\n}\n"
        "class Dog {\n  +bark()\n}\n"
        "Animal <|-- Dog\n");
    const auto er_src = ETCH_STR_LIT(
        "erDiagram\n"
        "CUSTOMER ||--o{ ORDER : places\n"
        "ORDER ||--|{ LINE_ITEM : contains\n");
    const auto pie_src = ETCH_STR_LIT(
        "pie title Compute Budget\n"
        "\"GPU\" : 60\n"
        "\"Layout\" : 25\n"
        "\"JS\" : 15\n");
    const auto gantt_src = ETCH_STR_LIT(
        "gantt\n"
        "title Release Plan\n"
        "section Core\n"
        "Etch :done, e1, 2026-01-01, 5d\n"
        "Inlay :active, i1, after e1, 4d\n"
        "section Polish\n"
        "QA :crit, q1, after i1, 2d\n");

    etch_begin_frame(ctx, &fr5);
    const InlayDiagramResult cls_r = inlay_diagram_layout(cls_src, &dd);
    const InlayDiagramResult er_r = inlay_diagram_layout(er_src, &dd);
    const InlayDiagramResult pie_r = inlay_diagram_layout(pie_src, &dd);
    const InlayDiagramResult gantt_r = inlay_diagram_layout(gantt_src, &dd);
    etch_end_frame(ctx);

    const uint32_t cls_svg = inlay_diagram_export_svg(cls_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool cls_ok = cls_r.valid && cls_r.node_count >= 2u && cls_r.edge_count >= 1u &&
                        cls_r.width > 40.f && cls_r.height > 40.f &&
                        cls_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "Animal");

    const uint32_t er_svg = inlay_diagram_export_svg(er_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool er_ok = er_r.valid && er_r.node_count >= 3u && er_r.edge_count >= 2u &&
                       er_r.width > 40.f && er_r.height > 40.f &&
                       er_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "CUSTOMER");

    const uint32_t pie_svg = inlay_diagram_export_svg(pie_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool pie_ok = pie_r.valid && pie_r.width > 80.f && pie_r.height > 80.f &&
                        pie_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "<path") &&
                        std::strstr(svg_buf, "GPU");

    const uint32_t gantt_svg = inlay_diagram_export_svg(gantt_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool gantt_ok = gantt_r.valid && gantt_r.width > 120.f && gantt_r.height > 60.f &&
                          gantt_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "<rect") &&
                          std::strstr(svg_buf, "Etch");

    // Also render all 4 diagrams fenced inside InlayDoc on GPU path
    const auto fenced_md = ETCH_STR_LIT(
        "```mermaid\nclassDiagram\nclass A\nclass B\nA <|-- B\n```\n\n"
        "```mermaid\nerDiagram\nUSER ||--o{ SESSION : has\n```\n\n"
        "```mermaid\npie title Share\n\"Alpha\" : 70\n\"Beta\" : 30\n```\n\n"
        "```mermaid\ngantt\ntitle Track\nsection S1\nT1 :2026-01-01, 3d\nT2 :after T1, 2d\n```\n");
    i3_r3 &= inlay_doc_set_utf8(d5, fenced_md);
    etch_begin_frame(ctx, &fr5);
    const InlayDocResult rd5 = inlay_doc_view(d5, &v5);
    etch_end_frame(ctx);
    const bool doc_diag_ok = rd5.measured_height_px > 200.f && inlay_doc_block_count(d5) == 4u;

    i3_r3 &= cls_ok && er_ok && pie_ok && gantt_ok && doc_diag_ok;
    if (!i3_r3) {
     std::fprintf(stderr, "i3_r3 diag: flags=0x%x/0x%x cls=%d er=%d pie=%d gantt=%d doc_diag=%d\n",
                  seen_flags, want_flags, cls_ok, er_ok, pie_ok, gantt_ok, doc_diag_ok);
    }
    inlay_doc_destroy(d5);
   }
  }

  // Round 4: nested inline HTML, HTML <table>, RTL paragraphs, gitGraph/mindmap/timeline, diagram node click
  bool i3_r4 = true;
  {
   InlayDoc* d6 = inlay_doc_create(ctx, nullptr);
   i3_r4 &= d6 != nullptr;
   if (d6) {
    const auto r4_md = ETCH_STR_LIT(
        "Nested <b><i>bold-italic</i></b> and <a href=\"https://inlay.dev\"><kbd>Ctrl+K</kbd></a>.\n\n"
        "<table><tr><th>Metric</th><th>Value</th></tr><tr><td>FPS</td><td>120</td></tr></table>\n\n"
        "مرحبا بالعالم — native RTL paragraph.\n\n"
        "שלום עולם — Hebrew RTL paragraph.\n");
    i3_r4 &= inlay_doc_set_utf8(d6, r4_md);
    EtchFrameDesc fr6 = etch_frame_desc_default();
    fr6.viewport_w = 640; fr6.viewport_h = 520;
    InlayDocViewDesc v6 = inlay_doc_view_desc_default();
    v6.id = etch_id("i3-r4-doc");
    v6.width_px = 560.f;
    v6.height_px = 0.f;
    etch_begin_frame(ctx, &fr6);
    const InlayDocResult r6 = inlay_doc_view(d6, &v6);
    etch_end_frame(ctx);

    const InlayMeasure* m6 = nullptr;
    for (uint32_t vi = 0u; vi < d6->view_count; ++vi) {
     if (d6->views[vi].id == v6.id) { m6 = &d6->views[vi]; break; }
    }
    bool has_nested_bi = false, has_nested_link_kbd = false;
    if (m6) {
     for (uint32_t ti = 0u; ti < m6->token_count; ++ti) {
      const uint32_t fl = m6->tokens[ti].flags;
      if ((fl & (InlayInlineBold | InlayInlineEmphasis)) == (InlayInlineBold | InlayInlineEmphasis)) {
       has_nested_bi = true;
      }
      if ((fl & (InlayInlineLink | InlayInlineKbd)) == (InlayInlineLink | InlayInlineKbd) &&
          m6->tokens[ti].url_length == 17u &&
          m6->tokens[ti].url_offset + m6->tokens[ti].url_length <= d6->source_size &&
          std::memcmp(d6->source + m6->tokens[ti].url_offset, "https://inlay.dev", 17u) == 0) {
       has_nested_link_kbd = true;
      }
     }
    }
    bool has_table_blk = false;
    for (uint32_t bi = 0u; bi < inlay_doc_block_count(d6); ++bi) {
     const InlayBlock* blk = inlay_doc_block(d6, bi);
     if (blk && blk->kind == INLAY_TABLE) has_table_blk = true;
    }
    char html_out[4096] = {};
    const uint32_t hlen = inlay_doc_export_html(d6, html_out, uint32_t(sizeof(html_out)));
    const bool html_tbl_ok = has_table_blk && hlen > 0u &&
                             std::strstr(html_out, "<table") != nullptr &&
                             std::strstr(html_out, "FPS") != nullptr &&
                             std::strstr(html_out, "120") != nullptr &&
                             r6.measured_height_px > 80.f;

    // Mermaid Round 4 diagram types: gitGraph, mindmap, timeline
    const InlayDiagramDesc dd = inlay_diagram_desc_default();
    char svg_buf[16384] = {};
    const auto git_src = ETCH_STR_LIT(
        "gitGraph\n"
        "commit id: \"init\"\n"
        "branch feat\n"
        "checkout feat\n"
        "commit id: \"ui\"\n"
        "checkout main\n"
        "merge feat\n");
    const auto mm_src = ETCH_STR_LIT(
        "mindmap\n"
        "  root((Orca))\n"
        "    Etch\n"
        "      Vulkan\n"
        "      D3D12\n"
        "    Inlay\n"
        "      TSX\n");
    const auto tl_src = ETCH_STR_LIT(
        "timeline\n"
        "  title Roadmap\n"
        "  section Foundation\n"
        "    R1 : Core\n"
        "    R2 : Bridge\n"
        "  section Polish\n"
        "    R4 : Convergence\n");

    etch_begin_frame(ctx, &fr6);
    const InlayDiagramResult git_r = inlay_diagram_layout(git_src, &dd);
    const InlayDiagramResult mm_r = inlay_diagram_layout(mm_src, &dd);
    const InlayDiagramResult tl_r = inlay_diagram_layout(tl_src, &dd);
    etch_end_frame(ctx);

    const uint32_t git_svg = inlay_diagram_export_svg(git_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool git_ok = git_r.valid && git_r.node_count >= 3u && git_r.edge_count >= 2u &&
                        git_r.width > 80.f && git_r.height > 40.f &&
                        git_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "init");

    const uint32_t mm_svg = inlay_diagram_export_svg(mm_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool mm_ok = mm_r.valid && mm_r.node_count >= 5u && mm_r.edge_count >= 4u &&
                       mm_r.width > 80.f && mm_r.height > 60.f &&
                       mm_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "Orca");

    const uint32_t tl_svg = inlay_diagram_export_svg(tl_src, &dd, svg_buf, uint32_t(sizeof(svg_buf)));
    const bool tl_ok = tl_r.valid && tl_r.node_count >= 3u &&
                       tl_r.width > 80.f && tl_r.height > 50.f &&
                       tl_svg > 0u && std::strstr(svg_buf, "<svg") && std::strstr(svg_buf, "Convergence");

    // Fenced rendering + cache hit verification + interactive node click hit-testing
    const auto diag_md = ETCH_STR_LIT(
        "```mermaid\nflowchart LR\nRootNode[ClickTarget] --> LeafNode[End]\n```\n\n"
        "```mermaid\ngitGraph\ncommit id: \"c1\"\nbranch dev\ncheckout dev\ncommit id: \"c2\"\ncheckout main\nmerge dev\n```\n\n"
        "```mermaid\nmindmap\n  root((Hub))\n    A\n    B\n```\n\n"
        "```mermaid\ntimeline\n  title Ship\n  2026 : Launch\n```\n");
    i3_r4 &= inlay_doc_set_utf8(d6, diag_md);
    InlayDocViewDesc v6d = inlay_doc_view_desc_default();
    v6d.id = etch_id("i3-r4-diags");
    v6d.width_px = 560.f;
    v6d.height_px = 0.f;
    etch_begin_frame(ctx, &fr6);
    const InlayDocResult rd6a = inlay_doc_view(d6, &v6d);
    etch_end_frame(ctx);
    uint64_t hits_before = 0u, misses_before = 0u;
    inlay_doc_diagram_cache_stats(d6, nullptr, &hits_before, &misses_before);
    etch_begin_frame(ctx, &fr6);
    const InlayDocResult rd6b = inlay_doc_view(d6, &v6d);
    etch_end_frame(ctx);
    uint64_t hits_after = 0u, misses_after = 0u;
    inlay_doc_diagram_cache_stats(d6, nullptr, &hits_after, &misses_after);
    const bool r4_cache_ok = rd6a.measured_height_px > 200.f && rd6b.measured_height_px > 200.f &&
                             hits_after > hits_before && misses_after == misses_before;

    // Click probe on the first diagram's RootNode (dbox is centered horizontally inside v6d)
    bool click_node_ok = false;
    EtchRect dbox_rect = {};
    for (uint32_t vi = 0u; vi < d6->view_count; ++vi) {
     if (d6->views[vi].id == v6d.id && d6->views[vi].vis_row_count > 0u) {
      dbox_rect = etch_element_bounds(etch_id_index(d6->views[vi].vis_row_ids[0], 7));
      break;
     }
    }
    if (dbox_rect.w > 20.f && dbox_rect.h > 10.f) {
     const EtchVec2 pt = { dbox_rect.x + 36.f, dbox_rect.y + dbox_rect.h * 0.45f };
     EtchFrameDesc fp = fr6;
     fp.pointer_pos = pt; fp.pointer_down = true;
     etch_begin_frame(ctx, &fp);
     (void)inlay_doc_view(d6, &v6d);
     etch_end_frame(ctx);
     EtchFrameDesc fr_rel = fr6;
     fr_rel.pointer_pos = pt; fr_rel.pointer_down = false;
     etch_begin_frame(ctx, &fr_rel);
     const InlayDocResult rc = inlay_doc_view(d6, &v6d);
     etch_end_frame(ctx);
     if (rc.clicked_diagram_node.chars && rc.clicked_diagram_node.length > 0u &&
         std::strstr(rc.clicked_diagram_node.chars, "RootNode") != nullptr) {
      click_node_ok = true;
     }
    }

    i3_r4 &= has_nested_bi && has_nested_link_kbd && html_tbl_ok &&
             git_ok && mm_ok && tl_ok && r4_cache_ok && click_node_ok;
    if (!i3_r4) {
     std::fprintf(stderr, "i3_r4 diag: bi=%d lk=%d tbl=%d git=%d mm=%d tl=%d cache=%d click=%d\n",
                  has_nested_bi, has_nested_link_kbd, html_tbl_ok,
                  git_ok, mm_ok, tl_ok, r4_cache_ok, click_node_ok);
    }
    inlay_doc_destroy(d6);
   }
  }

  i3 &= i3_cache && i3_a11y && i3_html && i3_dsvg && i3_msvg && i3_loader &&
        i3_fence && i3_outline_find_html && i3_mathml && i3_scroll_a11y && i3_hl_state && i3_r2 && i3_r3 && i3_r4;
  if(!i3)std::fprintf(stderr,"Track I3 failed: cache=%d a11y=%d html=%d dsvg=%d msvg=%d loader=%d fence=%d outline_find_html=%d mathml=%d scroll_a11y=%d hl_state=%d r2=%d r3=%d r4=%d\n",
                      i3_cache,i3_a11y,i3_html,i3_dsvg,i3_msvg,i3_loader,
                      i3_fence,i3_outline_find_html,i3_mathml,i3_scroll_a11y,i3_hl_state,i3_r2,i3_r3,i3_r4);
  ok &= i3;
 }

 inlay_doc_destroy(reference);inlay_doc_destroy(streamed);etch_destroy_context(ctx);
 ok &= flow_document(nullptr);
 std::printf("32KB streaming equivalence: %s\n",ok ? "PASS" : "FAIL");return ok ? 0 : 1;
}
