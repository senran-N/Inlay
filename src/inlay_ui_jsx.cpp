#include "inlay_ui_internal.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kMaxJsxOutBytes = 2u * 1024u * 1024u;
constexpr uint32_t kMaxJsxDepth = 128u;
constexpr uint32_t kMaxParenStack = 128u;
constexpr uint32_t kMaxTmplStack = 64u;

inline bool is_ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline bool is_ascii_hex(char c) noexcept {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool is_ident_start(char c) noexcept {
    return is_ascii_alpha(c) || c == '_' || c == '$' || static_cast<uint8_t>(c) >= 0x80u;
}

inline bool is_ident_part(char c) noexcept {
    return is_ident_start(c) || is_ascii_digit(c);
}

inline bool is_jsx_ident_start(char c) noexcept {
    return is_ascii_alpha(c) || c == '_' || c == '$';
}

inline bool is_jsx_name_part(char c) noexcept {
    return is_jsx_ident_start(c) || is_ascii_digit(c) || c == '-';
}

inline bool is_jsx_attr_part(char c) noexcept {
    return is_jsx_name_part(c) || c == ':';
}

inline bool is_js_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline bool kw_eq(const char* s, size_t n, const char* lit) noexcept {
    const size_t ln = std::strlen(lit);
    return n == ln && std::memcmp(s, lit, ln) == 0;
}

bool is_control_kw(const char* s, size_t n) noexcept {
    return kw_eq(s, n, "if") || kw_eq(s, n, "while") || kw_eq(s, n, "for") ||
           kw_eq(s, n, "with") || kw_eq(s, n, "catch") || kw_eq(s, n, "switch");
}

bool is_expr_prefix_kw(const char* s, size_t n) noexcept {
    return kw_eq(s, n, "return") || kw_eq(s, n, "case") || kw_eq(s, n, "throw") ||
           kw_eq(s, n, "delete") || kw_eq(s, n, "void") || kw_eq(s, n, "typeof") ||
           kw_eq(s, n, "yield") || kw_eq(s, n, "await") || kw_eq(s, n, "in") ||
           kw_eq(s, n, "instanceof") || kw_eq(s, n, "new") || kw_eq(s, n, "else") ||
           kw_eq(s, n, "do") || kw_eq(s, n, "export") || kw_eq(s, n, "default");
}

size_t scan_number_end(const char* src, size_t len, size_t p) noexcept {
    if (p < len && src[p] == '0' && p + 1u < len &&
        (src[p + 1u] == 'x' || src[p + 1u] == 'X' ||
         src[p + 1u] == 'b' || src[p + 1u] == 'B' ||
         src[p + 1u] == 'o' || src[p + 1u] == 'O')) {
        p += 2u;
        while (p < len && (is_ascii_hex(src[p]) || src[p] == '_')) ++p;
        if (p < len && src[p] == 'n') ++p;
        return p;
    }
    while (p < len && (is_ascii_digit(src[p]) || src[p] == '_')) ++p;
    if (p < len && src[p] == '.') {
        ++p;
        while (p < len && (is_ascii_digit(src[p]) || src[p] == '_')) ++p;
    }
    if (p < len && (src[p] == 'e' || src[p] == 'E')) {
        ++p;
        if (p < len && (src[p] == '+' || src[p] == '-')) ++p;
        while (p < len && (is_ascii_digit(src[p]) || src[p] == '_')) ++p;
    }
    if (p < len && src[p] == 'n') ++p;
    return p;
}

// Zero-allocation fast-path scan: returns true iff a JSX `<Tag` or `<>` candidate
// exists in expression position outside strings, comments, template text, and regexes.
bool has_jsx_candidate(const char* src, size_t len) noexcept {
    size_t p = 0;
    bool expr_expected = true;
    bool after_dot = false;
    bool prev_control_kw = false;
    uint8_t paren_stack[kMaxParenStack] = {};
    uint32_t paren_sp = 0;
    uint32_t tmpl_brace_stack[kMaxTmplStack] = {};
    uint32_t tmpl_sp = 0;
    uint32_t brace_depth = 0;

    auto scan_template_body = [&]() noexcept {
        while (p < len) {
            const char tc = src[p++];
            if (tc == '\\') {
                if (p < len) ++p;
            } else if (tc == '`') {
                expr_expected = false;
                after_dot = false;
                prev_control_kw = false;
                return;
            } else if (tc == '$' && p < len && src[p] == '{') {
                ++p;
                if (tmpl_sp < kMaxTmplStack) {
                    tmpl_brace_stack[tmpl_sp++] = brace_depth;
                }
                ++brace_depth;
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                return;
            }
        }
    };

    while (p < len) {
        const char c = src[p];
        if (is_js_ws(c)) {
            ++p;
            continue;
        }
        // Comments & regex / division
        if (c == '/') {
            if (p + 1u < len && src[p + 1u] == '/') {
                p += 2u;
                while (p < len && src[p] != '\n') ++p;
                continue;
            }
            if (p + 1u < len && src[p + 1u] == '*') {
                p += 2u;
                while (p + 1u < len && !(src[p] == '*' && src[p + 1u] == '/')) ++p;
                p = (p + 1u < len) ? (p + 2u) : len;
                continue;
            }
            if (expr_expected) {
                ++p;
                bool in_class = false;
                while (p < len) {
                    const char rc = src[p];
                    if (rc == '\n' || rc == '\r') break;
                    if (rc == '\\') {
                        ++p;
                        if (p < len && src[p] != '\n' && src[p] != '\r') ++p;
                        continue;
                    }
                    if (rc == '[') in_class = true;
                    else if (rc == ']') in_class = false;
                    else if (rc == '/' && !in_class) {
                        ++p;
                        while (p < len && is_ascii_alpha(src[p])) ++p;
                        break;
                    }
                    ++p;
                }
                expr_expected = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            ++p;
            if (p < len && src[p] == '=') ++p;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        // Quoted strings
        if (c == '\'' || c == '"') {
            ++p;
            while (p < len) {
                const char sc = src[p++];
                if (sc == '\\') {
                    if (p < len) ++p;
                } else if (sc == c) {
                    break;
                }
            }
            expr_expected = false;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        // Template literals
        if (c == '`') {
            ++p;
            scan_template_body();
            continue;
        }
        // Numbers
        if (is_ascii_digit(c) || (c == '.' && p + 1u < len && is_ascii_digit(src[p + 1u]))) {
            p = scan_number_end(src, len, p);
            expr_expected = false;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        // Identifiers & keywords
        if (is_ident_start(c)) {
            const size_t start = p++;
            while (p < len && is_ident_part(src[p])) ++p;
            const size_t n = p - start;
            if (after_dot) {
                expr_expected = false;
                prev_control_kw = false;
            } else if (is_control_kw(src + start, n)) {
                expr_expected = true;
                prev_control_kw = true;
            } else if (is_expr_prefix_kw(src + start, n)) {
                expr_expected = true;
                prev_control_kw = false;
            } else {
                expr_expected = false;
                prev_control_kw = false;
            }
            after_dot = false;
            continue;
        }
        // JSX candidate check on '<'
        if (c == '<') {
            if (expr_expected && p + 1u < len &&
                (src[p + 1u] == '>' || is_jsx_ident_start(src[p + 1u]))) {
                return true;
            }
            ++p;
            if (p < len && src[p] == '<') ++p;
            if (p < len && src[p] == '=') ++p;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        // Grouping / brackets / braces
        if (c == '(') {
            if (paren_sp < kMaxParenStack) {
                paren_stack[paren_sp++] = prev_control_kw ? 1u : 0u;
            }
            ++p;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        if (c == ')') {
            const bool is_ctrl = paren_sp > 0u ? (paren_stack[--paren_sp] != 0u) : false;
            ++p;
            expr_expected = is_ctrl;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        if (c == '[') {
            ++p;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        if (c == ']') {
            ++p;
            expr_expected = false;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        if (c == '{') {
            ++brace_depth;
            ++p;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        if (c == '}') {
            ++p;
            if (tmpl_sp > 0u && brace_depth == tmpl_brace_stack[tmpl_sp - 1u] + 1u) {
                --brace_depth;
                --tmpl_sp;
                scan_template_body();
                continue;
            }
            if (brace_depth > 0u) --brace_depth;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        // Dot / spread
        if (c == '.') {
            if (p + 2u < len && src[p + 1u] == '.' && src[p + 2u] == '.') {
                p += 3u;
                expr_expected = true;
                after_dot = false;
            } else {
                ++p;
                expr_expected = false;
                after_dot = true;
            }
            prev_control_kw = false;
            continue;
        }
        // Optional chaining vs ternary / nullish coalescing
        if (c == '?') {
            if (p + 1u < len && src[p + 1u] == '.' &&
                !(p + 2u < len && is_ascii_digit(src[p + 2u]))) {
                p += 2u;
                expr_expected = false;
                after_dot = true;
            } else {
                ++p;
                if (p < len && src[p] == '?') ++p;
                if (p < len && src[p] == '=') ++p;
                expr_expected = true;
                after_dot = false;
            }
            prev_control_kw = false;
            continue;
        }
        // ++ / -- preserve prefix/postfix expr_expected state
        if ((c == '+' || c == '-') && p + 1u < len && src[p + 1u] == c) {
            p += 2u;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        // Arrow => and multi-char operators
        if (c == '=' && p + 1u < len && src[p + 1u] == '>') {
            p += 2u;
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
            continue;
        }
        ++p;
        expr_expected = true;
        after_dot = false;
        prev_control_kw = false;
    }
    return false;
}

// ============================================================================
// Growable Output Buffer (SDL_realloc, 2 MiB Hard Cap)
// ============================================================================

struct JsxOut {
    char** io_buf;
    size_t* io_cap;
    size_t len;
    size_t initial_cap;
    bool oom;
    bool overflow;

    bool reserve(size_t extra) noexcept {
        if (oom || overflow) return false;
        if (extra > kMaxJsxOutBytes || len > kMaxJsxOutBytes - extra) {
            overflow = true;
            return false;
        }
        const size_t need = len + extra + 1u;
        if (need <= *io_cap) return true;
        size_t cap = *io_cap ? *io_cap : initial_cap;
        if (cap < 256u) cap = 256u;
        const size_t cap_limit = kMaxJsxOutBytes + 1u;
        if (cap > cap_limit) cap = cap_limit;
        while (cap < need) {
            cap = (cap > cap_limit / 2u) ? cap_limit : (cap * 2u);
        }
        void* mem = SDL_realloc(*io_buf, cap);
        if (!mem) {
            oom = true;
            return false;
        }
        *io_buf = static_cast<char*>(mem);
        *io_cap = cap;
        return true;
    }

    bool put(char c) noexcept {
        if (!reserve(1u)) return false;
        (*io_buf)[len++] = c;
        return true;
    }

    bool write(const char* s, size_t n) noexcept {
        if (!n) return true;
        if (!reserve(n)) return false;
        std::memcpy((*io_buf) + len, s, n);
        len += n;
        return true;
    }

    bool lit(const char* s) noexcept {
        return write(s, std::strlen(s));
    }

    bool newlines(size_t count) noexcept {
        if (!count) return true;
        if (!reserve(count)) return false;
        std::memset((*io_buf) + len, '\n', count);
        len += count;
        return true;
    }

    bool splice(size_t offset, const char* s, size_t n) noexcept {
        if (!n) return true;
        if (!reserve(n)) return false;
        if (offset < len) {
            std::memmove((*io_buf) + offset + n, (*io_buf) + offset, len - offset);
        }
        std::memcpy((*io_buf) + offset, s, n);
        len += n;
        return true;
    }
};

// ============================================================================
// HTML Entity Decoding & JS String Literal Escaping
// ============================================================================

void emit_js_escaped_byte(JsxOut& out, uint8_t b) noexcept {
    if (b == '"') { out.lit("\\\""); return; }
    if (b == '\\') { out.lit("\\\\"); return; }
    if (b == '\t') { out.lit("\\t"); return; }
    if (b == '\r') { out.lit("\\r"); return; }
    if (b == '\n') { out.lit("\\n"); return; }
    if (b < 0x20u) {
        static constexpr char kHex[] = "0123456789abcdef";
        char esc[6] = {'\\', 'u', '0', '0', kHex[b >> 4], kHex[b & 0x0Fu]};
        out.write(esc, 6u);
        return;
    }
    out.put(static_cast<char>(b));
}

void emit_utf8_cp_escaped(JsxOut& out, uint32_t cp) noexcept {
    if (cp < 0x80u) {
        emit_js_escaped_byte(out, static_cast<uint8_t>(cp));
    } else if (cp < 0x800u) {
        const char b[2] = {
            static_cast<char>(0xC0u | (cp >> 6)),
            static_cast<char>(0x80u | (cp & 0x3Fu))
        };
        out.write(b, 2u);
    } else if (cp < 0x10000u) {
        const char b[3] = {
            static_cast<char>(0xE0u | (cp >> 12)),
            static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)),
            static_cast<char>(0x80u | (cp & 0x3Fu))
        };
        out.write(b, 3u);
    } else if (cp <= 0x10FFFFu) {
        const char b[4] = {
            static_cast<char>(0xF0u | (cp >> 18)),
            static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)),
            static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)),
            static_cast<char>(0x80u | (cp & 0x3Fu))
        };
        out.write(b, 4u);
    }
}

bool try_decode_entity(const char* s, size_t len, size_t pos,
                       size_t* out_consumed, uint32_t* out_cp) noexcept {
    if (pos + 2u >= len || s[pos] != '&') return false;
    size_t semi = pos + 1u;
    const size_t limit = (pos + 12u < len) ? (pos + 12u) : len;
    while (semi < limit && s[semi] != ';' && s[semi] != '&' && !is_js_ws(s[semi])) ++semi;
    if (semi >= limit || s[semi] != ';') return false;
    const char* body = s + pos + 1u;
    const size_t blen = semi - (pos + 1u);
    if (blen == 0u) return false;

    uint32_t cp = 0u;
    if (kw_eq(body, blen, "amp")) cp = '&';
    else if (kw_eq(body, blen, "lt")) cp = '<';
    else if (kw_eq(body, blen, "gt")) cp = '>';
    else if (kw_eq(body, blen, "quot")) cp = '"';
    else if (kw_eq(body, blen, "apos")) cp = '\'';
    else if (kw_eq(body, blen, "nbsp")) cp = 0x00A0u;
    else if (body[0] == '#' && blen >= 2u) {
        if (body[1] == 'x' || body[1] == 'X') {
            if (blen < 3u) return false;
            for (size_t i = 2u; i < blen; ++i) {
                const int d = hex_val(body[i]);
                if (d < 0) return false;
                cp = (cp << 4) | static_cast<uint32_t>(d);
                if (cp > 0x10FFFFu) return false;
            }
        } else {
            for (size_t i = 1u; i < blen; ++i) {
                if (!is_ascii_digit(body[i])) return false;
                cp = cp * 10u + static_cast<uint32_t>(body[i] - '0');
                if (cp > 0x10FFFFu) return false;
            }
        }
        if (cp == 0u || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
    } else {
        return false;
    }
    *out_consumed = (semi - pos) + 1u;
    *out_cp = cp;
    return true;
}

void emit_jsx_text_escaped(JsxOut& out, const char* s, size_t n) noexcept {
    size_t i = 0;
    while (i < n) {
        if (s[i] == '&') {
            size_t consumed = 0;
            uint32_t cp = 0;
            if (try_decode_entity(s, n, i, &consumed, &cp)) {
                emit_utf8_cp_escaped(out, cp);
                i += consumed;
                continue;
            }
        }
        emit_js_escaped_byte(out, static_cast<uint8_t>(s[i++]));
    }
}

// ============================================================================
// Single-Pass Line-Preserving JS/JSX Transpiler
// ============================================================================

struct JsxTranspiler {
    const char* src;
    size_t len;
    const char* filename;
    size_t pos;
    uint32_t line;
    size_t line_start;
    uint32_t depth;
    JsxOut out;
    char* err_buf;
    size_t err_cap;
    bool failed;

    char peek(size_t ahead = 0u) const noexcept {
        return (pos + ahead < len) ? src[pos + ahead] : '\0';
    }

    char advance() noexcept {
        const char c = src[pos++];
        if (c == '\n') {
            ++line;
            line_start = pos;
        }
        return c;
    }

    uint32_t col() const noexcept {
        return static_cast<uint32_t>((pos - line_start) + 1u);
    }

    void check_out_error() noexcept {
        if (failed) return;
        if (out.overflow) {
            failed = true;
            if (err_buf && err_cap) {
                std::snprintf(err_buf, err_cap,
                              "JSXSyntaxError: Transpiled JSX exceeds 2 MiB limit (%s:%u:%u)",
                              filename, line, col());
            }
        } else if (out.oom) {
            failed = true;
            if (err_buf && err_cap) {
                std::snprintf(err_buf, err_cap,
                              "JSXSyntaxError: Out of memory transpiling JSX (%s:%u:%u)",
                              filename, line, col());
            }
        }
    }

    void fail_msg(uint32_t l, uint32_t c, const char* msg) noexcept {
        if (failed) return;
        failed = true;
        if (err_buf && err_cap) {
            std::snprintf(err_buf, err_cap, "JSXSyntaxError: %s (%s:%u:%u)",
                          msg, filename, l, c);
        }
    }

    void fail_tag_msg(uint32_t l, uint32_t c, const char* prefix,
                      const char* tag, size_t tag_len) noexcept {
        if (failed) return;
        failed = true;
        if (err_buf && err_cap) {
            if (tag && tag_len) {
                const int n = static_cast<int>(tag_len > 128u ? 128u : tag_len);
                std::snprintf(err_buf, err_cap, "JSXSyntaxError: %s <%.*s> (%s:%u:%u)",
                              prefix, n, tag, filename, l, c);
            } else {
                std::snprintf(err_buf, err_cap, "JSXSyntaxError: %s <> (%s:%u:%u)",
                              prefix, filename, l, c);
            }
        }
    }

    void fail_mismatch(uint32_t l, uint32_t c,
                       const char* exp, size_t exp_len,
                       const char* got, size_t got_len) noexcept {
        if (failed) return;
        failed = true;
        if (err_buf && err_cap) {
            const int en = static_cast<int>(exp_len > 128u ? 128u : exp_len);
            const int gn = static_cast<int>(got_len > 128u ? 128u : got_len);
            std::snprintf(err_buf, err_cap,
                          "JSXSyntaxError: Expected </%.*s>, got </%.*s> (%s:%u:%u)",
                          en, exp ? exp : "",
                          gn, got ? got : "",
                          filename, l, c);
        }
    }

    void skip_jsx_ws_and_comments() noexcept {
        while (pos < len && !failed) {
            const char c = peek();
            if (is_js_ws(c)) {
                if (advance() == '\n') {
                    out.put('\n');
                    check_out_error();
                }
                continue;
            }
            if (c == '/' && peek(1u) == '/') {
                advance();
                advance();
                while (pos < len && peek() != '\n') advance();
                if (pos < len && peek() == '\n') {
                    advance();
                    out.put('\n');
                    check_out_error();
                }
                continue;
            }
            if (c == '/' && peek(1u) == '*') {
                const uint32_t sl = line;
                const uint32_t sc = col();
                advance();
                advance();
                bool closed = false;
                while (pos < len) {
                    if (peek() == '*' && peek(1u) == '/') {
                        advance();
                        advance();
                        closed = true;
                        break;
                    }
                    if (advance() == '\n') {
                        out.put('\n');
                        check_out_error();
                    }
                }
                if (!closed) {
                    fail_msg(sl, sc, "Unterminated block comment in JSX");
                }
                continue;
            }
            break;
        }
    }

    bool parse_jsx_tag_name(const char** out_ptr, size_t* out_len, bool* out_has_dot) noexcept {
        if (!is_jsx_ident_start(peek())) {
            fail_msg(line, col(), "Expected JSX tag name");
            return false;
        }
        const size_t start = pos;
        bool has_dot = false;
        advance();
        while (pos < len && is_jsx_name_part(peek())) advance();
        while (pos < len && peek() == '.') {
            has_dot = true;
            advance();
            if (!is_jsx_ident_start(peek())) {
                fail_msg(line, col(), "Invalid member expression in JSX tag name");
                return false;
            }
            advance();
            while (pos < len && (is_jsx_ident_start(peek()) || is_ascii_digit(peek()))) advance();
        }
        *out_ptr = src + start;
        *out_len = pos - start;
        *out_has_dot = has_dot;
        return true;
    }

    void parse_jsx_attr_string() noexcept {
        const uint32_t sl = line;
        const uint32_t sc = col();
        const char quote = advance();
        out.put('"');
        while (pos < len && !failed) {
            const char c = peek();
            if (c == quote) {
                advance();
                out.put('"');
                check_out_error();
                return;
            }
            if (c == '\r') {
                advance();
                if (peek() == '\n') {
                    advance();
                    out.lit("\\n\"+\n\"");
                    check_out_error();
                } else {
                    out.lit("\\r");
                    check_out_error();
                }
                continue;
            }
            if (c == '\n') {
                advance();
                out.lit("\\n\"+\n\"");
                check_out_error();
                continue;
            }
            if (c == '&') {
                size_t consumed = 0;
                uint32_t cp = 0;
                if (try_decode_entity(src, len, pos, &consumed, &cp)) {
                    for (size_t k = 0; k < consumed; ++k) advance();
                    emit_utf8_cp_escaped(out, cp);
                    check_out_error();
                    continue;
                }
            }
            emit_js_escaped_byte(out, static_cast<uint8_t>(advance()));
            check_out_error();
        }
        fail_msg(sl, sc, "Unterminated JSX attribute string");
    }

    void emit_jsx_text() noexcept {
        const size_t text_start = pos;
        size_t nl_count = 0;
        while (pos < len && peek() != '<' && peek() != '{') {
            if (advance() == '\n') ++nl_count;
        }
        const size_t text_end = pos;
        if (text_end == text_start) return;

        if (nl_count == 0u) {
            out.lit(",\"");
            emit_jsx_text_escaped(out, src + text_start, text_end - text_start);
            out.put('"');
            check_out_error();
            return;
        }

        // Multiline JSX text: HTML-style whitespace trimming across wrapped lines
        // while preserving the exact newline count outside the emitted string literal.
        bool emitted_any = false;
        size_t cur = text_start;
        bool is_first = true;
        while (cur <= text_end) {
            if (cur == text_end && !is_first) break;
            const size_t lstart = cur;
            while (cur < text_end && src[cur] != '\n') ++cur;
            size_t lend = cur;
            const bool is_last = (cur == text_end);
            if (cur < text_end) ++cur;
            if (lend > lstart && src[lend - 1u] == '\r') --lend;
            size_t a = lstart;
            size_t b = lend;
            if (!is_first) {
                while (a < b && (src[a] == ' ' || src[a] == '\t')) ++a;
            }
            if (!is_last) {
                while (b > a && (src[b - 1u] == ' ' || src[b - 1u] == '\t')) --b;
            }
            is_first = false;
            if (b > a) {
                if (!emitted_any) {
                    out.lit(",\"");
                    emitted_any = true;
                } else {
                    out.put(' ');
                }
                emit_jsx_text_escaped(out, src + a, b - a);
            }
            if (is_last) break;
        }
        if (emitted_any) out.put('"');
        out.newlines(nl_count);
        check_out_error();
    }

    void parse_jsx_element() noexcept {
        if (failed) return;
        if (depth >= kMaxJsxDepth) {
            fail_msg(line, col(), "Maximum JSX nesting depth exceeded");
            return;
        }
        ++depth;
        const uint32_t open_line = line;
        const uint32_t open_col = col();
        advance(); // consume '<'

        bool is_frag = false;
        const char* tag_ptr = nullptr;
        size_t tag_len = 0;
        bool tag_has_dot = false;

        if (peek() == '>') {
            is_frag = true;
            advance();
            out.lit("__jsx(__frag,null");
            check_out_error();
        } else {
            if (!parse_jsx_tag_name(&tag_ptr, &tag_len, &tag_has_dot)) {
                --depth;
                return;
            }
            out.lit("__jsx(");
            if (!tag_has_dot && tag_len > 0u && tag_ptr[0] >= 'a' && tag_ptr[0] <= 'z') {
                out.put('"');
                out.write(tag_ptr, tag_len);
                out.put('"');
            } else {
                out.write(tag_ptr, tag_len);
            }
            out.put(',');
            check_out_error();

            bool has_spread = false;
            bool in_obj = false;
            bool first_in_obj = true;
            bool props_started = false;
            size_t props_start = 0;

            auto finalize_props = [&]() noexcept {
                if (!has_spread && !in_obj) {
                    out.lit("null");
                } else if (!has_spread && in_obj) {
                    out.put('}');
                } else {
                    if (in_obj) out.put('}');
                    out.put(')');
                }
                check_out_error();
            };

            bool self_closing = false;
            while (!failed) {
                skip_jsx_ws_and_comments();
                if (failed) break;
                if (pos >= len) {
                    fail_tag_msg(open_line, open_col, "Unclosed", tag_ptr, tag_len);
                    break;
                }
                if (peek() == '/') {
                    if (peek(1u) != '>') {
                        fail_msg(line, col(), "Expected '/>' to close self-closing JSX tag");
                        break;
                    }
                    advance();
                    advance();
                    finalize_props();
                    out.put(')');
                    check_out_error();
                    self_closing = true;
                    break;
                }
                if (peek() == '>') {
                    advance();
                    finalize_props();
                    break;
                }
                if (peek() == '{') {
                    const uint32_t sl = line;
                    const uint32_t sc = col();
                    advance(); // '{'
                    skip_jsx_ws_and_comments();
                    if (peek() != '.' || peek(1u) != '.' || peek(2u) != '.') {
                        fail_msg(sl, sc, "Expected '...' in JSX spread attribute");
                        break;
                    }
                    advance();
                    advance();
                    advance();
                    if (!props_started) {
                        props_start = out.len;
                        props_started = true;
                    }
                    if (!has_spread) {
                        if (in_obj) {
                            out.put('}');
                            in_obj = false;
                            out.splice(props_start, "Object.assign({},", 17u);
                            out.put(',');
                        } else {
                            out.lit("Object.assign({},");
                        }
                        has_spread = true;
                    } else {
                        if (in_obj) {
                            out.put('}');
                            in_obj = false;
                        }
                        out.put(',');
                    }
                    skip_jsx_ws_and_comments();
                    if (peek() == '}') {
                        fail_msg(sl, sc, "Empty JSX spread attribute");
                        break;
                    }
                    out.put('(');
                    check_out_error();
                    parse_js_block(true, sl, sc);
                    out.put(')');
                    check_out_error();
                    continue;
                }
                if (is_jsx_ident_start(peek())) {
                    const size_t astart = pos;
                    advance();
                    while (pos < len && is_jsx_attr_part(peek())) advance();
                    const size_t alen = pos - astart;
                    if (!props_started) {
                        props_start = out.len;
                        props_started = true;
                    }
                    if (!in_obj) {
                        if (has_spread) out.put(',');
                        out.put('{');
                        in_obj = true;
                        first_in_obj = true;
                    }
                    if (!first_in_obj) out.put(',');
                    first_in_obj = false;
                    out.put('"');
                    out.write(src + astart, alen);
                    out.lit("\":");
                    check_out_error();

                    skip_jsx_ws_and_comments();
                    if (peek() == '=') {
                        advance();
                        skip_jsx_ws_and_comments();
                        if (peek() == '"' || peek() == '\'') {
                            parse_jsx_attr_string();
                        } else if (peek() == '{') {
                            const uint32_t el = line;
                            const uint32_t ec = col();
                            advance(); // '{'
                            skip_jsx_ws_and_comments();
                            if (peek() == '}') {
                                fail_msg(el, ec, "Empty JSX attribute expression");
                                break;
                            }
                            out.put('(');
                            check_out_error();
                            parse_js_block(true, el, ec);
                            out.put(')');
                            check_out_error();
                        } else if (peek() == '<') {
                            parse_jsx_element();
                        } else {
                            fail_msg(line, col(), "Expected attribute value after '='");
                            break;
                        }
                    } else {
                        out.lit("true");
                        check_out_error();
                    }
                    continue;
                }
                fail_tag_msg(line, col(), "Unexpected character in JSX opening tag", tag_ptr, tag_len);
                break;
            }
            if (self_closing || failed) {
                --depth;
                return;
            }
        }

        // Children loop for open element / fragment
        while (!failed) {
            if (pos >= len) {
                fail_tag_msg(open_line, open_col, "Unclosed", tag_ptr, tag_len);
                break;
            }
            if (peek() == '<') {
                if (peek(1u) == '/') {
                    const uint32_t cl = line;
                    const uint32_t cc = col();
                    advance();
                    advance();
                    skip_jsx_ws_and_comments();
                    if (peek() == '>') {
                        advance();
                        if (!is_frag) {
                            fail_mismatch(cl, cc, tag_ptr, tag_len, "", 0u);
                            break;
                        }
                        out.put(')');
                        check_out_error();
                        break;
                    }
                    const char* cptr = nullptr;
                    size_t clen = 0;
                    bool cdot = false;
                    if (!parse_jsx_tag_name(&cptr, &clen, &cdot)) break;
                    skip_jsx_ws_and_comments();
                    if (peek() != '>') {
                        fail_msg(line, col(), "Expected '>' in JSX closing tag");
                        break;
                    }
                    advance();
                    if (is_frag) {
                        fail_mismatch(cl, cc, "", 0u, cptr, clen);
                        break;
                    }
                    if (clen != tag_len || std::memcmp(cptr, tag_ptr, tag_len) != 0) {
                        fail_mismatch(cl, cc, tag_ptr, tag_len, cptr, clen);
                        break;
                    }
                    out.put(')');
                    check_out_error();
                    break;
                }
                if (peek(1u) == '>' || is_jsx_ident_start(peek(1u))) {
                    out.put(',');
                    check_out_error();
                    parse_jsx_element();
                    continue;
                }
                fail_msg(line, col(), "Unexpected '<' in JSX child content");
                break;
            }
            if (peek() == '{') {
                const uint32_t el = line;
                const uint32_t ec = col();
                advance(); // '{'
                skip_jsx_ws_and_comments();
                if (peek() == '}') {
                    // Comment-only or empty JSX container `{/* ... */}` emits no child.
                    advance();
                    continue;
                }
                out.lit(",(");
                check_out_error();
                parse_js_block(true, el, ec);
                out.put(')');
                check_out_error();
                continue;
            }
            emit_jsx_text();
        }
        --depth;
    }

    void parse_template_literal() noexcept {
        if (depth >= kMaxJsxDepth) {
            fail_msg(line, col(), "Maximum template nesting depth exceeded");
            return;
        }
        ++depth;
        out.put(advance()); // '`'
        check_out_error();
        while (pos < len && !failed) {
            const char c = advance();
            if (c == '\\') {
                out.put('\\');
                if (pos < len) out.put(advance());
                check_out_error();
                continue;
            }
            if (c == '`') {
                out.put('`');
                check_out_error();
                --depth;
                return;
            }
            if (c == '$' && peek() == '{') {
                const uint32_t el = line;
                const uint32_t ec = col();
                advance(); // '{'
                out.lit("${");
                check_out_error();
                parse_js_block(true, el, ec);
                out.put('}');
                check_out_error();
                continue;
            }
            out.put(c);
            check_out_error();
        }
        --depth;
    }

    void parse_js_block(bool stop_on_rbrace, uint32_t open_line = 1u, uint32_t open_col = 1u) noexcept {
        bool expr_expected = true;
        bool after_dot = false;
        bool prev_control_kw = false;
        uint8_t paren_stack[kMaxParenStack] = {};
        uint32_t paren_sp = 0;
        uint32_t local_braces = 0;

        while (pos < len && !failed) {
            const char c = peek();
            if (is_js_ws(c)) {
                out.put(advance());
                check_out_error();
                continue;
            }
            // Comments, regex literals, and division
            if (c == '/') {
                if (peek(1u) == '/') {
                    const size_t start = pos;
                    advance();
                    advance();
                    while (pos < len && peek() != '\n') advance();
                    out.write(src + start, pos - start);
                    check_out_error();
                    continue;
                }
                if (peek(1u) == '*') {
                    const size_t start = pos;
                    advance();
                    advance();
                    while (pos < len && !(peek() == '*' && peek(1u) == '/')) advance();
                    if (pos < len) {
                        advance();
                        advance();
                    }
                    out.write(src + start, pos - start);
                    check_out_error();
                    continue;
                }
                if (expr_expected) {
                    const size_t start = pos;
                    advance(); // '/'
                    bool in_class = false;
                    while (pos < len) {
                        const char rc = peek();
                        if (rc == '\n' || rc == '\r') break;
                        if (rc == '\\') {
                            advance();
                            if (pos < len && peek() != '\n' && peek() != '\r') advance();
                            continue;
                        }
                        if (rc == '[') in_class = true;
                        else if (rc == ']') in_class = false;
                        else if (rc == '/' && !in_class) {
                            advance();
                            while (pos < len && is_ascii_alpha(peek())) advance();
                            break;
                        }
                        advance();
                    }
                    out.write(src + start, pos - start);
                    check_out_error();
                    expr_expected = false;
                    after_dot = false;
                    prev_control_kw = false;
                    continue;
                }
                out.put(advance());
                if (peek() == '=') out.put(advance());
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // JS string literals
            if (c == '\'' || c == '"') {
                const size_t start = pos;
                const char q = advance();
                while (pos < len) {
                    const char sc = advance();
                    if (sc == '\\') {
                        if (pos < len) advance();
                    } else if (sc == q) {
                        break;
                    }
                }
                out.write(src + start, pos - start);
                check_out_error();
                expr_expected = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // Template literals
            if (c == '`') {
                parse_template_literal();
                expr_expected = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // Numeric literals
            if (is_ascii_digit(c) || (c == '.' && is_ascii_digit(peek(1u)))) {
                const size_t start = pos;
                const size_t end = scan_number_end(src, len, pos);
                pos = end;
                out.write(src + start, end - start);
                check_out_error();
                expr_expected = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // Identifiers & keywords
            if (is_ident_start(c)) {
                const size_t start = pos;
                advance();
                while (pos < len && is_ident_part(peek())) advance();
                const size_t n = pos - start;
                out.write(src + start, n);
                check_out_error();
                if (after_dot) {
                    expr_expected = false;
                    prev_control_kw = false;
                } else if (is_control_kw(src + start, n)) {
                    expr_expected = true;
                    prev_control_kw = true;
                } else if (is_expr_prefix_kw(src + start, n)) {
                    expr_expected = true;
                    prev_control_kw = false;
                } else {
                    expr_expected = false;
                    prev_control_kw = false;
                }
                after_dot = false;
                continue;
            }
            // JSX element / fragment vs less-than operator
            if (c == '<') {
                if (expr_expected && (peek(1u) == '>' || is_jsx_ident_start(peek(1u)))) {
                    parse_jsx_element();
                    expr_expected = false;
                    after_dot = false;
                    prev_control_kw = false;
                    continue;
                }
                out.put(advance());
                if (peek() == '<') out.put(advance());
                if (peek() == '=') out.put(advance());
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // Parentheses / brackets / braces
            if (c == '(') {
                if (paren_sp < kMaxParenStack) {
                    paren_stack[paren_sp++] = prev_control_kw ? 1u : 0u;
                }
                out.put(advance());
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == ')') {
                const bool is_ctrl = paren_sp > 0u ? (paren_stack[--paren_sp] != 0u) : false;
                out.put(advance());
                check_out_error();
                expr_expected = is_ctrl;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '[') {
                out.put(advance());
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == ']') {
                out.put(advance());
                check_out_error();
                expr_expected = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '{') {
                ++local_braces;
                out.put(advance());
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '}') {
                if (stop_on_rbrace && local_braces == 0u) {
                    advance(); // consume closing '}' for caller
                    return;
                }
                if (local_braces > 0u) --local_braces;
                out.put(advance());
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // Dot vs spread
            if (c == '.') {
                if (peek(1u) == '.' && peek(2u) == '.') {
                    advance();
                    advance();
                    advance();
                    out.lit("...");
                    expr_expected = true;
                    after_dot = false;
                } else {
                    out.put(advance());
                    expr_expected = false;
                    after_dot = true;
                }
                check_out_error();
                prev_control_kw = false;
                continue;
            }
            // Optional chaining vs ternary / nullish coalescing
            if (c == '?') {
                if (peek(1u) == '.' && !is_ascii_digit(peek(2u))) {
                    advance();
                    advance();
                    out.lit("?.");
                    expr_expected = false;
                    after_dot = true;
                } else {
                    out.put(advance());
                    if (peek() == '?') out.put(advance());
                    if (peek() == '=') out.put(advance());
                    expr_expected = true;
                    after_dot = false;
                }
                check_out_error();
                prev_control_kw = false;
                continue;
            }
            // ++ / -- keep expr_expected
            if ((c == '+' || c == '-') && peek(1u) == c) {
                out.put(advance());
                out.put(advance());
                check_out_error();
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            // Arrow =>
            if (c == '=' && peek(1u) == '>') {
                advance();
                advance();
                out.lit("=>");
                check_out_error();
                expr_expected = true;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            out.put(advance());
            check_out_error();
            expr_expected = true;
            after_dot = false;
            prev_control_kw = false;
        }
        if (stop_on_rbrace && !failed) {
            fail_msg(open_line, open_col, "Unclosed JSX expression '{'");
        }
    }
};

// ============================================================================
// Zero-Dependency Line-Preserving TypeScript Type Erasure (TS / TSX -> JS / JSX)
// ============================================================================

static bool is_ts_ident_kw(const char* s, size_t n, const char* lit) noexcept {
    const size_t ln = std::strlen(lit);
    return n == ln && std::memcmp(s, lit, ln) == 0;
}

static bool has_ts_candidate(const char* src, size_t len) noexcept {
    for (size_t i = 0u; i < len; ++i) {
        const char c = src[i];
        if (c == ':' || c == '<') return true;
        if (c == '!' && i + 1u < len && (src[i + 1u] == '.' || src[i + 1u] == '[' || src[i + 1u] == '(' || src[i + 1u] == ':')) return true;
        if (c == '?' && i + 1u < len && (src[i + 1u] == ',' || src[i + 1u] == ')' || src[i + 1u] == ':')) return true;
        if (is_ident_start(c) && (i == 0u || !is_ident_part(src[i - 1u]))) {
            size_t j = i + 1u;
            while (j < len && is_ident_part(src[j])) ++j;
            const size_t n = j - i;
            if (is_ts_ident_kw(src + i, n, "interface") ||
                is_ts_ident_kw(src + i, n, "type") ||
                is_ts_ident_kw(src + i, n, "declare") ||
                is_ts_ident_kw(src + i, n, "implements") ||
                is_ts_ident_kw(src + i, n, "readonly") ||
                is_ts_ident_kw(src + i, n, "satisfies") ||
                is_ts_ident_kw(src + i, n, "as") ||
                is_ts_ident_kw(src + i, n, "public") ||
                is_ts_ident_kw(src + i, n, "private") ||
                is_ts_ident_kw(src + i, n, "protected") ||
                is_ts_ident_kw(src + i, n, "override") ||
                is_ts_ident_kw(src + i, n, "abstract")) {
                return true;
            }
            i = j - 1u;
        }
    }
    return false;
}

struct TsStripper {
    const char* src;
    size_t len;
    char* out; // Same length as src; erased non-newline chars become ' '.
    bool modified;
    uint32_t depth;

    void erase_span(size_t a, size_t b) noexcept {
        if (a >= b || a >= len) return;
        if (b > len) b = len;
        for (size_t i = a; i < b; ++i) {
            if (out[i] != '\n' && out[i] != '\r') {
                if (out[i] != ' ') {
                    out[i] = ' ';
                    modified = true;
                }
            }
        }
    }

    size_t skip_ws_comments(size_t p, bool* saw_nl = nullptr) const noexcept {
        if (saw_nl) *saw_nl = false;
        while (p < len) {
            const char c = src[p];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++p; continue; }
            if (c == '\n') { if (saw_nl) *saw_nl = true; ++p; continue; }
            if (c == '/' && p + 1u < len && src[p + 1u] == '/') {
                p += 2u;
                while (p < len && src[p] != '\n') ++p;
                continue;
            }
            if (c == '/' && p + 1u < len && src[p + 1u] == '*') {
                p += 2u;
                while (p + 1u < len && !(src[p] == '*' && src[p + 1u] == '/')) {
                    if (src[p] == '\n' && saw_nl) *saw_nl = true;
                    ++p;
                }
                p = (p + 1u < len) ? (p + 2u) : len;
                continue;
            }
            break;
        }
        return p;
    }

    size_t skip_string(size_t p) const noexcept {
        if (p >= len) return p;
        const char q = src[p++];
        while (p < len) {
            if (src[p] == '\\' && p + 1u < len) { p += 2u; continue; }
            if (src[p] == q) return p + 1u;
            ++p;
        }
        return len;
    }

    size_t skip_regex(size_t p) const noexcept {
        if (p >= len || src[p] != '/') return p;
        ++p;
        bool in_class = false;
        while (p < len) {
            const char c = src[p++];
            if (c == '\\' && p < len) { ++p; continue; }
            if (c == '[') { in_class = true; continue; }
            if (c == ']') { in_class = false; continue; }
            if (c == '\n') return p;
            if (c == '/' && !in_class) {
                while (p < len && is_ascii_alpha(src[p])) ++p;
                return p;
            }
        }
        return len;
    }

    size_t skip_template(size_t p) const noexcept {
        if (p >= len || src[p] != '`') return p;
        ++p;
        while (p < len) {
            const char c = src[p++];
            if (c == '\\' && p < len) { ++p; continue; }
            if (c == '`') return p;
            if (c == '$' && p < len && src[p] == '{') {
                p = skip_balanced(p, '{', '}');
            }
        }
        return len;
    }

    size_t skip_balanced(size_t p, char open_ch, char close_ch) const noexcept {
        if (p >= len || src[p] != open_ch) return p;
        int d = 0;
        while (p < len) {
            const char c = src[p];
            if (c == '\'' || c == '"') { p = skip_string(p); continue; }
            if (c == '`') { p = skip_template(p); continue; }
            if (c == '/' && p + 1u < len && (src[p + 1u] == '/' || src[p + 1u] == '*')) {
                p = skip_ws_comments(p);
                continue;
            }
            if (c == open_ch) ++d;
            else if (c == close_ch) {
                --d;
                if (d == 0) return p + 1u;
            }
            ++p;
        }
        return len;
    }

    // Consume a balanced `<...>` generic parameter or argument list starting at `<`.
    // Returns end offset (after `>`) or SIZE_MAX if invalid / unbalanced.
    size_t skip_angle_group(size_t p, bool strict_call_args) const noexcept {
        if (p >= len || src[p] != '<') return SIZE_MAX;
        int angle = 0, paren = 0, brace = 0, bracket = 0;
        size_t i = p;
        while (i < len) {
            const char c = src[i];
            if (c == '\'' || c == '"') { i = skip_string(i); continue; }
            if (c == '`') { i = skip_template(i); continue; }
            if (c == '/' && i + 1u < len && (src[i + 1u] == '/' || src[i + 1u] == '*')) {
                i = skip_ws_comments(i);
                continue;
            }
            if (c == '(') { ++paren; ++i; continue; }
            if (c == ')') { if (paren == 0) return SIZE_MAX; --paren; ++i; continue; }
            if (c == '{') { ++brace; ++i; continue; }
            if (c == '}') { if (brace == 0) return SIZE_MAX; --brace; ++i; continue; }
            if (c == '[') { ++bracket; ++i; continue; }
            if (c == ']') { if (bracket == 0) return SIZE_MAX; --bracket; ++i; continue; }
            if (c == '<') { ++angle; ++i; continue; }
            if (c == '>') {
                if (i > p && src[i - 1u] == '=' && (paren > 0 || brace > 0 || angle > 0)) {
                    ++i;
                    continue; // `=>` inside function type argument
                }
                --angle;
                ++i;
                if (angle == 0 && paren == 0 && brace == 0 && bracket == 0) return i;
                continue;
            }
            if (strict_call_args && paren == 0 && brace == 0 && bracket == 0) {
                if (c == ';' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') return SIZE_MAX;
                if ((c == '&' || c == '|') && i + 1u < len && src[i + 1u] == c) return SIZE_MAX;
                if ((c == '=' || c == '!') && i + 1u < len && src[i + 1u] == '=') return SIZE_MAX;
            }
            ++i;
        }
        return SIZE_MAX;
    }

    // Consume a TS type expression starting at `p`.
    size_t skip_type_expr(size_t p, bool stop_at_arrow, bool stop_at_eq, bool stop_at_lbrace_after_primary) const noexcept {
        if (depth > 96u) return p;
        size_t cur = skip_ws_comments(p);
        bool have_primary = false;
        while (cur < len) {
            bool saw_nl = false;
            const size_t next = skip_ws_comments(cur, &saw_nl);
            if (next >= len) break;
            const char c = src[next];

            if (!have_primary) {
                if (c == '|' || c == '&') { cur = next + 1u; continue; }
                if (c == '(') {
                    const size_t after_p = skip_balanced(next, '(', ')');
                    cur = after_p;
                    const size_t a2 = skip_ws_comments(cur);
                    if (a2 + 1u < len && src[a2] == '=' && src[a2 + 1u] == '>') {
                        cur = skip_type_expr(a2 + 2u, stop_at_arrow, stop_at_eq, stop_at_lbrace_after_primary);
                    }
                    have_primary = true;
                    continue;
                }
                if (c == '[') {
                    cur = skip_balanced(next, '[', ']');
                    have_primary = true;
                    continue;
                }
                if (c == '{') {
                    cur = skip_balanced(next, '{', '}');
                    have_primary = true;
                    continue;
                }
                if (c == '<') {
                    // Generic function type `<T>(x: T) => T`
                    const size_t ag = skip_angle_group(next, false);
                    if (ag != SIZE_MAX) {
                        const size_t ap = skip_ws_comments(ag);
                        if (ap < len && src[ap] == '(') {
                            const size_t after_p = skip_balanced(ap, '(', ')');
                            const size_t ar = skip_ws_comments(after_p);
                            if (ar + 1u < len && src[ar] == '=' && src[ar + 1u] == '>') {
                                cur = skip_type_expr(ar + 2u, stop_at_arrow, stop_at_eq, stop_at_lbrace_after_primary);
                                have_primary = true;
                                continue;
                            }
                        }
                    }
                    break;
                }
                if (c == '\'' || c == '"') {
                    cur = skip_string(next);
                    have_primary = true;
                    continue;
                }
                if (c == '`') {
                    cur = skip_template(next);
                    have_primary = true;
                    continue;
                }
                if (is_ascii_digit(c) || (c == '-' && next + 1u < len && is_ascii_digit(src[next + 1u]))) {
                    cur = scan_number_end(src, len, c == '-' ? next + 1u : next);
                    have_primary = true;
                    continue;
                }
                if (is_ident_start(c)) {
                    size_t id_end = next + 1u;
                    while (id_end < len && is_ident_part(src[id_end])) ++id_end;
                    const size_t id_len = id_end - next;
                    if (is_ts_ident_kw(src + next, id_len, "readonly") ||
                        is_ts_ident_kw(src + next, id_len, "keyof") ||
                        is_ts_ident_kw(src + next, id_len, "typeof") ||
                        is_ts_ident_kw(src + next, id_len, "unique") ||
                        is_ts_ident_kw(src + next, id_len, "infer") ||
                        is_ts_ident_kw(src + next, id_len, "asserts") ||
                        is_ts_ident_kw(src + next, id_len, "new")) {
                        // Prefix type operator: keep expecting primary unless followed by non-type token
                        const size_t peek_after = skip_ws_comments(id_end);
                        if (peek_after < len && (is_ident_start(src[peek_after]) || src[peek_after] == '[' ||
                                                 src[peek_after] == '{' || src[peek_after] == '(' || src[peek_after] == '<')) {
                            cur = id_end;
                            continue;
                        }
                    }
                    // Qualified name A.B.C
                    cur = id_end;
                    while (true) {
                        const size_t dot = skip_ws_comments(cur);
                        if (dot < len && src[dot] == '.' && dot + 1u < len && src[dot + 1u] != '.') {
                            const size_t sub = skip_ws_comments(dot + 1u);
                            if (sub < len && is_ident_start(src[sub])) {
                                cur = sub + 1u;
                                while (cur < len && is_ident_part(src[cur])) ++cur;
                                continue;
                            }
                        }
                        break;
                    }
                    // Optional generic args `<...>`
                    const size_t ag_pos = skip_ws_comments(cur);
                    if (ag_pos < len && src[ag_pos] == '<') {
                        const size_t ag_end = skip_angle_group(ag_pos, false);
                        if (ag_end != SIZE_MAX) cur = ag_end;
                    }
                    // Type predicate `x is Foo`
                    bool is_nl = false;
                    const size_t maybe_is = skip_ws_comments(cur, &is_nl);
                    if (!is_nl && maybe_is + 2u <= len && src[maybe_is] == 'i' && src[maybe_is + 1u] == 's' &&
                        (maybe_is + 2u == len || !is_ident_part(src[maybe_is + 2u]))) {
                        cur = skip_type_expr(maybe_is + 2u, stop_at_arrow, stop_at_eq, stop_at_lbrace_after_primary);
                    }
                    have_primary = true;
                    continue;
                }
                break;
            } else {
                // Already parsed a primary type; check for postfix/binary continuations.
                if (c == '[' && !saw_nl) {
                    cur = skip_balanced(next, '[', ']');
                    continue;
                }
                if (c == '|' && (next + 1u >= len || src[next + 1u] != '|')) {
                    cur = next + 1u;
                    have_primary = false;
                    continue;
                }
                if (c == '&' && (next + 1u >= len || src[next + 1u] != '&')) {
                    cur = next + 1u;
                    have_primary = false;
                    continue;
                }
                if (c == '=' && next + 1u < len && src[next + 1u] == '>') {
                    if (stop_at_arrow) break;
                    cur = next + 2u;
                    have_primary = false;
                    continue;
                }
                if (c == '=' && stop_at_eq) break;
                if (c == '{' && stop_at_lbrace_after_primary) break;
                if (!saw_nl && is_ident_start(c)) {
                    size_t kw_end = next + 1u;
                    while (kw_end < len && is_ident_part(src[kw_end])) ++kw_end;
                    if (is_ts_ident_kw(src + next, kw_end - next, "extends")) {
                        // Conditional type `A extends B ? C : D`
                        cur = skip_type_expr(kw_end, stop_at_arrow, stop_at_eq, stop_at_lbrace_after_primary);
                        const size_t q = skip_ws_comments(cur);
                        if (q < len && src[q] == '?') {
                            cur = skip_type_expr(q + 1u, stop_at_arrow, stop_at_eq, stop_at_lbrace_after_primary);
                            const size_t col = skip_ws_comments(cur);
                            if (col < len && src[col] == ':') {
                                cur = skip_type_expr(col + 1u, stop_at_arrow, stop_at_eq, stop_at_lbrace_after_primary);
                            }
                        }
                        continue;
                    }
                }
                break;
            }
        }
        return cur;
    }

    // Check if `(` at `p` starts an arrow function parameter list `( ... ) =>` or `( ... ): Ret =>`.
    bool is_arrow_params(size_t p) const noexcept {
        if (p >= len || src[p] != '(') return false;
        const size_t close = skip_balanced(p, '(', ')');
        if (close > len || close == p) return false;
        const size_t after = skip_ws_comments(close);
        if (after + 1u < len && src[after] == '=' && src[after + 1u] == '>') return true;
        if (after < len && src[after] == ':') {
            const size_t tend = skip_type_expr(after + 1u, true, true, true);
            const size_t a2 = skip_ws_comments(tend);
            if (a2 + 1u < len && src[a2] == '=' && src[a2 + 1u] == '>') return true;
        }
        return false;
    }

    // Strip parameter type annotations, optional `?`, and constructor modifiers inside `(...)`.
    size_t strip_param_list(size_t p) noexcept {
        if (p >= len || src[p] != '(') return p;
        size_t i = p + 1u;
        while (i < len) {
            i = skip_ws_comments(i);
            if (i >= len) return len;
            if (src[i] == ')') return i + 1u;
            if (src[i] == ',') { ++i; continue; }
            // Rest `...`
            if (src[i] == '.' && i + 2u < len && src[i + 1u] == '.' && src[i + 2u] == '.') {
                i += 3u;
                i = skip_ws_comments(i);
            }
            // Parameter property modifiers (`public`/`private`/`protected`/`readonly`/`override`)
            while (i < len && is_ident_start(src[i])) {
                size_t k = i + 1u;
                while (k < len && is_ident_part(src[k])) ++k;
                const size_t kn = k - i;
                if (is_ts_ident_kw(src + i, kn, "public") || is_ts_ident_kw(src + i, kn, "private") ||
                    is_ts_ident_kw(src + i, kn, "protected") || is_ts_ident_kw(src + i, kn, "readonly") ||
                    is_ts_ident_kw(src + i, kn, "override")) {
                    const size_t nxt = skip_ws_comments(k);
                    if (nxt < len && (is_ident_start(src[nxt]) || src[nxt] == '{' || src[nxt] == '[')) {
                        erase_span(i, k);
                        i = nxt;
                        continue;
                    }
                }
                break;
            }
            // Parameter pattern: ident, `{...}`, or `[...]`
            if (i < len && (src[i] == '{' || src[i] == '[')) {
                const char oc = src[i];
                const char cc = (oc == '{') ? '}' : ']';
                const size_t end_pat = skip_balanced(i, oc, cc);
                // Strip any TS expressions inside default values within destructuring.
                if (end_pat > i + 2u && depth < 64u) {
                    ++depth;
                    strip_range(i + 1u, end_pat - 1u, false, false);
                    --depth;
                }
                i = end_pat;
            } else if (i < len && is_ident_start(src[i])) {
                // Handle TS `this: Foo` fake parameter
                size_t id_end = i + 1u;
                while (id_end < len && is_ident_part(src[id_end])) ++id_end;
                if (is_ts_ident_kw(src + i, id_end - i, "this")) {
                    const size_t col = skip_ws_comments(id_end);
                    if (col < len && src[col] == ':') {
                        size_t tend = skip_type_expr(col + 1u, false, true, true);
                        const size_t comma = skip_ws_comments(tend);
                        if (comma < len && src[comma] == ',') tend = comma + 1u;
                        erase_span(i, tend);
                        i = tend;
                        continue;
                    }
                }
                i = id_end;
            } else {
                // Unexpected token: bail to matching `)`
                return skip_balanced(p, '(', ')');
            }
            i = skip_ws_comments(i);
            // Optional `?`
            if (i < len && src[i] == '?') {
                erase_span(i, i + 1u);
                i = skip_ws_comments(i + 1u);
            }
            // Type annotation `: Type`
            if (i < len && src[i] == ':') {
                const size_t tend = skip_type_expr(i + 1u, false, true, true);
                erase_span(i, tend);
                i = skip_ws_comments(tend);
            }
            // Default value `= expr`
            if (i < len && src[i] == '=') {
                const size_t val_start = i + 1u;
                int pd = 0, bd = 0, kd = 0;
                size_t j = val_start;
                while (j < len) {
                    const char ch = src[j];
                    if (ch == '\'' || ch == '"') { j = skip_string(j); continue; }
                    if (ch == '`') { j = skip_template(j); continue; }
                    if (ch == '/' && j + 1u < len && (src[j + 1u] == '/' || src[j + 1u] == '*')) {
                        j = skip_ws_comments(j);
                        continue;
                    }
                    if (ch == '(') ++pd;
                    else if (ch == ')') { if (pd == 0 && bd == 0 && kd == 0) break; if (pd > 0) --pd; }
                    else if (ch == '{') ++bd;
                    else if (ch == '}') { if (bd > 0) --bd; }
                    else if (ch == '[') ++kd;
                    else if (ch == ']') { if (kd > 0) --kd; }
                    else if (ch == ',' && pd == 0 && bd == 0 && kd == 0) break;
                    ++j;
                }
                if (j > val_start && depth < 64u) {
                    ++depth;
                    strip_range(val_start, j, false, false);
                    --depth;
                }
                i = j;
            }
            i = skip_ws_comments(i);
            if (i < len && src[i] == ',') ++i;
            else if (i < len && src[i] == ')') return i + 1u;
            else break;
        }
        return skip_balanced(p, '(', ')');
    }

    // Pass through a JSX element without corrupting JSX text/attributes, stripping TS inside `{...}`.
    size_t pass_jsx_element(size_t p, size_t end) noexcept {
        if (p >= end || src[p] != '<' || depth > 64u) return p;
        ++depth;
        size_t i = p + 1u;
        // Fragment `<>`
        bool is_frag = false;
        if (i < end && src[i] == '>') {
            is_frag = true;
            ++i;
        } else {
            // Tag name (possibly dotted `Ctx.Provider`)
            if (i >= end || !is_jsx_ident_start(src[i])) { --depth; return p; }
            while (i < end && (is_jsx_name_part(src[i]) || src[i] == '.' || src[i] == ':')) ++i;
            // Optional generic type arguments on JSX component `<List<Item> ...>`
            if (i < end && src[i] == '<') {
                const size_t ag = skip_angle_group(i, false);
                if (ag != SIZE_MAX && ag <= end) {
                    erase_span(i, ag);
                    i = ag;
                }
            }
            // Tag attributes
            bool self_closing = false;
            while (i < end) {
                i = skip_ws_comments(i);
                if (i >= end) break;
                if (src[i] == '/' && i + 1u < end && src[i + 1u] == '>') {
                    self_closing = true;
                    i += 2u;
                    break;
                }
                if (src[i] == '>') {
                    ++i;
                    break;
                }
                if (src[i] == '{') {
                    const size_t close = skip_balanced(i, '{', '}');
                    if (close > i + 2u && close <= end) {
                        strip_range(i + 1u, close - 1u, false, false);
                    }
                    i = close < end ? close : end;
                    continue;
                }
                if (src[i] == '\'' || src[i] == '"') {
                    i = skip_string(i);
                    continue;
                }
                ++i;
            }
            if (self_closing) { --depth; return i; }
        }
        // Children until matching `</...>`
        (void)is_frag;
        while (i < end) {
            if (src[i] == '<') {
                if (i + 1u < end && src[i + 1u] == '/') {
                    // Closing tag
                    i += 2u;
                    while (i < end && src[i] != '>') ++i;
                    if (i < end) ++i;
                    --depth;
                    return i;
                }
                if (i + 1u < end && (src[i + 1u] == '>' || is_jsx_ident_start(src[i + 1u]))) {
                    const size_t nxt = pass_jsx_element(i, end);
                    i = (nxt > i) ? nxt : (i + 1u);
                    continue;
                }
            }
            if (src[i] == '{') {
                const size_t close = skip_balanced(i, '{', '}');
                if (close > i + 2u && close <= end) {
                    strip_range(i + 1u, close - 1u, false, false);
                }
                i = close < end ? close : end;
                continue;
            }
            ++i;
        }
        --depth;
        return i;
    }

    // Strip a `const`/`let`/`var` binding pattern + optional `: Type` / `!: Type` before `=` / `,` / `;` / `in` / `of`.
    size_t strip_var_binding(size_t p, size_t end) noexcept {
        size_t i = skip_ws_comments(p);
        if (i >= end) return i;
        if (src[i] == '{' || src[i] == '[') {
            const char oc = src[i];
            const char cc = (oc == '{') ? '}' : ']';
            const size_t bend = skip_balanced(i, oc, cc);
            if (bend > i + 2u && bend <= end && depth < 64u) {
                ++depth;
                strip_range(i + 1u, bend - 1u, false, false);
                --depth;
            }
            i = bend < end ? bend : end;
        } else if (is_ident_start(src[i])) {
            while (i < end && is_ident_part(src[i])) ++i;
        } else {
            return p;
        }
        const size_t after = skip_ws_comments(i);
        if (after < end && src[after] == '!' && after + 1u < end && src[after + 1u] == ':') {
            const size_t tend = skip_type_expr(after + 2u, false, true, true);
            erase_span(after, tend);
            return tend;
        }
        if (after < end && src[after] == ':') {
            const size_t tend = skip_type_expr(after + 1u, false, true, true);
            erase_span(after, tend);
            return tend;
        }
        return i;
    }

    void strip_range(size_t start, size_t end, bool in_class_body, bool in_import_braces) noexcept {
        size_t i = start;
        bool expr_expected = true;
        bool stmt_start = true;
        bool after_dot = false;
        bool prev_control_kw = false;
        bool prev_was_arrow = false;
        int paren_depth = 0;
        int brace_depth = 0;
        int bracket_depth = 0;
        bool active_var_decl = false;
        int var_decl_brace = 0;
        int var_decl_paren = 0;
        uint8_t paren_ctrl[kMaxParenStack] = {};
        uint32_t paren_sp = 0u;
        uint8_t brace_is_expr[kMaxParenStack] = {};
        uint32_t brace_sp = 0u;

        while (i < end) {
            const char c = src[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }
            if (c == '\n') {
                if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0 && !expr_expected) {
                    stmt_start = true;
                    active_var_decl = false;
                }
                ++i;
                continue;
            }
            if (c == '/' && i + 1u < end && src[i + 1u] == '/') {
                i += 2u;
                while (i < end && src[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1u < end && src[i + 1u] == '*') {
                i += 2u;
                while (i + 1u < end && !(src[i] == '*' && src[i + 1u] == '/')) ++i;
                i = (i + 1u < end) ? (i + 2u) : end;
                continue;
            }
            if (c == '\'' || c == '"') {
                i = skip_string(i);
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '`') {
                // Scan template literal, stripping TS inside `${...}` expressions
                ++i;
                while (i < end) {
                    const char tc = src[i++];
                    if (tc == '\\' && i < end) { ++i; continue; }
                    if (tc == '`') break;
                    if (tc == '$' && i < end && src[i] == '{') {
                        const size_t close = skip_balanced(i, '{', '}');
                        if (close > i + 2u && close <= end && depth < 64u) {
                            ++depth;
                            strip_range(i + 1u, close - 1u, false, false);
                            --depth;
                        }
                        i = close < end ? close : end;
                    }
                }
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '/' && expr_expected) {
                i = skip_regex(i);
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }

            // JSX or generic arrow `<T extends ...>(...) =>` / `<T,>(...) =>` in expression position
            if (c == '<' && expr_expected) {
                // First test TSX generic arrow `<T extends ...>(...) =>` or `<T,>(...) =>`
                const size_t ag = skip_angle_group(i, false);
                bool handled_generic_arrow = false;
                if (ag != SIZE_MAX && ag <= end) {
                    bool has_ts_marker = false;
                    for (size_t k = i + 1u; k + 1u < ag; ++k) {
                        if (src[k] == ',' || src[k] == '=') { has_ts_marker = true; break; }
                        if (k + 7u < ag && std::memcmp(src + k, "extends", 7u) == 0 &&
                            !is_ident_part(src[k - 1u]) && !is_ident_part(src[k + 7u])) {
                            has_ts_marker = true;
                            break;
                        }
                    }
                    const size_t after_ag = skip_ws_comments(ag);
                    if (has_ts_marker && after_ag < end && is_arrow_params(after_ag)) {
                        erase_span(i, ag);
                        i = after_ag;
                        handled_generic_arrow = true;
                    }
                }
                if (handled_generic_arrow) continue;

                // Otherwise pass through JSX element if it looks like `<>` or `<Tag`
                if (i + 1u < end && (src[i + 1u] == '>' || is_jsx_ident_start(src[i + 1u]))) {
                    const size_t jend = pass_jsx_element(i, end);
                    if (jend > i) {
                        i = jend;
                        expr_expected = false;
                        stmt_start = false;
                        after_dot = false;
                        prev_control_kw = false;
                        continue;
                    }
                }
            }

            // Arrow parameter list `(a: number, b?: string): boolean =>`
            if (c == '(' && !prev_control_kw && !after_dot && is_arrow_params(i)) {
                const size_t after_p = strip_param_list(i);
                i = after_p;
                const size_t col = skip_ws_comments(i);
                if (col < end && src[col] == ':') {
                    const size_t tend = skip_type_expr(col + 1u, true, true, true);
                    erase_span(col, tend);
                    i = tend;
                }
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }

            // Inside `import { type Foo, bar, type Baz as B }`
            if (in_import_braces && is_ident_start(c)) {
                size_t id_end = i + 1u;
                while (id_end < end && is_ident_part(src[id_end])) ++id_end;
                if (is_ts_ident_kw(src + i, id_end - i, "type")) {
                    const size_t nxt = skip_ws_comments(id_end);
                    if (nxt < end && is_ident_start(src[nxt])) {
                        // Consume `type Foo [as Bar] [,]`
                        size_t cur = nxt + 1u;
                        while (cur < end && is_ident_part(src[cur])) ++cur;
                        const size_t a2 = skip_ws_comments(cur);
                        if (a2 + 2u <= end && src[a2] == 'a' && src[a2 + 1u] == 's' &&
                            (a2 + 2u == end || !is_ident_part(src[a2 + 2u]))) {
                            size_t a3 = skip_ws_comments(a2 + 2u);
                            while (a3 < end && is_ident_part(src[a3])) ++a3;
                            cur = a3;
                        }
                        const size_t comma = skip_ws_comments(cur);
                        if (comma < end && src[comma] == ',') {
                            erase_span(i, comma + 1u);
                            i = comma + 1u;
                        } else {
                            // Erase preceding comma if any so `{ bar, type Foo }` becomes `{ bar }`
                            size_t back = i;
                            while (back > start && is_js_ws(out[back - 1u])) --back;
                            if (back > start && out[back - 1u] == ',') erase_span(back - 1u, back);
                            erase_span(i, cur);
                            i = cur;
                        }
                        continue;
                    }
                }
                i = id_end;
                continue;
            }

            // Postfix non-null assertion `expr!`
            if (c == '!' && !expr_expected && (i + 1u >= end || src[i + 1u] != '=')) {
                const size_t nxt = skip_ws_comments(i + 1u);
                if (nxt >= end || src[nxt] == '.' || src[nxt] == '[' || src[nxt] == '(' ||
                    src[nxt] == ',' || src[nxt] == ';' || src[nxt] == ')' || src[nxt] == ']' ||
                    src[nxt] == '}' || src[nxt] == '?' || src[nxt] == ':') {
                    erase_span(i, i + 1u);
                    ++i;
                    continue;
                }
            }

            // Identifiers & Keywords
            if (is_ident_start(c)) {
                const size_t id_start = i;
                while (i < end && is_ident_part(src[i])) ++i;
                const size_t id_len = i - id_start;
                const char* id = src + id_start;

                if (after_dot) {
                    // Check generic method call `obj.foo<T>(...)`
                    const size_t ag_p = skip_ws_comments(i);
                    if (ag_p < end && src[ag_p] == '<') {
                        const size_t ag_end = skip_angle_group(ag_p, true);
                        if (ag_end != SIZE_MAX && ag_end <= end) {
                            const size_t after_ag = skip_ws_comments(ag_end);
                            if (after_ag < end && (src[after_ag] == '(' || src[after_ag] == '`')) {
                                erase_span(ag_p, ag_end);
                                i = ag_end;
                            }
                        }
                    }
                    expr_expected = false;
                    stmt_start = false;
                    after_dot = false;
                    prev_control_kw = false;
                    continue;
                }

                // Class body members (modifiers, fields, methods)
                if (in_class_body && brace_depth == 0 && paren_depth == 0 && bracket_depth == 0 && stmt_start) {
                    if (is_ts_ident_kw(id, id_len, "public") || is_ts_ident_kw(id, id_len, "private") ||
                        is_ts_ident_kw(id, id_len, "protected") || is_ts_ident_kw(id, id_len, "readonly") ||
                        is_ts_ident_kw(id, id_len, "override") || is_ts_ident_kw(id, id_len, "abstract")) {
                        const size_t nxt = skip_ws_comments(i);
                        if (nxt < end && (is_ident_start(src[nxt]) || src[nxt] == '[' || src[nxt] == '#' || src[nxt] == '*')) {
                            erase_span(id_start, i);
                            continue; // stay stmt_start
                        }
                    }
                    if (!is_ts_ident_kw(id, id_len, "static") && !is_ts_ident_kw(id, id_len, "async") &&
                        !is_ts_ident_kw(id, id_len, "get") && !is_ts_ident_kw(id, id_len, "set")) {
                        size_t cur = skip_ws_comments(i);
                        // Generic method `foo<T>(...)`
                        if (cur < end && src[cur] == '<') {
                            const size_t ag = skip_angle_group(cur, false);
                            if (ag != SIZE_MAX && ag <= end) {
                                erase_span(cur, ag);
                                cur = skip_ws_comments(ag);
                            }
                        }
                        if (cur < end && src[cur] == '(') {
                            cur = strip_param_list(cur);
                            const size_t col = skip_ws_comments(cur);
                            if (col < end && src[col] == ':') {
                                const size_t tend = skip_type_expr(col + 1u, false, true, true);
                                erase_span(col, tend);
                                cur = tend;
                            }
                            i = cur;
                            expr_expected = false;
                            stmt_start = false;
                            continue;
                        }
                        // Class field `foo?: T = ...` / `foo!: T;` / `foo: T = ...`
                        if (cur < end && (src[cur] == '?' || src[cur] == '!')) {
                            const size_t after_q = skip_ws_comments(cur + 1u);
                            if (after_q < end && (src[after_q] == ':' || src[after_q] == ';' || src[after_q] == '=' || src[after_q] == '\n')) {
                                erase_span(cur, cur + 1u);
                                cur = after_q;
                            }
                        }
                        if (cur < end && src[cur] == ':') {
                            const size_t tend = skip_type_expr(cur + 1u, false, true, true);
                            erase_span(cur, tend);
                            i = tend;
                            expr_expected = false;
                            stmt_start = false;
                            continue;
                        }
                    }
                }

                // Statement-start TS declarations (`export interface/type/declare`, `interface`, `type`, `declare`, `import type`)
                if (stmt_start && paren_depth == 0 && bracket_depth == 0 && !in_class_body) {
                    if (is_ts_ident_kw(id, id_len, "export")) {
                        const size_t nxt = skip_ws_comments(i);
                        if (nxt < end && is_ident_start(src[nxt])) {
                            size_t n2 = nxt + 1u;
                            while (n2 < end && is_ident_part(src[n2])) ++n2;
                            const size_t nlen = n2 - nxt;
                            if (is_ts_ident_kw(src + nxt, nlen, "interface")) {
                                size_t p = skip_ws_comments(n2);
                                while (p < end && src[p] != '{') {
                                    if (src[p] == '<') { const size_t a = skip_angle_group(p, false); p = a != SIZE_MAX ? a : p + 1u; }
                                    else ++p;
                                }
                                if (p < end && src[p] == '{') p = skip_balanced(p, '{', '}');
                                erase_span(id_start, p);
                                i = p;
                                continue;
                            }
                            if (is_ts_ident_kw(src + nxt, nlen, "type")) {
                                const size_t after_t = skip_ws_comments(n2);
                                if (after_t < end && (src[after_t] == '{' || src[after_t] == '*')) {
                                    // `export type { ... } [from '...'];`
                                    size_t p = (src[after_t] == '{') ? skip_balanced(after_t, '{', '}') : after_t + 1u;
                                    p = skip_ws_comments(p);
                                    if (p + 4u <= end && std::memcmp(src + p, "from", 4u) == 0 && !is_ident_part(src[p + 4u])) {
                                        p = skip_ws_comments(p + 4u);
                                        if (p < end && (src[p] == '\'' || src[p] == '"')) p = skip_string(p);
                                    }
                                    const size_t semi = skip_ws_comments(p);
                                    if (semi < end && src[semi] == ';') p = semi + 1u;
                                    erase_span(id_start, p);
                                    i = p;
                                    continue;
                                }
                                if (after_t < end && is_ident_start(src[after_t])) {
                                    size_t p = after_t + 1u;
                                    while (p < end && is_ident_part(src[p])) ++p;
                                    p = skip_ws_comments(p);
                                    if (p < end && src[p] == '<') {
                                        const size_t ag = skip_angle_group(p, false);
                                        if (ag != SIZE_MAX) p = skip_ws_comments(ag);
                                    }
                                    if (p < end && src[p] == '=') {
                                        p = skip_type_expr(p + 1u, false, false, false);
                                        const size_t semi = skip_ws_comments(p);
                                        if (semi < end && src[semi] == ';') p = semi + 1u;
                                        erase_span(id_start, p);
                                        i = p;
                                        continue;
                                    }
                                }
                            }
                            if (is_ts_ident_kw(src + nxt, nlen, "declare")) {
                                // Fall through to `declare` after erasing `export`
                                erase_span(id_start, i);
                                i = nxt;
                                continue;
                            }
                        }
                        // Regular `export ...`: stay stmt_start so `export const/function/class` work
                        expr_expected = true;
                        continue;
                    }

                    if (is_ts_ident_kw(id, id_len, "interface")) {
                        const size_t nxt = skip_ws_comments(i);
                        if (nxt < end && is_ident_start(src[nxt])) {
                            size_t p = nxt;
                            while (p < end && src[p] != '{') {
                                if (src[p] == '<') { const size_t a = skip_angle_group(p, false); p = a != SIZE_MAX ? a : p + 1u; }
                                else ++p;
                            }
                            if (p < end && src[p] == '{') p = skip_balanced(p, '{', '}');
                            erase_span(id_start, p);
                            i = p;
                            continue;
                        }
                    }

                    if (is_ts_ident_kw(id, id_len, "type")) {
                        const size_t nxt = skip_ws_comments(i);
                        if (nxt < end && is_ident_start(src[nxt])) {
                            size_t p = nxt + 1u;
                            while (p < end && is_ident_part(src[p])) ++p;
                            p = skip_ws_comments(p);
                            if (p < end && src[p] == '<') {
                                const size_t ag = skip_angle_group(p, false);
                                if (ag != SIZE_MAX) p = skip_ws_comments(ag);
                            }
                            if (p < end && src[p] == '=') {
                                p = skip_type_expr(p + 1u, false, false, false);
                                const size_t semi = skip_ws_comments(p);
                                if (semi < end && src[semi] == ';') p = semi + 1u;
                                erase_span(id_start, p);
                                i = p;
                                continue;
                            }
                        }
                    }

                    if (is_ts_ident_kw(id, id_len, "declare")) {
                        size_t p = skip_ws_comments(i);
                        while (p < end) {
                            if (src[p] == '{') { p = skip_balanced(p, '{', '}'); break; }
                            if (src[p] == ';') { ++p; break; }
                            if (src[p] == '\'' || src[p] == '"') { p = skip_string(p); continue; }
                            if (src[p] == '(') { p = skip_balanced(p, '(', ')'); continue; }
                            if (src[p] == '\n') { break; }
                            ++p;
                        }
                        erase_span(id_start, p);
                        i = p;
                        continue;
                    }

                    if (is_ts_ident_kw(id, id_len, "import")) {
                        const size_t nxt = skip_ws_comments(i);
                        if (nxt < end && is_ident_start(src[nxt])) {
                            size_t n2 = nxt + 1u;
                            while (n2 < end && is_ident_part(src[n2])) ++n2;
                            if (is_ts_ident_kw(src + nxt, n2 - nxt, "type")) {
                                const size_t after_t = skip_ws_comments(n2);
                                if (after_t < end && (is_ident_start(src[after_t]) || src[after_t] == '{' || src[after_t] == '*')) {
                                    // Erase entire `import type ... from '...'[;]`
                                    size_t p = after_t;
                                    while (p < end) {
                                        if (src[p] == '\'' || src[p] == '"') { p = skip_string(p); break; }
                                        if (src[p] == '{') { p = skip_balanced(p, '{', '}'); continue; }
                                        if (src[p] == ';') { ++p; break; }
                                        ++p;
                                    }
                                    const size_t semi = skip_ws_comments(p);
                                    if (semi < end && src[semi] == ';') p = semi + 1u;
                                    erase_span(id_start, p);
                                    i = p;
                                    continue;
                                }
                            }
                        }
                        // Regular `import { type Foo, bar } from '...'`: strip inline `type` specifiers inside `{...}`
                        size_t scan = nxt;
                        while (scan < end && src[scan] != ';' && src[scan] != '\n' && src[scan] != '\'' && src[scan] != '"') {
                            if (src[scan] == '{') {
                                const size_t bclose = skip_balanced(scan, '{', '}');
                                if (bclose > scan + 2u && bclose <= end && depth < 64u) {
                                    ++depth;
                                    strip_range(scan + 1u, bclose - 1u, false, true);
                                    --depth;
                                }
                                i = bclose < end ? bclose : end;
                                break;
                            }
                            ++scan;
                        }
                        expr_expected = false;
                        stmt_start = false;
                        continue;
                    }
                }

                // `function name<T>(params): RetType`
                if (is_ts_ident_kw(id, id_len, "function")) {
                    size_t p = skip_ws_comments(i);
                    if (p < end && src[p] == '*') p = skip_ws_comments(p + 1u);
                    if (p < end && is_ident_start(src[p])) {
                        while (p < end && is_ident_part(src[p])) ++p;
                        p = skip_ws_comments(p);
                    }
                    if (p < end && src[p] == '<') {
                        const size_t ag = skip_angle_group(p, false);
                        if (ag != SIZE_MAX && ag <= end) {
                            erase_span(p, ag);
                            p = skip_ws_comments(ag);
                        }
                    }
                    if (p < end && src[p] == '(') {
                        p = strip_param_list(p);
                        const size_t col = skip_ws_comments(p);
                        if (col < end && src[col] == ':') {
                            const size_t tend = skip_type_expr(col + 1u, false, true, true);
                            erase_span(col, tend);
                            p = tend;
                        }
                        i = p;
                    }
                    expr_expected = false;
                    stmt_start = false;
                    prev_control_kw = false;
                    continue;
                }

                // `class Name<T> [extends Base<U>] [implements I, J] { ... }`
                if (is_ts_ident_kw(id, id_len, "class")) {
                    size_t p = skip_ws_comments(i);
                    if (p < end && is_ident_start(src[p])) {
                        while (p < end && is_ident_part(src[p])) ++p;
                        p = skip_ws_comments(p);
                    }
                    if (p < end && src[p] == '<') {
                        const size_t ag = skip_angle_group(p, false);
                        if (ag != SIZE_MAX && ag <= end) {
                            erase_span(p, ag);
                            p = skip_ws_comments(ag);
                        }
                    }
                    while (p < end && src[p] != '{') {
                        if (is_ident_start(src[p])) {
                            const size_t ks = p;
                            while (p < end && is_ident_part(src[p])) ++p;
                            if (is_ts_ident_kw(src + ks, p - ks, "implements")) {
                                size_t imp_end = p;
                                while (imp_end < end && src[imp_end] != '{') {
                                    if (src[imp_end] == '<') {
                                        const size_t ag = skip_angle_group(imp_end, false);
                                        imp_end = (ag != SIZE_MAX) ? ag : (imp_end + 1u);
                                    } else {
                                        ++imp_end;
                                    }
                                }
                                erase_span(ks, imp_end);
                                p = imp_end;
                                break;
                            }
                            continue;
                        }
                        if (src[p] == '<') {
                            const size_t ag = skip_angle_group(p, false);
                            if (ag != SIZE_MAX && ag <= end) {
                                erase_span(p, ag);
                                p = ag;
                                continue;
                            }
                        }
                        ++p;
                    }
                    if (p < end && src[p] == '{') {
                        const size_t cend = skip_balanced(p, '{', '}');
                        if (cend > p + 2u && cend <= end && depth < 64u) {
                            ++depth;
                            strip_range(p + 1u, cend - 1u, true, false);
                            --depth;
                        }
                        i = cend < end ? cend : end;
                    } else {
                        i = p;
                    }
                    expr_expected = false;
                    stmt_start = false;
                    prev_control_kw = false;
                    continue;
                }

                // `const` / `let` / `var` variable declarations
                if (is_ts_ident_kw(id, id_len, "const") || is_ts_ident_kw(id, id_len, "let") ||
                    is_ts_ident_kw(id, id_len, "var")) {
                    i = strip_var_binding(i, end);
                    active_var_decl = true;
                    var_decl_brace = brace_depth;
                    var_decl_paren = paren_depth;
                    expr_expected = false;
                    stmt_start = false;
                    prev_control_kw = false;
                    continue;
                }

                // `as Type` / `satisfies Type` in expression postfix position
                if (!expr_expected &&
                    (is_ts_ident_kw(id, id_len, "as") || is_ts_ident_kw(id, id_len, "satisfies"))) {
                    const size_t nxt = skip_ws_comments(i);
                    if (nxt < end && src[nxt] != ':' && src[nxt] != '(' && src[nxt] != '=' && src[nxt] != ';') {
                        const size_t tend = skip_type_expr(nxt, true, true, true);
                        if (tend > nxt) {
                            erase_span(id_start, tend);
                            i = tend;
                            expr_expected = false;
                            continue;
                        }
                    }
                }

                // Generic function/constructor call `fn<TypeArgs>(...)`
                const bool is_prefix = is_expr_prefix_kw(id, id_len);
                const bool is_ctrl = is_control_kw(id, id_len);
                if (!is_prefix && !is_ctrl) {
                    const size_t ag_p = skip_ws_comments(i);
                    if (ag_p < end && src[ag_p] == '<') {
                        const size_t ag_end = skip_angle_group(ag_p, true);
                        if (ag_end != SIZE_MAX && ag_end <= end) {
                            const size_t after_ag = skip_ws_comments(ag_end);
                            if (after_ag < end && (src[after_ag] == '(' || src[after_ag] == '`')) {
                                erase_span(ag_p, ag_end);
                                i = ag_end;
                            }
                        }
                    }
                }

                expr_expected = is_prefix;
                prev_control_kw = is_ctrl;
                prev_was_arrow = false;
                if (is_ts_ident_kw(id, id_len, "async") || is_ts_ident_kw(id, id_len, "default")) {
                    // Keep `stmt_start` for `async function` / `export default function`
                } else {
                    stmt_start = false;
                }
                continue;
            }

            // Numbers
            if (is_ascii_digit(c)) {
                i = scan_number_end(src, end, i);
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }

            // Punctuation
            if (c == '(') {
                if (paren_sp < kMaxParenStack) paren_ctrl[paren_sp++] = prev_control_kw ? 1u : 0u;
                ++paren_depth;
                expr_expected = true;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                prev_was_arrow = false;
                ++i;
                continue;
            }
            if (c == ')') {
                if (paren_depth > 0) --paren_depth;
                const bool was_ctrl = (paren_sp > 0u) ? (paren_ctrl[--paren_sp] != 0u) : false;
                expr_expected = was_ctrl;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                ++i;
                continue;
            }
            if (c == '{') {
                const bool obj_lit = expr_expected && !stmt_start && !prev_was_arrow;
                if (brace_sp < kMaxParenStack) brace_is_expr[brace_sp++] = obj_lit ? 1u : 0u;
                ++brace_depth;
                expr_expected = true;
                stmt_start = true;
                after_dot = false;
                prev_control_kw = false;
                prev_was_arrow = false;
                ++i;
                continue;
            }
            if (c == '}') {
                if (brace_depth > 0) --brace_depth;
                if (active_var_decl && brace_depth < var_decl_brace) active_var_decl = false;
                const bool was_obj = (brace_sp > 0u) ? (brace_is_expr[--brace_sp] != 0u) : false;
                expr_expected = !was_obj;
                stmt_start = !was_obj;
                after_dot = false;
                prev_control_kw = false;
                prev_was_arrow = false;
                ++i;
                continue;
            }
            if (c == '[') {
                ++bracket_depth;
                expr_expected = true;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                ++i;
                continue;
            }
            if (c == ']') {
                if (bracket_depth > 0) --bracket_depth;
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                ++i;
                continue;
            }
            if (c == ';') {
                active_var_decl = false;
                expr_expected = true;
                stmt_start = true;
                after_dot = false;
                prev_control_kw = false;
                ++i;
                continue;
            }
            if (c == ',' && active_var_decl && brace_depth == var_decl_brace && paren_depth == var_decl_paren && bracket_depth == 0) {
                ++i;
                i = strip_var_binding(i, end);
                expr_expected = false;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '.') {
                if (i + 2u < end && src[i + 1u] == '.' && src[i + 2u] == '.') {
                    i += 3u;
                    expr_expected = true;
                    after_dot = false;
                } else {
                    ++i;
                    expr_expected = false;
                    after_dot = true;
                }
                stmt_start = false;
                prev_control_kw = false;
                continue;
            }
            if (c == '=' && i + 1u < end && src[i + 1u] == '>') {
                i += 2u;
                expr_expected = true;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                prev_was_arrow = true;
                continue;
            }
            if ((c == '+' || c == '-') && i + 1u < end && src[i + 1u] == c) {
                i += 2u;
                stmt_start = false;
                after_dot = false;
                prev_control_kw = false;
                prev_was_arrow = false;
                continue;
            }
            expr_expected = true;
            stmt_start = false;
            after_dot = false;
            prev_control_kw = false;
            prev_was_arrow = false;
            ++i;
        }
    }
};

} // namespace

bool inlay_jsx_transpile(const char* src, size_t src_len,
                         const char* filename,
                         char** io_buf, size_t* io_cap, size_t* out_len,
                         char* err_buf, size_t err_cap) {
    if (err_buf && err_cap) err_buf[0] = '\0';
    if (!io_buf || !io_cap || !out_len || (!src && src_len > 0u)) {
        if (err_buf && err_cap) {
            std::snprintf(err_buf, err_cap, "JSXSyntaxError: Invalid transpiler arguments");
        }
        return false;
    }
    *out_len = 0u;
    if (src_len == 0u) {
        return true;
    }

    // Stage 1: Line-preserving TypeScript type erasure (zero-allocation fast path when no TS candidates exist).
    char* ts_tmp = nullptr;
    const char* eff_src = src;
    size_t eff_len = src_len;
    bool ts_modified = false;
    if (has_ts_candidate(src, src_len)) {
        ts_tmp = static_cast<char*>(SDL_malloc(src_len + 1u));
        if (!ts_tmp) {
            if (err_buf && err_cap) {
                std::snprintf(err_buf, err_cap, "JSXSyntaxError: Out of memory during TypeScript erasure");
            }
            return false;
        }
        std::memcpy(ts_tmp, src, src_len);
        ts_tmp[src_len] = '\0';
        TsStripper ts = {};
        ts.src = src;
        ts.len = src_len;
        ts.out = ts_tmp;
        ts.strip_range(0u, src_len, false, false);
        if (ts.modified) {
            eff_src = ts_tmp;
            eff_len = src_len;
            ts_modified = true;
        } else {
            SDL_free(ts_tmp);
            ts_tmp = nullptr;
        }
    }

    // Stage 2: JSX transpilation (or direct copy if only TS erasure occurred).
    if (!has_jsx_candidate(eff_src, eff_len)) {
        if (!ts_modified) {
            SDL_free(ts_tmp);
            return true;
        }
        if (*io_cap < eff_len + 1u) {
            auto* nb = static_cast<char*>(SDL_realloc(*io_buf, eff_len + 1u));
            if (!nb) {
                SDL_free(ts_tmp);
                if (err_buf && err_cap) {
                    std::snprintf(err_buf, err_cap, "JSXSyntaxError: Out of memory");
                }
                return false;
            }
            *io_buf = nb;
            *io_cap = eff_len + 1u;
        }
        std::memcpy(*io_buf, eff_src, eff_len + 1u);
        *out_len = eff_len;
        SDL_free(ts_tmp);
        return true;
    }

    JsxTranspiler tp = {};
    tp.src = eff_src;
    tp.len = eff_len;
    tp.filename = (filename && filename[0]) ? filename : "<surface>";
    tp.line = 1u;
    tp.out.io_buf = io_buf;
    tp.out.io_cap = io_cap;
    tp.out.initial_cap = eff_len + (eff_len / 2u) + 256u;
    tp.err_buf = err_buf;
    tp.err_cap = err_cap;

    tp.parse_js_block(false);
    SDL_free(ts_tmp);
    if (tp.failed) {
        *out_len = 0u;
        return false;
    }
    if (!tp.out.reserve(0u)) {
        tp.check_out_error();
        *out_len = 0u;
        return false;
    }
    (*io_buf)[tp.out.len] = '\0';
    *out_len = tp.out.len;
    return true;
}
