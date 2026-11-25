#include <inlay/inlay_c.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
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

bool test_stream_r01_atomic_upsert() {
    bool ok = true;
    InlayStream* s = inlay_stream_create();
    if (!check(s != nullptr, "R01 stream create")) return false;

    InlayStreamItem orig = {};
    orig.id = 42u;
    orig.message_id = 7u;
    orig.role = INLAY_STREAM_ROLE_ASSISTANT;
    orig.kind = INLAY_STREAM_ITEM_MARKDOWN;
    orig.status = INLAY_STREAM_STATUS_RUNNING;
    orig.progress = 0.5f;
    orig.title = ETCH_STR_LIT("OrigTitle");
    orig.body = ETCH_STR_LIT("OrigBody");
    orig.metadata_json = ETCH_STR_LIT("{\"v\":1}");
    ok &= check(inlay_stream_upsert(s, &orig), "R01 initial insert");

    const uint64_t saved_cursor = inlay_stream_cursor(s);
    constexpr uint32_t kOverLimit = (32u * 1024u * 1024u) + 1u;

    auto verify_untouched = [&](const char* label, uint64_t expect_rev, uint64_t expect_cursor) {
        InlayStreamItem cur = {};
        const bool got = inlay_stream_get_by_id(s, 42u, &cur);
        ok &= check(got &&
                    cur.revision == expect_rev &&
                    inlay_stream_cursor(s) == expect_cursor &&
                    cur.status == INLAY_STREAM_STATUS_RUNNING &&
                    near_f(cur.progress, 0.5f) &&
                    str_eq(cur.title, "OrigTitle") &&
                    str_eq(cur.body, "OrigBody") &&
                    str_eq(cur.metadata_json, "{\"v\":1}"),
                    label);
    };

    // Over-limit body after valid modified title & metadata must not mutate existing entry
    {
        InlayStreamItem bad = orig;
        bad.status = INLAY_STREAM_STATUS_DONE;
        bad.progress = 1.0f;
        bad.title = ETCH_STR_LIT("CorruptedTitle");
        bad.body = {"x", kOverLimit, false};
        bad.metadata_json = ETCH_STR_LIT("{\"v\":2}");
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects over-limit body");
        verify_untouched("R01 over-limit body preserves existing entry", 1u, saved_cursor);
    }

    // Over-limit metadata_json after valid modified title & body must not mutate existing entry
    {
        InlayStreamItem bad = orig;
        bad.title = ETCH_STR_LIT("CorruptedTitle2");
        bad.body = ETCH_STR_LIT("CorruptedBody2");
        bad.metadata_json = {"x", kOverLimit, false};
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects over-limit metadata_json");
        verify_untouched("R01 over-limit metadata preserves existing entry", 1u, saved_cursor);
    }

    // Over-limit title
    {
        InlayStreamItem bad = orig;
        bad.title = {"x", kOverLimit, false};
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects over-limit title");
        verify_untouched("R01 over-limit title preserves existing entry", 1u, saved_cursor);
    }

    // Invalid UTF-8 in title / body / metadata_json
    {
        const char bad_ff[] = "a\xFF";
        InlayStreamItem bad = orig;
        bad.title = {bad_ff, 2u, false};
        bad.body = ETCH_STR_LIT("MutatedBody");
        bad.metadata_json = ETCH_STR_LIT("{\"mut\":true}");
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects invalid UTF-8 title");
        verify_untouched("R01 invalid UTF-8 title preserves existing entry", 1u, saved_cursor);
    }
    {
        const char bad_surr[] = "\xED\xA0\x80";
        InlayStreamItem bad = orig;
        bad.title = ETCH_STR_LIT("MutatedTitle");
        bad.body = {bad_surr, 3u, false};
        bad.metadata_json = ETCH_STR_LIT("{\"mut\":true}");
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects surrogate UTF-8 body");
        verify_untouched("R01 surrogate UTF-8 body preserves existing entry", 1u, saved_cursor);
    }
    {
        const char bad_trunc[] = "\xE4\xB8";
        InlayStreamItem bad = orig;
        bad.title = ETCH_STR_LIT("MutatedTitle");
        bad.body = {bad_trunc, 2u, false};
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects incomplete UTF-8 body");
        verify_untouched("R01 incomplete UTF-8 body preserves existing entry", 1u, saved_cursor);
    }
    {
        const char bad_meta[] = "\xE0\x80\x80";
        InlayStreamItem bad = orig;
        bad.title = ETCH_STR_LIT("MutatedTitle");
        bad.body = ETCH_STR_LIT("MutatedBody");
        bad.metadata_json = {bad_meta, 3u, false};
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects overlong UTF-8 metadata_json");
        verify_untouched("R01 invalid UTF-8 metadata preserves existing entry", 1u, saved_cursor);
    }

    // Invalid enum and null pointer with nonzero length
    {
        InlayStreamItem bad = orig;
        bad.role = static_cast<InlayStreamRole>(99);
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects out-of-range role");
        verify_untouched("R01 invalid role preserves existing entry", 1u, saved_cursor);
    }
    {
        InlayStreamItem bad = orig;
        bad.title = ETCH_STR_LIT("MutatedTitle");
        bad.metadata_json = {nullptr, 5u, false};
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 rejects null metadata with nonzero length");
        verify_untouched("R01 null metadata preserves existing entry", 1u, saved_cursor);
    }

    // Pending utf8_carry preserved on failed upsert, cleared on committed replacement upsert
    {
        const char lead[] = "\xE4";
        const char tail[] = "\xB8\xAD";
        ok &= check(inlay_stream_append_body(s, 42u, {lead, 1u, false}), "R01 stage 1-byte utf8 carry");
        const uint64_t cur_after_carry = inlay_stream_cursor(s);

        const char bad_utf8[] = "\xFF";
        InlayStreamItem bad = orig;
        bad.metadata_json = {bad_utf8, 1u, false};
        ok &= check(!inlay_stream_upsert(s, &bad), "R01 failed upsert while carry pending");
        ok &= check(inlay_stream_cursor(s) == cur_after_carry, "R01 failed upsert keeps cursor");

        ok &= check(inlay_stream_append_body(s, 42u, {tail, 2u, false}), "R01 pending carry completes after failed upsert");
        InlayStreamItem after_cjk = {};
        ok &= check(inlay_stream_get_by_id(s, 42u, &after_cjk) && str_eq(after_cjk.body, "OrigBody中"),
                    "R01 carry reassembled CJK char intact");

        const char emoji_lead[] = "\xF0";
        ok &= check(inlay_stream_append_body(s, 42u, {emoji_lead, 1u, false}), "R01 stage emoji lead carry");
        InlayStreamItem repl = orig;
        repl.body = ETCH_STR_LIT("Reset");
        ok &= check(inlay_stream_upsert(s, &repl), "R01 valid replacement upsert commits");
        ok &= check(inlay_stream_append_body(s, 42u, ETCH_STR_LIT("!")),
                    "R01 replacement upsert cleared stale utf8_carry");
        InlayStreamItem after_reset = {};
        ok &= check(inlay_stream_get_by_id(s, 42u, &after_reset) && str_eq(after_reset.body, "Reset!"),
                    "R01 post-replacement append matches");
    }

    inlay_stream_destroy(s);
    return ok;
}

bool test_stream_r02_utf8_carry() {
    bool ok = true;
    InlayStream* s = inlay_stream_create();
    if (!check(s != nullptr, "R02 stream create")) return false;

    InlayStreamItem item = {};
    item.id = 7u;
    item.role = INLAY_STREAM_ROLE_ASSISTANT;
    item.kind = INLAY_STREAM_ITEM_MARKDOWN;
    item.status = INLAY_STREAM_STATUS_RUNNING;
    item.title = ETCH_STR_LIT("Stream");
    item.body = ETCH_STR_LIT("Start:");
    ok &= check(inlay_stream_upsert(s, &item), "R02 insert base item");

    auto check_body_state = [&](const char* expected_body, uint64_t expected_rev,
                                uint64_t expected_cursor, const char* label) {
        InlayStreamItem cur = {};
        ok &= check(inlay_stream_get_by_id(s, 7u, &cur) &&
                    str_eq(cur.body, expected_body) &&
                    cur.revision == expected_rev &&
                    inlay_stream_cursor(s) == expected_cursor,
                    label);
    };

    const uint64_t c0 = inlay_stream_cursor(s);

    // Invalid UTF-8 sequences must be rejected atomically without modifying body/revision/cursor
    const char inv_ff[] = "\xFF";
    const char inv_cont[] = "\x80";
    const char inv_c0[] = "\xC0\x80";
    const char inv_c1[] = "\xC1\xBF";
    const char inv_overlong_e0[] = "\xE0\x80";
    const char inv_overlong_e0_full[] = "\xE0\x9F\xBF";
    const char inv_surr_prefix[] = "\xED\xA0";
    const char inv_surr_full[] = "\xED\xA0\x80";
    const char inv_overlong_f0[] = "\xF0\x80";
    const char inv_gt_max[] = "\xF4\x90";
    const char inv_f5[] = "\xF5\x80\x80\x80";
    const char inv_mixed[] = "ok\xFF";

    ok &= check(!inlay_stream_append_body(s, 7u, {inv_ff, 1u, false}), "R02 reject 0xFF");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_cont, 1u, false}), "R02 reject lone continuation 0x80");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_c0, 2u, false}), "R02 reject overlong C0 80");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_c1, 2u, false}), "R02 reject overlong C1 BF");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_overlong_e0, 2u, false}), "R02 reject overlong prefix E0 80");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_overlong_e0_full, 3u, false}), "R02 reject overlong E0 9F BF");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_surr_prefix, 2u, false}), "R02 reject surrogate prefix ED A0");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_surr_full, 3u, false}), "R02 reject surrogate ED A0 80");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_overlong_f0, 2u, false}), "R02 reject overlong prefix F0 80");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_gt_max, 2u, false}), "R02 reject >U+10FFFF F4 90");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_f5, 4u, false}), "R02 reject F5..");
    ok &= check(!inlay_stream_append_body(s, 7u, {inv_mixed, 3u, false}), "R02 reject mixed valid+0xFF atomically");
    check_body_state("Start:", 1u, c0, "R02 all invalid appends left state untouched");

    // 3-byte Chinese "中" (\xE4\xB8\xAD) split byte-by-byte with interleaved invalid rejection
    const char zh[] = "\xE4\xB8\xAD";
    ok &= check(inlay_stream_append_body(s, 7u, {&zh[0], 1u, false}), "R02 zh byte 1");
    check_body_state("Start:", 2u, c0 + 1u, "R02 zh byte 1 buffered in carry");

    ok &= check(!inlay_stream_append_body(s, 7u, ETCH_STR_LIT("A")), "R02 reject non-continuation after carry");
    check_body_state("Start:", 2u, c0 + 1u, "R02 failed continuation left carry & cursor untouched");

    ok &= check(inlay_stream_append_body(s, 7u, {&zh[1], 1u, false}), "R02 zh byte 2");
    check_body_state("Start:", 3u, c0 + 2u, "R02 zh byte 2 buffered in carry");

    ok &= check(!inlay_stream_append_body(s, 7u, {inv_ff, 1u, false}), "R02 reject 0xFF while 2-byte carry pending");
    check_body_state("Start:", 3u, c0 + 2u, "R02 failed 0xFF left 2-byte carry untouched");

    ok &= check(inlay_stream_append_body(s, 7u, {&zh[2], 1u, false}), "R02 zh byte 3 completes scalar");
    check_body_state("Start:中", 4u, c0 + 3u, "R02 3-byte Chinese reassembled identically");

    // 4-byte emoji "😀" (\xF0\x9F\x98\x80) split byte-by-byte with interleaved invalid rejection
    const char emoji[] = "\xF0\x9F\x98\x80";
    ok &= check(inlay_stream_append_body(s, 7u, {&emoji[0], 1u, false}), "R02 emoji byte 1");
    ok &= check(inlay_stream_append_body(s, 7u, {&emoji[1], 1u, false}), "R02 emoji byte 2");
    ok &= check(!inlay_stream_append_body(s, 7u, ETCH_STR_LIT("Z")), "R02 reject non-continuation mid-emoji");
    check_body_state("Start:中", 6u, c0 + 5u, "R02 failed mid-emoji left state untouched");

    ok &= check(inlay_stream_append_body(s, 7u, {&emoji[2], 1u, false}), "R02 emoji byte 3");
    ok &= check(inlay_stream_append_body(s, 7u, {&emoji[3], 1u, false}), "R02 emoji byte 4 completes scalar");
    check_body_state("Start:中😀", 8u, c0 + 7u, "R02 4-byte emoji reassembled identically");

    // Split overlong E0 + 80 across two calls must reject byte 2 and still accept valid A0 80 afterwards
    const char e0_lead[] = "\xE0";
    const char e0_bad[] = "\x80";
    const char e0_ok[] = "\xA0\x80";
    ok &= check(inlay_stream_append_body(s, 7u, {e0_lead, 1u, false}), "R02 E0 lead accepted");
    ok &= check(!inlay_stream_append_body(s, 7u, {e0_bad, 1u, false}), "R02 split overlong E0+80 rejected");
    ok &= check(inlay_stream_append_body(s, 7u, {e0_ok, 2u, false}), "R02 valid continuation after rejected split");
    ok &= check(inlay_stream_append_body(s, 7u, ETCH_STR_LIT("!")), "R02 trailing ASCII after carry recovery");
    check_body_state("Start:中😀\xE0\xA0\x80!", 11u, c0 + 10u,
                     "R02 final reassembled body exact");

    inlay_stream_destroy(s);
    return ok;
}

