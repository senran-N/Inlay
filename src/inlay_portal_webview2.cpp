#include <inlay/inlay_c.h>
#if defined(_WIN32) && INLAY_ENABLE_PORTAL_WEBVIEW2
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <WebView2.h>
// clang-format on
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
namespace {
struct WebPortal;
struct EnvironmentHandler {
  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler iface;
  WebPortal *owner;
};
struct ControllerHandler {
  ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface;
  WebPortal *owner;
};
struct WebPortal {
  EnvironmentHandler environment_handler;
  ControllerHandler controller_handler;
  volatile LONG references;
  DWORD com_thread;
  HWND parent;
  HWND child_hwnd;
  HMODULE loader;
  ICoreWebView2Controller *controller;
  ICoreWebView2 *web;
  wchar_t *navigation;
  bool html, closed, com_initialized;
  void (*completed)(void *, int32_t);
  void *user;
  RECT bounds;
  RECT clip_rect;
  bool visible, clipped, has_rgn;
};
ULONG retain(WebPortal *p) {
  return static_cast<ULONG>(InterlockedIncrement(&p->references));
}
ULONG release(WebPortal *p) {
  const ULONG n = static_cast<ULONG>(InterlockedDecrement(&p->references));
  if (!n) {
    std::free(p->navigation);
    FreeLibrary(p->loader);
    if (p->com_initialized && GetCurrentThreadId() == p->com_thread)
      CoUninitialize();
    std::free(p);
  }
  return n;
}
HWND find_webview_child(WebPortal *p) {
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
void apply_bounds(WebPortal *p) {
  if (!p->controller)
    return;
  p->controller->lpVtbl->put_Bounds(p->controller, p->bounds);
  p->controller->lpVtbl->put_IsVisible(p->controller, p->visible);
  HWND child = find_webview_child(p);
  if (!child)
    return;
  if (p->visible && p->clipped) {
    HRGN rgn = CreateRectRgn(p->clip_rect.left - p->bounds.left,
                             p->clip_rect.top - p->bounds.top,
                             p->clip_rect.right - p->bounds.left,
                             p->clip_rect.bottom - p->bounds.top);
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
wchar_t *wide(const char *bytes, uint32_t length) {
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
HRESULT navigate(WebPortal *p) {
  if (!p->navigation)
    return S_OK;
  return p->html ? p->web->lpVtbl->NavigateToString(p->web, p->navigation)
                 : p->web->lpVtbl->Navigate(p->web, p->navigation);
}
ULONG STDMETHODCALLTYPE
env_add(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *h) {
  return retain(reinterpret_cast<EnvironmentHandler *>(h)->owner);
}
ULONG STDMETHODCALLTYPE
env_release(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *h) {
  return release(reinterpret_cast<EnvironmentHandler *>(h)->owner);
}
HRESULT STDMETHODCALLTYPE
env_query(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *h,
          REFIID id, void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  if (id != IID_IUnknown &&
      id != IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
    return E_NOINTERFACE;
  *out = h;
  env_add(h);
  return S_OK;
}
ULONG STDMETHODCALLTYPE
controller_add(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *h) {
  return retain(reinterpret_cast<ControllerHandler *>(h)->owner);
}
ULONG STDMETHODCALLTYPE controller_release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *h) {
  return release(reinterpret_cast<ControllerHandler *>(h)->owner);
}
HRESULT STDMETHODCALLTYPE
controller_query(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *h,
                 REFIID id, void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  if (id != IID_IUnknown &&
      id != IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    return E_NOINTERFACE;
  *out = h;
  controller_add(h);
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
controller_ready(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *h,
                 HRESULT result, ICoreWebView2Controller *controller) {
  auto *p = reinterpret_cast<ControllerHandler *>(h)->owner;
  if (p->closed) {
    if (controller)
      controller->lpVtbl->Close(controller);
    return S_OK;
  }
  if (FAILED(result) || !controller) {
    if (p->completed)
      p->completed(p->user,
                   static_cast<int32_t>(FAILED(result) ? result : E_FAIL));
    return S_OK;
  }
  p->controller = controller;
  controller->lpVtbl->AddRef(controller);
  controller->lpVtbl->get_CoreWebView2(controller, &p->web);
  apply_bounds(p);
  if (p->web)
    navigate(p);
  if (p->completed)
    p->completed(p->user, static_cast<int32_t>(p->web ? S_OK : E_FAIL));
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
environment_ready(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *h,
                  HRESULT result, ICoreWebView2Environment *environment) {
  auto *p = reinterpret_cast<EnvironmentHandler *>(h)->owner;
  if (p->closed)
    return S_OK;
  if (FAILED(result) || !environment) {
    if (p->completed)
      p->completed(p->user,
                   static_cast<int32_t>(FAILED(result) ? result : E_FAIL));
    return S_OK;
  }
  const HRESULT started = environment->lpVtbl->CreateCoreWebView2Controller(
      environment, p->parent, &p->controller_handler.iface);
  if (FAILED(started) && p->completed)
    p->completed(p->user, static_cast<int32_t>(started));
  return S_OK;
}
ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl
    environment_vtable = {env_query, env_add, env_release, environment_ready};
ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl
    controller_vtable = {controller_query, controller_add, controller_release,
                         controller_ready};
void *create(void *user, uint32_t w, uint32_t h) {
  const auto *options = static_cast<const InlayPortalWebView2Options *>(user);
  if (!options || !IsWindow(static_cast<HWND>(options->underlay_hwnd)) ||
      !options->user_data_path)
    return nullptr;
  const size_t loader_len = options->loader_path ? std::strlen(options->loader_path) : 0,
               data_len = std::strlen(options->user_data_path);
  if (loader_len > 32767 || data_len > 32767)
    return nullptr;
  auto *loader_path = loader_len
      ? wide(options->loader_path, static_cast<uint32_t>(loader_len)) : nullptr;
  auto *data_path =
      wide(options->user_data_path, static_cast<uint32_t>(data_len));
  if ((loader_len && !loader_path) || !data_path) {
    std::free(loader_path);
    std::free(data_path);
    return nullptr;
  }
  const DWORD search = loader_path ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                   : LOAD_LIBRARY_SEARCH_APPLICATION_DIR;
  HMODULE module = LoadLibraryExW(loader_path ? loader_path : L"WebView2Loader.dll",
                                 nullptr, search | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
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
  auto *p = static_cast<WebPortal *>(std::calloc(1, sizeof(WebPortal)));
  if (!p) {
    FreeLibrary(module);
    std::free(data_path);
    return nullptr;
  }
  p->references = 1;
  p->completed = options->completed;
  p->user = options->user_data;
  p->loader = module;
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) {
    std::free(data_path);
    release(p);
    return nullptr;
  }
  p->com_initialized = true;
  p->com_thread = GetCurrentThreadId();
  p->parent = static_cast<HWND>(options->underlay_hwnd);
  p->bounds = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
  p->environment_handler = {{&environment_vtable}, p};
  p->controller_handler = {{&controller_vtable}, p};
  const HRESULT result =
      start(nullptr, data_path, nullptr, &p->environment_handler.iface);
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
  if (p->controller) {
    p->controller->lpVtbl->Close(p->controller);
    p->controller->lpVtbl->Release(p->controller);
    p->controller = nullptr;
  }
  if (p->web) {
    p->web->lpVtbl->Release(p->web);
    p->web = nullptr;
  }
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
bool resize(void *, uint32_t, uint32_t) { return true; }
uint32_t poll(void *, InlayPortalDirtyRect *, uint32_t) { return 0; }
void ack(void *, bool) {}
void bounds(void *b, EtchRect rect, EtchRect clip) {
  auto *p = static_cast<WebPortal *>(b);
  const float scale = static_cast<float>(GetDpiForWindow(p->parent)) / 96.f;
  const LONG bl = static_cast<LONG>(std::lround(rect.x * scale));
  const LONG bt = static_cast<LONG>(std::lround(rect.y * scale));
  const LONG br = static_cast<LONG>(std::lround((rect.x + rect.w) * scale));
  const LONG bb = static_cast<LONG>(std::lround((rect.y + rect.h) * scale));
  const float ix0 = rect.x > clip.x ? rect.x : clip.x;
  const float iy0 = rect.y > clip.y ? rect.y : clip.y;
  const float ix1 = (rect.x + rect.w) < (clip.x + clip.w) ? (rect.x + rect.w) : (clip.x + clip.w);
  const float iy1 = (rect.y + rect.h) < (clip.y + clip.h) ? (rect.y + rect.h) : (clip.y + clip.h);
  const LONG vl = static_cast<LONG>(std::lround(ix0 * scale));
  const LONG vt = static_cast<LONG>(std::lround(iy0 * scale));
  const LONG vr = static_cast<LONG>(std::lround(ix1 * scale));
  const LONG vb = static_cast<LONG>(std::lround(iy1 * scale));
  p->bounds = {bl, bt, br, bb};
  p->clip_rect = {vl, vt, vr, vb};
  p->visible = clip.w > 0 && clip.h > 0 && vr > vl && vb > vt;
  p->clipped = p->visible && (vl > bl || vt > bt || vr < br || vb < bb);
  apply_bounds(p);
}
const InlayPortalBackendVTable api = {
    create,  destroy, load_url, load_html, resize, poll, nullptr,
    nullptr, nullptr, nullptr,  nullptr,   bounds, ack};
} // namespace
extern "C" const InlayPortalBackendVTable *inlay_portal_webview2_backend(void) {
  return &api;
}
#else
extern "C" const InlayPortalBackendVTable *inlay_portal_webview2_backend(void) {
  return nullptr;
}
#endif
