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
    if (src_len == 0u || !has_jsx_candidate(src, src_len)) {
        return true;
    }

    JsxTranspiler tp = {};
    tp.src = src;
    tp.len = src_len;
    tp.filename = (filename && filename[0]) ? filename : "<surface>";
    tp.line = 1u;
    tp.out.io_buf = io_buf;
    tp.out.io_cap = io_cap;
    tp.out.initial_cap = src_len + (src_len / 2u) + 256u;
    tp.err_buf = err_buf;
    tp.err_cap = err_cap;

    tp.parse_js_block(false);
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
