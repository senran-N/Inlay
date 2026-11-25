#include "inlay_internal.h"
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
void inlay_portal_webview2_set_hooks(void *backend,
                                     const InlayPortalHostHooks *hooks) noexcept;
bool inlay_portal_webview2_set_zoom(void *backend, float zoom_factor) noexcept;
bool inlay_portal_webview2_inject_mouse_button(void *backend, EtchVec2 local_pos, uint8_t button, bool down) noexcept;
#if defined(_WIN32) && INLAY_ENABLE_PORTAL_WEBVIEW2
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <dcomp.h>
#include <WebView2.h>
// clang-format on
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
struct WebPortal;

ULONG retain(WebPortal *p) noexcept;
ULONG release(WebPortal *p) noexcept;
HRESULT on_environment_ready(WebPortal *p, HRESULT result, ICoreWebView2Environment *env) noexcept;
HRESULT on_controller_ready(WebPortal *p, HRESULT result, ICoreWebView2Controller *ctrl) noexcept;
HRESULT on_composition_ready(WebPortal *p, HRESULT result, ICoreWebView2CompositionController *comp) noexcept;
HRESULT on_navigation_starting(WebPortal *p, ICoreWebView2NavigationStartingEventArgs *args) noexcept;
HRESULT on_new_window_requested(WebPortal *p, ICoreWebView2NewWindowRequestedEventArgs *args) noexcept;
HRESULT on_web_message_received(WebPortal *p, ICoreWebView2WebMessageReceivedEventArgs *args) noexcept;
HRESULT on_permission_requested(WebPortal *p, ICoreWebView2PermissionRequestedEventArgs *args) noexcept;
HRESULT on_document_title_changed(WebPortal *p) noexcept;
HRESULT on_navigation_completed(WebPortal *p) noexcept;
HRESULT on_source_changed(WebPortal *p) noexcept;
#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
HRESULT on_favicon_changed(WebPortal *p) noexcept;
#endif

struct EnvironmentHandler final : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
  WebPortal *owner;
  explicit EnvironmentHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override {
    return on_environment_ready(owner, result, env);
  }
};

struct ControllerHandler final : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
  WebPortal *owner;
  explicit ControllerHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *ctrl) override {
    return on_controller_ready(owner, result, ctrl);
  }
};

struct CompositionHandler final : ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler {
  WebPortal *owner;
  explicit CompositionHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2CompositionController *comp) override {
    return on_composition_ready(owner, result, comp);
  }
};

struct NavStartingHandler final : ICoreWebView2NavigationStartingEventHandler {
  WebPortal *owner;
  explicit NavStartingHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2NavigationStartingEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2NavigationStartingEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                   ICoreWebView2NavigationStartingEventArgs *args) override {
    return on_navigation_starting(owner, args);
  }
};

struct NewWindowHandler final : ICoreWebView2NewWindowRequestedEventHandler {
  WebPortal *owner;
  explicit NewWindowHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2NewWindowRequestedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2NewWindowRequestedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                   ICoreWebView2NewWindowRequestedEventArgs *args) override {
    return on_new_window_requested(owner, args);
  }
};

struct WebMessageHandler final : ICoreWebView2WebMessageReceivedEventHandler {
  WebPortal *owner;
  explicit WebMessageHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2WebMessageReceivedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                   ICoreWebView2WebMessageReceivedEventArgs *args) override {
    return on_web_message_received(owner, args);
  }
};

struct PermissionHandler final : ICoreWebView2PermissionRequestedEventHandler {
  WebPortal *owner;
  explicit PermissionHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2PermissionRequestedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2PermissionRequestedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                   ICoreWebView2PermissionRequestedEventArgs *args) override {
    return on_permission_requested(owner, args);
  }
};

struct TitleChangedHandler final : ICoreWebView2DocumentTitleChangedEventHandler {
  WebPortal *owner;
  explicit TitleChangedHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2DocumentTitleChangedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2DocumentTitleChangedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, IUnknown *) override {
    return on_document_title_changed(owner);
  }
};

struct NavCompletedHandler final : ICoreWebView2NavigationCompletedEventHandler {
  WebPortal *owner;
  explicit NavCompletedHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2NavigationCompletedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2NavigationCompletedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                   ICoreWebView2NavigationCompletedEventArgs *) override {
    return on_navigation_completed(owner);
  }
};

struct SourceChangedHandler final : ICoreWebView2SourceChangedEventHandler {
  WebPortal *owner;
  explicit SourceChangedHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2SourceChangedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2SourceChangedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                   ICoreWebView2SourceChangedEventArgs *) override {
    return on_source_changed(owner);
  }
};

#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
struct FaviconChangedHandler final : ICoreWebView2FaviconChangedEventHandler {
  WebPortal *owner;
  explicit FaviconChangedHandler(WebPortal *o) noexcept : owner(o) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(id, IID_IUnknown) &&
        !IsEqualIID(id, IID_ICoreWebView2FaviconChangedEventHandler))
      return E_NOINTERFACE;
    *out = static_cast<ICoreWebView2FaviconChangedEventHandler *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return retain(owner); }
  ULONG STDMETHODCALLTYPE Release() override { return release(owner); }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, IUnknown *) override {
    return on_favicon_changed(owner);
  }
};
#endif

struct WebPortal {
  EnvironmentHandler environment_handler;
  ControllerHandler controller_handler;
  CompositionHandler composition_handler;
  NavStartingHandler nav_starting_handler;
  NewWindowHandler new_window_handler;
  WebMessageHandler web_message_handler;
  PermissionHandler perm_requested_handler;
  TitleChangedHandler title_changed_handler;
  NavCompletedHandler nav_completed_handler;
  SourceChangedHandler source_changed_handler;
#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
  FaviconChangedHandler favicon_changed_handler;
  EventRegistrationToken favicon_changed_token;
  bool has_favicon_changed;
#endif
  volatile LONG references;
  DWORD com_thread;
  HWND parent;
  HWND child_hwnd;
  HMODULE loader;
  HMODULE dcomp_dll;
  IDCompositionDevice *dcomp_device;
  IDCompositionTarget *dcomp_target;
  IDCompositionVisual *dcomp_root;
  IDCompositionVisual *dcomp_webview;
  IDCompositionRectangleClip *dcomp_clip;
  ICoreWebView2Environment *environment;
  ICoreWebView2CompositionController *comp_controller;
  ICoreWebView2Controller *controller;
  ICoreWebView2 *web;
  EventRegistrationToken nav_starting_token;
  EventRegistrationToken new_window_token;
  EventRegistrationToken web_message_token;
  EventRegistrationToken perm_requested_token;
  EventRegistrationToken title_changed_token;
  EventRegistrationToken nav_completed_token;
  EventRegistrationToken source_changed_token;
  bool has_nav_starting, has_new_window, has_web_message, has_perm_requested;
  bool has_title_changed, has_nav_completed, has_source_changed;
  InlayPortalHostHooks hooks;
  wchar_t *navigation;
  wchar_t **pending_init_scripts;
  uint32_t pending_init_count, pending_init_cap;
  double pending_zoom;
  bool html, closed, com_initialized, disable_dcomp, using_dcomp;
  void (*completed)(void *, int32_t);
  void *user;
  uint32_t portal_width, portal_height;
  EtchRect bounds_logical, visible_logical;
  float radius_logical;
  RECT bounds;
  RECT clip_rect;
  bool visible, clipped, has_rgn;
  bool left_down, has_mouse_pt;
  POINT last_mouse_pt;

