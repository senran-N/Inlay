#include <cmath>
#include <cstdio>
#include <cstring>
#include <etch/etch.h>
#include <inlay/inlay_c.h>
namespace {
uint32_t rejected = 0;
EtchRect native_visible = {};
struct PolicyProbe {
  InlayPortalNavDecision decision;
  uint32_t nav_calls;
  uint32_t msg_calls;
  bool last_new_window;
  char last_url[128];
  char last_msg[128];
};
bool str_eq(EtchStr s, const char *lit) noexcept {
  const size_t n = std::strlen(lit);
  return s.chars && s.length == n && std::memcmp(s.chars, lit, n) == 0;
}
InlayPortalNavDecision on_test_navigate(InlayPortal *, EtchStr url,
                                        bool is_new_window, void *user) {
  auto *probe = static_cast<PolicyProbe *>(user);
  ++probe->nav_calls;
  probe->last_new_window = is_new_window;
  const size_t n = url.length < sizeof(probe->last_url) - 1u
                       ? url.length
                       : sizeof(probe->last_url) - 1u;
  if (n && url.chars)
    std::memcpy(probe->last_url, url.chars, n);
  probe->last_url[n] = '\0';
  return probe->decision;
}
void on_test_message(InlayPortal *, EtchStr msg, void *user) {
  auto *probe = static_cast<PolicyProbe *>(user);
  ++probe->msg_calls;
  const size_t n = msg.length < sizeof(probe->last_msg) - 1u
                       ? msg.length
                       : sizeof(probe->last_msg) - 1u;
  if (n && msg.chars)
    std::memcpy(probe->last_msg, msg.chars, n);
  probe->last_msg[n] = '\0';
}
void record_bounds(void *, EtchRect, EtchRect visible) {
  native_visible = visible;
}
uint32_t invalid_poll(void *backend, InlayPortalDirtyRect *rects,
                      uint32_t capacity) {
  const uint32_t count =
      inlay_portal_mock_backend()->poll_dirty_rects(backend, rects, capacity);
  if (count)
    rects[0].width = 0;
  return count;
}
void record_ack(void *backend, bool accepted) {
  if (!accepted)
    ++rejected;
  inlay_portal_mock_backend()->ack_dirty_rects(backend, accepted);
}
} // namespace
int main(int argc, char **argv) {
  auto cd = etch_context_desc_default();
  cd.headless = true;
  auto *ctx = etch_create_context(&cd);
  if (!ctx)
    return 1;
  const auto *backend = inlay_portal_mock_backend();
  void *raw = backend->create(nullptr, 32, 32);
  InlayPortalDirtyRect dirty[1] = {};
  const bool retained = raw && backend->poll_dirty_rects(raw, dirty, 1) == 1;
  backend->ack_dirty_rects(raw, false);
  const bool retried = backend->poll_dirty_rects(raw, dirty, 1) == 1;
  backend->ack_dirty_rects(raw, true);
  const bool accepted = backend->poll_dirty_rects(raw, dirty, 1) == 0;
  backend->destroy(raw);
  auto invalid_api = *backend;
  invalid_api.poll_dirty_rects = invalid_poll;
  invalid_api.ack_dirty_rects = record_ack;
  auto invalid_desc = inlay_portal_desc_default();
  invalid_desc.backend = &invalid_api;
  invalid_desc.mode = INLAY_PORTAL_MODE_UNDERLAY;
  auto *invalid = inlay_portal_create(ctx, &invalid_desc);
  const bool rejected_batch =
      invalid && !inlay_portal_poll(invalid) && rejected == 1;
  inlay_portal_destroy(invalid);
  auto d = inlay_portal_desc_default();
  d.width = 320;
  d.height = 180;
  auto *p = inlay_portal_create(ctx, &d);
  if (!p) {
    etch_destroy_context(ctx);
    return 1;
  }
  bool ok =
      rejected_batch && retained && retried && accepted && inlay_portal_poll(p);
  auto stats = inlay_portal_stats(p);
  ok &= stats.uploaded_bytes == 320 * 180 * 4;
  size_t idle_arena_bytes = 0;
  const bool init_ok = ok;
  for (int f = 0; f < 32; ++f) {
    auto fd = etch_frame_desc_default();
    fd.viewport_w = 700;
    fd.viewport_h = 500;
    etch_begin_frame(ctx, &fd);
    auto canvas = etch_canvas_desc_default();
    canvas.id = etch_id("portal_canvas");
    canvas.width = etch_sizing_fixed(700);
    canvas.height = etch_sizing_fixed(500);
    canvas.default_pan = {120, 80};
    canvas.default_zoom = .65f;
    canvas.show_grid = false;
    etch_begin_canvas(&canvas);
    auto node = etch_canvas_node_desc_default();
    node.id = etch_id("portal_node");
    node.width_world = etch_sizing_fixed(320);
    node.height_world = etch_sizing_fixed(180);
    node.pos_value = {20, 30};
    etch_begin_canvas_node(&node);
    InlayPortalViewDesc view = inlay_portal_view_desc_default();
    view.id = etch_id("portal");
    view.width = 320;
    view.height = 180;
    view.radius = 12;
    inlay_portal_view(p, &view);
    etch_end_canvas_node();
    etch_end_canvas();
    etch_end_frame(ctx);
    ok &= inlay_portal_sync_geometry(p);
    const auto frame = etch_frame_stats(ctx);
    if (f == 4)
      idle_arena_bytes = frame.arena_used_bytes;
    if (f > 4)
      ok &= frame.arena_used_bytes == idle_arena_bytes &&
            frame.arena_used_bytes < 256 * 1024 && frame.idle_sleeping;
    ok &= inlay_portal_poll(p) &&
          inlay_portal_stats(p).uploaded_bytes == stats.uploaded_bytes;
  }
  const bool loop_ok = ok;
  EtchElementGeometry g = {};
  ok &= etch_element_geometry(etch_id("portal"), &g);
  ok &= std::fabs(g.local_scale.x - .65f) < .001f;
  ok &= !inlay_portal_inject_pointer(p, {g.bounds.x, g.bounds.y}, true);
  inlay_portal_inject_pointer(p, {g.bounds.x, g.bounds.y}, false);
  ok &= inlay_portal_inject_pointer(
      p, {g.bounds.x + 30 * .65f, g.bounds.y + 30 * .65f}, true);
  ok &= inlay_portal_poll(p);
  const auto delta =
      inlay_portal_stats(p).uploaded_bytes - stats.uploaded_bytes;
  ok &= delta == 80 * 40 * 4;
  stats = inlay_portal_stats(p);
  ok &= inlay_portal_poll(p) &&
        inlay_portal_stats(p).uploaded_bytes == stats.uploaded_bytes;
  inlay_portal_inject_text(p, ETCH_STR_LIT("abc"));
  ok &= inlay_portal_poll(p);
  EtchRect ime = {};
  ok &= inlay_portal_query_ime_rect(p, &ime);
  ok &= std::fabs(ime.x - (g.bounds.x + 48 * .65f)) < .01f &&
        std::fabs(ime.h - 18 * .65f) < .01f;
  // Capture releases outside, while an uncaptured outside press never routes.
  ok &= inlay_portal_inject_pointer(p, {-100, -100}, false);
  ok &= !inlay_portal_inject_pointer(p, {-100, -100}, true);
  ok &= !inlay_portal_has_focus(p);
  ok &= !inlay_portal_inject_pointer(
      p, {g.bounds.x + 30 * .65f, g.bounds.y + 30 * .65f}, true);
  inlay_portal_inject_pointer(p, {-100, -100}, false);
  ok &= inlay_portal_inject_pointer(
      p, {g.bounds.x + 30 * .65f, g.bounds.y + 30 * .65f}, true);
  inlay_portal_set_input_enabled(p, false);
  ok &= !inlay_portal_inject_pointer(
      p, {g.bounds.x + 30 * .65f, g.bounds.y + 30 * .65f}, true);
  ok &= !inlay_portal_query_ime_rect(p, &ime);
  inlay_portal_set_input_enabled(p, true);
  ok &= inlay_portal_mock_advance(p, 64) && inlay_portal_poll(p);
  stats = inlay_portal_stats(p);
  ok &= inlay_portal_mock_advance(p, 64) && inlay_portal_poll(p) &&
        inlay_portal_stats(p).uploaded_bytes == stats.uploaded_bytes;
  ok &= !inlay_portal_resize(p, 200, 100) && !inlay_portal_destroy(p);
  ok &= etch_render_headless_png(ctx, 700, 500,
                                 argc > 1 ? argv[1] : "portal-check.png",
                                 etch_color_black(255));
  inlay_portal_frame_submitted(p);
  ok &= inlay_portal_resize(p, 200, 100) && inlay_portal_poll(p);
  ok &= !inlay_portal_resize(p, 0, 100);
  ok &= inlay_portal_destroy(p);
  const bool tex_ok = ok;
  d.mode = INLAY_PORTAL_MODE_UNDERLAY;
  p = inlay_portal_create(ctx, &d);
  ok &= p != nullptr;
  auto fd = etch_frame_desc_default();
  fd.viewport_w = 700;
  fd.viewport_h = 500;
  auto *doc = inlay_doc_create(ctx, nullptr);
  ok &= inlay_doc_set_utf8(doc, ETCH_STR_LIT("Portal follows this document.\n\nFlow reserves its full height."));
  auto flow = inlay_doc_view_desc_default();
  flow.id = etch_id("portal-flow");
  flow.width_px = 360;
  flow.height_px = 0;
  for (int settle = 0; settle < 3; ++settle) {
    etch_begin_frame(ctx, &fd);
    auto root = etch_box_desc_default();
    root.width = etch_sizing_fixed(700);
    root.height = etch_sizing_fixed(500);
    etch_begin_box(&root);
    InlayPortalViewDesc underlay = inlay_portal_view_desc_default();
    underlay.id = etch_id("underlay");
    underlay.width = 240;
    underlay.height = 140;
    underlay.radius = 20;
    inlay_portal_view(p, &underlay);
    auto overlay = etch_box_desc_default();
    overlay.id = etch_id("portal-overlay");
    overlay.width = etch_sizing_fixed(80);
    overlay.height = etch_sizing_fixed(40);
    overlay.background = etch_rgba8(240, 40, 30, 255);
    overlay.backdrop_blur = 6;
    overlay.attach_to = ETCH_ATTACH_ROOT;
    overlay.anchor_parent = ETCH_ANCHOR_TOP_LEFT;
    overlay.floating_offset = {40, 40};
    overlay.z_index = 20;
    overlay.clip_to_parent = false;
    etch_begin_box(&overlay);
    etch_end_box();
    etch_end_box();
    etch_end_frame(ctx);
    ok &= inlay_portal_sync_geometry(p) && !inlay_portal_stats(p).has_texture;
  }
  const auto overlay_bounds = etch_element_bounds(etch_id("portal-overlay"));
  ok &= overlay_bounds.y == 40;
  ok &= etch_render_headless_png(ctx, 700, 500,
                                 argc > 2 ? argv[2] : "portal-underlay.png",
                                 etch_color_black(255));
  inlay_portal_frame_submitted(p);
  ok &= inlay_portal_destroy(p);
  const bool underlay_ok = ok;
  auto visibility_api = *backend;
  visibility_api.set_underlay_bounds = record_bounds;
  d.backend = &visibility_api;
  p = inlay_portal_create(ctx, &d);
  for (int step = 0; step < 3; ++step) {
    const bool hidden = (step == 1);
    etch_begin_frame(ctx, &fd);
    auto parent = etch_box_desc_default();
    parent.width = etch_sizing_fixed(400);
    parent.height = etch_sizing_fixed(300);
    parent.axis = ETCH_AXIS_COL;
    parent.opacity = hidden ? 0.f : 1.f;
    etch_begin_box(&parent);
    const auto measured = inlay_doc_view(doc, &flow);
    ok &= measured.visible_end == 2 && measured.measured_height_px > 0;
    InlayPortalViewDesc view = inlay_portal_view_desc_default();
    view.id = etch_id("native-visibility");
    view.width = 240;
    view.height = 140;
    view.radius = 0;
    inlay_portal_view(p, &view);
    etch_end_box();
    etch_end_frame(ctx);
    ok &= inlay_portal_sync_geometry(p);
    ok &= hidden ? native_visible.w == 0 && native_visible.h == 0
                 : native_visible.w > 0 && native_visible.h > 0;
    if (!hidden) {
      const auto bounds = etch_element_bounds(flow.id);
      ok &= bounds.h > 0 && native_visible.y >= bounds.y + bounds.h;
      const EtchVec2 center = {native_visible.x + native_visible.w * .5f,
                               native_visible.y + native_visible.h * .5f};
      // Step 2 verifies hiding in step 1 did NOT permanently clobber host input policy!
      ok &= inlay_portal_inject_pointer(p, center, true) && inlay_portal_has_focus(p);
      inlay_portal_inject_pointer(p, center, false);
      if (step == 2) {
        // Explicit host disable stays disabled even while geometrically visible.
        inlay_portal_set_input_enabled(p, false);
        ok &= !inlay_portal_inject_pointer(p, center, true) && !inlay_portal_has_focus(p);
        inlay_portal_inject_pointer(p, center, false);
      }
    } else {
      ok &= !inlay_portal_has_focus(p);
    }
    inlay_portal_frame_submitted(p);
  }
  ok &= inlay_portal_destroy(p);
  // Security policy, origin allowlist, navigation history, message bridge & end_frame_sync.
  bool policy_ok = true;
  {
    auto pd = inlay_portal_desc_default();
    pd.width = 240;
    pd.height = 140;
    auto *sp = inlay_portal_create(ctx, &pd);
    policy_ok &= sp != nullptr;
    if (sp) {
      // Initial empty history state.
      policy_ok &= !inlay_portal_can_go_back(sp) &&
                   !inlay_portal_can_go_forward(sp) &&
                   inlay_portal_current_url(sp).length == 0u &&
                   !inlay_portal_go_back(sp) &&
                   !inlay_portal_go_forward(sp) &&
                   !inlay_portal_reload(sp);

      // Origin allowlist with owned copy (mutate stack buffer after set_policy).
      char origins_buf[64];
      std::memcpy(origins_buf, "https://app.inlay.dev, http://localhost:8080", 45);
      InlayPortalPolicy pol = inlay_portal_policy_default();
      policy_ok &= pol.struct_size == static_cast<uint32_t>(sizeof(InlayPortalPolicy));
      pol.allowed_origins_csv = {origins_buf, 44u, false};
      inlay_portal_set_policy(sp, &pol);
      std::memset(origins_buf, 'X', sizeof(origins_buf));

      policy_ok &= inlay_portal_load_url(sp, ETCH_STR_LIT("https://app.inlay.dev/home"));
      policy_ok &= str_eq(inlay_portal_current_url(sp), "https://app.inlay.dev/home");
      policy_ok &= inlay_portal_load_url(sp, ETCH_STR_LIT("HTTPS://APP.INLAY.DEV:443/settings"));
      policy_ok &= inlay_portal_load_url(sp, ETCH_STR_LIT("http://localhost:8080/mcp"));
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("http://localhost:9090/mcp"));
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("https://evil.example.org/phish"));
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("https://app.inlay.dev.evil.org/"));
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("https://app.inlay.dev@evil.org/"));
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("javascript:alert(1)"));
      policy_ok &= str_eq(inlay_portal_current_url(sp), "http://localhost:8080/mcp");

      // on_navigate callback: BLOCK, OPEN_EXTERNAL, and ALLOW override.
      PolicyProbe probe = {};
      pol = inlay_portal_policy_default();
      pol.allowed_origins_csv = ETCH_STR_LIT("https://app.inlay.dev");
      pol.on_navigate = on_test_navigate;
      pol.on_message = on_test_message;
      pol.user_data = &probe;
      inlay_portal_set_policy(sp, &pol);

      probe.decision = INLAY_PORTAL_NAV_BLOCK;
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("https://app.inlay.dev/blocked"));
      policy_ok &= probe.nav_calls == 1u && !probe.last_new_window &&
                   std::strcmp(probe.last_url, "https://app.inlay.dev/blocked") == 0 &&
                   str_eq(inlay_portal_current_url(sp), "http://localhost:8080/mcp");

      probe.decision = INLAY_PORTAL_NAV_OPEN_EXTERNAL;
      policy_ok &= !inlay_portal_load_url(sp, ETCH_STR_LIT("https://docs.example.com/ext"));
      policy_ok &= probe.nav_calls == 2u &&
                   str_eq(inlay_portal_current_url(sp), "http://localhost:8080/mcp");

      probe.decision = INLAY_PORTAL_NAV_ALLOW;
      policy_ok &= inlay_portal_load_url(sp, ETCH_STR_LIT("https://override.example.com/ok"));
      policy_ok &= probe.nav_calls == 3u &&
                   str_eq(inlay_portal_current_url(sp), "https://override.example.com/ok");

      // History back/forward/reload and forward truncation on new load.
      inlay_portal_set_policy(sp, nullptr);
      auto *hp = inlay_portal_create(ctx, &pd);
      policy_ok &= hp != nullptr;
      if (hp) {
        policy_ok &= inlay_portal_poll(hp);
        policy_ok &= inlay_portal_load_url(hp, ETCH_STR_LIT("https://nav.inlay.dev/a"));
        policy_ok &= !inlay_portal_can_go_back(hp) && !inlay_portal_can_go_forward(hp);
        policy_ok &= inlay_portal_load_url(hp, ETCH_STR_LIT("https://nav.inlay.dev/b"));
        policy_ok &= inlay_portal_can_go_back(hp) && !inlay_portal_can_go_forward(hp) &&
                     str_eq(inlay_portal_current_url(hp), "https://nav.inlay.dev/b");
        policy_ok &= inlay_portal_go_back(hp) &&
                     !inlay_portal_can_go_back(hp) && inlay_portal_can_go_forward(hp) &&
                     str_eq(inlay_portal_current_url(hp), "https://nav.inlay.dev/a");
        policy_ok &= !inlay_portal_go_back(hp);
        policy_ok &= inlay_portal_go_forward(hp) &&
                     inlay_portal_can_go_back(hp) && !inlay_portal_can_go_forward(hp) &&
                     str_eq(inlay_portal_current_url(hp), "https://nav.inlay.dev/b");
        policy_ok &= inlay_portal_poll(hp);
        const uint64_t before_reload = inlay_portal_stats(hp).uploaded_bytes;
        policy_ok &= inlay_portal_reload(hp) && inlay_portal_poll(hp) &&
                     inlay_portal_stats(hp).uploaded_bytes > before_reload &&
                     str_eq(inlay_portal_current_url(hp), "https://nav.inlay.dev/b");
        policy_ok &= inlay_portal_go_back(hp) &&
                     inlay_portal_load_url(hp, ETCH_STR_LIT("https://nav.inlay.dev/c")) &&
                     inlay_portal_can_go_back(hp) && !inlay_portal_can_go_forward(hp) &&
                     str_eq(inlay_portal_current_url(hp), "https://nav.inlay.dev/c");
        policy_ok &= inlay_portal_destroy(hp);
      }

      // Bidirectional host<->web message bridge (post_message + on_message + peek/poll_message).
      inlay_portal_set_policy(sp, &pol);
      char msg_buf[128] = {};
      policy_ok &= inlay_portal_peek_message_size(nullptr) == 0u &&
                   inlay_portal_peek_message_size(sp) == 0u;
      policy_ok &= inlay_portal_poll_message(
                       sp, msg_buf, static_cast<uint32_t>(sizeof(msg_buf))) == 0u;
      policy_ok &= !inlay_portal_post_message(nullptr, ETCH_STR_LIT("{\"a\":1}"));
      policy_ok &= !inlay_portal_post_message(sp, ETCH_STR_LIT(""));
      const char bad_utf8[2] = {static_cast<char>(0xFF), static_cast<char>(0xFE)};
      policy_ok &= !inlay_portal_post_message(sp, {bad_utf8, 2u, false});

      const EtchStr m1 = ETCH_STR_LIT("{\"type\":\"ping\",\"id\":1}");
      const EtchStr m2 = ETCH_STR_LIT("{\"type\":\"tool\",\"ok\":true}");
      policy_ok &= inlay_portal_post_message(sp, m1);
      policy_ok &= probe.msg_calls == 1u &&
                   std::strcmp(probe.last_msg, "{\"type\":\"ping\",\"id\":1}") == 0;
      policy_ok &= inlay_portal_post_message(sp, m2);
      policy_ok &= probe.msg_calls == 2u;

      policy_ok &= inlay_portal_peek_message_size(sp) == m1.length &&
                   inlay_portal_poll_message(sp, nullptr, 0u) == m1.length &&
                   inlay_portal_peek_message_size(sp) == m1.length;
      const uint32_t len1 = inlay_portal_poll_message(
          sp, msg_buf, static_cast<uint32_t>(sizeof(msg_buf)));
      policy_ok &= len1 == m1.length &&
                   std::strcmp(msg_buf, "{\"type\":\"ping\",\"id\":1}") == 0;
      policy_ok &= inlay_portal_peek_message_size(sp) == m2.length;
      char tiny_buf[6] = {};
      const uint32_t len2 = inlay_portal_poll_message(
          sp, tiny_buf, static_cast<uint32_t>(sizeof(tiny_buf)));
      policy_ok &= len2 == m2.length && tiny_buf[5] == '\0' &&
                   std::strcmp(tiny_buf, "{\"typ") == 0;
      // Short buffer must preserve m2 in the queue (including exact len without room for NUL).
      policy_ok &= inlay_portal_peek_message_size(sp) == m2.length &&
                   inlay_portal_poll_message(sp, msg_buf, m2.length) == m2.length &&
                   inlay_portal_peek_message_size(sp) == m2.length;
      const uint32_t len2_full = inlay_portal_poll_message(
          sp, msg_buf, static_cast<uint32_t>(sizeof(msg_buf)));
      policy_ok &= len2_full == m2.length &&
                   std::strcmp(msg_buf, "{\"type\":\"tool\",\"ok\":true}") == 0;
      policy_ok &= inlay_portal_peek_message_size(sp) == 0u &&
                   inlay_portal_poll_message(
                       sp, msg_buf, static_cast<uint32_t>(sizeof(msg_buf))) == 0u &&
                   msg_buf[0] == '\0';

      // Unified inlay_portal_end_frame_sync lifecycle helper.
      policy_ok &= !inlay_portal_end_frame_sync(nullptr, true);
      for (int sync_step = 0; sync_step < 2; ++sync_step) {
        const bool allow_input = (sync_step == 1);
        etch_begin_frame(ctx, &fd);
        auto box = etch_box_desc_default();
        box.width = etch_sizing_fixed(400);
        box.height = etch_sizing_fixed(300);
        etch_begin_box(&box);
        InlayPortalViewDesc sv = inlay_portal_view_desc_default();
        sv.id = etch_id("portal-end-frame-sync");
        sv.width = 240;
        sv.height = 140;
        inlay_portal_view(sp, &sv);
        etch_end_box();
        etch_end_frame(ctx);
        policy_ok &= !inlay_portal_resize(sp, 200, 120);
        policy_ok &= inlay_portal_end_frame_sync(sp, allow_input);
        const auto sb = etch_element_bounds(sv.id);
        const EtchVec2 mid = {sb.x + sb.w * .5f, sb.y + sb.h * .5f};
        const bool routed = inlay_portal_inject_pointer(sp, mid, true);
        inlay_portal_inject_pointer(sp, mid, false);
        policy_ok &= (routed == allow_input) &&
                     (inlay_portal_has_focus(sp) == allow_input);
      }
      policy_ok &= inlay_portal_resize(sp, 200, 120);
      policy_ok &= inlay_portal_destroy(sp);
    }
  }
  ok &= policy_ok;
  inlay_doc_destroy(doc);
  etch_destroy_context(ctx);
  if (!ok) {
    std::fprintf(stderr, "Portal diag: init=%d loop=%d tex=%d underlay=%d policy=%d final=%d scale=(%.3f,%.3f)\n",
                 init_ok, loop_ok, tex_ok, underlay_ok, policy_ok, ok, g.local_scale.x, g.local_scale.y);
  }
  std::printf("Portal tiles/zoom/IME: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
