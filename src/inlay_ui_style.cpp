#include "inlay_ui_internal.h"
#if defined(INLAY_ENABLE_SCRIPT) && INLAY_ENABLE_SCRIPT

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kMaxColorTweens = 256u;

inline bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

inline void trim_slice(const char*& p, size_t& len) noexcept {
    while (len > 0u && is_ascii_space(*p)) {
        ++p;
        --len;
    }
    while (len > 0u && is_ascii_space(p[len - 1u])) {
        --len;
    }
}

inline bool str_ieq(const char* chars, size_t len, const char* lit) noexcept {
    const size_t n = std::strlen(lit);
    if (len != n) return false;
    for (size_t i = 0u; i < n; ++i) {
        if (ascii_lower(chars[i]) != ascii_lower(lit[i])) return false;
    }
    return true;
}

inline bool starts_with(const char* chars, size_t len, const char* prefix) noexcept {
    const size_t n = std::strlen(prefix);
    return len >= n && (n == 0u || std::memcmp(chars, prefix, n) == 0);
}

inline bool starts_with_ci(const char* chars, size_t len, const char* prefix) noexcept {
    const size_t n = std::strlen(prefix);
    if (len < n) return false;
    for (size_t i = 0u; i < n; ++i) {
        if (ascii_lower(chars[i]) != ascii_lower(prefix[i])) return false;
    }
    return true;
}

inline float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline uint8_t float_to_u8(float v) noexcept {
    const float c = clampf(v, 0.0f, 255.0f);
    return uint8_t(c + 0.5f);
}

inline uint8_t unit_to_u8(float v) noexcept {
    const float c = clampf(v, 0.0f, 1.0f);
    return uint8_t(c * 255.0f + 0.5f);
}

bool parse_float_slice(const char* chars, size_t len, float* out) noexcept {
    trim_slice(chars, len);
    if (len == 0u || len >= 48u) return false;
    char buf[48];
    for (size_t i = 0u; i < len; ++i) {
        const char c = chars[i];
        if (c == 'x' || c == 'X') return false;
        buf[i] = c;
    }
    buf[len] = '\0';
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    if (end == buf || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool parse_uint_slice(const char* chars, size_t len, uint32_t max_val, uint32_t* out) noexcept {
    if (len == 0u || len > 10u) return false;
    uint64_t acc = 0u;
    for (size_t i = 0u; i < len; ++i) {
        const char c = chars[i];
        if (c < '0' || c > '9') return false;
        acc = acc * 10u + uint64_t(c - '0');
        if (acc > max_val) return false;
    }
    *out = uint32_t(acc);
    return true;
}

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse optional `/alpha` modifier suffix (e.g. `15`, `8`, `80`, `50%`, `0.5`, `[0.35]`).
bool parse_alpha_suffix(const char* chars, size_t len, uint8_t* out_mul) noexcept {
    trim_slice(chars, len);
    if (len >= 2u && chars[0] == '[' && chars[len - 1u] == ']') {
        ++chars;
        len -= 2u;
        trim_slice(chars, len);
    }
    if (len == 0u) return false;

    bool is_pct = false;
    if (chars[len - 1u] == '%') {
        is_pct = true;
        --len;
    }
    bool has_dot = false;
    for (size_t i = 0u; i < len; ++i) {
        if (chars[i] == '.') {
            has_dot = true;
            break;
        }
    }
    float v = 0.0f;
    if (!parse_float_slice(chars, len, &v) || v < 0.0f) return false;

    float frac = 0.0f;
    if (is_pct) {
        if (v > 100.0f) return false;
        frac = v * 0.01f;
    } else if (has_dot && v <= 1.0f) {
        frac = v;
    } else {
        if (v > 100.0f) return false;
        frac = v * 0.01f;
    }
    *out_mul = unit_to_u8(frac);
    return true;
}

bool parse_hex_color(const char* chars, size_t len, EtchColor* out) noexcept {
    if (len < 4u || chars[0] != '#') return false;
    if (len == 7u || len == 9u) {
        uint32_t rgba = 0u;
        for (size_t i = 1u; i < len; ++i) {
            const int d = hex_digit(chars[i]);
            if (d < 0) return false;
            rgba = (rgba << 4) | uint32_t(d);
        }
        if (len == 7u) {
            *out = etch_rgba8(uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba), 255u);
        } else {
            *out = etch_rgba8(uint8_t(rgba >> 24), uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba));
        }
        return true;
    }
    if (len == 4u || len == 5u) {
        uint8_t ch[4] = {0u, 0u, 0u, 255u};
        for (size_t i = 1u; i < len; ++i) {
            const int d = hex_digit(chars[i]);
            if (d < 0) return false;
            ch[i - 1u] = uint8_t(d * 17);
        }
        *out = etch_rgba8(ch[0], ch[1], ch[2], ch[3]);
        return true;
    }
    return false;
}

// Tokenize CSS function arguments separated by commas, whitespace, or `/`.
// Returns count of tokens (up to max_parts), or 0 on empty/overflow.
size_t split_css_args(const char* body, size_t len, const char** out_ptr, size_t* out_len,
                      size_t max_parts) noexcept {
    size_t count = 0u;
    size_t i = 0u;
    while (i < len) {
        while (i < len && (is_ascii_space(body[i]) || body[i] == ',' || body[i] == '/')) ++i;
        if (i >= len) break;
        const size_t start = i;
        while (i < len && !is_ascii_space(body[i]) && body[i] != ',' && body[i] != '/') ++i;
        if (count >= max_parts) return 0u;
        out_ptr[count] = body + start;
        out_len[count] = i - start;
        ++count;
    }
    return count;
}

bool parse_css_alpha_arg(const char* chars, size_t len, float* out_alpha, bool allow_255) noexcept {
    if (len == 0u) return false;
    if (chars[len - 1u] == '%') {
        float pct = 0.0f;
        if (!parse_float_slice(chars, len - 1u, &pct)) return false;
        *out_alpha = clampf(pct * 0.01f, 0.0f, 1.0f);
        return true;
    }
    float v = 0.0f;
    if (!parse_float_slice(chars, len, &v)) return false;
    if (allow_255 && v > 1.0f) {
        *out_alpha = clampf(v / 255.0f, 0.0f, 1.0f);
    } else {
        *out_alpha = clampf(v, 0.0f, 1.0f);
    }
    return true;
}

bool parse_rgb_func(const char* body, size_t len, EtchColor* out) noexcept {
    const char* parts[5];
    size_t plen[5];
    const size_t n = split_css_args(body, len, parts, plen, 4u);
    if (n != 3u && n != 4u) return false;

    uint8_t rgb[3] = {};
    for (size_t i = 0u; i < 3u; ++i) {
        if (plen[i] > 1u && parts[i][plen[i] - 1u] == '%') {
            float pct = 0.0f;
            if (!parse_float_slice(parts[i], plen[i] - 1u, &pct)) return false;
            rgb[i] = float_to_u8(pct * 2.55f);
        } else {
            float v = 0.0f;
            if (!parse_float_slice(parts[i], plen[i], &v)) return false;
            rgb[i] = float_to_u8(v);
        }
    }
    float alpha = 1.0f;
    if (n == 4u && !parse_css_alpha_arg(parts[3], plen[3], &alpha, true)) return false;
    *out = etch_rgba8(rgb[0], rgb[1], rgb[2], unit_to_u8(alpha));
    return true;
}

bool parse_angle_deg_slice(const char* chars, size_t len, float* out_deg) noexcept {
    trim_slice(chars, len);
    if (len == 0u) return false;
    if (len > 4u && str_ieq(chars + len - 4u, 4u, "turn")) {
        float t = 0.0f;
        if (!parse_float_slice(chars, len - 4u, &t)) return false;
        *out_deg = t * 360.0f;
        return true;
    }
    if (len > 4u && str_ieq(chars + len - 4u, 4u, "grad")) {
        float g = 0.0f;
        if (!parse_float_slice(chars, len - 4u, &g)) return false;
        *out_deg = g * 0.9f;
        return true;
    }
    if (len > 3u && str_ieq(chars + len - 3u, 3u, "deg")) {
        return parse_float_slice(chars, len - 3u, out_deg);
    }
    if (len > 3u && str_ieq(chars + len - 3u, 3u, "rad")) {
        float r = 0.0f;
        if (!parse_float_slice(chars, len - 3u, &r)) return false;
        *out_deg = r * (180.0f / 3.14159265358979323846f);
        return true;
    }
    return parse_float_slice(chars, len, out_deg);
}

bool parse_oklch_func(const char* body, size_t len, EtchColor* out) noexcept {
    const char* parts[5];
    size_t plen[5];
    const size_t n = split_css_args(body, len, parts, plen, 4u);
    if (n != 3u && n != 4u) return false;

    float L = 0.0f;
    if (plen[0] > 1u && parts[0][plen[0] - 1u] == '%') {
        float pct = 0.0f;
        if (!parse_float_slice(parts[0], plen[0] - 1u, &pct)) return false;
        L = clampf(pct * 0.01f, 0.0f, 1.0f);
    } else {
        if (!parse_float_slice(parts[0], plen[0], &L)) return false;
        L = clampf(L, 0.0f, 1.0f);
    }

    float C = 0.0f;
    if (plen[1] > 1u && parts[1][plen[1] - 1u] == '%') {
        float pct = 0.0f;
        if (!parse_float_slice(parts[1], plen[1] - 1u, &pct)) return false;
        C = clampf(pct * 0.004f, 0.0f, 1.0f);
    } else {
        if (!parse_float_slice(parts[1], plen[1], &C)) return false;
        C = clampf(C, 0.0f, 1.0f);
    }

    float H = 0.0f;
    if (!parse_angle_deg_slice(parts[2], plen[2], &H)) return false;

    float alpha = 1.0f;
    if (n == 4u && !parse_css_alpha_arg(parts[3], plen[3], &alpha, false)) return false;

    *out = etch_color_oklch(L, C, H, alpha);
    return true;
}

// Normalize a semantic token name by lowercasing ASCII letters and stripping `-`/`_`.
// Rejects any non-alphanumeric separator characters.
bool normalize_token_key(const char* chars, size_t len, char* out, size_t cap, size_t* out_len) noexcept {
    size_t w = 0u;
    for (size_t i = 0u; i < len; ++i) {
        const char c = chars[i];
        if (c == '-' || c == '_') continue;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok || w + 1u >= cap) return false;
        out[w++] = ascii_lower(c);
    }
    if (w == 0u) return false;
    out[w] = '\0';
    *out_len = w;
    return true;
}

bool match_semantic_token(const char* key, size_t len, ThemeToken* out) noexcept {
    struct TokEntry {
        const char* name;
        ThemeToken tok;
    };
    static constexpr TokEntry kTable[] = {
        {"transparent",        TokTransparent},
        {"bgcanvas",           TokBgCanvas},
        {"canvas",             TokBgCanvas},
        {"bgsurface",          TokBgSurface},
        {"surface",            TokBgSurface},
        {"bgelevated",         TokBgElevated},
        {"elevated",           TokBgElevated},
        {"bgchrome",           TokBgChrome},
        {"chrome",             TokBgChrome},
        {"bgpopup",            TokBgPopup},
        {"popup",              TokBgPopup},
        {"bgtooltip",          TokBgTooltip},
        {"tooltip",            TokBgTooltip},
        {"bginput",            TokBgInput},
        {"input",              TokBgInput},
        {"bgtable",            TokBgTable},
        {"table",              TokBgTable},
        {"bgnodecanvas",       TokBgNodeCanvas},
        {"nodecanvas",         TokBgNodeCanvas},
        {"surfacecontrol",     TokSurfaceControl},
        {"control",            TokSurfaceControl},
        {"surfacehover",       TokSurfaceHover},
        {"hover",              TokSurfaceHover},
        {"surfaceactive",      TokSurfaceActive},
        {"active",             TokSurfaceActive},
        {"surfacedisabled",    TokSurfaceDisabled},
        {"disabled",           TokSurfaceDisabled},
        {"surfacesubtle",      TokSurfaceSubtle},
        {"subtle",             TokSurfaceSubtle},
        {"surfacesubtlehover", TokSurfaceSubtleHover},
        {"subtlehover",        TokSurfaceSubtleHover},
        {"surfacesubtlebody",  TokSurfaceSubtleBody},
        {"subtlebody",         TokSurfaceSubtleBody},
        {"surfaceselected",    TokSurfaceSelected},
        {"selected",           TokSurfaceSelected},
        {"textprimary",        TokTextPrimary},
        {"primary",            TokTextPrimary},
        {"textsecondary",      TokTextSecondary},
        {"secondary",          TokTextSecondary},
        {"textbody",           TokTextBody},
        {"body",               TokTextBody},
        {"textcode",           TokTextCode},
        {"code",               TokTextCode},
        {"textmuted",          TokTextMuted},
        {"muted",              TokTextMuted},
        {"textdisabled",       TokTextDisabled},
        {"accent",             TokAccent},
        {"accentsecondary",    TokAccentSecondary},
        {"accentprimary",      TokAccentPrimary},
        {"accenthover",        TokAccentHover},
        {"focusring",          TokFocusRing},
        {"ring",               TokFocusRing},
        {"danger",             TokDanger},
        {"error",              TokDanger},
        {"destructive",        TokDanger},
        {"warning",            TokWarning},
        {"warn",               TokWarning},
        {"success",            TokSuccess},
        {"info",               TokInfo},
        {"bordersubtle",       TokBorderSubtle},
        {"border",             TokBorderSubtle},
        {"borderline",         TokBorderLine},
        {"line",               TokBorderLine},
        {"borderinput",        TokBorderInput},
        {"bordernode",         TokBorderNode},
        {"bordercontrol",      TokBorderControl},
        {"bordertooltip",      TokBorderTooltip},
        {"selection",          TokSelection},
        {"selectionhover",     TokSelectionHover},
        {"zebrarow",           TokZebraRow},
        {"zebra",              TokZebraRow},
        {"activeline",         TokActiveLine},
        {"backdrop",           TokBackdrop},
        {"shadow",             TokShadow},
        {"onprimary",          TokOnPrimary},
        {"onerror",            TokOnError},
        {"ondanger",           TokOnError},
        {"syntaxcomment",      TokSyntaxComment},
        {"syntaxstring",       TokSyntaxString},
        {"syntaxnumber",       TokSyntaxNumber},
        {"syntaxkeyword",      TokSyntaxKeyword},
    };
    for (const auto& entry : kTable) {
        if (inlay_ui_str_eq(key, len, entry.name)) {
            *out = entry.tok;
            return true;
        }
    }
    return false;
}

