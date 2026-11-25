#include "inlay_internal.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef INLAY_PORTAL_HOST_HOOKS_DEFINED
#define INLAY_PORTAL_HOST_HOOKS_DEFINED
struct InlayPortalHostHooks {
  InlayPortalNavDecision (*on_nav_check)(void *ctx, EtchStr url,
                                         bool is_new_window) noexcept;
  void (*on_web_message)(void *ctx, EtchStr msg) noexcept;
  bool (*on_perm_check)(void *ctx, EtchStr origin, EtchStr perm_kind) noexcept;
  void (*on_title_changed)(void *ctx, EtchStr title) noexcept;
  void (*on_loading_changed)(void *ctx, bool is_loading) noexcept;
  void (*on_source_changed)(void *ctx, EtchStr url) noexcept;
  void (*on_favicon_changed)(void *ctx, EtchStr favicon_url) noexcept;
  void *ctx;
};
#endif

bool inlay_portal_webview2_post_message(void *backend, EtchStr msg) noexcept;
bool inlay_portal_webview2_nav_action(void *backend, int action) noexcept;
bool inlay_portal_webview2_eval_script(void *backend, EtchStr script) noexcept;
bool inlay_portal_webview2_add_init_script(void *backend, EtchStr script) noexcept;
bool inlay_portal_webview2_open_devtools(void *backend) noexcept;
bool inlay_portal_webview2_set_zoom(void *backend, float zoom_factor) noexcept;
bool inlay_portal_webview2_inject_mouse_button(void *backend, EtchVec2 local_pos, uint8_t button, bool down) noexcept;
void inlay_portal_webview2_set_hooks(void *backend,
                                     const InlayPortalHostHooks *hooks) noexcept;
bool inlay_portal_macos_nav_action(void *backend, int action) noexcept;
bool inlay_portal_macos_eval_script(void *backend, EtchStr script) noexcept;
bool inlay_portal_macos_add_init_script(void *backend, EtchStr script) noexcept;
bool inlay_portal_macos_set_zoom(void *backend, float zoom_factor) noexcept;
bool inlay_portal_macos_open_devtools(void *backend) noexcept;
void inlay_portal_macos_set_hooks(void *backend, const InlayPortalHostHooks *hooks) noexcept;
bool inlay_portal_gtk_nav_action(void *backend, int action) noexcept;
bool inlay_portal_gtk_eval_script(void *backend, EtchStr script) noexcept;
bool inlay_portal_gtk_add_init_script(void *backend, EtchStr script) noexcept;
bool inlay_portal_gtk_set_zoom(void *backend, float zoom_factor) noexcept;
bool inlay_portal_gtk_open_devtools(void *backend) noexcept;
void inlay_portal_gtk_set_hooks(void *backend, const InlayPortalHostHooks *hooks) noexcept;

struct PortalMsg {
  char *data;
  uint32_t len;
};