  WebPortal() noexcept
      : environment_handler(this),
        controller_handler(this),
        composition_handler(this),
        nav_starting_handler(this),
        new_window_handler(this),
        web_message_handler(this),
        perm_requested_handler(this),
        title_changed_handler(this),
        nav_completed_handler(this),
        source_changed_handler(this),
#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
        favicon_changed_handler(this),
        favicon_changed_token{},
        has_favicon_changed(false),
#endif
        references(1),
        com_thread(0),
        parent(nullptr),
        child_hwnd(nullptr),
        loader(nullptr),
        dcomp_dll(nullptr),
        dcomp_device(nullptr),
        dcomp_target(nullptr),
        dcomp_root(nullptr),
        dcomp_webview(nullptr),
        dcomp_clip(nullptr),
        environment(nullptr),
        comp_controller(nullptr),
        controller(nullptr),
        web(nullptr),
        nav_starting_token{},
        new_window_token{},
        web_message_token{},
        perm_requested_token{},
        title_changed_token{},
        nav_completed_token{},
        source_changed_token{},
        has_nav_starting(false),
        has_new_window(false),
        has_web_message(false),
        has_perm_requested(false),
        has_title_changed(false),
        has_nav_completed(false),
        has_source_changed(false),
        hooks{},
        navigation(nullptr),
        pending_init_scripts(nullptr),
        pending_init_count(0),
        pending_init_cap(0),
        pending_zoom(1.0),
        html(false),
        closed(false),
        com_initialized(false),
        disable_dcomp(false),
        using_dcomp(false),
        completed(nullptr),
        user(nullptr),
        portal_width(0),
        portal_height(0),
        bounds_logical{},
        visible_logical{},
        radius_logical(0.f),
        bounds{},
        clip_rect{},
        visible(false),
        clipped(false),
        has_rgn(false),
        left_down(false),
        has_mouse_pt(false),
        last_mouse_pt{0, 0} {}
};

void release_dcomp(WebPortal *p) noexcept {
  if (p->dcomp_webview) {
    p->dcomp_webview->SetClip(static_cast<IDCompositionClip *>(nullptr));
    p->dcomp_webview->Release();
    p->dcomp_webview = nullptr;
  }
  if (p->dcomp_clip) {
    p->dcomp_clip->Release();
    p->dcomp_clip = nullptr;
  }
  if (p->dcomp_root) {
    p->dcomp_root->Release();
    p->dcomp_root = nullptr;
  }
  if (p->dcomp_target) {
    p->dcomp_target->SetRoot(nullptr);
    p->dcomp_target->Release();
    p->dcomp_target = nullptr;
  }
  if (p->dcomp_device) {
    p->dcomp_device->Commit();
    p->dcomp_device->Release();
    p->dcomp_device = nullptr;
  }
  if (p->dcomp_dll) {
    FreeLibrary(p->dcomp_dll);
    p->dcomp_dll = nullptr;
  }
  p->using_dcomp = false;
}

bool init_dcomp_device(WebPortal *p) noexcept {
  if (p->disable_dcomp || !p->parent)
    return false;
  if (p->dcomp_device)
    return true;
  HMODULE dll = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll)
    return false;
  using CreateDeviceFn = HRESULT(STDAPICALLTYPE *)(IDXGIDevice *, REFIID, void **);
  const auto proc = GetProcAddress(dll, "DCompositionCreateDevice");
  CreateDeviceFn create_device = nullptr;
  static_assert(sizeof(create_device) == sizeof(proc));
  std::memcpy(&create_device, &proc, sizeof(create_device));
  if (!create_device) {
    FreeLibrary(dll);
    return false;
  }
  p->dcomp_dll = dll;
  const HRESULT hr = create_device(nullptr, __uuidof(IDCompositionDevice),
                                   reinterpret_cast<void **>(&p->dcomp_device));
  if (FAILED(hr) || !p->dcomp_device) {
    release_dcomp(p);
    return false;
  }
  return true;
}

bool init_dcomp_target(WebPortal *p) noexcept {
  if (!p->dcomp_device || !p->parent)
    return false;
  HRESULT hr = p->dcomp_device->CreateTargetForHwnd(p->parent, TRUE, &p->dcomp_target);
  if (FAILED(hr) || !p->dcomp_target) {
    release_dcomp(p);
    return false;
  }
  hr = p->dcomp_device->CreateVisual(&p->dcomp_root);
  if (FAILED(hr) || !p->dcomp_root) {
    release_dcomp(p);
    return false;
  }
  hr = p->dcomp_device->CreateVisual(&p->dcomp_webview);
  if (FAILED(hr) || !p->dcomp_webview) {
    release_dcomp(p);
    return false;
  }
  hr = p->dcomp_device->CreateRectangleClip(&p->dcomp_clip);
  if (FAILED(hr) || !p->dcomp_clip) {
    release_dcomp(p);
    return false;
  }
  if (FAILED(p->dcomp_root->AddVisual(p->dcomp_webview, FALSE, nullptr)) ||
      FAILED(p->dcomp_target->SetRoot(p->dcomp_root)) ||
      FAILED(p->dcomp_device->Commit())) {
    release_dcomp(p);
    return false;
  }
  return true;
}

ULONG retain(WebPortal *p) noexcept {
  return static_cast<ULONG>(InterlockedIncrement(&p->references));
}

