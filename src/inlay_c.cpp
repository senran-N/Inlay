#include "inlay_internal.h"
#include <cmath>

InlayDCompClipGeom inlay_dcomp_compute_clip(EtchRect bounds, EtchRect visible_bounds,
                                            float radius_logical, float dpi_scale) noexcept {
 InlayDCompClipGeom g = {};
 const float scale = (std::isfinite(dpi_scale) && dpi_scale > 0.f) ? dpi_scale : 1.f;
 const float bx0 = std::round(bounds.x * scale);
 const float by0 = std::round(bounds.y * scale);
 const float bx1 = std::round((bounds.x + bounds.w) * scale);
 const float by1 = std::round((bounds.y + bounds.h) * scale);
 const float ix0 = bounds.x > visible_bounds.x ? bounds.x : visible_bounds.x;
 const float iy0 = bounds.y > visible_bounds.y ? bounds.y : visible_bounds.y;
 const float ix1 = (bounds.x + bounds.w) < (visible_bounds.x + visible_bounds.w)
  ? (bounds.x + bounds.w) : (visible_bounds.x + visible_bounds.w);
 const float iy1 = (bounds.y + bounds.h) < (visible_bounds.y + visible_bounds.h)
  ? (bounds.y + bounds.h) : (visible_bounds.y + visible_bounds.h);
 const float vx0 = std::round(ix0 * scale);
 const float vy0 = std::round(iy0 * scale);
 const float vx1 = std::round(ix1 * scale);
 const float vy1 = std::round(iy1 * scale);
 g.offset_x = bx0;
 g.offset_y = by0;
 g.width = bx1 > bx0 ? (bx1 - bx0) : 0.f;
 g.height = by1 > by0 ? (by1 - by0) : 0.f;
 g.visible = visible_bounds.w > 0.f && visible_bounds.h > 0.f && vx1 > vx0 && vy1 > vy0 && g.width > 0.f && g.height > 0.f;
 g.clipped = g.visible && (vx0 > bx0 || vy0 > by0 || vx1 < bx1 || vy1 < by1);
 if (!g.visible) return g;
 g.left = vx0 - bx0;
 g.top = vy0 - by0;
 g.right = vx1 - bx0;
 g.bottom = vy1 - by0;
 const float clip_w = g.right - g.left;
 const float clip_h = g.bottom - g.top;
 const float r_raw = (std::isfinite(radius_logical) && radius_logical > 0.f) ? (radius_logical * scale) : 0.f;
 const float r_max = 0.5f * std::fmin(g.width, g.height);
 const float r = std::fmin(r_raw, r_max);
 const bool clip_l = vx0 > bx0;
 const bool clip_t = vy0 > by0;
 const bool clip_r = vx1 < bx1;
 const bool clip_b = vy1 < by1;
 g.tl_r = (clip_l || clip_t) ? 0.f : r;
 g.tr_r = (clip_r || clip_t) ? 0.f : r;
 g.br_r = (clip_r || clip_b) ? 0.f : r;
 g.bl_r = (clip_l || clip_b) ? 0.f : r;
 const auto clamp_pair = [](float& a, float& b, float limit) noexcept {
  const float sum = a + b;
  if (sum > limit && sum > 0.f) {
   const float k = limit / sum;
   a *= k;
   b *= k;
  }
 };
 clamp_pair(g.tl_r, g.tr_r, clip_w);
 clamp_pair(g.bl_r, g.br_r, clip_w);
 clamp_pair(g.tl_r, g.bl_r, clip_h);
 clamp_pair(g.tr_r, g.br_r, clip_h);
 g.need_clip = g.clipped || r > 0.f;
 return g;
}

