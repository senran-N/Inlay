// Integrated public-API consumer: flagship Cromma native agent workbench.
#include "cromma_aurora_dxil.h"
#include "cromma_aurora_msl.h"
#include "cromma_aurora_spv.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <inlay/inlay.h>

namespace {
using namespace etch;

constexpr uint32_t kTermCols = 76u;
constexpr uint32_t kTermRows = 6u;

struct Workbench {
  Context *ctx = nullptr;
  InlayDoc *document = nullptr;
  InlaySurface *inspector = nullptr;
  InlayPortal *portal = nullptr;
  ShaderSurfaceId aurora = 0u;
  TerminalCell terminal[kTermCols * kTermRows] = {};
  bool inspect = true;
  bool light_mode = false;
};

void label(const char *s, float size, Color col,
           uint16_t font = kFontSans) noexcept {
  text(str_cstr(s),
       {.color = col, .font_size_px = size, .font_id = font, .wrap = false});
}

void write_term_seg(TerminalCell *row_cells, uint32_t &col, const char *utf8,
                    uint32_t rgb, bool bold = false) noexcept {
  const auto *p = reinterpret_cast<const unsigned char *>(utf8);
  while (*p && col < kTermCols) {
    uint32_t cp = *p++;
    if ((cp & 0xE0u) == 0xC0u && p[0]) {
      cp = ((cp & 0x1Fu) << 6) | (uint32_t(*p++) & 0x3Fu);
    } else if ((cp & 0xF0u) == 0xE0u && p[0] && p[1]) {
      cp = ((cp & 0x0Fu) << 12) | ((uint32_t(p[0]) & 0x3Fu) << 6) |
           (uint32_t(p[1]) & 0x3Fu);
      p += 2;
    } else if ((cp & 0xF8u) == 0xF0u && p[0] && p[1] && p[2]) {
      cp = ((cp & 0x07u) << 18) | ((uint32_t(p[0]) & 0x3Fu) << 12) |
           ((uint32_t(p[1]) & 0x3Fu) << 6) | (uint32_t(p[2]) & 0x3Fu);
      p += 3;
    }
    auto &cell = row_cells[col++];
    cell = {};
    cell.codepoint = cp;
    cell.foreground = {TerminalColorMode::TrueColor, rgb};
    cell.bold = bold;
  }
}

void populate_terminal(Workbench *app) noexcept {
  for (uint32_t i = 0; i < kTermCols * kTermRows; ++i) {
    app->terminal[i] = {};
    app->terminal[i].codepoint = ' ';
    app->terminal[i].foreground = {TerminalColorMode::TrueColor, 0xCBD5E1u};
  }
  const uint32_t c_emerald = 0x34D399u;
  const uint32_t c_sky = 0x7DD3FCu;
  const uint32_t c_cyan = 0x38BDF8u;
  const uint32_t c_indigo = 0x818CF8u;
  const uint32_t c_violet = 0xA78BFAu;
  const uint32_t c_teal = 0x2DD4BFu;
  const uint32_t c_amber = 0xFBBF24u;
  const uint32_t c_body = 0xCBD5E1u;
  const uint32_t c_muted = 0x64748Bu;

  {
    uint32_t col = 0;
    auto *r = app->terminal + 0u * kTermCols;
    write_term_seg(r, col, "$ ", c_emerald, true);
    write_term_seg(r, col, "cromma open native-workbench --vibrancy --gpu",
                   c_sky);
  }
  {
    uint32_t col = 0;
    auto *r = app->terminal + 1u * kTermCols;
    write_term_seg(r, col, "[doc]    ", c_cyan, true);
    write_term_seg(r, col,
                   "md4c streaming UTF-8 · TeX math · Mermaid subgraph   ",
                   c_body);
    write_term_seg(r, col, "READY", c_emerald, true);
  }
  {
    uint32_t col = 0;
    auto *r = app->terminal + 2u * kTermCols;
    write_term_seg(r, col, "[portal] ", c_indigo, true);
    write_term_seg(r, col,
                   "456x240 mock compositor · frosted node @ 65% zoom    ",
                   c_body);
    write_term_seg(r, col, "SYNCED", c_emerald, true);
  }
  {
    uint32_t col = 0;
    auto *r = app->terminal + 3u * kTermCols;
    write_term_seg(r, col, "[ui]     ", c_violet, true);
    write_term_seg(r, col,
                   "QuickJS-NG v" INLAY_VERSION_STRING " JSX + cls · table + ring + slot   ",
                   c_body);
    write_term_seg(r, col, "0 IDLE", c_amber, true);
  }
  {
    uint32_t col = 0;
    auto *r = app->terminal + 4u * kTermCols;
    write_term_seg(r, col, "[gpu]    ", c_teal, true);
    write_term_seg(r, col,
                   "Etch micro-shaders · Oklab stops · R8 stem-darkened  ",
                   c_body);
    write_term_seg(r, col, "OK", c_emerald, true);
  }
  {
    uint32_t col = 0;
    auto *r = app->terminal + 5u * kTermCols;
    write_term_seg(r, col, "> ", c_emerald, true);
    write_term_seg(r, col, "awaiting operator input...", c_muted);
  }
}

void render_telemetry_slot(InlaySurface *surface, uint32_t, float, float,
                           EtchStr, void *user_data) {
  auto *app = static_cast<Workbench *>(user_data);
  const auto stats = inlay_surface_stats(surface);
  const Theme &t = theme(app->ctx);
  const bool light = (t.id == ThemeId::Light);
  const Color emerald =
      light ? Color::rgb_hex(0x059669) : Color::oklch(0.82f, 0.14f, 165.0f);
  const Color tile_bg =
      light ? Color::rgb_hex(0xF1F5F9) : Color::rgb_hex(0x10131A);
  const Color tile_border =
      light ? Color::rgba8(15, 23, 42, 22) : Color::rgba8(255, 255, 255, 18);

  char nodes_buf[24] = {};
  char ticks_buf[24] = {};
  char ms_buf[24] = {};
  std::snprintf(nodes_buf, sizeof(nodes_buf), "%u", stats.node_count);
  std::snprintf(ticks_buf, sizeof(ticks_buf), "%llu",
                static_cast<unsigned long long>(stats.js_ticks));
  const double ms =
      stats.last_reload_ms > 0.0 ? stats.last_reload_ms : stats.last_tick_ms;
  std::snprintf(ms_buf, sizeof(ms_buf), "%.2fms", ms);

  Arena *arena = frame_arena(app->ctx);
  const Str vals[3] = {
      arena ? arena->push_str(nodes_buf, std::strlen(nodes_buf))
            : str_cstr(nodes_buf),
      arena ? arena->push_str(ticks_buf, std::strlen(ticks_buf))
            : str_cstr(ticks_buf),
      arena ? arena->push_str(ms_buf, std::strlen(ms_buf)) : str_cstr(ms_buf),
  };
  const char *keys[3] = {"NODES", "TICKS", "RELOAD"};
  const Color cols[3] = {t.text_primary, emerald, t.accent};

  begin_box({.axis = Axis::Row,
             .width = Sizing::grow(),
             .height = Sizing::grow(),
             .gap = 6.0f});
  for (uint32_t i = 0; i < 3u; ++i) {
    begin_box({
        .axis = Axis::Col,
        .width = Sizing::grow(),
        .height = Sizing::grow(),
        .padding = Padding::xy(4.0f, 6.0f),
        .gap = 4.0f,
        .align_x = AlignX::Center,
        .align_y = AlignY::Center,
        .background = tile_bg,
        .border_color = tile_border,
        .border_width = 1.0f,
        .radius = Corners::all(8.0f),
        .continuous_corners = true,
    });
    label(keys[i], 9.0f, t.text_muted, kFontMono);
    text(vals[i], {.color = cols[i],
                   .font_size_px = 12.0f,
                   .font_id = kFontBold,
                   .wrap = false});
    end_box();
  }
  end_box();
}

void build(Context *ctx, Workbench *app) noexcept {
  const Theme &t = theme(ctx);
  const bool light = (t.id == ThemeId::Light);
  const Color emerald =
      light ? Color::rgb_hex(0x059669) : Color::oklch(0.82f, 0.14f, 165.0f);
  const Color cyan =
      light ? Color::rgb_hex(0x0284C7) : Color::oklch(0.80f, 0.13f, 215.0f);
  const Color indigo =
      light ? Color::rgb_hex(0x4F46E5) : Color::oklch(0.76f, 0.16f, 275.0f);
  const Color card_border =
      light ? Color::rgba8(15, 23, 42, 24) : t.border_subtle;
  const float card_rim = light ? 0.0f : 0.16f;

  const GradientDesc brand_pill_grad = {
      .kind = GradientKind::Linear,
      .oklab = true,
      .dir = {1.0f, 0.25f},
      .stop_count = 4u,
      .stops =
          {
              {Color::rgb_hex(0x6366F1), 0.00f},
              {Color::rgb_hex(0x3B82F6), 0.34f},
              {Color::rgb_hex(0x06B6D4), 0.68f},
              {Color::rgb_hex(0x10B981), 1.00f},
          },
  };
  const GradientDesc accent_bar_grad = {
      .kind = GradientKind::Linear,
      .oklab = true,
      .dir = {1.0f, 0.0f},
      .stop_count = 4u,
      .stops =
          {
              {Color::rgb_hex(0x6366F1), 0.00f},
              {Color::rgb_hex(0x0EA5E9), 0.35f},
              {Color::rgb_hex(0x10B981), 0.72f},
              {Color::rgb_hex(0xF59E0B), 1.00f},
          },
  };

  begin_box({
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .padding = Padding::xy(18.0f, 12.0f),
      .gap = 10.0f,
      .background = t.bg_canvas,
  });

  // Top Chrome Bar (44px)
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::fixed(44.0f),
      .padding = Padding::xy(2.0f, 0.0f),
      .gap = 12.0f,
      .align_y = AlignY::Center,
  });

  begin_box({
      .padding = Padding::xy(14.0f, 6.0f),
      .align_x = AlignX::Center,
      .align_y = AlignY::Center,
      .radius = Corners::all(999.0f),
      .elevation = Elevation::Sm,
      .gradient = brand_pill_grad,
  });
  label("C / Cromma", 14.5f, Color::white(), kFontBold);
  end_box();

  label("Native Agent Workbench", 13.5f, t.text_muted, kFontSans);
  box({.width = Sizing::grow()});

  if (button({
          .id = id("theme-toggle"),
          .label = light ? str_lit("Dark") : str_lit("Light"),
          .variant = ButtonVariant::Default,
          .height = Sizing::fixed(30.0f),
          .padding = Padding::xy(12.0f, 0.0f),
          .font_size_px = 12.0f,
          .radius = Corners::all(999.0f),
      })) {
    app->light_mode = !app->light_mode;
    set_theme(app->light_mode ? Theme::light() : Theme::dark(), ctx);
  }

  if (button({
          .id = id("inspector-toggle"),
          .label =
              app->inspect ? str_lit("Hide Inspector") : str_lit("Inspector"),
          .variant = ButtonVariant::Default,
          .height = Sizing::fixed(30.0f),
          .padding = Padding::xy(12.0f, 0.0f),
          .font_size_px = 12.0f,
          .radius = Corners::all(999.0f),
      })) {
    app->inspect = !app->inspect;
  }

  begin_box({
      .padding = Padding::xy(11.0f, 5.0f),
      .align_x = AlignX::Center,
      .align_y = AlignY::Center,
      .background = t.bg_surface,
      .border_color = card_border,
      .border_width = 1.0f,
      .radius = Corners::all(999.0f),
  });
  label("Etch " ETCH_VERSION_STRING " · Inlay " INLAY_VERSION_STRING, 11.0f,
        t.text_muted, kFontMono);
  end_box();
  end_box();

  // 2px Oklab multi-stop accent hairline
  box({
      .width = Sizing::grow(),
      .height = Sizing::fixed(2.0f),
      .radius = Corners::all(1.0f),
      .gradient = accent_bar_grad,
  });

  // Main 3-Column Workspace
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .gap = 14.0f,
  });

  // Left Column: Research · Streamed Document (356px)
  begin_box({
      .axis = Axis::Col,
      .width = Sizing::fixed(356.0f),
      .height = Sizing::grow(),
      .padding = Padding::all(14.0f),
      .gap = 10.0f,
      .background = t.bg_surface,
      .border_color = card_border,
      .border_width = 1.0f,
      .radius = Corners::all(16.0f),
      .continuous_corners = true,
      .top_rim_alpha = card_rim,
      .elevation = Elevation::Md,
  });
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::fixed(22.0f),
      .gap = 8.0f,
      .align_y = AlignY::Center,
  });
  box({.width = Sizing::fixed(7.0f),
       .height = Sizing::fixed(7.0f),
       .background = emerald,
       .radius = Corners::all(4.0f)});
  label("Research · Streamed Document", 13.0f, t.text_primary, kFontBold);
  box({.width = Sizing::grow()});
  label("STREAMING", 9.5f, emerald, kFontMono);
  end_box();

  begin_box({
      .id = id("research-scroll"),
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .clip_children = true,
      .scroll_y = true,
      .show_scrollbar = true,
  });
  auto doc_view = inlay_doc_view_desc_default();
  doc_view.id = id("research");
  doc_view.width_px = 322.0f;
  doc_view.height_px = 0.0f;
  doc_view.font_size_px = 13.0f;
  inlay_doc_view(app->document, &doc_view);
  end_box();
  end_box();

  // Center Column (Grow)
  begin_box({
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .gap = 12.0f,
  });

  // Center Top Card: Execution Canvas · Portal @ 65% (450px)
  begin_box({
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::fixed(450.0f),
      .padding = Padding::all(16.0f),
      .gap = 10.0f,
      .background = t.bg_surface,
      .border_color = card_border,
      .border_width = 1.0f,
      .radius = Corners::all(16.0f),
      .continuous_corners = true,
      .top_rim_alpha = card_rim,
      .elevation = Elevation::Md,
  });
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::fixed(22.0f),
      .gap = 8.0f,
      .align_y = AlignY::Center,
  });
  box({.width = Sizing::fixed(7.0f),
       .height = Sizing::fixed(7.0f),
       .background = cyan,
       .radius = Corners::all(4.0f)});
  label("Execution Canvas · Portal @ 65%", 13.0f, t.text_primary, kFontBold);
  box({.width = Sizing::grow()});
  label("AURORA SHADER · PAN/ZOOM", 9.5f, t.text_muted, kFontMono);
  end_box();

  begin_box({
      .id = id("canvas-well"),
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .background = Color::rgb_hex(0x0B1320),
      .border_color = card_border,
      .border_width = 1.0f,
      .radius = Corners::all(12.0f),
      .clip_children = true,
      .continuous_corners = true,
  });
  const Rect well_bounds = element_bounds(id("canvas-well"));
  ShaderSurfaceEvalDesc eval = {.width = 900u, .height = 386u};
  eval.params[0].w = -1.0f;
  if (auto *texture = eval_shader_surface(ctx, app->aurora, eval)) {
    draw_image({.texture = texture,
                .rect = {0.0f, 0.0f,
                         well_bounds.w > 0.0f ? well_bounds.w : 688.0f,
                         well_bounds.h > 0.0f ? well_bounds.h : 386.0f}});
  }
  begin_canvas({
      .id = id("portal-canvas"),
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .default_pan = {152.0f, 81.0f},
      .default_zoom = 0.65f,
      .background = Color::transparent(),
      .border_color = Color::transparent(),
      .border_width = 0.0f,
      .show_grid = false,
  });
  begin_canvas_node({
      .id = id("portal-agent"),
      .pos_value = {20.0f, 20.0f},
      .width_world = Sizing::fixed(484.0f),
      .height_world = Sizing::fixed(304.0f),
      .background = Color::transparent(),
      .border_color = Color::transparent(),
      .border_width_world = 0.0f,
      .shadow_color = Color::transparent(),
      .shadow_blur_world = 0.0f,
  });
  begin_box({
      .axis = Axis::Col,
      .width = Sizing::fixed(484.0f),
      .height = Sizing::fixed(304.0f),
      .padding = Padding::all(14.0f),
      .gap = 10.0f,
      .background = light ? Color::rgba8(245, 250, 255, 204)
                          : Color::rgba8(14, 20, 32, 168),
      .radius = Corners::all(16.0f),
      .backdrop_blur = 16.0f,
      .continuous_corners = true,
      .border_beam =
          {
              .color = Color::oklch(0.76f, 0.14f, 175.0f),
              .base_color = light ? Color::rgba8(255, 255, 255, 195)
                                  : Color::rgba8(120, 175, 220, 92),
              .width = 1.25f,
              .angle_rad = 0.7f,
              .glow_px = 5.0f,
          },
      .top_rim_alpha = light ? 0.42f : 0.30f,
      .elevation = Elevation::Lg,
      .backdrop_saturation = 1.25f,
      .backdrop_brightness = light ? 1.05f : 1.04f,
      .backdrop_noise = 0.015f,
  });
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::fixed(24.0f),
      .gap = 8.0f,
      .align_y = AlignY::Center,
  });
  label("Browser Tool · Synthetic Portal", 15.0f,
        light ? Color::rgb_hex(0x0F172A) : Color::white(), kFontBold);
  box({.width = Sizing::grow()});
  begin_box({
      .padding = Padding::xy(9.0f, 3.0f),
      .align_x = AlignX::Center,
      .align_y = AlignY::Center,
      .background = light ? Color::rgba8(2, 132, 199, 26)
                          : Color::rgba8(56, 189, 248, 30),
      .border_color = light ? Color::rgba8(2, 132, 199, 96)
                            : Color::rgba8(56, 189, 248, 96),
      .border_width = 1.0f,
      .radius = Corners::all(999.0f),
  });
  label("65% ZOOM", 10.5f,
        light ? Color::rgb_hex(0x0369A1) : Color::rgb_hex(0x7DD3FC), kFontMono);
  end_box();
  end_box();

  InlayPortalViewDesc portal_view = inlay_portal_view_desc_default();
  portal_view.id = id("browser-portal");
  portal_view.width = 456.0f;
  portal_view.height = 240.0f;
  portal_view.radius = 10.0f;
  inlay_portal_view(app->portal, &portal_view);
  end_box();
  end_canvas_node();
  end_canvas();
  end_box();
  end_box();

  // Center Bottom Card: Terminal · Event Trace (Grow)
  begin_box({
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .padding = Padding::all(14.0f),
      .gap = 10.0f,
      .background = t.bg_surface,
      .border_color = card_border,
      .border_width = 1.0f,
      .radius = Corners::all(16.0f),
      .continuous_corners = true,
      .top_rim_alpha = light ? 0.0f : 0.14f,
      .elevation = Elevation::Md,
  });
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::fixed(20.0f),
      .gap = 8.0f,
      .align_y = AlignY::Center,
  });
  box({.width = Sizing::fixed(7.0f),
       .height = Sizing::fixed(7.0f),
       .background = emerald,
       .radius = Corners::all(4.0f)});
  label("Terminal · Event Trace", 13.0f, t.text_primary, kFontBold);
  box({.width = Sizing::grow()});
  label("ANSI · ZERO-COPY RING", 9.5f, t.text_muted, kFontMono);
  end_box();

  // Dedicated Rounded Recessed Dark Terminal Well (never raw black pasted on white)
  begin_box({
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .background =
          light ? Color::rgb_hex(0x111318) : Color::rgb_hex(0x090A0D),
      .border_color = light ? Color::rgba8(15, 23, 42, 46)
                            : Color::rgba8(255, 255, 255, 22),
      .border_width = 1.0f,
      .radius = Corners::all(12.0f),
      .shadow_color = Color::rgba8(0, 0, 0, 170),
      .shadow_blur = 10.0f,
      .shadow_offset = {0.0f, 2.5f},
      .clip_children = true,
      .continuous_corners = true,
      .shadow_inset = true,
  });
  // macOS / Linear-style Terminal Title Strip
  begin_box({
      .axis = Axis::Row,
      .width = Sizing::grow(),
      .height = Sizing::fixed(28.0f),
      .padding = Padding::xy(12.0f, 0.0f),
      .gap = 6.0f,
      .align_y = AlignY::Center,
      .background = Color::rgba8(255, 255, 255, 7),
      .border_color = Color::rgba8(255, 255, 255, 14),
      .border_sides = {.bottom = 1.0f},
  });
  box({.width = Sizing::fixed(8.0f),
       .height = Sizing::fixed(8.0f),
       .background = Color::rgba8(255, 95, 86, 210),
       .radius = Corners::all(4.0f)});
  box({.width = Sizing::fixed(8.0f),
       .height = Sizing::fixed(8.0f),
       .background = Color::rgba8(255, 189, 46, 210),
       .radius = Corners::all(4.0f)});
  box({.width = Sizing::fixed(8.0f),
       .height = Sizing::fixed(8.0f),
       .background = Color::rgba8(39, 201, 63, 210),
       .radius = Corners::all(4.0f)});
  box({.width = Sizing::fixed(6.0f)});
  label("cromma-shell · trace", 11.0f, Color::rgb_hex(0x94A3B8), kFontMono);
  box({.width = Sizing::grow()});
  begin_box({
      .padding = Padding::xy(7.0f, 2.0f),
      .align_x = AlignX::Center,
      .align_y = AlignY::Center,
      .background = Color::rgba8(16, 185, 129, 34),
      .border_color = Color::rgba8(52, 211, 153, 96),
      .border_width = 1.0f,
      .radius = Corners::all(999.0f),
  });
  label("LIVE", 9.0f, Color::rgb_hex(0x34D399), kFontMono);
  end_box();
  end_box();

  begin_box({
      .axis = Axis::Col,
      .width = Sizing::grow(),
      .height = Sizing::grow(),
      .padding = Padding::xy(12.0f, 8.0f),
  });
  terminal_grid({
      .id = id("event-terminal"),
      .cells = app->terminal,
      .columns = kTermCols,
      .rows = kTermRows,
      .cell_width_px = 8.0f,
      .line_height_px = 21.0f,
      .font_size_px = 12.5f,
      .foreground = Color::rgb_hex(0xD8E0EB),
      .background = Color::transparent(),
      .cursor_column = 28,
      .cursor_row = 5,
      .cursor_color = Color::rgb_hex(0x34D399),
  });
  end_box();
  end_box();
  end_box();
  end_box();

  // Right Column: Declarative Inspector · JSX + QuickJS-NG (348px, toggleable)
  if (app->inspect) {
    begin_box({
        .axis = Axis::Col,
        .width = Sizing::fixed(348.0f),
        .height = Sizing::grow(),
        .padding = Padding::all(14.0f),
        .gap = 10.0f,
        .background = t.bg_surface,
        .border_color = card_border,
        .border_width = 1.0f,
        .radius = Corners::all(16.0f),
        .continuous_corners = true,
        .top_rim_alpha = card_rim,
        .elevation = Elevation::Md,
    });
    begin_box({
        .axis = Axis::Row,
        .width = Sizing::grow(),
        .height = Sizing::fixed(22.0f),
        .gap = 8.0f,
        .align_y = AlignY::Center,
    });
    box({.width = Sizing::fixed(7.0f),
         .height = Sizing::fixed(7.0f),
         .background = indigo,
         .radius = Corners::all(4.0f)});
    label("Declarative Inspector", 13.0f, t.text_primary, kFontBold);
    box({.width = Sizing::grow()});
    label("JSX · QuickJS-NG", 9.5f, indigo, kFontMono);
    end_box();

    if (app->inspector) {
      InlaySurfaceViewDesc surface_view = inlay_surface_view_desc_default();
      surface_view.id = id("sandbox-inspector");
      surface_view.width_px = 0.0f;
      surface_view.height_px = 0.0f;
      inlay_surface_view(app->inspector, &surface_view);
    } else {
      label("Scripting disabled", 12.5f, t.text_muted);
    }
    end_box();
  }

  end_box();
  end_box();
}

} // namespace

