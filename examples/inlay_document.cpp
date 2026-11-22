#include <inlay/inlay_c.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  bool headless = false;
  bool diagram = false;
  bool math = false;
  bool stress = false;
  bool light = false;
  const char *out_png = "build/inlay_document.png";
  const char *gpu_driver = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--headless")) {
      headless = true;
    } else if (!std::strcmp(argv[i], "--diagram")) {
      diagram = true;
    } else if (!std::strcmp(argv[i], "--math")) {
      math = true;
    } else if (!std::strcmp(argv[i], "--stress")) {
      stress = true;
    } else if (!std::strcmp(argv[i], "--light")) {
      light = true;
    } else if (!std::strcmp(argv[i], "--dark")) {
      light = false;
    } else if (!std::strcmp(argv[i], "--renderer") && (i + 1) < argc) {
      gpu_driver = argv[++i];
      if (!std::strcmp(gpu_driver, "d3d12"))
        gpu_driver = "direct3d12";
    } else if (argv[i][0] != '-') {
      out_png = argv[i];
    }
  }

  if (!headless && !stress && !math && !diagram) {
    std::puts("Use --headless [--light] [--stress|--math|--diagram] [output.png]");
    return 1;
  }

  auto config = etch_context_desc_default();
  config.headless = true;
  config.gpu_driver = gpu_driver;
  auto *ctx = etch_create_context(&config);
  if (!ctx)
    return 1;

  const EtchTheme active_theme = light ? etch_theme_light() : etch_theme_dark();
  etch_set_theme(&active_theme, ctx);
  const EtchColor canvas_bg = etch_get_theme(ctx).bg_canvas;

  auto *doc = inlay_doc_create(ctx, nullptr);
  if (!doc) {
    etch_destroy_context(ctx);
    return 1;
  }

  bool ok = inlay_doc_set_utf8(
      doc,
      ETCH_STR_LIT(
          "# Inlay / streamed documents\n\nA native document built on **Etch**, "
          "with `inline code` and [source links](https://example.org).\n\n"
          "> [!NOTE]\n> Unicode streams retain partial codepoints: 中文 / 😀\n\n"
          "Inline $x_1^2+x_2^2$ and \\(E=mc^2\\).\n\n$$\n\\frac{a+b}{\\sqrt{x}}\n$$\n\n"
          "```mermaid\nflowchart LR\nA[Input] --> B[Output]\n```\n\n"
          "## One surface, many blocks\n\n- [x] UTF-8 carry across chunks\n"
          "- [ ] Ship the full document milestone\n\n"
          "```cpp\nconst auto document = stream.next();\n\nint 变量 = 1;\nrender(document);\n```\n\n"
          "| Stage | Status |\n| --- | --- |\n| Streaming | Verified |\n"
          "| Rendering | In development |\n\n"
          "<details>\n<summary>Implementation notes</summary>\n"
          "Content remains native and selectable.\n</details>\n\n---\n\n"
          "Native text remains selectable.\n"));

  if (stress) {
    inlay_doc_clear(doc);
    ok &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("# Large document\n\n"));
    char long_line[102400];
    for (char &c : long_line)
      c = 'a';
    ok &= inlay_doc_append_utf8(doc, {long_line, sizeof(long_line), false});
    ok &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("\n\n```cpp\n"));
    for (int i = 0; i < 900; ++i)
      ok &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("int 变量 = 1;\n"));
    ok &= inlay_doc_append_utf8(doc, ETCH_STR_LIT("```\n\n# Last block\n"));
  }

  uint32_t total_rows = 0, last_end = 0;
  auto view = inlay_doc_view_desc_default();
  view.id = etch_id("document");
  view.width_px = 860.f;
  view.height_px = 700.f;
  view.font_size_px = 16.f;

  for (int frame = 0; frame < 12; ++frame) {
    auto desc = etch_frame_desc_default();
    desc.viewport_w = 920u;
    desc.viewport_h = 760u;
    etch_begin_frame(ctx, &desc);

    auto box = etch_box_desc_default();
    box.width = etch_sizing_fixed(920.f);
    box.height = etch_sizing_fixed(760.f);
    box.padding = etch_padding_all(30.f);
    box.background = canvas_bg;
    etch_begin_box(&box);

    if (math) {
      auto md = inlay_math_desc_default();
      md.pos = {24.f, 36.f};
      md.font_size_px = 32.f;
      const EtchStr samples[] = {
          ETCH_STR_LIT("E=mc^2"),
          ETCH_STR_LIT("\\frac{a+b}{\\sqrt{x_1^2+x_2^2}}"),
          ETCH_STR_LIT("\\sum_{i=1}^n i=\\frac{n(n+1)}{2}"),
          ETCH_STR_LIT("\\begin{matrix}a&b\\\\c&d\\end{matrix}"),
          ETCH_STR_LIT("\\hat{x}+\\vec{v}+\\overline{AB}"),
          ETCH_STR_LIT("\\alpha+\\beta=\\gamma"),
      };
      for (const auto sample : samples) {
        const auto metrics = inlay_draw_math(sample, &md);
        ok &= metrics.valid;
        md.pos.y += metrics.height + 30.f;
      }
    }
    if (diagram) {
      auto dd = inlay_diagram_desc_default();
      dd.pos = {60.f, 40.f};
      const auto dg = inlay_diagram_view(
          ETCH_STR_LIT("flowchart TD\nA[Stream] --> B[Parse]\nA --> C[Validate]\n"
                       "B --> D[Render]\nC --> D\nD --> A"),
          &dd);
      ok &= dg.valid;
    }

    InlayDocResult result = {};
    if (math || diagram) {
      result.visible_end = 1u;
      result.total_rows = 1u;
    } else {
      result = inlay_doc_view(doc, &view);
    }
    ok &= result.visible_end > result.visible_first;
    total_rows = result.total_rows;
    last_end = result.visible_end;

    etch_end_box();
    etch_end_frame(ctx);
    if (stress && frame == 2)
      etch_virtual_list_scroll_to_row(view.id, total_rows - 1u, 24.f,
                                      ETCH_ALIGN_Y_BOTTOM);
  }

  if (stress)
    ok &= total_rows > 30u && last_end == total_rows &&
          etch_virtual_list_scroll_offset(view.id) > 0.0;
  ok &= etch_render_headless_png(ctx, 920u, 760u, out_png, canvas_bg);
  ok &= etch_frame_stats(ctx).draw_call_count > 0u &&
        etch_frame_stats(ctx).draw_call_count <= 32u;

  inlay_doc_destroy(doc);
  etch_destroy_context(ctx);
  std::printf("Inlay headless document: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
