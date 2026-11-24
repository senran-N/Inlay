#include "inlay_internal.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kGenUiSchemaVersion = 1u;
constexpr uint32_t kMaxGenUiDepth = 16u;
constexpr uint32_t kMaxGenUiNodes = 512u;
constexpr uint32_t kMaxPayloadDepth = 8u;
constexpr uint32_t kMaxPoolBytes = 2u * 1024u * 1024u;
constexpr uint32_t kMaxScriptOutBytes = 1024u * 1024u;
constexpr uint32_t kMaxActionLen = 63u;
constexpr uint32_t kNoneIdx = UINT32_MAX;

enum class GenUiType : uint8_t {
    Box = 0,
    Row,
    Col,
    Card,
    Text,
    Badge,
    Button,
    Progress,
    Divider,
    Spacer,
    Input,
    Toggle,
    Markdown,
    Math,
    Diagram
};

enum class GenUiValKind : uint8_t {
    None = 0,
    String,
    Number,
    Bool
};

enum GenUiSeenKey : uint32_t {
    KeyType     = 1u << 0,
    KeyId       = 1u << 1,
    KeyCls      = 1u << 2,
    KeyText     = 1u << 3,
    KeyLabel    = 1u << 4,
    KeyValue    = 1u << 5,
    KeyAction   = 1u << 6,
    KeyPayload  = 1u << 7,
    KeyChildren = 1u << 8
};

struct StrSlice {
    uint32_t off;
    uint32_t len;
};

struct GenUiNode {
    GenUiType type;
    GenUiValKind value_kind;
    bool has_id;
    bool has_cls;
    bool has_text;
    bool has_label;
    bool has_action;
    bool has_payload;
    bool value_bool;
    double value_num;
    StrSlice id;
    StrSlice cls;
    StrSlice text;
    StrSlice label;
    StrSlice action;
    StrSlice value_str;
    StrSlice payload_json;
    uint32_t first_child;
    uint32_t next_sibling;
};

struct GenUiTree {
    GenUiNode* nodes;
    uint32_t node_count;
    uint32_t node_cap;
    char* pool;
    uint32_t pool_len;
    uint32_t pool_cap;
};

void tree_free(GenUiTree& t) noexcept {
    SDL_free(t.nodes);
    SDL_free(t.pool);
    t = {};
}

bool pool_append(GenUiTree& t, const char* bytes, uint32_t n) noexcept {
    if (n == 0u) return true;
    if (n > kMaxPoolBytes - t.pool_len) return false;
    if (!inlay_grow_pod(t.pool, t.pool_cap, t.pool_len + n, 512u, kMaxPoolBytes)) return false;
    std::memcpy(t.pool + t.pool_len, bytes, n);
    t.pool_len += n;
    return true;
}

bool pool_append_char(GenUiTree& t, char c) noexcept {
    return pool_append(t, &c, 1u);
}

bool pool_append_lit(GenUiTree& t, const char* lit) noexcept {
    return pool_append(t, lit, static_cast<uint32_t>(std::strlen(lit)));
}

bool pool_append_js_quoted(GenUiTree& t, const char* bytes, uint32_t n) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    if (!pool_append_char(t, '"')) return false;
    uint32_t i = 0u;
    while (i < n) {
        const uint8_t ch = static_cast<uint8_t>(bytes[i]);
        // Escape U+2028 / U+2029 line separators for JS safety.
        if (ch == 0xE2u && i + 2u < n && static_cast<uint8_t>(bytes[i + 1u]) == 0x80u &&
            (static_cast<uint8_t>(bytes[i + 2u]) == 0xA8u || static_cast<uint8_t>(bytes[i + 2u]) == 0xA9u)) {
            const char* esc = (static_cast<uint8_t>(bytes[i + 2u]) == 0xA8u) ? "\\u2028" : "\\u2029";
            if (!pool_append(t, esc, 6u)) return false;
            i += 3u;
            continue;
        }
        switch (ch) {
            case '"':  if (!pool_append(t, "\\\"", 2u)) return false; break;
            case '\\': if (!pool_append(t, "\\\\", 2u)) return false; break;
            case '\b': if (!pool_append(t, "\\b", 2u)) return false; break;
            case '\f': if (!pool_append(t, "\\f", 2u)) return false; break;
            case '\n': if (!pool_append(t, "\\n", 2u)) return false; break;
            case '\r': if (!pool_append(t, "\\r", 2u)) return false; break;
            case '\t': if (!pool_append(t, "\\t", 2u)) return false; break;
            case '<':  if (!pool_append(t, "\\u003c", 6u)) return false; break;
            default:
                if (ch < 0x20u) {
                    const char esc[6] = {'\\', 'u', '0', '0', kHex[ch >> 4], kHex[ch & 15u]};
                    if (!pool_append(t, esc, 6u)) return false;
                } else {
                    if (!pool_append_char(t, static_cast<char>(ch))) return false;
                }
                break;
        }
        ++i;
    }
    return pool_append_char(t, '"');
}

inline int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

uint32_t encode_utf8(uint32_t cp, char out[4]) noexcept {
    if (cp < 0x80u) {
        out[0] = static_cast<char>(cp);
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (cp >> 6));
        out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out[0] = static_cast<char>(0xE0u | (cp >> 12));
        out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = static_cast<char>(0xF0u | (cp >> 18));
    out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
    return 4u;
}

inline bool str_eq(const char* s, uint32_t n, const char* lit) noexcept {
    const size_t m = std::strlen(lit);
    return n == m && std::memcmp(s, lit, m) == 0;
}