ULONG release(WebPortal *p) noexcept {
  const ULONG n = static_cast<ULONG>(InterlockedDecrement(&p->references));
  if (!n) {
    release_dcomp(p);
    if (p->environment) {
      p->environment->Release();
      p->environment = nullptr;
    }
    std::free(p->navigation);
    for (uint32_t i = 0u; i < p->pending_init_count; ++i)
      std::free(p->pending_init_scripts[i]);
    std::free(p->pending_init_scripts);
    p->pending_init_scripts = nullptr;
    p->pending_init_count = 0u;
    p->pending_init_cap = 0u;
    if (p->loader)
      FreeLibrary(p->loader);
    if (p->com_initialized && GetCurrentThreadId() == p->com_thread)
      CoUninitialize();
    std::free(p);
  }
  return n;
}

HWND find_webview_child(WebPortal *p) noexcept {
  if (p->child_hwnd && IsWindow(p->child_hwnd) && GetParent(p->child_hwnd) == p->parent)
    return p->child_hwnd;
  HWND first = nullptr;
  int count = 0;
  for (HWND child = GetWindow(p->parent, GW_CHILD); child;
       child = GetWindow(child, GW_HWNDNEXT)) {
    ++count;
    if (!first)
      first = child;
    RECT wr = {};
    if (GetWindowRect(child, &wr)) {
      MapWindowPoints(nullptr, p->parent, reinterpret_cast<POINT *>(&wr), 2);
      if (wr.left == p->bounds.left && wr.top == p->bounds.top &&
          wr.right == p->bounds.right && wr.bottom == p->bounds.bottom) {
        p->child_hwnd = child;
        return child;
      }
    }
  }
  if (count == 1)
    p->child_hwnd = first;
  return p->child_hwnd;
}

void apply_bounds(WebPortal *p) noexcept {
  if (!p->controller)
    return;
  const float scale = static_cast<float>(GetDpiForWindow(p->parent)) / 96.f;
  const InlayDCompClipGeom geom =
      inlay_dcomp_compute_clip(p->bounds_logical, p->visible_logical, p->radius_logical, scale);
  p->bounds = {static_cast<LONG>(geom.offset_x), static_cast<LONG>(geom.offset_y),
               static_cast<LONG>(geom.offset_x + geom.width),
               static_cast<LONG>(geom.offset_y + geom.height)};
  p->clip_rect = {static_cast<LONG>(geom.offset_x + geom.left),
                  static_cast<LONG>(geom.offset_y + geom.top),
                  static_cast<LONG>(geom.offset_x + geom.right),
                  static_cast<LONG>(geom.offset_y + geom.bottom)};
  p->visible = geom.visible;
  p->clipped = geom.clipped;

  if (p->using_dcomp && p->dcomp_device && p->dcomp_webview) {
    const RECT local_bounds = {0, 0, static_cast<LONG>(geom.width),
                               static_cast<LONG>(geom.height)};
    p->controller->put_Bounds(local_bounds);
    p->controller->put_IsVisible(geom.visible ? TRUE : FALSE);
    p->dcomp_webview->SetOffsetX(geom.offset_x);
    p->dcomp_webview->SetOffsetY(geom.offset_y);
    if (geom.need_clip && p->dcomp_clip) {
      p->dcomp_clip->SetLeft(geom.left);
      p->dcomp_clip->SetTop(geom.top);
      p->dcomp_clip->SetRight(geom.right);
      p->dcomp_clip->SetBottom(geom.bottom);
      p->dcomp_clip->SetTopLeftRadiusX(geom.tl_r);
      p->dcomp_clip->SetTopLeftRadiusY(geom.tl_r);
      p->dcomp_clip->SetTopRightRadiusX(geom.tr_r);
      p->dcomp_clip->SetTopRightRadiusY(geom.tr_r);
      p->dcomp_clip->SetBottomRightRadiusX(geom.br_r);
      p->dcomp_clip->SetBottomRightRadiusY(geom.br_r);
      p->dcomp_clip->SetBottomLeftRadiusX(geom.bl_r);
      p->dcomp_clip->SetBottomLeftRadiusY(geom.bl_r);
      p->dcomp_webview->SetClip(p->dcomp_clip);
    } else {
      p->dcomp_webview->SetClip(static_cast<IDCompositionClip *>(nullptr));
    }
    p->dcomp_device->Commit();
    return;
  }

  p->controller->put_Bounds(p->bounds);
  p->controller->put_IsVisible(p->visible ? TRUE : FALSE);
  HWND child = find_webview_child(p);
  if (!child)
    return;
  if (geom.visible && geom.need_clip) {
    HRGN rgn = nullptr;
    if (!geom.clipped && geom.tl_r > 0.5f) {
      const int diam = static_cast<int>(std::lround(geom.tl_r * 2.f));
      rgn = CreateRoundRectRgn(0, 0, static_cast<int>(geom.width) + 1,
                               static_cast<int>(geom.height) + 1, diam, diam);
    } else {
      rgn = CreateRectRgn(static_cast<int>(geom.left), static_cast<int>(geom.top),
                          static_cast<int>(geom.right), static_cast<int>(geom.bottom));
    }
    if (rgn) {
      if (SetWindowRgn(child, rgn, TRUE))
        p->has_rgn = true;
      else
        DeleteObject(rgn);
    }
  } else if (p->has_rgn) {
    SetWindowRgn(child, nullptr, TRUE);
    p->has_rgn = false;
  }
}

wchar_t *wide(const char *bytes, uint32_t length) noexcept {
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                                    static_cast<int>(length), nullptr, 0);
  if (n <= 0)
    return nullptr;
  auto *out =
      static_cast<wchar_t *>(std::malloc((size_t(n) + 1) * sizeof(wchar_t)));
  if (!out)
    return nullptr;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                      static_cast<int>(length), out, n);
  out[n] = 0;
  return out;
}

char *narrow_utf8(LPCWSTR ws, uint32_t *out_len) noexcept {
  *out_len = 0u;
  if (!ws || !ws[0])
    return nullptr;
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws, -1,
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 1)
    return nullptr;
  auto *out = static_cast<char *>(std::malloc(static_cast<size_t>(n)));
  if (!out)
    return nullptr;
  const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws, -1,
                                          out, n, nullptr, nullptr);
  if (written <= 1) {
    std::free(out);
    return nullptr;
  }
  out[written - 1] = '\0';
  *out_len = static_cast<uint32_t>(written - 1);
  return out;
}

