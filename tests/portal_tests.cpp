#include <cmath>
#include <cstdio>
#include <etch/etch.h>
#include <inlay/inlay_c.h>
namespace {
uint32_t rejected = 0;
EtchRect native_visible = {};
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
    const InlayPortalViewDesc view = {etch_id("portal"), 320, 180, 12};
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
    const InlayPortalViewDesc underlay = {etch_id("underlay"), 240, 140, 20};
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
  auto visibility_api = *backend;
  visibility_api.set_underlay_bounds = record_bounds;
  d.backend = &visibility_api;
  p = inlay_portal_create(ctx, &d);
  for (int hidden = 0; hidden < 2; ++hidden) {
    etch_begin_frame(ctx, &fd);
    auto parent = etch_box_desc_default();
    parent.width = etch_sizing_fixed(400);
    parent.height = etch_sizing_fixed(300);
    parent.axis = ETCH_AXIS_COL;
    parent.opacity = hidden ? 0.f : 1.f;
    etch_begin_box(&parent);
    const auto measured = inlay_doc_view(doc, &flow);
    ok &= measured.visible_end == 2 && measured.measured_height_px > 0;
    const InlayPortalViewDesc view = {etch_id("native-visibility"), 240, 140, 0};
    inlay_portal_view(p, &view);
    etch_end_box();
    etch_end_frame(ctx);
    ok &= inlay_portal_sync_geometry(p);
    ok &= hidden ? native_visible.w == 0 && native_visible.h == 0
                 : native_visible.w > 0 && native_visible.h > 0;
    if (!hidden) {
      const auto bounds = etch_element_bounds(flow.id);
      ok &= bounds.h > 0 && native_visible.y >= bounds.y + bounds.h;
    }
    inlay_portal_frame_submitted(p);
  }
  ok &= inlay_portal_destroy(p);
  inlay_doc_destroy(doc);
  etch_destroy_context(ctx);
  std::printf("Portal tiles/zoom/IME: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