bool parse_component_type(const char* s, uint32_t n, GenUiType* out) noexcept {
    if (str_eq(s, n, "box"))      { *out = GenUiType::Box; return true; }
    if (str_eq(s, n, "row"))      { *out = GenUiType::Row; return true; }
    if (str_eq(s, n, "col"))      { *out = GenUiType::Col; return true; }
    if (str_eq(s, n, "card"))     { *out = GenUiType::Card; return true; }
    if (str_eq(s, n, "text"))     { *out = GenUiType::Text; return true; }
    if (str_eq(s, n, "badge"))    { *out = GenUiType::Badge; return true; }
    if (str_eq(s, n, "button"))   { *out = GenUiType::Button; return true; }
    if (str_eq(s, n, "progress")) { *out = GenUiType::Progress; return true; }
    if (str_eq(s, n, "divider"))  { *out = GenUiType::Divider; return true; }
    if (str_eq(s, n, "spacer"))   { *out = GenUiType::Spacer; return true; }
    if (str_eq(s, n, "input"))    { *out = GenUiType::Input; return true; }
    if (str_eq(s, n, "toggle"))   { *out = GenUiType::Toggle; return true; }
    if (str_eq(s, n, "markdown")) { *out = GenUiType::Markdown; return true; }
    if (str_eq(s, n, "math"))     { *out = GenUiType::Math; return true; }
    if (str_eq(s, n, "diagram"))  { *out = GenUiType::Diagram; return true; }
    return false;
}

bool is_disallowed_key(const char* s, uint32_t n) noexcept {
    if (n >= 2u && s[0] == '_' && s[1] == '_') return true;
    if (n >= 3u && s[0] == 'o' && s[1] == 'n' && s[2] >= 'A' && s[2] <= 'Z') return true;
    return str_eq(s, n, "script") || str_eq(s, n, "eval") || str_eq(s, n, "onClick") ||
           str_eq(s, n, "__proto__") || str_eq(s, n, "constructor") || str_eq(s, n, "prototype") ||
           str_eq(s, n, "function") || str_eq(s, n, "code") || str_eq(s, n, "js") ||
           str_eq(s, n, "html") || str_eq(s, n, "innerHTML") ||
           str_eq(s, n, "dangerouslySetInnerHTML") || str_eq(s, n, "render") ||
           str_eq(s, n, "renderItem");
}

