#include <inlay/inlay.h>
#include <cstdio>
#include "../src/inlay_internal.h"
#include <cmath>
#include <cstring>
int main() {
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
 const auto view=InlayDocViewDesc{etch_id("stream_view"),200,160,24};
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
 bool math_token=false;for(uint32_t i=0;i<streamed->views[0].token_count;++i)math_token |= streamed->views[0].tokens[i].kind==4;
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
 const auto sequence=inlay_diagram_layout(ETCH_STR_LIT("sequenceDiagram\nparticipant A as Client\nparticipant B as Server\nA->>B:Request\nB->>A:Reply"),&diagram_desc);
 ok &= sequence.valid && sequence.sequence && sequence.node_count==2 && sequence.edge_count==2;
 const auto self_graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA --> A"),&diagram_desc);
 ok &= self_graph.valid && self_graph.width>=self_graph.nodes[0].bounds.w+40;
 const auto horizontal=inlay_diagram_layout(ETCH_STR_LIT("flowchart LR\nA --> B --> C"),&diagram_desc);
 ok &= horizontal.valid && horizontal.horizontal && horizontal.nodes[0].bounds.x<horizontal.nodes[2].bounds.x;
 bool feedback=false;for(uint32_t i=0;i<graph.edge_count;++i)feedback |= graph.edges[i].reversed;
 ok &= feedback && !inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA -->"),&diagram_desc).valid;
 etch_end_frame(ctx);
 inlay_doc_destroy(reference);inlay_doc_destroy(streamed);etch_destroy_context(ctx);
 std::printf("32KB streaming equivalence: %s\n",ok ? "PASS" : "FAIL");return ok ? 0 : 1;
}
