#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <inlay/inlay_c.h>
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
} // namespace
int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  HWND window = CreateWindowExW(0, L"STATIC", L"Inlay native smoke",
                                WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  const auto *api = inlay_portal_webview2_backend();
  Completion state = {};
  InlayPortalWebView2Options options = {window, nullptr, argv[2], completed,
                                        &state};
  if (!window || !api)
    return 1;
  const HRESULT mta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(mta))
    return 1;
  void *backend = api->create(&options, 640, 480);
  bool ok = !backend && !state.called;
  if (backend)
    api->destroy(backend);
  CoUninitialize();
  backend = api->create(&options, 640, 480);
  ok &= backend &&
      api->load_html(backend,
                     ETCH_STR_LIT("<!doctype html><input autofocus "
                                  "value='Native IME'><p>Inlay WebView2</p>"));
  const ULONGLONG deadline = GetTickCount64() + 15000;
  while (ok && !state.called && GetTickCount64() < deadline) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
  }
  ok &= state.called && state.result >= 0;
  if (!ok)
    std::printf("Default loader: started=%d called=%d HRESULT=0x%08lx\n",
                backend != nullptr, state.called, static_cast<unsigned long>(state.result));
  if (backend) {
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 40, 640, 400});
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 0, 640, 480});
    api->destroy(backend);
  }
  // Destruction during asynchronous creation must leave callbacks safe and
  // silent.
  state = {};
  const HRESULT sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ok &= SUCCEEDED(sta);
  const char* loaders[] = {argv[1], ""};
  for (const char* loader : loaders) {
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
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
  }
  ok &= !state.called;
  if (SUCCEEDED(sta))
    CoUninitialize();
  DestroyWindow(window);
  std::printf("WebView2 native async/lifetime: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
