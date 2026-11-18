// Integrated public-API consumer: no private Etch state or headers.
#include "cromma_aurora_dxil.h"
#include "cromma_aurora_msl.h"
#include "cromma_aurora_spv.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <etch/etch.h>
#include <inlay/inlay_c.h>
namespace {
using namespace etch;
struct Workbench {
  InlayDoc *document;
  InlaySurface *inspector;
  InlayPortal *portal;
  ShaderSurfaceId aurora;
  TerminalCell terminal[72 * 5];
  bool inspect = true;
};
void label(const char *s, float size = 14) {
  text(
      str_cstr(s),
      {.color = Color::rgb_hex(0x263334), .font_size_px = size, .wrap = false});
}
void build(Context *ctx, Workbench *app) {
  begin_box({.axis = Axis::Col,
             .width = Sizing::grow(),
             .height = Sizing::grow(),
             .padding = Padding::all(8),
             .gap = 8,
             .background = Color::rgb_hex(0xEDF2F0)});
  begin_box({.width = Sizing::grow(),
             .height = Sizing::fixed(40),
             .padding = Padding::xy(16, 4),
             .gap = 16,
             .align_y = AlignY::Center});
  label("C / Cromma", 20);
  label("Native agent workbench");
  box({.width = Sizing::grow()});
  if (button(
          {.id = id("inspector-toggle"), .label = str_lit("Toggle inspector")}))
    app->inspect = !app->inspect;
  label("Etch " ETCH_VERSION_STRING " / Inlay " INLAY_VERSION_STRING, 12);
  end_box();
  begin_box({.width = Sizing::grow(), .height = Sizing::grow(), .gap = 12});
  begin_box({.axis = Axis::Col,
             .width = Sizing::fixed(340),
             .height = Sizing::grow(),
             .padding = Padding::all(16),
             .gap = 12,
             .background = Color::white(),
             .radius = Corners::all(16)});
  label("Research / streamed response", 16);
  const InlayDocViewDesc doc_view = {id("research"), 308, 600, 15};
  inlay_doc_view(app->document, &doc_view);
  end_box();
  begin_box({.axis = Axis::Col,
             .width = Sizing::grow(),
             .height = Sizing::grow(),
             .gap = 12});
  begin_box({.axis = Axis::Col,
             .width = Sizing::grow(),
             .height = Sizing::fixed(430),
             .padding = Padding::all(16),
             .gap = 10,
             .background = Color::white(),
             .radius = Corners::all(16)});
  label("Execution canvas / portal at 65%", 16);
  begin_box({.id = id("canvas-well"),
             .width = Sizing::grow(),
             .height = Sizing::grow(),
             .radius = Corners::all(12),
             .clip_children = true});
  ShaderSurfaceEvalDesc eval = {.width = 900, .height = 380};
  eval.params[0].w = -1;
  if (auto *texture = eval_shader_surface(ctx, app->aurora, eval)) {
    const auto bounds = element_bounds(id("canvas-well"));
    draw_image({.texture = texture,
                .rect = {0, 0, bounds.w > 0 ? bounds.w : 700, 380}});
  }
  begin_canvas({.id = id("portal-canvas"),
                .width = Sizing::grow(),
                .height = Sizing::grow(),
                .default_pan = {120, 80},
                .default_zoom = .65f,
                .background = Color::rgba8(0, 0, 0, 0),
                .show_grid = false});
  begin_canvas_node({.id = id("portal-agent"),
                     .pos_value = {20, 30},
                     .width_world = Sizing::fixed(480),
                     .height_world = Sizing::fixed(300)});
  begin_box(
      {.axis = Axis::Col,
       .width = Sizing::fixed(480),
       .height = Sizing::fixed(300),
       .padding = Padding::all(12),
       .gap = 12,
       .background = Color::rgb_hex(0xEDF2F0),
       .radius = Corners::all(12),
       .border_beam = {.color = Color::oklch(.7f, .13f, 160), .glow_px = 3}});
  label("Browser tool / synthetic interactive view", 18);
  const InlayPortalViewDesc portal_view = {id("browser-portal"), 456, 240, 8};
  inlay_portal_view(app->portal, &portal_view);
  end_box();
  end_canvas_node();
  end_canvas();
  end_box();
  end_box();
  begin_box({.axis = Axis::Col,
             .width = Sizing::grow(),
             .height = Sizing::grow(),
             .padding = Padding::all(16),
             .gap = 12,
             .background = Color::white(),
             .radius = Corners::all(16)});
  label("Terminal / event trace", 16);
  terminal_grid({.id = id("event-terminal"),
                 .cells = app->terminal,
                 .columns = 72,
                 .rows = 5,
                 .cell_width_px = 8,
                 .line_height_px = 24,
                 .font_size_px = 13,
                 .foreground = Color::rgb_hex(0xD9E4E0),
                 .background = Color::rgb_hex(0x17201D)});
  end_box();
  end_box();
  if (app->inspect) {
    begin_box({.axis = Axis::Col,
               .width = Sizing::fixed(232),
               .height = Sizing::grow(),
               .padding = Padding::all(16),
               .gap = 16,
               .background = Color::white(),
               .radius = Corners::all(16)});
    label("Sandbox inspector", 16);
    if (app->inspector) {
      const InlaySurfaceViewDesc surface_view = {id("sandbox-inspector"), 200,
                                                 400};
      inlay_surface_view(app->inspector, &surface_view);
    } else
      label("Scripting disabled");
    label("Portal input", 14);
    label("Click the blue tile", 12);
    label("Type to move the caret", 12);
    label("Drag canvas to pan", 12);
    end_box();
  }
  end_box();
  end_box();
}
} // namespace
int main(int argc, char **argv) {
  const bool headless = argc > 1 && !std::strcmp(argv[1], "--headless");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    return 1;
  auto *window = headless ? nullptr
                          : SDL_CreateWindow("Cromma workbench", 1440, 800,
                                             SDL_WINDOW_RESIZABLE |
                                                 SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!headless && !window) {
    SDL_Quit();
    return 1;
  }
  auto *ctx = create_context({.window = window, .headless = headless});
  if (!ctx) {
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  set_theme(Theme::from_seed(Color::oklch(.6f, .12f, 160), false), ctx);
  auto *c = reinterpret_cast<EtchContext *>(ctx);
  Workbench app = {};
  app.document = inlay_doc_create(c, nullptr);
  app.inspector = inlay_surface_create(c, nullptr);
  auto portal_desc = inlay_portal_desc_default();
  portal_desc.width = 456;
  portal_desc.height = 240;
  app.portal = inlay_portal_create(c, &portal_desc);
  app.aurora = create_shader_surface(
      ctx, {.spirv = {g_cromma_aurora_spv, sizeof(g_cromma_aurora_spv)},
            .dxil = {g_cromma_aurora_dxil, sizeof(g_cromma_aurora_dxil)},
            .msl = {g_cromma_aurora_msl, sizeof(g_cromma_aurora_msl)},
            .continuous_time = false});
  bool ok = app.document && app.portal && app.aurora;
  if (app.document) {
    const char *chunks[] = {
        "# A native research loop\n\n",
        "**Plan with `evidence` and [sources](https://example.org)**, then "
        "inspect the *live* tool. ~~Manual refresh~~\n\n",
        "| Evidence | Status |\n| --- | --- |\n| Search \\| verify | **Ready** |\n\n",
        "```mermaid\nflowchart TD\nsubgraph Research\nA[Plan] -->|query| B[Browser]\n"
        "B -- evidence --> C[Inspect]\nC -. refine .-> A\nend\n```\n\n",
        "## Confidence model\n\n$$\\frac{a+b}{\\sqrt{x_1^2+x_2^2}}$$\n\n",
        "- [x] Native math and diagrams\n- [x] Sandboxed inspector\n- [x] "
        "Zoomed portal\n\n"};
    for (const auto *chunk : chunks)
      ok &= inlay_doc_append_utf8(
          app.document,
          {chunk, static_cast<uint32_t>(std::strlen(chunk)), false});
  }
  if (app.inspector)
    ok &= inlay_surface_set_script(
        app.inspector,
        ETCH_STR_LIT(
            "function render(s){return "
            "h('box',{axis:'column',width:-1,gap:12},h('text',{text:'Runtime: QuickJS-NG'}),"
            "h('input',{value:s.query||'',placeholder:'Filter...',onChange:'setQuery',width:-1}),"
            "h('toggle',{label:'Live',checked:!!s.live,onChange:'setLive'}),"
            "h('text',{color:'var(--etch-text-muted)',text:s.query||'All evidence'}),"
            "h('text',{text:'Tick budget: 4 ms'}),h('text',{text:'Idle: cached native tree'}))}"
            "function onEvent(name,payload,s){if(name==='setLive')s.live=payload;"
            "if(name==='setQuery')s.query=payload;}"));
  const char *lines[] = {"$ cromma open native-workbench",
                         "[doc] streaming UTF-8 / math / mermaid",
                         "[ui] sandbox inspector mounted",
                         "[portal] 456 x 240 / mock backend",
                         "[host] waiting for input"};
  for (uint32_t row = 0; row < 5; ++row)
    for (uint32_t col = 0; col < 72; ++col) {
      auto &cell = app.terminal[row * 72 + col];
      cell = {};
      cell.codepoint =
          col < std::strlen(lines[row]) ? uint32_t(lines[row][col]) : ' ';
    }
  bool running = ok;
  uint32_t frames = 0;
  while (running) {
    if (!headless) {
      const float input_scale =
          SDL_GetWindowPixelDensity(window) / SDL_GetWindowDisplayScale(window);
      SDL_Event event;
      if (wait_events(ctx, &event))
        do {
          process_event(ctx, event);
          if (event.type == SDL_EVENT_QUIT ||
              event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            running = false;
          if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
              event.button.button == SDL_BUTTON_LEFT)
            inlay_portal_inject_pointer(
                app.portal,
                {event.button.x * input_scale, event.button.y * input_scale},
                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
          if (event.type == SDL_EVENT_MOUSE_MOTION)
            inlay_portal_inject_pointer(
                app.portal,
                {event.motion.x * input_scale, event.motion.y * input_scale},
                (event.motion.state & SDL_BUTTON_LMASK) != 0);
          if (event.type == SDL_EVENT_TEXT_INPUT)
            inlay_portal_inject_text(
                app.portal,
                {event.text.text,
                 static_cast<uint32_t>(std::strlen(event.text.text)), false});
          if (event.type == SDL_EVENT_KEY_DOWN ||
              event.type == SDL_EVENT_KEY_UP)
            inlay_portal_inject_key(app.portal, event.key.key, event.key.down,
                                    event.key.mod);
          if (event.type == SDL_EVENT_MOUSE_WHEEL)
            inlay_portal_inject_wheel(app.portal,
                                      {event.wheel.x, event.wheel.y});
        } while (SDL_PollEvent(&event));
    }
    if (!running)
      break;
    ok &= inlay_portal_poll(app.portal);
    if (headless)
      begin_frame(ctx, {.viewport_w = 1440, .viewport_h = 800});
    else
      begin_window_frame(ctx, window, 1.f / 60);
    build(ctx, &app);
    if (headless)
      end_frame(ctx);
    else
      end_window_frame(ctx);
    const bool portal_visible = inlay_portal_sync_geometry(app.portal);
    inlay_portal_set_input_enabled(app.portal, portal_visible);
    if (!headless) {
      EtchRect caret = {};
      if (inlay_portal_has_focus(app.portal) &&
          inlay_portal_query_ime_rect(app.portal, &caret)) {
        const float caret_scale = SDL_GetWindowDisplayScale(window) /
                                  SDL_GetWindowPixelDensity(window);
        const SDL_Rect area = {
            int(caret.x * caret_scale), int(caret.y * caret_scale),
            int(caret.w * caret_scale), int(caret.h * caret_scale)};
        SDL_SetTextInputArea(window, &area, 0);
        SDL_StartTextInput(window);
      } else
        SDL_StopTextInput(window);
      ok &= render_window(ctx);
    }
    if (headless && ++frames == 12) {
      ok &= render_headless_png(
          ctx, 1440, 800, argc > 2 ? argv[2] : "build/cromma-workbench.png");
      running = false;
    }
    if (!headless || !running)
      inlay_portal_frame_submitted(app.portal);
  }
  if (headless)
    std::printf("Cromma integration: %s\n", ok ? "PASS" : "FAIL");
  inlay_portal_frame_submitted(app.portal);
  inlay_portal_destroy(app.portal);
  inlay_surface_destroy(app.inspector);
  inlay_doc_destroy(app.document);
  destroy_context(ctx);
  if (window)
    SDL_DestroyWindow(window);
  SDL_Quit();
  return ok ? 0 : 1;
}