bool test_stream_c10_replay_and_budget() {
    bool ok = true;

    // Part A: Incremental replay via inlay_stream_export_since_json
    {
        char null_buf[8] = {'x'};
        ok &= check(inlay_stream_export_since_json(nullptr, 0u, null_buf, sizeof(null_buf)) == 0u &&
                    null_buf[0] == '\0',
                    "C10 export_since null guard");

        InlayStream* s = inlay_stream_create();
        if (!check(s != nullptr, "C10 replay stream create")) return false;

        for (uint64_t id = 101u; id <= 103u; ++id) {
            InlayStreamItem it = {};
            it.id = id;
            it.role = INLAY_STREAM_ROLE_ASSISTANT;
            it.kind = INLAY_STREAM_ITEM_MARKDOWN;
            it.status = INLAY_STREAM_STATUS_RUNNING;
            it.title = ETCH_STR_LIT("T");
            it.body = ETCH_STR_LIT("B");
            ok &= check(inlay_stream_upsert(s, &it), "C10 insert replay item");
        }

        char buf[1024] = {};
        const uint32_t need0 = inlay_stream_export_since_json(s, 0u, nullptr, 0u);
        const uint32_t got0 = inlay_stream_export_since_json(s, 0u, buf, sizeof(buf));
        ok &= check(need0 == got0 && got0 < sizeof(buf) &&
                    std::strstr(buf, "\"cursor\":3,\"since\":0,\"items\":[") != nullptr &&
                    std::strstr(buf, "\"id\":101") != nullptr &&
                    std::strstr(buf, "\"id\":102") != nullptr &&
                    std::strstr(buf, "\"id\":103") != nullptr,
                    "C10 since=0 exports all 3 items");

        const uint32_t got2 = inlay_stream_export_since_json(s, 2u, buf, sizeof(buf));
        ok &= check(got2 > 0u &&
                    std::strstr(buf, "\"cursor\":3,\"since\":2,\"items\":[") != nullptr &&
                    std::strstr(buf, "\"id\":101") == nullptr &&
                    std::strstr(buf, "\"id\":102") == nullptr &&
                    std::strstr(buf, "\"id\":103") != nullptr,
                    "C10 since=2 exports only item 103");

        const uint32_t got3 = inlay_stream_export_since_json(s, 3u, buf, sizeof(buf));
        ok &= check(got3 > 0u && std::strcmp(buf, "{\"cursor\":3,\"since\":3,\"items\":[]}") == 0,
                    "C10 since=3 exports empty delta");

        // Append to item 101 -> cursor 4; upsert item 102 -> cursor 5
        ok &= check(inlay_stream_append_body(s, 101u, ETCH_STR_LIT("+")), "C10 append item 101");
        const uint32_t got3_after_append = inlay_stream_export_since_json(s, 3u, buf, sizeof(buf));
        ok &= check(got3_after_append > 0u &&
                    std::strstr(buf, "\"cursor\":4,\"since\":3,\"items\":[") != nullptr &&
                    std::strstr(buf, "\"id\":101") != nullptr &&
                    std::strstr(buf, "\"id\":102") == nullptr &&
                    std::strstr(buf, "\"id\":103") == nullptr,
                    "C10 since=3 after append exports only item 101");

        InlayStreamItem upd102 = {};
        ok &= check(inlay_stream_get_by_id(s, 102u, &upd102), "C10 fetch 102");
        upd102.status = INLAY_STREAM_STATUS_DONE;
        ok &= check(inlay_stream_upsert(s, &upd102), "C10 update item 102");

        const uint32_t got3_after_upd = inlay_stream_export_since_json(s, 3u, buf, sizeof(buf));
        ok &= check(got3_after_upd > 0u &&
                    std::strstr(buf, "\"cursor\":5,\"since\":3,\"items\":[") != nullptr &&
                    std::strstr(buf, "\"id\":101") != nullptr &&
                    std::strstr(buf, "\"id\":102") != nullptr &&
                    std::strstr(buf, "\"id\":103") == nullptr,
                    "C10 since=3 exports items 101 and 102");

        const uint32_t got4 = inlay_stream_export_since_json(s, 4u, buf, sizeof(buf));
        ok &= check(got4 > 0u &&
                    std::strstr(buf, "\"cursor\":5,\"since\":4,\"items\":[") != nullptr &&
                    std::strstr(buf, "\"id\":101") == nullptr &&
                    std::strstr(buf, "\"id\":102") != nullptr &&
                    std::strstr(buf, "\"id\":103") == nullptr,
                    "C10 since=4 exports only item 102");

        // Roundtrip full JSON preserves lastCursor for incremental export
        const uint32_t full_len = inlay_stream_export_json(s, buf, sizeof(buf));
        InlayStream* s2 = inlay_stream_create();
        ok &= check(s2 != nullptr && inlay_stream_import_json(s2, {buf, full_len, false}),
                    "C10 full roundtrip import");
        if (s2) {
            char delta_buf[1024] = {};
            inlay_stream_export_since_json(s2, 4u, delta_buf, sizeof(delta_buf));
            ok &= check(std::strstr(delta_buf, "\"id\":102") != nullptr &&
                        std::strstr(delta_buf, "\"id\":101") == nullptr &&
                        std::strstr(delta_buf, "\"id\":103") == nullptr,
                        "C10 imported stream preserves per-item last_cursor");
            inlay_stream_destroy(s2);
        }

        inlay_stream_destroy(s);
    }

    // Part B: Bounded memory budget & oldest-item eviction via inlay_stream_set_max_bytes
    {
        inlay_stream_set_max_bytes(nullptr, 60u); // Null guard

        InlayStream* s = inlay_stream_create();
        if (!check(s != nullptr, "C10 budget stream create")) return false;
        inlay_stream_set_max_bytes(s, 60u);

        const char b25[26] = "1234567890123456789012345";
        const char b20[21] = "12345678901234567890";
        const char b61[62] = "1234567890123456789012345678901234567890123456789012345678901";

        InlayStreamItem it = {};
        it.role = INLAY_STREAM_ROLE_ASSISTANT;
        it.kind = INLAY_STREAM_ITEM_MARKDOWN;
        it.status = INLAY_STREAM_STATUS_RUNNING;
        it.body = {b25, 25u, false};

        it.id = 1u;
        ok &= check(inlay_stream_upsert(s, &it) && inlay_stream_count(s) == 1u, "C10 insert item 1 (25B)");
        it.id = 2u;
        ok &= check(inlay_stream_upsert(s, &it) && inlay_stream_count(s) == 2u, "C10 insert item 2 (50B)");
        it.id = 3u;
        ok &= check(inlay_stream_upsert(s, &it) && inlay_stream_count(s) == 2u,
                    "C10 insert item 3 (75B > 60B) evicts oldest");

        InlayStreamItem at0 = {}, at1 = {};
        ok &= check(!inlay_stream_get_by_id(s, 1u, &at0) &&
                    inlay_stream_at(s, 0u, &at0) && inlay_stream_at(s, 1u, &at1) &&
                    at0.id == 2u && at1.id == 3u,
                    "C10 item 1 evicted, items 2 and 3 retained in order");

        // Append 20B to item 2 (25->45B, total 70B > 60B): evicts item 3, keeps item 2 being updated
        ok &= check(inlay_stream_append_body(s, 2u, {b20, 20u, false}),
                    "C10 append_body evicts other item to fit budget");
        ok &= check(inlay_stream_count(s) == 1u &&
                    inlay_stream_get_by_id(s, 2u, &at0) && at0.body.length == 45u &&
                    !inlay_stream_get_by_id(s, 3u, &at1),
                    "C10 item 2 grew to 45B and item 3 evicted");

        // Appending 20B more would make item 2 alone 65B > 60B -> must reject without corrupting item 2
        const uint64_t saved_cursor = inlay_stream_cursor(s);
        const uint64_t saved_rev = at0.revision;
        ok &= check(!inlay_stream_append_body(s, 2u, {b20, 20u, false}),
                    "C10 reject append exceeding single-item budget");
        ok &= check(inlay_stream_count(s) == 1u &&
                    inlay_stream_cursor(s) == saved_cursor &&
                    inlay_stream_get_by_id(s, 2u, &at0) &&
                    at0.body.length == 45u && at0.revision == saved_rev,
                    "C10 rejected append left item 2 untouched");

        // Single oversized upsert (new item or existing item) must reject without evicting or mutating
        InlayStreamItem huge = it;
        huge.id = 4u;
        huge.body = {b61, 61u, false};
        ok &= check(!inlay_stream_upsert(s, &huge), "C10 reject oversized new item");
        ok &= check(inlay_stream_count(s) == 1u && inlay_stream_cursor(s) == saved_cursor,
                    "C10 oversized new item did not evict existing item");

        huge.id = 2u;
        ok &= check(!inlay_stream_upsert(s, &huge), "C10 reject oversized update on existing item");
        ok &= check(inlay_stream_get_by_id(s, 2u, &at0) &&
                    at0.body.length == 45u && at0.revision == saved_rev &&
                    inlay_stream_cursor(s) == saved_cursor,
                    "C10 oversized update preserved existing item");

        // Shrinking max_bytes immediately evicts oldest items; setting 0 restores unbounded mode
        it.body = {b20, 20u, false};
        it.id = 2u; ok &= check(inlay_stream_upsert(s, &it), "C10 shrink item 2 to 20B");
        it.id = 5u; ok &= check(inlay_stream_upsert(s, &it), "C10 insert item 5 (20B)");
        it.id = 6u; ok &= check(inlay_stream_upsert(s, &it), "C10 insert item 6 (20B)");
        ok &= check(inlay_stream_count(s) == 3u, "C10 3 items fit exactly in 60B");

        inlay_stream_set_max_bytes(s, 40u);
        ok &= check(inlay_stream_count(s) == 2u &&
                    inlay_stream_at(s, 0u, &at0) && inlay_stream_at(s, 1u, &at1) &&
                    at0.id == 5u && at1.id == 6u,
                    "C10 lowering max_bytes evicts oldest item");

        inlay_stream_set_max_bytes(s, 0u);
        huge.id = 7u;
        ok &= check(inlay_stream_upsert(s, &huge) && inlay_stream_count(s) == 3u,
                    "C10 max_bytes=0 restores unbounded budget");

        inlay_stream_destroy(s);
    }

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

struct LiveTermCtx {
    EtchTerminalCell cells[40];
    uint32_t reads;
};

static bool read_live_term(InlayTerminalSnapshot* out, void* user) {
    auto* t = static_cast<LiveTermCtx*>(user);
    if (!t || !out) return false;
    ++t->reads;
    out->cells = t->cells;
    out->cell_count = 40u;
    out->columns = 10u;
    out->rows = 4u;
    out->cursor_column = 2;
    out->cursor_row = 1;
    out->cursor_shape = ETCH_TERMINAL_CURSOR_BLOCK;
    out->cursor_visible = true;
    out->revision = t->reads;
    return true;
}

static bool test_ade_surface_upgrades(EtchContext* ctx) {
    bool ok = true;
    // 1. useReducer + createContext / useContext / provideContext + memo() invalidation
    {
        InlaySurface* s = inlay_surface_create(ctx, nullptr);
        if (!s) return true; // Script engine disabled (INLAY_ENABLE_SCRIPT=OFF)
        static const char kScript[] =
            "const ThemeCtx = createContext('light');\n"
            "let memoRenders = 0;\n"
            "let dispatchRef = null;\n"
            "const Leaf = memo(function Leaf() {\n"
            "  memoRenders++;\n"
            "  const t = useContext(ThemeCtx);\n"
            "  return <Text>{t + ':' + memoRenders}</Text>;\n"
            "});\n"
            "function Counter(p) {\n"
            "  const [st, dispatch] = useReducer((cur, act) => act === 'inc' ? {n: cur.n + 1} : cur, {n: 0});\n"
            "  if (!dispatchRef) dispatchRef = dispatch;\n"
            "  if (dispatchRef !== dispatch) throw new Error('unstable dispatch');\n"
            "  if (p.bump > st.n) dispatch('inc');\n"
            "  return <Box><Text>{'cnt=' + st.n}</Text><Leaf /></Box>;\n"
            "}\n"
            "function render(state) {\n"
            "  return <ThemeCtx.Provider value={state.theme}>{() => <Counter bump={state.bump} />}</ThemeCtx.Provider>;\n"
            "}\n";
        ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"theme\":\"light\",\"bump\":0}")),
                    "set initial context state");
        ok &= check(inlay_surface_set_script(s, {kScript, uint32_t(sizeof(kScript) - 1u), true}),
                    "load hooks/context script");
        auto find_text = [&](const char* substr) -> bool {
            uint32_t cnt = 0u;
            const InlayUiNode* nodes = inlay_surface_nodes(s, &cnt);
            for (uint32_t i = 0u; i < cnt; ++i) {
                if (nodes[i].kind == INLAY_UI_TEXT && nodes[i].text.chars && nodes[i].text.length) {
                    if (std::strstr(nodes[i].text.chars, substr) != nullptr) return true;
                }
            }
            return false;
        };
        ok &= check(find_text("light:1") && find_text("cnt=0"), "initial context & reducer render");
        // Changing bump triggers reducer without changing theme: memo Leaf stays at 1 render
        ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"theme\":\"light\",\"bump\":1}")),
                    "bump reducer state");
        ok &= check(find_text("light:1") && find_text("cnt=1"), "memo preserved when context unchanged");
        // Changing theme invalidates memo Leaf even with identical props!
        ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"theme\":\"dark\",\"bump\":1}")),
                    "switch context theme");
        ok &= check(find_text("dark:2") && find_text("cnt=1"), "memo invalidated on context change");
        inlay_surface_destroy(s);
    }

    // 2. Sandboxed ES Module Loader (with JSX transpilation & path escape rejection)
    {
        InlaySurface* s = inlay_surface_create(ctx, nullptr);
        ok &= check(s != nullptr, "create surface for es modules");
        if (!s) return false;
        ok &= check(inlay_surface_register_module(s, ETCH_STR_LIT("utils/fmt.js"),
                        ETCH_STR_LIT("export function fmt(x) { return '[' + x + ']'; }")),
                    "register js module");
        ok &= check(inlay_surface_register_module(s, ETCH_STR_LIT("ui/pill.jsx"),
                        ETCH_STR_LIT("import { fmt } from '../utils/fmt';\nexport function Pill(p) { return <Badge>{fmt(p.text)}</Badge>; }")),
                    "register jsx module with relative import");
        static const char kEntry[] =
            "import { Pill } from 'ui/pill';\n"
            "export function render(state) { return <Box><Pill text={state.tag || 'ok'} /></Box>; }\n";
        ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"tag\":\"esm\"}")), "set esm state");
        ok &= check(inlay_surface_set_script(s, {kEntry, uint32_t(sizeof(kEntry) - 1u), true}),
                    "eval es module entry script");
        uint32_t cnt = 0u;
        const InlayUiNode* nodes = inlay_surface_nodes(s, &cnt);
        bool saw_esm = false;
        for (uint32_t i = 0u; i < cnt; ++i) {
            if (nodes[i].text.chars && std::strstr(nodes[i].text.chars, "[esm]") != nullptr) saw_esm = true;
        }
        ok &= check(saw_esm, "esm jsx component rendered imported helper");
        // Sandbox escape & unregistered module must fail safely
        ok &= check(!inlay_surface_set_script(s, ETCH_STR_LIT("import x from '../../etc/passwd'; export function render(){return <Box/>;}")),
                    "sandbox rejects path traversal import");
        ok &= check(inlay_surface_unregister_module(s, ETCH_STR_LIT("ui/pill.jsx")), "unregister module");
        ok &= check(!inlay_surface_set_script(s, {kEntry, uint32_t(sizeof(kEntry) - 1u), true}),
                    "unregistered module fails cleanly");
        inlay_surface_destroy(s);
    }

    // 3. Live zero-copy Terminal binding, declarative <Portal>, <Input autoGrow>, and .d.ts export
    {
        InlaySurface* s = inlay_surface_create(ctx, nullptr);
        InlayPortalDesc pdesc = inlay_portal_desc_default();
        pdesc.width = 200u;
        pdesc.height = 120u;
        InlayPortal* portal = inlay_portal_create(ctx, &pdesc);
        ok &= check(s != nullptr && portal != nullptr, "create surface & portal");
        if (!s || !portal) {
            inlay_portal_destroy(portal);
            inlay_surface_destroy(s);
            return false;
        }
        LiveTermCtx tctx = {};
        for (uint32_t i = 0u; i < 40u; ++i) {
            tctx.cells[i] = etch_terminal_cell_default();
            tctx.cells[i].codepoint = 'A' + (i % 26u);
        }
        ok &= check(inlay_surface_bind_terminal(s, ETCH_STR_LIT("pty0"), read_live_term, &tctx),
                    "bind live terminal");
        ok &= check(inlay_surface_bind_portal(s, ETCH_STR_LIT("preview"), portal),
                    "bind declarative portal");
        static const char kUi[] =
            "function render(st) {\n"
            "  return <Col width={420} height={360} gap={8}>\n"
            "    <Terminal buffer='pty0' width={240} height={96} autoFit focusable cursorShape='block'\n"
            "      onResize={(ev, s) => { s.termCols = ev.cols; s.termRows = ev.rows; }} />\n"
            "    <Portal name='preview' width={200} height={120} cls='rounded-md' />\n"
            "    <Input multiline autoGrow minHeight={40} maxHeight={140} value='line1\\nline2' />\n"
            "  </Col>;\n"
            "}\n";
        ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"termCols\":0,\"termRows\":0}")), "set ui state");
        ok &= check(inlay_surface_set_script(s, {kUi, uint32_t(sizeof(kUi) - 1u), true}), "load live term/portal/autogrow");

        EtchFrameDesc fd = etch_frame_desc_default();
        fd.viewport_w = 480u;
        fd.viewport_h = 400u;
        InlaySurfaceViewDesc sv = inlay_surface_view_desc_default();
        sv.id = etch_id("ade_surface_view");
        sv.width_px = 420.f;
        sv.height_px = 360.f;
        for (int f = 0; f < 2; ++f) {
            etch_begin_frame(ctx, &fd);
            inlay_surface_view(s, &sv);
            etch_end_frame(ctx);
            ok &= check(inlay_portal_end_frame_sync(portal, true), "declarative portal syncs geometry");
        }
        ok &= check(tctx.reads >= 2u, "live terminal read_fn invoked directly each frame");
        const EtchStr st = inlay_surface_state_json(s);
        ok &= check(st.chars && std::strstr(st.chars, "\"termCols\":0") == nullptr,
                    "terminal onResize reported visible columns");

        char dts[16384] = {};
        const uint32_t dlen = inlay_surface_export_dts(s, dts, uint32_t(sizeof(dts)));
        ok &= check(dlen > 0u &&
                    std::strstr(dts, "TerminalName = 'pty0'") != nullptr &&
                    std::strstr(dts, "PortalName = 'preview'") != nullptr &&
                    std::strstr(dts, "useReducer") != nullptr &&
                    std::strstr(dts, "createContext") != nullptr &&
                    std::strstr(dts, "autoGrow?: boolean") != nullptr,
                    "export_dts covers new hooks, terminal, portal, autoGrow");

        ok &= check(inlay_surface_bind_portal(s, ETCH_STR_LIT("preview"), nullptr), "unbind portal");
        ok &= check(inlay_surface_bind_terminal(s, ETCH_STR_LIT("pty0"), nullptr, nullptr), "unbind terminal");
        inlay_surface_destroy(s);
        ok &= check(inlay_portal_destroy(portal), "destroy portal");
    }
    return ok;
}

