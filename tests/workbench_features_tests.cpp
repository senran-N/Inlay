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

    etch_destroy_context(ctx);
    std::puts(ok ? "Workbench features PASS" : "Workbench features FAIL");
    return ok ? 0 : 1;
}
