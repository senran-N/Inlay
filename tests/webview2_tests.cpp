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
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(init))
    return 1;
  HWND window = CreateWindowExW(0, L"STATIC", L"Inlay native smoke",
                                WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  const auto *api = inlay_portal_webview2_backend();
  Completion state = {};
  InlayPortalWebView2Options options = {window, argv[1], argv[2], completed,
                                        &state};
  void *backend = api ? api->create(&options, 640, 480) : nullptr;
  bool ok =
      backend &&
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
  if (backend) {
    api->set_underlay_bounds(backend, {0, 0, 640, 480}, {0, 0, 640, 480});
    api->destroy(backend);
  }
  // Destruction during asynchronous creation must leave callbacks safe and
  // silent.
  state = {};
  backend = api ? api->create(&options, 320, 240) : nullptr;
  if (backend)
    api->destroy(backend);
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
  DestroyWindow(window);
  CoUninitialize();
  std::printf("WebView2 native async/lifetime: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
