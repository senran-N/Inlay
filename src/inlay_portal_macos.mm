// Native macOS WKWebView underlay portal backend.
// Compiled as Objective-C++ when INLAY_ENABLE_PORTAL_MACOS=ON on Apple platforms;
// compiled as a lightweight C++ stub elsewhere so inlay_portal_macos_backend() links cleanly.
#include "inlay_internal.h"
#if defined(__APPLE__) && defined(INLAY_ENABLE_PORTAL_MACOS) && INLAY_ENABLE_PORTAL_MACOS
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
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

struct MacPortal;

@interface InlayWKDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property (nonatomic, assign) MacPortal *owner;
@end

struct MacPortal {
  NSView *parent;
  WKWebView *webview;
  InlayWKDelegate *delegate;
  InlayPortalHostHooks hooks;
  float zoom_factor;
  float radius_logical;
  bool closed;
};

@implementation InlayWKDelegate
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action
decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
  (void)webView;
  MacPortal *p = self.owner;
  if (!p || p->closed || !p->hooks.on_nav_check) {
    decisionHandler(WKNavigationActionPolicyAllow);
    return;
  }
  NSString *urlStr = action.request.URL.absoluteString ?: @"";
  const char *utf8 = urlStr.UTF8String;
  const EtchStr s = {utf8 ? utf8 : "", utf8 ? static_cast<uint32_t>(std::strlen(utf8)) : 0u, false};
  const bool isNew = (action.targetFrame == nil);
  const InlayPortalNavDecision d = p->hooks.on_nav_check(p->hooks.ctx, s, isNew);
  decisionHandler(d == INLAY_PORTAL_NAV_ALLOW ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)nav {
  (void)webView; (void)nav;
  MacPortal *p = self.owner;
  if (p && !p->closed && p->hooks.on_loading_changed) p->hooks.on_loading_changed(p->hooks.ctx, true);
}
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)nav {
  (void)nav;
  MacPortal *p = self.owner;
  if (!p || p->closed) return;
  if (p->hooks.on_loading_changed) p->hooks.on_loading_changed(p->hooks.ctx, false);
  if (p->hooks.on_title_changed && webView.title) {
    const char *u = webView.title.UTF8String;
    if (u) p->hooks.on_title_changed(p->hooks.ctx, {u, static_cast<uint32_t>(std::strlen(u)), false});
  }
  if (p->hooks.on_source_changed && webView.URL.absoluteString) {
    const char *u = webView.URL.absoluteString.UTF8String;
    if (u) p->hooks.on_source_changed(p->hooks.ctx, {u, static_cast<uint32_t>(std::strlen(u)), false});
  }
  if (p->hooks.on_favicon_changed) {
    [webView evaluateJavaScript:@"(function(){var l=document.querySelector('link[rel*=\"icon\"]');return l&&l.href?l.href:'';})()"
              completionHandler:^(id res, NSError *err) {
                (void)err;
                MacPortal *mp = self.owner;
                if (!mp || mp->closed || !mp->hooks.on_favicon_changed) return;
                if ([res isKindOfClass:[NSString class]] && ((NSString *)res).length > 0) {
                  const char *fu = ((NSString *)res).UTF8String;
                  if (fu) mp->hooks.on_favicon_changed(mp->hooks.ctx, {fu, static_cast<uint32_t>(std::strlen(fu)), false});
                }
              }];
  }
}
- (void)webView:(WKWebView *)webView requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
initiatedByFrame:(WKFrameInfo *)frame type:(WKMediaCaptureType)type
decisionHandler:(void (^)(WKPermissionDecision))decisionHandler API_AVAILABLE(macos(12.0)) {
  (void)webView; (void)frame;
  MacPortal *p = self.owner;
  if (!p || p->closed || !p->hooks.on_perm_check) {
    decisionHandler(WKPermissionDecisionPrompt);
    return;
  }
  NSString *origStr = [NSString stringWithFormat:@"%@://%@", origin.protocol ?: @"", origin.host ?: @""];
  const char *ou = origStr.UTF8String;
  const EtchStr orig = {ou ? ou : "", ou ? static_cast<uint32_t>(std::strlen(ou)) : 0u, false};
  const char *kind = (type == WKMediaCaptureTypeCamera) ? "camera"
                   : (type == WKMediaCaptureTypeMicrophone) ? "microphone" : "camera+microphone";
  const bool allow = p->hooks.on_perm_check(p->hooks.ctx, orig, {kind, static_cast<uint32_t>(std::strlen(kind)), true});
  decisionHandler(allow ? WKPermissionDecisionGrant : WKPermissionDecisionDeny);
}
- (void)userContentController:(WKUserContentController *)ctrl didReceiveScriptMessage:(WKScriptMessage *)message {
  (void)ctrl;
  MacPortal *p = self.owner;
  if (!p || p->closed || !p->hooks.on_web_message) return;
  NSString *str = [message.body isKindOfClass:[NSString class]] ? (NSString *)message.body : [NSString stringWithFormat:@"%@", message.body];
  const char *u = str.UTF8String;
  if (u) p->hooks.on_web_message(p->hooks.ctx, {u, static_cast<uint32_t>(std::strlen(u)), false});
}
@end