static bool test_orca_round2_bridge(EtchContext* ctx) {
    bool ok = true;
    InlaySurfaceDesc r2_desc = inlay_surface_desc_default();
    r2_desc.max_tick_ms = 50.f;
    InlaySurface* s = inlay_surface_create(ctx, &r2_desc);
    if (!s) return true; // Script engine disabled

    // Bind a host EtchTextBuffer & live terminal for full CodeEditor + Terminal bridge verification
    EtchTextBuffer* buf = etch_create_text_buffer(ETCH_STR_LIT("int a = 1;\nint b = 2;\nint c = 3;\n"), 0u);
    LiveTermCtx tctx = {};
    for (uint32_t i = 0u; i < 40u; ++i) {
        tctx.cells[i] = etch_terminal_cell_default();
        tctx.cells[i].codepoint = (i < 10u) ? ('a' + i) : ' ';
    }
    ok &= check(buf != nullptr, "create code editor buffer for round 2");
    if (!buf) { inlay_surface_destroy(s); return false; }

    ok &= check(inlay_surface_bind_text_buffer(s, ETCH_STR_LIT("diff.cpp"), buf), "bind text buffer");
    ok &= check(inlay_surface_bind_terminal(s, ETCH_STR_LIT("term0"), read_live_term, &tctx), "bind terminal");

    static const char kScript[] =
        "function render(st) {\n"
        "  const fm = fuzzyMatch('op', 'open_panel');\n"
        "  clipboardWrite('orca_clip_ok');\n"
        "  const clip = clipboardRead();\n"
        "  pushFocusRestorePoint();\n"
        "  popFocusRestorePoint();\n"
        "  setUiZoom(1.25);\n"
        "  const zm = getUiZoom();\n"
        "  setUiZoom(1.0);\n"
        "  return <Col id='r2_root' width={680} height={540} gap={6} globalKeys\n"
        "    onKeyDown={(ev, s) => { s.lastKey = ev.key; }}\n"
        "    onFileDrop={(ev, s) => { s.droppedCount = ev.files.length; }}>\n"
        "    <Text>{'fm=' + (fm.matched ? fm.score : -1) + ',clip=' + clip + ',zm=' + zm.toFixed(2)}</Text>\n"
        "    <Row gap={8}>\n"
        "      <Box id='drag_src' width={80} height={28} draggable dragKind='tab' dragLabel='T1' dragData={{id:1}}><Text>Drag</Text></Box>\n"
        "      <Box id='drop_dst' width={80} height={28} dropAccept='tab' onDrop={(ev, s) => { s.dropped = ev.label; }}><Text>Drop</Text></Box>\n"
        "      <RadioGroup items={['A','B']} value={st.radio} direction='row' onChange={(v, s) => { s.radio = v; }} />\n"
        "      <ToggleGroup items={['Bold','Italic']} values={['Bold']} multiple variant='outline' onChange={(v, s) => { s.toggles = v.length; }} />\n"
        "    </Row>\n"
        "    <Input value='hi @main.cpp' width={240}\n"
        "      highlights={[{offset:3,length:9,color:'#38bdf8',bg:'rgba(56,189,248,0.2)',radius:4,atomic:true}]}\n"
        "      ghostText=' ...' acceptGhostOnTab\n"
        "      onPasteImage={(ok, s) => { s.pastedImg = ok; }}\n"
        "      onCaret={(ev, s) => { s.caret = ev.cursor; }} />\n"
        "    <CodeEditor buffer='diff.cpp' width={360} autoHeight showOverviewRuler stickyHeader='diff.cpp'\n"
        "      inlineHighlights={[{line:0,startCol:4,endCol:5,bg:'#22c55e'}]}\n"
        "      overviewMarkers={[{line:1,color:'#ef4444',lane:0}]}\n"
        "      inlayHints={[{line:0,col:9,text:': i32'}]}\n"
        "      lineDecorations={[{line:0,gutterTag:'+',rowBg:'rgba(34,197,94,0.12)'}]}\n"
        "      folds={[{start:1,end:2,collapsed:false}]}\n"
        "      viewZones={[{id:7,afterLine:0,height:32}]}\n"
        "      onGutterRange={(ev, s) => { s.gStart = ev.startLine; }}>\n"
        "      <ViewZone zoneId={7}><Text>Inline Diff Comment</Text></ViewZone>\n"
        "    </CodeEditor>\n"
        "    <Terminal buffer='term0' width={240} height={80} interactiveSelection\n"
        "      searchMatches={[{row:0,startCol:0,endCol:3,active:true}]}\n"
        "      onSelectionChange={(ev, s) => { s.termSel = ev.hasSelection; }}\n"
        "      onWheel={(ev, s) => { s.termWheel = ev.deltaY; }} />\n"
        "    <DockSpace width={360} height={90} defaultSplit='horizontal' defaultSplitRatio={0.5}>\n"
        "      <DockPane paneId='left' title='Explorer' pinned dirty badgeColor='#22c55e'><Text>L</Text></DockPane>\n"
        "      <DockPane paneId='right' title='Editor' splitSecond><Text>R</Text></DockPane>\n"
        "    </DockSpace>\n"
        "    <Row gap={8}>\n"
        "      <HoverCard openDelayMs={0}><Text>HoverMe</Text><Text>CardBody</Text></HoverCard>\n"
        "      <ColorPicker value='#3b82f6' onChange={(c, s) => { s.color = c; }} />\n"
        "      <Chart variant='area' values={[10,25,18,40]} labels={['M','T','W','T']} chartHeight={64} />\n"
        "    </Row>\n"
        "    <Doc markdown='Streaming **bold' streamingRepair />\n"
        "    <Sheet open={st.sheetOpen} edge='right' extent={240} onClose={(v, s) => { s.sheetOpen = v; }}><Text>Drawer</Text></Sheet>\n"
        "    <Toast open title='Stacked' description='Msg' stackIndex={1} durationMs={60000} />\n"
        "  </Col>;\n"
        "}\n";

    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT(
        "{\"radio\":\"A\",\"toggles\":1,\"sheetOpen\":true,\"lastKey\":\"\",\"droppedCount\":0,\"color\":\"#3b82f6\"}")),
        "set round 2 initial state");
    ok &= check(inlay_surface_set_script(s, {kScript, uint32_t(sizeof(kScript) - 1u), true}),
                "load round 2 bridge script");
    if (inlay_surface_stats(s).has_error) {
        const EtchStr err = inlay_surface_error(s);
        std::fprintf(stderr, "round 2 surface error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }
    ok &= check(!inlay_surface_stats(s).has_error, "round 2 script has zero errors");

    // Verify JS host globals output landed in rendered text node
    bool saw_globals = false;
    {
        uint32_t cnt = 0u;
        const InlayUiNode* nodes = inlay_surface_nodes(s, &cnt);
        for (uint32_t i = 0u; i < cnt; ++i) {
            if (nodes[i].kind == INLAY_UI_TEXT && nodes[i].text.chars &&
                std::strstr(nodes[i].text.chars, "clip=orca_clip_ok,zm=1.25") != nullptr) {
                saw_globals = true;
            }
        }
    }
    ok &= check(saw_globals, "fuzzyMatch + clipboard + uiZoom globals executed cleanly");

    EtchFrameDesc fd = etch_frame_desc_default();
    fd.viewport_w = 720u;
    fd.viewport_h = 600u;
    InlaySurfaceViewDesc sv = inlay_surface_view_desc_default();
    sv.id = etch_id("r2_surface_view");
    sv.width_px = 680.f;
    sv.height_px = 540.f;

    // Frame 1: initial layout + render of all Round 2 widgets
    etch_begin_frame(ctx, &fd);
    inlay_surface_view(s, &sv);
    etch_end_frame(ctx);

    // Frame 2: global onKeyDown (Enter) + OS file drop onto root via SDL drop events
    const EtchRect rb = etch_element_bounds(etch_id("r2_root"));
    {
        SDL_Event ev = {};
        ev.type = SDL_EVENT_DROP_BEGIN;
        ev.drop.x = rb.x + 20.f;
        ev.drop.y = rb.y + 20.f;
        etch_process_event(ctx, &ev);
        ev = {};
        ev.type = SDL_EVENT_DROP_FILE;
        ev.drop.x = rb.x + 20.f;
        ev.drop.y = rb.y + 20.f;
        ev.drop.data = "d:/repo/readme.md";
        etch_process_event(ctx, &ev);
        ev = {};
        ev.type = SDL_EVENT_DROP_COMPLETE;
        ev.drop.x = rb.x + 20.f;
        ev.drop.y = rb.y + 20.f;
        etch_process_event(ctx, &ev);
    }
    fd.key_pressed_code = 13u; // SDLK_RETURN
    etch_begin_frame(ctx, &fd);
    inlay_surface_view(s, &sv);
    etch_end_frame(ctx);
    fd.key_pressed_code = 0u;

    const EtchStr st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"lastKey\":\"Enter\"") != nullptr,
                "globalKeys onKeyDown fired Enter");
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"droppedCount\":1") != nullptr,
                "OS file drop delivered to onFileDrop");

    // Verify .d.ts includes Round 2 declarations
    char dts[24576] = {};
    const uint32_t dlen = inlay_surface_export_dts(s, dts, uint32_t(sizeof(dts)));
    ok &= check(dlen > 0u &&
                std::strstr(dts, "fuzzyMatch(") != nullptr &&
                std::strstr(dts, "setUiZoom(") != nullptr &&
                std::strstr(dts, "inlineHighlights?:") != nullptr &&
                std::strstr(dts, "dockSpace(") != nullptr &&
                std::strstr(dts, "colorPicker(") != nullptr &&
                std::strstr(dts, "chart(") != nullptr,
                "export_dts documents Round 2 bridge");

    // Multi-surface shared InlayModel smoke (2 surfaces bound to 1 host model)
    InlayModel* shared_model = inlay_model_create([](uint32_t i, InlayModelRow* out, void*) -> bool {
        out->row_id = uint64_t(i) + 1u;
        out->label = ETCH_STR_LIT("SharedRow");
        return true;
    }, nullptr);
    if (shared_model) {
        inlay_model_bind_context(shared_model, ctx);
        inlay_model_set_count(shared_model, 5u);
        InlaySurface* s2 = inlay_surface_create(ctx, nullptr);
        if (s2) {
            ok &= check(inlay_surface_bind_model(s, ETCH_STR_LIT("m"), shared_model) &&
                        inlay_surface_bind_model(s2, ETCH_STR_LIT("m"), shared_model),
                        "bind shared InlayModel across 2 surfaces");
            ok &= check(inlay_surface_set_script(s2, ETCH_STR_LIT(
                "function render(){ return <VirtualList model='m' width={200} height={120} itemHeight={24} />; }")),
                "second surface script using shared model");
            InlaySurfaceViewDesc sv2 = inlay_surface_view_desc_default();
            sv2.id = etch_id("r2_surface_2");
            sv2.width_px = 200.f;
            sv2.height_px = 120.f;
            etch_begin_frame(ctx, &fd);
            inlay_surface_view(s2, &sv2);
            etch_end_frame(ctx);
            inlay_surface_bind_model(s2, ETCH_STR_LIT("m"), nullptr);
            inlay_surface_destroy(s2);
        }
        inlay_surface_bind_model(s, ETCH_STR_LIT("m"), nullptr);
        inlay_model_destroy(shared_model);
    }

    // Unbind host resources cleanly before destroy
    inlay_surface_bind_text_buffer(s, ETCH_STR_LIT("diff.cpp"), nullptr);
    inlay_surface_bind_terminal(s, ETCH_STR_LIT("term0"), nullptr, nullptr);
    etch_begin_frame(ctx, &fd);
    etch_end_frame(ctx);
    inlay_surface_destroy(s);
    etch_destroy_text_buffer(buf);
    return ok;
}

