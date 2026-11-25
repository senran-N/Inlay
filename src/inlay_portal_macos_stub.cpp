// Non-Apple C++ stub for inlay_portal_macos_backend() (real ObjC++ backend is inlay_portal_macos.mm).
#include "inlay_internal.h"
#if !defined(__APPLE__) || !defined(INLAY_ENABLE_PORTAL_MACOS) || !INLAY_ENABLE_PORTAL_MACOS
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