namespace {
void *mac_create(void *ud, uint32_t w, uint32_t h) {
  if (!ud) return nullptr;
  InlayPortalMacOSOptions opts = inlay_portal_macos_options_default();
  if (!inlay_read_abi_struct(static_cast<const InlayPortalMacOSOptions *>(ud), &opts) || !opts.parent_nsview)
    return nullptr;
  auto *p = static_cast<MacPortal *>(std::calloc(1u, sizeof(MacPortal)));
  if (!p) return nullptr;
  p->parent = (__bridge NSView *)opts.parent_nsview;
  p->zoom_factor = 1.0f;
  WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
  p->delegate = [[InlayWKDelegate alloc] init];
  p->delegate.owner = p;
  [cfg.userContentController addScriptMessageHandler:p->delegate name:@"inlay"];
  if (@available(macOS 10.11, *)) {
    [cfg.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
  }
  p->webview = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, w, h) configuration:cfg];
  p->webview.navigationDelegate = p->delegate;
  p->webview.UIDelegate = p->delegate;
  p->webview.wantsLayer = YES;
  if (opts.transparent_background) [p->webview setValue:@NO forKey:@"drawsBackground"];
  [p->parent addSubview:p->webview positioned:NSWindowBelow relativeTo:nil];
  return p;
}
void mac_destroy(void *b) {
  auto *p = static_cast<MacPortal *>(b);
  if (!p) return;
  p->closed = true;
  if (p->webview) {
    [p->webview.configuration.userContentController removeScriptMessageHandlerForName:@"inlay"];
    p->webview.navigationDelegate = nil;
    p->webview.UIDelegate = nil;
    [p->webview removeFromSuperview];
    p->webview = nil;
  }
  if (p->delegate) {
    p->delegate.owner = nullptr;
    p->delegate = nil;
  }
  std::free(p);
}
bool mac_load_url(void *b, EtchStr s) {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || !p->webview || !s.chars || !s.length) return false;
  NSString *ns = [[NSString alloc] initWithBytes:s.chars length:s.length encoding:NSUTF8StringEncoding];
  NSURL *url = ns ? [NSURL URLWithString:ns] : nil;
  if (!url) return false;
  [p->webview loadRequest:[NSURLRequest requestWithURL:url]];
  return true;
}
bool mac_load_html(void *b, EtchStr s) {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || !p->webview || !s.chars) return false;
  NSString *ns = [[NSString alloc] initWithBytes:s.chars length:s.length encoding:NSUTF8StringEncoding];
  if (!ns) return false;
  [p->webview loadHTMLString:ns baseURL:nil];
  return true;
}
bool mac_resize(void *, uint32_t, uint32_t) { return true; }
uint32_t mac_poll(void *, InlayPortalDirtyRect *, uint32_t) { return 0u; }
void mac_ack(void *, bool) {}
void mac_bounds(void *b, EtchRect r, EtchRect clip) {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || !p->webview || !p->parent) return;
  const BOOL vis = (clip.w > 0.f && clip.h > 0.f && r.w > 0.f && r.h > 0.f);
  p->webview.hidden = !vis;
  if (!vis) return;
  const CGFloat ph = p->parent.bounds.size.height;
  const CGFloat y = p->parent.isFlipped ? r.y : (ph - r.y - r.h);
  p->webview.frame = NSMakeRect(r.x, y, r.w, r.h);
  if (p->webview.layer) {
    p->webview.layer.cornerRadius = p->radius_logical;
    p->webview.layer.masksToBounds = (p->radius_logical > 0.f);
  }
}
void mac_radius(void *b, float rad) {
  auto *p = static_cast<MacPortal *>(b);
  if (p) p->radius_logical = (std::isfinite(rad) && rad > 0.f) ? rad : 0.f;
}
const InlayPortalBackendVTable mac_api = {
    mac_create, mac_destroy, mac_load_url, mac_load_html, mac_resize,
    mac_poll, nullptr, nullptr, nullptr, nullptr,
    nullptr, mac_bounds, mac_ack, mac_radius};
} // namespace

