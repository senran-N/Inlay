#include "inlay_internal.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kMaxStreamItems = 65536u;
constexpr uint32_t kMaxFieldBytes = 32u * 1024u * 1024u;
constexpr uint32_t kMaxSkipDepth = 64u;

struct OwnedBuf {
    char* data;
    uint32_t len;
    uint32_t cap;
};

struct StreamEntry {
    uint64_t id;
    uint64_t message_id;
    InlayStreamRole role;
    InlayStreamItemKind kind;
    InlayStreamStatus status;
    float progress;
    uint64_t revision;
    uint64_t last_cursor;
    OwnedBuf title;
    OwnedBuf body;
    OwnedBuf metadata_json;
    char utf8_carry[4];
    uint8_t utf8_carry_len;
};

inline size_t entry_bytes(const StreamEntry& e) noexcept {
    return size_t(e.title.len) + size_t(e.body.len) + size_t(e.metadata_json.len) + size_t(e.utf8_carry_len);
}

void buf_free(OwnedBuf& b) noexcept {
    SDL_free(b.data);
    b = {};
}

bool buf_push_bytes(OwnedBuf& b, const char* bytes, uint32_t n) noexcept {
    if (n == 0u) return true;
    if (n > kMaxFieldBytes - b.len) return false;
    const uint32_t needed = b.len + n + 1u;
    if (!inlay_grow_pod(b.data, b.cap, needed, 64u, kMaxFieldBytes + 1u)) return false;
    std::memcpy(b.data + b.len, bytes, n);
    b.len += n;
    b.data[b.len] = '\0';
    return true;
}

bool buf_clone(OwnedBuf& dst, EtchStr src) noexcept {
    dst = {};
    if (src.length == 0u) return true;
    if (src.length > kMaxFieldBytes) return false;
    const uint32_t cap = src.length + 1u;
    char* mem = static_cast<char*>(SDL_malloc(cap));
    if (!mem) return false;
    std::memcpy(mem, src.chars, src.length);
    mem[src.length] = '\0';
    dst.data = mem;
    dst.len = src.length;
    dst.cap = cap;
    return true;
}

bool scan_utf8_stream(const char* carry, uint32_t carry_len,
                      const char* chunk, uint32_t chunk_len,
                      uint32_t* out_complete_bytes) noexcept {
    const uint32_t total = carry_len + chunk_len;
    auto byte_at = [&](uint32_t i) noexcept -> uint8_t {
        return static_cast<uint8_t>(i < carry_len ? carry[i] : chunk[i - carry_len]);
    };
    uint32_t pos = 0u;
    while (pos < total) {
        const uint8_t b0 = byte_at(pos);
        if (b0 < 0x80u) {
            ++pos;
            continue;
        }
        if (b0 >= 0xC2u && b0 <= 0xDFu) {
            if (pos + 1u >= total) break;
            const uint8_t b1 = byte_at(pos + 1u);
            if ((b1 & 0xC0u) != 0x80u) return false;
            pos += 2u;
            continue;
        }
        if (b0 >= 0xE0u && b0 <= 0xEFu) {
            if (pos + 1u >= total) break;
            const uint8_t b1 = byte_at(pos + 1u);
            if ((b1 & 0xC0u) != 0x80u) return false;
            if ((b0 == 0xE0u && b1 < 0xA0u) || (b0 == 0xEDu && b1 >= 0xA0u)) return false;
            if (pos + 2u >= total) break;
            const uint8_t b2 = byte_at(pos + 2u);
            if ((b2 & 0xC0u) != 0x80u) return false;
            pos += 3u;
            continue;
        }
        if (b0 >= 0xF0u && b0 <= 0xF4u) {
            if (pos + 1u >= total) break;
            const uint8_t b1 = byte_at(pos + 1u);
            if ((b1 & 0xC0u) != 0x80u) return false;
            if ((b0 == 0xF0u && b1 < 0x90u) || (b0 == 0xF4u && b1 >= 0x90u)) return false;
            if (pos + 2u >= total) break;
            const uint8_t b2 = byte_at(pos + 2u);
            if ((b2 & 0xC0u) != 0x80u) return false;
            if (pos + 3u >= total) break;
            const uint8_t b3 = byte_at(pos + 3u);
            if ((b3 & 0xC0u) != 0x80u) return false;
            pos += 4u;
            continue;
        }
        return false;
    }
    *out_complete_bytes = pos;
    return true;
}