const char *permission_kind_name(COREWEBVIEW2_PERMISSION_KIND kind) noexcept {
  switch (kind) {
  case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:
    return "microphone";
  case COREWEBVIEW2_PERMISSION_KIND_CAMERA:
    return "camera";
  case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
    return "geolocation";
  case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
    return "notifications";
  case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS:
    return "other_sensors";
  case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ:
    return "clipboard_read";
  case COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS:
    return "multiple_automatic_downloads";
  case COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE:
    return "file_read_write";
  case COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY:
    return "autoplay";
  case COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS:
    return "local_fonts";
  case COREWEBVIEW2_PERMISSION_KIND_MIDI_SYSTEM_EXCLUSIVE_MESSAGES:
    return "midi_sysex";
  case COREWEBVIEW2_PERMISSION_KIND_WINDOW_MANAGEMENT:
    return "window_management";
  case COREWEBVIEW2_PERMISSION_KIND_PERSISTENT_STORAGE:
    return "persistent_storage";
  case COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION:
  default:
    return "unknown";
  }
}

void attach_web_events(WebPortal *p) noexcept {
  if (!p->web)
    return;
  if (p->pending_init_count > 0u) {
    for (uint32_t i = 0u; i < p->pending_init_count; ++i) {
      if (p->pending_init_scripts[i]) {
        (void)p->web->AddScriptToExecuteOnDocumentCreated(p->pending_init_scripts[i], nullptr);
        std::free(p->pending_init_scripts[i]);
      }
    }
    std::free(p->pending_init_scripts);
    p->pending_init_scripts = nullptr;
    p->pending_init_count = 0u;
    p->pending_init_cap = 0u;
  }
  if (!p->has_nav_starting &&
      SUCCEEDED(p->web->add_NavigationStarting(&p->nav_starting_handler,
                                               &p->nav_starting_token)))
    p->has_nav_starting = true;
  if (!p->has_new_window &&
      SUCCEEDED(p->web->add_NewWindowRequested(&p->new_window_handler,
                                               &p->new_window_token)))
    p->has_new_window = true;
  if (!p->has_web_message &&
      SUCCEEDED(p->web->add_WebMessageReceived(&p->web_message_handler,
                                               &p->web_message_token)))
    p->has_web_message = true;
  if (!p->has_perm_requested &&
      SUCCEEDED(p->web->add_PermissionRequested(&p->perm_requested_handler,
                                                &p->perm_requested_token)))
    p->has_perm_requested = true;
  if (!p->has_title_changed &&
      SUCCEEDED(p->web->add_DocumentTitleChanged(&p->title_changed_handler,
                                                 &p->title_changed_token)))
    p->has_title_changed = true;
  if (!p->has_nav_completed &&
      SUCCEEDED(p->web->add_NavigationCompleted(&p->nav_completed_handler,
                                                &p->nav_completed_token)))
    p->has_nav_completed = true;
  if (!p->has_source_changed &&
      SUCCEEDED(p->web->add_SourceChanged(&p->source_changed_handler,
                                          &p->source_changed_token)))
    p->has_source_changed = true;
#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
  if (!p->has_favicon_changed) {
    ICoreWebView2_15 *web15 = nullptr;
    if (SUCCEEDED(p->web->QueryInterface(IID_ICoreWebView2_15, reinterpret_cast<void **>(&web15))) && web15) {
      if (SUCCEEDED(web15->add_FaviconChanged(&p->favicon_changed_handler, &p->favicon_changed_token))) {
        p->has_favicon_changed = true;
      }
      web15->Release();
    }
  }
#endif
  if (p->controller && p->pending_zoom > 0.0 && p->pending_zoom != 1.0)
    (void)p->controller->put_ZoomFactor(p->pending_zoom);
}

void detach_web_events(WebPortal *p) noexcept {
  if (!p->web)
    return;
  if (p->has_nav_starting) {
    p->web->remove_NavigationStarting(p->nav_starting_token);
    p->has_nav_starting = false;
  }
  if (p->has_new_window) {
    p->web->remove_NewWindowRequested(p->new_window_token);
    p->has_new_window = false;
  }
  if (p->has_web_message) {
    p->web->remove_WebMessageReceived(p->web_message_token);
    p->has_web_message = false;
  }
  if (p->has_perm_requested) {
    p->web->remove_PermissionRequested(p->perm_requested_token);
    p->has_perm_requested = false;
  }
  if (p->has_title_changed) {
    p->web->remove_DocumentTitleChanged(p->title_changed_token);
    p->has_title_changed = false;
  }
  if (p->has_nav_completed) {
    p->web->remove_NavigationCompleted(p->nav_completed_token);
    p->has_nav_completed = false;
  }
  if (p->has_source_changed) {
    p->web->remove_SourceChanged(p->source_changed_token);
    p->has_source_changed = false;
  }
#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
  if (p->has_favicon_changed) {
    ICoreWebView2_15 *web15 = nullptr;
    if (SUCCEEDED(p->web->QueryInterface(IID_ICoreWebView2_15, reinterpret_cast<void **>(&web15))) && web15) {
      web15->remove_FaviconChanged(p->favicon_changed_token);
      web15->Release();
    }
    p->has_favicon_changed = false;
  }
#endif
}

HRESULT on_navigation_starting(
    WebPortal *p, ICoreWebView2NavigationStartingEventArgs *args) noexcept {
  if (p->closed || !args)
    return S_OK;
  const auto check = p->hooks.on_nav_check;
  void *const ctx = p->hooks.ctx;
  if (!check)
    return S_OK;
  LPWSTR uri = nullptr;
  const HRESULT hr = args->get_Uri(&uri);
  const bool had_uri = SUCCEEDED(hr) && uri && uri[0] != 0;
  uint32_t url_len = 0u;
  char *url_utf8 = had_uri ? narrow_utf8(uri, &url_len) : nullptr;
  if (uri)
    CoTaskMemFree(uri);
  if (had_uri && !url_utf8) {
    args->put_Cancel(TRUE);
    return S_OK;
  }
  const EtchStr url = {url_utf8 ? url_utf8 : "", url_len, false};
  const InlayPortalNavDecision decision = check(ctx, url, false);
  std::free(url_utf8);
  if (decision != INLAY_PORTAL_NAV_ALLOW) {
    args->put_Cancel(TRUE);
  } else if (p->hooks.on_loading_changed) {
    p->hooks.on_loading_changed(ctx, true);
  }
  return S_OK;
}