bool inlay_portal_macos_nav_action(void *b, int action) noexcept {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || p->closed || !p->webview) return false;
  if (action == 0) { [p->webview goBack]; return true; }
  if (action == 1) { [p->webview goForward]; return true; }
  if (action == 2) { [p->webview reload]; return true; }
  return false;
}
bool inlay_portal_macos_eval_script(void *b, EtchStr s) noexcept {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || p->closed || !p->webview || !s.chars || !s.length) return false;
  NSString *ns = [[NSString alloc] initWithBytes:s.chars length:s.length encoding:NSUTF8StringEncoding];
  if (!ns) return false;
  [p->webview evaluateJavaScript:ns completionHandler:nil];
  return true;
}
bool inlay_portal_macos_add_init_script(void *b, EtchStr s) noexcept {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || p->closed || !p->webview || !s.chars || !s.length) return false;
  NSString *ns = [[NSString alloc] initWithBytes:s.chars length:s.length encoding:NSUTF8StringEncoding];
  if (!ns) return false;
  WKUserScript *us = [[WKUserScript alloc] initWithSource:ns
                                            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                         forMainFrameOnly:NO];
  [p->webview.configuration.userContentController addUserScript:us];
  return true;
}
bool inlay_portal_macos_set_zoom(void *b, float z) noexcept {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || p->closed || !p->webview || !std::isfinite(z) || z <= 0.f) return false;
  p->zoom_factor = z;
  if (@available(macOS 11.0, *)) {
    p->webview.pageZoom = z;
    return true;
  }
  return false;
}
bool inlay_portal_macos_open_devtools(void *b) noexcept {
  auto *p = static_cast<MacPortal *>(b);
  if (!p || p->closed || !p->webview) return false;
  if (@available(macOS 13.3, *)) {
    p->webview.inspectable = YES;
  }
  [p->webview.configuration.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
  return true;
}
void inlay_portal_macos_set_hooks(void *b, const InlayPortalHostHooks *hooks) noexcept {
  auto *p = static_cast<MacPortal *>(b);
  if (p) p->hooks = hooks ? *hooks : InlayPortalHostHooks{};
}
extern "C" INLAY_API const InlayPortalBackendVTable *inlay_portal_macos_backend(void) {
  return &mac_api;
}
#else
struct InlayPortalHostHooks;
bool inlay_portal_macos_nav_action(void *, int) noexcept { return false; }
bool inlay_portal_macos_eval_script(void *, EtchStr) noexcept { return false; }
bool inlay_portal_macos_add_init_script(void *, EtchStr) noexcept { return false; }
bool inlay_portal_macos_set_zoom(void *, float) noexcept { return false; }
bool inlay_portal_macos_open_devtools(void *) noexcept { return false; }
void inlay_portal_macos_set_hooks(void *, const InlayPortalHostHooks *) noexcept {}
extern "C" INLAY_API const InlayPortalBackendVTable *inlay_portal_macos_backend(void) {
  return nullptr;
}
#endif