struct InlayPortal {
  static constexpr uint32_t kMaxMsgQueue = 32u;
  static constexpr uint32_t kMaxHistory = 256u;
  static constexpr uint32_t kMaxMessageBytes = 65536u;

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
  bool is_webview2, is_macos, is_gtk, dispatching_message, is_loading, dispatching_state;
  float zoom_factor;
  bool find_active;
  uint32_t find_active_match, find_total_matches;
  InlayPortalStats stats;
  InlayPortalPolicy policy;
  char *allowed_origins;
  uint32_t allowed_origins_len;
  char *title;
  uint32_t title_len;
  char *favicon_url;
  uint32_t favicon_url_len;
  char *find_query;
  uint32_t find_query_len;
  char *html_body;
  uint32_t html_body_len;
  char **history;
  uint32_t history_count, history_cap;
  int32_t history_index;
  PortalMsg msg_queue[kMaxMsgQueue];
  uint32_t msg_head, msg_count;
};
namespace {
struct MockPortal {
  uint32_t width, height, characters;
  unsigned char *pixels;
  bool full_dirty, button_dirty, down, selected;
  uint32_t phase;
  uint32_t eval_count, init_script_count, find_count;
  bool devtools_open, find_active;
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
  size_t pos = 0;
  while (pos < text.length && added < 20) {
    (void)inlay_utf8_next(text.chars, text.length, &pos);
    ++added;
  }
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
char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}
bool ascii_ieq(const char *a, uint32_t a_len, const char *b,
               uint32_t b_len) noexcept {
  if (a_len != b_len)
    return false;
  for (uint32_t i = 0; i < a_len; ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i]))
      return false;
  }
  return true;
}
bool is_ascii_alpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_ascii_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
bool is_scheme_char(char c) noexcept {
  return is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '-' ||
         c == '.';
}
struct OriginParts {
  const char *scheme;
  uint32_t scheme_len;
  const char *host;
  uint32_t host_len;
  const char *port;
  uint32_t port_len;
};
bool extract_origin(const char *s, uint32_t len, OriginParts *out,
                    bool require_origin_only) noexcept {
  if (!s || len < 4u || !is_ascii_alpha(s[0]))
    return false;
  uint32_t i = 1u;
  while (i < len && is_scheme_char(s[i]))
    ++i;
  if (i + 3u > len || s[i] != ':' || s[i + 1u] != '/' || s[i + 2u] != '/')
    return false;
  const uint32_t scheme_len = i;
  const uint32_t auth_start = i + 3u;
  uint32_t auth_end = auth_start;
  while (auth_end < len) {
    const unsigned char ch = static_cast<unsigned char>(s[auth_end]);
    if (ch == '/' || ch == '?' || ch == '#' || ch <= 0x20u)
      break;
    ++auth_end;
  }
  if (auth_end == auth_start)
    return false;
  if (require_origin_only && auth_end < len &&
      !(auth_end + 1u == len && s[auth_end] == '/'))
    return false;
  uint32_t host_start = auth_start;
  for (uint32_t k = auth_start; k < auth_end; ++k) {
    if (s[k] == '@')
      host_start = k + 1u;
  }
  if (host_start >= auth_end)
    return false;
  uint32_t host_end = auth_end;
  const char *port = nullptr;
  uint32_t port_len = 0u;
  if (s[host_start] == '[') {
    uint32_t rb = host_start + 1u;
    while (rb < auth_end && s[rb] != ']')
      ++rb;
    if (rb >= auth_end || rb == host_start + 1u)
      return false;
    host_end = rb + 1u;
    if (host_end < auth_end) {
      if (s[host_end] != ':')
        return false;
      port = s + host_end + 1u;
      port_len = auth_end - (host_end + 1u);
    }
  } else {
    for (uint32_t k = host_start; k < auth_end; ++k) {
      if (s[k] == ':') {
        host_end = k;
        port = s + k + 1u;
        port_len = auth_end - (k + 1u);
        break;
      }
    }
  }
  if (host_end == host_start)
    return false;
  if (port) {
    if (port_len == 0u)
      return false;
    for (uint32_t k = 0u; k < port_len; ++k) {
      if (!is_ascii_digit(port[k]))
        return false;
    }
    const bool is_http = ascii_ieq(s, scheme_len, "http", 4u) ||
                         ascii_ieq(s, scheme_len, "ws", 2u);
    const bool is_https = ascii_ieq(s, scheme_len, "https", 5u) ||
                          ascii_ieq(s, scheme_len, "wss", 3u);
    if ((is_http && port_len == 2u && port[0] == '8' && port[1] == '0') ||
        (is_https && port_len == 3u && port[0] == '4' && port[1] == '4' &&
         port[2] == '3')) {
      port = nullptr;
      port_len = 0u;
    }
  }
  out->scheme = s;
  out->scheme_len = scheme_len;
  out->host = s + host_start;
  out->host_len = host_end - host_start;
  out->port = port;
  out->port_len = port_len;
  return true;
}
bool origin_in_allowlist(EtchStr url, const char *csv,
                         uint32_t csv_len) noexcept {
  OriginParts target = {};
  if (!extract_origin(url.chars, url.length, &target, false))
    return false;
  uint32_t pos = 0u;
  while (pos < csv_len) {
    uint32_t end = pos;
    while (end < csv_len && csv[end] != ',')
      ++end;
    uint32_t tok_start = pos;
    uint32_t tok_end = end;
    while (tok_start < tok_end && is_ascii_space(csv[tok_start]))
      ++tok_start;
    while (tok_end > tok_start && is_ascii_space(csv[tok_end - 1u]))
      --tok_end;
    if (tok_end > tok_start) {
      OriginParts allowed = {};
      if (extract_origin(csv + tok_start, tok_end - tok_start, &allowed,
                         true) &&
          ascii_ieq(target.scheme, target.scheme_len, allowed.scheme,
                    allowed.scheme_len) &&
          ascii_ieq(target.host, target.host_len, allowed.host,
                    allowed.host_len) &&
          target.port_len == allowed.port_len &&
          (target.port_len == 0u ||
           std::memcmp(target.port, allowed.port, target.port_len) == 0))
        return true;
    }
    pos = end < csv_len ? end + 1u : csv_len;
  }
  return false;
}
bool is_valid_utf8(const char *s, uint32_t len) noexcept {
  size_t pos = 0u;
  while (pos < len) {
    const size_t before = pos;
    const uint32_t cp = inlay_utf8_next(s, len, &pos);
    if (cp == 0xFFFDu && pos - before == 1u)
      return false;
  }
  return true;
}
void push_history(InlayPortal *p, EtchStr url) noexcept {
  char *copy = static_cast<char *>(std::malloc(size_t(url.length) + 1u));
  if (!copy)
    return;
  std::memcpy(copy, url.chars, url.length);
  copy[url.length] = '\0';
  const uint32_t keep =
      p->history_index >= 0 ? static_cast<uint32_t>(p->history_index + 1) : 0u;
  for (uint32_t i = keep; i < p->history_count; ++i)
    std::free(p->history[i]);
  p->history_count = keep;
  if (p->history_count >= InlayPortal::kMaxHistory) {
    std::free(p->history[0]);
    std::memmove(p->history, p->history + 1,
                 size_t(p->history_count - 1u) * sizeof(char *));
    --p->history_count;
    if (p->history_index > 0)
      --p->history_index;
  }
  if (!inlay_grow_pod(p->history, p->history_cap, p->history_count + 1u, 16u,
                      InlayPortal::kMaxHistory)) {
    std::free(copy);
    return;
  }
  p->history[p->history_count] = copy;
  p->history_index = static_cast<int32_t>(p->history_count);
  ++p->history_count;
}
bool enqueue_message(InlayPortal *p, const char *data, uint32_t len) noexcept {
  char *copy = static_cast<char *>(std::malloc(size_t(len) + 1u));
  if (!copy)
    return false;
  std::memcpy(copy, data, len);
  copy[len] = '\0';
  if (p->msg_count == InlayPortal::kMaxMsgQueue) {
    std::free(p->msg_queue[p->msg_head].data);
    p->msg_queue[p->msg_head] = {nullptr, 0u};
    p->msg_head = (p->msg_head + 1u) % InlayPortal::kMaxMsgQueue;
    --p->msg_count;
  }
  const uint32_t tail = (p->msg_head + p->msg_count) % InlayPortal::kMaxMsgQueue;
  p->msg_queue[tail] = {copy, len};
  ++p->msg_count;
  return true;
}
bool valid_portal_str(EtchStr s) noexcept {
  return s.chars && s.length > 0u && s.length <= InlayPortal::kMaxMessageBytes &&
         !std::memchr(s.chars, 0, s.length) && is_valid_utf8(s.chars, s.length);
}
InlayPortalNavDecision evaluate_navigation_policy(InlayPortal *p, EtchStr url,
                                                  bool is_new_window) noexcept {
  const InlayPortalNavFn nav_fn = p->policy.on_navigate;
  void *const user_data = p->policy.user_data;
  if (nav_fn) {
    const InlayPortalNavDecision decision =
        nav_fn(p, url, is_new_window, user_data);
    if (is_new_window && !p->policy.allow_new_windows &&
        decision == INLAY_PORTAL_NAV_ALLOW)
      return INLAY_PORTAL_NAV_BLOCK;
    return decision;
  }
  if (is_new_window && !p->policy.allow_new_windows)
    return INLAY_PORTAL_NAV_BLOCK;
  if (p->allowed_origins_len > 0u &&
      !origin_in_allowlist(url, p->allowed_origins, p->allowed_origins_len)) {
    return p->policy.allow_external_links ? INLAY_PORTAL_NAV_OPEN_EXTERNAL
                                          : INLAY_PORTAL_NAV_BLOCK;
  }
  return INLAY_PORTAL_NAV_ALLOW;
}
bool deliver_incoming_message(InlayPortal *p, EtchStr msg) noexcept {
  if (!enqueue_message(p, msg.chars, msg.length))
    return false;
  const InlayPortalMessageFn msg_fn = p->policy.on_message;
  void *const user_data = p->policy.user_data;
  if (msg_fn && !p->dispatching_message) {
    p->dispatching_message = true;
    msg_fn(p, msg, user_data);
    p->dispatching_message = false;
  }
  return true;
}
InlayPortalNavDecision portal_hook_nav_check(void *ctx, EtchStr url,
                                             bool is_new_window) noexcept {
  return evaluate_navigation_policy(static_cast<InlayPortal *>(ctx), url,
                                    is_new_window);
}
void portal_hook_web_message(void *ctx, EtchStr msg) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  if (!valid_portal_str(msg))
    return;
  (void)deliver_incoming_message(p, msg);
}
bool portal_hook_perm_check(void *ctx, EtchStr origin,
                            EtchStr perm_kind) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  const InlayPortalPermissionFn perm_fn = p->policy.on_permission;
  if (!perm_fn)
    return false;
  return perm_fn(p, origin, perm_kind, p->policy.user_data);
}
void set_owned_str(char **dst, uint32_t *dst_len, EtchStr src) noexcept {
  if (!src.chars || src.length == 0u) {
    std::free(*dst);
    *dst = nullptr;
    *dst_len = 0u;
    return;
  }
  const uint32_t clamped = src.length > 4096u ? 4096u : src.length;
  auto *copy = static_cast<char *>(std::malloc(size_t(clamped) + 1u));
  if (!copy)
    return;
  std::memcpy(copy, src.chars, clamped);
  copy[clamped] = '\0';
  std::free(*dst);
  *dst = copy;
  *dst_len = clamped;
}
void derive_favicon_from_url(InlayPortal *p, EtchStr url) noexcept {
  OriginParts parts = {};
  if (extract_origin(url.chars, url.length, &parts, false) &&
      (ascii_ieq(parts.scheme, parts.scheme_len, "http", 4u) ||
       ascii_ieq(parts.scheme, parts.scheme_len, "https", 5u))) {
    const uint32_t need = parts.scheme_len + 3u + parts.host_len +
                          (parts.port_len ? (1u + parts.port_len) : 0u) + 12u;
    auto *buf = static_cast<char *>(std::malloc(size_t(need) + 1u));
    if (buf) {
      uint32_t wp = 0u;
      std::memcpy(buf + wp, parts.scheme, parts.scheme_len); wp += parts.scheme_len;
      std::memcpy(buf + wp, "://", 3u); wp += 3u;
      std::memcpy(buf + wp, parts.host, parts.host_len); wp += parts.host_len;
      if (parts.port_len) {
        buf[wp++] = ':';
        std::memcpy(buf + wp, parts.port, parts.port_len); wp += parts.port_len;
      }
      std::memcpy(buf + wp, "/favicon.ico", 12u); wp += 12u;
      buf[wp] = '\0';
      std::free(p->favicon_url);
      p->favicon_url = buf;
      p->favicon_url_len = wp;
      return;
    }
  }
  set_owned_str(&p->favicon_url, &p->favicon_url_len, {});
}
void extract_html_title(InlayPortal *p, EtchStr html) noexcept {
  if (!html.chars || html.length < 15u)
    return;
  for (uint32_t i = 0u; i + 7u <= html.length; ++i) {
    if (html.chars[i] == '<' && ascii_ieq(html.chars + i + 1u, 5u, "title", 5u)) {
      uint32_t gt = i + 6u;
      while (gt < html.length && html.chars[gt] != '>') ++gt;
      if (gt >= html.length) break;
      const uint32_t ts = gt + 1u;
      for (uint32_t j = ts; j + 8u <= html.length; ++j) {
        if (html.chars[j] == '<' && html.chars[j + 1u] == '/' &&
            ascii_ieq(html.chars + j + 2u, 5u, "title", 5u) && html.chars[j + 7u] == '>') {
          set_owned_str(&p->title, &p->title_len, {html.chars + ts, j - ts, false});
          return;
        }
      }
      break;
    }
  }
}
void fill_portal_state(const InlayPortal *p, InlayPortalState *out) noexcept {
  *out = inlay_portal_state_default();
  if (!p)
    return;
  out->is_loading = p->is_loading;
  out->can_go_back = p->history_index > 0;
  out->can_go_forward = p->history_index >= 0 &&
                        static_cast<uint32_t>(p->history_index) + 1u < p->history_count;
  out->zoom_factor = p->zoom_factor > 0.f ? p->zoom_factor : 1.0f;
  out->title = p->title ? EtchStr{p->title, p->title_len, false} : EtchStr{};
  out->url = inlay_portal_current_url(p);
  out->favicon_url = p->favicon_url ? EtchStr{p->favicon_url, p->favicon_url_len, false} : EtchStr{};
}
void notify_state_change(InlayPortal *p) noexcept {
  if (!p || p->dispatching_state || !p->policy.on_state_change)
    return;
  InlayPortalState st = {};
  fill_portal_state(p, &st);
  p->dispatching_state = true;
  p->policy.on_state_change(p, &st, p->policy.user_data);
  p->dispatching_state = false;
}
void portal_hook_title_changed(void *ctx, EtchStr title) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  set_owned_str(&p->title, &p->title_len, title);
  notify_state_change(p);
}
void portal_hook_loading_changed(void *ctx, bool is_loading) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  p->is_loading = is_loading;
  notify_state_change(p);
}
void portal_hook_source_changed(void *ctx, EtchStr url) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  if (!valid_portal_str(url))
    return;
  const EtchStr cur = inlay_portal_current_url(p);
  if (cur.length != url.length || (cur.length > 0u && std::memcmp(cur.chars, url.chars, cur.length) != 0)) {
    push_history(p, url);
  }
  derive_favicon_from_url(p, url);
  notify_state_change(p);
}
void portal_hook_favicon_changed(void *ctx, EtchStr favicon_url) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  if (!p) return;
  if (favicon_url.chars && favicon_url.length > 0u) {
    set_owned_str(&p->favicon_url, &p->favicon_url_len, favicon_url);
    notify_state_change(p);
  }
}
void portal_hook_web_message_dispatch(void *ctx, EtchStr msg) noexcept {
  auto *p = static_cast<InlayPortal *>(ctx);
  if (!p || !valid_portal_str(msg)) return;
  static constexpr char kFindPrefix[] = "__inlay_find__:";
  constexpr uint32_t kFindPrefixLen = sizeof(kFindPrefix) - 1u;
  if (msg.length > kFindPrefixLen && std::memcmp(msg.chars, kFindPrefix, kFindPrefixLen) == 0) {
    uint32_t act = 0u, tot = 0u;
    const char *s = msg.chars + kFindPrefixLen;
    const char *end = msg.chars + msg.length;
    while (s < end && *s >= '0' && *s <= '9') { act = act * 10u + uint32_t(*s - '0'); ++s; }
    if (s < end && *s == '/') {
      ++s;
      while (s < end && *s >= '0' && *s <= '9') { tot = tot * 10u + uint32_t(*s - '0'); ++s; }
    }
    p->find_active = (tot > 0u);
    p->find_active_match = act;
    p->find_total_matches = tot;
    return;
  }
  portal_hook_web_message(ctx, msg);
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
  const auto *wv2 = inlay_portal_webview2_backend();
  const auto *macb = inlay_portal_macos_backend();
  const auto *gtkb = inlay_portal_gtk_backend();
  const bool is_wv2 = wv2 != nullptr && api == wv2;
  const bool is_mac = macb != nullptr && api == macb;
  const bool is_gtk = gtkb != nullptr && api == gtkb;
  if (!ctx || !valid_size(d.width, d.height) ||
      (d.mode != INLAY_PORTAL_MODE_TEXTURE &&
       d.mode != INLAY_PORTAL_MODE_UNDERLAY) ||
      !api->create || !api->destroy || !api->resize || !api->poll_dirty_rects ||
      !api->ack_dirty_rects ||
      ((is_wv2 || is_mac || is_gtk) && d.mode != INLAY_PORTAL_MODE_UNDERLAY))
    return nullptr;
  auto *p = static_cast<InlayPortal *>(std::calloc(1, sizeof(InlayPortal)));
  if (!p)
    return nullptr;
  p->context = ctx;
  p->host_input_enabled = true;
  p->is_webview2 = is_wv2;
  p->is_macos = is_mac;
  p->is_gtk = is_gtk;
  p->zoom_factor = 1.0f;
  p->policy = inlay_portal_policy_default();
  p->history_index = -1;
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
  if (p->is_webview2 || p->is_macos || p->is_gtk) {
    const InlayPortalHostHooks hooks = {
        portal_hook_nav_check,
        portal_hook_web_message_dispatch,
        portal_hook_perm_check,
        portal_hook_title_changed,
        portal_hook_loading_changed,
        portal_hook_source_changed,
        portal_hook_favicon_changed,
        p,
    };
    if (p->is_webview2) inlay_portal_webview2_set_hooks(p->backend, &hooks);
    else if (p->is_macos) inlay_portal_macos_set_hooks(p->backend, &hooks);
    else if (p->is_gtk) inlay_portal_gtk_set_hooks(p->backend, &hooks);
  }
  return p;
}
INLAY_API bool inlay_portal_destroy(InlayPortal *p) {
  if (!p)
    return true;
  if (p->pending_draw)
    return false;
  if (p->is_webview2)
    inlay_portal_webview2_set_hooks(p->backend, nullptr);
  else if (p->is_macos)
    inlay_portal_macos_set_hooks(p->backend, nullptr);
  else if (p->is_gtk)
    inlay_portal_gtk_set_hooks(p->backend, nullptr);
  p->api.destroy(p->backend);
  etch_destroy_loaded_texture(p->context, &p->texture);
  std::free(p->allowed_origins);
  std::free(p->title);
  std::free(p->favicon_url);
  std::free(p->find_query);
  std::free(p->html_body);
  for (uint32_t i = 0; i < p->history_count; ++i)
    std::free(p->history[i]);
  std::free(p->history);
  for (uint32_t i = 0; i < p->msg_count; ++i) {
    const uint32_t idx = (p->msg_head + i) % InlayPortal::kMaxMsgQueue;
    std::free(p->msg_queue[idx].data);
  }
  std::free(p);
  return true;
}
INLAY_API void inlay_portal_frame_submitted(InlayPortal *p) {
  if (p)
    p->pending_draw = false;
}
INLAY_API bool inlay_portal_load_url(InlayPortal *p, EtchStr s) {
  if (!p || !valid_portal_str(s) || !p->api.load_url)
    return false;
  if (evaluate_navigation_policy(p, s, false) != INLAY_PORTAL_NAV_ALLOW)
    return false;
  if (!p->api.load_url(p->backend, s))
    return false;
  push_history(p, s);
  derive_favicon_from_url(p, s);
  set_owned_str(&p->html_body, &p->html_body_len, {});
  if (!p->is_webview2) {
    set_owned_str(&p->title, &p->title_len, s);
    p->is_loading = false;
  } else {
    p->is_loading = true;
  }
  notify_state_change(p);
  return true;
}
INLAY_API bool inlay_portal_load_html(InlayPortal *p, EtchStr s) {
  if (!p || !s.chars || !p->api.load_html || !p->api.load_html(p->backend, s))
    return false;
  extract_html_title(p, s);
  // Extract visible body text (stripping <title>/<script>/<style> blocks and tags) for mock find-in-page.
  {
    const uint32_t cap = s.length < 131072u ? s.length : 131072u;
    char *plain = static_cast<char *>(std::malloc(size_t(cap) + 1u));
    if (plain) {
      uint32_t out_n = 0u;
      bool in_tag = false;
      bool skip_block = false;
      const char *skip_end_tag = nullptr;
      uint32_t skip_end_len = 0u;
      for (uint32_t i = 0u; i < cap; ++i) {
        if (skip_block) {
          if (s.chars[i] == '<' && i + 1u < cap && s.chars[i + 1u] == '/' &&
              i + 2u + skip_end_len <= cap &&
              ascii_ieq(s.chars + i + 2u, skip_end_len, skip_end_tag, skip_end_len)) {
            uint32_t k = i + 2u + skip_end_len;
            while (k < cap && s.chars[k] != '>') ++k;
            if (k < cap) { i = k; skip_block = false; }
          }
          continue;
        }
        if (s.chars[i] == '<') {
          in_tag = true;
          const uint32_t ts = i + 1u;
          uint32_t te = ts;
          while (te < cap && is_ascii_alpha(s.chars[te])) ++te;
          const uint32_t tlen = te - ts;
          if (ascii_ieq(s.chars + ts, tlen, "script", 6u)) { skip_block = true; skip_end_tag = "script"; skip_end_len = 6u; }
          else if (ascii_ieq(s.chars + ts, tlen, "style", 5u)) { skip_block = true; skip_end_tag = "style"; skip_end_len = 5u; }
          else if (ascii_ieq(s.chars + ts, tlen, "title", 5u)) { skip_block = true; skip_end_tag = "title"; skip_end_len = 5u; }
          continue;
        }
        if (in_tag) {
          if (s.chars[i] == '>') in_tag = false;
          continue;
        }
        plain[out_n++] = s.chars[i];
      }
      plain[out_n] = '\0';
      set_owned_str(&p->html_body, &p->html_body_len, {plain, out_n, false});
      std::free(plain);
    }
  }
  p->is_loading = p->is_webview2;
  notify_state_change(p);
  return true;
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
INLAY_API void inlay_portal_set_policy(InlayPortal *p,
                                       const InlayPortalPolicy *policy_in) {
  if (!p)
    return;
  InlayPortalPolicy next = inlay_portal_policy_default();
  if (policy_in && !inlay_read_abi_struct(policy_in, &next))
    return;
  char *copy = nullptr;
  uint32_t copy_len = 0u;
  if (next.allowed_origins_csv.chars && next.allowed_origins_csv.length > 0u) {
    if (next.allowed_origins_csv.length > InlayPortal::kMaxMessageBytes)
      return;
    copy_len = next.allowed_origins_csv.length;
    copy = static_cast<char *>(std::malloc(size_t(copy_len) + 1u));
    if (!copy)
      return;
    std::memcpy(copy, next.allowed_origins_csv.chars, copy_len);
    copy[copy_len] = '\0';
  }
  std::free(p->allowed_origins);
  p->allowed_origins = copy;
  p->allowed_origins_len = copy_len;
  next.allowed_origins_csv = {copy, copy_len, false};
  p->policy = next;
}
INLAY_API bool inlay_portal_post_message(InlayPortal *p, EtchStr message_json) {
  if (!p || !valid_portal_str(message_json))
    return false;
  if (p->is_webview2)
    return inlay_portal_webview2_post_message(p->backend, message_json);
  return deliver_incoming_message(p, message_json);
}
INLAY_API uint32_t inlay_portal_peek_message_size(const InlayPortal *p) {
  if (!p || p->msg_count == 0u)
    return 0u;
  return p->msg_queue[p->msg_head].len;
}
INLAY_API uint32_t inlay_portal_poll_message(InlayPortal *p, char *out_buf,
                                             uint32_t cap) {
  if (!p || p->msg_count == 0u) {
    if (out_buf && cap > 0u)
      out_buf[0] = '\0';
    return 0u;
  }
  const PortalMsg front = p->msg_queue[p->msg_head];
  if (!out_buf || cap == 0u)
    return front.len;
  if (cap <= front.len) {
    const uint32_t copy_len = cap - 1u;
    if (copy_len > 0u)
      std::memcpy(out_buf, front.data, copy_len);
    out_buf[copy_len] = '\0';
    return front.len;
  }
  if (front.len > 0u)
    std::memcpy(out_buf, front.data, front.len);
  out_buf[front.len] = '\0';
  p->msg_queue[p->msg_head] = {nullptr, 0u};
  p->msg_head = (p->msg_head + 1u) % InlayPortal::kMaxMsgQueue;
  --p->msg_count;
  const uint32_t full_len = front.len;
  std::free(front.data);
  return full_len;
}
INLAY_API bool inlay_portal_can_go_back(const InlayPortal *p) {
  return p && p->history_index > 0;
}
INLAY_API bool inlay_portal_can_go_forward(const InlayPortal *p) {
  return p && p->history_index >= 0 &&
         static_cast<uint32_t>(p->history_index) + 1u < p->history_count;
}
INLAY_API bool inlay_portal_go_back(InlayPortal *p) {
  if (!inlay_portal_can_go_back(p))
    return false;
  const int32_t target = p->history_index - 1;
  if (p->is_webview2) {
    if (!inlay_portal_webview2_nav_action(p->backend, 0))
      return false;
    p->history_index = target;
    notify_state_change(p);
    return true;
  }
  const char *url = p->history[target];
  const EtchStr s = {url, static_cast<uint32_t>(std::strlen(url)), false};
  if (!p->api.load_url || !p->api.load_url(p->backend, s))
    return false;
  p->history_index = target;
  derive_favicon_from_url(p, s);
  set_owned_str(&p->title, &p->title_len, s);
  notify_state_change(p);
  return true;
}
INLAY_API bool inlay_portal_go_forward(InlayPortal *p) {
  if (!inlay_portal_can_go_forward(p))
    return false;
  const int32_t target = p->history_index + 1;
  if (p->is_webview2) {
    if (!inlay_portal_webview2_nav_action(p->backend, 1))
      return false;
    p->history_index = target;
    notify_state_change(p);
    return true;
  }
  const char *url = p->history[target];
  const EtchStr s = {url, static_cast<uint32_t>(std::strlen(url)), false};
  if (!p->api.load_url || !p->api.load_url(p->backend, s))
    return false;
  p->history_index = target;
  derive_favicon_from_url(p, s);
  set_owned_str(&p->title, &p->title_len, s);
  notify_state_change(p);
  return true;
}
INLAY_API bool inlay_portal_reload(InlayPortal *p) {
  if (!p)
    return false;
  if (p->is_webview2)
    return inlay_portal_webview2_nav_action(p->backend, 2);
  if (p->history_index < 0 ||
      static_cast<uint32_t>(p->history_index) >= p->history_count ||
      !p->api.load_url)
    return false;
  const char *url = p->history[p->history_index];
  const EtchStr s = {url, static_cast<uint32_t>(std::strlen(url)), false};
  return p->api.load_url(p->backend, s);
}
INLAY_API EtchStr inlay_portal_current_url(const InlayPortal *p) {
  if (!p || p->history_index < 0 ||
      static_cast<uint32_t>(p->history_index) >= p->history_count)
    return {nullptr, 0u, false};
  const char *url = p->history[p->history_index];
  return {url, static_cast<uint32_t>(std::strlen(url)), false};
}
INLAY_API bool inlay_portal_end_frame_sync(InlayPortal *p,
                                           bool host_input_enabled) {
  if (!p)
    return false;
  const bool synced = inlay_portal_sync_geometry(p);
  inlay_portal_set_input_enabled(p, host_input_enabled);
  inlay_portal_frame_submitted(p);
  return synced;
}
INLAY_API bool inlay_portal_eval_script(InlayPortal *p, EtchStr script_utf8) {
  if (!p || !valid_portal_str(script_utf8))
    return false;
  if (p->is_webview2)
    return inlay_portal_webview2_eval_script(p->backend, script_utf8);
  if (p->is_macos)
    return inlay_portal_macos_eval_script(p->backend, script_utf8);
  if (p->is_gtk)
    return inlay_portal_gtk_eval_script(p->backend, script_utf8);
  if (p->api.create == mock_create) {
    auto *m = static_cast<MockPortal *>(p->backend);
    ++m->eval_count;
    return true;
  }
  return false;
}
INLAY_API bool inlay_portal_add_init_script(InlayPortal *p, EtchStr script_utf8) {
  if (!p || !valid_portal_str(script_utf8))
    return false;
  if (p->is_webview2)
    return inlay_portal_webview2_add_init_script(p->backend, script_utf8);
  if (p->is_macos)
    return inlay_portal_macos_add_init_script(p->backend, script_utf8);
  if (p->is_gtk)
    return inlay_portal_gtk_add_init_script(p->backend, script_utf8);
  if (p->api.create == mock_create) {
    auto *m = static_cast<MockPortal *>(p->backend);
    ++m->init_script_count;
    return true;
  }
  return false;
}
INLAY_API bool inlay_portal_open_devtools(InlayPortal *p) {
  if (!p)
    return false;
  if (p->is_webview2)
    return inlay_portal_webview2_open_devtools(p->backend);
  if (p->is_macos)
    return inlay_portal_macos_open_devtools(p->backend);
  if (p->is_gtk)
    return inlay_portal_gtk_open_devtools(p->backend);
  if (p->api.create == mock_create) {
    auto *m = static_cast<MockPortal *>(p->backend);
    m->devtools_open = true;
    return true;
  }
  return false;
}
INLAY_API bool inlay_portal_capture_preview_rgba(InlayPortal *p, uint8_t *out_rgba,
                                                 uint32_t width, uint32_t height) {
  if (!p || !out_rgba || width == 0u || height == 0u || width > 4096u || height > 4096u)
    return false;
  if (p->api.create == mock_create) {
    auto *m = static_cast<MockPortal *>(p->backend);
    if (!m || !m->pixels || m->width == 0u || m->height == 0u) return false;
    for (uint32_t y = 0u; y < height; ++y) {
      const uint32_t sy = (y * m->height) / height;
      const unsigned char *src_row = m->pixels + size_t(sy) * size_t(m->width) * 4u;
      uint8_t *dst_row = out_rgba + size_t(y) * size_t(width) * 4u;
      for (uint32_t x = 0u; x < width; ++x) {
        const uint32_t sx = (x * m->width) / width;
        std::memcpy(dst_row + size_t(x) * 4u, src_row + size_t(sx) * 4u, 4u);
      }
    }
    return true;
  }
  return false;
}
INLAY_API bool inlay_portal_state(const InlayPortal *p, InlayPortalState *out_state) {
  if (!p || !out_state)
    return false;
  fill_portal_state(p, out_state);
  return true;
}
INLAY_API bool inlay_portal_set_zoom(InlayPortal *p, float zoom_factor) {
  if (!p || !std::isfinite(zoom_factor) || zoom_factor <= 0.0f)
    return false;
  const float clamped = zoom_factor < 0.25f ? 0.25f : (zoom_factor > 5.0f ? 5.0f : zoom_factor);
  if (p->is_webview2 && !inlay_portal_webview2_set_zoom(p->backend, clamped))
    return false;
  if (p->is_macos && !inlay_portal_macos_set_zoom(p->backend, clamped))
    return false;
  if (p->is_gtk && !inlay_portal_gtk_set_zoom(p->backend, clamped))
    return false;
  p->zoom_factor = clamped;
  notify_state_change(p);
  return true;
}
INLAY_API bool inlay_portal_find_in_page(InlayPortal *p, EtchStr query,
                                         bool forward, bool match_case) {
  if (!p || !valid_portal_str(query) || query.length > 1024u)
    return false;
  const bool same_q = p->find_query && p->find_query_len == query.length &&
      std::memcmp(p->find_query, query.chars, query.length) == 0;
  set_owned_str(&p->find_query, &p->find_query_len, query);
  if (p->is_webview2 || p->is_macos || p->is_gtk) {
    char script[4096];
    uint32_t wp = 0u;
    auto put = [&](const char *lit) {
      const size_t n = std::strlen(lit);
      if (wp + n < sizeof(script) - 1u) { std::memcpy(script + wp, lit, n); wp += uint32_t(n); }
    };
    put("(()=>{const q=\"");
    for (uint32_t i = 0u; i < query.length && wp + 8u < sizeof(script) - 512u; ++i) {
      const char ch = query.chars[i];
      if (ch == '"' || ch == '\\') { script[wp++] = '\\'; script[wp++] = ch; }
      else if (ch == '\n') { script[wp++] = '\\'; script[wp++] = 'n'; }
      else if (ch == '\r') { script[wp++] = '\\'; script[wp++] = 'r'; }
      else if (static_cast<unsigned char>(ch) >= 0x20u) { script[wp++] = ch; }
    }
    put(match_case ? "\",mc=true" : "\",mc=false");
    put(forward ? ",fwd=true;" : ",fwd=false;");
    put("const txt=document.body?(document.body.innerText||''):'';"
        "const hay=mc?txt:txt.toLowerCase(),ndl=mc?q:q.toLowerCase();"
        "let tot=0,idx=0;if(ndl.length){while((idx=hay.indexOf(ndl,idx))!==-1){tot++;idx+=ndl.length;}}"
        "window.__inlayFindIdx=tot?(((window.__inlayFindIdx||0)+(fwd?1:-1)+tot)%tot):0;"
        "if(window.find)window.find(q,mc,!fwd,true,false,false,false);"
        "const msg='__inlay_find__:'+(tot?(window.__inlayFindIdx+1):0)+'/'+tot;"
        "if(window.chrome&&window.chrome.webview)window.chrome.webview.postMessage(msg);"
        "else if(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.inlay)"
        "window.webkit.messageHandlers.inlay.postMessage(msg);})();");
    script[wp] = '\0';
    p->find_active = true;
    return inlay_portal_eval_script(p, {script, wp, false});
  }
  if (p->api.create == mock_create) {
    auto *m = static_cast<MockPortal *>(p->backend);
    ++m->find_count;
    m->find_active = true;
    // Count occurrences against loaded HTML plain body text (or mock title/URL when no HTML loaded)
    const char *hay = (p->html_body && p->html_body_len > 0u) ? p->html_body : p->title;
    const uint32_t hay_len = (p->html_body && p->html_body_len > 0u) ? p->html_body_len : p->title_len;
    uint32_t tot = 0u;
    if (hay && hay_len >= query.length) {
      for (uint32_t i = 0u; i + query.length <= hay_len; ++i) {
        bool eq = true;
        for (uint32_t k = 0u; k < query.length; ++k) {
          const char a = match_case ? hay[i + k] : ascii_lower(hay[i + k]);
          const char b = match_case ? query.chars[k] : ascii_lower(query.chars[k]);
          if (a != b) { eq = false; break; }
        }
        if (eq) { ++tot; i += query.length - 1u; }
      }
    }
    if (tot == 0u && (!p->html_body || p->html_body_len == 0u)) tot = 1u;
    p->find_active = true;
    p->find_total_matches = tot;
    if (tot == 0u) {
      p->find_active_match = 0u;
    } else if (!same_q || p->find_active_match == 0u || p->find_active_match > tot) {
      p->find_active_match = forward ? 1u : tot;
    } else if (forward) {
      p->find_active_match = (p->find_active_match % tot) + 1u;
    } else {
      p->find_active_match = (p->find_active_match == 1u) ? tot : (p->find_active_match - 1u);
    }
    return true;
  }
  return false;
}
INLAY_API bool inlay_portal_stop_find(InlayPortal *p) {
  if (!p)
    return false;
  set_owned_str(&p->find_query, &p->find_query_len, {});
  p->find_active = false;
  p->find_active_match = 0u;
  p->find_total_matches = 0u;
  if (p->is_webview2 || p->is_macos || p->is_gtk) {
    return inlay_portal_eval_script(
        p, ETCH_STR_LIT("window.__inlayFindIdx=0;if(window.getSelection)window.getSelection().removeAllRanges();"));
  }
  if (p->api.create == mock_create) {
    auto *m = static_cast<MockPortal *>(p->backend);
    m->find_active = false;
    return true;
  }
  return false;
}
INLAY_API bool inlay_portal_find_status(const InlayPortal *p, InlayPortalFindResult *out_result) {
  if (!p || !out_result)
    return false;
  *out_result = inlay_portal_find_result_default();
  out_result->active = p->find_active;
  out_result->active_match = p->find_active_match;
  out_result->total_matches = p->find_total_matches;
  return true;
}
INLAY_API bool inlay_portal_inject_mouse_button(InlayPortal *p, EtchVec2 screen_pos,
                                                uint8_t button, bool down) {
  if (!portal_accepts_input(p) || button < 1u || button > 3u)
    return false;
  if (button == 1u)
    return inlay_portal_inject_pointer(p, screen_pos, down);
  const EtchRect &v = p->geometry.visible_bounds;
  const bool inside = screen_pos.x >= v.x && screen_pos.x < v.x + v.w &&
                      screen_pos.y >= v.y && screen_pos.y < v.y + v.h;
  if (!inside) return false;
  EtchVec2 local = {};
  if (!etch_screen_to_element_local(p->id, screen_pos, &local))
    return false;
  if (p->view_width <= 0.f || p->view_height <= 0.f) return false;
  const EtchVec2 scaled = {local.x * (float(p->width) / p->view_width),
                           local.y * (float(p->height) / p->view_height)};
  if (down) p->focused = true;
  if (p->is_webview2)
    return inlay_portal_webview2_inject_mouse_button(p->backend, scaled, button, down);
  return true;
}
INLAY_API const InlayPortalBackendVTable *inlay_portal_native_backend(void) {
  if (const auto *wv2 = inlay_portal_webview2_backend()) return wv2;
  if (const auto *mac = inlay_portal_macos_backend()) return mac;
  if (const auto *gtk = inlay_portal_gtk_backend()) return gtk;
  return nullptr;
}
}
EtchContext *inlay_portal_context(const InlayPortal *p) noexcept {
  return p ? p->context : nullptr;
}