bool parse_time_token(const char* chars, size_t len, float* out_sec) noexcept {
    if (len < 2u) return false;
    if (len > 2u && ascii_lower(chars[len - 2u]) == 'm' && ascii_lower(chars[len - 1u]) == 's') {
        float ms = 0.0f;
        if (!parse_float_slice(chars, len - 2u, &ms) || ms < 0.0f || ms > 60000.0f) return false;
        *out_sec = ms * 0.001f;
        return true;
    }
    if (ascii_lower(chars[len - 1u]) == 's') {
        float sec = 0.0f;
        if (!parse_float_slice(chars, len - 1u, &sec) || sec < 0.0f || sec > 60.0f) return false;
        *out_sec = sec;
        return true;
    }
    return false;
}

bool parse_transition_prop_word(const char* w, size_t len, uint16_t* out_mask) noexcept {
    if (str_ieq(w, len, "all")) { *out_mask = TransAll; return true; }
    if (str_ieq(w, len, "colors")) { *out_mask = uint16_t(TransBg | TransColor | TransBorder); return true; }
    if (str_ieq(w, len, "color") || str_ieq(w, len, "text")) { *out_mask = TransColor; return true; }
    if (str_ieq(w, len, "bg") || str_ieq(w, len, "background")) { *out_mask = TransBg; return true; }
    if (str_ieq(w, len, "border") || str_ieq(w, len, "border-color") || str_ieq(w, len, "borderColor")) {
        *out_mask = TransBorder;
        return true;
    }
    if (str_ieq(w, len, "opacity")) { *out_mask = TransOpacity; return true; }
    if (str_ieq(w, len, "transform") || str_ieq(w, len, "scale") ||
        str_ieq(w, len, "rotate") || str_ieq(w, len, "translate")) {
        *out_mask = TransTransform;
        return true;
    }
    if (str_ieq(w, len, "layout")) { *out_mask = TransLayout; return true; }
    return false;
}

bool parse_transition_easing_word(const char* w, size_t len, TransitionSpec* out) noexcept {
    if (str_ieq(w, len, "linear")) {
        out->easing = TransEaseLinear;
        return true;
    }
    if (str_ieq(w, len, "ease-in") || str_ieq(w, len, "in") || str_ieq(w, len, "easeIn")) {
        out->easing = TransEaseIn;
        return true;
    }
    if (str_ieq(w, len, "ease-out") || str_ieq(w, len, "out") || str_ieq(w, len, "ease") ||
        str_ieq(w, len, "easeOut")) {
        out->easing = TransEaseOut;
        return true;
    }
    if (str_ieq(w, len, "ease-in-out") || str_ieq(w, len, "in-out") || str_ieq(w, len, "easeInOut")) {
        out->easing = TransEaseInOut;
        return true;
    }
    if (str_ieq(w, len, "spring") || str_ieq(w, len, "snappy")) {
        const EtchSpringSpec sp = etch_spring_spec_snappy();
        out->easing = TransEaseSpring;
        out->bezier[0] = sp.response;
        out->bezier[1] = sp.damping_ratio;
        return true;
    }
    if (str_ieq(w, len, "fluid")) {
        const EtchSpringSpec sp = etch_spring_spec_fluid();
        out->easing = TransEaseSpring;
        out->bezier[0] = sp.response;
        out->bezier[1] = sp.damping_ratio;
        return true;
    }
    if (str_ieq(w, len, "bouncy")) {
        const EtchSpringSpec sp = etch_spring_spec_bouncy();
        out->easing = TransEaseSpring;
        out->bezier[0] = sp.response;
        out->bezier[1] = sp.damping_ratio;
        return true;
    }
    if (str_ieq(w, len, "gentle")) {
        const EtchSpringSpec sp = etch_spring_spec_gentle();
        out->easing = TransEaseSpring;
        out->bezier[0] = sp.response;
        out->bezier[1] = sp.damping_ratio;
        return true;
    }
    if (len > 14u && starts_with_ci(w, len, "cubic-bezier(") && w[len - 1u] == ')') {
        const char* parts[5];
        size_t plen[5];
        const size_t n = split_css_args(w + 13u, len - 14u, parts, plen, 4u);
        if (n != 4u) return false;
        float vals[4] = {};
        for (size_t i = 0u; i < 4u; ++i) {
            if (!parse_float_slice(parts[i], plen[i], &vals[i])) return false;
        }
        out->easing = TransEaseCubicBezier;
        out->bezier[0] = clampf(vals[0], 0.0f, 1.0f);
        out->bezier[1] = vals[1];
        out->bezier[2] = clampf(vals[2], 0.0f, 1.0f);
        out->bezier[3] = vals[3];
        return true;
    }
    if (len > 8u && starts_with_ci(w, len, "spring(") && w[len - 1u] == ')') {
        const char* parts[3];
        size_t plen[3];
        const size_t n = split_css_args(w + 7u, len - 8u, parts, plen, 2u);
        if (n != 2u) return false;
        float resp = 0.0f, damp = 0.0f;
        if (!parse_float_slice(parts[0], plen[0], &resp) ||
            !parse_float_slice(parts[1], plen[1], &damp) ||
            resp <= 0.0f || damp <= 0.0f) {
            return false;
        }
        out->easing = TransEaseSpring;
        out->bezier[0] = clampf(resp, 0.02f, 5.0f);
        out->bezier[1] = clampf(damp, 0.05f, 4.0f);
        return true;
    }
    return false;
}

bool apply_alpha_mask_preset(const char* chars, size_t len, EtchAlphaMaskDesc* out) noexcept {
    trim_slice(chars, len);
    if (len == 0u) return false;
    if (str_ieq(chars, len, "none")) {
        *out = etch_alpha_mask_desc_default();
        return true;
    }
    if (str_ieq(chars, len, "fade-bottom") || str_ieq(chars, len, "fadeBottom")) {
        out->kind = ETCH_ALPHA_MASK_LINEAR_GRADIENT;
        out->angle_deg = 90.0f;
        out->start_t = 0.65f;
        out->end_t = 1.0f;
        out->alpha_start = 1.0f;
        out->alpha_end = 0.0f;
        return true;
    }
    if (str_ieq(chars, len, "fade-top") || str_ieq(chars, len, "fadeTop")) {
        out->kind = ETCH_ALPHA_MASK_LINEAR_GRADIENT;
        out->angle_deg = 90.0f;
        out->start_t = 0.0f;
        out->end_t = 0.35f;
        out->alpha_start = 0.0f;
        out->alpha_end = 1.0f;
        return true;
    }
    if (str_ieq(chars, len, "fade-right") || str_ieq(chars, len, "fadeRight")) {
        out->kind = ETCH_ALPHA_MASK_LINEAR_GRADIENT;
        out->angle_deg = 0.0f;
        out->start_t = 0.65f;
        out->end_t = 1.0f;
        out->alpha_start = 1.0f;
        out->alpha_end = 0.0f;
        return true;
    }
    if (str_ieq(chars, len, "fade-left") || str_ieq(chars, len, "fadeLeft")) {
        out->kind = ETCH_ALPHA_MASK_LINEAR_GRADIENT;
        out->angle_deg = 0.0f;
        out->start_t = 0.0f;
        out->end_t = 0.35f;
        out->alpha_start = 0.0f;
        out->alpha_end = 1.0f;
        return true;
    }
    if (str_ieq(chars, len, "fade-y") || str_ieq(chars, len, "fadeY")) {
        out->kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
        out->center_norm = {0.5f, 0.5f};
        out->radius_norm = {1000.0f, 0.5f};
        out->start_t = 0.72f;
        out->end_t = 1.0f;
        out->alpha_start = 1.0f;
        out->alpha_end = 0.0f;
        return true;
    }
    if (str_ieq(chars, len, "fade-x") || str_ieq(chars, len, "fadeX")) {
        out->kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
        out->center_norm = {0.5f, 0.5f};
        out->radius_norm = {0.5f, 1000.0f};
        out->start_t = 0.72f;
        out->end_t = 1.0f;
        out->alpha_start = 1.0f;
        out->alpha_end = 0.0f;
        return true;
    }
    if (str_ieq(chars, len, "spotlight")) {
        out->kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
        out->center_norm = {0.5f, 0.5f};
        out->radius_norm = {0.5f, 0.5f};
        out->start_t = 0.25f;
        out->end_t = 1.0f;
        out->alpha_start = 1.0f;
        out->alpha_end = 0.0f;
        return true;
    }
    if (str_ieq(chars, len, "vignette")) {
        out->kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
        out->center_norm = {0.5f, 0.5f};
        out->radius_norm = {0.65f, 0.65f};
        out->start_t = 0.45f;
        out->end_t = 1.0f;
        out->alpha_start = 1.0f;
        out->alpha_end = 0.15f;
        return true;
    }
    if (str_ieq(chars, len, "rounded") || str_ieq(chars, len, "rect")) {
        out->kind = ETCH_ALPHA_MASK_ROUNDED;
        return true;
    }
    if (str_ieq(chars, len, "linear") || str_ieq(chars, len, "linear-gradient") ||
        str_ieq(chars, len, "linearGradient")) {
        out->kind = ETCH_ALPHA_MASK_LINEAR_GRADIENT;
        return true;
    }
    if (str_ieq(chars, len, "radial") || str_ieq(chars, len, "radial-spotlight") ||
        str_ieq(chars, len, "radialSpotlight")) {
        out->kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
        return true;
    }
    if (str_ieq(chars, len, "texture")) {
        out->kind = ETCH_ALPHA_MASK_TEXTURE;
        return true;
    }
    return false;
}

bool read_js_float_range(JSContext* js, JSValueConst v, float lo, float hi, float* out) noexcept {
    double d = 0.0;
    if (JS_ToFloat64(js, &d, v) < 0 || !std::isfinite(d)) return false;
    const float f = float(d);
    if (!std::isfinite(f)) return false;
    *out = clampf(f, lo, hi);
    return true;
}

bool read_js_vec2_prop(JSContext* js, JSValueConst obj, const char* name, float lo, float hi,
                       EtchVec2* out, bool allow_scalar) noexcept {
    const JSValue v = JS_GetPropertyStr(js, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(js, v);
        return false;
    }
    bool ok = false;
    if (JS_IsArray(v)) {
        const JSValue v0 = JS_GetPropertyUint32(js, v, 0u);
        const JSValue v1 = JS_GetPropertyUint32(js, v, 1u);
        float x = 0.0f, y = 0.0f;
        if (!JS_IsUndefined(v0) && !JS_IsUndefined(v1) &&
            read_js_float_range(js, v0, lo, hi, &x) &&
            read_js_float_range(js, v1, lo, hi, &y)) {
            out->x = x;
            out->y = y;
            ok = true;
        }
        JS_FreeValue(js, v0);
        JS_FreeValue(js, v1);
    } else if (JS_IsObject(v)) {
        const JSValue vx = JS_GetPropertyStr(js, v, "x");
        const JSValue vy = JS_GetPropertyStr(js, v, "y");
        float x = out->x, y = out->y;
        const bool hx = !JS_IsUndefined(vx) && read_js_float_range(js, vx, lo, hi, &x);
        const bool hy = !JS_IsUndefined(vy) && read_js_float_range(js, vy, lo, hi, &y);
        if (hx || hy) {
            out->x = x;
            out->y = y;
            ok = true;
        }
        JS_FreeValue(js, vx);
        JS_FreeValue(js, vy);
    } else if (allow_scalar) {
        float s_val = 0.0f;
        if (read_js_float_range(js, v, lo, hi, &s_val)) {
            out->x = s_val;
            out->y = s_val;
            ok = true;
        }
    }
    JS_FreeValue(js, v);
    return ok;
}

bool read_js_corners_prop(JSContext* js, JSValueConst obj, const char* name, EtchCorners* out) noexcept {
    const JSValue v = JS_GetPropertyStr(js, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(js, v);
        return false;
    }
    bool ok = false;
    if (JS_IsArray(v)) {
        float c[4] = {};
        bool all_ok = true;
        for (uint32_t i = 0u; i < 4u; ++i) {
            const JSValue el = JS_GetPropertyUint32(js, v, i);
            if (JS_IsUndefined(el) || !read_js_float_range(js, el, 0.0f, 16384.0f, &c[i])) {
                all_ok = false;
            }
            JS_FreeValue(js, el);
        }
        if (all_ok) {
            *out = {c[0], c[1], c[2], c[3]};
            ok = true;
        }
    } else if (JS_IsObject(v)) {
        static const char* const keys[4] = {"tl", "tr", "br", "bl"};
        float* dst[4] = {&out->tl, &out->tr, &out->br, &out->bl};
        for (uint32_t i = 0u; i < 4u; ++i) {
            const JSValue el = JS_GetPropertyStr(js, v, keys[i]);
            if (!JS_IsUndefined(el) && read_js_float_range(js, el, 0.0f, 16384.0f, dst[i])) {
                ok = true;
            }
            JS_FreeValue(js, el);
        }
    } else {
        float r = 0.0f;
        if (read_js_float_range(js, v, 0.0f, 16384.0f, &r)) {
            *out = etch_corners_all(r);
            ok = true;
        }
    }
    JS_FreeValue(js, v);
    return ok;
}

// ============================================================================
// Utility Class (`cls`) Helpers
// ============================================================================

enum ClsState : uint8_t {
    ClsNormal = 0,
    ClsHover  = 1,
    ClsActive = 2,
    ClsFocus  = 3
};

// Parse `[Npx]`, `[N]`, or `[Nrem]` inside square brackets.
bool parse_bracket_px(const char* s, size_t len, float* out_px, bool allow_neg) noexcept {
    if (len < 3u || s[0] != '[' || s[len - 1u] != ']') return false;
    const char* inner = s + 1u;
    size_t n = len - 2u;
    float mult = 1.0f;
    if (n > 2u && inner[n - 2u] == 'p' && inner[n - 1u] == 'x') {
        n -= 2u;
    } else if (n > 3u && inner[n - 3u] == 'r' && inner[n - 2u] == 'e' && inner[n - 1u] == 'm') {
        n -= 3u;
        mult = 16.0f;
    }
    float v = 0.0f;
    if (!parse_float_slice(inner, n, &v)) return false;
    v *= mult;
    if (!allow_neg && v < 0.0f) return false;
    if (v < -16384.0f || v > 16384.0f) return false;
    *out_px = v;
    return true;
}

