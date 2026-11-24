#include "inlay_internal.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kMaxDtcgDepth = 32u;

// ============================================================================
// DTCG JSON Exporter
// ============================================================================

struct DtcgWriter {
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

    void put_lit(const char* lit) noexcept {
        put_raw(lit, static_cast<uint32_t>(std::strlen(lit)));
    }

    void put_color_token(const char* name, EtchColor c, bool comma) noexcept {
        if (comma) put_raw(",", 1u);
        char buf[96] = {};
        const int n = std::snprintf(buf, sizeof(buf),
                                    "\"%s\":{\"$type\":\"color\",\"$value\":\"#%02x%02x%02x%02x\"}",
                                    name,
                                    static_cast<unsigned int>(c.r),
                                    static_cast<unsigned int>(c.g),
                                    static_cast<unsigned int>(c.b),
                                    static_cast<unsigned int>(c.a));
        if (n > 0) put_raw(buf, static_cast<uint32_t>(n));
    }

    void put_dim_token(const char* name, float px, bool comma) noexcept {
        if (comma) put_raw(",", 1u);
        const float clean = (std::isfinite(px) && px >= 0.f) ? px : 0.f;
        char buf[96] = {};
        const int n = std::snprintf(buf, sizeof(buf),
                                    "\"%s\":{\"$type\":\"dimension\",\"$value\":\"%.6gpx\"}",
                                    name, static_cast<double>(clean));
        if (n > 0) put_raw(buf, static_cast<uint32_t>(n));
    }

    uint32_t finish() noexcept {
        if (out && cap > 0u) {
            out[total < cap - 1u ? total : cap - 1u] = '\0';
        }
        return total;
    }
};

// ============================================================================
// DTCG Color & Dimension Parsers
// ============================================================================

inline int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool parse_hex_color(const char* s, uint32_t n, EtchColor* out) noexcept {
    while (n > 0u && (*s == ' ' || *s == '\t')) { ++s; --n; }
    while (n > 0u && (s[n - 1u] == ' ' || s[n - 1u] == '\t')) --n;
    if (n < 4u || s[0] != '#') return false;
    ++s;
    --n;
    if (n == 3u || n == 4u) {
        const int r = hex_digit(s[0]);
        const int g = hex_digit(s[1]);
        const int b = hex_digit(s[2]);
        const int a = (n == 4u) ? hex_digit(s[3]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) return false;
        *out = etch_rgba8(static_cast<uint8_t>(r * 17), static_cast<uint8_t>(g * 17),
                          static_cast<uint8_t>(b * 17), static_cast<uint8_t>(a * 17));
        return true;
    }
    if (n == 6u || n == 8u) {
        int d[8] = {0, 0, 0, 0, 0, 0, 15, 15};
        for (uint32_t i = 0u; i < n; ++i) {
            d[i] = hex_digit(s[i]);
            if (d[i] < 0) return false;
        }
        *out = etch_rgba8(static_cast<uint8_t>((d[0] << 4) | d[1]),
                          static_cast<uint8_t>((d[2] << 4) | d[3]),
                          static_cast<uint8_t>((d[4] << 4) | d[5]),
                          static_cast<uint8_t>((d[6] << 4) | d[7]));
        return true;
    }
    return false;
}

bool parse_dimension_px(const char* s, uint32_t n, float* out) noexcept {
    while (n > 0u && (*s == ' ' || *s == '\t')) { ++s; --n; }
    while (n > 0u && (s[n - 1u] == ' ' || s[n - 1u] == '\t')) --n;
    if (n == 0u || n >= 64u) return false;
    if (n >= 2u && (s[n - 2u] == 'p' || s[n - 2u] == 'P') && (s[n - 1u] == 'x' || s[n - 1u] == 'X')) {
        n -= 2u;
        while (n > 0u && (s[n - 1u] == ' ' || s[n - 1u] == '\t')) --n;
    }
    if (n == 0u) return false;
    char buf[64] = {};
    std::memcpy(buf, s, n);
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end != buf + n || !std::isfinite(v) || v < 0.0 || v > 4096.0) return false;
    *out = static_cast<float>(v);
    return true;
}