HRESULT on_new_window_requested(
    WebPortal *p, ICoreWebView2NewWindowRequestedEventArgs *args) noexcept {
  if (p->closed || !args)
    return S_OK;
  const auto check = p->hooks.on_nav_check;
  void *const ctx = p->hooks.ctx;
  if (!check)
    return S_OK;
  LPWSTR uri = nullptr;
  const HRESULT hr = args->get_Uri(&uri);
  const bool had_uri = SUCCEEDED(hr) && uri && uri[0] != 0;
  uint32_t url_len = 0u;
  char *url_utf8 = had_uri ? narrow_utf8(uri, &url_len) : nullptr;
  if (uri)
    CoTaskMemFree(uri);
  if (had_uri && !url_utf8) {
    args->put_Handled(TRUE);
    return S_OK;
  }
  const EtchStr url = {url_utf8 ? url_utf8 : "", url_len, false};
  const InlayPortalNavDecision decision = check(ctx, url, true);
  std::free(url_utf8);
  if (decision != INLAY_PORTAL_NAV_ALLOW)
    args->put_Handled(TRUE);
  return S_OK;
}

HRESULT on_web_message_received(
    WebPortal *p, ICoreWebView2WebMessageReceivedEventArgs *args) noexcept {
  if (p->closed || !args)
    return S_OK;
  const auto on_msg = p->hooks.on_web_message;
  void *const ctx = p->hooks.ctx;
  if (!on_msg)
    return S_OK;
  LPWSTR wmsg = nullptr;
  HRESULT hr = args->TryGetWebMessageAsString(&wmsg);
  if (FAILED(hr) || !wmsg) {
    if (wmsg) {
      CoTaskMemFree(wmsg);
      wmsg = nullptr;
    }
    hr = args->get_WebMessageAsJson(&wmsg);
  }
  if (FAILED(hr) || !wmsg) {
    if (wmsg)
      CoTaskMemFree(wmsg);
    return S_OK;
  }
  uint32_t len = 0u;
  char *utf8 = narrow_utf8(wmsg, &len);
  CoTaskMemFree(wmsg);
  if (!utf8 || len == 0u) {
    std::free(utf8);
    return S_OK;
  }
  on_msg(ctx, {utf8, len, false});
  std::free(utf8);
  return S_OK;
}

HRESULT on_permission_requested(
    WebPortal *p, ICoreWebView2PermissionRequestedEventArgs *args) noexcept {
  if (p->closed || !args)
    return S_OK;
  const auto check = p->hooks.on_perm_check;
  void *const ctx = p->hooks.ctx;
  if (!check)
    return S_OK;
  LPWSTR uri = nullptr;
  const HRESULT hr = args->get_Uri(&uri);
  const bool had_uri = SUCCEEDED(hr) && uri && uri[0] != 0;
  uint32_t origin_len = 0u;
  char *origin_utf8 = had_uri ? narrow_utf8(uri, &origin_len) : nullptr;
  if (uri)
    CoTaskMemFree(uri);
  if (had_uri && !origin_utf8) {
    args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
    return S_OK;
  }
  COREWEBVIEW2_PERMISSION_KIND kind =
      COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
  (void)args->get_PermissionKind(&kind);
  const char *kind_str = permission_kind_name(kind);
  const EtchStr origin = {origin_utf8 ? origin_utf8 : "", origin_len, false};
  const EtchStr perm = {kind_str, static_cast<uint32_t>(std::strlen(kind_str)),
                        true};
  const bool allowed = check(ctx, origin, perm);
  std::free(origin_utf8);
  args->put_State(allowed ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                          : COREWEBVIEW2_PERMISSION_STATE_DENY);
  return S_OK;
}

HRESULT on_document_title_changed(WebPortal *p) noexcept {
  if (p->closed || !p->web || !p->hooks.on_title_changed)
    return S_OK;
  LPWSTR wtitle = nullptr;
  if (SUCCEEDED(p->web->get_DocumentTitle(&wtitle)) && wtitle) {
    uint32_t len = 0u;
    char *utf8 = narrow_utf8(wtitle, &len);
    CoTaskMemFree(wtitle);
    p->hooks.on_title_changed(p->hooks.ctx, {utf8 ? utf8 : "", len, false});
    std::free(utf8);
  }
  return S_OK;
}

HRESULT on_navigation_completed(WebPortal *p) noexcept {
  if (p->closed)
    return S_OK;
  if (p->hooks.on_loading_changed)
    p->hooks.on_loading_changed(p->hooks.ctx, false);
  (void)on_document_title_changed(p);
  return S_OK;
}

HRESULT on_source_changed(WebPortal *p) noexcept {
  if (p->closed || !p->web || !p->hooks.on_source_changed)
    return S_OK;
  LPWSTR wsrc = nullptr;
  if (SUCCEEDED(p->web->get_Source(&wsrc)) && wsrc) {
    uint32_t len = 0u;
    char *utf8 = narrow_utf8(wsrc, &len);
    CoTaskMemFree(wsrc);
    if (utf8 && len > 0u)
      p->hooks.on_source_changed(p->hooks.ctx, {utf8, len, false});
    std::free(utf8);
  }
  return S_OK;
}

#ifdef __ICoreWebView2_15_INTERFACE_DEFINED__
HRESULT on_favicon_changed(WebPortal *p) noexcept {
  if (p->closed || !p->web || !p->hooks.on_favicon_changed)
    return S_OK;
  ICoreWebView2_15 *web15 = nullptr;
  if (SUCCEEDED(p->web->QueryInterface(IID_ICoreWebView2_15, reinterpret_cast<void **>(&web15))) && web15) {
    LPWSTR wfav = nullptr;
    if (SUCCEEDED(web15->get_FaviconUri(&wfav)) && wfav) {
      uint32_t len = 0u;
      char *utf8 = narrow_utf8(wfav, &len);
      CoTaskMemFree(wfav);
      if (utf8 && len > 0u)
        p->hooks.on_favicon_changed(p->hooks.ctx, {utf8, len, false});
      std::free(utf8);
    }
    web15->Release();
  }
  return S_OK;
}
#endif

HRESULT navigate(WebPortal *p) noexcept {
  if (!p->navigation || !p->web)
    return S_OK;
  return p->html ? p->web->NavigateToString(p->navigation)
                 : p->web->Navigate(p->navigation);
}