// Parse `[N%]` inside square brackets -> ratio [0..1].
bool parse_bracket_pct(const char* s, size_t len, float* out_ratio) noexcept {
    if (len < 4u || s[0] != '[' || s[len - 1u] != ']' || s[len - 2u] != '%') return false;
    float pct = 0.0f;
    if (!parse_float_slice(s + 1u, len - 3u, &pct) || pct < 0.0f || pct > 100.0f) return false;
    *out_ratio = pct * 0.01f;
    return true;
}

// Parse Tailwind spacing scale (`px`=1, `0.5`=2, `1.5`=6, `2.5`=10, `{n}`=n*4, `[Npx]`).
bool parse_spacing_px(const char* s, size_t len, float* out_px) noexcept {
    if (len == 0u) return false;
    if (inlay_ui_str_eq(s, len, "px")) {
        *out_px = 1.0f;
        return true;
    }
    if (s[0] == '[') {
        return parse_bracket_px(s, len, out_px, false);
    }
    float v = 0.0f;
    if (!parse_float_slice(s, len, &v) || v < 0.0f || v > 4096.0f) return false;
    *out_px = v * 4.0f;
    return true;
}

// Parse size token for `w-*` / `h-*` (`full`, `fit`, `auto`, `a/b`, `[N%]`, `[Npx]`, `{n}`).
bool parse_cls_size(const char* rest, size_t len, uint8_t* out_kind, float* out_val) noexcept {
    if (len == 0u) return false;
    if (inlay_ui_str_eq(rest, len, "full")) {
        *out_kind = uint8_t(ETCH_SIZING_GROW);
        *out_val = 0.0f;
        return true;
    }
    if (inlay_ui_str_eq(rest, len, "fit") || inlay_ui_str_eq(rest, len, "auto")) {
        *out_kind = uint8_t(ETCH_SIZING_FIT);
        *out_val = 0.0f;
        return true;
    }
    if (rest[0] == '[') {
        float ratio = 0.0f;
        if (parse_bracket_pct(rest, len, &ratio)) {
            *out_kind = uint8_t(ETCH_SIZING_PERCENT);
            *out_val = ratio;
            return true;
        }
        float px = 0.0f;
        if (parse_bracket_px(rest, len, &px, false)) {
            *out_kind = uint8_t(ETCH_SIZING_FIXED);
            *out_val = px;
            return true;
        }
        return false;
    }
    // Check fraction `a/b` (e.g. `1/2`, `1/3`, `2/3`, `1/4`, `3/4`, `5/12`).
    for (size_t i = 1u; i + 1u < len; ++i) {
        if (rest[i] == '/') {
            uint32_t num = 0u, den = 0u;
            if (parse_uint_slice(rest, i, 100u, &num) &&
                parse_uint_slice(rest + i + 1u, len - i - 1u, 100u, &den) &&
                den > 0u && num <= den) {
                *out_kind = uint8_t(ETCH_SIZING_PERCENT);
                *out_val = float(num) / float(den);
                return true;
            }
            return false;
        }
    }
    float px = 0.0f;
    if (parse_spacing_px(rest, len, &px)) {
        *out_kind = uint8_t(ETCH_SIZING_FIXED);
        *out_val = px;
        return true;
    }
    return false;
}

bool parse_cls_limit_px(const char* rest, size_t len, float* out_px) noexcept {
    if (inlay_ui_str_eq(rest, len, "none")) { *out_px = 0.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "xs"))   { *out_px = 320.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "sm"))   { *out_px = 384.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "md"))   { *out_px = 448.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "lg"))   { *out_px = 512.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "xl"))   { *out_px = 576.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "2xl"))  { *out_px = 672.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "3xl"))  { *out_px = 768.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "4xl"))  { *out_px = 896.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "5xl"))  { *out_px = 1024.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "6xl"))  { *out_px = 1152.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "7xl"))  { *out_px = 1280.0f; return true; }
    return parse_spacing_px(rest, len, out_px);
}

// Parse color from a `cls` token where `full` is e.g. `border-subtle/50` and `rest` is `subtle/50`.
// Handles bracketed arbitrary values `[#1e293b]`, `[#1e293b]/80`, `[oklch(0.65_0.15_240)]`.
ColorRef parse_cls_color(const char* full, size_t full_len, const char* rest, size_t rest_len) noexcept {
    if (rest_len >= 3u && rest[0] == '[') {
        size_t close = 0u;
        for (size_t i = 1u; i < rest_len; ++i) {
            if (rest[i] == ']') {
                close = i;
                break;
            }
        }
        if (close > 1u) {
            const size_t inner_len = close - 1u;
            const size_t tail_len = rest_len - (close + 1u);
            if (inner_len + tail_len < 96u) {
                char buf[96];
                for (size_t i = 0u; i < inner_len; ++i) {
                    buf[i] = rest[1u + i] == '_' ? ' ' : rest[1u + i];
                }
                if (tail_len > 0u) {
                    std::memcpy(buf + inner_len, rest + close + 1u, tail_len);
                }
                return inlay_ui_parse_color(buf, inner_len + tail_len);
            }
        }
        return {};
    }
    // Try full token first so `bg-surface`, `text-disabled`, `border-subtle`, `border-input`
    // match their exact domain-specific ThemeToken rather than an ambiguous short alias.
    if (full && full_len > 0u) {
        const ColorRef exact = inlay_ui_parse_color(full, full_len);
        if (exact.set) return exact;
    }
    return inlay_ui_parse_color(rest, rest_len);
}

bool parse_cls_radius_val(const char* rest, size_t len, float* out_px, uint8_t* out_tok) noexcept {
    if (len == 0u) { *out_tok = RadControl; *out_px = 6.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "none"))    { *out_tok = RadPx; *out_px = 0.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "sm"))      { *out_tok = RadPx; *out_px = 4.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "md"))      { *out_tok = RadPx; *out_px = 6.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "lg"))      { *out_tok = RadPx; *out_px = 8.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "xl"))      { *out_tok = RadPx; *out_px = 12.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "2xl"))     { *out_tok = RadPx; *out_px = 16.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "3xl"))     { *out_tok = RadPx; *out_px = 24.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "full"))    { *out_tok = RadPill; *out_px = 9999.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "panel"))   { *out_tok = RadPanel; *out_px = 0.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "control")) { *out_tok = RadControl; *out_px = 0.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "input"))   { *out_tok = RadInput; *out_px = 0.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "small"))   { *out_tok = RadSmall; *out_px = 0.0f; return true; }
    if (inlay_ui_str_eq(rest, len, "pill"))    { *out_tok = RadPill; *out_px = 0.0f; return true; }
    float px = 0.0f;
    if (parse_bracket_px(rest, len, &px, false)) {
        *out_tok = RadPx;
        *out_px = px;
        return true;
    }
    return false;
}

bool parse_cls_scale_val(const char* rest, size_t len, float* out_scale) noexcept {
    if (len == 0u) return false;
    if (rest[0] == '[' && rest[len - 1u] == ']') {
        float v = 0.0f;
        if (!parse_float_slice(rest + 1u, len - 2u, &v)) return false;
        *out_scale = clampf(v, 0.05f, 8.0f);
        return true;
    }
    float pct = 0.0f;
    if (!parse_float_slice(rest, len, &pct) || pct < 0.0f || pct > 800.0f) return false;
    *out_scale = clampf(pct * 0.01f, 0.05f, 8.0f);
    return true;
}

bool parse_cls_opacity_val(const char* rest, size_t len, float* out_op) noexcept {
    if (len == 0u) return false;
    if (rest[0] == '[' && rest[len - 1u] == ']') {
        float v = 0.0f;
        if (!parse_float_slice(rest + 1u, len - 2u, &v)) return false;
        *out_op = clampf(v, 0.0f, 1.0f);
        return true;
    }
    float pct = 0.0f;
    if (!parse_float_slice(rest, len, &pct) || pct < 0.0f || pct > 100.0f) return false;
    *out_op = clampf(pct * 0.01f, 0.0f, 1.0f);
    return true;
}

bool parse_cls_border_side_width(const char* rest, size_t len, float* out_w) noexcept {
    if (len == 0u) { *out_w = 1.0f; return true; }
    if (rest[0] != '-') return false;
    const char* num = rest + 1u;
    const size_t nlen = len - 1u;
    if (nlen == 0u) return false;
    if (num[0] == '[') return parse_bracket_px(num, nlen, out_w, false);
    uint32_t w = 0u;
    if (!parse_uint_slice(num, nlen, 64u, &w)) return false;
    *out_w = float(w);
    return true;
}

ColdVisualStyle* ensure_transition_cold(InlaySurface* s, NodeStyle& st) {
    ColdVisualStyle* c = inlay_ui_ensure_cold(s, st);
    if (!c) return nullptr;
    if (!(c->cflags & CfHasTransition)) {
        c->transition = {};
        c->transition.prop_mask = TransAll;
        c->transition.easing = TransEaseOut;
        c->transition.duration_sec = 0.18f;
        c->cflags |= CfHasTransition;
    }
    return c;
}

uint32_t tween_sub_id(uint32_t element_id, uint16_t prop_bit) noexcept {
    uint32_t h = element_id ^ 0x54574E31u; // "TWN1"
    h = (h ^ (uint32_t(prop_bit) & 0xFFu)) * 16777619u;
    h = (h ^ ((uint32_t(prop_bit) >> 8) & 0xFFu)) * 16777619u;
    return h ? h : 1u;
}

EtchSpringSpec resolve_spring_spec(const TransitionSpec& trans) noexcept {
    if (trans.bezier[0] > 1e-4f && trans.bezier[1] > 1e-4f) {
        return {trans.bezier[0], trans.bezier[1]};
    }
    return etch_spring_spec_snappy();
}

EtchEasingSpec resolve_easing_spec(const TransitionSpec& trans) noexcept {
    switch (trans.easing) {
        case TransEaseLinear:      return etch_easing_linear();
        case TransEaseIn:          return etch_easing_in();
        case TransEaseOut:         return etch_easing_out();
        case TransEaseInOut:       return etch_easing_in_out();
        case TransEaseCubicBezier: return etch_easing_cubic_bezier(
            trans.bezier[0], trans.bezier[1], trans.bezier[2], trans.bezier[3]);
        default:                   return etch_easing_out();
    }
}

} // namespace

// ============================================================================
// 1. Color Parsing (`#hex`, `rgb`/`rgba`, `oklch`, `var(--etch-*)`, tokens, `/alpha`)
// ============================================================================

ColorRef inlay_ui_parse_color(const char* chars, size_t len) noexcept {
    if (!chars || len == 0u) return {};
    trim_slice(chars, len);
    if (len == 0u) return {};

    // Split optional trailing `/alpha` modifier when `/` is outside parentheses.
    uint8_t alpha_mul = 255u;
    int paren_depth = 0;
    size_t slash_pos = len;
    for (size_t i = 0u; i < len; ++i) {
        if (chars[i] == '(') ++paren_depth;
        else if (chars[i] == ')' && paren_depth > 0) --paren_depth;
        else if (chars[i] == '/' && paren_depth == 0) slash_pos = i;
    }
    if (slash_pos < len) {
        if (!parse_alpha_suffix(chars + slash_pos + 1u, len - slash_pos - 1u, &alpha_mul)) {
            return {};
        }
        len = slash_pos;
        trim_slice(chars, len);
        if (len == 0u) return {};
    }

    // Hex: #RGB, #RGBA, #RRGGBB, #RRGGBBAA
    if (chars[0] == '#') {
        EtchColor c = {};
        if (!parse_hex_color(chars, len, &c)) return {};
        return inlay_color_ref_rgba(c, alpha_mul);
    }

    // CSS functional notations: rgb(...), rgba(...), oklch(...), var(--etch-...)
    if (len > 5u && chars[len - 1u] == ')') {
        if (starts_with_ci(chars, len, "rgba(")) {
            EtchColor c = {};
            if (!parse_rgb_func(chars + 5u, len - 6u, &c)) return {};
            return inlay_color_ref_rgba(c, alpha_mul);
        }
        if (starts_with_ci(chars, len, "rgb(")) {
            EtchColor c = {};
            if (!parse_rgb_func(chars + 4u, len - 5u, &c)) return {};
            return inlay_color_ref_rgba(c, alpha_mul);
        }
        if (starts_with_ci(chars, len, "oklch(")) {
            EtchColor c = {};
            if (!parse_oklch_func(chars + 6u, len - 7u, &c)) return {};
            return inlay_color_ref_rgba(c, alpha_mul);
        }
        if (len > 12u && starts_with_ci(chars, len, "var(--etch-")) {
            chars += 11u;
            len -= 12u;
            trim_slice(chars, len);
        } else if (len > 7u && starts_with_ci(chars, len, "var(--")) {
            chars += 6u;
            len -= 7u;
            trim_slice(chars, len);
        }
    }

    // Named literals `white` / `black` and semantic theme tokens.
    char norm[40];
    size_t norm_len = 0u;
    if (!normalize_token_key(chars, len, norm, sizeof(norm), &norm_len)) return {};

    if (inlay_ui_str_eq(norm, norm_len, "white")) {
        return inlay_color_ref_rgba(etch_rgba8(255u, 255u, 255u, 255u), alpha_mul);
    }
    if (inlay_ui_str_eq(norm, norm_len, "black")) {
        return inlay_color_ref_rgba(etch_rgba8(0u, 0u, 0u, 255u), alpha_mul);
    }
    ThemeToken tok = TokNone;
    if (match_semantic_token(norm, norm_len, &tok)) {
        return inlay_color_ref_token(tok, alpha_mul);
    }
    return {};
}

// ============================================================================
// 2. Live Theme Color Resolution
// ============================================================================