inline bool path_eq(const char* p, uint32_t len, const char* lit) noexcept {
    const size_t m = std::strlen(lit);
    return len == m && std::memcmp(p, lit, m) == 0;
}

void apply_color_token(EtchTheme& theme, const char* path, uint32_t plen, EtchColor col) noexcept {
    if (plen > 6u && std::memcmp(path, "color.", 6u) == 0) {
        path += 6u;
        plen -= 6u;
    }
    if (path_eq(path, plen, "bg.canvas"))               theme.bg_canvas = col;
    else if (path_eq(path, plen, "bg.surface"))         theme.bg_surface = col;
    else if (path_eq(path, plen, "bg.elevated"))        theme.bg_elevated = col;
    else if (path_eq(path, plen, "bg.chrome"))          theme.bg_chrome = col;
    else if (path_eq(path, plen, "bg.popup"))           theme.bg_popup = col;
    else if (path_eq(path, plen, "bg.tooltip"))         theme.bg_tooltip = col;
    else if (path_eq(path, plen, "bg.input"))           theme.bg_input = col;
    else if (path_eq(path, plen, "bg.table"))           theme.bg_table = col;
    else if (path_eq(path, plen, "bg.node_canvas"))     theme.bg_node_canvas = col;
    else if (path_eq(path, plen, "surface.control"))    theme.surface_control = col;
    else if (path_eq(path, plen, "surface.hover"))      theme.surface_hover = col;
    else if (path_eq(path, plen, "surface.active"))     theme.surface_active = col;
    else if (path_eq(path, plen, "surface.disabled"))   theme.surface_disabled = col;
    else if (path_eq(path, plen, "surface.subtle"))     theme.surface_subtle = col;
    else if (path_eq(path, plen, "surface.selected"))   theme.surface_selected = col;
    else if (path_eq(path, plen, "text.primary"))       theme.text_primary = col;
    else if (path_eq(path, plen, "text.secondary"))     theme.text_secondary = col;
    else if (path_eq(path, plen, "text.body"))          theme.text_body = col;
    else if (path_eq(path, plen, "text.code"))          theme.text_code = col;
    else if (path_eq(path, plen, "text.muted"))         theme.text_muted = col;
    else if (path_eq(path, plen, "text.disabled"))      theme.text_disabled = col;
    else if (path_eq(path, plen, "accent"))             theme.accent = col;
    else if (path_eq(path, plen, "accent_secondary") || path_eq(path, plen, "accent.secondary")) {
        theme.accent_secondary = col;
    } else if (path_eq(path, plen, "accent_primary") || path_eq(path, plen, "accent.primary")) {
        theme.accent_primary = col;
    } else if (path_eq(path, plen, "focus_ring") || path_eq(path, plen, "focus.ring")) {
        theme.focus_ring = col;
    } else if (path_eq(path, plen, "danger"))           theme.danger = col;
    else if (path_eq(path, plen, "warning"))            theme.warning = col;
    else if (path_eq(path, plen, "on_primary") || path_eq(path, plen, "on.primary")) {
        theme.on_primary = col;
    } else if (path_eq(path, plen, "on_error") || path_eq(path, plen, "on.error")) {
        theme.on_error = col;
    } else if (path_eq(path, plen, "border.subtle"))    theme.border_subtle = col;
    else if (path_eq(path, plen, "border.line"))        theme.border_line = col;
    else if (path_eq(path, plen, "border.input"))       theme.border_input = col;
    else if (path_eq(path, plen, "border.node"))        theme.border_node = col;
    else if (path_eq(path, plen, "border.control"))     theme.border_control = col;
    else if (path_eq(path, plen, "border.tooltip"))     theme.border_tooltip = col;
    else if (path_eq(path, plen, "selection"))          theme.selection = col;
}