bool is_valid_utf8_complete(const char* bytes, uint32_t len) noexcept {
    if (len == 0u) return true;
    uint32_t complete = 0u;
    return scan_utf8_stream(nullptr, 0u, bytes, len, &complete) && complete == len;
}

inline EtchStr buf_view(const OwnedBuf& b) noexcept {
    return b.len > 0u ? EtchStr{b.data, b.len, false} : EtchStr{"", 0u, true};
}

void entry_free(StreamEntry& e) noexcept {
    buf_free(e.title);
    buf_free(e.body);
    buf_free(e.metadata_json);
    std::memset(e.utf8_carry, 0, sizeof(e.utf8_carry));
    e.utf8_carry_len = 0u;
}

void fill_public_item(const StreamEntry& e, InlayStreamItem* out) noexcept {
    out->id = e.id;
    out->message_id = e.message_id;
    out->role = e.role;
    out->kind = e.kind;
    out->status = e.status;
    out->progress = e.progress;
    out->revision = e.revision;
    out->title = buf_view(e.title);
    out->body = buf_view(e.body);
    out->metadata_json = buf_view(e.metadata_json);
}

// ============================================================================
// JSON Exporter
// ============================================================================

struct JsonWriter {
    char* out;
    uint32_t cap;
    uint32_t total;

    void put_raw(const char* s, uint32_t n) noexcept {
        if (n == 0u) return;
        if (out && cap > 1u && total < cap - 1u) {
            const uint32_t room = (cap - 1u) - total;
            const uint32_t take = n < room ? n : room;
            std::memcpy(out + total, s, take);
        }
        total += n;
    }

    void put_char(char c) noexcept {
        if (out && cap > 1u && total < cap - 1u) {
            out[total] = c;
        }
        ++total;
    }

    void put_lit(const char* lit) noexcept {
        put_raw(lit, static_cast<uint32_t>(std::strlen(lit)));
    }

    void put_u64(uint64_t v) noexcept {
        char tmp[32] = {};
        const int n = std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(v));
        if (n > 0) put_raw(tmp, static_cast<uint32_t>(n));
    }

    void put_u32(uint32_t v) noexcept {
        char tmp[16] = {};
        const int n = std::snprintf(tmp, sizeof(tmp), "%u", static_cast<unsigned int>(v));
        if (n > 0) put_raw(tmp, static_cast<uint32_t>(n));
    }

    void put_float(float v) noexcept {
        if (!std::isfinite(v)) {
            put_char('0');
            return;
        }
        char tmp[32] = {};
        const int n = std::snprintf(tmp, sizeof(tmp), "%.7g", static_cast<double>(v));
        if (n > 0) put_raw(tmp, static_cast<uint32_t>(n));
    }

    void put_json_str(const OwnedBuf& b) noexcept {
        static constexpr char kHex[] = "0123456789abcdef";
        put_char('"');
        for (uint32_t i = 0u; i < b.len; ++i) {
            const uint8_t ch = static_cast<uint8_t>(b.data[i]);
            switch (ch) {
                case '"':  put_raw("\\\"", 2u); break;
                case '\\': put_raw("\\\\", 2u); break;
                case '\b': put_raw("\\b", 2u); break;
                case '\f': put_raw("\\f", 2u); break;
                case '\n': put_raw("\\n", 2u); break;
                case '\r': put_raw("\\r", 2u); break;
                case '\t': put_raw("\\t", 2u); break;
                default:
                    if (ch < 0x20u) {
                        const char esc[6] = {'\\', 'u', '0', '0', kHex[ch >> 4], kHex[ch & 15u]};
                        put_raw(esc, 6u);
                    } else {
                        put_char(static_cast<char>(ch));
                    }
                    break;
            }
        }
        put_char('"');
    }

    uint32_t finish() noexcept {
        if (out && cap > 0u) {
            const uint32_t nul_pos = total < cap - 1u ? total : cap - 1u;
            out[nul_pos] = '\0';
        }
        return total;
    }
};

// ============================================================================
// Minimal Zero-STL RFC 8259 JSON Scanner
// ============================================================================

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

struct JsonScanner {
    const char* src;
    uint32_t len;
    uint32_t pos;

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