EtchColor inlay_ui_resolve_color(ColorRef ref, const EtchTheme& theme, EtchColor fallback) noexcept {
    if (!ref.set) return fallback;

    EtchColor c = fallback;
    switch (ThemeToken(ref.token)) {
        case TokNone:               c = ref.rgba; break;
        case TokTransparent:        return etch_rgba8(0u, 0u, 0u, 0u);
        case TokBgCanvas:           c = theme.bg_canvas; break;
        case TokBgSurface:          c = theme.bg_surface; break;
        case TokBgElevated:         c = theme.bg_elevated; break;
        case TokBgChrome:           c = theme.bg_chrome; break;
        case TokBgPopup:            c = theme.bg_popup; break;
        case TokBgTooltip:          c = theme.bg_tooltip; break;
        case TokBgInput:            c = theme.bg_input; break;
        case TokBgTable:            c = theme.bg_table; break;
        case TokBgNodeCanvas:       c = theme.bg_node_canvas; break;
        case TokSurfaceControl:     c = theme.surface_control; break;
        case TokSurfaceHover:       c = theme.surface_hover; break;
        case TokSurfaceActive:      c = theme.surface_active; break;
        case TokSurfaceDisabled:    c = theme.surface_disabled; break;
        case TokSurfaceSubtle:      c = theme.surface_subtle; break;
        case TokSurfaceSubtleHover: c = theme.surface_subtle_hover; break;
        case TokSurfaceSubtleBody:  c = theme.surface_subtle_body; break;
        case TokSurfaceSelected:    c = theme.surface_selected; break;
        case TokTextPrimary:        c = theme.text_primary; break;
        case TokTextSecondary:      c = theme.text_secondary; break;
        case TokTextBody:           c = theme.text_body; break;
        case TokTextCode:           c = theme.text_code; break;
        case TokTextMuted:          c = theme.text_muted; break;
        case TokTextDisabled:       c = theme.text_disabled; break;
        case TokAccent:             c = theme.accent; break;
        case TokAccentSecondary:    c = theme.accent_secondary; break;
        case TokAccentPrimary:      c = theme.accent_primary; break;
        case TokAccentHover: {
            const bool dark = inlay_is_dark_theme(theme);
            c = etch_color_blend_over(
                theme.accent, dark ? etch_rgba8(255u, 255u, 255u, 28u) : etch_rgba8(0u, 0u, 0u, 24u));
            break;
        }
        case TokFocusRing:          c = theme.focus_ring; break;
        case TokDanger:             c = theme.danger; break;
        case TokWarning:            c = theme.warning; break;
        case TokSuccess: {
            const bool dark = inlay_is_dark_theme(theme);
            c = dark ? etch_rgb_hex(0x34D399u, 255u) : etch_rgb_hex(0x16A34Au, 255u);
            break;
        }
        case TokInfo: {
            const bool dark = inlay_is_dark_theme(theme);
            c = dark ? etch_rgb_hex(0x60A5FAu, 255u) : etch_rgb_hex(0x2563EBu, 255u);
            break;
        }
        case TokBorderSubtle:       c = theme.border_subtle; break;
        case TokBorderLine:         c = theme.border_line; break;
        case TokBorderInput:        c = theme.border_input; break;
        case TokBorderNode:         c = theme.border_node; break;
        case TokBorderControl:      c = theme.border_control; break;
        case TokBorderTooltip:      c = theme.border_tooltip; break;
        case TokSelection:          c = theme.selection; break;
        case TokSelectionHover:     c = theme.selection_hover; break;
        case TokZebraRow:           c = theme.zebra_row; break;
        case TokActiveLine:         c = theme.active_line; break;
        case TokBackdrop:           c = theme.backdrop; break;
        case TokShadow:             c = theme.shadow; break;
        case TokOnPrimary:          c = theme.on_primary; break;
        case TokOnError:            c = theme.on_error; break;
        case TokSyntaxComment:      c = theme.syntax_comment; break;
        case TokSyntaxString:       c = theme.syntax_string; break;
        case TokSyntaxNumber:       c = theme.syntax_number; break;
        case TokSyntaxKeyword:      c = theme.syntax_keyword; break;
        default:                    return fallback;
    }

    if (ref.alpha_mul < 255u) {
        c.a = uint8_t((uint32_t(c.a) * uint32_t(ref.alpha_mul) + 127u) / 255u);
    }
    return c;
}

// ============================================================================
// 3. Transition Shorthand Parser
// ============================================================================

bool inlay_ui_parse_transition(const char* chars, size_t len, TransitionSpec* out) noexcept {
    if (!out || !chars || len == 0u) return false;
    trim_slice(chars, len);
    if (len == 0u) return false;

    if (str_ieq(chars, len, "none") || str_ieq(chars, len, "off") || str_ieq(chars, len, "false")) {
        *out = {};
        out->prop_mask = TransNone;
        return true;
    }

    TransitionSpec spec = {};
    spec.easing = TransEaseOut;
    spec.duration_sec = 0.18f;

    uint16_t prop_mask = 0u;
    int time_count = 0;
    bool any_token = false;

    size_t i = 0u;
    while (i < len) {
        while (i < len && is_ascii_space(chars[i])) ++i;
        if (i >= len) break;
        const size_t start = i;
        int paren = 0;
        while (i < len && (!is_ascii_space(chars[i]) || paren > 0)) {
            if (chars[i] == '(') ++paren;
            else if (chars[i] == ')' && paren > 0) --paren;
            ++i;
        }
        const char* tok = chars + start;
        const size_t tlen = i - start;

        // Speed presets
        if (str_ieq(tok, tlen, "fast")) {
            spec.duration_sec = 0.12f;
            time_count = time_count < 1 ? 1 : time_count;
            any_token = true;
            continue;
        }
        if (str_ieq(tok, tlen, "normal") || str_ieq(tok, tlen, "default")) {
            spec.duration_sec = 0.18f;
            time_count = time_count < 1 ? 1 : time_count;
            any_token = true;
            continue;
        }
        if (str_ieq(tok, tlen, "slow")) {
            spec.duration_sec = 0.30f;
            time_count = time_count < 1 ? 1 : time_count;
            any_token = true;
            continue;
        }

        // Duration / delay (`180ms`, `0.25s`)
        float sec = 0.0f;
        if (parse_time_token(tok, tlen, &sec)) {
            if (time_count == 0) spec.duration_sec = sec;
            else if (time_count == 1) spec.delay_sec = sec;
            else return false;
            ++time_count;
            any_token = true;
            continue;
        }

        // Easing or spring preset (`ease-out`, `snappy`, `cubic-bezier(...)`, `spring(...)`)
        if (parse_transition_easing_word(tok, tlen, &spec)) {
            any_token = true;
            continue;
        }

        // Property or comma-separated property list (`all`, `colors`, `bg,border`, `transform`)
        size_t pos = 0u;
        bool prop_ok = true;
        uint16_t local_mask = 0u;
        while (pos < tlen) {
            size_t comma = pos;
            while (comma < tlen && tok[comma] != ',') ++comma;
            const char* sub = tok + pos;
            size_t slen = comma - pos;
            trim_slice(sub, slen);
            uint16_t bit = 0u;
            if (slen == 0u || !parse_transition_prop_word(sub, slen, &bit)) {
                prop_ok = false;
                break;
            }
            local_mask |= bit;
            pos = comma < tlen ? comma + 1u : tlen;
        }
        if (prop_ok && local_mask != 0u) {
            prop_mask |= local_mask;
            any_token = true;
            continue;
        }
        return false;
    }

    if (!any_token) return false;
    spec.prop_mask = prop_mask ? prop_mask : uint16_t(TransAll);
    *out = spec;
    return true;
}

// ============================================================================
// 4. Blend Mode Parser
// ============================================================================

bool inlay_ui_parse_blend_mode(const char* chars, size_t len, EtchBlendMode* out) noexcept {
    if (!out || !chars || len == 0u) return false;
    trim_slice(chars, len);
    if (len == 0u) return false;
    if (str_ieq(chars, len, "normal")) {
        *out = ETCH_BLEND_NORMAL;
        return true;
    }
    if (str_ieq(chars, len, "additive") || str_ieq(chars, len, "plus-lighter") ||
        str_ieq(chars, len, "plusLighter") || str_ieq(chars, len, "add")) {
        *out = ETCH_BLEND_ADDITIVE;
        return true;
    }
    if (str_ieq(chars, len, "multiply")) {
        *out = ETCH_BLEND_MULTIPLY;
        return true;
    }
    if (str_ieq(chars, len, "screen")) {
        *out = ETCH_BLEND_SCREEN;
        return true;
    }
    return false;
}

// ============================================================================
// 5. OpenType Feature Flags Parser
// ============================================================================

bool inlay_ui_parse_opentype_flags(const char* chars, size_t len, uint32_t* io_flags) noexcept {
    if (!io_flags || !chars || len == 0u) return false;
    trim_slice(chars, len);
    if (len == 0u) return false;

    uint32_t flags = *io_flags;
    bool matched = false;
    size_t i = 0u;
    while (i < len) {
        while (i < len && (is_ascii_space(chars[i]) || chars[i] == ',')) ++i;
        if (i >= len) break;
        const size_t start = i;
        while (i < len && !is_ascii_space(chars[i]) && chars[i] != ',') ++i;
        const char* tok = chars + start;
        size_t tlen = i - start;
        // Strip optional surrounding quotes (e.g. `'tnum'`, `"zero"`).
        if (tlen >= 2u && ((tok[0] == '\'' && tok[tlen - 1u] == '\'') ||
                           (tok[0] == '"' && tok[tlen - 1u] == '"'))) {
            ++tok;
            tlen -= 2u;
        }
        if (tlen == 0u) continue;

        if (str_ieq(tok, tlen, "none") || str_ieq(tok, tlen, "normal")) {
            flags = ETCH_OT_NONE;
            matched = true;
        } else if (str_ieq(tok, tlen, "liga")) {
            flags &= ~uint32_t(ETCH_OT_NO_LIGA);
            matched = true;
        } else if (str_ieq(tok, tlen, "no-liga") || str_ieq(tok, tlen, "noliga") ||
                   str_ieq(tok, tlen, "-liga")) {
            flags |= uint32_t(ETCH_OT_NO_LIGA);
            matched = true;
        } else if (str_ieq(tok, tlen, "tnum") || str_ieq(tok, tlen, "tabular-nums") ||
                   str_ieq(tok, tlen, "tabularNums") || str_ieq(tok, tlen, "tabular")) {
            flags |= uint32_t(ETCH_OT_TABULAR_NUMS);
            matched = true;
        } else if (str_ieq(tok, tlen, "zero") || str_ieq(tok, tlen, "slashed-zero") ||
                   str_ieq(tok, tlen, "slashedZero")) {
            flags |= uint32_t(ETCH_OT_SLASHED_ZERO);
            matched = true;
        } else if (str_ieq(tok, tlen, "smcp") || str_ieq(tok, tlen, "small-caps") ||
                   str_ieq(tok, tlen, "smallCaps")) {
            flags |= uint32_t(ETCH_OT_SMALL_CAPS);
            matched = true;
        } else if (str_ieq(tok, tlen, "onum") || str_ieq(tok, tlen, "oldstyle-nums") ||
                   str_ieq(tok, tlen, "oldstyleNums") || str_ieq(tok, tlen, "oldstyle")) {
            flags |= uint32_t(ETCH_OT_OLDSTYLE_NUMS);
            matched = true;
        } else if (str_ieq(tok, tlen, "frac") || str_ieq(tok, tlen, "fractions") ||
                   str_ieq(tok, tlen, "diagonal-fractions")) {
            flags |= uint32_t(ETCH_OT_FRACTIONS);
            matched = true;
        } else if (str_ieq(tok, tlen, "ss01")) {
            flags |= uint32_t(ETCH_OT_STYLISTIC_SET01);
            matched = true;
        } else if (str_ieq(tok, tlen, "ss02")) {
            flags |= uint32_t(ETCH_OT_STYLISTIC_SET02);
            matched = true;
        }
    }
    if (!matched) return false;
    *io_flags = flags;
    return true;
}

// ============================================================================
// 6. Alpha Mask Parser (String Shorthands & Full Object)
// ============================================================================