HRESULT start_classic_controller(WebPortal *p, ICoreWebView2Environment *env) noexcept {
  release_dcomp(p);
  const HRESULT started =
      env->CreateCoreWebView2Controller(p->parent, &p->controller_handler);
  if (FAILED(started) && p->completed)
    p->completed(p->user, static_cast<int32_t>(started));
  return S_OK;
}

HRESULT on_controller_ready(WebPortal *p, HRESULT result,
                            ICoreWebView2Controller *controller) noexcept {
  if (p->closed) {
    if (controller)
      controller->Close();
    return S_OK;
  }
  if (FAILED(result) || !controller) {
    if (p->completed)
      p->completed(p->user,
                   static_cast<int32_t>(FAILED(result) ? result : E_FAIL));
    return S_OK;
  }
  p->controller = controller;
  controller->AddRef();
  controller->get_CoreWebView2(&p->web);
  p->using_dcomp = false;
  attach_web_events(p);
  apply_bounds(p);
  if (p->web)
    navigate(p);
  if (p->completed)
    p->completed(p->user, static_cast<int32_t>(p->web ? S_OK : E_FAIL));
  return S_OK;
}

HRESULT on_composition_ready(WebPortal *p, HRESULT result,
                             ICoreWebView2CompositionController *comp) noexcept {
  ICoreWebView2Environment *fallback_env = p->environment;
  p->environment = nullptr;
  if (p->closed) {
    if (fallback_env)
      fallback_env->Release();
    if (comp) {
      ICoreWebView2Controller *ctrl = nullptr;
      if (SUCCEEDED(comp->QueryInterface(IID_ICoreWebView2Controller,
                                         reinterpret_cast<void **>(&ctrl))) &&
          ctrl) {
        ctrl->Close();
        ctrl->Release();
      }
    }
    return S_OK;
  }
  ICoreWebView2Controller *ctrl = nullptr;
  const bool ok_comp = SUCCEEDED(result) && comp &&
                       SUCCEEDED(comp->QueryInterface(
                           IID_ICoreWebView2Controller,
                           reinterpret_cast<void **>(&ctrl))) &&
                       ctrl && init_dcomp_target(p) && p->dcomp_webview &&
                       SUCCEEDED(comp->put_RootVisualTarget(p->dcomp_webview));
  if (!ok_comp) {
    if (ctrl) {
      ctrl->Close();
      ctrl->Release();
    }
    if (fallback_env) {
      const HRESULT hr = start_classic_controller(p, fallback_env);
      fallback_env->Release();
      return hr;
    }
    if (p->completed)
      p->completed(p->user,
                   static_cast<int32_t>(FAILED(result) ? result : E_FAIL));
    return S_OK;
  }
  if (fallback_env)
    fallback_env->Release();
  p->comp_controller = comp;
  comp->AddRef();
  p->controller = ctrl;
  p->using_dcomp = true;
  p->controller->get_CoreWebView2(&p->web);
  attach_web_events(p);
  apply_bounds(p);
  if (p->web)
    navigate(p);
  if (p->completed)
    p->completed(p->user, static_cast<int32_t>(p->web ? S_OK : E_FAIL));
  return S_OK;
}

HRESULT on_environment_ready(WebPortal *p, HRESULT result,
                             ICoreWebView2Environment *environment) noexcept {
  if (p->closed)
    return S_OK;
  if (FAILED(result) || !environment) {
    if (p->completed)
      p->completed(p->user,
                   static_cast<int32_t>(FAILED(result) ? result : E_FAIL));
    return S_OK;
  }
  ICoreWebView2Environment3 *env3 = nullptr;
  if (!p->disable_dcomp && init_dcomp_device(p) &&
      SUCCEEDED(environment->QueryInterface(IID_ICoreWebView2Environment3,
                                            reinterpret_cast<void **>(&env3))) &&
      env3) {
    p->environment = environment;
    environment->AddRef();
    const HRESULT started = env3->CreateCoreWebView2CompositionController(
        p->parent, &p->composition_handler);
    env3->Release();
    if (SUCCEEDED(started))
      return S_OK;
    p->environment->Release();
    p->environment = nullptr;
  }
  return start_classic_controller(p, environment);
}