    bool consume(char expected) noexcept {
        skip_ws();
        if (pos >= len || src[pos] != expected) return false;
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

    bool parse_string_into(OwnedBuf& dst) noexcept {
        if (!consume('"')) return false;
        dst.len = 0u;
        if (dst.data) dst.data[0] = '\0';
        uint32_t run_start = pos;
        while (pos < len) {
            const uint8_t ch = static_cast<uint8_t>(src[pos]);
            if (ch < 0x20u) return false;
            if (ch == '"') {
                if (pos > run_start && !buf_push_bytes(dst, src + run_start, pos - run_start)) return false;
                ++pos;
                return true;
            }
            if (ch == '\\') {
                if (pos > run_start && !buf_push_bytes(dst, src + run_start, pos - run_start)) return false;
                ++pos;
                char esc[4] = {};
                uint32_t esc_n = 0u;
                if (!decode_escape(esc, &esc_n) || !buf_push_bytes(dst, esc, esc_n)) return false;
                run_start = pos;
                continue;
            }
            ++pos;
        }
        return false;
    }

    bool parse_key(char* out, uint32_t cap, uint32_t* out_len) noexcept {
        if (!consume('"')) return false;
        uint32_t n = 0u;
        bool overflow = false;
        while (pos < len) {
            const uint8_t ch = static_cast<uint8_t>(src[pos]);
            if (ch < 0x20u) return false;
            if (ch == '"') {
                ++pos;
                if (!overflow && n < cap) out[n] = '\0';
                *out_len = overflow ? UINT32_MAX : n;
                return true;
            }
            if (ch == '\\') {
                ++pos;
                char esc[4] = {};
                uint32_t esc_n = 0u;
                if (!decode_escape(esc, &esc_n)) return false;
                for (uint32_t i = 0u; i < esc_n; ++i) {
                    if (n + 1u < cap) out[n++] = esc[i];
                    else overflow = true;
                }
                continue;
            }
            if (n + 1u < cap) out[n++] = static_cast<char>(ch);
            else overflow = true;
            ++pos;
        }
        return false;
    }

    bool parse_u64(uint64_t* out) noexcept {
        skip_ws();
        if (pos >= len || src[pos] < '0' || src[pos] > '9') return false;
        uint64_t val = 0u;
        if (src[pos] == '0') {
            ++pos;
            if (pos < len && src[pos] >= '0' && src[pos] <= '9') return false;
        } else {
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') {
                const uint64_t d = static_cast<uint64_t>(src[pos] - '0');
                if (val > (UINT64_MAX - d) / 10ull) return false;
                val = val * 10ull + d;
                ++pos;
            }
        }
        if (pos < len && (src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E')) return false;
        *out = val;
        return true;
    }

    bool parse_number_double(double* out) noexcept {
        skip_ws();
        const uint32_t start = pos;
        if (pos < len && src[pos] == '-') ++pos;
        if (pos >= len || src[pos] < '0' || src[pos] > '9') return false;
        if (src[pos] == '0') {
            ++pos;
            if (pos < len && src[pos] >= '0' && src[pos] <= '9') return false;
        } else {
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        if (pos < len && src[pos] == '.') {
            ++pos;
            if (pos >= len || src[pos] < '0' || src[pos] > '9') return false;
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < len && (src[pos] == '+' || src[pos] == '-')) ++pos;
            if (pos >= len || src[pos] < '0' || src[pos] > '9') return false;
            while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }
        const uint32_t n = pos - start;
        if (n == 0u || n >= 128u) return false;
        char tmp[128] = {};
        std::memcpy(tmp, src + start, n);
        char* end = nullptr;
        const double v = std::strtod(tmp, &end);
        if (end != tmp + n || !std::isfinite(v)) return false;
        *out = v;
        return true;
    }

    bool skip_string() noexcept {
        if (!consume('"')) return false;
        while (pos < len) {
            const uint8_t ch = static_cast<uint8_t>(src[pos]);
            if (ch < 0x20u) return false;
            if (ch == '"') {
                ++pos;
                return true;
            }
            if (ch == '\\') {
                ++pos;
                char esc[4] = {};
                uint32_t esc_n = 0u;
                if (!decode_escape(esc, &esc_n)) return false;
                continue;
            }
            ++pos;
        }
        return false;
    }

    bool skip_value(uint32_t depth) noexcept {
        if (depth > kMaxSkipDepth) return false;
        char c = '\0';
        if (!peek(&c)) return false;
        if (c == '"') return skip_string();
        if (c == '{') {
            ++pos;
            if (peek(&c) && c == '}') {
                ++pos;
                return true;
            }
            for (;;) {
                if (!skip_string() || !consume(':') || !skip_value(depth + 1u)) return false;
                if (!peek(&c)) return false;
                if (c == ',') {
                    ++pos;
                    continue;
                }
                if (c == '}') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++pos;
            if (peek(&c) && c == ']') {
                ++pos;
                return true;
            }
            for (;;) {
                if (!skip_value(depth + 1u)) return false;
                if (!peek(&c)) return false;
                if (c == ',') {
                    ++pos;
                    continue;
                }
                if (c == ']') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }
        if (c == 't') {
            if (pos + 4u > len || std::memcmp(src + pos, "true", 4u) != 0) return false;
            pos += 4u;
            return true;
        }
        if (c == 'f') {
            if (pos + 5u > len || std::memcmp(src + pos, "false", 5u) != 0) return false;
            pos += 5u;
            return true;
        }
        if (c == 'n') {
            if (pos + 4u > len || std::memcmp(src + pos, "null", 4u) != 0) return false;
            pos += 4u;
            return true;
        }
        double dummy = 0.0;
        return parse_number_double(&dummy);
    }
};

inline bool key_eq(const char* key, uint32_t len, const char* lit) noexcept {
    const size_t n = std::strlen(lit);
    return len == n && std::memcmp(key, lit, n) == 0;
}

bool parse_stream_item(JsonScanner& sc, StreamEntry& e) noexcept {
    if (!sc.consume('{')) return false;
    e = {};
    e.revision = 1u;
    e.last_cursor = UINT64_MAX;
    bool has_id = false;
    char c = '\0';
    if (sc.peek(&c) && c == '}') {
        ++sc.pos;
        return false; // Missing required id
    }
    for (;;) {
        char key[64] = {};
        uint32_t klen = 0u;
        if (!sc.parse_key(key, sizeof(key), &klen) || !sc.consume(':')) return false;
        if (key_eq(key, klen, "id")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v) || v == 0u) return false;
            e.id = v;
            has_id = true;
        } else if (key_eq(key, klen, "messageId")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v)) return false;
            e.message_id = v;
        } else if (key_eq(key, klen, "role")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v) || v > static_cast<uint64_t>(INLAY_STREAM_ROLE_SYSTEM)) return false;
            e.role = static_cast<InlayStreamRole>(v);
        } else if (key_eq(key, klen, "kind")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v) || v > static_cast<uint64_t>(INLAY_STREAM_ITEM_PROGRESS)) return false;
            e.kind = static_cast<InlayStreamItemKind>(v);
        } else if (key_eq(key, klen, "status")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v) || v > static_cast<uint64_t>(INLAY_STREAM_STATUS_CANCELLED)) return false;
            e.status = static_cast<InlayStreamStatus>(v);
        } else if (key_eq(key, klen, "progress")) {
            double v = 0.0;
            if (!sc.parse_number_double(&v)) return false;
            e.progress = static_cast<float>(v);
        } else if (key_eq(key, klen, "revision")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v)) return false;
            e.revision = v;
        } else if (key_eq(key, klen, "lastCursor")) {
            uint64_t v = 0u;
            if (!sc.parse_u64(&v)) return false;
            e.last_cursor = v;
        } else if (key_eq(key, klen, "title")) {
            if (!sc.parse_string_into(e.title)) return false;
        } else if (key_eq(key, klen, "body")) {
            if (!sc.parse_string_into(e.body)) return false;
        } else if (key_eq(key, klen, "metadata")) {
            if (!sc.parse_string_into(e.metadata_json)) return false;
        } else {
            if (!sc.skip_value(0u)) return false;
        }
        if (!sc.peek(&c)) return false;
        if (c == ',') {
            ++sc.pos;
            continue;
        }
        if (c == '}') {
            ++sc.pos;
            break;
        }
        return false;
    }
    if (!has_id || e.id == 0u) return false;
    if (!is_valid_utf8_complete(e.title.data, e.title.len) ||
        !is_valid_utf8_complete(e.body.data, e.body.len) ||
        !is_valid_utf8_complete(e.metadata_json.data, e.metadata_json.len)) {
        return false;
    }
    return true;
}