bool inlay_ui_parse_alpha_mask(InlaySurface* s, JSValueConst val, EtchAlphaMaskDesc* out) {
    if (!s || !s->js || !out) return false;
    EtchAlphaMaskDesc m = etch_alpha_mask_desc_default();
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        *out = m;
        return false;
    }

    if (JS_IsString(val)) {
        size_t n = 0u;
        const char* chars = JS_ToCStringLen(s->js, &n, val);
        bool ok = false;
        if (chars) {
            ok = apply_alpha_mask_preset(chars, n, &m);
            JS_FreeCString(s->js, chars);
        }
        *out = m;
        return ok && m.kind != ETCH_ALPHA_MASK_NONE;
    }

    if (!JS_IsObject(val) || JS_IsArray(val)) {
        *out = m;
        return false;
    }

    bool kind_explicit = false;
    const auto read_str_preset = [&](const char* prop_name) {
        const JSValue pv = JS_GetPropertyStr(s->js, val, prop_name);
        if (JS_IsString(pv)) {
            size_t n = 0u;
            const char* chars = JS_ToCStringLen(s->js, &n, pv);
            if (chars) {
                if (apply_alpha_mask_preset(chars, n, &m)) kind_explicit = true;
                JS_FreeCString(s->js, chars);
            }
        }
        JS_FreeValue(s->js, pv);
    };
    read_str_preset("preset");
    read_str_preset("kind");

    inlay_ui_opt_bool_prop(s, val, "invert", &m.invert);

    bool has_angle = false;
    float tmp = 0.0f;
    if (inlay_ui_opt_number_prop(s, val, "angle", &tmp, false, -36000.0, 36000.0) ||
        inlay_ui_opt_number_prop(s, val, "angleDeg", &tmp, false, -36000.0, 36000.0) ||
        inlay_ui_opt_number_prop(s, val, "angle_deg", &tmp, false, -36000.0, 36000.0)) {
        m.angle_deg = tmp;
        has_angle = true;
    }

    if (inlay_ui_opt_number_prop(s, val, "start", &tmp, false, -16.0, 16.0) ||
        inlay_ui_opt_number_prop(s, val, "startT", &tmp, false, -16.0, 16.0) ||
        inlay_ui_opt_number_prop(s, val, "start_t", &tmp, false, -16.0, 16.0)) {
        m.start_t = tmp;
    }
    if (inlay_ui_opt_number_prop(s, val, "end", &tmp, false, -16.0, 16.0) ||
        inlay_ui_opt_number_prop(s, val, "endT", &tmp, false, -16.0, 16.0) ||
        inlay_ui_opt_number_prop(s, val, "end_t", &tmp, false, -16.0, 16.0)) {
        m.end_t = tmp;
    }
    if (inlay_ui_opt_number_prop(s, val, "alphaStart", &tmp, false, 0.0, 1.0) ||
        inlay_ui_opt_number_prop(s, val, "alpha_start", &tmp, false, 0.0, 1.0)) {
        m.alpha_start = clampf(tmp, 0.0f, 1.0f);
    }
    if (inlay_ui_opt_number_prop(s, val, "alphaEnd", &tmp, false, 0.0, 1.0) ||
        inlay_ui_opt_number_prop(s, val, "alpha_end", &tmp, false, 0.0, 1.0)) {
        m.alpha_end = clampf(tmp, 0.0f, 1.0f);
    }
    if (JS_HasException(s->js)) {
        *out = etch_alpha_mask_desc_default();
        return false;
    }

    const bool has_center =
        read_js_vec2_prop(s->js, val, "center", -16.0f, 16.0f, &m.center_norm, false) ||
        read_js_vec2_prop(s->js, val, "centerNorm", -16.0f, 16.0f, &m.center_norm, false) ||
        read_js_vec2_prop(s->js, val, "center_norm", -16.0f, 16.0f, &m.center_norm, false);

    const bool has_radius_norm =
        read_js_vec2_prop(s->js, val, "radiusNorm", 0.001f, 10000.0f, &m.radius_norm, true) ||
        read_js_vec2_prop(s->js, val, "radius_norm", 0.001f, 10000.0f, &m.radius_norm, true);

    const bool has_corners =
        read_js_corners_prop(s->js, val, "corners", &m.radius) ||
        read_js_corners_prop(s->js, val, "r", &m.radius);

    // Disambiguate `radius`: rounded mask uses pixel corner radii; radial/spotlight uses normalized radii.
    if (m.kind == ETCH_ALPHA_MASK_ROUNDED) {
        read_js_corners_prop(s->js, val, "radius", &m.radius);
    } else {
        if (read_js_vec2_prop(s->js, val, "radius", 0.001f, 10000.0f, &m.radius_norm, true) && !kind_explicit) {
            m.kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
            kind_explicit = true;
        }
    }

    if (!inlay_ui_opt_bool_prop(s, val, "continuousCorners", &m.continuous_corners)) {
        if (!inlay_ui_opt_bool_prop(s, val, "continuous_corners", &m.continuous_corners)) {
            inlay_ui_opt_bool_prop(s, val, "squircle", &m.continuous_corners);
        }
    }

    // `uv`: [u0, v0, u1, v1] or { x, y, w, h }
    const JSValue uv_v = JS_GetPropertyStr(s->js, val, "uv");
    if (JS_IsArray(uv_v)) {
        float uv4[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        bool all_ok = true;
        for (uint32_t i = 0u; i < 4u; ++i) {
            const JSValue el = JS_GetPropertyUint32(s->js, uv_v, i);
            if (JS_IsUndefined(el) || !read_js_float_range(s->js, el, -16.0f, 16.0f, &uv4[i])) {
                all_ok = false;
            }
            JS_FreeValue(s->js, el);
        }
        if (all_ok) {
            m.uv = {uv4[0], uv4[1], uv4[2] - uv4[0], uv4[3] - uv4[1]};
        }
    } else if (JS_IsObject(uv_v) && !JS_IsNull(uv_v)) {
        inlay_ui_opt_number_prop(s, uv_v, "x", &m.uv.x, false, -16.0, 16.0);
        inlay_ui_opt_number_prop(s, uv_v, "y", &m.uv.y, false, -16.0, 16.0);
        inlay_ui_opt_number_prop(s, uv_v, "w", &m.uv.w, false, -16.0, 16.0);
        inlay_ui_opt_number_prop(s, uv_v, "h", &m.uv.h, false, -16.0, 16.0);
    }
    JS_FreeValue(s->js, uv_v);

    // Infer kind if omitted in object form.
    if (!kind_explicit) {
        if (has_center || has_radius_norm) {
            m.kind = ETCH_ALPHA_MASK_RADIAL_SPOTLIGHT;
        } else if (has_corners) {
            m.kind = ETCH_ALPHA_MASK_ROUNDED;
        } else if (has_angle) {
            m.kind = ETCH_ALPHA_MASK_LINEAR_GRADIENT;
        }
    }

    *out = m;
    return m.kind != ETCH_ALPHA_MASK_NONE;
}

// ============================================================================
// 7. Utility Class Engine (`cls` / `className`)
// ============================================================================

constexpr uint32_t kClsCacheSets = 128u;
constexpr uint32_t kClsCacheWays = 2u;
constexpr uint32_t kClsCacheSlots = kClsCacheSets * kClsCacheWays;
constexpr uint32_t kClsCacheStrCap = 32768u;
constexpr size_t   kClsCacheMaxStrLen = 512u;