void apply_dimension_token(EtchTheme& theme, const char* path, uint32_t plen, float px) noexcept {
    if (plen > 10u && std::memcmp(path, "dimension.", 10u) == 0) {
        path += 10u;
        plen -= 10u;
    }
    if (path_eq(path, plen, "radius.small"))        theme.radius_small_px = px;
    else if (path_eq(path, plen, "radius.control")) theme.radius_control_px = px;
    else if (path_eq(path, plen, "radius.table"))   theme.radius_table_px = px;
    else if (path_eq(path, plen, "radius.input"))   theme.radius_input_px = px;
    else if (path_eq(path, plen, "radius.panel"))   theme.radius_panel_px = px;
    else if (path_eq(path, plen, "radius.pill"))    theme.radius_pill_px = px;
}

// ============================================================================
// Strict Zero-STL JSON Parser for DTCG Documents
// ============================================================================

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

enum class DtcgTypeHint : uint8_t {
    Unknown = 0,
    Color,
    Dimension
};

struct DtcgParser {
    const char* src;
    uint32_t len;
    uint32_t pos;
    EtchTheme* theme;

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

    bool parse_string_buf(char* out, uint32_t cap, uint32_t* out_len, bool* out_truncated) noexcept {
        if (!consume('"')) return false;
        uint32_t n = 0u;
        bool trunc = false;
        while (pos < len) {
            const uint8_t ch = static_cast<uint8_t>(src[pos]);
            if (ch < 0x20u) return false;
            if (ch == '"') {
                ++pos;
                if (out && cap > 0u) out[n < cap ? n : cap - 1u] = '\0';
                if (out_len) *out_len = n;
                if (out_truncated) *out_truncated = trunc;
                return true;
            }
            if (ch == '\\') {
                ++pos;
                char esc[4] = {};
                uint32_t esc_n = 0u;
                if (!decode_escape(esc, &esc_n)) return false;
                for (uint32_t i = 0u; i < esc_n; ++i) {
                    if (out && n + 1u < cap) out[n++] = esc[i];
                    else trunc = true;
                }
                continue;
            }
            if (out && n + 1u < cap) out[n++] = static_cast<char>(ch);
            else trunc = true;
            ++pos;
        }
        return false;
    }

    bool parse_number(double* out) noexcept {
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
        if (out) *out = v;
        return true;
    }