void *create(void *user, uint32_t w, uint32_t h) {
  InlayPortalWebView2Options options = inlay_portal_webview2_options_default();
  if (!inlay_read_abi_struct(static_cast<const InlayPortalWebView2Options *>(user),
                             &options))
    return nullptr;
  if (!IsWindow(static_cast<HWND>(options.underlay_hwnd)) ||
      !options.user_data_path)
    return nullptr;
  const size_t loader_len =
                   options.loader_path ? std::strlen(options.loader_path) : 0,
               data_len = std::strlen(options.user_data_path);
  if (loader_len > 32767 || data_len > 32767)
    return nullptr;
  auto *loader_path =
      loader_len
          ? wide(options.loader_path, static_cast<uint32_t>(loader_len))
          : nullptr;
  auto *data_path =
      wide(options.user_data_path, static_cast<uint32_t>(data_len));
  if ((loader_len && !loader_path) || !data_path) {
    std::free(loader_path);
    std::free(data_path);
    return nullptr;
  }
  const DWORD search = loader_path ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                   : LOAD_LIBRARY_SEARCH_APPLICATION_DIR;
  HMODULE module = LoadLibraryExW(
      loader_path ? loader_path : L"WebView2Loader.dll", nullptr,
      search | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  std::free(loader_path);
  if (!module) {
    std::free(data_path);
    return nullptr;
  }
  using CreateEnvironment = HRESULT(STDAPICALLTYPE *)(
      PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
  const auto address =
      GetProcAddress(module, "CreateCoreWebView2EnvironmentWithOptions");
  CreateEnvironment start = nullptr;
  static_assert(sizeof(start) == sizeof(address));
  std::memcpy(&start, &address, sizeof(start));
  if (!start) {
    FreeLibrary(module);
    std::free(data_path);
    return nullptr;
  }
  void *raw = std::malloc(sizeof(WebPortal));
  if (!raw) {
    FreeLibrary(module);
    std::free(data_path);
    return nullptr;
  }
  auto *p = new (raw) WebPortal();
  p->completed = options.completed;
  p->user = options.user_data;
  p->disable_dcomp = options.disable_dcomp;
  p->loader = module;
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) {
    std::free(data_path);
    release(p);
    return nullptr;
  }
  p->com_initialized = true;
  p->com_thread = GetCurrentThreadId();
  p->parent = static_cast<HWND>(options.underlay_hwnd);
  p->portal_width = w;
  p->portal_height = h;
  p->bounds_logical = {0.f, 0.f, static_cast<float>(w), static_cast<float>(h)};
  p->visible_logical = p->bounds_logical;
  p->bounds = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
  const HRESULT result = start(nullptr, data_path, nullptr, &p->environment_handler);
  std::free(data_path);
  if (FAILED(result)) {
    p->closed = true;
    release(p);
    return nullptr;
  }
  return p;
}

void destroy(void *backend) {
  auto *p = static_cast<WebPortal *>(backend);
  p->closed = true;
  p->hooks = {};
  detach_web_events(p);
  if (p->comp_controller) {
    p->comp_controller->put_RootVisualTarget(nullptr);
    p->comp_controller->Release();
    p->comp_controller = nullptr;
  }
  if (p->controller) {
    p->controller->Close();
    p->controller->Release();
    p->controller = nullptr;
  }
  if (p->web) {
    p->web->Release();
    p->web = nullptr;
  }
  if (p->environment) {
    p->environment->Release();
    p->environment = nullptr;
  }
  release_dcomp(p);
  release(p);
}

bool load(WebPortal *p, EtchStr text, bool html) {
  if (text.length > 16 * 1024 * 1024 || std::memchr(text.chars, 0, text.length))
    return false;
  auto *next = wide(text.chars, text.length);
  if (!next)
    return false;
  std::free(p->navigation);
  p->navigation = next;
  p->html = html;
  return !p->web || SUCCEEDED(navigate(p));
}

bool load_url(void *b, EtchStr s) {
  return load(static_cast<WebPortal *>(b), s, false);
}

bool load_html(void *b, EtchStr s) {
  return load(static_cast<WebPortal *>(b), s, true);
}

bool resize(void *b, uint32_t w, uint32_t h) {
  auto *p = static_cast<WebPortal *>(b);
  p->portal_width = w;
  p->portal_height = h;
  return true;
}

uint32_t poll(void *, InlayPortalDirtyRect *, uint32_t) { return 0; }
void ack(void *, bool) {}

void inject_pointer(void *b, EtchVec2 local_pos, bool down) {
  auto *p = static_cast<WebPortal *>(b);
  if (!p->using_dcomp || !p->comp_controller)
    return;
  if (local_pos.x < 0.f || local_pos.y < 0.f) {
    if (p->left_down) {
      p->comp_controller->SendMouseInput(
          COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP,
          COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0u, p->last_mouse_pt);
      p->left_down = false;
    }
    p->comp_controller->SendMouseInput(
        COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
        COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0u, p->last_mouse_pt);
    return;
  }
  const float bw = static_cast<float>(p->bounds.right - p->bounds.left);
  const float bh = static_cast<float>(p->bounds.bottom - p->bounds.top);
  const float sx = p->portal_width ? (bw / static_cast<float>(p->portal_width)) : 1.f;
  const float sy = p->portal_height ? (bh / static_cast<float>(p->portal_height)) : 1.f;
  const POINT pt = {static_cast<LONG>(std::lround(local_pos.x * sx)),
                    static_cast<LONG>(std::lround(local_pos.y * sy))};
  const auto move_keys = p->left_down ? COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON
                                      : COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
  if (!p->has_mouse_pt || pt.x != p->last_mouse_pt.x || pt.y != p->last_mouse_pt.y) {
    p->comp_controller->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE,
                                       move_keys, 0u, pt);
    p->last_mouse_pt = pt;
    p->has_mouse_pt = true;
  }
  if (down && !p->left_down) {
    p->comp_controller->SendMouseInput(
        COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN,
        COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON, 0u, pt);
    p->left_down = true;
  } else if (!down && p->left_down) {
    p->comp_controller->SendMouseInput(
        COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP,
        COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0u, pt);
    p->left_down = false;
  }
}

void inject_wheel(void *b, EtchVec2 delta) {
  auto *p = static_cast<WebPortal *>(b);
  if (!p->using_dcomp || !p->comp_controller)
    return;
  const auto keys = p->left_down ? COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON
                                 : COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
  if (delta.y != 0.f) {
    const int32_t d = static_cast<int32_t>(std::lround(delta.y * static_cast<float>(WHEEL_DELTA)));
    p->comp_controller->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                                       keys, static_cast<UINT32>(d), p->last_mouse_pt);
  }
  if (delta.x != 0.f) {
    const int32_t d = static_cast<int32_t>(std::lround(delta.x * static_cast<float>(WHEEL_DELTA)));
    p->comp_controller->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL,
                                       keys, static_cast<UINT32>(d), p->last_mouse_pt);
  }
}

void inject_key(void *b, uint32_t key, bool down, uint16_t mods) {
  (void)mods;
  auto *p = static_cast<WebPortal *>(b);
  if (!p || p->closed || !p->controller) return;
  if (down) (void)p->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  HWND child = find_webview_child(p);
  if (child) {
    WPARAM vk = static_cast<WPARAM>(key);
    if (key >= 'a' && key <= 'z') vk = static_cast<WPARAM>(key - ('a' - 'A'));
    else if (key == 13u) vk = VK_RETURN;
    else if (key == 27u) vk = VK_ESCAPE;
    else if (key == 8u) vk = VK_BACK;
    else if (key == 9u) vk = VK_TAB;
    PostMessageW(child, down ? WM_KEYDOWN : WM_KEYUP, vk, 0);
  }
}

void inject_text(void *b, EtchStr text) {
  auto *p = static_cast<WebPortal *>(b);
  if (!p || p->closed || !p->controller || !text.chars || text.length == 0u) return;
  HWND child = find_webview_child(p);
  if (!child) return;
  wchar_t *ws = wide(text.chars, text.length);
  if (!ws) return;
  for (size_t i = 0u; ws[i] != 0; ++i) {
    PostMessageW(child, WM_CHAR, static_cast<WPARAM>(ws[i]), 0);
  }
  std::free(ws);
}

void set_radius(void *b, float radius_px) {
  auto *p = static_cast<WebPortal *>(b);
  p->radius_logical = (std::isfinite(radius_px) && radius_px > 0.f) ? radius_px : 0.f;
}