extern "C" {
INLAY_API uint32_t inlay_abi_version(void) {
 return INLAY_ABI_VERSION;
}

INLAY_API InlayDocConfig inlay_doc_config_default(void) {
 InlayDocConfig c = {};
 c.struct_size = uint32_t(sizeof(InlayDocConfig));
 c.initial_source_bytes = 4096u;
 c.initial_blocks = 64u;
 return c;
}

INLAY_API InlayDocStyle inlay_doc_style_default(void) {
 InlayDocStyle s = {};
 s.struct_size = uint32_t(sizeof(InlayDocStyle));
 s.font_id = ETCH_INVALID_FONT;
 s.bold_font_id = ETCH_INVALID_FONT;
 s.mono_font_id = ETCH_INVALID_FONT;
 return s;
}

INLAY_API InlayDocViewDesc inlay_doc_view_desc_default(void) {
 InlayDocViewDesc v = {};
 v.struct_size = uint32_t(sizeof(InlayDocViewDesc));
 v.width_px = 640.f;
 v.height_px = 480.f;
 v.font_size_px = 15.f;
 v.block_padding_px = 12.f;
 v.block_gap_px = 8.f;
 v.table_cell_padding_x = 12.f;
 v.table_cell_padding_y = 8.f;
 v.table_radius_px = 8.f;
 return v;
}

INLAY_API InlayMathDesc inlay_math_desc_default(void) {
 InlayMathDesc d = {};
 d.struct_size = uint32_t(sizeof(InlayMathDesc));
 d.font_size_px = 20.f;
 d.color = etch_get_theme(etch_current_context()).text_primary;
 return d;
}

INLAY_API InlayDiagramDesc inlay_diagram_desc_default(void) {
 InlayDiagramDesc d = {};
 d.struct_size = uint32_t(sizeof(InlayDiagramDesc));
 d.font_size_px = 16.f;
 d.node_gap_px = 24.f;
 d.rank_gap_px = 48.f;
 d.viewport_width_px = 640.f;
 d.viewport_height_px = 400.f;
 d.max_width_px = 0.f;
 d.min_scale = 0.f;
 d.has_custom_fonts = false;
 d.font_id = ETCH_FONT_SANS;
 d.bold_font_id = ETCH_FONT_SEMIBOLD;
 return d;
}

INLAY_API InlaySurfaceDesc inlay_surface_desc_default(void) {
 InlaySurfaceDesc d = {};
 d.struct_size = uint32_t(sizeof(InlaySurfaceDesc));
 d.memory_limit_bytes = 8u * 1024u * 1024u;
 d.max_stack_bytes = 512u * 1024u;
 d.max_tick_ms = 4.f;
 d.max_nodes = 4096u;
 d.max_text_bytes = 65536u;
 d.disable_jsx = false;
 return d;
}

INLAY_API InlaySurfaceViewDesc inlay_surface_view_desc_default(void) {
 InlaySurfaceViewDesc d = {};
 d.struct_size = uint32_t(sizeof(InlaySurfaceViewDesc));
 d.width_px = 640.f;
 d.height_px = 480.f;
 return d;
}

INLAY_API InlayPortalDesc inlay_portal_desc_default(void) {
 InlayPortalDesc d = {};
 d.struct_size = uint32_t(sizeof(InlayPortalDesc));
 d.width = 640u;
 d.height = 360u;
 d.mode = INLAY_PORTAL_MODE_TEXTURE;
 return d;
}

INLAY_API InlayPortalViewDesc inlay_portal_view_desc_default(void) {
 InlayPortalViewDesc d = {};
 d.struct_size = uint32_t(sizeof(InlayPortalViewDesc));
 d.width = 640.f;
 d.height = 360.f;
 return d;
}

INLAY_API InlayPortalWebView2Options inlay_portal_webview2_options_default(void) {
 InlayPortalWebView2Options o = {};
 o.struct_size = uint32_t(sizeof(InlayPortalWebView2Options));
 return o;
}

INLAY_API InlayActionPolicy inlay_action_policy_default(void) {
 InlayActionPolicy p = {};
 p.struct_size = uint32_t(sizeof(InlayActionPolicy));
 p.permission = INLAY_ACTION_READ;
 return p;
}

INLAY_API InlayImageLoaderDesc inlay_image_loader_desc_default(void) {
 InlayImageLoaderDesc d = {};
 d.struct_size = uint32_t(sizeof(InlayImageLoaderDesc));
 d.max_entries = 128u;
 d.max_vram_bytes = 64u * 1024u * 1024u;
 return d;
}

INLAY_API InlayPortalPolicy inlay_portal_policy_default(void) {
 InlayPortalPolicy p = {};
 p.struct_size = uint32_t(sizeof(InlayPortalPolicy));
 return p;
}
}