uint64_t cls_hash64(const char* chars, size_t len) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0u; i < len; ++i) {
        h ^= static_cast<uint8_t>(chars[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

void inlay_ui_apply_cls_parse(InlaySurface* s, const char* cls, size_t cls_len,
                              InlayUiKind kind, NodeStyle& st) {
    int8_t axis_override = -1; // -1 = unchanged, 0 = row, 1 = col
    int8_t pending_items = -1; // -1 = none, 0 = start, 1 = center, 2 = end
    bool saw_gap_x = false;
    bool saw_gap_y = false;

    size_t pos = 0u;
    while (pos < cls_len) {
        while (pos < cls_len && is_ascii_space(cls[pos])) ++pos;
        if (pos >= cls_len) break;
        const size_t start = pos;
        while (pos < cls_len && !is_ascii_space(cls[pos])) ++pos;
        const char* tok = cls + start;
        size_t len = pos - start;

        // Parse optional `dark:` theme modifier (`dark:bg-*`, `dark:border-*`, `dark:text-*`).
        if (starts_with(tok, len, "dark:")) {
            tok += 5u;
            len -= 5u;
            if (len == 0u) continue;
            if (starts_with(tok, len, "bg-")) {
                const ColorRef c = parse_cls_color(tok, len, tok + 3u, len - 3u);
                if (c.set) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->dark_bg = c;
                }
            } else if (starts_with(tok, len, "border-")) {
                const ColorRef c = parse_cls_color(tok, len, tok + 7u, len - 7u);
                if (c.set) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->dark_border_color = c;
                }
            } else if (starts_with(tok, len, "text-")) {
                const ColorRef c = parse_cls_color(tok, len, tok + 5u, len - 5u);
                if (c.set) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->dark_text_color = c;
                }
            }
            continue;
        }

        // Parse optional state prefix (`hover:`, `active:`, `press:`, `focus:`, `focus-visible:`).
        ClsState state = ClsNormal;
        if (starts_with(tok, len, "hover:")) {
            state = ClsHover;
            tok += 6u;
            len -= 6u;
        } else if (starts_with(tok, len, "active:")) {
            state = ClsActive;
            tok += 7u;
            len -= 7u;
        } else if (starts_with(tok, len, "press:")) {
            state = ClsActive;
            tok += 6u;
            len -= 6u;
        } else if (starts_with(tok, len, "focus:")) {
            state = ClsFocus;
            tok += 6u;
            len -= 6u;
        } else if (starts_with(tok, len, "focus-visible:")) {
            state = ClsFocus;
            tok += 14u;
            len -= 14u;
        }
        if (len == 0u) continue;

        // ---- State-prefixed utilities (`hover:`, `active:`/`press:`, `focus:`) ----
        if (state != ClsNormal) {
            if (starts_with(tok, len, "bg-")) {
                const ColorRef c = parse_cls_color(tok, len, tok + 3u, len - 3u);
                if (c.set) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    if (state == ClsHover) cold->hover_bg = c;
                    else if (state == ClsActive) cold->press_bg = c;
                }
            } else if (starts_with(tok, len, "border-")) {
                const ColorRef c = parse_cls_color(tok, len, tok + 7u, len - 7u);
                if (c.set) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    if (state == ClsHover) cold->hover_border_color = c;
                    else if (state == ClsActive) cold->press_border_color = c;
                    else if (state == ClsFocus) cold->focus_border_color = c;
                }
            } else if (starts_with(tok, len, "text-")) {
                if (state == ClsHover) {
                    const ColorRef c = parse_cls_color(tok, len, tok + 5u, len - 5u);
                    if (c.set) {
                        ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                        if (!cold) return;
                        cold->hover_text_color = c;
                    }
                }
            } else if (state == ClsFocus && inlay_ui_str_eq(tok, len, "ring")) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->focus_ring_color = inlay_color_ref_token(TokFocusRing);
            } else if (starts_with(tok, len, "ring-")) {
                if (state == ClsFocus) {
                    const ColorRef c = parse_cls_color(nullptr, 0u, tok + 5u, len - 5u);
                    if (c.set) {
                        ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                        if (!cold) return;
                        cold->focus_ring_color = c;
                    }
                }
            } else if (starts_with(tok, len, "scale-")) {
                float sc = 1.0f;
                if (parse_cls_scale_val(tok + 6u, len - 6u, &sc)) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    if (state == ClsHover) {
                        cold->hover_scale = sc;
                        cold->cflags |= CfHasHoverScale;
                    } else if (state == ClsActive) {
                        cold->press_scale = sc;
                        cold->cflags |= CfHasPressScale;
                    }
                }
            } else if (starts_with(tok, len, "opacity-")) {
                float op = 1.0f;
                if (state == ClsHover && parse_cls_opacity_val(tok + 8u, len - 8u, &op)) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->hover_opacity = op;
                    cold->cflags |= CfHasHoverOpacity;
                }
            }
            continue;
        }

        // ---- Layout & Flex ----
        if (inlay_ui_str_eq(tok, len, "flex") || inlay_ui_str_eq(tok, len, "grid")) {
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "row") || inlay_ui_str_eq(tok, len, "flex-row")) {
            st.flags &= ~NfColumn;
            axis_override = 0;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "col") || inlay_ui_str_eq(tok, len, "flex-col")) {
            st.flags |= NfColumn;
            axis_override = 1;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "wrap") || inlay_ui_str_eq(tok, len, "flex-wrap")) {
            st.flags |= (NfHasWrap | NfWrapFlag);
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "nowrap") || inlay_ui_str_eq(tok, len, "flex-nowrap") ||
            inlay_ui_str_eq(tok, len, "whitespace-nowrap") || inlay_ui_str_eq(tok, len, "text-nowrap")) {
            st.flags |= NfHasWrap;
            st.flags &= ~NfWrapFlag;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "grow") || inlay_ui_str_eq(tok, len, "flex-1")) {
            st.flex_grow = 1.0f;
            st.flags |= NfHasFlexGrow;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "grow-0")) {
            st.flex_grow = 0.0f;
            st.flags |= NfHasFlexGrow;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "shrink-0")) {
            st.flex_shrink = 0.0f;
            st.flags |= NfHasFlexShrink;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "shrink")) {
            st.flex_shrink = 1.0f;
            st.flags |= NfHasFlexShrink;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "flex-none")) {
            st.flex_grow = 0.0f;
            st.flex_shrink = 0.0f;
            st.flags |= (NfHasFlexGrow | NfHasFlexShrink);
            continue;
        }

        // ---- Sizing (`w-*`, `h-*`, `size-*`, `min-*`, `max-*`, `grid-cols-*`, `col-span-*`, `row-span-*`) ----
        if (starts_with(tok, len, "w-")) {
            uint8_t k = 0u;
            float v = 0.0f;
            if (parse_cls_size(tok + 2u, len - 2u, &k, &v)) {
                st.w_kind = k;
                st.w_val = v;
                st.flags2 |= Nf2WExplicit;
            }
            continue;
        }
        if (starts_with(tok, len, "h-")) {
            uint8_t k = 0u;
            float v = 0.0f;
            if (parse_cls_size(tok + 2u, len - 2u, &k, &v)) {
                st.h_kind = k;
                st.h_val = v;
                st.flags2 |= Nf2HExplicit;
            }
            continue;
        }
        if (starts_with(tok, len, "size-")) {
            uint8_t k = 0u;
            float v = 0.0f;
            if (parse_cls_size(tok + 5u, len - 5u, &k, &v)) {
                st.w_kind = k;
                st.w_val = v;
                st.h_kind = k;
                st.h_val = v;
                st.flags2 |= (Nf2WExplicit | Nf2HExplicit);
            }
            continue;
        }
        if (starts_with(tok, len, "min-w-")) {
            if (inlay_ui_str_eq(tok + 6u, len - 6u, "0")) {
                st.flags |= NfShrinkWidth;
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->w_min = 0.0f;
            } else {
                float px = 0.0f;
                if (parse_cls_limit_px(tok + 6u, len - 6u, &px)) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->w_min = px;
                }
            }
            continue;
        }
        if (starts_with(tok, len, "max-w-")) {
            float px = 0.0f;
            if (parse_cls_limit_px(tok + 6u, len - 6u, &px)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->w_max = px;
            }
            continue;
        }
        if (starts_with(tok, len, "min-h-")) {
            if (inlay_ui_str_eq(tok + 6u, len - 6u, "0")) {
                st.flags |= NfShrinkHeight;
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->h_min = 0.0f;
            } else {
                float px = 0.0f;
                if (parse_cls_limit_px(tok + 6u, len - 6u, &px)) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->h_min = px;
                }
            }
            continue;
        }
        if (starts_with(tok, len, "max-h-")) {
            float px = 0.0f;
            if (parse_cls_limit_px(tok + 6u, len - 6u, &px)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->h_max = px;
            }
            continue;
        }
        if (starts_with(tok, len, "grid-cols-")) {
            uint32_t cols = 0u;
            if (parse_uint_slice(tok + 10u, len - 10u, 128u, &cols) && cols >= 1u) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->grid_cols = uint16_t(cols);
            }
            continue;
        }
        if (starts_with(tok, len, "grid-rows-")) {
            uint32_t rows = 0u;
            if (parse_uint_slice(tok + 10u, len - 10u, 128u, &rows) && rows >= 1u) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->grid_rows = uint16_t(rows);
            }
            continue;
        }
        if (starts_with(tok, len, "col-span-")) {
            uint32_t span = 0u;
            if (parse_uint_slice(tok + 9u, len - 9u, 128u, &span) && span >= 1u) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->col_span = uint16_t(span);
            }
            continue;
        }
        if (starts_with(tok, len, "row-span-")) {
            uint32_t span = 0u;
            if (parse_uint_slice(tok + 9u, len - 9u, 128u, &span) && span >= 1u) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->row_span = uint16_t(span);
            }
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "aspect-square") || inlay_ui_str_eq(tok, len, "aspect-video")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            cold->aspect_ratio = (tok[7] == 's') ? 1.0f : (16.0f / 9.0f);
            cold->cflags |= CfHasAspectRatio;
            continue;
        }

        // ---- Alignment ----
        if (inlay_ui_str_eq(tok, len, "items-start"))  { pending_items = 0; continue; }
        if (inlay_ui_str_eq(tok, len, "items-center")) { pending_items = 1; continue; }
        if (inlay_ui_str_eq(tok, len, "items-end"))    { pending_items = 2; continue; }
        if (inlay_ui_str_eq(tok, len, "place-items-center")) {
            st.align_x = uint8_t(ETCH_ALIGN_X_CENTER);
            st.align_y = uint8_t(ETCH_ALIGN_Y_CENTER);
            st.flags |= (NfHasAlignX | NfHasAlignY);
            continue;
        }
        if (starts_with(tok, len, "justify-")) {
            const char* r = tok + 8u;
            const size_t rl = len - 8u;
            if (inlay_ui_str_eq(r, rl, "start"))        { st.justify = uint8_t(ETCH_JUSTIFY_START); st.flags |= NfHasJustify; }
            else if (inlay_ui_str_eq(r, rl, "center"))  { st.justify = uint8_t(ETCH_JUSTIFY_CENTER); st.flags |= NfHasJustify; }
            else if (inlay_ui_str_eq(r, rl, "end"))     { st.justify = uint8_t(ETCH_JUSTIFY_END); st.flags |= NfHasJustify; }
            else if (inlay_ui_str_eq(r, rl, "between")) { st.justify = uint8_t(ETCH_JUSTIFY_SPACE_BETWEEN); st.flags |= NfHasJustify; }
            else if (inlay_ui_str_eq(r, rl, "around"))  { st.justify = uint8_t(ETCH_JUSTIFY_SPACE_AROUND); st.flags |= NfHasJustify; }
            else if (inlay_ui_str_eq(r, rl, "evenly"))  { st.justify = uint8_t(ETCH_JUSTIFY_SPACE_EVENLY); st.flags |= NfHasJustify; }
            continue;
        }
        if (starts_with(tok, len, "self-")) {
            const char* r = tok + 5u;
            const size_t rl = len - 5u;
            if (inlay_ui_str_eq(r, rl, "auto"))         { st.align_self = uint8_t(ETCH_ALIGN_SELF_AUTO); st.flags |= NfHasAlignSelf; }
            else if (inlay_ui_str_eq(r, rl, "start"))   { st.align_self = uint8_t(ETCH_ALIGN_SELF_START); st.flags |= NfHasAlignSelf; }
            else if (inlay_ui_str_eq(r, rl, "center"))  { st.align_self = uint8_t(ETCH_ALIGN_SELF_CENTER); st.flags |= NfHasAlignSelf; }
            else if (inlay_ui_str_eq(r, rl, "end"))     { st.align_self = uint8_t(ETCH_ALIGN_SELF_END); st.flags |= NfHasAlignSelf; }
            else if (inlay_ui_str_eq(r, rl, "stretch")) { st.align_self = uint8_t(ETCH_ALIGN_SELF_STRETCH); st.flags |= NfHasAlignSelf; }
            continue;
        }

        // ---- Padding (`p/px/py/pt/pr/pb/pl-*`) ----
        if (len >= 3u && tok[0] == 'p' && (tok[1] == '-' || (len >= 4u && tok[2] == '-'))) {
            float px = 0.0f;
            if (tok[1] == '-' && parse_spacing_px(tok + 2u, len - 2u, &px)) {
                st.padding = {px, px, px, px};
                st.flags |= NfHasPadding;
                continue;
            }
            if (len >= 4u && tok[2] == '-' && parse_spacing_px(tok + 3u, len - 3u, &px)) {
                if (tok[1] == 'x')      { st.padding.left = st.padding.right = px; st.flags |= NfHasPadding; continue; }
                else if (tok[1] == 'y') { st.padding.top = st.padding.bottom = px; st.flags |= NfHasPadding; continue; }
                else if (tok[1] == 't') { st.padding.top = px; st.flags |= NfHasPadding; continue; }
                else if (tok[1] == 'r') { st.padding.right = px; st.flags |= NfHasPadding; continue; }
                else if (tok[1] == 'b') { st.padding.bottom = px; st.flags |= NfHasPadding; continue; }
                else if (tok[1] == 'l') { st.padding.left = px; st.flags |= NfHasPadding; continue; }
            }
        }

        // ---- Margin (`m/mx/my/mt/mr/mb/ml-*` and negative `-m*`) ----
        {
            const bool neg = (len > 3u && tok[0] == '-' && tok[1] == 'm');
            const char* mt = neg ? (tok + 1u) : tok;
            const size_t ml = neg ? (len - 1u) : len;
            if (ml >= 3u && mt[0] == 'm' && (mt[1] == '-' || (ml >= 4u && mt[2] == '-'))) {
                float px = 0.0f;
                bool ok_m = false;
                char side = 0;
                if (mt[1] == '-' && parse_spacing_px(mt + 2u, ml - 2u, &px)) {
                    side = 'a';
                    ok_m = true;
                } else if (ml >= 4u && mt[2] == '-' &&
                           (mt[1] == 'x' || mt[1] == 'y' || mt[1] == 't' || mt[1] == 'r' || mt[1] == 'b' || mt[1] == 'l') &&
                           parse_spacing_px(mt + 3u, ml - 3u, &px)) {
                    side = mt[1];
                    ok_m = true;
                }
                if (ok_m) {
                    if (neg) px = -px;
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    if (side == 'a')      cold->margin = {px, px, px, px};
                    else if (side == 'x') cold->margin.left = cold->margin.right = px;
                    else if (side == 'y') cold->margin.top = cold->margin.bottom = px;
                    else if (side == 't') cold->margin.top = px;
                    else if (side == 'r') cold->margin.right = px;
                    else if (side == 'b') cold->margin.bottom = px;
                    else if (side == 'l') cold->margin.left = px;
                    cold->cflags |= CfHasMargin;
                    continue;
                }
            }
        }

        // ---- Gap (`gap-x-*`, `gap-y-*`, `gap-*`) ----
        if (starts_with(tok, len, "gap-x-")) {
            float px = 0.0f;
            if (parse_spacing_px(tok + 6u, len - 6u, &px)) {
                st.gap = px;
                st.flags |= NfHasGap;
                saw_gap_x = true;
            }
            continue;
        }
        if (starts_with(tok, len, "gap-y-")) {
            float px = 0.0f;
            if (parse_spacing_px(tok + 6u, len - 6u, &px)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->row_gap = px;
                cold->cflags |= CfHasRowGap;
                saw_gap_y = true;
            }
            continue;
        }
        if (starts_with(tok, len, "gap-")) {
            float px = 0.0f;
            if (parse_spacing_px(tok + 4u, len - 4u, &px)) {
                st.gap = px;
                st.flags |= NfHasGap;
                saw_gap_x = true;
            }
            continue;
        }

        // ---- Background (`bg-*`) ----
        if (starts_with(tok, len, "bg-")) {
            const ColorRef c = parse_cls_color(tok, len, tok + 3u, len - 3u);
            if (c.set) st.bg = c;
            continue;
        }

        // ---- Text alignment, font size & text color (`text-*`) ----
        if (starts_with(tok, len, "text-")) {
            const char* r = tok + 5u;
            const size_t rl = len - 5u;
            if (inlay_ui_str_eq(r, rl, "left"))   { st.align_x = uint8_t(ETCH_ALIGN_X_LEFT); st.flags |= NfHasAlignX; continue; }
            if (inlay_ui_str_eq(r, rl, "center")) { st.align_x = uint8_t(ETCH_ALIGN_X_CENTER); st.flags |= NfHasAlignX; continue; }
            if (inlay_ui_str_eq(r, rl, "right"))  { st.align_x = uint8_t(ETCH_ALIGN_X_RIGHT); st.flags |= NfHasAlignX; continue; }
            if (inlay_ui_str_eq(r, rl, "2xs"))    { st.font_size = 10.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "xs"))     { st.font_size = 12.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "sm"))     { st.font_size = 13.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "base"))   { st.font_size = 15.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "lg"))     { st.font_size = 18.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "xl"))     { st.font_size = 20.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "2xl"))    { st.font_size = 24.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "3xl"))    { st.font_size = 30.0f; st.flags |= NfHasSize; continue; }
            if (inlay_ui_str_eq(r, rl, "4xl"))    { st.font_size = 36.0f; st.flags |= NfHasSize; continue; }
            float fpx = 0.0f;
            if (rl >= 3u && r[0] == '[' && parse_bracket_px(r, rl, &fpx, false) && fpx > 0.0f) {
                st.font_size = fpx;
                st.flags |= NfHasSize;
                continue;
            }
            const ColorRef c = parse_cls_color(tok, len, r, rl);
            if (c.set) st.color = c;
            continue;
        }

        // ---- Border width, sides & color (`border`, `border-*`) ----
        if (inlay_ui_str_eq(tok, len, "border")) {
            st.border_width = 1.0f;
            st.flags |= NfHasBorderWidth;
            continue;
        }
        if (starts_with(tok, len, "border-")) {
            const char* r = tok + 7u;
            const size_t rl = len - 7u;
            uint32_t bw = 0u;
            float bpx = 0.0f;
            if (parse_uint_slice(r, rl, 64u, &bw)) {
                st.border_width = float(bw);
                st.flags |= NfHasBorderWidth;
                continue;
            }
            if (rl >= 3u && r[0] == '[' && parse_bracket_px(r, rl, &bpx, false)) {
                st.border_width = bpx;
                st.flags |= NfHasBorderWidth;
                continue;
            }
            // Side-specific widths (`border-t`, `border-r`, `border-b`, `border-l`, `border-x`, `border-y`, optional `-N`).
            if (rl >= 1u && (r[0] == 't' || r[0] == 'r' || r[0] == 'b' || r[0] == 'l' || r[0] == 'x' || r[0] == 'y')) {
                float sw = 1.0f;
                if (parse_cls_border_side_width(r + 1u, rl - 1u, &sw)) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    if (r[0] == 't') cold->border_sides.top = sw;
                    else if (r[0] == 'r') cold->border_sides.right = sw;
                    else if (r[0] == 'b') cold->border_sides.bottom = sw;
                    else if (r[0] == 'l') cold->border_sides.left = sw;
                    else if (r[0] == 'x') cold->border_sides.left = cold->border_sides.right = sw;
                    else if (r[0] == 'y') cold->border_sides.top = cold->border_sides.bottom = sw;
                    cold->cflags |= CfHasBorderSides;
                    continue;
                }
            }
            const ColorRef c = parse_cls_color(tok, len, r, rl);
            if (c.set) st.border_color = c;
            continue;
        }

        // ---- Focus Ring (`ring`, `ring-*`) ----
        if (inlay_ui_str_eq(tok, len, "ring")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            cold->focus_ring_color = inlay_color_ref_token(TokFocusRing);
            continue;
        }
        if (starts_with(tok, len, "ring-")) {
            const ColorRef c = parse_cls_color(nullptr, 0u, tok + 5u, len - 5u);
            if (c.set) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->focus_ring_color = c;
            }
            continue;
        }

        // ---- Radii & Squircle (`rounded`, `rounded-*`, `squircle`) ----
        if (inlay_ui_str_eq(tok, len, "rounded")) {
            st.radius_tok = RadControl;
            st.radius_px = 6.0f;
            st.flags |= NfRadiusSet;
            if (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count) {
                s->cold_visuals[st.cold_idx].cflags &= ~CfPerCornerRadius;
            }
            continue;
        }
        if (starts_with(tok, len, "rounded-")) {
            const char* r = tok + 8u;
            const size_t rl = len - 8u;
            // Check per-corner / per-side prefixes (`tl`, `tr`, `br`, `bl`, `t`, `r`, `b`, `l`).
            uint8_t mask = 0u; // bit 0=tl, 1=tr, 2=br, 3=bl
            const char* val_s = nullptr;
            size_t val_l = 0u;
            if (inlay_ui_str_eq(r, rl, "tl") || starts_with(r, rl, "tl-"))      { mask = 1u;  val_s = rl > 2u ? r + 3u : r + 2u; val_l = rl > 2u ? rl - 3u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "tr") || starts_with(r, rl, "tr-")) { mask = 2u;  val_s = rl > 2u ? r + 3u : r + 2u; val_l = rl > 2u ? rl - 3u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "br") || starts_with(r, rl, "br-")) { mask = 4u;  val_s = rl > 2u ? r + 3u : r + 2u; val_l = rl > 2u ? rl - 3u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "bl") || starts_with(r, rl, "bl-")) { mask = 8u;  val_s = rl > 2u ? r + 3u : r + 2u; val_l = rl > 2u ? rl - 3u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "t") || starts_with(r, rl, "t-"))   { mask = 3u;  val_s = rl > 1u ? r + 2u : r + 1u; val_l = rl > 1u ? rl - 2u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "r") || starts_with(r, rl, "r-"))   { mask = 6u;  val_s = rl > 1u ? r + 2u : r + 1u; val_l = rl > 1u ? rl - 2u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "b") || starts_with(r, rl, "b-"))   { mask = 12u; val_s = rl > 1u ? r + 2u : r + 1u; val_l = rl > 1u ? rl - 2u : 0u; }
            else if (inlay_ui_str_eq(r, rl, "l") || starts_with(r, rl, "l-"))   { mask = 9u;  val_s = rl > 1u ? r + 2u : r + 1u; val_l = rl > 1u ? rl - 2u : 0u; }

            if (mask != 0u) {
                float rpx = 0.0f;
                uint8_t rtok = RadPx;
                if (parse_cls_radius_val(val_s, val_l, &rpx, &rtok)) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    if (!(cold->cflags & CfPerCornerRadius) && (st.flags & NfRadiusSet)) {
                        cold->radius_vals = etch_corners_all(st.radius_px);
                        cold->radius_tok[0] = cold->radius_tok[1] = cold->radius_tok[2] = cold->radius_tok[3] = st.radius_tok;
                    }
                    if (mask & 1u) { cold->radius_vals.tl = rpx; cold->radius_tok[0] = rtok; }
                    if (mask & 2u) { cold->radius_vals.tr = rpx; cold->radius_tok[1] = rtok; }
                    if (mask & 4u) { cold->radius_vals.br = rpx; cold->radius_tok[2] = rtok; }
                    if (mask & 8u) { cold->radius_vals.bl = rpx; cold->radius_tok[3] = rtok; }
                    cold->cflags |= CfPerCornerRadius;
                    st.flags |= NfRadiusSet;
                }
                continue;
            }
            float rpx = 0.0f;
            uint8_t rtok = RadPx;
            if (parse_cls_radius_val(r, rl, &rpx, &rtok)) {
                st.radius_px = rpx;
                st.radius_tok = rtok;
                st.flags |= NfRadiusSet;
                if (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count) {
                    s->cold_visuals[st.cold_idx].cflags &= ~CfPerCornerRadius;
                }
            }
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "squircle")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            cold->cflags |= (CfHasContinuousCorners | CfContinuousCorners);
            continue;
        }

        // ---- Shadows & Elevation (`shadow`, `shadow-*`) ----
        if (inlay_ui_str_eq(tok, len, "shadow")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            cold->elevation = ETCH_ELEVATION_SM;
            cold->cflags |= CfHasElevation;
            continue;
        }
        if (starts_with(tok, len, "shadow-")) {
            const char* r = tok + 7u;
            const size_t rl = len - 7u;
            if (inlay_ui_str_eq(r, rl, "inset") || inlay_ui_str_eq(r, rl, "inner")) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->cflags |= CfInsetShadowBool;
            } else {
                EtchElevation elev = ETCH_ELEVATION_NONE;
                bool matched_e = true;
                if (inlay_ui_str_eq(r, rl, "none"))      elev = ETCH_ELEVATION_NONE;
                else if (inlay_ui_str_eq(r, rl, "sm"))   elev = ETCH_ELEVATION_SM;
                else if (inlay_ui_str_eq(r, rl, "md"))   elev = ETCH_ELEVATION_MD;
                else if (inlay_ui_str_eq(r, rl, "lg"))   elev = ETCH_ELEVATION_LG;
                else if (inlay_ui_str_eq(r, rl, "xl"))   elev = ETCH_ELEVATION_XL;
                else matched_e = false;
                if (matched_e) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    cold->elevation = elev;
                    cold->cflags |= CfHasElevation;
                }
            }
            continue;
        }

        // ---- Backdrop Vibrancy Blur (`blur-*`, `backdrop-blur-*`) ----
        if (inlay_ui_str_eq(tok, len, "blur") || inlay_ui_str_eq(tok, len, "backdrop-blur") ||
            starts_with(tok, len, "blur-") || starts_with(tok, len, "backdrop-blur-")) {
            const char* r = nullptr;
            size_t rl = 0u;
            if (starts_with(tok, len, "backdrop-blur-")) { r = tok + 14u; rl = len - 14u; }
            else if (starts_with(tok, len, "blur-"))     { r = tok + 5u; rl = len - 5u; }
            float blur_px = -1.0f;
            if (!r)                                  blur_px = 16.0f;
            else if (inlay_ui_str_eq(r, rl, "none")) blur_px = 0.0f;
            else if (inlay_ui_str_eq(r, rl, "sm"))   blur_px = 8.0f;
            else if (inlay_ui_str_eq(r, rl, "md"))   blur_px = 16.0f;
            else if (inlay_ui_str_eq(r, rl, "lg"))   blur_px = 24.0f;
            else if (inlay_ui_str_eq(r, rl, "xl"))   blur_px = 36.0f;
            else if (inlay_ui_str_eq(r, rl, "2xl"))  blur_px = 48.0f;
            else {
                float bp = 0.0f;
                if (parse_bracket_px(r, rl, &bp, false)) blur_px = clampf(bp, 0.0f, 128.0f);
            }
            if (blur_px >= 0.0f) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->backdrop_blur = blur_px;
            }
            continue;
        }

        // ---- Specular Rim Highlight (`rim`, `rim-*`) ----
        if (inlay_ui_str_eq(tok, len, "rim")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            cold->top_rim_alpha = 0.14f;
            cold->cflags |= CfHasTopRimAlpha;
            continue;
        }
        if (starts_with(tok, len, "rim-")) {
            const char* r = tok + 4u;
            const size_t rl = len - 4u;
            float alpha = -1.0f;
            if (inlay_ui_str_eq(r, rl, "none")) {
                alpha = 0.0f;
            } else {
                parse_cls_opacity_val(r, rl, &alpha);
            }
            if (alpha >= 0.0f) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->top_rim_alpha = alpha;
                cold->cflags |= CfHasTopRimAlpha;
            }
            continue;
        }

        // ---- Opacity (`opacity-*`) ----
        if (starts_with(tok, len, "opacity-")) {
            float op = 1.0f;
            if (parse_cls_opacity_val(tok + 8u, len - 8u, &op)) {
                st.opacity = op;
                st.flags |= NfHasOpacity;
            }
            continue;
        }

        // ---- Overflow, Scrolling, Pointer & Z-Index ----
        if (inlay_ui_str_eq(tok, len, "overflow-hidden")) {
            st.flags |= (NfHasClipChildren | NfClipChildren);
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "overflow-visible")) {
            st.flags |= NfHasClipChildren;
            st.flags &= ~NfClipChildren;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "scroll-y") || inlay_ui_str_eq(tok, len, "overflow-y-auto") ||
            inlay_ui_str_eq(tok, len, "overflow-y-scroll")) {
            st.flags |= (NfHasScrollY | NfScrollY);
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "scroll-x") || inlay_ui_str_eq(tok, len, "overflow-x-auto") ||
            inlay_ui_str_eq(tok, len, "overflow-x-scroll")) {
            st.flags |= (NfHasScrollX | NfScrollX);
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "scroll-both") || inlay_ui_str_eq(tok, len, "overflow-auto")) {
            st.flags |= (NfHasScrollX | NfScrollX | NfHasScrollY | NfScrollY);
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "pointer-events-none")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            cold->cflags |= CfPointerPassthrough;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "pointer-events-auto")) {
            if (st.cold_idx != INLAY_UI_NONE && st.cold_idx < s->cold_visual_count) {
                s->cold_visuals[st.cold_idx].cflags &= ~CfPointerPassthrough;
            }
            continue;
        }
        if (starts_with(tok, len, "z-") || starts_with(tok, len, "-z-")) {
            const bool neg = (tok[0] == '-');
            uint32_t zv = 0u;
            if (parse_uint_slice(tok + (neg ? 3u : 2u), len - (neg ? 3u : 2u), 32767u, &zv)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->z_index = neg ? int16_t(-int32_t(zv)) : int16_t(zv);
                cold->cflags |= CfHasZIndex;
            }
            continue;
        }

        // ---- Cursor (`cursor-*`) ----
        if (starts_with(tok, len, "cursor-")) {
            const char* r = tok + 7u;
            const size_t rl = len - 7u;
            EtchCursor cur = ETCH_CURSOR_ARROW;
            bool ok_cur = true;
            if (inlay_ui_str_eq(r, rl, "default") || inlay_ui_str_eq(r, rl, "auto")) cur = ETCH_CURSOR_ARROW;
            else if (inlay_ui_str_eq(r, rl, "pointer")) cur = ETCH_CURSOR_HAND;
            else if (inlay_ui_str_eq(r, rl, "text")) cur = ETCH_CURSOR_IBEAM;
            else if (inlay_ui_str_eq(r, rl, "move") || inlay_ui_str_eq(r, rl, "grab") ||
                     inlay_ui_str_eq(r, rl, "grabbing")) cur = ETCH_CURSOR_MOVE;
            else if (inlay_ui_str_eq(r, rl, "not-allowed")) cur = ETCH_CURSOR_NOT_ALLOWED;
            else if (inlay_ui_str_eq(r, rl, "crosshair")) cur = ETCH_CURSOR_CROSSHAIR;
            else if (inlay_ui_str_eq(r, rl, "ew-resize") || inlay_ui_str_eq(r, rl, "col-resize")) cur = ETCH_CURSOR_RESIZE_EW;
            else if (inlay_ui_str_eq(r, rl, "ns-resize") || inlay_ui_str_eq(r, rl, "row-resize")) cur = ETCH_CURSOR_RESIZE_NS;
            else if (inlay_ui_str_eq(r, rl, "nwse-resize")) cur = ETCH_CURSOR_RESIZE_NWSE;
            else if (inlay_ui_str_eq(r, rl, "nesw-resize")) cur = ETCH_CURSOR_RESIZE_NESW;
            else ok_cur = false;
            if (ok_cur) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->cursor = cur;
                cold->cflags |= CfHasCursor;
            }
            continue;
        }

        // ---- Blend Modes (`blend-*`, `mix-blend-*`) ----
        if (starts_with(tok, len, "blend-") || starts_with(tok, len, "mix-blend-")) {
            const size_t off = tok[0] == 'm' ? 10u : 6u;
            EtchBlendMode bm = ETCH_BLEND_NORMAL;
            if (inlay_ui_parse_blend_mode(tok + off, len - off, &bm)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->blend_mode = bm;
                cold->cflags |= CfHasBlendMode;
            }
            continue;
        }

        // ---- Typography & OpenType Features ----
        if (starts_with(tok, len, "font-")) {
            const char* r = tok + 5u;
            const size_t rl = len - 5u;
            if (inlay_ui_str_eq(r, rl, "normal") || inlay_ui_str_eq(r, rl, "regular")) {
                st.font_id = ETCH_FONT_SANS; st.flags |= NfHasFontId;
            } else if (inlay_ui_str_eq(r, rl, "medium")) {
                st.font_id = ETCH_FONT_MEDIUM; st.flags |= NfHasFontId;
            } else if (inlay_ui_str_eq(r, rl, "semibold")) {
                st.font_id = ETCH_FONT_SEMIBOLD; st.flags |= NfHasFontId;
            } else if (inlay_ui_str_eq(r, rl, "bold")) {
                st.font_id = ETCH_FONT_BOLD; st.flags |= NfHasFontId;
            } else if (inlay_ui_str_eq(r, rl, "mono")) {
                st.font_id = ETCH_FONT_MONO; st.flags |= NfHasFontId;
            } else if (inlay_ui_str_eq(r, rl, "italic")) {
                st.font_id = ETCH_FONT_ITALIC; st.flags |= NfHasFontId;
            }
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "italic")) {
            st.font_id = ETCH_FONT_ITALIC;
            st.flags |= NfHasFontId;
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "tabular-nums") || inlay_ui_str_eq(tok, len, "tnum") ||
            inlay_ui_str_eq(tok, len, "no-liga") || inlay_ui_str_eq(tok, len, "slashed-zero") ||
            inlay_ui_str_eq(tok, len, "small-caps") || inlay_ui_str_eq(tok, len, "smcp") ||
            inlay_ui_str_eq(tok, len, "oldstyle-nums") || inlay_ui_str_eq(tok, len, "onum") ||
            inlay_ui_str_eq(tok, len, "diagonal-fractions") || inlay_ui_str_eq(tok, len, "frac") ||
            inlay_ui_str_eq(tok, len, "ss01") || inlay_ui_str_eq(tok, len, "ss02")) {
            ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
            if (!cold) return;
            if (inlay_ui_parse_opentype_flags(tok, len, &cold->opentype_flags)) {
                cold->cflags |= CfHasOpenType;
            }
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "truncate")) {
            st.flags |= NfEllipsis;
            st.max_lines = 1u;
            continue;
        }
        if (starts_with(tok, len, "line-clamp-")) {
            const char* r = tok + 11u;
            const size_t rl = len - 11u;
            if (inlay_ui_str_eq(r, rl, "none")) {
                st.flags &= ~NfEllipsis;
                st.max_lines = 0u;
            } else {
                uint32_t lines = 0u;
                if (parse_uint_slice(r, rl, 64u, &lines) && lines >= 1u) {
                    st.flags |= NfEllipsis;
                    st.max_lines = uint16_t(lines);
                }
            }
            continue;
        }
        if (starts_with(tok, len, "tracking-")) {
            const char* r = tok + 9u;
            const size_t rl = len - 9u;
            float ls = 0.0f;
            bool ok_ls = true;
            if (inlay_ui_str_eq(r, rl, "tighter"))      ls = -0.6f;
            else if (inlay_ui_str_eq(r, rl, "tight"))   ls = -0.3f;
            else if (inlay_ui_str_eq(r, rl, "normal"))  ls = 0.0f;
            else if (inlay_ui_str_eq(r, rl, "wide"))    ls = 0.4f;
            else if (inlay_ui_str_eq(r, rl, "wider"))   ls = 0.8f;
            else if (inlay_ui_str_eq(r, rl, "widest"))  ls = 1.4f;
            else ok_ls = false;
            if (ok_ls) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->letter_spacing = ls;
                cold->cflags |= CfHasLetterSpacing;
            }
            continue;
        }
        if (starts_with(tok, len, "leading-")) {
            float lh = 0.0f;
            if (parse_bracket_px(tok + 8u, len - 8u, &lh, false) && lh > 0.0f) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->line_height = lh;
                cold->cflags |= CfHasLineHeight;
            }
            continue;
        }

        // ---- 2D Transforms (`scale-*`, `rotate-*`, `-rotate-*`, `translate-x/y-*`, `-translate-*`) ----
        if (starts_with(tok, len, "scale-x-")) {
            float sc = 1.0f;
            if (parse_cls_scale_val(tok + 8u, len - 8u, &sc)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->transform.sx = sc;
                cold->cflags |= CfHasTransform;
            }
            continue;
        }
        if (starts_with(tok, len, "scale-y-")) {
            float sc = 1.0f;
            if (parse_cls_scale_val(tok + 8u, len - 8u, &sc)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->transform.sy = sc;
                cold->cflags |= CfHasTransform;
            }
            continue;
        }
        if (starts_with(tok, len, "scale-")) {
            float sc = 1.0f;
            if (parse_cls_scale_val(tok + 6u, len - 6u, &sc)) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->transform.sx = sc;
                cold->transform.sy = sc;
                cold->cflags |= CfHasTransform;
            }
            continue;
        }
        if (starts_with(tok, len, "rotate-") || starts_with(tok, len, "-rotate-")) {
            const bool neg = (tok[0] == '-');
            const char* r = tok + (neg ? 8u : 7u);
            const size_t rl = len - (neg ? 8u : 7u);
            float deg = 0.0f;
            bool ok_rot = false;
            if (rl >= 3u && r[0] == '[' && r[rl - 1u] == ']') {
                ok_rot = parse_angle_deg_slice(r + 1u, rl - 2u, &deg);
            } else {
                ok_rot = parse_float_slice(r, rl, &deg);
            }
            if (ok_rot) {
                if (neg) deg = -deg;
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->transform.rotation_rad = deg * INLAY_UI_DEG_TO_RAD;
                cold->cflags |= CfHasTransform;
            }
            continue;
        }
        if (starts_with(tok, len, "translate-x-") || starts_with(tok, len, "-translate-x-") ||
            starts_with(tok, len, "translate-y-") || starts_with(tok, len, "-translate-y-")) {
            const bool neg = (tok[0] == '-');
            const size_t off = neg ? 13u : 12u;
            const char axis_c = tok[neg ? 11u : 10u];
            float px = 0.0f;
            if (parse_spacing_px(tok + off, len - off, &px)) {
                if (neg) px = -px;
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                if (axis_c == 'x') cold->transform.tx = px;
                else cold->transform.ty = px;
                cold->cflags |= CfHasTransform;
            }
            continue;
        }

        // ---- Motion & Transitions (`transition*`, `duration-*`, `delay-*`, `ease-*`, `animate-layout`) ----
        if (inlay_ui_str_eq(tok, len, "transition")) {
            ColdVisualStyle* cold = ensure_transition_cold(s, st);
            if (!cold) return;
            cold->transition.prop_mask = TransAll;
            continue;
        }
        if (starts_with(tok, len, "transition-")) {
            const char* r = tok + 11u;
            const size_t rl = len - 11u;
            if (inlay_ui_str_eq(r, rl, "none")) {
                ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                if (!cold) return;
                cold->transition = {};
                cold->transition.prop_mask = TransNone;
                cold->cflags |= CfHasTransition;
            } else {
                uint16_t mask = 0u;
                if (parse_transition_prop_word(r, rl, &mask)) {
                    ColdVisualStyle* cold = ensure_transition_cold(s, st);
                    if (!cold) return;
                    cold->transition.prop_mask = mask;
                    if (mask & TransLayout) st.flags |= NfAnimateLayout;
                }
            }
            continue;
        }
        if (starts_with(tok, len, "duration-")) {
            const char* r = tok + 9u;
            const size_t rl = len - 9u;
            float dur_sec = -1.0f;
            if (rl >= 3u && r[0] == '[' && r[rl - 1u] == ']') {
                parse_time_token(r + 1u, rl - 2u, &dur_sec);
            } else {
                uint32_t ms = 0u;
                if (parse_uint_slice(r, rl, 60000u, &ms)) dur_sec = float(ms) * 0.001f;
            }
            if (dur_sec >= 0.0f) {
                ColdVisualStyle* cold = ensure_transition_cold(s, st);
                if (!cold) return;
                cold->transition.duration_sec = dur_sec;
            }
            continue;
        }
        if (starts_with(tok, len, "delay-")) {
            const char* r = tok + 6u;
            const size_t rl = len - 6u;
            float del_sec = -1.0f;
            if (rl >= 3u && r[0] == '[' && r[rl - 1u] == ']') {
                parse_time_token(r + 1u, rl - 2u, &del_sec);
            } else {
                uint32_t ms = 0u;
                if (parse_uint_slice(r, rl, 60000u, &ms)) del_sec = float(ms) * 0.001f;
            }
            if (del_sec >= 0.0f) {
                ColdVisualStyle* cold = ensure_transition_cold(s, st);
                if (!cold) return;
                cold->transition.delay_sec = del_sec;
            }
            continue;
        }
        if (starts_with(tok, len, "ease-")) {
            const char* r = tok + 5u;
            const size_t rl = len - 5u;
            TransitionSpec tmp_tr = {};
            if (parse_transition_easing_word(r, rl, &tmp_tr)) {
                ColdVisualStyle* cold = ensure_transition_cold(s, st);
                if (!cold) return;
                cold->transition.easing = tmp_tr.easing;
                for (int bi = 0; bi < 4; ++bi) cold->transition.bezier[bi] = tmp_tr.bezier[bi];
            }
            continue;
        }
        if (inlay_ui_str_eq(tok, len, "animate-layout")) {
            st.flags |= NfAnimateLayout;
            continue;
        }
    }

    // Resolve final container axis before committing axis-dependent `items-*` and `gap-y-*`.
    const bool default_col =
        kind == INLAY_UI_SCROLL || kind == INLAY_UI_CARD ||
        kind == INLAY_UI_MODAL || kind == INLAY_UI_POPOVER;
    const bool is_col = (axis_override >= 0)
        ? (axis_override == 1)
        : (((st.flags & NfColumn) != 0u) || default_col);

    if (pending_items >= 0) {
        if (is_col) {
            st.align_x = uint8_t(pending_items);
            st.flags |= NfHasAlignX;
        } else {
            st.align_y = uint8_t(pending_items);
            st.flags |= NfHasAlignY;
        }
    }

    // If `gap-y-*` was specified alone on a non-grid column, propagate to main-axis `st.gap`.
    if (saw_gap_y && !saw_gap_x && is_col) {
        const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
        if (cold && cold->grid_cols == 0u) {
            st.gap = cold->row_gap;
            st.flags |= NfHasGap;
        }
    }
}