    bool skip_value(uint32_t depth) noexcept {
        if (depth > kMaxDtcgDepth) return false;
        char c = '\0';
        if (!peek(&c)) return false;
        if (c == '"') return parse_string_buf(nullptr, 0u, nullptr, nullptr);
        if (c == '{') {
            ++pos;
            if (peek(&c) && c == '}') { ++pos; return true; }
            for (;;) {
                if (!parse_string_buf(nullptr, 0u, nullptr, nullptr) ||
                    !consume(':') || !skip_value(depth + 1u)) {
                    return false;
                }
                if (!peek(&c)) return false;
                if (c == ',') { ++pos; continue; }
                if (c == '}') { ++pos; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++pos;
            if (peek(&c) && c == ']') { ++pos; return true; }
            for (;;) {
                if (!skip_value(depth + 1u)) return false;
                if (!peek(&c)) return false;
                if (c == ',') { ++pos; continue; }
                if (c == ']') { ++pos; return true; }
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
        return parse_number(nullptr);
    }

    bool parse_dim_object(uint32_t depth, float* out_px, bool* out_valid) noexcept {
        *out_valid = false;
        if (depth > kMaxDtcgDepth || !consume('{')) return false;
        bool has_val = false;
        bool unit_px = true;
        double val = 0.0;
        char c = '\0';
        if (peek(&c) && c == '}') { ++pos; return true; }
        for (;;) {
            char k[32] = {};
            uint32_t klen = 0u;
            bool ktrunc = false;
            if (!parse_string_buf(k, sizeof(k), &klen, &ktrunc) || !consume(':')) return false;
            if (!ktrunc && path_eq(k, klen, "value")) {
                if (!parse_number(&val)) return false;
                has_val = true;
            } else if (!ktrunc && path_eq(k, klen, "unit")) {
                char u[16] = {};
                uint32_t ulen = 0u;
                bool utrunc = false;
                if (!parse_string_buf(u, sizeof(u), &ulen, &utrunc)) return false;
                unit_px = !utrunc && (path_eq(u, ulen, "px") || path_eq(u, ulen, "PX"));
            } else {
                if (!skip_value(depth + 1u)) return false;
            }
            if (!peek(&c)) return false;
            if (c == ',') { ++pos; continue; }
            if (c == '}') { ++pos; break; }
            return false;
        }
        if (has_val && unit_px && std::isfinite(val) && val >= 0.0 && val <= 4096.0) {
            *out_px = static_cast<float>(val);
            *out_valid = true;
        }
        return true;
    }

    bool parse_object(const char* path, uint32_t path_len, DtcgTypeHint inherited_type, uint32_t depth) noexcept {
        if (depth > kMaxDtcgDepth || !consume('{')) return false;
        char c = '\0';
        if (peek(&c) && c == '}') {
            ++pos;
            return true;
        }

        DtcgTypeHint local_type = inherited_type;
        bool has_str_val = false;
        char str_val[64] = {};
        uint32_t str_val_len = 0u;
        bool has_num_val = false;
        float num_val = 0.f;

        for (;;) {
            char key[64] = {};
            uint32_t klen = 0u;
            bool ktrunc = false;
            if (!parse_string_buf(key, sizeof(key), &klen, &ktrunc) || !consume(':')) return false;

            if (!ktrunc && path_eq(key, klen, "$type")) {
                char tbuf[32] = {};
                uint32_t tlen = 0u;
                bool ttrunc = false;
                if (!parse_string_buf(tbuf, sizeof(tbuf), &tlen, &ttrunc)) return false;
                if (!ttrunc && path_eq(tbuf, tlen, "color")) local_type = DtcgTypeHint::Color;
                else if (!ttrunc && path_eq(tbuf, tlen, "dimension")) local_type = DtcgTypeHint::Dimension;
            } else if (!ktrunc && path_eq(key, klen, "$value")) {
                if (!peek(&c)) return false;
                if (c == '"') {
                    bool vtrunc = false;
                    if (!parse_string_buf(str_val, sizeof(str_val), &str_val_len, &vtrunc)) return false;
                    has_str_val = !vtrunc;
                } else if (c == '{') {
                    float px = 0.f;
                    bool ok_dim = false;
                    if (!parse_dim_object(depth + 1u, &px, &ok_dim)) return false;
                    if (ok_dim) {
                        has_num_val = true;
                        num_val = px;
                    }
                } else if (c == '-' || (c >= '0' && c <= '9')) {
                    double nv = 0.0;
                    if (!parse_number(&nv)) return false;
                    if (nv >= 0.0 && nv <= 4096.0) {
                        has_num_val = true;
                        num_val = static_cast<float>(nv);
                    }
                } else {
                    if (!skip_value(depth + 1u)) return false;
                }
            } else if (!ktrunc && klen > 0u && key[0] == '$') {
                if (!skip_value(depth + 1u)) return false;
            } else {
                if (!peek(&c)) return false;
                if (c == '{' && !ktrunc) {
                    char child_path[192] = {};
                    uint32_t child_len = 0u;
                    if (path_len == 0u) {
                        if (klen < sizeof(child_path)) {
                            std::memcpy(child_path, key, klen);
                            child_len = klen;
                        }
                    } else if (path_len + 1u + klen < sizeof(child_path)) {
                        std::memcpy(child_path, path, path_len);
                        child_path[path_len] = '.';
                        std::memcpy(child_path + path_len + 1u, key, klen);
                        child_len = path_len + 1u + klen;
                    }
                    if (!parse_object(child_path, child_len, local_type, depth + 1u)) return false;
                } else {
                    if (!skip_value(depth + 1u)) return false;
                }
            }

            if (!peek(&c)) return false;
            if (c == ',') { ++pos; continue; }
            if (c == '}') { ++pos; break; }
            return false;
        }

        if (path_len > 0u) {
            if (has_str_val) {
                EtchColor col = {};
                float px = 0.f;
                if ((local_type == DtcgTypeHint::Color || local_type == DtcgTypeHint::Unknown) &&
                    parse_hex_color(str_val, str_val_len, &col)) {
                    apply_color_token(*theme, path, path_len, col);
                } else if ((local_type == DtcgTypeHint::Dimension || local_type == DtcgTypeHint::Unknown) &&
                           parse_dimension_px(str_val, str_val_len, &px)) {
                    apply_dimension_token(*theme, path, path_len, px);
                }
            } else if (has_num_val && (local_type == DtcgTypeHint::Dimension || local_type == DtcgTypeHint::Unknown)) {
                apply_dimension_token(*theme, path, path_len, num_val);
            }
        }
        return true;
    }
};

} // namespace

extern "C" {

INLAY_API uint32_t inlay_tokens_export_theme(const EtchTheme* theme, char* out_buf, uint32_t cap) {
    if (!theme) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }
    DtcgWriter w = {out_buf, cap, 0u};
    w.put_lit("{\"color\":{");
    w.put_lit("\"bg\":{");
    w.put_color_token("canvas", theme->bg_canvas, false);
    w.put_color_token("surface", theme->bg_surface, true);
    w.put_color_token("elevated", theme->bg_elevated, true);
    w.put_color_token("chrome", theme->bg_chrome, true);
    w.put_color_token("popup", theme->bg_popup, true);
    w.put_color_token("tooltip", theme->bg_tooltip, true);
    w.put_color_token("input", theme->bg_input, true);
    w.put_color_token("table", theme->bg_table, true);
    w.put_color_token("node_canvas", theme->bg_node_canvas, true);
    w.put_lit("},\"surface\":{");
    w.put_color_token("control", theme->surface_control, false);
    w.put_color_token("hover", theme->surface_hover, true);
    w.put_color_token("active", theme->surface_active, true);
    w.put_color_token("disabled", theme->surface_disabled, true);
    w.put_color_token("subtle", theme->surface_subtle, true);
    w.put_color_token("selected", theme->surface_selected, true);
    w.put_lit("},\"text\":{");
    w.put_color_token("primary", theme->text_primary, false);
    w.put_color_token("secondary", theme->text_secondary, true);
    w.put_color_token("body", theme->text_body, true);
    w.put_color_token("code", theme->text_code, true);
    w.put_color_token("muted", theme->text_muted, true);
    w.put_color_token("disabled", theme->text_disabled, true);
    w.put_lit("}");
    w.put_color_token("accent", theme->accent, true);
    w.put_color_token("accent_secondary", theme->accent_secondary, true);
    w.put_color_token("accent_primary", theme->accent_primary, true);
    w.put_color_token("focus_ring", theme->focus_ring, true);
    w.put_color_token("danger", theme->danger, true);
    w.put_color_token("warning", theme->warning, true);
    w.put_color_token("on_primary", theme->on_primary, true);
    w.put_color_token("on_error", theme->on_error, true);
    w.put_color_token("selection", theme->selection, true);
    w.put_lit(",\"border\":{");
    w.put_color_token("subtle", theme->border_subtle, false);
    w.put_color_token("line", theme->border_line, true);
    w.put_color_token("input", theme->border_input, true);
    w.put_color_token("node", theme->border_node, true);
    w.put_color_token("control", theme->border_control, true);
    w.put_color_token("tooltip", theme->border_tooltip, true);
    w.put_lit("}},\"radius\":{");
    w.put_dim_token("small", theme->radius_small_px, false);
    w.put_dim_token("control", theme->radius_control_px, true);
    w.put_dim_token("table", theme->radius_table_px, true);
    w.put_dim_token("input", theme->radius_input_px, true);
    w.put_dim_token("panel", theme->radius_panel_px, true);
    w.put_dim_token("pill", theme->radius_pill_px, true);
    w.put_lit("}}");
    return w.finish();
}

INLAY_API bool inlay_tokens_import_theme(EtchStr dtcg_json, bool dark_mode, EtchTheme* out_theme) {
    if (!out_theme || !dtcg_json.chars || dtcg_json.length == 0u) return false;
    EtchTheme result = dark_mode ? etch_theme_dark() : etch_theme_light();
    DtcgParser p = {dtcg_json.chars, dtcg_json.length, 0u, &result};
    if (!p.parse_object("", 0u, DtcgTypeHint::Unknown, 0u)) return false;
    p.skip_ws();
    if (p.pos != p.len) return false;
    result.id = ETCH_THEME_CUSTOM;
    *out_theme = result;
    return true;
}

} // extern "C"