int main(int argc, char **argv) {
  bool headless = false;
  bool light_mode = false;
  const char *out_png = "build/cromma-workbench.png";
  const char *gpu_driver = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--headless") == 0) {
      headless = true;
    } else if (std::strcmp(argv[i], "--light") == 0) {
      light_mode = true;
    } else if (std::strcmp(argv[i], "--dark") == 0) {
      light_mode = false;
    } else if (std::strcmp(argv[i], "--renderer") == 0 && (i + 1) < argc) {
      gpu_driver = argv[++i];
      if (std::strcmp(gpu_driver, "d3d12") == 0)
        gpu_driver = "direct3d12";
    } else if (argv[i][0] != '-') {
      out_png = argv[i];
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    return 1;
  auto *window = headless
                     ? nullptr
                     : SDL_CreateWindow("Cromma Workbench", 1440, 800,
                                        SDL_WINDOW_RESIZABLE |
                                            SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!headless && !window) {
    SDL_Quit();
    return 1;
  }
  auto *ctx = create_context(
      {.window = window, .headless = headless, .gpu_driver = gpu_driver});
  if (!ctx) {
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  set_theme(light_mode ? Theme::light() : Theme::dark(), ctx);

  auto *c = inlay::to_c_context(ctx);
  Workbench app = {};
  app.ctx = ctx;
  app.light_mode = light_mode;
  app.document = inlay_doc_create(c, nullptr);
  app.inspector = inlay_surface_create(c, nullptr);
  auto portal_desc = inlay_portal_desc_default();
  portal_desc.width = 456u;
  portal_desc.height = 240u;
  app.portal = inlay_portal_create(c, &portal_desc);
  app.aurora = create_shader_surface(
      ctx, {.spirv = {g_cromma_aurora_spv, sizeof(g_cromma_aurora_spv)},
            .dxil = {g_cromma_aurora_dxil, sizeof(g_cromma_aurora_dxil)},
            .msl = {g_cromma_aurora_msl, sizeof(g_cromma_aurora_msl)},
            .continuous_time = false});
  bool ok = app.document && app.portal && app.aurora;

  if (app.document) {
    const char *chunks[] = {
        "# A Native Research Loop\n\n",
        "Synthesize **live evidence** with `inlay_doc`, [verified "
        "sources](https://example.org), and *zero-copy* native layout. "
        "~~Manual refresh~~\n\n",
        "> [!TIP]\n> Incremental UTF-8 parsing freezes completed blocks while "
        "streaming smoothly at 60 fps.\n\n",
        "| Stage | Engine | Status |\n| --- | --- | --- |\n"
        "| Parse \\| stream | md4c | **Ready** |\n"
        "| Layout | Etch GPU | **Active** |\n\n",
        "```mermaid\nflowchart TD\nsubgraph Research\n"
        "A[Plan] -->|query| B[Portal]\n"
        "B -->|evidence| C[Inspect]\n"
        "C -. refine .-> A\nend\n```\n\n",
        "## Confidence Model\n\n$$\\frac{a+b}{\\sqrt{x_1^2+x_2^2}}$$\n\n",
        "```cpp\n// Single-pass GPU frame arena\n"
        "const auto res = inlay_doc_view(doc, &view);\n"
        "if (res.at_bottom) sync_portal(app->portal);\n```\n\n",
        "- [x] Native TeX math and Mermaid diagrams\n"
        "- [x] QuickJS-NG declarative inspector\n"
        "- [x] Zoomed optical-vibrancy portal node\n\n",
    };
    for (const auto *chunk : chunks)
      ok &= inlay_doc_append_utf8(
          app.document,
          {chunk, static_cast<uint32_t>(std::strlen(chunk)), false});
  }

  if (app.inspector) {
    ok &= inlay_surface_bind_slot(app.inspector, ETCH_STR_LIT("live-badge"),
                                  render_telemetry_slot, &app);
    ok &= inlay_surface_bind_slot(app.inspector, ETCH_STR_LIT("telemetry"),
                                  render_telemetry_slot, &app);
    static const char inspector_js[] = R"JS(
const PIPELINE_COLS = [
  { key: 'pass',   title: 'Pass',   minWidth: 68 },
  { key: 'time',   title: 'Time',   width: 66, numeric: true },
  { key: 'mem',    title: 'Mem',    width: 60, numeric: true },
  { key: 'status', title: 'Status', width: 58, align: 'right' }
];

const PIPELINE_ROWS = [
  { pass: 'Doc',      time: '0.42ms', mem: '1.8MB', status: 'Ready' },
  { pass: 'Portal',   time: '1.15ms', mem: '4.2MB', status: 'Sync'  },
  { pass: 'JSX UI',   time: '0.00ms', mem: '0.6MB', status: 'Idle'  },
  { pass: 'Etch GPU', time: '0.68ms', mem: '6.4MB', status: 'OK'    }
];

function SectionTitle({ title, tag }) {
  return (
    <Row cls="w-full justify-between items-center">
      <Text cls="text-2xs font-mono text-muted tracking-wide">{title}</Text>
      {tag ? <Badge variant="secondary" cls="text-2xs">{tag}</Badge> : null}
    </Row>
  );
}

function render(s) {
  const tab = s.tab || 0;
  const budget = s.budget !== undefined ? s.budget : 4.0;
  const pct = Math.round(Math.min(100, Math.max(0, (budget / 8.0) * 100)));
  const selectedNode = s.node || 'portal';
  const selectedRow = s.row !== undefined ? s.row : 2;
  const checks = s.checks || 1;
  const vibrancy = s.vibrancy !== false;
  const liveSync = s.live !== false;
  const tabular = s.tabular !== false;

  const overviewPane = (
    <>
      <Card cls="w-full bg-elevated rounded-xl shadow-sm p-2.5 gap-2 border border-subtle squircle transition duration-150 ease-out hover:border-accent/40">
        <SectionTitle title="PIPELINE METRICS" tag="0-tick sort" />
        <Table
          id="pipeline-table"
          columns={PIPELINE_COLS}
          rows={PIPELINE_ROWS}
          headerHeight={28}
          rowHeight={26}
          cellPaddingX={6}
          fontSize={11.5}
          selectedRow={selectedRow}
          onRowClick={e => { state.row = e.row; }}
        />
      </Card>

      <Card cls="w-full bg-elevated rounded-xl shadow-sm p-2.5 gap-2 border border-subtle squircle transition duration-150 ease-out hover:bg-hover">
        <SectionTitle title="FRAME HEADROOM" tag={budget.toFixed(1) + ' ms'} />
        <Row cls="w-full items-center gap-3">
          <Progress
            id="headroom-ring"
            variant="ring"
            value={pct}
            min={0}
            max={100}
            width={50}
            height={50}
            thickness={4.5}
            showPercent
          />
          <Col cls="flex-1 gap-1.5 min-w-0">
            <Row cls="w-full justify-between items-center">
              <Text cls="text-xs font-medium text-primary">Tick budget</Text>
              <Text cls="text-xs font-mono tabular-nums text-accent">{pct}%</Text>
            </Row>
            <Progress value={pct} min={0} max={100} cls="w-full h-2 rounded-full" />
            <Slider
              id="budget-slider"
              value={budget}
              min={1}
              max={8}
              step={0.5}
              width="grow"
              onChange={v => { state.budget = v; }}
            />
          </Col>
        </Row>
      </Card>

      <Card cls="w-full bg-elevated rounded-xl shadow-sm p-2.5 gap-2 border border-subtle squircle">
        <Row cls="w-full justify-between items-center">
          <Text cls="text-2xs font-mono text-muted tracking-wide">LATENCY GUARD</Text>
          <Math tex={'\\Delta t < 2\\,\\text{ms}'} fontSize={12.5} />
        </Row>
        <Slot name="live-badge" cls="w-full h-[52px]" props={{ tab, budget }} />
      </Card>

      <Row cls="w-full items-center gap-2">
        <Button
          id="run-check"
          variant="primary"
          cls="flex-1 h-8 rounded-lg text-xs transition duration-150 hover:scale-102 active:scale-98"
          onClick={() => { state.checks = checks + 1; state.toastOpen = true; }}
        >
          Verify
          <Badge cls="bg-white/20 text-white text-2xs px-1.5 py-0.5">{checks}</Badge>
        </Button>
        <Button
          id="open-scene"
          variant="default"
          cls="h-8 rounded-lg text-xs transition duration-150 hover:bg-hover"
          onClick={() => { state.tab = 1; }}
        >
          Scene
        </Button>
      </Row>
    </>
  );

  const scenePane = (
    <>
      <Card cls="w-full bg-elevated rounded-xl shadow-sm p-2.5 gap-1.5 border border-subtle squircle">
        <SectionTitle title="SCENE TREE" tag={selectedNode} />
        <Tree id="scene-tree" showIndentGuide>
          <TreeItem label="Cromma Workbench" defaultOpen selected={selectedNode === 'root'} onSelect={() => { state.node = 'root'; }}>
            <TreeItem label="Aurora Shader" leaf selected={selectedNode === 'aurora'} onSelect={() => { state.node = 'aurora'; }} />
            <TreeItem label="Portal Node @ 65%" defaultOpen selected={selectedNode === 'portal'} onSelect={() => { state.node = 'portal'; }}>
              <TreeItem label="Frosted Vibrancy" leaf selected={selectedNode === 'frost'} onSelect={() => { state.node = 'frost'; }} />
            </TreeItem>
            <TreeItem label="Markdown Stream" leaf selected={selectedNode === 'doc'} onSelect={() => { state.node = 'doc'; }} />
            <TreeItem label="ANSI Terminal" leaf selected={selectedNode === 'term'} onSelect={() => { state.node = 'term'; }} />
            <TreeItem label="Inspector JSX" leaf selected={selectedNode === 'jsx'} onSelect={() => { state.node = 'jsx'; }} />
          </TreeItem>
        </Tree>
      </Card>

      <Accordion id="render-opts" title="Compositor Settings" defaultOpen cls="w-full">
        <Switch label="Live stream sync" size="sm" checked={liveSync} onChange={v => { state.live = v; }} />
        <Switch label="Optical vibrancy" size="sm" checked={vibrancy} onChange={v => { state.vibrancy = v; }} />
        <Switch label="Tabular numerals" size="sm" checked={tabular} onChange={v => { state.tabular = v; }} />
      </Accordion>

      <Card cls="w-full bg-elevated rounded-xl shadow-sm p-2.5 gap-2 border border-subtle squircle">
        <SectionTitle title="ASYNC PLACEHOLDER" tag="Skeleton" />
        <Row cls="w-full items-center gap-2.5">
          <Skeleton variant="circle" width={28} height={28} />
          <Skeleton variant="text" lines={2} lineGap={6} />
        </Row>
      </Card>
    </>
  );

  const consolePane = (
    <>
      <Terminal
        id="jsx-term"
        title="jsx-probe"
        cols={34}
        rows={6}
        fontSize={11}
        cls="w-full"
        lines={[
          '\x1b[32m$\x1b[0m inlay inspect --jsx --cls',
          '\x1b[36m[jsx]\x1b[0m zero-Node transpile  \x1b[32mOK\x1b[0m',
          '\x1b[35m[cls]\x1b[0m live theme tokens    \x1b[32mOK\x1b[0m',
          '\x1b[33m[vm]\x1b[0m  idle JS ticks        \x1b[1m0\x1b[0m',
          '\x1b[90m> ready for HMR reload...\x1b[0m'
        ]}
      />

      <Card cls="w-full bg-elevated rounded-xl shadow-sm p-2.5 gap-1.5 border border-subtle squircle">
        <SectionTitle title="NOTES" tag="Doc" />
        <Doc
          fontSize={12}
          markdown={'### Inlay v)JS" INLAY_VERSION_STRING R"JS(\n- Zero-build **JSX** + utility `cls`\n- Native **Table**, **Tree**, **Terminal**\n- Automatic `transition` · **0** idle ticks'}
        />
      </Card>

      <Row cls="w-full items-center gap-2">
        <Button
          id="show-toast"
          variant="primary"
          cls="flex-1 h-8 rounded-lg text-xs transition duration-150 hover:scale-102"
          onClick={() => { state.toastOpen = true; }}
        >
          Show Toast
        </Button>
        <Button
          id="hide-toast"
          variant="default"
          cls="h-8 rounded-lg text-xs transition duration-150 hover:bg-hover"
          onClick={() => { state.toastOpen = false; }}
        >
          Dismiss
        </Button>
      </Row>
    </>
  );

  return (
    <Col cls="w-full h-full gap-2.5">
      <Row cls="w-full items-center gap-2 bg-elevated rounded-xl p-2 border border-subtle squircle">
        <Avatar name="Cromma AI" size="sm" status="online" />
        <Col cls="flex-1 gap-0.5 min-w-0">
          <Text cls="text-xs font-semibold text-primary truncate">Cromma AI</Text>
          <Text cls="text-2xs font-mono text-muted truncate">Live Inspector</Text>
        </Col>
        <Badge variant="success" dot cls="text-2xs">v)JS" INLAY_VERSION_STRING R"JS(</Badge>
        <Badge variant="accent" cls="text-2xs">JSX + HMR</Badge>
      </Row>

      <Tabs
        id="mode"
        cls="w-full"
        fontSize={12}
        items={['Overview', 'Scene', 'Console']}
        active={tab}
        onChange={i => { state.tab = i; }}
      />

      <Scroll id="inspector-body" cls="w-full h-full gap-2.5">
        {tab === 0 ? overviewPane : tab === 1 ? scenePane : consolePane}
      </Scroll>

      <Toast
        id="status-toast"
        open={!!s.toastOpen}
        variant="success"
        position="bottom-right"
        title="Workbench Verified"
        description="Zero-Node JSX, Oklab cls tokens & 0 idle JS ticks."
        actionLabel="Scene"
        onAction={() => { state.tab = 1; state.toastOpen = false; }}
        onClose={() => { state.toastOpen = false; }}
      />
    </Col>
  );
}

function onEvent(name, payload, s) {
  if (name === 'setTab') s.tab = payload;
  if (name === 'setBudget') s.budget = payload;
  if (name === 'setLive') s.live = payload;
  if (name === 'setVibrancy') s.vibrancy = payload;
}
)JS";
    ok &= inlay_surface_set_script(
        app.inspector,
        {inspector_js, static_cast<uint32_t>(sizeof(inspector_js) - 1u), true});
    if (inlay_surface_stats(app.inspector).has_error) {
      const auto err = inlay_surface_error(app.inspector);
      std::fprintf(stderr, "Inspector script error: %.*s\n", int(err.length),
                   err.chars ? err.chars : "");
    }
  }

  populate_terminal(&app);

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
      begin_frame(ctx,
                  {.viewport_w = 1440u, .viewport_h = 800u, .dt_seconds = 1.f / 60.f});
    else
      begin_window_frame(ctx, window, 1.f / 60.f);
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
      ok &= render_window(ctx, theme(ctx).bg_canvas);
    }
    if (headless && ++frames == 12u) {
      const bool png_ok = render_headless_png(ctx, 1440u, 800u, out_png,
                                              theme(ctx).bg_canvas);
      const auto stats = frame_stats(ctx);
      const bool dc_ok = stats.draw_call_count > 0u && stats.draw_call_count <= 48u;
      const bool insp_ok = !app.inspector || !inlay_surface_stats(app.inspector).has_error;
      if (!ok || !png_ok || !dc_ok || !portal_visible || !insp_ok) {
        std::fprintf(stderr, "Cromma frame12: prev_ok=%d png=%d dc_ok=%d portal_vis=%d insp_ok=%d\n",
                     ok, png_ok, dc_ok, portal_visible, insp_ok);
      }
      ok &= png_ok && dc_ok && portal_visible && insp_ok;
      running = false;
    }
    if (!headless || !running)
      inlay_portal_frame_submitted(app.portal);
  }
  if (headless) {
    const auto stats = frame_stats(ctx);
    std::printf("Cromma integration: %s (dc=%u)\n", ok ? "PASS" : "FAIL",
                stats.draw_call_count);
    if (!ok) {
      std::fprintf(stderr, "Cromma diag: doc=%d portal=%d aurora=%d insp_err=%d dc=%u\n",
                   app.document != nullptr, app.portal != nullptr, app.aurora != 0u,
                   app.inspector ? int(inlay_surface_stats(app.inspector).has_error) : -1,
                   stats.draw_call_count);
    }
  }
  inlay_portal_frame_submitted(app.portal);
  inlay_portal_destroy(app.portal);
  inlay_surface_destroy(app.inspector);
  inlay_doc_destroy(app.document);
  if (app.aurora)
    destroy_shader_surface(ctx, app.aurora);
  destroy_context(ctx);
  if (window)
    SDL_DestroyWindow(window);
  SDL_Quit();
  return ok ? 0 : 1;
}