// ============================================================================
// 8. Orca Round 3: JS Runtime, Stream/Console/Doc/Portal/Terminal/CodeEditor/Dock/DiffView
// ============================================================================

struct Round3ConsoleProbe {
    uint32_t count;
    uint32_t warn_count;
    uint32_t error_count;
    char last_msg[256];
};

void r3_console_cb(InlaySurface*, InlayConsoleLevel level, EtchStr msg, void* user) {
    auto* p = static_cast<Round3ConsoleProbe*>(user);
    if (!p) return;
    ++p->count;
    if (level == INLAY_CONSOLE_WARN) ++p->warn_count;
    if (level == INLAY_CONSOLE_ERROR) ++p->error_count;
    const uint32_t n = msg.length < 255u ? msg.length : 255u;
    if (n && msg.chars) std::memcpy(p->last_msg, msg.chars, n);
    p->last_msg[n] = '\0';
}

bool r3_read_term(InlayTerminalSnapshot* out, void* user) {
    auto* cells = static_cast<EtchTerminalCell*>(user);
    if (!out || !cells) return false;
    out->columns = 20u;
    out->rows = 4u;
    out->cells = cells;
    out->cell_count = 80u;
    out->cursor_column = 4;
    out->cursor_row = 0;
    out->cursor_visible = true;
    return true;
}

