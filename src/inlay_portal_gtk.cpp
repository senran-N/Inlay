// Native Linux WebKitGTK underlay portal backend.
// Enabled via INLAY_ENABLE_PORTAL_GTK=ON on Linux; provides clean nullptr stub elsewhere.
#include "inlay_internal.h"
#if defined(__linux__) && defined(INLAY_ENABLE_PORTAL_GTK) && INLAY_ENABLE_PORTAL_GTK
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

struct InlayPortalHostHooks {
  InlayPortalNavDecision (*on_nav_check)(void *ctx, EtchStr url, bool is_new_window) noexcept;
  void (*on_web_message)(void *ctx, EtchStr msg) noexcept;
  bool (*on_perm_check)(void *ctx, EtchStr origin, EtchStr perm_kind) noexcept;
  void (*on_title_changed)(void *ctx, EtchStr title) noexcept;
  void (*on_loading_changed)(void *ctx, bool is_loading) noexcept;
  void (*on_source_changed)(void *ctx, EtchStr url) noexcept;
  void (*on_favicon_changed)(void *ctx, EtchStr favicon_url) noexcept;
  void *ctx;
};

struct GtkPortal {
  GtkWidget *parent;
  WebKitWebView *webview;
  InlayPortalHostHooks hooks;
  bool closed;
};

namespace {
void on_load_changed(WebKitWebView *view, WebKitLoadEvent ev, gpointer ud) {
  auto *p = static_cast<GtkPortal *>(ud);
  if (!p || p->closed) return;
  if (p->hooks.on_loading_changed) {
    p->hooks.on_loading_changed(p->hooks.ctx, ev != WEBKIT_LOAD_FINISHED);
  }
  if (ev == WEBKIT_LOAD_COMMITTED || ev == WEBKIT_LOAD_FINISHED) {
    const char *uri = webkit_web_view_get_uri(view);
    if (uri && p->hooks.on_source_changed) {
      p->hooks.on_source_changed(p->hooks.ctx, {uri, static_cast<uint32_t>(std::strlen(uri)), false});
    }
    const char *title = webkit_web_view_get_title(view);
    if (title && p->hooks.on_title_changed) {
      p->hooks.on_title_changed(p->hooks.ctx, {title, static_cast<uint32_t>(std::strlen(title)), false});
    }
  }
}
gboolean on_permission_request(WebKitWebView *view, WebKitPermissionRequest *req, gpointer ud) {
  auto *p = static_cast<GtkPortal *>(ud);
  if (!p || p->closed || !p->hooks.on_perm_check) return FALSE;
  const char *uri = webkit_web_view_get_uri(view);
  const EtchStr orig = {uri ? uri : "", uri ? static_cast<uint32_t>(std::strlen(uri)) : 0u, false};
  const char *kind = WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req) ? "media"
                   : WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req) ? "notifications"
                   : WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req) ? "geolocation" : "unknown";
  const bool allow = p->hooks.on_perm_check(p->hooks.ctx, orig, {kind, static_cast<uint32_t>(std::strlen(kind)), true});
  if (allow) webkit_permission_request_allow(req);
  else webkit_permission_request_deny(req);
  return TRUE;
}
void *gtk_create(void *ud, uint32_t w, uint32_t h) {
  if (!ud) return nullptr;
  InlayPortalGtkOptions opts = inlay_portal_gtk_options_default();
  if (!inlay_read_abi_struct(static_cast<const InlayPortalGtkOptions *>(ud), &opts) || !opts.parent_gtk_widget)
    return nullptr;
  auto *p = static_cast<GtkPortal *>(std::calloc(1u, sizeof(GtkPortal)));
  if (!p) return nullptr;
  p->parent = static_cast<GtkWidget *>(opts.parent_gtk_widget);
  p->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
  if (!p->webview) { std::free(p); return nullptr; }
  gtk_widget_set_size_request(GTK_WIDGET(p->webview), static_cast<int>(w), static_cast<int>(h));
  g_signal_connect(p->webview, "load-changed", G_CALLBACK(on_load_changed), p);
  g_signal_connect(p->webview, "permission-request", G_CALLBACK(on_permission_request), p);
  if (GTK_IS_FIXED(p->parent)) {
    gtk_fixed_put(GTK_FIXED(p->parent), GTK_WIDGET(p->webview), 0, 0);
  }
  return p;
}
void gtk_destroy(void *b) {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p) return;
  p->closed = true;
  if (p->webview) {
    g_signal_handlers_disconnect_by_data(p->webview, p);
    if (p->parent && GTK_IS_FIXED(p->parent)) {
      gtk_fixed_remove(GTK_FIXED(p->parent), GTK_WIDGET(p->webview));
    }
    p->webview = nullptr;
  }
  std::free(p);
}
bool gtk_load_url(void *b, EtchStr s) {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || !p->webview || !s.chars || !s.length) return false;
  char *z = static_cast<char *>(std::malloc(size_t(s.length) + 1u));
  if (!z) return false;
  std::memcpy(z, s.chars, s.length); z[s.length] = '\0';
  webkit_web_view_load_uri(p->webview, z);
  std::free(z);
  return true;
}
bool gtk_load_html(void *b, EtchStr s) {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || !p->webview || !s.chars) return false;
  char *z = static_cast<char *>(std::malloc(size_t(s.length) + 1u));
  if (!z) return false;
  std::memcpy(z, s.chars, s.length); z[s.length] = '\0';
  webkit_web_view_load_html(p->webview, z, nullptr);
  std::free(z);
  return true;
}
bool gtk_resize(void *, uint32_t, uint32_t) { return true; }
uint32_t gtk_poll(void *, InlayPortalDirtyRect *, uint32_t) { return 0u; }
void gtk_ack(void *, bool) {}
void gtk_bounds(void *b, EtchRect r, EtchRect clip) {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || !p->webview) return;
  const bool vis = clip.w > 0.f && clip.h > 0.f && r.w > 0.f && r.h > 0.f;
  gtk_widget_set_visible(GTK_WIDGET(p->webview), vis ? TRUE : FALSE);
  if (vis && p->parent && GTK_IS_FIXED(p->parent)) {
    gtk_fixed_move(GTK_FIXED(p->parent), GTK_WIDGET(p->webview), r.x, r.y);
    gtk_widget_set_size_request(GTK_WIDGET(p->webview), static_cast<int>(r.w), static_cast<int>(r.h));
  }
}
const InlayPortalBackendVTable gtk_api = {
    gtk_create, gtk_destroy, gtk_load_url, gtk_load_html, gtk_resize,
    gtk_poll, nullptr, nullptr, nullptr, nullptr,
    nullptr, gtk_bounds, gtk_ack, nullptr};
} // namespace