bool is_safe_action_name(const char* s, uint32_t n) noexcept {
    if (n == 0u || n > kMaxActionLen) return false;
    for (uint32_t i = 0u; i < n; ++i) {
        const char c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

struct GenUiParser {
    const char* src;
    uint32_t len;
    uint32_t pos;
    uint32_t node_count;
    uint32_t error_node_index;
    uint32_t error_offset;
    char error_message[256];
    bool failed;
    GenUiTree* tree;

    bool fail(uint32_t node_idx, uint32_t offset, const char* msg) noexcept {
        if (!failed) {
            failed = true;
            error_node_index = node_idx;
            error_offset = offset;
            std::snprintf(error_message, sizeof(error_message), "%s", msg);
        }
        return false;
    }

    bool fail_fmt(uint32_t node_idx, uint32_t offset, const char* prefix, const char* detail, uint32_t detail_len) noexcept {
        if (!failed) {
            failed = true;
            error_node_index = node_idx;
            error_offset = offset;
            const uint32_t take = detail_len < 64u ? detail_len : 64u;
            std::snprintf(error_message, sizeof(error_message), "%s '%.*s'", prefix, static_cast<int>(take), detail);
        }
        return false;
    }

    void skip_ws() noexcept {
        while (pos < len) {
            const char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos;
            else break;
        }
    }

    bool peek(char* out) noexcept {
        skip_ws();
        if (pos >= len) return false;
        *out = src[pos];
        return true;
    }

    bool consume(char expected, uint32_t node_idx, const char* err_msg) noexcept {
        skip_ws();
        if (pos >= len || src[pos] != expected) return fail(node_idx, pos, err_msg);
        ++pos;
        return true;
    }

    bool parse_hex4(uint32_t* out) noexcept {
        if (pos + 4u > len) return false;
        uint32_t v = 0u;
        for (uint32_t i = 0u; i < 4u; ++i) {
            const int d = hex_digit(src[pos + i]);
            if (d < 0) return false;
            v = (v << 4) | static_cast<uint32_t>(d);
        }
        pos += 4u;
        *out = v;
        return true;
    }

    bool decode_escape(char out[4], uint32_t* out_len) noexcept {
        if (pos >= len) return false;
        const char e = src[pos++];
        switch (e) {
            case '"':  out[0] = '"';  *out_len = 1u; return true;
            case '\\': out[0] = '\\'; *out_len = 1u; return true;
            case '/':  out[0] = '/';  *out_len = 1u; return true;
            case 'b':  out[0] = '\b'; *out_len = 1u; return true;
            case 'f':  out[0] = '\f'; *out_len = 1u; return true;
            case 'n':  out[0] = '\n'; *out_len = 1u; return true;
            case 'r':  out[0] = '\r'; *out_len = 1u; return true;
            case 't':  out[0] = '\t'; *out_len = 1u; return true;
            case 'u': {
                uint32_t u1 = 0u;
                if (!parse_hex4(&u1)) return false;
                uint32_t cp = u1;
                if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
                    if (pos + 2u > len || src[pos] != '\\' || src[pos + 1u] != 'u') return false;
                    pos += 2u;
                    uint32_t u2 = 0u;
                    if (!parse_hex4(&u2) || u2 < 0xDC00u || u2 > 0xDFFFu) return false;
                    cp = 0x10000u + (((u1 - 0xD800u) << 10) | (u2 - 0xDC00u));
                } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                    return false;
                }
                *out_len = encode_utf8(cp, out);
                return true;
            }
            default:
                return false;
        }
    }

    bool parse_small_string(uint32_t node_idx, char* out, uint32_t cap, uint32_t* out_len) noexcept {
        skip_ws();
        const uint32_t start = pos;
        if (pos >= len || src[pos] != '"') return fail(node_idx, start, "Expected JSON string");
        ++pos;
        uint32_t n = 0u;
        while (pos < len) {
            const uint8_t ch = static_cast<uint8_t>(src[pos]);
            if (ch < 0x20u) return fail(node_idx, pos, "Unescaped control character in string");
            if (ch == '"') {
                ++pos;
                if (n < cap) out[n] = '\0';
                *out_len = n;
                return true;
            }
            if (ch == '\\') {
                ++pos;
                char esc[4] = {};
                uint32_t esc_n = 0u;
                if (!decode_escape(esc, &esc_n)) return fail(node_idx, pos, "Invalid JSON string escape");
                if (n + esc_n >= cap) return fail(node_idx, start, "Identifier/key string exceeds limit");
                for (uint32_t i = 0u; i < esc_n; ++i) out[n++] = esc[i];
                continue;
            }
            if (n + 1u >= cap) return fail(node_idx, start, "Identifier/key string exceeds limit");
            out[n++] = static_cast<char>(ch);
            ++pos;
        }
        return fail(node_idx, start, "Unterminated JSON string");
    }

    bool parse_string_slice(uint32_t node_idx, StrSlice* out) noexcept {
        skip_ws();
        const uint32_t start = pos;
        if (pos >= len || src[pos] != '"') return fail(node_idx, start, "Expected JSON string");
        ++pos;
        const uint32_t pool_start = tree ? tree->pool_len : 0u;
        uint32_t run_start = pos;
        while (pos < len) {
            const uint8_t ch = static_cast<uint8_t>(src[pos]);
            if (ch < 0x20u) return fail(node_idx, pos, "Unescaped control character in string");
            if (ch == '"') {
                if (tree && pos > run_start) {
                    if (!pool_append(*tree, src + run_start, pos - run_start)) {
                        return fail(node_idx, pos, "GenUI text pool capacity exceeded");
                    }
                }
                ++pos;
                if (out) {
                    out->off = pool_start;
                    out->len = tree ? (tree->pool_len - pool_start) : 0u;
                }
                return true;
            }
            if (ch == '\\') {
                if (tree && pos > run_start) {
                    if (!pool_append(*tree, src + run_start, pos - run_start)) {
                        return fail(node_idx, pos, "GenUI text pool capacity exceeded");
                    }
                }
                ++pos;
                char esc[4] = {};
                uint32_t esc_n = 0u;
                if (!decode_escape(esc, &esc_n)) return fail(node_idx, pos, "Invalid JSON string escape");
                if (tree && !pool_append(*tree, esc, esc_n)) {
                    return fail(node_idx, pos, "GenUI text pool capacity exceeded");
                }
                run_start = pos;
                continue;
            }
            ++pos;
        }
        return fail(node_idx, start, "Unterminated JSON string");
    }

    bool parse_number(uint32_t node_idx, double* out) noexcept {
        skip_ws();
        const uint32_t start = pos;
        if (pos < len && src[pos] == '-') ++pos;
        if (pos >= len || src[pos] < '0' || src[pos] > '9') return fail(node_idx, start, "Invalid JSON number");
        if (src[pos] == '0') {
            ++pos;
            if (pos < len && src[pos] >= '0' && src[pos] <= '9') return fail(node_idx, pos, "Leading zero in JSON number");
        } else {
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        if (pos < len && src[pos] == '.') {
            ++pos;
            if (pos >= len || src[pos] < '0' || src[pos] > '9') return fail(node_idx, pos, "Invalid fractional JSON number");
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < len && (src[pos] == '+' || src[pos] == '-')) ++pos;
            if (pos >= len || src[pos] < '0' || src[pos] > '9') return fail(node_idx, pos, "Invalid exponent in JSON number");
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        const uint32_t n = pos - start;
        if (n == 0u || n >= 128u) return fail(node_idx, start, "JSON number out of range");
        char tmp[128] = {};
        std::memcpy(tmp, src + start, n);
        char* end = nullptr;
        const double v = std::strtod(tmp, &end);
        if (end != tmp + n || !std::isfinite(v)) return fail(node_idx, start, "Non-finite JSON number");
        *out = v;
        return true;
    }

    bool parse_payload_value(uint32_t node_idx, uint32_t depth, bool top_level) noexcept {
        if (depth > kMaxPayloadDepth) {
            return fail(node_idx, pos, "Payload nesting depth exceeded");
        }
        char c = '\0';
        if (!peek(&c)) return fail(node_idx, pos, "Unexpected EOF in payload");

        if (c == '"') {
            if (!tree) {
                return parse_string_slice(node_idx, nullptr);
            }
            const uint32_t tmp_start = tree->pool_len;
            StrSlice decoded = {};
            if (!parse_string_slice(node_idx, &decoded)) return false;
            // Re-encode safely with JS escaping using a temporary copy.
            char* copy = nullptr;
            if (decoded.len > 0u) {
                copy = static_cast<char*>(SDL_malloc(decoded.len));
                if (!copy) return fail(node_idx, pos, "Out of memory");
                std::memcpy(copy, tree->pool + decoded.off, decoded.len);
            }
            tree->pool_len = tmp_start;
            const bool ok = pool_append_js_quoted(*tree, copy ? copy : "", decoded.len);
            SDL_free(copy);
            return ok ? true : fail(node_idx, pos, "GenUI payload pool exceeded");
        }

        if (c == '{') {
            ++pos;
            if (tree && !pool_append_char(*tree, '{')) return fail(node_idx, pos, "GenUI payload pool exceeded");
            if (peek(&c) && c == '}') {
                ++pos;
                if (tree && !pool_append_char(*tree, '}')) return fail(node_idx, pos, "GenUI payload pool exceeded");
                return true;
            }
            bool first = true;
            for (;;) {
                skip_ws();
                const uint32_t key_off = pos;
                char key[128] = {};
                uint32_t klen = 0u;
                if (!parse_small_string(node_idx, key, sizeof(key), &klen)) return false;
                if (str_eq(key, klen, "__proto__") || str_eq(key, klen, "constructor") ||
                    str_eq(key, klen, "prototype")) {
                    return fail_fmt(node_idx, key_off, "Disallowed payload key", key, klen);
                }
                if (!consume(':', node_idx, "Expected ':' in payload object")) return false;
                if (tree) {
                    if (!first && !pool_append_char(*tree, ',')) return fail(node_idx, pos, "GenUI payload pool exceeded");
                    if (!pool_append_js_quoted(*tree, key, klen) || !pool_append_char(*tree, ':')) {
                        return fail(node_idx, pos, "GenUI payload pool exceeded");
                    }
                }
                first = false;
                if (!parse_payload_value(node_idx, depth + 1u, false)) return false;
                if (!peek(&c)) return fail(node_idx, pos, "Unterminated payload object");
                if (c == ',') {
                    ++pos;
                    continue;
                }
                if (c == '}') {
                    ++pos;
                    if (tree && !pool_append_char(*tree, '}')) return fail(node_idx, pos, "GenUI payload pool exceeded");
                    return true;
                }
                return fail(node_idx, pos, "Expected ',' or '}' in payload object");
            }
        }

        if (c == '[') {
            if (top_level) {
                return fail(node_idx, pos, "Top-level payload must be object, string, number, or boolean");
            }
            ++pos;
            if (tree && !pool_append_char(*tree, '[')) return fail(node_idx, pos, "GenUI payload pool exceeded");
            if (peek(&c) && c == ']') {
                ++pos;
                if (tree && !pool_append_char(*tree, ']')) return fail(node_idx, pos, "GenUI payload pool exceeded");
                return true;
            }
            bool first = true;
            for (;;) {
                if (tree && !first && !pool_append_char(*tree, ',')) {
                    return fail(node_idx, pos, "GenUI payload pool exceeded");
                }
                first = false;
                if (!parse_payload_value(node_idx, depth + 1u, false)) return false;
                if (!peek(&c)) return fail(node_idx, pos, "Unterminated payload array");
                if (c == ',') {
                    ++pos;
                    continue;
                }
                if (c == ']') {
                    ++pos;
                    if (tree && !pool_append_char(*tree, ']')) return fail(node_idx, pos, "GenUI payload pool exceeded");
                    return true;
                }
                return fail(node_idx, pos, "Expected ',' or ']' in payload array");
            }
        }

        if (c == 't') {
            if (pos + 4u > len || std::memcmp(src + pos, "true", 4u) != 0) {
                return fail(node_idx, pos, "Invalid literal in payload");
            }
            pos += 4u;
            if (tree && !pool_append_lit(*tree, "true")) return fail(node_idx, pos, "GenUI payload pool exceeded");
            return true;
        }
        if (c == 'f') {
            if (pos + 5u > len || std::memcmp(src + pos, "false", 5u) != 0) {
                return fail(node_idx, pos, "Invalid literal in payload");
            }
            pos += 5u;
            if (tree && !pool_append_lit(*tree, "false")) return fail(node_idx, pos, "GenUI payload pool exceeded");
            return true;
        }
        if (c == 'n') {
            if (pos + 4u > len || std::memcmp(src + pos, "null", 4u) != 0) {
                return fail(node_idx, pos, "Invalid literal in payload");
            }
            pos += 4u;
            if (tree && !pool_append_lit(*tree, "null")) return fail(node_idx, pos, "GenUI payload pool exceeded");
            return true;
        }

        double num = 0.0;
        if (!parse_number(node_idx, &num)) return false;
        if (tree) {
            char buf[48] = {};
            const int n = std::snprintf(buf, sizeof(buf), "%.17g", num);
            if (n <= 0 || !pool_append(*tree, buf, static_cast<uint32_t>(n))) {
                return fail(node_idx, pos, "GenUI payload pool exceeded");
            }
        }
        return true;
    }

    uint32_t parse_node(uint32_t depth) noexcept {
        skip_ws();
        const uint32_t node_start = pos;
        if (depth > kMaxGenUiDepth) {
            fail(node_count, node_start, "Maximum nesting depth (16) exceeded");
            return kNoneIdx;
        }
        if (node_count >= kMaxGenUiNodes) {
            fail(node_count, node_start, "Maximum node count (512) exceeded");
            return kNoneIdx;
        }
        if (pos >= len || src[pos] != '{') {
            fail(node_count, node_start, "Expected GenUI node object '{'");
            return kNoneIdx;
        }
        ++pos;

        const uint32_t node_idx = node_count++;
        if (tree) {
            if (!inlay_grow_pod(tree->nodes, tree->node_cap, tree->node_count + 1u, 32u, kMaxGenUiNodes)) {
                fail(node_idx, node_start, "Out of memory allocating GenUI node");
                return kNoneIdx;
            }
            GenUiNode& nd = tree->nodes[tree->node_count++];
            nd = {};
            nd.first_child = kNoneIdx;
            nd.next_sibling = kNoneIdx;
        }

        uint32_t seen = 0u;
        char c = '\0';
        if (peek(&c) && c == '}') {
            ++pos;
            fail(node_idx, node_start, "GenUI node missing required 'type' field");
            return kNoneIdx;
        }

        for (;;) {
            skip_ws();
            const uint32_t key_off = pos;
            char key[128] = {};
            uint32_t klen = 0u;
            if (!parse_small_string(node_idx, key, sizeof(key), &klen)) return kNoneIdx;

            if (is_disallowed_key(key, klen)) {
                fail_fmt(node_idx, key_off, "Disallowed executable key", key, klen);
                return kNoneIdx;
            }
            if (!consume(':', node_idx, "Expected ':' after node property key")) return kNoneIdx;

            if (str_eq(key, klen, "type")) {
                if (seen & KeyType) {
                    fail(node_idx, key_off, "Duplicate 'type' property");
                    return kNoneIdx;
                }
                seen |= KeyType;
                skip_ws();
                const uint32_t val_off = pos;
                char tbuf[64] = {};
                uint32_t tlen = 0u;
                if (!parse_small_string(node_idx, tbuf, sizeof(tbuf), &tlen)) return kNoneIdx;
                GenUiType ty = GenUiType::Box;
                if (!parse_component_type(tbuf, tlen, &ty)) {
                    fail_fmt(node_idx, val_off, "Unknown component type", tbuf, tlen);
                    return kNoneIdx;
                }
                if (tree) tree->nodes[node_idx].type = ty;
            } else if (str_eq(key, klen, "id")) {
                if (seen & KeyId) { fail(node_idx, key_off, "Duplicate 'id' property"); return kNoneIdx; }
                seen |= KeyId;
                StrSlice sl = {};
                if (!parse_string_slice(node_idx, &sl)) return kNoneIdx;
                if (tree) { tree->nodes[node_idx].id = sl; tree->nodes[node_idx].has_id = true; }
            } else if (str_eq(key, klen, "cls")) {
                if (seen & KeyCls) { fail(node_idx, key_off, "Duplicate 'cls' property"); return kNoneIdx; }
                seen |= KeyCls;
                StrSlice sl = {};
                if (!parse_string_slice(node_idx, &sl)) return kNoneIdx;
                if (tree) { tree->nodes[node_idx].cls = sl; tree->nodes[node_idx].has_cls = true; }
            } else if (str_eq(key, klen, "text")) {
                if (seen & KeyText) { fail(node_idx, key_off, "Duplicate 'text' property"); return kNoneIdx; }
                seen |= KeyText;
                StrSlice sl = {};
                if (!parse_string_slice(node_idx, &sl)) return kNoneIdx;
                if (tree) { tree->nodes[node_idx].text = sl; tree->nodes[node_idx].has_text = true; }
            } else if (str_eq(key, klen, "label")) {
                if (seen & KeyLabel) { fail(node_idx, key_off, "Duplicate 'label' property"); return kNoneIdx; }
                seen |= KeyLabel;
                StrSlice sl = {};
                if (!parse_string_slice(node_idx, &sl)) return kNoneIdx;
                if (tree) { tree->nodes[node_idx].label = sl; tree->nodes[node_idx].has_label = true; }
            } else if (str_eq(key, klen, "value")) {
                if (seen & KeyValue) { fail(node_idx, key_off, "Duplicate 'value' property"); return kNoneIdx; }
                seen |= KeyValue;
                char vc = '\0';
                if (!peek(&vc)) { fail(node_idx, pos, "Unexpected EOF in 'value'"); return kNoneIdx; }
                if (vc == '"') {
                    StrSlice sl = {};
                    if (!parse_string_slice(node_idx, &sl)) return kNoneIdx;
                    if (tree) {
                        tree->nodes[node_idx].value_kind = GenUiValKind::String;
                        tree->nodes[node_idx].value_str = sl;
                    }
                } else if (vc == 't' || vc == 'f') {
                    bool bv = false;
                    if (vc == 't') {
                        if (pos + 4u > len || std::memcmp(src + pos, "true", 4u) != 0) {
                            fail(node_idx, pos, "Invalid boolean in 'value'");
                            return kNoneIdx;
                        }
                        pos += 4u;
                        bv = true;
                    } else {
                        if (pos + 5u > len || std::memcmp(src + pos, "false", 5u) != 0) {
                            fail(node_idx, pos, "Invalid boolean in 'value'");
                            return kNoneIdx;
                        }
                        pos += 5u;
                        bv = false;
                    }
                    if (tree) {
                        tree->nodes[node_idx].value_kind = GenUiValKind::Bool;
                        tree->nodes[node_idx].value_bool = bv;
                    }
                } else if (vc == '-' || (vc >= '0' && vc <= '9')) {
                    double nv = 0.0;
                    if (!parse_number(node_idx, &nv)) return kNoneIdx;
                    if (tree) {
                        tree->nodes[node_idx].value_kind = GenUiValKind::Number;
                        tree->nodes[node_idx].value_num = nv;
                    }
                } else {
                    fail(node_idx, pos, "'value' must be number, string, or boolean");
                    return kNoneIdx;
                }
            } else if (str_eq(key, klen, "action")) {
                if (seen & KeyAction) { fail(node_idx, key_off, "Duplicate 'action' property"); return kNoneIdx; }
                seen |= KeyAction;
                skip_ws();
                const uint32_t act_off = pos;
                char abuf[128] = {};
                uint32_t alen = 0u;
                if (!parse_small_string(node_idx, abuf, sizeof(abuf), &alen)) return kNoneIdx;
                if (!is_safe_action_name(abuf, alen)) {
                    fail_fmt(node_idx, act_off, "Unsafe action identifier", abuf, alen);
                    return kNoneIdx;
                }
                if (tree) {
                    const uint32_t off = tree->pool_len;
                    if (!pool_append(*tree, abuf, alen)) {
                        fail(node_idx, act_off, "GenUI text pool capacity exceeded");
                        return kNoneIdx;
                    }
                    tree->nodes[node_idx].action = {off, alen};
                    tree->nodes[node_idx].has_action = true;
                }
            } else if (str_eq(key, klen, "payload")) {
                if (seen & KeyPayload) { fail(node_idx, key_off, "Duplicate 'payload' property"); return kNoneIdx; }
                seen |= KeyPayload;
                const uint32_t pstart = tree ? tree->pool_len : 0u;
                if (!parse_payload_value(node_idx, 0u, true)) return kNoneIdx;
                if (tree) {
                    tree->nodes[node_idx].payload_json = {pstart, tree->pool_len - pstart};
                    tree->nodes[node_idx].has_payload = true;
                }
            } else if (str_eq(key, klen, "children")) {
                if (seen & KeyChildren) { fail(node_idx, key_off, "Duplicate 'children' property"); return kNoneIdx; }
                seen |= KeyChildren;
                if (!consume('[', node_idx, "'children' must be a JSON array")) return kNoneIdx;
                if (peek(&c) && c == ']') {
                    ++pos;
                } else {
                    uint32_t prev_child = kNoneIdx;
                    for (;;) {
                        const uint32_t child_idx = parse_node(depth + 1u);
                        if (child_idx == kNoneIdx) return kNoneIdx;
                        if (tree) {
                            if (prev_child == kNoneIdx) tree->nodes[node_idx].first_child = child_idx;
                            else tree->nodes[prev_child].next_sibling = child_idx;
                            prev_child = child_idx;
                        }
                        if (!peek(&c)) { fail(node_idx, pos, "Unterminated 'children' array"); return kNoneIdx; }
                        if (c == ',') {
                            ++pos;
                            continue;
                        }
                        if (c == ']') {
                            ++pos;
                            break;
                        }
                        fail(node_idx, pos, "Expected ',' or ']' in 'children' array");
                        return kNoneIdx;
                    }
                }
            } else {
                fail_fmt(node_idx, key_off, "Unknown node property", key, klen);
                return kNoneIdx;
            }

            if (!peek(&c)) { fail(node_idx, pos, "Unterminated GenUI node object"); return kNoneIdx; }
            if (c == ',') {
                ++pos;
                continue;
            }
            if (c == '}') {
                ++pos;
                break;
            }
            fail(node_idx, pos, "Expected ',' or '}' in GenUI node object");
            return kNoneIdx;
        }

        if ((seen & KeyType) == 0u) {
            fail(node_idx, node_start, "GenUI node missing required 'type' field");
            return kNoneIdx;
        }
        return node_idx;
    }
};

bool run_validate(EtchStr spec_json, uint32_t schema_version, GenUiTree* opt_tree, InlayGenUiValidation* out) noexcept {
    InlayGenUiValidation res = {};
    if (schema_version != kGenUiSchemaVersion) {
        std::snprintf(res.error_message, sizeof(res.error_message),
                      "Unsupported GenUI schema_version %u (expected %u)",
                      static_cast<unsigned int>(schema_version),
                      static_cast<unsigned int>(kGenUiSchemaVersion));
        if (out) *out = res;
        return false;
    }
    if (!spec_json.chars || spec_json.length == 0u) {
        std::snprintf(res.error_message, sizeof(res.error_message), "Empty GenUI specification");
        if (out) *out = res;
        return false;
    }

    GenUiParser p = {};
    p.src = spec_json.chars;
    p.len = spec_json.length;
    p.tree = opt_tree;

    const uint32_t root = p.parse_node(1u);
    if (root != kNoneIdx && !p.failed) {
        p.skip_ws();
        if (p.pos != p.len) {
            p.fail(0u, p.pos, "Unexpected trailing content after GenUI root object");
        }
    }

    if (p.failed) {
        res.valid = false;
        res.node_count = p.node_count;
        res.error_node_index = p.error_node_index;
        res.error_offset = p.error_offset;
        std::memcpy(res.error_message, p.error_message, sizeof(res.error_message));
        if (out) *out = res;
        return false;
    }

    res.valid = true;
    res.node_count = p.node_count;
    if (out) *out = res;
    return true;
}

// ============================================================================
// Safe Deterministic Script Code Generator (ui.* + JSON-escaped literals)
// ============================================================================

struct ScriptBuf {
    char* data;
    uint32_t len;
    uint32_t cap;
    bool overflow;

    bool append(const char* bytes, uint32_t n) noexcept {
        if (n == 0u) return true;
        if (overflow || n > kMaxScriptOutBytes - len) {
            overflow = true;
            return false;
        }
        if (!inlay_grow_pod(data, cap, len + n + 1u, 1024u, kMaxScriptOutBytes + 1u)) {
            overflow = true;
            return false;
        }
        std::memcpy(data + len, bytes, n);
        len += n;
        data[len] = '\0';
        return true;
    }

    bool lit(const char* s) noexcept {
        return append(s, static_cast<uint32_t>(std::strlen(s)));
    }

    bool ch(char c) noexcept {
        return append(&c, 1u);
    }

    bool js_str(const char* bytes, uint32_t n) noexcept {
        static constexpr char kHex[] = "0123456789abcdef";
        if (!ch('"')) return false;
        uint32_t i = 0u;
        while (i < n) {
            const uint8_t b = static_cast<uint8_t>(bytes[i]);
            if (b == 0xE2u && i + 2u < n && static_cast<uint8_t>(bytes[i + 1u]) == 0x80u &&
                (static_cast<uint8_t>(bytes[i + 2u]) == 0xA8u || static_cast<uint8_t>(bytes[i + 2u]) == 0xA9u)) {
                const char* esc = (static_cast<uint8_t>(bytes[i + 2u]) == 0xA8u) ? "\\u2028" : "\\u2029";
                if (!append(esc, 6u)) return false;
                i += 3u;
                continue;
            }
            switch (b) {
                case '"':  if (!append("\\\"", 2u)) return false; break;
                case '\\': if (!append("\\\\", 2u)) return false; break;
                case '\b': if (!append("\\b", 2u)) return false; break;
                case '\f': if (!append("\\f", 2u)) return false; break;
                case '\n': if (!append("\\n", 2u)) return false; break;
                case '\r': if (!append("\\r", 2u)) return false; break;
                case '\t': if (!append("\\t", 2u)) return false; break;
                case '<':  if (!append("\\u003c", 6u)) return false; break;
                default:
                    if (b < 0x20u) {
                        const char esc[6] = {'\\', 'u', '0', '0', kHex[b >> 4], kHex[b & 15u]};
                        if (!append(esc, 6u)) return false;
                    } else {
                        if (!ch(static_cast<char>(b))) return false;
                    }
                    break;
            }
            ++i;
        }
        return ch('"');
    }

    bool js_slice(const GenUiTree& t, StrSlice sl) noexcept {
        return js_str(sl.len ? (t.pool + sl.off) : "", sl.len);
    }

    bool num(double v) noexcept {
        if (!std::isfinite(v)) return ch('0');
        char tmp[48] = {};
        const int n = std::snprintf(tmp, sizeof(tmp), "%.17g", v);
        return n > 0 && append(tmp, static_cast<uint32_t>(n));
    }
};

const char* ui_fn_name(GenUiType type) noexcept {
    switch (type) {
        case GenUiType::Box:      return "ui.box";
        case GenUiType::Row:      return "ui.row";
        case GenUiType::Col:      return "ui.col";
        case GenUiType::Card:     return "ui.card";
        case GenUiType::Text:     return "ui.text";
        case GenUiType::Badge:    return "ui.badge";
        case GenUiType::Button:   return "ui.button";
        case GenUiType::Progress: return "ui.progress";
        case GenUiType::Divider:  return "ui.divider";
        case GenUiType::Spacer:   return "ui.spacer";
        case GenUiType::Input:    return "ui.input";
        case GenUiType::Toggle:   return "ui.toggle";
        case GenUiType::Markdown: return "ui.doc";
        case GenUiType::Math:     return "ui.math";
        case GenUiType::Diagram:  return "ui.diagram";
    }
    return "ui.box";
}

bool is_container_type(GenUiType type) noexcept {
    return type == GenUiType::Box || type == GenUiType::Row || type == GenUiType::Col ||
           type == GenUiType::Card || type == GenUiType::Badge || type == GenUiType::Button;
}

bool emit_value_as_string(ScriptBuf& out, const GenUiTree& t, const GenUiNode& nd) noexcept {
    if (nd.value_kind == GenUiValKind::String) {
        return out.js_slice(t, nd.value_str);
    }
    if (nd.value_kind == GenUiValKind::Bool) {
        return out.lit(nd.value_bool ? "\"true\"" : "\"false\"");
    }
    if (nd.value_kind == GenUiValKind::Number) {
        char tmp[48] = {};
        const int n = std::snprintf(tmp, sizeof(tmp), "%.17g", nd.value_num);
        return n > 0 && out.js_str(tmp, static_cast<uint32_t>(n));
    }
    return out.lit("\"\"");
}

bool emit_node_js(ScriptBuf& out, const GenUiTree& t, uint32_t idx) noexcept {
    const GenUiNode& nd = t.nodes[idx];
    if (!out.lit(ui_fn_name(nd.type)) || !out.lit("({")) return false;

    bool need_comma = false;
    const auto sep = [&]() noexcept -> bool {
        if (need_comma && !out.ch(',')) return false;
        need_comma = true;
        return true;
    };

    if (nd.has_id) {
        if (!sep() || !out.lit("\"id\":") || !out.js_slice(t, nd.id)) return false;
    }
    if (nd.has_cls) {
        if (!sep() || !out.lit("\"cls\":") || !out.js_slice(t, nd.cls)) return false;
    }

    switch (nd.type) {
        case GenUiType::Text:
            if (nd.has_text) {
                if (!sep() || !out.lit("\"text\":") || !out.js_slice(t, nd.text)) return false;
            } else if (nd.has_label) {
                if (!sep() || !out.lit("\"text\":") || !out.js_slice(t, nd.label)) return false;
            } else if (nd.value_kind != GenUiValKind::None) {
                if (!sep() || !out.lit("\"text\":") || !emit_value_as_string(out, t, nd)) return false;
            }
            break;

        case GenUiType::Badge:
        case GenUiType::Button:
            if (nd.has_text) {
                if (!sep() || !out.lit("\"text\":") || !out.js_slice(t, nd.text)) return false;
            } else if (nd.has_label) {
                if (!sep() || !out.lit("\"text\":") || !out.js_slice(t, nd.label)) return false;
            }
            if (nd.has_action) {
                if (!sep() || !out.lit("\"onClick\":function(){action(") || !out.js_slice(t, nd.action) || !out.ch(',')) {
                    return false;
                }
                if (nd.has_payload) {
                    if (!out.append(t.pool + nd.payload_json.off, nd.payload_json.len)) return false;
                } else {
                    if (!out.lit("null")) return false;
                }
                if (!out.lit(");}")) return false;
            }
            break;

        case GenUiType::Box:
        case GenUiType::Row:
        case GenUiType::Col:
        case GenUiType::Card:
            if (nd.has_action) {
                if (!sep() || !out.lit("\"onClick\":function(){action(") || !out.js_slice(t, nd.action) || !out.ch(',')) {
                    return false;
                }
                if (nd.has_payload) {
                    if (!out.append(t.pool + nd.payload_json.off, nd.payload_json.len)) return false;
                } else {
                    if (!out.lit("null")) return false;
                }
                if (!out.lit(");}")) return false;
            }
            break;

        case GenUiType::Progress:
            if (nd.value_kind == GenUiValKind::Number) {
                if (!sep() || !out.lit("\"value\":") || !out.num(nd.value_num)) return false;
            }
            if (nd.has_label) {
                if (!sep() || !out.lit("\"label\":") || !out.js_slice(t, nd.label)) return false;
            } else if (nd.has_text) {
                if (!sep() || !out.lit("\"label\":") || !out.js_slice(t, nd.text)) return false;
            }
            break;

        case GenUiType::Spacer:
            if (nd.value_kind == GenUiValKind::Number && nd.value_num >= 0.0) {
                const double clamped = nd.value_num > 16384.0 ? 16384.0 : nd.value_num;
                if (!sep() || !out.lit("\"size\":") || !out.num(clamped)) return false;
            }
            break;

        case GenUiType::Input:
            if (nd.value_kind != GenUiValKind::None) {
                if (!sep() || !out.lit("\"value\":") || !emit_value_as_string(out, t, nd)) return false;
            }
            if (nd.has_label) {
                if (!sep() || !out.lit("\"placeholder\":") || !out.js_slice(t, nd.label)) return false;
            } else if (nd.has_text) {
                if (!sep() || !out.lit("\"placeholder\":") || !out.js_slice(t, nd.text)) return false;
            }
            if (nd.has_action) {
                if (!sep() || !out.lit("\"onChange\":function(v){action(") || !out.js_slice(t, nd.action) || !out.ch(',')) {
                    return false;
                }
                if (nd.has_payload) {
                    if (!out.lit("{\"value\":v,\"payload\":") ||
                        !out.append(t.pool + nd.payload_json.off, nd.payload_json.len) || !out.ch('}')) {
                        return false;
                    }
                } else {
                    if (!out.ch('v')) return false;
                }
                if (!out.lit(");}")) return false;
            }
            break;

        case GenUiType::Toggle:
            if (nd.has_label) {
                if (!sep() || !out.lit("\"label\":") || !out.js_slice(t, nd.label)) return false;
            } else if (nd.has_text) {
                if (!sep() || !out.lit("\"label\":") || !out.js_slice(t, nd.text)) return false;
            }
            if (nd.value_kind == GenUiValKind::Bool) {
                if (!sep() || !out.lit("\"checked\":") || !out.lit(nd.value_bool ? "true" : "false")) return false;
            } else if (nd.value_kind == GenUiValKind::Number) {
                if (!sep() || !out.lit("\"checked\":") || !out.lit(nd.value_num != 0.0 ? "true" : "false")) return false;
            }
            if (nd.has_action) {
                if (!sep() || !out.lit("\"onChange\":function(v){action(") || !out.js_slice(t, nd.action) || !out.ch(',')) {
                    return false;
                }
                if (nd.has_payload) {
                    if (!out.lit("{\"value\":v,\"payload\":") ||
                        !out.append(t.pool + nd.payload_json.off, nd.payload_json.len) || !out.ch('}')) {
                        return false;
                    }
                } else {
                    if (!out.ch('v')) return false;
                }
                if (!out.lit(");}")) return false;
            }
            break;

        case GenUiType::Markdown:
            if (nd.has_text) {
                if (!sep() || !out.lit("\"markdown\":") || !out.js_slice(t, nd.text)) return false;
            } else if (nd.value_kind == GenUiValKind::String) {
                if (!sep() || !out.lit("\"markdown\":") || !out.js_slice(t, nd.value_str)) return false;
            } else if (nd.has_label) {
                if (!sep() || !out.lit("\"markdown\":") || !out.js_slice(t, nd.label)) return false;
            }
            break;

        case GenUiType::Math:
            if (nd.has_text) {
                if (!sep() || !out.lit("\"tex\":") || !out.js_slice(t, nd.text)) return false;
            } else if (nd.value_kind == GenUiValKind::String) {
                if (!sep() || !out.lit("\"tex\":") || !out.js_slice(t, nd.value_str)) return false;
            } else if (nd.has_label) {
                if (!sep() || !out.lit("\"tex\":") || !out.js_slice(t, nd.label)) return false;
            }
            break;

        case GenUiType::Diagram:
            if (nd.has_text) {
                if (!sep() || !out.lit("\"source\":") || !out.js_slice(t, nd.text)) return false;
            } else if (nd.value_kind == GenUiValKind::String) {
                if (!sep() || !out.lit("\"source\":") || !out.js_slice(t, nd.value_str)) return false;
            } else if (nd.has_label) {
                if (!sep() || !out.lit("\"source\":") || !out.js_slice(t, nd.label)) return false;
            }
            break;

        case GenUiType::Divider:
            break;
    }

    if (!out.ch('}')) return false;

    if (is_container_type(nd.type)) {
        for (uint32_t c = nd.first_child; c != kNoneIdx; c = t.nodes[c].next_sibling) {
            if (!out.ch(',') || !emit_node_js(out, t, c)) return false;
        }
    }
    return out.ch(')');
}

} // namespace