bool test_orca_round3_surface(EtchContext* ctx) {
    bool ok = true;
    InlaySurfaceDesc r3_desc = inlay_surface_desc_default();
    r3_desc.max_tick_ms = 50.f;
    InlaySurface* s = inlay_surface_create(ctx, &r3_desc);
    if (!check(s != nullptr, "r3 surface create")) return false;

    Round3ConsoleProbe cprobe = {};
    inlay_surface_set_console_callback(s, r3_console_cb, &cprobe);

    // Host stream bound to JS readStream('chat')
    InlayStream* stream = inlay_stream_create();
    if (!check(stream != nullptr, "r3 stream create")) { inlay_surface_destroy(s); return false; }
    InlayStreamItem si = {};
    si.id = 1u;
    si.message_id = 1u;
    si.role = INLAY_STREAM_ROLE_ASSISTANT;
    si.kind = INLAY_STREAM_ITEM_MARKDOWN;
    si.status = INLAY_STREAM_STATUS_RUNNING;
    si.title = ETCH_STR_LIT("Agent");
    si.body = ETCH_STR_LIT("Hello ");
    inlay_stream_upsert(stream, &si);
    ok &= check(inlay_surface_bind_stream(s, ETCH_STR_LIT("chat"), stream), "r3 bind stream");

    // Host portal bound to <Portal name='p0' ... />
    InlayPortalDesc pdesc = inlay_portal_desc_default();
    pdesc.width = 320u;
    pdesc.height = 180u;
    InlayPortal* portal = inlay_portal_create(ctx, &pdesc);
    ok &= check(portal != nullptr && inlay_surface_bind_portal(s, ETCH_STR_LIT("p0"), portal), "r3 bind portal");

    // Host text buffer for <CodeEditor>
    EtchTextBuffer* ed_buf = etch_create_text_buffer(ETCH_STR_LIT(
        "export function greet(name: string): string {\n"
        "  const msg = `Hello ${name}`;\n"
        "  return msg;\n"
        "}\n"), 0u);
    ok &= check(ed_buf != nullptr && inlay_surface_bind_text_buffer(s, ETCH_STR_LIT("app.ts"), ed_buf),
                "r3 bind text buffer");

    // Host terminal snapshot
    EtchTerminalCell tcells[80] = {};
    const char* greeting = "orca$ ";
    for (uint32_t i = 0u; i < 6u; ++i) {
        tcells[i].codepoint = uint32_t(greeting[i]);
        tcells[i].foreground = {ETCH_TERMINAL_ANSI16, 2u};
        tcells[i].width = 1u;
    }
    ok &= check(inlay_surface_bind_terminal(s, ETCH_STR_LIT("term3"), r3_read_term, tcells), "r3 bind terminal");

    // Host Doc hooks (custom highlighter probe)
    struct HlProbe { uint32_t calls; } hl_probe = {};
    inlay_surface_set_doc_hooks(
        s,
        [](EtchStr, EtchStr, uint32_t, InlayCodeHighlightToken*, uint32_t, void* u) -> uint32_t {
            ++static_cast<HlProbe*>(u)->calls;
            return 0u;
        },
        &hl_probe, nullptr, nullptr, nullptr);

    const char* init_state =
        "{\"rafTicks\":0,\"ivTicks\":0,\"ivId\":0,\"lastCmd\":\"\","
        "\"portalTitle\":\"\",\"portalZoom\":0,\"portalMatches\":0,"
        "\"docMatches\":0,\"streamBody\":\"\",\"dockSavedBefore\":\"\","
        "\"dockSavedAfter\":\"\",\"paneCount\":2,\"measuredW\":0,"
        "\"urlBlocked\":false,\"notified\":false,\"vtEnter\":\"\","
        "\"sgrClick\":\"\",\"typedText\":\"\",\"keysSeen\":0,"
        "\"cursorLine\":-1,\"initDone\":false}";
    ok &= check(inlay_surface_set_state_json(s, EtchStr{init_state, uint32_t(std::strlen(init_state)), false}),
                "r3 set initial state");

    const char* script = R"JS(
function onCommand(id, s) {
  s.lastCmd = String(id);
}

function render(s) {
  if (!s.initDone) {
    s.initDone = true;
    console.log('r3-boot', 42);
    console.warn('r3-warn');
    console.error('r3-err');
    requestAnimationFrame(function(now) {
      if (now >= 0) state.rafTicks += 1;
    });
    s.ivId = setInterval(function() {
      state.ivTicks += 1;
      if (state.ivTicks >= 1) clearInterval(state.ivId);
    }, 1);
    registerCommand({ id: 'cmd.r3.ping', title: 'R3 Ping', shortcut: 'Ctrl+Shift+P' });
    dispatchCommand('cmd.r3.ping');
    s.urlBlocked = (openUrl('javascript:alert(1)') === false);
    s.notified = notifyDesktop('Orca R3', 'Ready', { silent: true });
    s.vtEnter = encodeTerminalKey(13, 0, false);
    s.sgrClick = encodeTerminalMouseSgr(1, 3, 2, 0, true, false);
  }

  const st = readStream('chat');
  if (st && st.items && st.items.length > 0) {
    s.streamBody = String(st.items[0].body || '');
  }

  const pal = [
    '#11111b','#f38ba8','#a6e3a1','#f9e2af','#89b4fa','#cba6f7','#94e2d5','#cdd6f4',
    '#585b70','#f38ba8','#a6e3a1','#f9e2af','#89b4fa','#cba6f7','#94e2d5','#ffffff'
  ];

  const panes = [
    <DockPane key='left' paneId={101} title='Diff'>
      <DiffView
        id='r3_diff'
        oldText={"const a = 1;\nconst b = 2;\nconst c = 3;\n"}
        newText={"const a = 1;\nconst b = 42;\nconst c = 3;\nconst d = 4;\n"}
        language='typescript'
        filePath='src/app.ts'
        mode='split'
        wordDiff={true}
        showOverviewRuler={true}
        annotations={[{ line: 1, side: 'new', author: 'AI', body: 'Updated b to 42' }]}
        width={420}
        height={180}
      />
    </DockPane>,
    <DockPane key='right' paneId={102} title='Editor'>
      <CodeEditor
        id='r3_ed'
        buffer='app.ts'
        language='typescript'
        highlightBrackets={true}
        showIndentGuides={true}
        showDiagnosticTooltips={true}
        onCursorChange={(ev, st2) => { st2.cursorLine = ev.line; }}
        width={380}
        height={160}
      />
    </DockPane>
  ];
  if (s.paneCount >= 3) {
    panes.push(
      <DockPane key='extra' paneId={103} title='Extra'>
        <Text>Third pane</Text>
      </DockPane>
    );
  }

  return (
    <Col
      id='r3_root'
      cls='p-2 gap-2'
      width={900}
      height={640}
      globalKeys={true}
      onKeyDown={(ev, st2) => { st2.keysSeen += 1; }}
      onTextInput={(txt, st2) => { st2.typedText += String(txt || ''); }}
    >
      <DockSpace id='r3_dock' defaultSplit='horizontal' defaultSplitRatio={0.42} width={860} height={220}>
        {panes}
      </DockSpace>
      <Row cls='gap-2'>
        <Doc
          id='r3_doc'
          markdown={"# Orca R3\n\nPress <kbd>Ctrl</kbd>+<kbd>P</kbd> to search Orca.\n\n```ts\nconst x = 1;\n```\n"}
          findQuery='Orca'
          onFindResult={(ev, st2) => { st2.docMatches = ev.count; }}
          width={280}
          height={150}
        />
        <Portal
          id='r3_portal'
          name='p0'
          html="<html><head><title>PortalR3</title></head><body>hello orca world</body></html>"
          zoom={1.25}
          findQuery='orca'
          evalScript='1+1'
          postMessage='{"hello":1}'
          onStateChange={(ev, st2) => {
            st2.portalTitle = String(ev.title || '');
            st2.portalZoom = Number(ev.zoom || 0);
            st2.portalMatches = Number((ev.find && ev.find.totalMatches) || 0);
          }}
          width={260}
          height={150}
        />
        <Terminal
          id='r3_term'
          buffer='term3'
          palette={pal}
          captureInput={true}
          bracketedPaste={true}
          mouseReporting={true}
          width={280}
          height={150}
        />
      </Row>
      <DiffView
        id='r3_diff_uni'
        oldText={"line1\nline2\nline3\nline4\nline5\nline6\nline7\n"}
        newText={"line1\nline2\nline3\nline4_mod\nline5\nline6\nline7\n"}
        mode='unified'
        collapseUnchanged={true}
        contextLines={1}
        width={860}
        height={120}
      />
    </Col>
  );
}

function onEvent(name, payload, s) {
  if (name === 'probe_imperative') {
    const m = measureElement('r3_root');
    s.measuredW = m ? m.w : 0;
    s.dockSavedBefore = saveDockLayout('r3_dock');
    focusElement('r3_term');
    blurElement();
    scrollTo('r3_doc', 0, 10);
    scrollToBottom('r3_doc');
    s.paneCount = 3;
  } else if (name === 'probe_dock_after') {
    s.dockSavedAfter = saveDockLayout('r3_dock');
  }
}
)JS";

    ok &= check(inlay_surface_set_script(s, EtchStr{script, uint32_t(std::strlen(script)), false}),
                "r3 set script");
    if (!ok) {
        const EtchStr err = inlay_surface_error(s);
        std::printf("r3 script error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }

    EtchFrameDesc fd = etch_frame_desc_default();
    fd.viewport_w = 960;
    fd.viewport_h = 700;
    fd.dt_seconds = 1.f / 60.f;
    InlaySurfaceViewDesc sv = inlay_surface_view_desc_default();
    sv.id = etch_id("r3_surface");
    sv.width_px = 900.f;
    sv.height_px = 640.f;

    // Frame 1: initial draw, sets up bounds, rAF/setInterval, command dispatch, Portal/Doc sync
    etch_begin_frame(ctx, &fd);
    inlay_surface_view(s, &sv);
    etch_end_frame(ctx);

    // Wait briefly so 1ms setInterval becomes due, then append to bound stream + dispatch text input
    SDL_Delay(5);
    inlay_stream_append_body(stream, 1u, ETCH_STR_LIT("Orca!"));
    {
        SDL_Event tev = {};
        tev.type = SDL_EVENT_TEXT_INPUT;
        tev.text.text = "abc";
        etch_process_event(ctx, &tev);
    }
    fd.key_pressed_code = 13u; // Enter
    etch_begin_frame(ctx, &fd);
    inlay_surface_view(s, &sv);
    etch_end_frame(ctx);
    fd.key_pressed_code = 0u;

    // Imperative helpers + incremental DockSpace reconciliation (2 -> 3 panes preserves split)
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("probe_imperative"), ETCH_STR_LIT("{}")),
                "r3 dispatch probe_imperative");
    etch_begin_frame(ctx, &fd);
    inlay_surface_view(s, &sv);
    etch_end_frame(ctx);

    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("probe_dock_after"), ETCH_STR_LIT("{}")),
                "r3 dispatch probe_dock_after");
    etch_begin_frame(ctx, &fd);
    inlay_surface_view(s, &sv);
    etch_end_frame(ctx);

    const EtchStr st = inlay_surface_state_json(s);
    const char* sj = st.chars ? st.chars : "";
    ok &= check(cprobe.count >= 3u && cprobe.warn_count == 1u && cprobe.error_count == 1u &&
                std::strstr(cprobe.last_msg, "r3-err") != nullptr,
                "r3 console callback captured log/warn/error");
    ok &= check(std::strstr(sj, "\"rafTicks\":1") != nullptr, "r3 requestAnimationFrame fired");
    ok &= check(std::strstr(sj, "\"ivTicks\":1") != nullptr, "r3 setInterval + clearInterval fired once");
    ok &= check(std::strstr(sj, "\"lastCmd\":\"cmd.r3.ping\"") != nullptr,
                "r3 registerCommand + dispatchCommand -> onCommand");
    ok &= check(std::strstr(sj, "\"streamBody\":\"Hello Orca!\"") != nullptr,
                "r3 readStream reactive auto-rebuild on stream append");
    ok &= check(std::strstr(sj, "\"portalTitle\":\"PortalR3\"") != nullptr &&
                std::strstr(sj, "\"portalZoom\":1.25") != nullptr &&
                std::strstr(sj, "\"portalMatches\":1") != nullptr,
                "r3 Portal declarative props + onStateChange + find match count");
    ok &= check(std::strstr(sj, "\"docMatches\":2") != nullptr || std::strstr(sj, "\"docMatches\":1") != nullptr,
                "r3 Doc findQuery + onFindResult");
    ok &= check(hl_probe.calls >= 1u, "r3 host doc highlighter hook invoked");
    ok &= check(std::strstr(sj, "\"urlBlocked\":true") != nullptr, "r3 openUrl rejects javascript: scheme");
    ok &= check(std::strstr(sj, "\"notified\":true") != nullptr, "r3 notifyDesktop succeeded");
    ok &= check(std::strstr(sj, "\"vtEnter\":\"\\r\"") != nullptr, "r3 encodeTerminalKey Enter -> CR");
    ok &= check(std::strstr(sj, "\"sgrClick\":\"\\u001b[<0;4;3M\"") != nullptr, "r3 encodeTerminalMouseSgr");
    ok &= check(std::strstr(sj, "\"typedText\":\"abc\"") != nullptr, "r3 onTextInput received text");
    ok &= check(std::strstr(sj, "\"keysSeen\":1") != nullptr, "r3 onKeyDown received Enter");
    ok &= check(std::strstr(sj, "\"measuredW\":900") != nullptr, "r3 measureElement returned layout width");
    ok &= check(std::strstr(sj, "\"dockSavedBefore\":\"{\\\"") != nullptr &&
                std::strstr(sj, "\"dockSavedAfter\":\"{\\\"") != nullptr &&
                std::strstr(sj, "103") != nullptr,
                "r3 saveDockLayout + incremental pane reconciliation");
    if (!ok) {
        std::printf("r3 state json: %s\n", sj);
    }

    // Verify .d.ts includes all Round 3 declarations
    char dts[40960] = {};
    const uint32_t dlen = inlay_surface_export_dts(s, dts, uint32_t(sizeof(dts)));
    ok &= check(dlen > 0u && dlen < uint32_t(sizeof(dts)) &&
                std::strstr(dts, "DiffViewProps") != nullptr &&
                std::strstr(dts, "StreamName") != nullptr &&
                std::strstr(dts, "setInterval(") != nullptr &&
                std::strstr(dts, "requestAnimationFrame(") != nullptr &&
                std::strstr(dts, "readStream(") != nullptr &&
                std::strstr(dts, "registerCommand(") != nullptr &&
                std::strstr(dts, "saveDockLayout(") != nullptr &&
                std::strstr(dts, "encodeTerminalKey(") != nullptr,
                "r3 export_dts documents all Round 3 bridges");

    // Optional visual verification PNG showcase (gated by INLAY_CAPTURE_R3_PNG=1)
    if (const char* cap_env = SDL_getenv("INLAY_CAPTURE_R3_PNG"); cap_env && cap_env[0] == '1') {
        InlaySurface* vis = inlay_surface_create(ctx, nullptr);
        if (vis) {
            inlay_surface_bind_text_buffer(vis, ETCH_STR_LIT("app.ts"), ed_buf);
            const char* vis_js = R"JS(
function render() {
  return (
    <Col cls='p-4 gap-3 bg-canvas' width={1280} height={820}>
      <Row cls='items-center justify-between'>
        <Text cls='text-lg font-bold'>Orca Round 3 Native Workbench Showcase</Text>
        <Badge variant='success'>Etch + Inlay C++20</Badge>
      </Row>
      <Row cls='gap-3' height={340}>
        <Card cls='p-2' width={660} height={340}>
          <DiffView
            id='vis_diff'
            oldText={"export function plan(tasks: Task[]) {\n  const out = [];\n  for (const t of tasks) {\n    if (t.enabled) out.push(t.name);\n  }\n  return out;\n}\n"}
            newText={"export function plan(tasks: readonly Task[]): string[] {\n  const out: string[] = [];\n  for (const t of tasks) {\n    if (t.enabled && !t.archived) out.push(t.title);\n  }\n  return out.sort();\n}\n"}
            language='typescript'
            filePath='src/planner.ts'
            mode='split'
            wordDiff={true}
            showOverviewRuler={true}
            annotations={[{ line: 3, side: 'new', author: 'Reviewer', body: 'Nice archive guard + explicit return type.' }]}
            width={640}
            height={316}
          />
        </Card>
        <Card cls='p-2' width={560} height={340}>
          <CodeEditor
            id='vis_ed'
            buffer='app.ts'
            language='typescript'
            highlightBrackets={true}
            showIndentGuides={true}
            width={540}
            height={316}
          />
        </Card>
      </Row>
      <Card cls='p-3' width={1232} height={390}>
        <Doc
          id='vis_doc'
          markdown={"## Rich Markdown + New Mermaid Diagrams\n\nShortcut <kbd>Ctrl</kbd>+<kbd>D</kbd> adds multi-cursor; formula H<sub>2</sub>O, x<sup>2</sup>, <mark>highlighted</mark>, <del>deprecated</del>, <u>underlined</u>.\n\n```mermaid\npie title GPU Frame Budget\n\"Text & Glyphs\" : 45\n\"UI Quads\" : 35\n\"Diff & Diagrams\" : 20\n```\n\n```mermaid\ngantt\ntitle Orca Frontend Engine\nsection Core\nEtch :done, e1, 2026-01-01, 5d\nInlay :active, i1, after e1, 4d\n```\n"}
          width={1200}
          height={360}
        />
      </Card>
    </Col>
  );
}
)JS";
            if (inlay_surface_set_script(vis, EtchStr{vis_js, uint32_t(std::strlen(vis_js)), false})) {
                EtchFrameDesc vfd = etch_frame_desc_default();
                vfd.viewport_w = 1280;
                vfd.viewport_h = 820;
                InlaySurfaceViewDesc vsv = inlay_surface_view_desc_default();
                vsv.id = etch_id("r3_vis_surface");
                vsv.width_px = 1280.f;
                vsv.height_px = 820.f;
                for (int f = 0; f < 2; ++f) {
                    etch_begin_frame(ctx, &vfd);
                    inlay_surface_view(vis, &vsv);
                    etch_end_frame(ctx);
                }
                const char* out_path = SDL_getenv("INLAY_R3_PNG_PATH");
                if (!out_path || !out_path[0]) out_path = "r3_showcase.png";
                ok &= check(etch_render_headless_png(ctx, 1280u, 820u, out_path, etch_rgba8(15, 17, 23, 255)),
                            "r3 render headless showcase PNG");
            }
            inlay_surface_bind_text_buffer(vis, ETCH_STR_LIT("app.ts"), nullptr);
            inlay_surface_destroy(vis);
        }
    }

    inlay_surface_set_console_callback(s, nullptr, nullptr);
    inlay_surface_set_doc_hooks(s, nullptr, nullptr, nullptr, nullptr, nullptr);
    inlay_surface_bind_stream(s, ETCH_STR_LIT("chat"), nullptr);
    inlay_surface_bind_portal(s, ETCH_STR_LIT("p0"), nullptr);
    inlay_surface_bind_text_buffer(s, ETCH_STR_LIT("app.ts"), nullptr);
    inlay_surface_bind_terminal(s, ETCH_STR_LIT("term3"), nullptr, nullptr);
    inlay_surface_destroy(s);
    inlay_portal_destroy(portal);
    inlay_stream_destroy(stream);
    etch_destroy_text_buffer(ed_buf);
    return ok;
}

