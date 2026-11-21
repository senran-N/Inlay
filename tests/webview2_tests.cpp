#define WIN32_LEAN_AND_MEAN
#include <cmath>
#include <cstdio>
#include <inlay/inlay_c.h>
#include "../src/inlay_internal.h"
#include <objbase.h>
#include <windows.h>

namespace {
struct Completion {
  bool called;
  int32_t result;
};
void completed(void *user, int32_t result) {
  auto *s = static_cast<Completion *>(user);
  s->called = true;
  s->result = result;
}
bool test_dcomp_clip_math() {
  // Unclipped rounded rect at 2x DPI
  const auto c1 = inlay_dcomp_compute_clip({10.f, 20.f, 300.f, 200.f},
                                           {10.f, 20.f, 300.f, 200.f}, 12.f, 2.f);
  bool ok = c1.visible && !c1.clipped && c1.need_clip &&
            std::fabs(c1.offset_x - 20.f) < .1f && std::fabs(c1.offset_y - 40.f) < .1f &&
            std::fabs(c1.width - 600.f) < .1f && std::fabs(c1.height - 400.f) < .1f &&
            std::fabs(c1.tl_r - 24.f) < .1f && std::fabs(c1.tr_r - 24.f) < .1f &&
            std::fabs(c1.br_r - 24.f) < .1f && std::fabs(c1.bl_r - 24.f) < .1f;
  // Partially clipped at top by scroll viewport: top radii collapse to 0 while bottom radii remain rounded!
  const auto c2 = inlay_dcomp_compute_clip({10.f, 20.f, 300.f, 200.f},
                                           {10.f, 60.f, 300.f, 160.f}, 12.f, 1.f);
  ok &= c2.visible && c2.clipped && c2.need_clip &&
        std::fabs(c2.left) < .1f && std::fabs(c2.top - 40.f) < .1f &&
        std::fabs(c2.right - 300.f) < .1f && std::fabs(c2.bottom - 200.f) < .1f &&
        c2.tl_r == 0.f && c2.tr_r == 0.f &&
        std::fabs(c2.br_r - 12.f) < .1f && std::fabs(c2.bl_r - 12.f) < .1f;
  // Oversized radius clamps cleanly to half of narrow visible strip
  const auto c3 = inlay_dcomp_compute_clip({0.f, 0.f, 100.f, 100.f},
                                           {0.f, 0.f, 100.f, 100.f}, 80.f, 1.f);
  ok &= c3.visible && c3.need_clip && std::fabs(c3.tl_r - 50.f) < .1f &&
        c3.tl_r + c3.tr_r <= 100.1f && c3.tl_r + c3.bl_r <= 100.1f;
  // Empty / non-overlapping clip marks invisible
  const auto c4 = inlay_dcomp_compute_clip({0.f, 0.f, 100.f, 100.f},
                                           {200.f, 0.f, 100.f, 100.f}, 8.f, 1.f);
  ok &= !c4.visible;
  return ok;
}
void pump_until(bool *flag, ULONGLONG timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (!*flag && GetTickCount64() < deadline) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  bool ok = test_dcomp_clip_math();
  HWND window = CreateWindowExW(0, L"STATIC", L"Inlay native smoke",
                                WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  const auto *api = inlay_portal_webview2_backend();
  Completion state = {};
  InlayPortalWebView2Options options = inlay_portal_webview2_options_default();
  options.underlay_hwnd = window;
  options.loader_path = nullptr;
  options.user_data_path = argv[2];
  options.completed = completed;
  options.user_data = &state;
  if (!window || !api)
    return 1;
  // Invalid struct_size rejected
  InlayPortalWebView2Options bad_opts = options;
  bad_opts.struct_size = 2u;
  ok &= api->create(&bad_opts, 640, 480) == nullptr;

  // MTA thread must fail cleanly
  const HRESULT mta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(mta))
    return 1;
  void *backend = api->create(&options, 640, 480);
  ok &= !backend && !state.called;
  if (backend)
    api->destroy(backend);
  CoUninitialize();

  // 1. Default DirectComposition path (with automatic fallback if unavailable)
  backend = api->create(&options, 640, 480);
  ok &= backend &&
      api->load_html(backend,
                     ETCH_STR_LIT("<!doctype html><input autofocus "
                                  "value='Native IME'><p>Inlay WebView2 DComp</p>"));
  if (ok)
    pump_until(&state.called, 15000);
  ok &= state.called && state.result >= 0;
  if (!ok)
    std::printf("DComp loader: started=%d called=%d HRESULT=0x%08lx\n",
                backend != nullptr, state.called, static_cast<unsigned long>(state.result));
  if (backend) {
    if (api->set_underlay_radius)
      api->set_underlay_radius(backend, 12.f);
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 40, 640, 400});
    if (api->inject_pointer) {
      api->inject_pointer(backend, {120.f, 120.f}, true);
      api->inject_pointer(backend, {120.f, 120.f}, false);
    }
    if (api->inject_wheel)
      api->inject_wheel(backend, {0.f, -1.f});
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 0, 640, 480});
    api->destroy(backend);
  }

  // 2. Classic HWND controller + SetWindowRgn fallback path (`disable_dcomp = true`)
  state = {};
  HWND fallback_window = CreateWindowExW(0, L"STATIC", L"Inlay classic fallback",
                                         WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
  char fallback_dir[512] = {};
  std::snprintf(fallback_dir, sizeof(fallback_dir), "%s-classic", argv[2]);
  InlayPortalWebView2Options fallback_opts = options;
  fallback_opts.underlay_hwnd = fallback_window;
  fallback_opts.user_data_path = fallback_dir;
  fallback_opts.disable_dcomp = true;
  backend = fallback_window ? api->create(&fallback_opts, 640, 480) : nullptr;
  ok &= backend &&
      api->load_html(backend, ETCH_STR_LIT("<!doctype html><p>Classic fallback</p>"));
  if (ok)
    pump_until(&state.called, 15000);
  ok &= state.called && state.result >= 0;
  if (!ok)
    std::printf("Fallback loader: started=%d called=%d HRESULT=0x%08lx\n",
                backend != nullptr, state.called, static_cast<unsigned long>(state.result));
  if (backend) {
    if (api->set_underlay_radius)
      api->set_underlay_radius(backend, 10.f);
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 40, 640, 400});
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 0, 640, 480});
    api->destroy(backend);
  }
  if (fallback_window)
    DestroyWindow(fallback_window);

  // 3. Destruction during asynchronous creation must leave callbacks safe and silent.
  state = {};
  const HRESULT sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ok &= SUCCEEDED(sta);
  const char *loaders[] = {argv[1], ""};
  for (const char *loader : loaders) {
    options.loader_path = loader;
    backend = api->create(&options, 320, 240);
    ok &= backend != nullptr;
    if (backend)
      api->destroy(backend);
  }
  const ULONGLONG drain = GetTickCount64() + 1000;
  while (GetTickCount64() < drain) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
  ok &= !state.called;
  if (SUCCEEDED(sta))
    CoUninitialize();
  DestroyWindow(window);
  std::printf("WebView2 native DComp/fallback/async: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
