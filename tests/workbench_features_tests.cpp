#include <inlay/inlay_c.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool check(bool cond, const char* msg) {
    if (!cond) std::printf("FAIL: %s\n", msg);
    return cond;
}

bool str_eq(EtchStr s, const char* lit) {
    const size_t n = std::strlen(lit);
    return s.length == n && (n == 0u || (s.chars != nullptr && std::memcmp(s.chars, lit, n) == 0));
}

bool color_eq(EtchColor a, EtchColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool near_f(float a, float b) {
    return std::fabs(a - b) < 1e-3f;
}

// ============================================================================
// 1. InlayStream Tests (#10, #16)
// ============================================================================

bool test_stream() {
    bool ok = true;
    InlayStream* s = inlay_stream_create();
    if (!check(s != nullptr, "stream create")) return false;

    ok &= check(inlay_stream_count(s) == 0u && inlay_stream_cursor(s) == 0u, "initial stream empty");
    ok &= check(!inlay_stream_upsert(nullptr, nullptr) && !inlay_stream_upsert(s, nullptr), "null upsert rejected");

    InlayStreamItem bad_zero = {};
    bad_zero.id = 0u;
    ok &= check(!inlay_stream_upsert(s, &bad_zero), "zero id upsert rejected");

    InlayStreamItem a = {};
    a.id = 1001u;
    a.message_id = 10u;
    a.role = INLAY_STREAM_ROLE_ASSISTANT;
    a.kind = INLAY_STREAM_ITEM_MARKDOWN;
    a.status = INLAY_STREAM_STATUS_RUNNING;
    a.progress = 0.0f;
    a.title = ETCH_STR_LIT("Answer");
    a.body = ETCH_STR_LIT("Hello ");
    a.metadata_json = ETCH_STR_LIT("{\"model\":\"test\"}");

    InlayStreamItem tool1 = {};
    tool1.id = 0x123456789ABCDEF0ull;
    tool1.message_id = 10u;
    tool1.role = INLAY_STREAM_ROLE_TOOL;
    tool1.kind = INLAY_STREAM_ITEM_TOOL_CALL;
    tool1.status = INLAY_STREAM_STATUS_RUNNING;
    tool1.progress = 0.25f;
    tool1.title = ETCH_STR_LIT("grep_search");
    tool1.body = ETCH_STR_LIT("{\"q\":\"etch\"}");
    tool1.metadata_json = ETCH_STR_LIT("{\"parallel\":1}");

    InlayStreamItem tool2 = {};
    tool2.id = 3003u;
    tool2.message_id = 10u;
    tool2.role = INLAY_STREAM_ROLE_TOOL;
    tool2.kind = INLAY_STREAM_ITEM_TOOL_RESULT;
    tool2.status = INLAY_STREAM_STATUS_PENDING;
    tool2.progress = 0.0f;
    tool2.title = ETCH_STR_LIT("read_file");
    tool2.body = ETCH_STR_LIT("");

    ok &= check(inlay_stream_upsert(s, &a) && inlay_stream_upsert(s, &tool1) && inlay_stream_upsert(s, &tool2),
                "stream insert 3 items including 64-bit id");
    ok &= check(inlay_stream_count(s) == 3u && inlay_stream_cursor(s) == 3u, "stream count and cursor after inserts");

    // In-place parallel tool update by stable 64-bit ID (preserves order and handles self-aliasing)
    InlayStreamItem fetched = {};
    ok &= check(inlay_stream_get_by_id(s, 0x123456789ABCDEF0ull, &fetched), "get_by_id 64-bit");
    ok &= check(fetched.revision == 1u && str_eq(fetched.title, "grep_search"), "initial tool1 revision and title");
    fetched.status = INLAY_STREAM_STATUS_DONE;
    fetched.progress = 1.0f;
    ok &= check(inlay_stream_upsert(s, &fetched), "self-aliased upsert update");
    ok &= check(inlay_stream_cursor(s) == 4u, "cursor increments on upsert update");

    InlayStreamItem at0 = {}, at1 = {}, at2 = {};
    ok &= check(inlay_stream_at(s, 0u, &at0) && inlay_stream_at(s, 1u, &at1) && inlay_stream_at(s, 2u, &at2),
                "stream_at valid indices");
    ok &= check(!inlay_stream_at(s, 3u, &at0), "stream_at out-of-range rejected");
    ok &= check(at0.id == 1001u && at1.id == 0x123456789ABCDEF0ull && at2.id == 3003u,
                "upsert update preserves item order");
    ok &= check(at1.revision == 2u && at1.status == INLAY_STREAM_STATUS_DONE && near_f(at1.progress, 1.0f),
                "upsert update bumps revision and updates fields");

    // Streaming append_body with UTF-8, quotes, backslashes, newlines, and amortized growth
    const uint64_t cursor_before_append = inlay_stream_cursor(s);
    ok &= check(inlay_stream_append_body(s, 1001u, ETCH_STR_LIT("world \"quoted\" \\path\n")), "append_body chunk 1");
    ok &= check(inlay_stream_append_body(s, 1001u, ETCH_STR_LIT("UTF8: 你好!")), "append_body chunk 2");
    for (int i = 0; i < 40; ++i) {
        ok &= inlay_stream_append_body(s, 1001u, ETCH_STR_LIT("."));
    }
    ok &= check(!inlay_stream_append_body(s, 999999u, ETCH_STR_LIT("x")), "append_body missing id returns false");
    ok &= check(inlay_stream_cursor(s) == cursor_before_append + 42u, "append_body cursor monotonic");

    ok &= check(inlay_stream_get_by_id(s, 1001u, &fetched), "get updated streaming item");
    ok &= check(fetched.revision == 43u, "append_body bumps revision");
    ok &= check(fetched.body.length > 40u && std::strstr(fetched.body.chars, "Hello world \"quoted\" \\path\nUTF8: 你好!") != nullptr,
                "append_body concatenated UTF-8 content");

    // Remove middle item preserving order
    const uint64_t cursor_before_rm = inlay_stream_cursor(s);
    ok &= check(inlay_stream_remove(s, 0x123456789ABCDEF0ull), "remove middle item");
    ok &= check(!inlay_stream_remove(s, 0x123456789ABCDEF0ull), "double remove returns false");
    ok &= check(inlay_stream_count(s) == 2u && inlay_stream_cursor(s) == cursor_before_rm + 1u,
                "remove updates count and cursor");
    ok &= check(inlay_stream_at(s, 0u, &at0) && inlay_stream_at(s, 1u, &at1) &&
                at0.id == 1001u && at1.id == 3003u,
                "remove preserves remaining order");

    // Export JSON sizing query, truncation safety, and full roundtrip
    const uint32_t need = inlay_stream_export_json(s, nullptr, 0u);
    ok &= check(need > 64u, "export_json reports required bytes");
    char small_buf[16] = {};
    const uint32_t need2 = inlay_stream_export_json(s, small_buf, sizeof(small_buf));
    ok &= check(need2 == need && small_buf[15] == '\0', "export_json small buffer NUL-terminated");

    auto* json_buf = static_cast<char*>(std::malloc(size_t(need) + 1u));
    if (!check(json_buf != nullptr, "alloc export buffer")) {
        inlay_stream_destroy(s);
        return false;
    }
    const uint32_t written = inlay_stream_export_json(s, json_buf, need + 1u);
    ok &= check(written == need && json_buf[need] == '\0', "export_json exact length");

    InlayStream* s2 = inlay_stream_create();
    ok &= check(s2 != nullptr && inlay_stream_import_json(s2, {json_buf, need, false}),
                "import_json roundtrip succeeds");
    if (s2) {
        ok &= check(inlay_stream_cursor(s2) == inlay_stream_cursor(s) &&
                    inlay_stream_count(s2) == inlay_stream_count(s),
                    "import_json restores cursor and count");
        InlayStreamItem r0 = {}, r1 = {};
        ok &= check(inlay_stream_at(s2, 0u, &r0) && inlay_stream_at(s2, 1u, &r1), "imported items readable");
        ok &= check(r0.id == 1001u && r0.message_id == 10u && r0.role == INLAY_STREAM_ROLE_ASSISTANT &&
                    r0.kind == INLAY_STREAM_ITEM_MARKDOWN && r0.status == INLAY_STREAM_STATUS_RUNNING &&
                    r0.revision == 43u && str_eq(r0.title, "Answer") &&
                    r0.body.length == fetched.body.length &&
                    std::memcmp(r0.body.chars, fetched.body.chars, fetched.body.length) == 0 &&
                    str_eq(r0.metadata_json, "{\"model\":\"test\"}"),
                    "imported item 0 exact roundtrip");
        ok &= check(r1.id == 3003u && r1.role == INLAY_STREAM_ROLE_TOOL &&
                    r1.kind == INLAY_STREAM_ITEM_TOOL_RESULT && r1.status == INLAY_STREAM_STATUS_PENDING &&
                    str_eq(r1.title, "read_file") && r1.body.length == 0u,
                    "imported item 1 exact roundtrip");

        // Invalid JSON import must fail atomically leaving s2 untouched
        const uint64_t saved_cursor = inlay_stream_cursor(s2);
        const uint32_t saved_count = inlay_stream_count(s2);
        ok &= check(!inlay_stream_import_json(s2, ETCH_STR_LIT("")), "import rejects empty");
        ok &= check(!inlay_stream_import_json(s2, ETCH_STR_LIT("{broken")), "import rejects broken JSON");
        ok &= check(!inlay_stream_import_json(s2, ETCH_STR_LIT("{\"cursor\":9}")), "import rejects missing items");
        ok &= check(!inlay_stream_import_json(s2, ETCH_STR_LIT("{\"cursor\":9,\"items\":[{\"id\":0}]}")),
                    "import rejects zero id item");
        ok &= check(!inlay_stream_import_json(s2, ETCH_STR_LIT("{\"cursor\":9,\"items\":[]} trailing")),
                    "import rejects trailing tokens");
        ok &= check(inlay_stream_cursor(s2) == saved_cursor && inlay_stream_count(s2) == saved_count,
                    "failed import leaves existing stream untouched");
        inlay_stream_destroy(s2);
    }

    std::free(json_buf);
    inlay_stream_destroy(s);
    return ok;
}

// ============================================================================
// 2. GenUI Schema Validator & Safe Surface Renderer Tests (#13)
// ============================================================================

struct ActionCapture {
    int count;
    char last_payload[256];
};

void on_genui_action(InlaySurface*, EtchStr payload, void* user_data) {
    auto* cap = static_cast<ActionCapture*>(user_data);
    ++cap->count;
    const uint32_t take = payload.length < sizeof(cap->last_payload) - 1u
                        ? payload.length
                        : static_cast<uint32_t>(sizeof(cap->last_payload) - 1u);
    if (take > 0u && payload.chars) std::memcpy(cap->last_payload, payload.chars, take);
    cap->last_payload[take] = '\0';
}

uint32_t find_surface_node_eid(const InlayUiNode* nodes, uint32_t count, uint32_t idx,
                               uint32_t parent_eid, InlayUiKind target) {
    if (idx >= count) return 0u;
    const uint32_t eid = etch_id_index(parent_eid, nodes[idx].id);
    if (nodes[idx].kind == target) return eid;
    for (uint32_t c = nodes[idx].first_child; c < count; c = nodes[c].next_sibling) {
        const uint32_t found = find_surface_node_eid(nodes, count, c, eid, target);
        if (found != 0u) return found;
    }
    return 0u;
}

bool test_genui(EtchContext* ctx) {
    bool ok = true;

    const EtchStr valid_spec = ETCH_STR_LIT(
        "{"
        "\"type\":\"col\",\"id\":\"root\",\"cls\":\"p-3 gap-2\","
        "\"children\":["
        "  {\"type\":\"card\",\"cls\":\"p-2 gap-2\",\"children\":["
        "    {\"type\":\"row\",\"cls\":\"gap-2 items-center\",\"children\":["
        "      {\"type\":\"text\",\"text\":\"Deploy Summary <v1>\"},"
        "      {\"type\":\"badge\",\"label\":\"ready\"}"
        "    ]},"
        "    {\"type\":\"progress\",\"value\":0.65,\"label\":\"65%\"},"
        "    {\"type\":\"divider\"},"
        "    {\"type\":\"spacer\",\"value\":8},"
        "    {\"type\":\"input\",\"value\":\"query\",\"label\":\"Filter...\",\"action\":\"filter.update\"},"
        "    {\"type\":\"toggle\",\"label\":\"Auto-retry\",\"value\":true,\"action\":\"retry:toggle\"},"
        "    {\"type\":\"markdown\",\"text\":\"**Bold** and `code`\"},"
        "    {\"type\":\"math\",\"text\":\"x^2 + y^2 = r^2\"},"
        "    {\"type\":\"diagram\",\"text\":\"flowchart LR\\nA-->B\"},"
        "    {\"type\":\"button\",\"id\":\"go\",\"text\":\"Approve\",\"action\":\"deploy.approve\",\"payload\":{\"id\":42,\"ok\":true}},"
        "    {\"type\":\"button\",\"text\":\"Approve Again\",\"action\":\"deploy.approve\"}"
        "  ]}"
        "]"
        "}"
    );

    InlayGenUiValidation v = {};
    ok &= check(inlay_genui_validate_json(valid_spec, 1u, &v), "valid GenUI spec passes");
    ok &= check(v.valid && v.node_count == 15u && v.error_message[0] == '\0',
                "valid GenUI node count and empty error");

    // Action listing & deduplication
    char actions_buf[256] = {};
    const uint32_t act_need = inlay_genui_list_actions(valid_spec, nullptr, 0u);
    const uint32_t act_written = inlay_genui_list_actions(valid_spec, actions_buf, sizeof(actions_buf));
    ok &= check(act_need == act_written &&
                std::strcmp(actions_buf, "[\"filter.update\",\"retry:toggle\",\"deploy.approve\"]") == 0,
                "list_actions exports deduplicated JSON array");

    // Schema version mismatch
    ok &= check(!inlay_genui_validate_json(valid_spec, 2u, &v) && !v.valid && v.error_message[0] != '\0',
                "unsupported schema_version rejected");

    // Unknown component type at child index 1
    const EtchStr unknown_type = ETCH_STR_LIT(
        "{\"type\":\"col\",\"children\":[{\"type\":\"iframe\",\"text\":\"evil\"}]}"
    );
    ok &= check(!inlay_genui_validate_json(unknown_type, 1u, &v) && !v.valid &&
                v.error_node_index == 1u && v.error_offset > 0u && v.error_message[0] != '\0',
                "unknown component type rejected with node index");

    // Disallowed executable keys (direct and Unicode-escaped)
    const EtchStr inject_script = ETCH_STR_LIT("{\"type\":\"box\",\"script\":\"alert(1)\"}");
    ok &= check(!inlay_genui_validate_json(inject_script, 1u, &v) && !v.valid && v.error_node_index == 0u,
                "script key rejected");

    const EtchStr inject_eval = ETCH_STR_LIT("{\"type\":\"box\",\"eval\":\"1+1\"}");
    ok &= check(!inlay_genui_validate_json(inject_eval, 1u, &v) && !v.valid, "eval key rejected");

    const EtchStr inject_onclick = ETCH_STR_LIT("{\"type\":\"button\",\"text\":\"x\",\"onClick\":\"hack()\"}");
    ok &= check(!inlay_genui_validate_json(inject_onclick, 1u, &v) && !v.valid, "onClick key rejected");

    const EtchStr inject_proto = ETCH_STR_LIT("{\"type\":\"box\",\"__proto__\":{}}");
    ok &= check(!inlay_genui_validate_json(inject_proto, 1u, &v) && !v.valid, "__proto__ key rejected");

    const EtchStr inject_escaped = ETCH_STR_LIT("{\"type\":\"box\",\"\\u0073cript\":\"alert(1)\"}");
    ok &= check(!inlay_genui_validate_json(inject_escaped, 1u, &v) && !v.valid,
                "unicode-escaped script key rejected");

    // Unsafe action expression
    const EtchStr bad_action = ETCH_STR_LIT("{\"type\":\"button\",\"text\":\"x\",\"action\":\"foo();evil()\"}");
    ok &= check(!inlay_genui_validate_json(bad_action, 1u, &v) && !v.valid,
                "JS expression in action rejected");

    // Boundary nesting depth: 16 passes, 17 fails
    {
        char ok_deep[2048] = {};
        uint32_t opos = 0u;
        for (int i = 0; i < 16; ++i) {
            const int n = std::snprintf(ok_deep + opos, sizeof(ok_deep) - opos, "%s",
                                        i < 15 ? "{\"type\":\"box\",\"children\":[" : "{\"type\":\"text\",\"text\":\"ok\"}");
            if (n > 0) opos += static_cast<uint32_t>(n);
        }
        for (int i = 0; i < 15; ++i) {
            const int n = std::snprintf(ok_deep + opos, sizeof(ok_deep) - opos, "%s", "]}");
            if (n > 0) opos += static_cast<uint32_t>(n);
        }
        ok &= check(inlay_genui_validate_json({ok_deep, opos, false}, 1u, &v) && v.valid && v.node_count == 16u,
                    "depth == 16 allowed");
    }
    char deep_buf[2048] = {};
    uint32_t dpos = 0u;
    for (int i = 0; i < 17; ++i) {
        const int n = std::snprintf(deep_buf + dpos, sizeof(deep_buf) - dpos, "%s",
                                    i < 16 ? "{\"type\":\"box\",\"children\":[" : "{\"type\":\"text\",\"text\":\"deep\"}");
        if (n > 0) dpos += static_cast<uint32_t>(n);
    }
    for (int i = 0; i < 16; ++i) {
        const int n = std::snprintf(deep_buf + dpos, sizeof(deep_buf) - dpos, "%s", "]}");
        if (n > 0) dpos += static_cast<uint32_t>(n);
    }
    ok &= check(!inlay_genui_validate_json({deep_buf, dpos, false}, 1u, &v) && !v.valid &&
                v.error_message[0] != '\0',
                "depth > 16 rejected");

    // Excess node count (> 512)
    auto* many_buf = static_cast<char*>(std::malloc(32768u));
    if (many_buf) {
        uint32_t mpos = static_cast<uint32_t>(std::snprintf(many_buf, 32768u, "%s", "{\"type\":\"col\",\"children\":["));
        for (int i = 0; i < 513; ++i) {
            const int n = std::snprintf(many_buf + mpos, 32768u - mpos, "%s",
                                        i == 0 ? "{\"type\":\"text\",\"text\":\"x\"}"
                                               : ",{\"type\":\"text\",\"text\":\"x\"}");
            if (n > 0) mpos += static_cast<uint32_t>(n);
        }
        const int tail = std::snprintf(many_buf + mpos, 32768u - mpos, "%s", "]}");
        if (tail > 0) mpos += static_cast<uint32_t>(tail);
        ok &= check(!inlay_genui_validate_json({many_buf, mpos, false}, 1u, &v) && !v.valid &&
                    v.error_message[0] != '\0',
                    "node count > 512 rejected");
        std::free(many_buf);
    }

    // Invalid spec yields 0 from list_actions
    ok &= check(inlay_genui_list_actions(inject_script, actions_buf, sizeof(actions_buf)) == 0u &&
                actions_buf[0] == '\0',
                "list_actions returns 0 on invalid spec");

    // Null surface gracefully fails
    ok &= check(!inlay_genui_render_to_surface(nullptr, valid_spec, 1u, &v) && !v.valid,
                "render_to_surface rejects null surface");

    // Live headless surface render (runs when INLAY_ENABLE_SCRIPT=ON; gracefully skipped when OFF)
    InlaySurfaceDesc sdesc = inlay_surface_desc_default();
    sdesc.max_tick_ms = 50.f;
    InlaySurface* surface = inlay_surface_create(ctx, &sdesc);
    if (surface) {
        ActionCapture cap = {};
        ok &= check(inlay_surface_bind_action(surface, ETCH_STR_LIT("deploy.approve"), on_genui_action, &cap),
                    "bind GenUI host action");
        ok &= check(inlay_genui_render_to_surface(surface, valid_spec, 1u, &v) && v.valid,
                    "render_to_surface loads validated GenUI tree");
        ok &= check(!inlay_surface_stats(surface).has_error, "GenUI generated script has zero errors");

        uint32_t ncount = 0u;
        const InlayUiNode* nodes = inlay_surface_nodes(surface, &ncount);
        ok &= check(nodes != nullptr && ncount >= 15u, "GenUI surface built native nodes");

        InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
        view.id = etch_id("genui-test-view");
        view.width_px = 640.f;
        view.height_px = 800.f;

        EtchFrameDesc frame = etch_frame_desc_default();
        frame.viewport_w = 640u;
        frame.viewport_h = 800u;

        etch_begin_frame(ctx, &frame);
        inlay_surface_view(surface, &view);
        etch_end_frame(ctx);

        const uint32_t btn_eid = find_surface_node_eid(nodes, ncount, 0u, view.id, INLAY_UI_BUTTON);
        ok &= check(btn_eid != 0u, "found GenUI button element ID");
        if (btn_eid != 0u) {
            ok &= check(etch_perform_a11y_action(btn_eid, ETCH_A11Y_ACT_PRESS, 0.f, {}, ctx, nullptr),
                        "press GenUI button via a11y");
            etch_begin_frame(ctx, &frame);
            inlay_surface_view(surface, &view);
            etch_end_frame(ctx);
            ok &= check(cap.count == 1 && std::strstr(cap.last_payload, "\"id\":42") != nullptr &&
                        std::strstr(cap.last_payload, "\"ok\":true") != nullptr,
                        "GenUI button invokes bound host action with payload");
        }

        ok &= check(!inlay_genui_render_to_surface(surface, inject_script, 1u, &v) && !v.valid,
                    "render_to_surface rejects unsafe spec before touching surface");
        inlay_surface_destroy(surface);
    }

    return ok;
}

// ============================================================================
// 3. W3C Design Tokens (DTCG JSON) Tests (#46)
// ============================================================================

bool test_tokens() {
    bool ok = true;

    const EtchTheme dark = etch_theme_dark();
    const EtchTheme light = etch_theme_light();

    // Export dark theme to DTCG JSON and re-import over light base
    const uint32_t need = inlay_tokens_export_theme(&dark, nullptr, 0u);
    ok &= check(need > 128u, "tokens export dark theme size");
    auto* buf = static_cast<char*>(std::malloc(size_t(need) + 1u));
    if (!check(buf != nullptr, "alloc tokens buffer")) return false;
    const uint32_t written = inlay_tokens_export_theme(&dark, buf, need + 1u);
    ok &= check(written == need && buf[need] == '\0', "tokens export dark exact length");
    ok &= check(std::strstr(buf, "\"$type\":\"color\"") != nullptr &&
                std::strstr(buf, "\"$type\":\"dimension\"") != nullptr,
                "tokens export contains DTCG $type color and dimension");

    EtchTheme imported = {};
    ok &= check(inlay_tokens_import_theme({buf, need, false}, false, &imported),
                "tokens import dark JSON over light base");
    ok &= check(imported.id == ETCH_THEME_CUSTOM, "imported theme id is ETCH_THEME_CUSTOM");
    ok &= check(color_eq(imported.bg_canvas, dark.bg_canvas) &&
                color_eq(imported.bg_surface, dark.bg_surface) &&
                color_eq(imported.bg_elevated, dark.bg_elevated) &&
                color_eq(imported.bg_chrome, dark.bg_chrome) &&
                color_eq(imported.bg_input, dark.bg_input) &&
                color_eq(imported.text_primary, dark.text_primary) &&
                color_eq(imported.text_secondary, dark.text_secondary) &&
                color_eq(imported.text_body, dark.text_body) &&
                color_eq(imported.text_muted, dark.text_muted) &&
                color_eq(imported.accent, dark.accent) &&
                color_eq(imported.accent_primary, dark.accent_primary) &&
                color_eq(imported.danger, dark.danger) &&
                color_eq(imported.warning, dark.warning) &&
                color_eq(imported.border_subtle, dark.border_subtle) &&
                color_eq(imported.border_control, dark.border_control),
                "dark theme DTCG color roundtrip matches");
    ok &= check(near_f(imported.radius_small_px, dark.radius_small_px) &&
                near_f(imported.radius_control_px, dark.radius_control_px) &&
                near_f(imported.radius_input_px, dark.radius_input_px) &&
                near_f(imported.radius_panel_px, dark.radius_panel_px) &&
                near_f(imported.radius_pill_px, dark.radius_pill_px),
                "dark theme DTCG radius roundtrip matches");
    std::free(buf);

    // Export light theme and re-import over dark base
    const uint32_t light_need = inlay_tokens_export_theme(&light, nullptr, 0u);
    auto* lbuf = static_cast<char*>(std::malloc(size_t(light_need) + 1u));
    if (!check(lbuf != nullptr, "alloc light tokens buffer")) return false;
    inlay_tokens_export_theme(&light, lbuf, light_need + 1u);
    EtchTheme imported_light = {};
    ok &= check(inlay_tokens_import_theme({lbuf, light_need, false}, true, &imported_light),
                "tokens import light JSON over dark base");
    ok &= check(imported_light.id == ETCH_THEME_CUSTOM &&
                color_eq(imported_light.bg_canvas, light.bg_canvas) &&
                color_eq(imported_light.text_primary, light.text_primary) &&
                color_eq(imported_light.accent_primary, light.accent_primary) &&
                near_f(imported_light.radius_panel_px, light.radius_panel_px),
                "light theme DTCG roundtrip matches");
    std::free(lbuf);

    // Hand-authored DTCG with #rgb, #rrggbb, #rrggbbaa, group-inherited $type, and float px
    const EtchStr custom_dtcg = ETCH_STR_LIT(
        "{"
        "\"color\":{"
        "  \"$type\":\"color\","
        "  \"bg\":{\"canvas\":{\"$value\":\"#123\"},\"surface\":{\"$value\":\"#112233\"}},"
        "  \"accent\":{\"$value\":\"#4488cc80\"}"
        "},"
        "\"radius\":{"
        "  \"small\":{\"$type\":\"dimension\",\"$value\":\"5.5px\"},"
        "  \"panel\":{\"$type\":\"dimension\",\"$value\":{\"value\":14,\"unit\":\"px\"}}"
        "}"
        "}"
    );
    EtchTheme custom = {};
    ok &= check(inlay_tokens_import_theme(custom_dtcg, true, &custom), "hand-authored DTCG import");
    ok &= check(custom.id == ETCH_THEME_CUSTOM &&
                color_eq(custom.bg_canvas, etch_rgba8(0x11u, 0x22u, 0x33u, 0xFFu)) &&
                color_eq(custom.bg_surface, etch_rgba8(0x11u, 0x22u, 0x33u, 0xFFu)) &&
                color_eq(custom.accent, etch_rgba8(0x44u, 0x88u, 0xCCu, 0x80u)) &&
                near_f(custom.radius_small_px, 5.5f) &&
                near_f(custom.radius_panel_px, 14.0f),
                "custom DTCG hex formats and dimensions parsed");

    // Invalid JSON rejection leaves target untouched
    EtchTheme sentinel = dark;
    ok &= check(!inlay_tokens_import_theme(ETCH_STR_LIT(""), true, &sentinel), "tokens reject empty");
    ok &= check(!inlay_tokens_import_theme(ETCH_STR_LIT("{broken"), true, &sentinel), "tokens reject malformed JSON");
    ok &= check(!inlay_tokens_import_theme(ETCH_STR_LIT("[]"), true, &sentinel), "tokens reject array root");
    ok &= check(!inlay_tokens_import_theme(ETCH_STR_LIT("{} extra"), true, &sentinel), "tokens reject trailing tokens");
    ok &= check(sentinel.id == dark.id && color_eq(sentinel.bg_canvas, dark.bg_canvas),
                "failed token import leaves out_theme untouched");

    return ok;
}

// ============================================================================
// 4. Inbox -> Surface Bridge Tests (#12)
// ============================================================================

bool test_inbox_drain_to_surface(EtchContext* ctx) {
    bool ok = true;
    InlayInboxDesc idesc = inlay_inbox_desc_default();
    InlayInbox* inbox = inlay_inbox_create(&idesc);
    if (!check(inbox != nullptr, "inbox create")) return false;

    ok &= check(inlay_inbox_drain_to_surface(nullptr, nullptr, 4u) == 0u, "drain null guards");
    ok &= check(inlay_inbox_drain_to_surface(inbox, nullptr, 4u) == 0u, "drain null surface guard");

    InlaySurfaceDesc sdesc = inlay_surface_desc_default();
    sdesc.max_tick_ms = 50.f;
    InlaySurface* surface = inlay_surface_create(ctx, &sdesc);
    if (surface) {
        ok &= check(inlay_surface_set_script(surface, ETCH_STR_LIT(
            "function render(s){return ui.text('sum='+(s.sum||0));}"
            "function onEvent(name,payload,s){"
            "  s.events=(s.events||0)+1;"
            "  if(name==='inbox') s.kind0=(s.kind0||0)+1;"
            "  if(name==='inbox:7') s.kind7=(s.kind7||0)+1;"
            "  if(payload && typeof payload.v==='number') s.sum=(s.sum||0)+payload.v;"
            "}")),
            "inbox bridge surface script");

        // Post 10 messages: 7 with kind 0, 1 with kind 7, 1 empty payload, 1 invalid UTF-8
        for (int i = 0; i < 7; ++i) {
            const char msg[] = "{\"v\":10}";
            ok &= check(inlay_inbox_post(inbox, 0u, static_cast<uint32_t>(i + 1), msg, sizeof(msg) - 1u) == INLAY_INBOX_OK,
                        "post kind 0 json message");
        }
        {
            const char msg7[] = "{\"v\":5}";
            ok &= check(inlay_inbox_post(inbox, 7u, 8u, msg7, sizeof(msg7) - 1u) == INLAY_INBOX_OK,
                        "post kind 7 json message");
        }
        ok &= check(inlay_inbox_post(inbox, 0u, 9u, nullptr, 0u) == INLAY_INBOX_OK,
                    "post empty payload message");
        {
            const unsigned char bad_utf8[2] = {0xFFu, 0xFEu};
            ok &= check(inlay_inbox_post(inbox, 0u, 10u, bad_utf8, 2u) == INLAY_INBOX_OK,
                        "post invalid utf8 payload");
        }
        ok &= check(inlay_inbox_pending(inbox) == 10u, "10 pending messages before drain");

        // First frame budget: max_messages = 4
        const uint32_t d1 = inlay_inbox_drain_to_surface(inbox, surface, 4u);
        ok &= check(d1 == 4u && inlay_inbox_pending(inbox) == 6u, "frame budget max_messages=4 enforced");

        // Second frame budget: 0 -> default 64 drains remaining 6
        const uint32_t d2 = inlay_inbox_drain_to_surface(inbox, surface, 0u);
        ok &= check(d2 == 6u && inlay_inbox_pending(inbox) == 0u, "default budget drains remaining 6");

        ok &= check(!inlay_surface_stats(surface).has_error, "invalid utf8 skipped without surface error");
        const EtchStr st = inlay_surface_state_json(surface);
        ok &= check(st.chars != nullptr &&
                    std::strstr(st.chars, "\"events\":9") != nullptr &&
                    std::strstr(st.chars, "\"kind0\":8") != nullptr &&
                    std::strstr(st.chars, "\"kind7\":1") != nullptr &&
                    std::strstr(st.chars, "\"sum\":75") != nullptr,
                    "surface received valid events with expected names and sum");

        inlay_surface_destroy(surface);
    }

    inlay_inbox_destroy(inbox);
    return ok;
}

} // namespace

int main() {
    EtchContextDesc cfg = etch_context_desc_default();
    cfg.headless = true;
    EtchContext* ctx = etch_create_context(&cfg);
    if (!ctx) {
        std::puts("Workbench features FAIL (failed to create headless EtchContext)");
        return 1;
    }

    bool ok = true;
    ok &= test_stream();
    ok &= test_genui(ctx);
    ok &= test_tokens();
    ok &= test_inbox_drain_to_surface(ctx);

    etch_destroy_context(ctx);
    std::puts(ok ? "Workbench features PASS" : "Workbench features FAIL");
    return ok ? 0 : 1;
}