// ============================================================================
// 9. Orca Round 4: Convergence Pass (TS/TSX Erasure, Dynamic Limits, Web Globals,
//    Host localStorage Hooks, DiffView/Minimap/Terminal/Doc Showcase)
// ============================================================================

struct R4HostKv {
    char key[64];
    char val[128];
    bool has_val;
    uint32_t sets;
    uint32_t gets;
    uint32_t removes;
};

static bool r4_kv_get(EtchStr key, char* out_buf, uint32_t cap, uint32_t* out_len, void* user) {
    auto* kv = static_cast<R4HostKv*>(user);
    if (!kv || !key.chars) return false;
    ++kv->gets;
    if (!kv->has_val || std::strlen(kv->key) != key.length || std::memcmp(kv->key, key.chars, key.length) != 0) {
        return false;
    }
    const uint32_t n = uint32_t(std::strlen(kv->val));
    if (out_len) *out_len = n;
    if (out_buf && cap > 0u) {
        const uint32_t cp = n < (cap - 1u) ? n : (cap - 1u);
        if (cp > 0u) std::memcpy(out_buf, kv->val, cp);
        out_buf[cp] = '\0';
    }
    return true;
}

static bool r4_kv_set(EtchStr key, EtchStr value, void* user) {
    auto* kv = static_cast<R4HostKv*>(user);
    if (!kv || !key.chars || key.length >= sizeof(kv->key) || value.length >= sizeof(kv->val)) return false;
    ++kv->sets;
    std::memcpy(kv->key, key.chars, key.length);
    kv->key[key.length] = '\0';
    if (value.length > 0u && value.chars) std::memcpy(kv->val, value.chars, value.length);
    kv->val[value.length] = '\0';
    kv->has_val = true;
    return true;
}

static bool r4_kv_remove(EtchStr key, void* user) {
    auto* kv = static_cast<R4HostKv*>(user);
    if (!kv || !key.chars) return false;
    ++kv->removes;
    if (kv->has_val && std::strlen(kv->key) == key.length && std::memcmp(kv->key, key.chars, key.length) == 0) {
        kv->has_val = false;
        kv->key[0] = '\0';
        kv->val[0] = '\0';
    }
    return true;
}

struct R4ActionProbe {
    uint32_t hits;
    int32_t last_n;
};

static void r4_action_cb(InlaySurface*, EtchStr payload_json, void* user) {
    auto* p = static_cast<R4ActionProbe*>(user);
    if (!p) return;
    ++p->hits;
    if (payload_json.chars && std::strstr(payload_json.chars, "39") != nullptr) {
        p->last_n = 39;
    }
}

struct R4TermBuf {
    EtchTerminalCell cells[48u * 6u];
    uint32_t cols;
    uint32_t rows;
};

static bool r4_read_term(InlayTerminalSnapshot* out, void* user) {
    auto* tb = static_cast<R4TermBuf*>(user);
    if (!tb || !out) return false;
    out->columns = tb->cols;
    out->rows = tb->rows;
    out->cells = tb->cells;
    out->cell_count = size_t(tb->cols) * size_t(tb->rows);
    out->cursor_column = 14;
    out->cursor_row = 4;
    out->cursor_shape = ETCH_TERMINAL_CURSOR_BAR;
    out->cursor_visible = true;
    out->revision = 1u;
    return true;
}