void write_stream_entry_json(JsonWriter& w, const StreamEntry& e) noexcept {
    w.put_lit("{\"id\":");
    w.put_u64(e.id);
    w.put_lit(",\"messageId\":");
    w.put_u64(e.message_id);
    w.put_lit(",\"role\":");
    w.put_u32(static_cast<uint32_t>(e.role));
    w.put_lit(",\"kind\":");
    w.put_u32(static_cast<uint32_t>(e.kind));
    w.put_lit(",\"status\":");
    w.put_u32(static_cast<uint32_t>(e.status));
    w.put_lit(",\"progress\":");
    w.put_float(e.progress);
    w.put_lit(",\"revision\":");
    w.put_u64(e.revision);
    w.put_lit(",\"lastCursor\":");
    w.put_u64(e.last_cursor);
    w.put_lit(",\"title\":");
    w.put_json_str(e.title);
    w.put_lit(",\"body\":");
    w.put_json_str(e.body);
    w.put_lit(",\"metadata\":");
    w.put_json_str(e.metadata_json);
    w.put_char('}');
}

} // namespace

struct InlayStream {
    StreamEntry* items;
    uint32_t count;
    uint32_t cap;
    uint64_t cursor;
    size_t max_bytes;
    size_t total_bytes;
};

namespace {

void clear_stream_entries(InlayStream& st) noexcept {
    for (uint32_t i = 0u; i < st.count; ++i) {
        entry_free(st.items[i]);
    }
    SDL_free(st.items);
    st.items = nullptr;
    st.count = 0u;
    st.cap = 0u;
    st.total_bytes = 0u;
}

uint32_t find_index(const InlayStream* s, uint64_t id) noexcept {
    for (uint32_t i = 0u; i < s->count; ++i) {
        if (s->items[i].id == id) return i;
    }
    return UINT32_MAX;
}

uint32_t evict_oldest_except(InlayStream* s, uint32_t keep_idx) noexcept {
    if (s->max_bytes == 0u || s->total_bytes <= s->max_bytes) return 0u;
    uint32_t write_idx = 0u;
    uint32_t evicted = 0u;
    for (uint32_t i = 0u; i < s->count; ++i) {
        if (i != keep_idx && s->total_bytes > s->max_bytes) {
            s->total_bytes -= entry_bytes(s->items[i]);
            entry_free(s->items[i]);
            ++evicted;
        } else {
            if (write_idx != i) {
                s->items[write_idx] = s->items[i];
            }
            ++write_idx;
        }
    }
    s->count = write_idx;
    return evicted;
}

} // namespace

