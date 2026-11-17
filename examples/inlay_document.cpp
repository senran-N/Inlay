#include <inlay/inlay_c.h>
#include <cstdio>
#include <cstring>
int main(int argc,char** argv) {
 const bool diagram=argc>1 && !std::strcmp(argv[1],"--diagram");
 const bool math=argc>1 && !std::strcmp(argv[1],"--math");
 const bool stress=argc>1 && !std::strcmp(argv[1],"--stress");
 if(argc<2 || (!stress && !math && !diagram && std::strcmp(argv[1],"--headless"))) {std::puts("Use --headless [output.png]");return 1;}
 auto config=etch_context_desc_default();config.headless=true;
 auto* ctx=etch_create_context(&config);if(!ctx)return 1;
 auto* doc=inlay_doc_create(ctx,nullptr);if(!doc) {etch_destroy_context(ctx);return 1;}
 bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT(
 "# Inlay / streamed documents\n\nA native document built on **Etch**, with `inline code` and [source links](https://example.org).\n\n"
 "> [!NOTE]\n> Unicode streams retain partial codepoints: 中文 / 😀\n\n"
 "Inline $x_1^2+x_2^2$ and \\(E=mc^2\\).\n\n$$\n\\frac{a+b}{\\sqrt{x}}\n$$\n\n```mermaid\nflowchart LR\nA[Input] --> B[Output]\n```\n\n"
 "## One surface, many blocks\n\n- [x] UTF-8 carry across chunks\n- [ ] Ship the full document milestone\n\n"
 "```cpp\nconst auto document = stream.next();\n\nint 变量 = 1;\nrender(document);\n```\n\n"
 "| Stage | Status |\n| --- | --- |\n| Streaming | Verified |\n| Rendering | In development |\n\n"
 "<details>\n<summary>Implementation notes</summary>\nContent remains native and selectable.\n</details>\n\n---\n\nNative text remains selectable.\n"));
 if(stress) {
  inlay_doc_clear(doc);
  ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("# Large document\n\n"));
  char long_line[102400];for(char& c:long_line)c='a';
  ok &= inlay_doc_append_utf8(doc,{long_line,sizeof(long_line),false});
  ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("\n\n```cpp\n"));
  for(int i=0;i<900;++i)ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("int 变量 = 1;\n"));
  ok &= inlay_doc_append_utf8(doc,ETCH_STR_LIT("```\n\n# Last block\n"));
 }
 uint32_t total_rows=0,last_end=0;
 const auto view=InlayDocViewDesc{etch_id("document"),860,700,16};
 for(int frame=0;frame<12;++frame) {
  auto desc=etch_frame_desc_default();desc.viewport_w=920;desc.viewport_h=760;etch_begin_frame(ctx,&desc);
  auto box=etch_box_desc_default();box.width=etch_sizing_fixed(920);box.height=etch_sizing_fixed(760);box.padding=etch_padding_all(30);box.background=etch_rgba8(12,18,28,255);
  etch_begin_box(&box);
  if(math) {
   auto md=inlay_math_desc_default();md.pos={24,36};md.font_size_px=32;
   const EtchStr samples[]={ETCH_STR_LIT("E=mc^2"),ETCH_STR_LIT("\\frac{a+b}{\\sqrt{x_1^2+x_2^2}}"),ETCH_STR_LIT("\\sum_{i=1}^n i=\\frac{n(n+1)}{2}"),ETCH_STR_LIT("\\begin{matrix}a&b\\\\c&d\\end{matrix}"),ETCH_STR_LIT("\\hat{x}+\\vec{v}+\\overline{AB}"),ETCH_STR_LIT("\\alpha+\\beta=\\gamma")};
   for(const auto sample:samples){const auto metrics=inlay_draw_math(sample,&md);ok &= metrics.valid;md.pos.y+=metrics.height+30;}
  }
  if(diagram){auto dd=inlay_diagram_desc_default();dd.pos={60,40};const auto dg=inlay_diagram_view(ETCH_STR_LIT("flowchart TD\nA[Stream] --> B[Parse]\nA --> C[Validate]\nB --> D[Render]\nC --> D\nD --> A"),&dd);ok &= dg.valid;}
  const auto result=(math || diagram) ? InlayDocResult{{},0,1,1} : inlay_doc_view(doc,&view);ok &= result.visible_end>result.visible_first;
  total_rows=result.total_rows;last_end=result.visible_end;etch_end_box();etch_end_frame(ctx);
  if(stress && frame==2)etch_virtual_list_scroll_to_row(view.id,total_rows-1,24,ETCH_ALIGN_Y_BOTTOM);
 }
 if(stress)ok &= total_rows>30 && last_end==total_rows && etch_virtual_list_scroll_offset(view.id)>0;
 ok &= etch_render_headless_png(ctx,920,760,argc>2 ? argv[2] : "inlay_document.png",etch_color_black(255));
 ok &= etch_frame_stats(ctx).draw_call_count<=3;
 inlay_doc_destroy(doc);etch_destroy_context(ctx);
 std::printf("Inlay headless document: %s\n",ok ? "PASS" : "FAIL");return ok ? 0 : 1;
}
