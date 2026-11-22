#include "inlay_internal.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

struct InlayPortal {
  EtchContext *context;
  InlayPortalBackendVTable api;
  void *backend;
  EtchLoadedTexture texture;
  InlayPortalMode mode;
  uint32_t width, height, id;
  float view_width, view_height, radius;
  EtchElementGeometry geometry;
  bool has_geometry;
  bool pending_draw;
  bool host_input_enabled, geom_visible, focused, captured, pointer_down;
  InlayPortalStats stats;
};
namespace {
struct MockPortal {
  uint32_t width, height, characters;
  unsigned char *pixels;
  bool full_dirty, button_dirty, down, selected;
  uint32_t phase;
  EtchRect bounds, visible;
};
void paint(MockPortal *m, uint32_t x, uint32_t y, uint32_t w,
           uint32_t h) noexcept {
  for (uint32_t row = y; row < y + h; ++row)
    for (uint32_t col = x; col < x + w; ++col) {
      auto *p = m->pixels + (row * m->width + col) * 4;
      const bool button = col >= 20 && col < 100 && row >= 20 && row < 60;
      p[0] = button ? (m->selected ? 180 : 40) : 18;
      p[1] = button ? 130 : static_cast<unsigned char>(28 + (row / 16) % 2 * 8);
      p[2] = button ? 210 : static_cast<unsigned char>(45 + (col / 16) % 2 * 8);
      const uint32_t caret = 24 + m->characters * 8;
      if (col >= caret && col < caret + 2 && row >= 90 && row < 108) {
        p[0] = 210;
        p[1] = 230;
        p[2] = 255;
      }
      if (row >= 92 && row < 104 && col >= 24 && col < 24 + m->characters * 8 &&
          ((col - 24) % 8) < 5) {
        p[0] = 170;
        p[1] = 195;
        p[2] = 220;
      }
      p[3] = 255;
      if (row >= 64 && row < 72 && col >= 20 && col < 100)
        p[0] = static_cast<unsigned char>(m->phase);
    }
}
void *mock_create(void *, uint32_t w, uint32_t h) {
  auto *m = static_cast<MockPortal *>(std::calloc(1, sizeof(MockPortal)));
  if (!m)
    return nullptr;
  m->pixels = static_cast<unsigned char *>(std::malloc(size_t(w) * h * 4));
  if (!m->pixels) {
    std::free(m);
    return nullptr;
  }
  m->width = w;
  m->height = h;
  m->full_dirty = true;
  paint(m, 0, 0, w, h);
  return m;
}
void mock_destroy(void *b) {
  auto *m = static_cast<MockPortal *>(b);
  std::free(m->pixels);
  std::free(m);
}
bool mock_load(void *b, EtchStr) {
  auto *m = static_cast<MockPortal *>(b);
  m->full_dirty = true;
  paint(m, 0, 0, m->width, m->height);
  return true;
}
bool mock_resize(void *b, uint32_t w, uint32_t h) {
  auto *m = static_cast<MockPortal *>(b);
  auto *pixels = static_cast<unsigned char *>(std::malloc(size_t(w) * h * 4));
  if (!pixels)
    return false;
  std::free(m->pixels);
  m->pixels = pixels;
  m->width = w;
  m->height = h;
  m->full_dirty = true;
  paint(m, 0, 0, w, h);
  return true;
}
uint32_t mock_poll(void *b, InlayPortalDirtyRect *out, uint32_t capacity) {
  auto *m = static_cast<MockPortal *>(b);
  if (!capacity || (!m->full_dirty && !m->button_dirty))
    return 0;
  if (m->full_dirty)
    out[0] = {0, 0, m->width, m->height, m->width * 4, m->pixels};
  else {
    const uint32_t w = m->width < 100 ? m->width - 20 : 80,
                   h = m->height < 60 ? m->height - 20 : 40;
    out[0] = {20, 20, w, h, m->width * 4, m->pixels + (20 * m->width + 20) * 4};
  }
  return 1;
}
void mock_pointer(void *b, EtchVec2 p, bool down) {
  auto *m = static_cast<MockPortal *>(b);
  if (down && !m->down && p.x >= 20 && p.x < 100 && p.y >= 20 && p.y < 60 &&
      m->width > 20 && m->height > 20) {
    m->selected = !m->selected;
    m->button_dirty = true;
    paint(m, 20, 20, m->width < 100 ? m->width - 20 : 80,
          m->height < 60 ? m->height - 20 : 40);
  }
  m->down = down;
}
void mock_wheel(void *b, EtchVec2) { mock_load(b, {}); }
void mock_key(void *b, uint32_t key, bool down, uint16_t) {
  auto *m = static_cast<MockPortal *>(b);
  if (down && key == 8 && m->characters) {
    --m->characters;
    m->full_dirty = true;
    paint(m, 0, 0, m->width, m->height);
  }
}
void mock_text(void *b, EtchStr text) {
  auto *m = static_cast<MockPortal *>(b);
  uint32_t added = 0;
  for (uint32_t i = 0; i < text.length && added < 20; ++i)
    if ((static_cast<unsigned char>(text.chars[i]) & 0xC0) != 0x80)
      ++added;
  if (!added)
    return;
  m->characters += added;
  if (m->characters > 20)
    m->characters = 20;
  m->full_dirty = true;
  paint(m, 0, 0, m->width, m->height);
}
bool mock_ime(void *b, EtchRect *out) {
  auto *m = static_cast<MockPortal *>(b);
  *out = {24 + float(m->characters) * 8, 90, 2, 18};
  return true;
}
void mock_bounds(void *b, EtchRect bounds, EtchRect visible) {
  auto *m = static_cast<MockPortal *>(b);
  m->bounds = bounds;
  m->visible = visible;
}
void mock_ack(void *b, bool accepted) {
  auto *m = static_cast<MockPortal *>(b);
  if (accepted) {
    m->full_dirty = false;
    m->button_dirty = false;
  }
}
const InlayPortalBackendVTable mock_api = {
    mock_create, mock_destroy, mock_load,  mock_load, mock_resize,
    mock_poll,   mock_pointer, mock_wheel, mock_key,  mock_text,
    mock_ime,    mock_bounds,  mock_ack,   nullptr};
bool valid_size(uint32_t w, uint32_t h) noexcept {
  return w >= 1 && h >= 1 && w <= 8192 && h <= 8192;
}
void cancel_portal_input(InlayPortal *p) noexcept {
  if (p->captured && p->api.inject_pointer)
    p->api.inject_pointer(p->backend, {-1, -1}, false);
  p->captured = false;
  p->focused = false;
}
bool portal_accepts_input(const InlayPortal *p) noexcept {
  return p && p->has_geometry && p->host_input_enabled && p->geom_visible;
}
EtchLoadedTexture blank_texture(EtchContext *ctx, uint32_t w,
                                uint32_t h) noexcept {
  void *pixels = std::calloc(size_t(w) * h, 4);
  if (!pixels)
    return {};
  auto desc = etch_texture_desc_default();
  desc.width = w;
  desc.height = h;
  desc.rgba_pixels = pixels;
  auto texture = etch_create_texture_rgba(ctx, &desc);
  std::free(pixels);
  return texture;
}
} // namespace
extern "C" {
INLAY_API const InlayPortalBackendVTable *inlay_portal_mock_backend(void) {
  return &mock_api;
}
INLAY_API InlayPortal *inlay_portal_create(EtchContext *ctx,
                                           const InlayPortalDesc *config) {
  InlayPortalDesc d = inlay_portal_desc_default();
  if (config && !inlay_read_abi_struct(config, &d))
    return nullptr;
  const auto *api = d.backend ? d.backend : &mock_api;
  if (!ctx || !valid_size(d.width, d.height) ||
      (d.mode != INLAY_PORTAL_MODE_TEXTURE &&
       d.mode != INLAY_PORTAL_MODE_UNDERLAY) ||
      !api->create || !api->destroy || !api->resize || !api->poll_dirty_rects ||
      !api->ack_dirty_rects ||
      (api == inlay_portal_webview2_backend() &&
       d.mode != INLAY_PORTAL_MODE_UNDERLAY))
    return nullptr;
  auto *p = static_cast<InlayPortal *>(std::calloc(1, sizeof(InlayPortal)));
  if (!p)
    return nullptr;
  p->context = ctx;
  p->host_input_enabled = true;
  p->api = *api;
  p->width = d.width;
  p->height = d.height;
  p->mode = d.mode;
  p->backend = p->api.create(d.backend_user_data, d.width, d.height);
  if (!p->backend) {
    std::free(p);
    return nullptr;
  }
  if (d.mode == INLAY_PORTAL_MODE_TEXTURE) {
    p->texture = blank_texture(ctx, d.width, d.height);
    if (!p->texture.texture) {
      p->api.destroy(p->backend);
      std::free(p);
      return nullptr;
    }
  }
  return p;
}
INLAY_API bool inlay_portal_destroy(InlayPortal *p) {
  if (!p)
    return true;
  if (p->pending_draw)
    return false;
  p->api.destroy(p->backend);
  etch_destroy_loaded_texture(p->context, &p->texture);
  std::free(p);
  return true;
}
INLAY_API void inlay_portal_frame_submitted(InlayPortal *p) {
  if (p)
    p->pending_draw = false;
}
INLAY_API bool inlay_portal_load_url(InlayPortal *p, EtchStr s) {
  return p && s.chars && p->api.load_url && p->api.load_url(p->backend, s);
}
INLAY_API bool inlay_portal_load_html(InlayPortal *p, EtchStr s) {
  return p && s.chars && p->api.load_html && p->api.load_html(p->backend, s);
}
INLAY_API bool inlay_portal_resize(InlayPortal *p, uint32_t w, uint32_t h) {
  if (!p || p->pending_draw || !valid_size(w, h))
    return false;
  if (w == p->width && h == p->height)
    return true;
  EtchLoadedTexture texture = {};
  if (p->mode == INLAY_PORTAL_MODE_TEXTURE) {
    texture = blank_texture(p->context, w, h);
    if (!texture.texture)
      return false;
  }
  if (!p->api.resize(p->backend, w, h)) {
    etch_destroy_loaded_texture(p->context, &texture);
    return false;
  }
  etch_destroy_loaded_texture(p->context, &p->texture);
  p->texture = texture;
  p->width = w;
  p->height = h;
  p->has_geometry = false;
  p->geom_visible = false;
  cancel_portal_input(p);
  return true;
}
INLAY_API bool inlay_portal_poll(InlayPortal *p) {
  if (!p)
    return false;
  InlayPortalDirtyRect rects[32] = {};
  const uint32_t n = p->api.poll_dirty_rects(p->backend, rects, 32);
  if (n > 32) {
    p->api.ack_dirty_rects(p->backend, false);
    return false;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const auto &r = rects[i];
    if (!r.rgba_pixels || !r.width || !r.height || r.x >= p->width ||
        r.y >= p->height || r.width > p->width - r.x ||
        r.height > p->height - r.y || r.row_pitch_bytes < r.width * 4) {
      p->api.ack_dirty_rects(p->backend, false);
      return false;
    }
    if (p->mode == INLAY_PORTAL_MODE_TEXTURE) {
      if (!etch_update_texture_region_rgba(p->context, p->texture.texture, r.x,
                                           r.y, r.width, r.height,
                                           r.rgba_pixels, r.row_pitch_bytes)) {
        p->api.ack_dirty_rects(p->backend, false);
        return false;
      }
      p->stats.uploaded_bytes += uint64_t(r.width) * r.height * 4;
      ++p->stats.uploaded_rects;
    }
  }
  p->api.ack_dirty_rects(p->backend, true);
  if (n)
    etch_request_redraw(p->context);
  return true;
}
INLAY_API void inlay_portal_view(InlayPortal *p, const InlayPortalViewDesc *view_in) {
  InlayPortalViewDesc view = inlay_portal_view_desc_default();
  if (!p || etch_current_context() != p->context || !inlay_read_abi_struct(view_in, &view) || !view.id ||
      !std::isfinite(view.width) || !std::isfinite(view.height) ||
      view.width <= 0 || view.height <= 0 || !std::isfinite(view.radius) ||
      view.radius < 0)
    return;
  p->pending_draw = true;
  p->id = view.id;
  p->view_width = view.width;
  p->view_height = view.height;
  p->radius =
      std::fmin(view.radius, .5f * std::fmin(view.width, view.height));
  auto box = etch_box_desc_default();
  box.id = view.id;
  box.width = etch_sizing_fixed(view.width);
  box.height = etch_sizing_fixed(view.height);
  box.background = etch_rgba8(0, 0, 0, 0);
  box.radius = etch_corners_all(view.radius);
  box.clip_children = true;
  etch_begin_box(&box);
  if (p->mode == INLAY_PORTAL_MODE_TEXTURE) {
    auto image = etch_image_draw_desc_default();
    image.texture = p->texture.texture;
    image.rect = {0, 0, view.width, view.height};
    image.radius = view.radius;
    etch_draw_image(&image);
  } else {
    auto hole = etch_punch_hole_desc_default();
    hole.rect = {0, 0, view.width, view.height};
    hole.radius = etch_corners_all(view.radius);
    etch_punch_hole(&hole);
  }
  etch_end_box();
}
INLAY_API bool inlay_portal_sync_geometry(InlayPortal *p) {
  if (!p || etch_current_context() != p->context)
    return false;
  p->has_geometry = etch_element_geometry(p->id, &p->geometry);
  p->geom_visible = p->has_geometry && p->geometry.visible_bounds.w > 0 &&
                    p->geometry.visible_bounds.h > 0 && p->geometry.opacity > 0;
  if (!p->geom_visible)
    cancel_portal_input(p);
  if (p->api.set_underlay_radius) {
    const float sx = p->has_geometry && p->geometry.local_scale.x > 0.f ? p->geometry.local_scale.x : 1.f;
    const float sy = p->has_geometry && p->geometry.local_scale.y > 0.f ? p->geometry.local_scale.y : 1.f;
    p->api.set_underlay_radius(p->backend, p->radius * std::fmin(sx, sy));
  }
  if (p->api.set_underlay_bounds)
    p->api.set_underlay_bounds(
        p->backend, p->has_geometry ? p->geometry.bounds : EtchRect{},
        p->geom_visible ? p->geometry.visible_bounds : EtchRect{});
  return p->has_geometry;
}
INLAY_API bool inlay_portal_inject_pointer(InlayPortal *p, EtchVec2 screen, bool down) {
  if (!p || !p->api.inject_pointer || !std::isfinite(screen.x) ||
      !std::isfinite(screen.y))
    return false;
  if (!portal_accepts_input(p))
    return false;
  const bool pressed = down && !p->pointer_down;
  p->pointer_down = down;
  const auto &g = p->geometry;
  if (g.local_scale.x <= 0 || g.local_scale.y <= 0)
    return false;
  const auto &v = g.visible_bounds;
  bool hit = screen.x >= v.x && screen.y >= v.y && screen.x < v.x + v.w &&
             screen.y < v.y + v.h;
  const float local_x = (screen.x - g.bounds.x) / g.local_scale.x;
  const float local_y = (screen.y - g.bounds.y) / g.local_scale.y;
  const float dx =
      std::fmax(p->radius - local_x,
                std::fmax(local_x - (p->view_width - p->radius), 0.f));
  const float dy =
      std::fmax(p->radius - local_y,
                std::fmax(local_y - (p->view_height - p->radius), 0.f));
  hit &= dx * dx + dy * dy <= p->radius * p->radius;
  if (!p->captured && !hit) {
    if (down)
      p->focused = false;
    return false;
  }
  if (!p->captured && down && !pressed)
    return false;
  if (pressed)
    p->focused = true;
  p->captured = down;
  p->api.inject_pointer(
      p->backend,
      {(screen.x - g.bounds.x) / g.local_scale.x * p->width / p->view_width,
       (screen.y - g.bounds.y) / g.local_scale.y * p->height / p->view_height},
      down);
  return true;
}
INLAY_API void inlay_portal_inject_wheel(InlayPortal *p, EtchVec2 delta) {
  if (portal_accepts_input(p) && p->focused && p->api.inject_wheel &&
      std::isfinite(delta.x) && std::isfinite(delta.y))
    p->api.inject_wheel(p->backend, delta);
}
INLAY_API void inlay_portal_inject_key(InlayPortal *p, uint32_t key, bool down,
                                       uint16_t mods) {
  if (portal_accepts_input(p) && p->focused && p->api.inject_key)
    p->api.inject_key(p->backend, key, down, mods);
}
INLAY_API void inlay_portal_inject_text(InlayPortal *p, EtchStr text) {
  if (portal_accepts_input(p) && p->focused && text.chars && p->api.inject_text)
    p->api.inject_text(p->backend, text);
}
INLAY_API bool inlay_portal_query_ime_rect(InlayPortal *p, EtchRect *out) {
  if (!out || !portal_accepts_input(p) || !p->focused || !p->api.query_ime_rect)
    return false;
  EtchRect r;
  if (!p->api.query_ime_rect(p->backend, &r))
    return false;
  const auto &g = p->geometry;
  const float sx = g.local_scale.x * p->view_width / p->width,
              sy = g.local_scale.y * p->view_height / p->height;
  *out = {g.bounds.x + r.x * sx, g.bounds.y + r.y * sy, r.w * sx, r.h * sy};
  return true;
}
INLAY_API InlayPortalStats inlay_portal_stats(const InlayPortal *p) {
  if (!p)
    return {};
  auto s = p->stats;
  s.has_texture = p->texture.texture != nullptr;
  return s;
}
INLAY_API bool inlay_portal_has_focus(const InlayPortal *p) {
  return portal_accepts_input(p) && p->focused;
}
INLAY_API void inlay_portal_set_input_enabled(InlayPortal *p, bool enabled) {
  if (!p)
    return;
  if (!enabled)
    cancel_portal_input(p);
  p->host_input_enabled = enabled;
}
INLAY_API bool inlay_portal_mock_advance(InlayPortal *p, uint32_t phase) {
  if (!p || p->api.create != mock_create)
    return false;
  auto *m = static_cast<MockPortal *>(p->backend);
  if (m->phase == phase)
    return true;
  m->phase = phase;
  m->full_dirty = true;
  paint(m, 0, 0, m->width, m->height);
  return true;
}
}