bool inlay_portal_gtk_nav_action(void *b, int action) noexcept {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || p->closed || !p->webview) return false;
  if (action == 0) { webkit_web_view_go_back(p->webview); return true; }
  if (action == 1) { webkit_web_view_go_forward(p->webview); return true; }
  if (action == 2) { webkit_web_view_reload(p->webview); return true; }
  return false;
}
bool inlay_portal_gtk_eval_script(void *b, EtchStr s) noexcept {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || p->closed || !p->webview || !s.chars || !s.length) return false;
  webkit_web_view_evaluate_javascript(p->webview, s.chars, static_cast<gssize>(s.length), nullptr, nullptr, nullptr, nullptr, nullptr);
  return true;
}
bool inlay_portal_gtk_add_init_script(void *b, EtchStr s) noexcept {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || p->closed || !p->webview || !s.chars || !s.length) return false;
  char *z = static_cast<char *>(std::malloc(size_t(s.length) + 1u));
  if (!z) return false;
  std::memcpy(z, s.chars, s.length); z[s.length] = '\0';
  WebKitUserScript *us = webkit_user_script_new(z, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(p->webview);
  if (ucm && us) webkit_user_content_manager_add_script(ucm, us);
  if (us) webkit_user_script_unref(us);
  std::free(z);
  return true;
}
bool inlay_portal_gtk_set_zoom(void *b, float z) noexcept {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || p->closed || !p->webview || !std::isfinite(z) || z <= 0.f) return false;
  webkit_web_view_set_zoom_level(p->webview, static_cast<gdouble>(z));
  return true;
}
bool inlay_portal_gtk_open_devtools(void *b) noexcept {
  auto *p = static_cast<GtkPortal *>(b);
  if (!p || p->closed || !p->webview) return false;
  WebKitSettings *st = webkit_web_view_get_settings(p->webview);
  if (st) webkit_settings_set_enable_developer_extras(st, TRUE);
  WebKitWebInspector *insp = webkit_web_view_get_inspector(p->webview);
  if (insp) webkit_web_inspector_show(insp);
  return insp != nullptr;
}
void inlay_portal_gtk_set_hooks(void *b, const InlayPortalHostHooks *hooks) noexcept {
  auto *p = static_cast<GtkPortal *>(b);
  if (p) p->hooks = hooks ? *hooks : InlayPortalHostHooks{};
}
extern "C" INLAY_API const InlayPortalBackendVTable *inlay_portal_gtk_backend(void) {
  return &gtk_api;
}
#else
struct InlayPortalHostHooks;
bool inlay_portal_gtk_nav_action(void *, int) noexcept { return false; }
bool inlay_portal_gtk_eval_script(void *, EtchStr) noexcept { return false; }
bool inlay_portal_gtk_add_init_script(void *, EtchStr) noexcept { return false; }
bool inlay_portal_gtk_set_zoom(void *, float) noexcept { return false; }
bool inlay_portal_gtk_open_devtools(void *) noexcept { return false; }
void inlay_portal_gtk_set_hooks(void *, const InlayPortalHostHooks *) noexcept {}
extern "C" INLAY_API const InlayPortalBackendVTable *inlay_portal_gtk_backend(void) {
  return nullptr;
}
#endif