void bounds(void *b, EtchRect rect, EtchRect clip) {
  auto *p = static_cast<WebPortal *>(b);
  p->bounds_logical = rect;
  p->visible_logical = clip;
  apply_bounds(p);
}

const InlayPortalBackendVTable api = {
    create,  destroy,        load_url,     load_html, resize,
    poll,    inject_pointer, inject_wheel, inject_key, inject_text,
    nullptr, bounds,         ack,          set_radius};
} // namespace

bool inlay_portal_webview2_inject_mouse_button(void *b, EtchVec2 local_pos, uint8_t button, bool down) noexcept {
  auto *p = static_cast<WebPortal *>(b);
  if (!p || p->closed || !p->using_dcomp || !p->comp_controller || button < 1u || button > 3u)
    return false;
  if (button == 1u) {
    inject_pointer(b, local_pos, down);
    return true;
  }
  const float bw = static_cast<float>(p->bounds.right - p->bounds.left);
  const float bh = static_cast<float>(p->bounds.bottom - p->bounds.top);
  const float sx = p->portal_width ? (bw / static_cast<float>(p->portal_width)) : 1.f;
  const float sy = p->portal_height ? (bh / static_cast<float>(p->portal_height)) : 1.f;
  const POINT pt = {static_cast<LONG>(std::lround(local_pos.x * sx)),
                    static_cast<LONG>(std::lround(local_pos.y * sy))};
  p->last_mouse_pt = pt;
  p->has_mouse_pt = true;
  const auto kind = (button == 2u)
      ? (down ? COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN : COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP)
      : (down ? COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN : COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP);
  const auto vkeys = down
      ? (button == 2u ? COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON : COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON)
      : COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
  return SUCCEEDED(p->comp_controller->SendMouseInput(kind, vkeys, 0u, pt));
}

bool inlay_portal_webview2_post_message(void *backend, EtchStr msg) noexcept {
  if (!backend || !msg.chars || msg.length == 0u || msg.length > 65536u ||
      std::memchr(msg.chars, 0, msg.length))
    return false;
  auto *p = static_cast<WebPortal *>(backend);
  if (p->closed || !p->web)
    return false;
  wchar_t *wmsg = wide(msg.chars, msg.length);
  if (!wmsg)
    return false;
  const HRESULT hr = p->web->PostWebMessageAsString(wmsg);
  std::free(wmsg);
  return SUCCEEDED(hr);
}

bool inlay_portal_webview2_nav_action(void *backend, int action) noexcept {
  if (!backend)
    return false;
  auto *p = static_cast<WebPortal *>(backend);
  if (p->closed || !p->web)
    return false;
  switch (action) {
  case 0:
    return SUCCEEDED(p->web->GoBack());
  case 1:
    return SUCCEEDED(p->web->GoForward());
  case 2:
    return SUCCEEDED(p->web->Reload());
  default:
    return false;
  }
}

void inlay_portal_webview2_set_hooks(void *backend,
                                     const InlayPortalHostHooks *hooks) noexcept {
  if (!backend)
    return;
  auto *p = static_cast<WebPortal *>(backend);
  if (hooks)
    p->hooks = *hooks;
  else
    p->hooks = {};
}

bool inlay_portal_webview2_eval_script(void *backend, EtchStr script) noexcept {
  if (!backend || !script.chars || script.length == 0u || script.length > 65536u ||
      std::memchr(script.chars, 0, script.length))
    return false;
  auto *p = static_cast<WebPortal *>(backend);
  if (p->closed || !p->web)
    return false;
  wchar_t *ws = wide(script.chars, script.length);
  if (!ws)
    return false;
  const HRESULT hr = p->web->ExecuteScript(ws, nullptr);
  std::free(ws);
  return SUCCEEDED(hr);
}

bool inlay_portal_webview2_add_init_script(void *backend, EtchStr script) noexcept {
  if (!backend || !script.chars || script.length == 0u || script.length > 65536u ||
      std::memchr(script.chars, 0, script.length))
    return false;
  auto *p = static_cast<WebPortal *>(backend);
  if (p->closed)
    return false;
  wchar_t *ws = wide(script.chars, script.length);
  if (!ws)
    return false;
  if (p->web) {
    const HRESULT hr = p->web->AddScriptToExecuteOnDocumentCreated(ws, nullptr);
    std::free(ws);
    return SUCCEEDED(hr);
  }
  if (!inlay_grow_pod(p->pending_init_scripts, p->pending_init_cap,
                      p->pending_init_count + 1u, 4u, 64u)) {
    std::free(ws);
    return false;
  }
  p->pending_init_scripts[p->pending_init_count++] = ws;
  return true;
}

bool inlay_portal_webview2_open_devtools(void *backend) noexcept {
  if (!backend)
    return false;
  auto *p = static_cast<WebPortal *>(backend);
  if (p->closed || !p->web)
    return false;
  return SUCCEEDED(p->web->OpenDevToolsWindow());
}

bool inlay_portal_webview2_set_zoom(void *backend, float zoom_factor) noexcept {
  if (!backend || !std::isfinite(zoom_factor) || zoom_factor <= 0.0f)
    return false;
  auto *p = static_cast<WebPortal *>(backend);
  if (p->closed)
    return false;
  p->pending_zoom = static_cast<double>(zoom_factor);
  if (p->controller)
    return SUCCEEDED(p->controller->put_ZoomFactor(p->pending_zoom));
  return true;
}

extern "C" INLAY_API const InlayPortalBackendVTable *
inlay_portal_webview2_backend(void) {
  return &api;
}
#else
bool inlay_portal_webview2_post_message(void *, EtchStr) noexcept {
  return false;
}

bool inlay_portal_webview2_nav_action(void *, int) noexcept {
  return false;
}

bool inlay_portal_webview2_eval_script(void *, EtchStr) noexcept {
  return false;
}

bool inlay_portal_webview2_add_init_script(void *, EtchStr) noexcept {
  return false;
}

bool inlay_portal_webview2_open_devtools(void *) noexcept {
  return false;
}

bool inlay_portal_webview2_set_zoom(void *, float) noexcept {
  return false;
}

bool inlay_portal_webview2_inject_mouse_button(void *, EtchVec2, uint8_t, bool) noexcept {
  return false;
}

void inlay_portal_webview2_set_hooks(void *,
                                     const InlayPortalHostHooks *) noexcept {}

extern "C" INLAY_API const InlayPortalBackendVTable *
inlay_portal_webview2_backend(void) {
  return nullptr;
}
#endif