extern "C" {

INLAY_API InlayStream* inlay_stream_create(void) {
    return static_cast<InlayStream*>(SDL_calloc(1u, sizeof(InlayStream)));
}

INLAY_API void inlay_stream_destroy(InlayStream* s) {
    if (!s) return;
    clear_stream_entries(*s);
    SDL_free(s);
}

INLAY_API void inlay_stream_set_max_bytes(InlayStream* s, size_t max_bytes) {
    if (!s) return;
    s->max_bytes = max_bytes;
    if (s->max_bytes > 0u && s->total_bytes > s->max_bytes) {
        if (evict_oldest_except(s, UINT32_MAX) > 0u) {
            ++s->cursor;
        }
    }
}

INLAY_API bool inlay_stream_upsert(InlayStream* s, const InlayStreamItem* item) {
    if (!s || !item || item->id == 0u) return false;
    if ((!item->title.chars && item->title.length > 0u) ||
        (!item->body.chars && item->body.length > 0u) ||
        (!item->metadata_json.chars && item->metadata_json.length > 0u)) {
        return false;
    }
    if (item->title.length > kMaxFieldBytes ||
        item->body.length > kMaxFieldBytes ||
        item->metadata_json.length > kMaxFieldBytes) {
        return false;
    }
    if (static_cast<uint32_t>(item->role) > static_cast<uint32_t>(INLAY_STREAM_ROLE_SYSTEM) ||
        static_cast<uint32_t>(item->kind) > static_cast<uint32_t>(INLAY_STREAM_ITEM_PROGRESS) ||
        static_cast<uint32_t>(item->status) > static_cast<uint32_t>(INLAY_STREAM_STATUS_CANCELLED)) {
        return false;
    }
    if (!is_valid_utf8_complete(item->title.chars, item->title.length) ||
        !is_valid_utf8_complete(item->body.chars, item->body.length) ||
        !is_valid_utf8_complete(item->metadata_json.chars, item->metadata_json.length)) {
        return false;
    }

    const size_t new_bytes = size_t(item->title.length) + size_t(item->body.length) + size_t(item->metadata_json.length);
    if (s->max_bytes > 0u && new_bytes > s->max_bytes) {
        return false;
    }

    const float progress = std::isfinite(item->progress) ? item->progress : 0.f;
    const uint32_t existing = find_index(s, item->id);

    uint32_t evict_count = 0u;
    if (existing == UINT32_MAX) {
        if (s->max_bytes > 0u) {
            const size_t budget_for_kept = s->max_bytes - new_bytes;
            size_t kept_bytes = s->total_bytes;
            while (evict_count < s->count && kept_bytes > budget_for_kept) {
                kept_bytes -= entry_bytes(s->items[evict_count]);
                ++evict_count;
            }
        }
        if ((s->count - evict_count) + 1u > kMaxStreamItems) return false;
    }

    OwnedBuf new_title = {};
    OwnedBuf new_body = {};
    OwnedBuf new_meta = {};
    if (!buf_clone(new_title, item->title) ||
        !buf_clone(new_body, item->body) ||
        !buf_clone(new_meta, item->metadata_json)) {
        buf_free(new_title);
        buf_free(new_body);
        buf_free(new_meta);
        return false;
    }

    if (existing != UINT32_MAX) {
        StreamEntry& e = s->items[existing];
        const size_t old_bytes = entry_bytes(e);
        buf_free(e.title);
        buf_free(e.body);
        buf_free(e.metadata_json);
        e.title = new_title;
        e.body = new_body;
        e.metadata_json = new_meta;
        std::memset(e.utf8_carry, 0, sizeof(e.utf8_carry));
        e.utf8_carry_len = 0u;
        e.message_id = item->message_id;
        e.role = item->role;
        e.kind = item->kind;
        e.status = item->status;
        e.progress = progress;
        ++e.revision;
        ++s->cursor;
        e.last_cursor = s->cursor;
        s->total_bytes = (s->total_bytes - old_bytes) + new_bytes;
        if (s->max_bytes > 0u && s->total_bytes > s->max_bytes) {
            evict_oldest_except(s, existing);
        }
        return true;
    }

    const uint32_t final_count = (s->count - evict_count) + 1u;
    if (final_count > s->cap && !inlay_grow_pod(s->items, s->cap, final_count, 16u, kMaxStreamItems)) {
        buf_free(new_title);
        buf_free(new_body);
        buf_free(new_meta);
        return false;
    }

    if (evict_count > 0u) {
        for (uint32_t i = 0u; i < evict_count; ++i) {
            s->total_bytes -= entry_bytes(s->items[i]);
            entry_free(s->items[i]);
        }
        const uint32_t remain = s->count - evict_count;
        if (remain > 0u) {
            std::memmove(&s->items[0], &s->items[evict_count], size_t(remain) * sizeof(StreamEntry));
        }
        s->count = remain;
    }

    StreamEntry fresh = {};
    fresh.id = item->id;
    fresh.message_id = item->message_id;
    fresh.role = item->role;
    fresh.kind = item->kind;
    fresh.status = item->status;
    fresh.progress = progress;
    fresh.revision = 1u;
    ++s->cursor;
    fresh.last_cursor = s->cursor;
    fresh.title = new_title;
    fresh.body = new_body;
    fresh.metadata_json = new_meta;
    s->items[s->count++] = fresh;
    s->total_bytes += new_bytes;
    return true;
}

INLAY_API bool inlay_stream_append_body(InlayStream* s, uint64_t id, EtchStr delta_utf8) {
    if (!s || id == 0u || (!delta_utf8.chars && delta_utf8.length > 0u)) return false;
    const uint32_t idx = find_index(s, id);
    if (idx == UINT32_MAX) return false;
    StreamEntry& e = s->items[idx];

    const uint32_t carry_len = static_cast<uint32_t>(e.utf8_carry_len);
    const uint32_t existing_body_total = e.body.len + carry_len;
    if (existing_body_total > kMaxFieldBytes || delta_utf8.length > kMaxFieldBytes - existing_body_total) {
        return false;
    }

    uint32_t complete = 0u;
    if (!scan_utf8_stream(e.utf8_carry, carry_len, delta_utf8.chars, delta_utf8.length, &complete)) {
        return false;
    }

    const uint32_t total_in = carry_len + delta_utf8.length;
    const uint32_t new_carry_len = total_in - complete;
    const size_t old_entry_bytes = entry_bytes(e);
    const size_t new_entry_bytes = size_t(e.title.len) + size_t(e.body.len) + size_t(complete) +
                                   size_t(new_carry_len) + size_t(e.metadata_json.len);
    if (s->max_bytes > 0u && new_entry_bytes > s->max_bytes) {
        return false;
    }

    char new_carry[4] = {};
    for (uint32_t i = 0u; i < new_carry_len; ++i) {
        const uint32_t at = complete + i;
        new_carry[i] = (at < carry_len) ? e.utf8_carry[at] : delta_utf8.chars[at - carry_len];
    }

    if (complete > 0u) {
        const uint32_t chunk_take = complete - carry_len;
        const uintptr_t p = reinterpret_cast<uintptr_t>(delta_utf8.chars);
        const uintptr_t base = reinterpret_cast<uintptr_t>(e.body.data);
        const bool overlaps = chunk_take > 0u && e.body.data != nullptr && p >= base && p < base + e.body.cap;
        const size_t rel_off = overlaps ? static_cast<size_t>(p - base) : 0u;
        const uint32_t needed = e.body.len + complete + 1u;
        if (!inlay_grow_pod(e.body.data, e.body.cap, needed, 128u, kMaxFieldBytes + 1u)) {
            return false;
        }
        const char* chunk_src = overlaps ? (e.body.data + rel_off) : delta_utf8.chars;
        if (carry_len > 0u) {
            std::memcpy(e.body.data + e.body.len, e.utf8_carry, carry_len);
        }
        if (chunk_take > 0u) {
            std::memmove(e.body.data + e.body.len + carry_len, chunk_src, chunk_take);
        }
        e.body.len += complete;
        e.body.data[e.body.len] = '\0';
    }

    std::memcpy(e.utf8_carry, new_carry, sizeof(e.utf8_carry));
    e.utf8_carry_len = static_cast<uint8_t>(new_carry_len);
    ++e.revision;
    ++s->cursor;
    e.last_cursor = s->cursor;
    s->total_bytes = (s->total_bytes - old_entry_bytes) + new_entry_bytes;

    if (s->max_bytes > 0u && s->total_bytes > s->max_bytes) {
        evict_oldest_except(s, idx);
    }
    return true;
}

INLAY_API bool inlay_stream_remove(InlayStream* s, uint64_t id) {
    if (!s || id == 0u) return false;
    const uint32_t idx = find_index(s, id);
    if (idx == UINT32_MAX) return false;
    s->total_bytes -= entry_bytes(s->items[idx]);
    entry_free(s->items[idx]);
    const uint32_t tail = s->count - idx - 1u;
    if (tail > 0u) {
        std::memmove(&s->items[idx], &s->items[idx + 1u], size_t(tail) * sizeof(StreamEntry));
    }
    --s->count;
    ++s->cursor;
    return true;
}

INLAY_API uint32_t inlay_stream_count(const InlayStream* s) {
    return s ? s->count : 0u;
}

INLAY_API bool inlay_stream_get_by_id(const InlayStream* s, uint64_t id, InlayStreamItem* out_item) {
    if (!s || id == 0u || !out_item) return false;
    const uint32_t idx = find_index(s, id);
    if (idx == UINT32_MAX) return false;
    fill_public_item(s->items[idx], out_item);
    return true;
}

INLAY_API bool inlay_stream_at(const InlayStream* s, uint32_t index, InlayStreamItem* out_item) {
    if (!s || !out_item || index >= s->count) return false;
    fill_public_item(s->items[index], out_item);
    return true;
}

INLAY_API uint64_t inlay_stream_cursor(const InlayStream* s) {
    return s ? s->cursor : 0u;
}

INLAY_API uint32_t inlay_stream_export_json(const InlayStream* s, char* out_buf, uint32_t cap) {
    if (!s) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }
    JsonWriter w = {out_buf, cap, 0u};
    w.put_lit("{\"cursor\":");
    w.put_u64(s->cursor);
    w.put_lit(",\"items\":[");
    for (uint32_t i = 0u; i < s->count; ++i) {
        if (i > 0u) w.put_char(',');
        write_stream_entry_json(w, s->items[i]);
    }
    w.put_lit("]}");
    return w.finish();
}