void inlay_ui_apply_cls(InlaySurface* s, const char* cls, size_t cls_len,
                        InlayUiKind kind, NodeStyle& st) {
    if (!s || !cls || cls_len == 0u) return;

    // Bypass cache if node already carries cold state or exceeds cache string cap.
    if (st.cold_idx != INLAY_UI_NONE || cls_len > kClsCacheMaxStrLen) {
        inlay_ui_apply_cls_parse(s, cls, cls_len, kind, st);
        return;
    }

    const bool default_col_kind =
        kind == INLAY_UI_SCROLL || kind == INLAY_UI_CARD ||
        kind == INLAY_UI_MODAL || kind == INLAY_UI_POPOVER;
    const uint8_t key_bits = static_cast<uint8_t>(((st.flags & NfColumn) != 0u ? 1u : 0u) |
                                                  (default_col_kind ? 2u : 0u));
    const uint64_t h = cls_hash64(cls, cls_len);
    const uint32_t base_slot = static_cast<uint32_t>(h & (kClsCacheSets - 1u)) * kClsCacheWays;

    if (s->cls_cache && s->cls_cache_strs) {
        for (uint32_t w = 0u; w < kClsCacheWays; ++w) {
            auto& e = s->cls_cache[base_slot + w];
            if ((e.key_flags & 0x80u) != 0u && e.hash == h && e.str_len == uint16_t(cls_len) &&
                (e.key_flags & 0x03u) == key_bits &&
                std::memcmp(s->cls_cache_strs + e.str_off, cls, cls_len) == 0)
            {
                e.last_hit = ++s->cls_cache_clock;
                st = e.style;
                st.cold_idx = INLAY_UI_NONE;
                if ((e.key_flags & 0x04u) != 0u) {
                    ColdVisualStyle* cold = inlay_ui_ensure_cold(s, st);
                    if (!cold) return;
                    *cold = e.cold;
                }
                return;
            }
        }
    }

    const uint32_t prev_shadows = s->shadow_pool_count;
    const uint32_t prev_grads = s->gradient_pool_count;
    inlay_ui_apply_cls_parse(s, cls, cls_len, kind, st);
    if (JS_HasException(s->js) || s->shadow_pool_count != prev_shadows || s->gradient_pool_count != prev_grads) {
        return;
    }

    if (!s->cls_cache) {
        s->cls_cache = static_cast<ClsCacheEntry*>(SDL_calloc(kClsCacheSlots, sizeof(ClsCacheEntry)));
        s->cls_cache_strs = static_cast<char*>(SDL_malloc(kClsCacheStrCap));
        if (!s->cls_cache || !s->cls_cache_strs) {
            SDL_free(s->cls_cache);
            SDL_free(s->cls_cache_strs);
            s->cls_cache = nullptr;
            s->cls_cache_strs = nullptr;
            return;
        }
        s->cls_cache_str_used = 0u;
    }
    if (s->cls_cache_str_used + uint32_t(cls_len) > kClsCacheStrCap) {
        std::memset(s->cls_cache, 0, size_t(kClsCacheSlots) * sizeof(ClsCacheEntry));
        s->cls_cache_str_used = 0u;
    }

    uint32_t pick = base_slot;
    if ((s->cls_cache[base_slot].key_flags & 0x80u) != 0u) {
        if ((s->cls_cache[base_slot + 1u].key_flags & 0x80u) == 0u ||
            s->cls_cache[base_slot + 1u].last_hit < s->cls_cache[base_slot].last_hit)
        {
            pick = base_slot + 1u;
        }
    }

    auto& dst = s->cls_cache[pick];
    dst.hash = h;
    dst.str_off = s->cls_cache_str_used;
    dst.str_len = uint16_t(cls_len);
    dst.last_hit = ++s->cls_cache_clock;
    std::memcpy(s->cls_cache_strs + dst.str_off, cls, cls_len);
    s->cls_cache_str_used += uint32_t(cls_len);
    dst.style = st;
    dst.style.cold_idx = INLAY_UI_NONE;
    const ColdVisualStyle* cold = inlay_ui_get_cold(s, st);
    if (cold) {
        dst.cold = *cold;
        dst.key_flags = static_cast<uint8_t>(0x80u | 0x04u | key_bits);
    } else {
        dst.key_flags = static_cast<uint8_t>(0x80u | key_bits);
    }
}