extern "C" {

INLAY_API bool inlay_genui_validate_json(EtchStr spec_json, uint32_t schema_version, InlayGenUiValidation* out) {
    return run_validate(spec_json, schema_version, nullptr, out);
}

INLAY_API uint32_t inlay_genui_list_actions(EtchStr spec_json, char* out_buf, uint32_t cap) {
    GenUiTree tree = {};
    if (!run_validate(spec_json, kGenUiSchemaVersion, &tree, nullptr)) {
        tree_free(tree);
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }

    StrSlice unique[kMaxGenUiNodes] = {};
    uint32_t unique_count = 0u;
    for (uint32_t i = 0u; i < tree.node_count; ++i) {
        if (!tree.nodes[i].has_action) continue;
        const StrSlice act = tree.nodes[i].action;
        bool seen = false;
        for (uint32_t j = 0u; j < unique_count; ++j) {
            if (unique[j].len == act.len &&
                std::memcmp(tree.pool + unique[j].off, tree.pool + act.off, act.len) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen && unique_count < kMaxGenUiNodes) {
            unique[unique_count++] = act;
        }
    }

    uint32_t total = 0u;
    const auto put = [&](const char* s, uint32_t n) noexcept {
        if (out_buf && cap > 1u && total < cap - 1u) {
            const uint32_t room = (cap - 1u) - total;
            const uint32_t take = n < room ? n : room;
            std::memcpy(out_buf + total, s, take);
        }
        total += n;
    };

    put("[", 1u);
    for (uint32_t i = 0u; i < unique_count; ++i) {
        if (i > 0u) put(",", 1u);
        put("\"", 1u);
        // Safe action names contain only [A-Za-z0-9_.:-], so no escaping is needed.
        put(tree.pool + unique[i].off, unique[i].len);
        put("\"", 1u);
    }
    put("]", 1u);

    if (out_buf && cap > 0u) {
        out_buf[total < cap - 1u ? total : cap - 1u] = '\0';
    }
    tree_free(tree);
    return total;
}

INLAY_API bool inlay_genui_render_to_surface(InlaySurface* surface, EtchStr spec_json,
                                             uint32_t schema_version, InlayGenUiValidation* out) {
    GenUiTree tree = {};
    if (!run_validate(spec_json, schema_version, &tree, out)) {
        tree_free(tree);
        return false;
    }
    if (!surface) {
        tree_free(tree);
        if (out) {
            out->valid = false;
            std::snprintf(out->error_message, sizeof(out->error_message), "Target surface is null");
        }
        return false;
    }

    ScriptBuf sb = {};
    const bool built = sb.lit("function render(state){return ") &&
                       emit_node_js(sb, tree, 0u) &&
                       sb.lit(";}");
    tree_free(tree);
    if (!built) {
        SDL_free(sb.data);
        if (out) {
            out->valid = false;
            std::snprintf(out->error_message, sizeof(out->error_message),
                          "Generated GenUI script exceeded size limit");
        }
        return false;
    }

    const bool ok = inlay_surface_set_script(surface, {sb.data, sb.len, false});
    SDL_free(sb.data);
    if (!ok && out) {
        out->valid = false;
        const EtchStr err = inlay_surface_error(surface);
        if (err.chars && err.length > 0u) {
            const uint32_t take = err.length < sizeof(out->error_message) - 1u
                                ? err.length
                                : static_cast<uint32_t>(sizeof(out->error_message) - 1u);
            std::memcpy(out->error_message, err.chars, take);
            out->error_message[take] = '\0';
        } else {
            std::snprintf(out->error_message, sizeof(out->error_message),
                          "Surface rejected generated GenUI script");
        }
    }
    return ok;
}

} // extern "C"