INLAY_API uint32_t inlay_stream_export_since_json(const InlayStream* s, uint64_t since_cursor,
                                                  char* out_buf, uint32_t cap) {
    if (!s) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }
    JsonWriter w = {out_buf, cap, 0u};
    w.put_lit("{\"cursor\":");
    w.put_u64(s->cursor);
    w.put_lit(",\"since\":");
    w.put_u64(since_cursor);
    w.put_lit(",\"items\":[");
    bool first = true;
    for (uint32_t i = 0u; i < s->count; ++i) {
        const StreamEntry& e = s->items[i];
        if (e.last_cursor <= since_cursor) continue;
        if (!first) w.put_char(',');
        first = false;
        write_stream_entry_json(w, e);
    }
    w.put_lit("]}");
    return w.finish();
}

INLAY_API bool inlay_stream_import_json(InlayStream* s, EtchStr json) {
    if (!s || !json.chars || json.length == 0u) return false;
    JsonScanner sc = {json.chars, json.length, 0u};
    if (!sc.consume('{')) return false;

    InlayStream tmp = {};
    tmp.max_bytes = s->max_bytes;
    bool has_items = false;
    bool ok = true;
    char c = '\0';

    if (sc.peek(&c) && c == '}') {
        ++sc.pos;
        ok = false; // Missing required "items"
    } else {
        for (;;) {
            char key[64] = {};
            uint32_t klen = 0u;
            if (!sc.parse_key(key, sizeof(key), &klen) || !sc.consume(':')) {
                ok = false;
                break;
            }
            if (key_eq(key, klen, "cursor")) {
                if (!sc.parse_u64(&tmp.cursor)) {
                    ok = false;
                    break;
                }
            } else if (key_eq(key, klen, "items")) {
                if (has_items || !sc.consume('[')) {
                    ok = false;
                    break;
                }
                has_items = true;
                if (sc.peek(&c) && c == ']') {
                    ++sc.pos;
                } else {
                    for (;;) {
                        StreamEntry entry = {};
                        if (!parse_stream_item(sc, entry)) {
                            entry_free(entry);
                            ok = false;
                            break;
                        }
                        const size_t item_bytes = entry_bytes(entry);
                        if (find_index(&tmp, entry.id) != UINT32_MAX ||
                            (tmp.max_bytes > 0u &&
                             (item_bytes > tmp.max_bytes || tmp.total_bytes > tmp.max_bytes - item_bytes)) ||
                            !inlay_grow_pod(tmp.items, tmp.cap, tmp.count + 1u, 16u, kMaxStreamItems)) {
                            entry_free(entry);
                            ok = false;
                            break;
                        }
                        tmp.items[tmp.count++] = entry;
                        tmp.total_bytes += item_bytes;
                        if (!sc.peek(&c)) {
                            ok = false;
                            break;
                        }
                        if (c == ',') {
                            ++sc.pos;
                            continue;
                        }
                        if (c == ']') {
                            ++sc.pos;
                            break;
                        }
                        ok = false;
                        break;
                    }
                    if (!ok) break;
                }
            } else {
                if (!sc.skip_value(0u)) {
                    ok = false;
                    break;
                }
            }

            if (!sc.peek(&c)) {
                ok = false;
                break;
            }
            if (c == ',') {
                ++sc.pos;
                continue;
            }
            if (c == '}') {
                ++sc.pos;
                break;
            }
            ok = false;
            break;
        }
    }

    if (ok) {
        sc.skip_ws();
        if (sc.pos != sc.len || !has_items) ok = false;
    }

    if (!ok) {
        clear_stream_entries(tmp);
        return false;
    }

    for (uint32_t i = 0u; i < tmp.count; ++i) {
        if (tmp.items[i].last_cursor == UINT64_MAX) {
            tmp.items[i].last_cursor = tmp.cursor;
        }
    }

    clear_stream_entries(*s);
    *s = tmp;
    return true;
}

} // extern "C"