// ============================================================================
// 8 & 9. Spring & Timed Easing Tweens (Oklab Color + Scalar)
// ============================================================================

EtchColor inlay_ui_tween_color(InlaySurface* s, uint32_t element_id, uint16_t prop_bit,
                               EtchColor target, const TransitionSpec& trans) noexcept {
    if (!s || element_id == 0u || (trans.prop_mask & prop_bit) == 0u) {
        return target;
    }

    const uint32_t sub_id = tween_sub_id(element_id, prop_bit);
    if (trans.easing == TransEaseSpring) {
        return etch_spring_color(sub_id, target, resolve_spring_spec(trans));
    }

    const float dur = trans.duration_sec > 0.0f ? trans.duration_sec : 0.0f;
    if (dur <= 1e-4f) {
        for (uint32_t i = 0u; i < s->color_tween_count; ++i) {
            if (s->color_tweens[i].id_prop_hash == sub_id) {
                auto& slot = s->color_tweens[i];
                slot.start_col = target;
                slot.target_col = target;
                slot.current_col = target;
                slot.duration_sec = 0.0f;
                slot.delay_sec = 0.0f;
                slot.bezier[0] = 0.0f;
                slot.bezier[1] = 0.0f;
                slot.bezier[2] = 0.0f;
                slot.last_frame = s->frame_counter;
                slot.active = false;
                (void)etch_tween(sub_id, 0.0f, 0.0f, resolve_easing_spec(trans), 0.0f);
                break;
            }
        }
        return target;
    }
    const float delay = trans.delay_sec > 0.0f ? trans.delay_sec : 0.0f;
    const EtchEasingSpec ease = resolve_easing_spec(trans);

    // Lookup existing slot in bounded LRU cache (cap = 256).
    ColorTweenSlot* slot = nullptr;
    for (uint32_t i = 0u; i < s->color_tween_count; ++i) {
        if (s->color_tweens[i].id_prop_hash == sub_id) {
            slot = &s->color_tweens[i];
            break;
        }
    }

    if (!slot) {
        if (s->color_tween_count < kMaxColorTweens) {
            if (!inlay_grow_pod(s->color_tweens, s->color_tween_cap, s->color_tween_count + 1u,
                                16u, kMaxColorTweens)) {
                return target;
            }
            slot = &s->color_tweens[s->color_tween_count++];
        } else {
            // Evict least-recently-used slot (prefer settled slots on ties).
            uint32_t victim = 0u;
            for (uint32_t i = 1u; i < s->color_tween_count; ++i) {
                const auto& cur = s->color_tweens[i];
                const auto& best = s->color_tweens[victim];
                if (cur.last_frame < best.last_frame ||
                    (cur.last_frame == best.last_frame && !cur.active && best.active)) {
                    victim = i;
                }
            }
            slot = &s->color_tweens[victim];
        }

        *slot = {};
        slot->id_prop_hash = sub_id;
        slot->start_col = target;
        slot->target_col = target;
        slot->current_col = target;
        slot->duration_sec = dur;
        slot->delay_sec = delay;
        slot->easing = trans.easing;
        // bezier[0] = start_v, bezier[1] = goal_v, bezier[2] = last_raw
        slot->bezier[0] = 0.0f;
        slot->bezier[1] = 0.0f;
        slot->bezier[2] = 0.0f;
        slot->last_frame = s->frame_counter;
        slot->active = false;
        (void)etch_tween(sub_id, 0.0f, 0.0f, ease, 0.0f);
        return target;
    }

    slot->last_frame = s->frame_counter;

    if (etch_color_pack_rgba8(target) != etch_color_pack_rgba8(slot->target_col)) {
        slot->start_col = slot->current_col;
        slot->target_col = target;
        slot->duration_sec = dur;
        slot->delay_sec = delay;
        slot->easing = trans.easing;

        // Pick a new goal in {0, 1, 2} distinct from prev_goal maximizing |goal - start_v|
        // so etch_tween always detects retargeting (even mid-delay or mid-flight) with span >= 0.5.
        const float start_v = slot->bezier[2];
        const float prev_goal = slot->bezier[1];
        float goal_v = 1.0f;
        float best_dist = -1.0f;
        constexpr float kGoals[3] = {0.0f, 1.0f, 2.0f};
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(kGoals[i] - prev_goal) < 0.25f) continue;
            const float dist = std::fabs(kGoals[i] - start_v);
            if (dist > best_dist) {
                best_dist = dist;
                goal_v = kGoals[i];
            }
        }
        slot->bezier[0] = start_v;
        slot->bezier[1] = goal_v;
        slot->active = true;
    }

    if (!slot->active) {
        return slot->current_col;
    }

    const float raw = etch_tween(sub_id, slot->bezier[1], dur, ease, delay);
    slot->bezier[2] = raw;

    const float span = slot->bezier[1] - slot->bezier[0];
    const float u = std::fabs(span) > 1e-4f
        ? clampf((raw - slot->bezier[0]) / span, 0.0f, 1.0f)
        : 1.0f;
    slot->current_col = etch_color_lerp_oklab(slot->start_col, slot->target_col, u);

    if (raw == slot->bezier[1]) {
        slot->current_col = slot->target_col;
        slot->active = false;
    }
    return slot->current_col;
}

float inlay_ui_tween_scalar(InlaySurface* s, uint32_t element_id, uint16_t prop_bit,
                            float target, const TransitionSpec& trans) noexcept {
    if (!s || element_id == 0u || !std::isfinite(target) || (trans.prop_mask & prop_bit) == 0u) {
        return target;
    }

    const uint32_t sub_id = tween_sub_id(element_id, prop_bit);
    if (trans.easing == TransEaseSpring) {
        return etch_spring(sub_id, target, resolve_spring_spec(trans));
    }

    const float dur = trans.duration_sec > 0.0f ? trans.duration_sec : 0.0f;
    if (dur <= 1e-4f) {
        return target;
    }
    return etch_tween(sub_id, target, dur, resolve_easing_spec(trans),
                      trans.delay_sec > 0.0f ? trans.delay_sec : 0.0f);
}

#endif // INLAY_ENABLE_SCRIPT