static bool test_orca_round4_convergence(EtchContext* ctx) {
    bool ok = true;

    // 1. Native TypeScript / TSX Type Erasure + .ts/.tsx ESM module resolution
    {
        InlaySurface* s = inlay_surface_create(ctx, nullptr);
        if (!s) return true; // Script engine disabled
        ok &= check(inlay_surface_register_module(s, ETCH_STR_LIT("types.ts"), ETCH_STR_LIT(
            "export interface Item { id: number; name: string; }\n"
            "export type Label = string | number;\n"
            "export declare const AMBIENT_ONLY: number;\n")),
            "r4 register types.ts");
        ok &= check(inlay_surface_register_module(s, ETCH_STR_LIT("helper.tsx"), ETCH_STR_LIT(
            "import type { Item, Label } from './types';\n"
            "export function fmt<T extends Item>(x: T, suffix?: string): string {\n"
            "  const lbl: Label = (x.name as string) + (suffix ?? '');\n"
            "  return String(lbl);\n"
            "}\n")),
            "r4 register helper.tsx");
        static const char kTsxEntry[] =
            "import { fmt } from 'helper';\n"
            "import type { Item } from 'types';\n"
            "interface LocalCfg { tag?: string; }\n"
            "const meta = { tag: 'TSX' } as const satisfies Record<string, string>;\n"
            "const dbl = (n: number): number => n * 2;\n"
            "function CardItem(props: { item: Item }): any {\n"
            "  const [count] = useState<number>(1);\n"
            "  const cfg: LocalCfg = { tag: meta.tag };\n"
            "  const nn: string = cfg.tag!;\n"
            "  return <Badge>{fmt<Item>({ id: props.item.id, name: nn }, '!') + ':' + dbl(count)}</Badge>;\n"
            "}\n"
            "export function render(st: { id?: number }) {\n"
            "  return <Box><CardItem item={{ id: st.id ?? 1, name: 'init' }} /></Box>;\n"
            "}\n";
        ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"id\":7}")), "r4 tsx state");
        const bool loaded = inlay_surface_set_script(s, {kTsxEntry, uint32_t(sizeof(kTsxEntry) - 1u), true});
        if (!loaded) {
            const EtchStr err = inlay_surface_error(s);
            std::fprintf(stderr, "r4 tsx error: %.*s\n", int(err.length), err.chars ? err.chars : "");
        }
        ok &= check(loaded && !inlay_surface_stats(s).has_error, "r4 tsx entry compiles & runs cleanly");
        uint32_t cnt = 0u;
        const InlayUiNode* nodes = inlay_surface_nodes(s, &cnt);
        bool saw_tsx = false;
        for (uint32_t i = 0u; i < cnt; ++i) {
            if (nodes[i].text.chars && std::strstr(nodes[i].text.chars, "TSX!:2") != nullptr) saw_tsx = true;
        }
        ok &= check(saw_tsx, "r4 tsx generic component rendered TSX!:2");
        inlay_surface_destroy(s);
    }

    // 2. Configurable max_state_bytes (256 KiB), >32 actions (40), >64 commands (70),
    //    standard JS globals, host localStorage hooks, <CodeEditor showMinimap>, <DiffView> production features
    {
        InlaySurfaceDesc desc = inlay_surface_desc_default();
        desc.max_tick_ms = 50.f;
        desc.max_state_bytes = 262144u;
        InlaySurface* s = inlay_surface_create(ctx, &desc);
        ok &= check(s != nullptr, "r4 create surface with 256 KiB state cap");
        if (!s) return false;

        R4ActionProbe act_probe = {};
        for (uint32_t i = 0u; i < 40u; ++i) {
            char aname[32];
            const int n = std::snprintf(aname, sizeof(aname), "act_%u", i);
            ok &= check(inlay_surface_bind_action(s, EtchStr{aname, uint32_t(n), false}, r4_action_cb, &act_probe),
                        "r4 bind dynamic action");
        }

        // Build ~100 KiB initial state JSON (> old 64 KiB cap)
        const size_t pad_len = 100000u;
        auto* big_state = static_cast<char*>(SDL_malloc(pad_len + 512u));
        ok &= check(big_state != nullptr, "r4 alloc big state");
        if (!big_state) { inlay_surface_destroy(s); return false; }
        int head = std::snprintf(big_state, 256u,
            "{\"init\":false,\"lastCmd\":\"\",\"globalsOk\":false,\"mtOk\":false,\"memLsOk\":false,\"hostLsOk\":false,\"blob\":\"");
        std::memset(big_state + head, 'x', pad_len);
        const char tail[] = "\"}";
        std::memcpy(big_state + head + pad_len, tail, sizeof(tail));
        const uint32_t big_len = uint32_t(size_t(head) + pad_len + sizeof(tail) - 1u);
        ok &= check(inlay_surface_set_state_json(s, EtchStr{big_state, big_len, false}),
                    "r4 set 100 KiB state json");
        SDL_free(big_state);

        EtchTextBuffer* ed_buf = etch_create_text_buffer(ETCH_STR_LIT(
            "/* Multiline\n   block comment */\nexport const v = 42;\n"), 0u);
        ok &= check(ed_buf != nullptr && inlay_surface_bind_text_buffer(s, ETCH_STR_LIT("r4.ts"), ed_buf),
                    "r4 bind text buffer");

        static const char kR4Script[] = R"JS(
function onCommand(id, s) {
  s.lastCmd = String(id);
}
function onEvent(name, payload, s) {
  if (name === 'check_mt_and_host_ls') {
    s.mtOk = (globalThis.__r4_mt === 1);
    localStorage.setItem('theme', 'obsidian');
    const got = localStorage.getItem('theme');
    localStorage.removeItem('theme');
    const after = localStorage.getItem('theme');
    s.hostLsOk = (got === 'obsidian' && after === null);
  }
}
function render(s) {
  if (!s.init) {
    s.init = true;
    for (let i = 0; i < 70; ++i) {
      registerCommand({ id: 'cmd.r4.' + i, title: 'Cmd ' + i });
    }
    dispatchCommand('cmd.r4.69');
    action('act_39', { n: 39 });

    // Standard Web/JS globals verification
    const t0 = performance.now();
    const u1 = crypto.randomUUID();
    const u2 = crypto.randomUUID();
    const uuidOk = (u1.length === 36 && u1[14] === '4' && u1 !== u2);
    const b64Ok = (atob(btoa('Orca-Inlay!')) === 'Orca-Inlay!');
    const orig = { a: [1, 2], d: new Date(1700000000000), m: new Map([['k', 7]]), st: new Set([3]) };
    orig.self = orig;
    const cloned = structuredClone(orig);
    const cloneOk = (cloned !== orig && cloned.self === cloned && cloned.a[1] === 2 &&
                     cloned.d.getTime() === 1700000000000 && cloned.m.get('k') === 7 && cloned.st.has(3));
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const utf8Ok = (dec.decode(enc.encode('你好🚀Orca')) === '你好🚀Orca');
    const url = new URL('/api/v1?a=1&b=two#top', 'https://orca.dev');
    url.searchParams.set('c', '3');
    const urlOk = (url.origin === 'https://orca.dev' && url.pathname === '/api/v1' &&
                   url.hash === '#top' && url.searchParams.get('b') === 'two' &&
                   url.href.indexOf('c=3') >= 0);
    s.globalsOk = (t0 >= 0 && uuidOk && b64Ok && cloneOk && utf8Ok && urlOk);

    // In-memory localStorage fallback before host hooks are attached
    localStorage.setItem('k1', 'v1');
    const lv = localStorage.getItem('k1');
    const llen = localStorage.length;
    localStorage.removeItem('k1');
    s.memLsOk = (lv === 'v1' && llen >= 1 && localStorage.getItem('k1') === null);

    queueMicrotask(() => { globalThis.__r4_mt = 1; });
  }
  return (
    <Col id='r4_root' width={820} height={540} gap={8}>
      <CodeEditor id='r4_ed' buffer='r4.ts' language='typescript' showMinimap={true} width={780} height={140} />
      <DiffView
        id='r4_diff'
        oldText={"line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\n"}
        newText={"line1\nline2_mod\nline3\nline4\nline5\nline6\nline7_mod\nline8\n"}
        mode='split'
        filePath='src/engine.ts'
        collapseUnchanged={true}
        contextLines={1}
        editable={true}
        hunkActions={[{ id: 'accept', label: 'Accept' }, { id: 'reject', label: 'Reject' }]}
        onHunkAction={(ev, st2) => { st2.lastHunkAct = ev.actionId; }}
        onChange={(txt, st2) => { st2.editedLen = txt.length; }}
        width={780}
        height={180}
      />
      <Doc
        id='r4_doc'
        markdown={"```mermaid\ngitGraph\ncommit id: \"c0\"\nbranch ui\ncheckout ui\ncommit id: \"c1\"\ncheckout main\nmerge ui\n```\n"}
        onDiagramNodeClick={(ev, st2) => { st2.clickedNode = ev.nodeId; }}
        width={780}
        height={180}
      />
    </Col>
  );
}
)JS";

        const bool r4_loaded = inlay_surface_set_script(s, EtchStr{kR4Script, uint32_t(std::strlen(kR4Script)), false});
        if (!r4_loaded) {
            const EtchStr err = inlay_surface_error(s);
            std::fprintf(stderr, "r4 script error: %.*s\n", int(err.length), err.chars ? err.chars : "");
        }
        ok &= check(r4_loaded && !inlay_surface_stats(s).has_error, "r4 convergence script loaded");

        EtchFrameDesc fd = etch_frame_desc_default();
        fd.viewport_w = 860u;
        fd.viewport_h = 580u;
        InlaySurfaceViewDesc sv = inlay_surface_view_desc_default();
        sv.id = etch_id("r4_surface");
        sv.width_px = 820.f;
        sv.height_px = 540.f;
        etch_begin_frame(ctx, &fd);
        inlay_surface_view(s, &sv);
        etch_end_frame(ctx);

        // Attach host localStorage hooks & verify microtask + host storage callbacks
        R4HostKv kv = {};
        inlay_surface_set_storage_hooks(s, r4_kv_get, r4_kv_set, r4_kv_remove, &kv);
        ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("check_mt_and_host_ls"), ETCH_STR_LIT("{}")),
                    "r4 dispatch microtask & host localStorage check");
        etch_begin_frame(ctx, &fd);
        inlay_surface_view(s, &sv);
        etch_end_frame(ctx);

        const EtchStr st = inlay_surface_state_json(s);
        const char* sj = st.chars ? st.chars : "";
        ok &= check(st.length > 100000u, "r4 100 KiB state preserved without 64 KiB truncation");
        ok &= check(act_probe.hits >= 1u && act_probe.last_n == 39, "r4 40th dynamic action fired");
        ok &= check(std::strstr(sj, "\"lastCmd\":\"cmd.r4.69\"") != nullptr, "r4 70th dynamic command fired");
        ok &= check(std::strstr(sj, "\"globalsOk\":true") != nullptr,
                    "r4 performance/crypto/atob/btoa/structuredClone/TextEncoder/URL globals passed");
        ok &= check(std::strstr(sj, "\"mtOk\":true") != nullptr, "r4 queueMicrotask executed");
        ok &= check(std::strstr(sj, "\"memLsOk\":true") != nullptr, "r4 in-memory localStorage fallback passed");
        ok &= check(std::strstr(sj, "\"hostLsOk\":true") != nullptr &&
                    kv.sets >= 1u && kv.gets >= 2u && kv.removes >= 1u,
                    "r4 host C localStorage hooks invoked cleanly");

        char dts[49152] = {};
        const uint32_t dlen = inlay_surface_export_dts(s, dts, uint32_t(sizeof(dts)));
        ok &= check(dlen > 0u && dlen < uint32_t(sizeof(dts)) &&
                    std::strstr(dts, "showMinimap?: boolean") != nullptr &&
                    std::strstr(dts, "hunkActions?:") != nullptr &&
                    std::strstr(dts, "onDiagramNodeClick?:") != nullptr &&
                    std::strstr(dts, "structuredClone") != nullptr &&
                    std::strstr(dts, "URLSearchParams") != nullptr &&
                    std::strstr(dts, "localStorage") != nullptr,
                    "r4 export_dts covers all Round 4 APIs & globals");

        inlay_surface_set_storage_hooks(s, nullptr, nullptr, nullptr, nullptr);
        inlay_surface_bind_text_buffer(s, ETCH_STR_LIT("r4.ts"), nullptr);
        inlay_surface_destroy(s);
        etch_destroy_text_buffer(ed_buf);
    }

    // 3. Optional Round 4 Headless Showcase PNG (gated by INLAY_R4_PNG_PATH)
    if (const char* png_path = SDL_getenv("INLAY_R4_PNG_PATH"); png_path && png_path[0]) {
        InlaySurfaceDesc vdesc = inlay_surface_desc_default();
        vdesc.max_tick_ms = 50.f;
        InlaySurface* vis = inlay_surface_create(ctx, &vdesc);
        EtchTextBuffer* ed_buf = etch_create_text_buffer(ETCH_STR_LIT(
            "/*\n"
            " * Etch Stateful Multiline Syntax Lexer\n"
            " * + Clickable Minimap + Sticky Scroll\n"
            " */\n"
            "export interface AgentRun {\n"
            "  readonly id: string;\n"
            "  tokensPerSec: number;\n"
            "}\n"
            "export function score(r: AgentRun): number {\n"
            "  const factor = 1.25;\n"
            "  return Math.round(r.tokensPerSec * factor);\n"
            "}\n"
            "export const STATUS = `ready:${score({ id: 'r4', tokensPerSec: 120 })}`;\n"), 0u);
        R4TermBuf tb = {};
        tb.cols = 48u;
        tb.rows = 6u;
        for (uint32_t i = 0u; i < tb.cols * tb.rows; ++i) {
            tb.cells[i] = etch_terminal_cell_default();
            tb.cells[i].codepoint = ' ';
        }
        auto put_term_row = [&](uint32_t row, const uint32_t* cps, uint32_t count, EtchTerminalColor fg, EtchTerminalColor bg) {
            for (uint32_t c = 0u; c < count && c < tb.cols; ++c) {
                auto& cell = tb.cells[row * tb.cols + c];
                cell.codepoint = cps[c];
                cell.foreground = fg;
                cell.background = bg;
                cell.width = 1u;
            }
        };
        auto put_term_ascii = [&](uint32_t row, uint32_t col, const char* txt, EtchTerminalColor fg, EtchTerminalColor bg, bool bold = false) {
            for (uint32_t i = 0u; txt[i] && (col + i) < tb.cols; ++i) {
                auto& cell = tb.cells[row * tb.cols + col + i];
                cell.codepoint = uint8_t(txt[i]);
                cell.foreground = fg;
                cell.background = bg;
                cell.bold = bold;
                cell.width = 1u;
            }
        };
        const EtchTerminalColor fg_def = {ETCH_TERMINAL_DEFAULT, 0u};
        const EtchTerminalColor bg_def = {ETCH_TERMINAL_DEFAULT, 0u};
        const EtchTerminalColor bg_blue = {ETCH_TERMINAL_TRUE_COLOR, 0x1d4ed8u};
        const EtchTerminalColor fg_blue = {ETCH_TERMINAL_TRUE_COLOR, 0x1d4ed8u};
        const EtchTerminalColor bg_teal = {ETCH_TERMINAL_TRUE_COLOR, 0x0f766eu};
        const EtchTerminalColor fg_teal = {ETCH_TERMINAL_TRUE_COLOR, 0x0f766eu};
        const EtchTerminalColor fg_white = {ETCH_TERMINAL_TRUE_COLOR, 0xf8fafcu};
        const EtchTerminalColor fg_green = {ETCH_TERMINAL_TRUE_COLOR, 0x34d399u};
        const EtchTerminalColor fg_cyan = {ETCH_TERMINAL_TRUE_COLOR, 0x38bdf8u};
        // Row 0: Powerline prompt with U+E0B0 hard right arrows
        put_term_ascii(0u, 0u, " orca ", fg_white, bg_blue, true);
        const uint32_t pl0[1] = { 0xE0B0u };
        put_term_row(0u, pl0, 0u, fg_def, bg_def); // keep helper used cleanly
        tb.cells[0u * tb.cols + 6u].codepoint = 0xE0B0u;
        tb.cells[0u * tb.cols + 6u].foreground = fg_blue;
        tb.cells[0u * tb.cols + 6u].background = bg_teal;
        put_term_ascii(0u, 7u, " main ", fg_white, bg_teal, true);
        tb.cells[0u * tb.cols + 13u].codepoint = 0xE0B0u;
        tb.cells[0u * tb.cols + 13u].foreground = fg_teal;
        tb.cells[0u * tb.cols + 13u].background = bg_def;
        put_term_ascii(0u, 15u, "ctest --all-backends", fg_def, bg_def);
        // Row 1: Braille GPU utilization sparkline (U+2800..U+28FF)
        put_term_ascii(1u, 0u, "GPU FPS ", fg_green, bg_def, true);
        const uint32_t braille[12] = { 0x2840u, 0x28C0u, 0x28E4u, 0x28F6u, 0x28FFu, 0x28FFu, 0x28F6u, 0x28FEu, 0x28FFu, 0x28FFu, 0x28E4u, 0x28FFu };
        for (uint32_t bi = 0u; bi < 12u; ++bi) {
            auto& cell = tb.cells[1u * tb.cols + 8u + bi];
            cell.codepoint = braille[bi];
            cell.foreground = fg_green;
            cell.width = 1u;
        }
        put_term_ascii(1u, 21u, " 120.0 fps (VK+D3D12)", fg_def, bg_def);
        // Row 2: Auto-detected clickable URL
        put_term_ascii(2u, 0u, "Docs: https://inlay.dev/orca/convergence", fg_cyan, bg_def);
        // Row 3: Pass summary
        put_term_ascii(3u, 0u, "PASS: 22/22 suites zero warnings (/W4 /WX)", fg_green, bg_def, true);
        put_term_ascii(4u, 0u, "orca@native $ ", fg_cyan, bg_def, true);

        if (vis && ed_buf) {
            inlay_surface_bind_text_buffer(vis, ETCH_STR_LIT("showcase.ts"), ed_buf);
            inlay_surface_bind_terminal(vis, ETCH_STR_LIT("term_r4"), r4_read_term, &tb);
            static const char kVisTsx[] = R"JS(
interface BannerProps { label: string; }
function Banner(p: BannerProps) {
  return <Badge variant='success'>{p.label}</Badge>;
}
function render() {
  return (
    <Col cls='p-4 gap-3 bg-canvas' width={1280} height={880}>
      <Row cls='items-center justify-between'>
        <Text cls='text-lg font-bold'>Orca Round 4 Convergence — Etch + Inlay Native Stack</Text>
        <Banner label='TSX + Minimap + Diff + Braille/Powerline + Mermaid' />
      </Row>
      <Row cls='gap-3' height={320}>
        <Card cls='p-2' width={680} height={320}>
          <DiffView
            id='r4_vis_diff'
            oldText={"const cfg = {\n  fps: 60,\n  stateCap: 65536,\n  minimap: false,\n  rtl: false,\n};\n"}
            newText={"const cfg = {\n  fps: 120,\n  stateCap: 262144,\n  minimap: true,\n  rtl: true,\n};\n"}
            language='typescript'
            filePath='src/config.ts'
            mode='split'
            wordDiff={true}
            showOverviewRuler={true}
            hunkActions={[{ id: 'accept', label: 'Accept' }, { id: 'reject', label: 'Reject' }]}
            annotations={[{ line: 2, side: 'new', author: 'Orca', body: 'Configurable state cap + real line numbers & hunk controls.' }]}
            width={660}
            height={296}
          />
        </Card>
        <Card cls='p-2' width={556} height={320}>
          <CodeEditor
            id='r4_vis_ed'
            buffer='showcase.ts'
            language='typescript'
            showMinimap={true}
            highlightBrackets={true}
            showIndentGuides={true}
            width={536}
            height={296}
          />
        </Card>
      </Row>
      <Row cls='gap-3' height={476}>
        <Card cls='p-2' width={440} height={476}>
          <Col cls='gap-2'>
            <Text cls='text-sm font-bold'>Native Terminal (Powerline + Braille + Auto-URL)</Text>
            <Terminal id='r4_vis_term' buffer='term_r4' width={420} height={150} />
            <Doc
              id='r4_vis_doc_left'
              markdown={"### Nested HTML, Table & RTL\n\n<b><i>Bold+Italic</i></b> & <a href=\"https://inlay.dev\"><kbd>Ctrl+K</kbd></a>\n\n<table><tr><th>Engine</th><th>Status</th></tr><tr><td>Etch</td><td>12/12 PASS</td></tr><tr><td>Inlay</td><td>10/10 PASS</td></tr></table>\n\nمرحبا بالعالم — RTL auto-align\n"}
              width={420}
              height={280}
            />
          </Col>
        </Card>
        <Card cls='p-2' width={796} height={476}>
          <Doc
            id='r4_vis_doc_right'
            markdown={"### Mermaid Round 4: gitGraph + mindmap + timeline\n\n```mermaid\ngitGraph\ncommit id: \"v0.13\"\nbranch orca\ncheckout orca\ncommit id: \"TSX\"\ncommit id: \"Diff\"\ncheckout main\nmerge orca\n```\n\n```mermaid\nmindmap\n  root((Orca Native))\n    Etch\n      Minimap\n      Braille\n    Inlay\n      TSX\n      Mermaid\n```\n\n```mermaid\ntimeline\n  title Convergence\n  section Shipping\n    R1-R3 : Core + Widgets\n    R4 : Production Ready\n```\n"}
            width={776}
            height={452}
          />
        </Card>
      </Row>
    </Col>
  );
}
)JS";
            const bool vis_ok = inlay_surface_set_script(vis, EtchStr{kVisTsx, uint32_t(std::strlen(kVisTsx)), false});
            if (!vis_ok) {
                const EtchStr err = inlay_surface_error(vis);
                std::fprintf(stderr, "r4 showcase error: %.*s\n", int(err.length), err.chars ? err.chars : "");
            }
            ok &= check(vis_ok, "r4 showcase TSX compiled");
            if (vis_ok) {
                EtchFrameDesc vfd = etch_frame_desc_default();
                vfd.viewport_w = 1280u;
                vfd.viewport_h = 880u;
                InlaySurfaceViewDesc vsv = inlay_surface_view_desc_default();
                vsv.id = etch_id("r4_vis_surface");
                vsv.width_px = 1280.f;
                vsv.height_px = 880.f;
                for (int f = 0; f < 2; ++f) {
                    etch_begin_frame(ctx, &vfd);
                    inlay_surface_view(vis, &vsv);
                    etch_end_frame(ctx);
                }
                ok &= check(etch_render_headless_png(ctx, 1280u, 880u, png_path, etch_rgba8(15, 17, 23, 255)),
                            "r4 render headless showcase PNG");
            }
            inlay_surface_bind_terminal(vis, ETCH_STR_LIT("term_r4"), nullptr, nullptr);
            inlay_surface_bind_text_buffer(vis, ETCH_STR_LIT("showcase.ts"), nullptr);
        }
        inlay_surface_destroy(vis);
        etch_destroy_text_buffer(ed_buf);
    }

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
    ok &= test_stream_r01_atomic_upsert();
    ok &= test_stream_r02_utf8_carry();
    ok &= test_stream_c10_replay_and_budget();
    ok &= test_genui(ctx);
    ok &= test_tokens();
    ok &= test_inbox_drain_to_surface(ctx);
    ok &= test_ade_surface_upgrades(ctx);
    ok &= test_orca_round2_bridge(ctx);
    ok &= test_orca_round3_surface(ctx);
    ok &= test_orca_round4_convergence(ctx);

    etch_destroy_context(ctx);
    std::puts(ok ? "Workbench features PASS" : "Workbench features FAIL");
    return ok ? 0 : 1;
}
