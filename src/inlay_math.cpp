#include "inlay_internal.h"
#include <cmath>
#include <cstring>

namespace {
enum Kind : uint8_t { Row, Glyph, Fraction, Root, Scripts, Accent, Grid, Delimited, Limits, Space };
enum MathStyle : uint8_t { StyleDisplay = 0, StyleText = 1, StyleScript = 2, StyleScriptScript = 3 };
enum MathClass : uint8_t { ClOrd = 0, ClOp = 1, ClBin = 2, ClRel = 3, ClOpen = 4, ClClose = 5, ClPunct = 6 };
enum AccentKind : uint8_t { AccOverline = 0, AccUnderline = 1, AccHat = 2, AccVec = 3, AccTilde = 4, AccBar = 5, AccDot = 6, AccDDot = 7 };

struct Box {
    Kind kind;
    MathClass mclass;
    uint8_t flags; // bit0: large_op_limits, bit1: binom_no_rule, bit2: accent_wide
    uint8_t sub_kind; // AccentKind or DelimKind
    uint16_t font_id;
    float width, ascent, descent, size;
    EtchStr text;
    Box *first, *last, *next, *a, *b, *c;
    float ax, ay, bx, by, cx, cy;
};

struct StyleState {
    MathStyle style;
    bool cramped;
};

struct Parser {
    EtchStr source;
    uint32_t pos;
    bool valid;
    uint32_t depth;
    float base_size;
};

float style_scale(MathStyle st) noexcept {
    switch (st) {
        case StyleDisplay:      return 1.0f;
        case StyleText:         return 1.0f;
        case StyleScript:       return 0.72f;
        case StyleScriptScript: return 0.55f;
    }
    return 1.0f;
}

float style_font_size(const Parser* p, StyleState st) noexcept {
    return std::fmax(7.0f, p->base_size * style_scale(st.style));
}

StyleState frac_num_style(StyleState st) noexcept {
    switch (st.style) {
        case StyleDisplay:      return { StyleText, st.cramped };
        case StyleText:         return { StyleScript, st.cramped };
        case StyleScript:       return { StyleScriptScript, st.cramped };
        case StyleScriptScript: return { StyleScriptScript, st.cramped };
    }
    return { StyleScript, st.cramped };
}

StyleState frac_den_style(StyleState st) noexcept {
    StyleState s = frac_num_style(st);
    s.cramped = true;
    return s;
}

StyleState sup_style(StyleState st) noexcept {
    const MathStyle next = (st.style <= StyleText) ? StyleScript : StyleScriptScript;
    return { next, st.cramped };
}

StyleState sub_style(StyleState st) noexcept {
    const MathStyle next = (st.style <= StyleText) ? StyleScript : StyleScriptScript;
    return { next, true };
}

float math_axis(float size) noexcept {
    const auto m = etch_font_metrics(ETCH_FONT_SANS, size);
    return m.ascent * 0.36f;
}

Box* make(Parser* p, Kind kind, float size, MathClass cls = ClOrd) noexcept {
    auto* box = static_cast<Box*>(etch_frame_arena_alloc(etch_current_context(), sizeof(Box), alignof(Box)));
    if (!box) { p->valid = false; return nullptr; }
    *box = {};
    box->kind = kind;
    box->mclass = cls;
    box->font_id = ETCH_FONT_SANS;
    box->size = size;
    return box;
}

Box* glyph(Parser* p, EtchStr text, float size, uint16_t font_id = ETCH_FONT_SANS, MathClass cls = ClOrd, uint8_t flags = 0) noexcept {
    auto* box = make(p, Glyph, size, cls);
    if (!box) return nullptr;
    box->text = text;
    box->font_id = font_id;
    box->flags = flags;
    const auto measure = etch_measure_text(text, font_id, size, 0);
    const auto metrics = etch_font_metrics(font_id, size);
    box->width = measure.x;
    box->ascent = metrics.ascent;
    box->descent = metrics.descent;
    return box;
}

// TeX spacing table in ems (thin=0.16, med=0.22, thick=0.28). Scripts suppress med/thick.
float class_spacing_em(MathClass left, MathClass right, MathStyle st) noexcept {
    const bool script = st >= StyleScript;
    const float thin = 0.16f;
    const float med = script ? 0.0f : 0.22f;
    const float thick = script ? 0.0f : 0.28f;
    switch (left) {
        case ClOrd:
            if (right == ClOp) return thin;
            if (right == ClBin) return med;
            if (right == ClRel) return thick;
            return 0.0f;
        case ClOp:
            if (right == ClOrd || right == ClOp) return thin;
            if (right == ClRel) return thick;
            return 0.0f;
        case ClBin:
            if (right == ClOrd || right == ClOp || right == ClOpen) return med;
            return 0.0f;
        case ClRel:
            if (right == ClOrd || right == ClOp || right == ClOpen) return thick;
            return 0.0f;
        case ClOpen:
            return 0.0f;
        case ClClose:
            if (right == ClOp) return thin;
            if (right == ClBin) return med;
            if (right == ClRel) return thick;
            return 0.0f;
        case ClPunct:
            if (right == ClOrd || right == ClOp || right == ClOpen || right == ClRel) return thin;
            return 0.0f;
    }
    return 0.0f;
}

void append(Box* row, Box* item) noexcept {
    if (row->last) row->last->next = item; else row->first = item;
    row->last = item;
    row->width += item->width;
    row->ascent = std::fmax(row->ascent, item->ascent);
    row->descent = std::fmax(row->descent, item->descent);
}

void spaces(Parser* p) noexcept {
    while (p->pos < p->source.length &&
           (p->source.chars[p->pos] == ' ' || p->source.chars[p->pos] == '\n' ||
            p->source.chars[p->pos] == '\r' || p->source.chars[p->pos] == '\t'))
    {
        ++p->pos;
    }
}

Box* expression(Parser*, StyleState, bool, bool = false, bool = false) noexcept;
Box* atom(Parser*, StyleState) noexcept;

// Parse a delimiter after \left or \right (supports (, ), [, ], \{, \}, ., |, \|, \langle, \rangle).
// Returns single-byte tag: '(', ')', '[', ']', '{', '}', '.', '|', 'V' (\|), '<' (\langle), '>' (\rangle), or 0.
char parse_delimiter(Parser* p) noexcept {
    spaces(p);
    if (p->pos >= p->source.length) { p->valid = false; return 0; }
    const char c = p->source.chars[p->pos];
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '.' || c == '|') {
        ++p->pos;
        return c;
    }
    if (c == '\\') {
        ++p->pos;
        if (p->pos >= p->source.length) { p->valid = false; return 0; }
        const char next = p->source.chars[p->pos];
        if (next == '{' || next == '}' || next == '|') {
            ++p->pos;
            return next == '|' ? 'V' : next;
        }
        const uint32_t start = p->pos;
        while (p->pos < p->source.length &&
               ((p->source.chars[p->pos] >= 'a' && p->source.chars[p->pos] <= 'z') ||
                (p->source.chars[p->pos] >= 'A' && p->source.chars[p->pos] <= 'Z')))
        {
            ++p->pos;
        }
        const uint32_t n = p->pos - start;
        if (n == 6 && !std::memcmp(p->source.chars + start, "langle", 6)) return '<';
        if (n == 6 && !std::memcmp(p->source.chars + start, "rangle", 6)) return '>';
        if (n == 5 && !std::memcmp(p->source.chars + start, "lvert", 5)) return '|';
        if (n == 5 && !std::memcmp(p->source.chars + start, "rvert", 5)) return '|';
        if (n == 5 && !std::memcmp(p->source.chars + start, "lVert", 5)) return 'V';
        if (n == 5 && !std::memcmp(p->source.chars + start, "rVert", 5)) return 'V';
        if (n == 4 && !std::memcmp(p->source.chars + start, "vert", 4)) return '|';
        if (n == 4 && !std::memcmp(p->source.chars + start, "Vert", 4)) return 'V';
        if (n == 6 && !std::memcmp(p->source.chars + start, "lbrace", 6)) return '{';
        if (n == 6 && !std::memcmp(p->source.chars + start, "rbrace", 6)) return '}';
    }
    p->valid = false;
    return 0;
}

Box* make_fraction_box(Parser* p, StyleState st, bool no_rule) noexcept {
    const float size = style_font_size(p, st);
    auto* box = make(p, Fraction, size, ClOrd);
    if (!box) return nullptr;
    if (no_rule) box->flags |= 2u;
    const StyleState num_st = frac_num_style(st);
    const StyleState den_st = frac_den_style(st);
    box->a = atom(p, num_st);
    box->b = atom(p, den_st);
    if (!box->a || !box->b) return nullptr;
    const float axis = math_axis(size);
    const float rule_gap = (st.style == StyleDisplay) ? std::fmax(2.5f, size * 0.16f) : std::fmax(1.8f, size * 0.11f);
    const float pad = no_rule ? size * 0.12f : size * 0.24f;
    box->width = std::fmax(box->a->width, box->b->width) + pad;
    box->ax = (box->width - box->a->width) * 0.5f;
    box->bx = (box->width - box->b->width) * 0.5f;
    box->ay = -(axis + rule_gap + box->a->descent);
    box->by = -(axis - rule_gap) + box->b->ascent;
    box->ascent = box->a->ascent - box->ay;
    box->descent = box->by + box->b->descent;
    return box;
}

Box* atom_impl(Parser* p, StyleState st) noexcept {
    spaces(p);
    if (p->pos == p->source.length) { p->valid = false; return nullptr; }
    const auto source = p->source;
    const uint32_t start = p->pos;
    const char c = source.chars[p->pos++];
    const float size = style_font_size(p, st);

    if (c == '{') return expression(p, st, true);
    if (c == '}') { p->valid = false; return nullptr; }
    if (c != '\\') {
        while (p->pos < source.length && (uint8_t(source.chars[p->pos]) & 0xc0) == 0x80) ++p->pos;
        const EtchStr tok = { source.chars + start, p->pos - start, false };
        if (tok.length == 1) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                return glyph(p, tok, size, ETCH_FONT_ITALIC, ClOrd);
            }
            if (c == '+' || c == '-') return glyph(p, c == '-' ? ETCH_STR_LIT("−") : tok, size, ETCH_FONT_SANS, ClBin);
            if (c == '=' || c == '<' || c == '>') return glyph(p, tok, size, ETCH_FONT_SANS, ClRel);
            if (c == '(' || c == '[') return glyph(p, tok, size, ETCH_FONT_SANS, ClOpen);
            if (c == ')' || c == ']') return glyph(p, tok, size, ETCH_FONT_SANS, ClClose);
            if (c == ',' || c == ';') return glyph(p, tok, size, ETCH_FONT_SANS, ClPunct);
            if (c == '*') return glyph(p, ETCH_STR_LIT("∗"), size, ETCH_FONT_SANS, ClBin);
        }
        return glyph(p, tok, size, ETCH_FONT_SANS, ClOrd);
    }

    const uint32_t command = p->pos;
    while (p->pos < source.length &&
           ((source.chars[p->pos] >= 'a' && source.chars[p->pos] <= 'z') ||
            (source.chars[p->pos] >= 'A' && source.chars[p->pos] <= 'Z')))
    {
        ++p->pos;
    }
    const uint32_t n = p->pos - command;
    const auto is = [&](const char* word) noexcept {
        return std::strlen(word) == n && !std::memcmp(source.chars + command, word, n);
    };
    if (!n) {
        if (p->pos == source.length) { p->valid = false; return nullptr; }
        const char esc = source.chars[p->pos++];
        if (esc == ',') { auto* b = make(p, Space, size); if (b) b->width = size * 0.16f; return b; }
        if (esc == ':' || esc == '>') { auto* b = make(p, Space, size); if (b) b->width = size * 0.22f; return b; }
        if (esc == ';') { auto* b = make(p, Space, size); if (b) b->width = size * 0.28f; return b; }
        if (esc == '!') { auto* b = make(p, Space, size); if (b) b->width = -size * 0.16f; return b; }
        if (esc == ' ') { auto* b = make(p, Space, size); if (b) b->width = size * 0.28f; return b; }
        if (esc == '{') return glyph(p, ETCH_STR_LIT("{"), size, ETCH_FONT_SANS, ClOpen);
        if (esc == '}') return glyph(p, ETCH_STR_LIT("}"), size, ETCH_FONT_SANS, ClClose);
        if (esc == '|') return glyph(p, ETCH_STR_LIT("‖"), size, ETCH_FONT_SANS, ClOrd);
        return glyph(p, { source.chars + (p->pos - 1u), 1, false }, size, ETCH_FONT_SANS, ClOrd);
    }
    if (is("quad") || is("qquad")) {
        auto* box = make(p, Space, size);
        if (box) box->width = size * (is("qquad") ? 2.0f : 1.0f);
        return box;
    }
    if (is("text") || is("mathrm") || is("operatorname") || is("mathbf") || is("mathit") || is("mathtt") || is("mathbb")) {
        const bool is_op = is("operatorname");
        const bool is_bb = is("mathbb");
        const uint16_t fid = static_cast<uint16_t>(is("mathbf") ? ETCH_FONT_BOLD
            : is("mathit") ? ETCH_FONT_ITALIC
            : is("mathtt") ? ETCH_FONT_MONO
            : ETCH_FONT_SANS);
        spaces(p);
        if (p->pos == source.length || source.chars[p->pos++] != '{') { p->valid = false; return nullptr; }
        const uint32_t text = p->pos;
        while (p->pos < source.length && source.chars[p->pos] != '}') ++p->pos;
        if (p->pos == source.length) { p->valid = false; return nullptr; }
        const uint32_t length = p->pos++ - text;
        if (is_bb && length == 1) {
            const char ch = source.chars[text];
            if (ch == 'R') return glyph(p, ETCH_STR_LIT("ℝ"), size, ETCH_FONT_SANS, ClOrd);
            if (ch == 'N') return glyph(p, ETCH_STR_LIT("ℕ"), size, ETCH_FONT_SANS, ClOrd);
            if (ch == 'Z') return glyph(p, ETCH_STR_LIT("ℤ"), size, ETCH_FONT_SANS, ClOrd);
            if (ch == 'Q') return glyph(p, ETCH_STR_LIT("ℚ"), size, ETCH_FONT_SANS, ClOrd);
            if (ch == 'C') return glyph(p, ETCH_STR_LIT("ℂ"), size, ETCH_FONT_SANS, ClOrd);
            if (ch == 'E') return glyph(p, ETCH_STR_LIT("𝔼"), size, ETCH_FONT_SANS, ClOrd);
            if (ch == 'P') return glyph(p, ETCH_STR_LIT("ℙ"), size, ETCH_FONT_SANS, ClOrd);
        }
        return glyph(p, { source.chars + text, length, false }, size, fid, is_op ? ClOp : ClOrd);
    }
    if (is("begin")) {
        spaces(p);
        if (p->pos == source.length || source.chars[p->pos++] != '{') { p->valid = false; return nullptr; }
        const uint32_t env = p->pos;
        while (p->pos < source.length && source.chars[p->pos] != '}') ++p->pos;
        const uint32_t env_len = p->pos - env;
        const auto env_is = [&](const char* name) noexcept {
            return std::strlen(name) == env_len && !std::memcmp(source.chars + env, name, env_len);
        };
        const bool is_aligned = env_is("aligned") || env_is("align");
        const bool is_cases = env_is("cases");
        const bool is_matrix = env_is("matrix");
        const bool is_pmatrix = env_is("pmatrix");
        const bool is_bmatrix = env_is("bmatrix");
        const bool is_Bmatrix = env_is("Bmatrix");
        const bool is_vmatrix = env_is("vmatrix");
        const bool is_Vmatrix = env_is("Vmatrix");
        if (p->pos == source.length ||
            !(is_aligned || is_cases || is_matrix || is_pmatrix || is_bmatrix || is_Bmatrix || is_vmatrix || is_Vmatrix))
        {
            p->valid = false;
            return nullptr;
        }
        ++p->pos;
        auto* grid = make(p, Grid, size, ClOrd);
        if (!grid) return nullptr;
        const StyleState cell_st = (is_aligned) ? st : StyleState{ StyleText, st.cramped };
        float widths[64] = {}, ascents[64] = {}, descents[64] = {};
        uint32_t row = 0, column = 0, columns = 0;
        while (p->valid) {
            if (row == 64 || column == 64) { p->valid = false; break; }
            auto* cell = expression(p, cell_st, false, true);
            if (!cell) break;
            cell->ax = float(column);
            cell->ay = float(row);
            append(grid, cell);
            widths[column] = std::fmax(widths[column], cell->width);
            ascents[row] = std::fmax(ascents[row], cell->ascent);
            descents[row] = std::fmax(descents[row], cell->descent);
            if (column + 1 > columns) columns = column + 1;
            if (p->pos < source.length && source.chars[p->pos] == '&') { ++p->pos; ++column; continue; }
            if (p->pos + 1 < source.length && source.chars[p->pos] == '\\' && source.chars[p->pos + 1] == '\\') {
                p->pos += 2;
                spaces(p);
                // Allow trailing \\ before \end{...} without spawning a phantom empty row.
                if (p->pos + 5 <= source.length && !std::memcmp(source.chars + p->pos, "\\end{", 5)) {
                    p->pos += 5;
                    if (p->pos + env_len >= source.length || std::memcmp(source.chars + p->pos, source.chars + env, env_len) || source.chars[p->pos + env_len] != '}') {
                        p->valid = false; break;
                    }
                    p->pos += env_len + 1;
                    break;
                }
                ++row;
                column = 0;
                continue;
            }
            if (p->pos + 5 <= source.length && !std::memcmp(source.chars + p->pos, "\\end{", 5)) {
                p->pos += 5;
                if (p->pos + env_len >= source.length || std::memcmp(source.chars + p->pos, source.chars + env, env_len) || source.chars[p->pos + env_len] != '}') {
                    p->valid = false; break;
                }
                p->pos += env_len + 1;
                break;
            }
            p->valid = false;
            break;
        }
        if (!p->valid) return nullptr;
        float x[64] = {}, y[64] = {};
        for (uint32_t cidx = 1; cidx < columns; ++cidx) {
            const float col_gap = is_aligned
                ? ((cidx & 1u) ? size * 0.18f : size * 1.1f)
                : (is_cases ? size * 0.9f : size * 0.7f);
            x[cidx] = x[cidx - 1] + widths[cidx - 1] + col_gap;
        }
        for (uint32_t ridx = 1; ridx <= row; ++ridx) {
            y[ridx] = y[ridx - 1] + descents[ridx - 1] + size * 0.35f + ascents[ridx];
        }
        const float total = ascents[0] + y[row] + descents[row];
        const float axis = math_axis(size);
        const float grid_ascent = total * 0.5f + axis;
        for (auto* cell = grid->first; cell; cell = cell->next) {
            const uint32_t cidx = uint32_t(cell->ax);
            const uint32_t ridx = uint32_t(cell->ay);
            if (is_aligned) {
                cell->ax = x[cidx] + ((cidx & 1u) == 0u ? (widths[cidx] - cell->width) : 0.0f);
            } else if (is_cases) {
                cell->ax = x[cidx];
            } else {
                cell->ax = x[cidx] + (widths[cidx] - cell->width) * 0.5f;
            }
            cell->ay = y[ridx] + ascents[0] - grid_ascent;
        }
        grid->width = columns ? x[columns - 1] + widths[columns - 1] : 0.0f;
        grid->ascent = grid_ascent;
        grid->descent = std::fmax(0.0f, total - grid_ascent);
        EtchStr delim = {};
        if (is_pmatrix) delim = ETCH_STR_LIT("()");
        else if (is_bmatrix) delim = ETCH_STR_LIT("[]");
        else if (is_Bmatrix) delim = ETCH_STR_LIT("{}");
        else if (is_vmatrix) delim = ETCH_STR_LIT("||");
        else if (is_Vmatrix) delim = ETCH_STR_LIT("VV");
        else if (is_cases) delim = ETCH_STR_LIT("{.");
        if (delim.length) {
            grid->text = delim;
            const float left_w = delim.chars[0] == '.' ? 0.0f : size * 0.38f;
            const float right_w = delim.chars[1] == '.' ? 0.0f : size * 0.38f;
            grid->width += left_w + right_w;
            for (auto* cell = grid->first; cell; cell = cell->next) cell->ax += left_w;
        }
        return grid;
    }
    if (is("frac")) return make_fraction_box(p, st, false);
    if (is("dfrac")) return make_fraction_box(p, { StyleDisplay, st.cramped }, false);
    if (is("tfrac")) return make_fraction_box(p, { StyleText, st.cramped }, false);
    if (is("binom") || is("dbinom") || is("tbinom")) {
        const StyleState bst = is("dbinom") ? StyleState{ StyleDisplay, st.cramped }
            : (is("tbinom") ? StyleState{ StyleText, st.cramped } : st);
        auto* inner = make_fraction_box(p, bst, true);
        if (!inner) return nullptr;
        auto* box = make(p, Delimited, size, ClOrd);
        if (!box) return nullptr;
        box->text = ETCH_STR_LIT("()");
        box->a = inner;
        box->ax = size * 0.36f;
        box->width = inner->width + size * 0.72f;
        box->ascent = inner->ascent;
        box->descent = inner->descent;
        return box;
    }
    if (is("sqrt")) {
        auto* box = make(p, Root, size, ClOrd);
        if (!box) return nullptr;
        spaces(p);
        if (p->pos < source.length && source.chars[p->pos] == '[') {
            ++p->pos;
            const uint32_t idx_start = p->pos;
            int depth_b = 1;
            while (p->pos < source.length && depth_b > 0) {
                if (source.chars[p->pos] == '[') ++depth_b;
                else if (source.chars[p->pos] == ']') --depth_b;
                if (depth_b > 0) ++p->pos;
            }
            if (p->pos >= source.length || source.chars[p->pos] != ']') { p->valid = false; return nullptr; }
            const EtchStr idx_src = { source.chars + idx_start, p->pos - idx_start, false };
            ++p->pos;
            Parser sub_p = { idx_src, 0, true, p->depth, p->base_size };
            box->b = expression(&sub_p, { StyleScriptScript, true }, false);
            if (!box->b || !sub_p.valid) { p->valid = false; return nullptr; }
        }
        StyleState rad_st = st;
        rad_st.cramped = true;
        box->a = atom(p, rad_st);
        if (!box->a) return nullptr;
        const float hook_w = size * 0.62f;
        const float idx_extra = box->b ? std::fmax(0.0f, box->b->width - size * 0.25f) : 0.0f;
        box->ax = idx_extra + hook_w;
        box->width = box->ax + box->a->width + size * 0.12f;
        box->ascent = box->a->ascent + std::fmax(2.5f, size * 0.18f);
        box->descent = box->a->descent;
        if (box->b) {
            box->bx = std::fmax(0.0f, box->ax - hook_w * 0.68f - box->b->width);
            const float idx_base_y = -(box->ascent - box->a->ascent * 0.55f);
            box->by = idx_base_y;
            box->ascent = std::fmax(box->ascent, box->b->ascent - box->by + 1.0f);
        }
        return box;
    }
    if (is("hat") || is("widehat") || is("vec") || is("overline") || is("underline") ||
        is("tilde") || is("widetilde") || is("bar") || is("dot") || is("ddot"))
    {
        auto* box = make(p, Accent, size, ClOrd);
        if (!box) return nullptr;
        StyleState inner_st = st;
        inner_st.cramped = true;
        box->a = atom(p, inner_st);
        if (!box->a) return nullptr;
        AccentKind ak = AccOverline;
        if (is("underline")) ak = AccUnderline;
        else if (is("hat") || is("widehat")) ak = AccHat;
        else if (is("vec")) ak = AccVec;
        else if (is("tilde") || is("widetilde")) ak = AccTilde;
        else if (is("bar")) ak = AccBar;
        else if (is("dot")) ak = AccDot;
        else if (is("ddot")) ak = AccDDot;
        box->sub_kind = static_cast<uint8_t>(ak);
        if (is("widehat") || is("widetilde")) box->flags |= 4u;
        const float min_w = (ak == AccVec) ? size * 0.48f : (ak == AccHat || ak == AccTilde || ak == AccBar) ? size * 0.36f : 0.0f;
        box->width = std::fmax(box->a->width, min_w);
        box->ax = (box->width - box->a->width) * 0.5f;
        if (ak == AccUnderline) {
            box->ascent = box->a->ascent;
            box->descent = box->a->descent + std::fmax(2.5f, size * 0.18f);
        } else {
            const float extra = (ak == AccOverline || ak == AccBar) ? std::fmax(2.5f, size * 0.18f) : size * 0.30f;
            box->ascent = box->a->ascent + extra;
            box->descent = box->a->descent;
        }
        return box;
    }
    if (is("left")) {
        const char open = parse_delimiter(p);
        if (!open || !p->valid) return nullptr;
        auto* content = expression(p, st, false, false, true);
        if (!content || !p->valid) return nullptr;
        const char close = parse_delimiter(p);
        if (!close || !p->valid) return nullptr;
        auto* box = make(p, Delimited, size, ClOrd);
        if (!box) return nullptr;
        auto* delimiters = static_cast<char*>(etch_frame_arena_alloc(etch_current_context(), 2, 1));
        if (!delimiters) { p->valid = false; return nullptr; }
        delimiters[0] = open;
        delimiters[1] = close;
        box->text = { delimiters, 2, false };
        box->a = content;
        const float left_w = (open == '.') ? 0.0f : size * 0.38f;
        const float right_w = (close == '.') ? 0.0f : size * 0.38f;
        box->ax = left_w;
        box->width = content->width + left_w + right_w;
        box->ascent = content->ascent;
        box->descent = content->descent;
        return box;
    }
    if (is("right")) { p->valid = false; return nullptr; }

    struct Symbol { const char* name; EtchStr text; MathClass cls; uint8_t flags; };
    // flags: bit0 = supports display above/below limits; bit1 = italic Greek variable; bit2 = tall large operator (1.35x in Display)
    static const Symbol symbols[] = {
        {"alpha",ETCH_STR_LIT("α"),ClOrd,2},{"beta",ETCH_STR_LIT("β"),ClOrd,2},{"gamma",ETCH_STR_LIT("γ"),ClOrd,2},{"delta",ETCH_STR_LIT("δ"),ClOrd,2},
        {"epsilon",ETCH_STR_LIT("ε"),ClOrd,2},{"zeta",ETCH_STR_LIT("ζ"),ClOrd,2},{"eta",ETCH_STR_LIT("η"),ClOrd,2},{"theta",ETCH_STR_LIT("θ"),ClOrd,2},
        {"iota",ETCH_STR_LIT("ι"),ClOrd,2},{"kappa",ETCH_STR_LIT("κ"),ClOrd,2},{"lambda",ETCH_STR_LIT("λ"),ClOrd,2},{"mu",ETCH_STR_LIT("μ"),ClOrd,2},
        {"nu",ETCH_STR_LIT("ν"),ClOrd,2},{"xi",ETCH_STR_LIT("ξ"),ClOrd,2},{"omicron",ETCH_STR_LIT("ο"),ClOrd,2},{"pi",ETCH_STR_LIT("π"),ClOrd,2},
        {"rho",ETCH_STR_LIT("ρ"),ClOrd,2},{"sigma",ETCH_STR_LIT("σ"),ClOrd,2},{"tau",ETCH_STR_LIT("τ"),ClOrd,2},{"upsilon",ETCH_STR_LIT("υ"),ClOrd,2},
        {"phi",ETCH_STR_LIT("φ"),ClOrd,2},{"chi",ETCH_STR_LIT("χ"),ClOrd,2},{"psi",ETCH_STR_LIT("ψ"),ClOrd,2},{"omega",ETCH_STR_LIT("ω"),ClOrd,2},
        {"Gamma",ETCH_STR_LIT("Γ"),ClOrd,0},{"Delta",ETCH_STR_LIT("Δ"),ClOrd,0},{"Theta",ETCH_STR_LIT("Θ"),ClOrd,0},{"Lambda",ETCH_STR_LIT("Λ"),ClOrd,0},
        {"Xi",ETCH_STR_LIT("Ξ"),ClOrd,0},{"Pi",ETCH_STR_LIT("Π"),ClOrd,0},{"Sigma",ETCH_STR_LIT("Σ"),ClOrd,0},{"Upsilon",ETCH_STR_LIT("Υ"),ClOrd,0},
        {"Phi",ETCH_STR_LIT("Φ"),ClOrd,0},{"Psi",ETCH_STR_LIT("Ψ"),ClOrd,0},{"Omega",ETCH_STR_LIT("Ω"),ClOrd,0},
        {"varepsilon",ETCH_STR_LIT("ϵ"),ClOrd,2},{"vartheta",ETCH_STR_LIT("ϑ"),ClOrd,2},{"varpi",ETCH_STR_LIT("ϖ"),ClOrd,2},
        {"varrho",ETCH_STR_LIT("ϱ"),ClOrd,2},{"varsigma",ETCH_STR_LIT("ς"),ClOrd,2},{"varphi",ETCH_STR_LIT("ϕ"),ClOrd,2},
        {"mp",ETCH_STR_LIT("∓"),ClBin,0},{"div",ETCH_STR_LIT("÷"),ClBin,0},{"ast",ETCH_STR_LIT("∗"),ClBin,0},{"star",ETCH_STR_LIT("⋆"),ClBin,0},
        {"circ",ETCH_STR_LIT("∘"),ClBin,0},{"bullet",ETCH_STR_LIT("•"),ClBin,0},{"oplus",ETCH_STR_LIT("⊕"),ClBin,0},{"otimes",ETCH_STR_LIT("⊗"),ClBin,0},
        {"wedge",ETCH_STR_LIT("∧"),ClBin,0},{"vee",ETCH_STR_LIT("∨"),ClBin,0},{"neg",ETCH_STR_LIT("¬"),ClOrd,0},
        {"forall",ETCH_STR_LIT("∀"),ClOrd,0},{"exists",ETCH_STR_LIT("∃"),ClOrd,0},{"emptyset",ETCH_STR_LIT("∅"),ClOrd,0},{"varnothing",ETCH_STR_LIT("∅"),ClOrd,0},
        {"supset",ETCH_STR_LIT("⊃"),ClRel,0},{"supseteq",ETCH_STR_LIT("⊇"),ClRel,0},{"to",ETCH_STR_LIT("→"),ClRel,0},{"mapsto",ETCH_STR_LIT("↦"),ClRel,0},
        {"leftrightarrow",ETCH_STR_LIT("↔"),ClRel,0},{"Leftrightarrow",ETCH_STR_LIT("⇔"),ClRel,0},{"iff",ETCH_STR_LIT("⇔"),ClRel,0},{"implies",ETCH_STR_LIT("⇒"),ClRel,0},
        {"ldots",ETCH_STR_LIT("…"),ClOrd,0},{"cdots",ETCH_STR_LIT("⋯"),ClOrd,0},{"vdots",ETCH_STR_LIT("⋮"),ClOrd,0},{"ddots",ETCH_STR_LIT("⋱"),ClOrd,0},
        {"perp",ETCH_STR_LIT("⊥"),ClRel,0},{"parallel",ETCH_STR_LIT("∥"),ClRel,0},{"angle",ETCH_STR_LIT("∠"),ClOrd,0},
        {"le",ETCH_STR_LIT("≤"),ClRel,0},{"ge",ETCH_STR_LIT("≥"),ClRel,0},{"ne",ETCH_STR_LIT("≠"),ClRel,0},{"simeq",ETCH_STR_LIT("≃"),ClRel,0},
        {"sim",ETCH_STR_LIT("∼"),ClRel,0},{"propto",ETCH_STR_LIT("∝"),ClRel,0},{"ll",ETCH_STR_LIT("≪"),ClRel,0},{"gg",ETCH_STR_LIT("≫"),ClRel,0},
        {"langle",ETCH_STR_LIT("⟨"),ClOpen,0},{"rangle",ETCH_STR_LIT("⟩"),ClClose,0},{"mid",ETCH_STR_LIT("|"),ClRel,0},
        {"oint",ETCH_STR_LIT("∮"),ClOp,4},{"coprod",ETCH_STR_LIT("∐"),ClOp,5},{"bigcup",ETCH_STR_LIT("⋃"),ClOp,5},{"bigcap",ETCH_STR_LIT("⋂"),ClOp,5},
        {"sum",ETCH_STR_LIT("∑"),ClOp,5},{"prod",ETCH_STR_LIT("∏"),ClOp,5},{"int",ETCH_STR_LIT("∫"),ClOp,4},{"infty",ETCH_STR_LIT("∞"),ClOrd,0},
        {"times",ETCH_STR_LIT("×"),ClBin,0},{"cdot",ETCH_STR_LIT("·"),ClBin,0},{"pm",ETCH_STR_LIT("±"),ClBin,0},
        {"leq",ETCH_STR_LIT("≤"),ClRel,0},{"geq",ETCH_STR_LIT("≥"),ClRel,0},{"neq",ETCH_STR_LIT("≠"),ClRel,0},
        {"approx",ETCH_STR_LIT("≈"),ClRel,0},{"equiv",ETCH_STR_LIT("≡"),ClRel,0},{"cong",ETCH_STR_LIT("≅"),ClRel,0},
        {"partial",ETCH_STR_LIT("∂"),ClOrd,0},{"nabla",ETCH_STR_LIT("∇"),ClOrd,0},{"in",ETCH_STR_LIT("∈"),ClRel,0},{"notin",ETCH_STR_LIT("∉"),ClRel,0},
        {"subset",ETCH_STR_LIT("⊂"),ClRel,0},{"subseteq",ETCH_STR_LIT("⊆"),ClRel,0},{"cup",ETCH_STR_LIT("∪"),ClBin,0},{"cap",ETCH_STR_LIT("∩"),ClBin,0},
        {"rightarrow",ETCH_STR_LIT("→"),ClRel,0},{"leftarrow",ETCH_STR_LIT("←"),ClRel,0},{"Rightarrow",ETCH_STR_LIT("⇒"),ClRel,0},{"Leftarrow",ETCH_STR_LIT("⇐"),ClRel,0},
        {"arcsin",ETCH_STR_LIT("arcsin"),ClOp,0},{"arccos",ETCH_STR_LIT("arccos"),ClOp,0},{"arctan",ETCH_STR_LIT("arctan"),ClOp,0},
        {"sinh",ETCH_STR_LIT("sinh"),ClOp,0},{"cosh",ETCH_STR_LIT("cosh"),ClOp,0},{"tanh",ETCH_STR_LIT("tanh"),ClOp,0},
        {"sin",ETCH_STR_LIT("sin"),ClOp,0},{"cos",ETCH_STR_LIT("cos"),ClOp,0},{"tan",ETCH_STR_LIT("tan"),ClOp,0},
        {"log",ETCH_STR_LIT("log"),ClOp,0},{"ln",ETCH_STR_LIT("ln"),ClOp,0},{"exp",ETCH_STR_LIT("exp"),ClOp,0},
        {"det",ETCH_STR_LIT("det"),ClOp,0},{"arg",ETCH_STR_LIT("arg"),ClOp,0},{"deg",ETCH_STR_LIT("deg"),ClOp,0},
        {"lim",ETCH_STR_LIT("lim"),ClOp,1},{"min",ETCH_STR_LIT("min"),ClOp,1},{"max",ETCH_STR_LIT("max"),ClOp,1},
        {"inf",ETCH_STR_LIT("inf"),ClOp,1},{"sup",ETCH_STR_LIT("sup"),ClOp,1},
    };
    for (const auto& symbol : symbols) {
        if (is(symbol.name)) {
            const bool big_op = (symbol.flags & 4u) != 0u && st.style == StyleDisplay;
            const float gsize = big_op ? size * 1.38f : size;
            const uint16_t fid = static_cast<uint16_t>((symbol.flags & 2u) ? ETCH_FONT_ITALIC : ETCH_FONT_SANS);
            auto* g = glyph(p, symbol.text, gsize, fid, symbol.cls, symbol.flags & 1u);
            if (g && big_op) {
                // Vertically center scaled display operators on the surrounding math axis.
                const float total = g->ascent + g->descent;
                const float target_a = total * 0.5f + math_axis(size);
                g->ascent = target_a;
                g->descent = std::fmax(0.0f, total - target_a);
            }
            return g;
        }
    }
    p->valid = false;
    return nullptr;
}

Box* atom(Parser* p, StyleState st) noexcept {
    if (++p->depth > 64) { p->valid = false; --p->depth; return nullptr; }
    auto* box = atom_impl(p, st);
    --p->depth;
    return box;
}

bool match_word_cmd(Parser* p, const char* word) noexcept {
    const uint32_t wlen = uint32_t(std::strlen(word));
    if (p->pos + 1 + wlen > p->source.length || p->source.chars[p->pos] != '\\') return false;
    if (std::memcmp(p->source.chars + p->pos + 1, word, wlen) != 0) return false;
    if (p->pos + 1 + wlen < p->source.length) {
        const char next = p->source.chars[p->pos + 1 + wlen];
        if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) return false;
    }
    p->pos += 1 + wlen;
    return true;
}

Box* expression(Parser* p, StyleState st, bool grouped, bool cell, bool delimited) noexcept {
    if (++p->depth > 64) { p->valid = false; --p->depth; return nullptr; }
    auto* row = make(p, Row, style_font_size(p, st), ClOrd);
    if (!row) { --p->depth; return nullptr; }

    bool has_prev = false;
    MathClass prev_cls = ClOpen;
    while (p->valid) {
        spaces(p);
        if (p->pos == p->source.length) { if (grouped) p->valid = false; break; }
        if (delimited && p->pos + 6 <= p->source.length && !std::memcmp(p->source.chars + p->pos, "\\right", 6)) {
            const uint32_t after = p->pos + 6;
            if (after == p->source.length || !((p->source.chars[after] >= 'a' && p->source.chars[after] <= 'z') || (p->source.chars[after] >= 'A' && p->source.chars[after] <= 'Z'))) {
                p->pos += 6;
                break;
            }
        }
        if (cell && (p->source.chars[p->pos] == '&' ||
                     (p->pos + 1 < p->source.length && p->source.chars[p->pos] == '\\' && p->source.chars[p->pos + 1] == '\\') ||
                     (p->pos + 5 <= p->source.length && !std::memcmp(p->source.chars + p->pos, "\\end{", 5))))
        {
            break;
        }
        if (p->source.chars[p->pos] == '}') { if (grouped) ++p->pos; else p->valid = false; break; }

        // Style declarations inside an expression affect all remaining items in the group.
        if (match_word_cmd(p, "displaystyle")) { st.style = StyleDisplay; continue; }
        if (match_word_cmd(p, "textstyle")) { st.style = StyleText; continue; }
        if (match_word_cmd(p, "scriptstyle")) { st.style = StyleScript; continue; }
        if (match_word_cmd(p, "scriptscriptstyle")) { st.style = StyleScriptScript; continue; }

        auto* base = atom(p, st);
        if (!base) break;
        const float size = style_font_size(p, st);

        // Check explicit \limits / \nolimits modifiers right after an operator.
        int8_t limits_override = 0;
        for (;;) {
            spaces(p);
            if (match_word_cmd(p, "limits")) { limits_override = 1; continue; }
            if (match_word_cmd(p, "nolimits")) { limits_override = -1; continue; }
            break;
        }

        Box* sup = nullptr;
        Box* sub = nullptr;
        while (p->valid) {
            spaces(p);
            if (p->pos == p->source.length) break;
            const char c = p->source.chars[p->pos];
            if (c != '^' && c != '_') break;
            ++p->pos;
            const StyleState sst = (c == '^') ? sup_style(st) : sub_style(st);
            auto* value = atom(p, sst);
            if (!value) break;
            if (c == '^') { if (sup) p->valid = false; sup = value; }
            else { if (sub) p->valid = false; sub = value; }
        }
        if (!p->valid) break;

        if (sup || sub) {
            const bool use_limits = (limits_override == 1) ||
                (limits_override == 0 && st.style == StyleDisplay && (base->flags & 1u) != 0u);
            if (use_limits) {
                auto* lim = make(p, Limits, size, base->mclass);
                if (!lim) break;
                lim->a = base;
                lim->b = sup;
                lim->c = sub;
                const float w = std::fmax(base->width, std::fmax(sup ? sup->width : 0.0f, sub ? sub->width : 0.0f));
                lim->width = w;
                lim->ax = (w - base->width) * 0.5f;
                lim->ascent = base->ascent;
                lim->descent = base->descent;
                const float gap = std::fmax(1.5f, size * 0.08f);
                const float op_trim = (base->kind == Glyph) ? size * 0.14f : 0.0f;
                if (sup) {
                    lim->bx = (w - sup->width) * 0.5f;
                    lim->by = -(std::fmax(0.0f, base->ascent - op_trim) + gap + sup->descent);
                    lim->ascent = std::fmax(lim->ascent, sup->ascent - lim->by);
                }
                if (sub) {
                    lim->cx = (w - sub->width) * 0.5f;
                    lim->cy = std::fmax(0.0f, base->descent - op_trim * 0.5f) + gap + sub->ascent;
                    lim->descent = std::fmax(lim->descent, lim->cy + sub->descent);
                }
                base = lim;
            } else {
                auto* scripts = make(p, Scripts, size, base->mclass);
                if (!scripts) break;
                scripts->a = base;
                scripts->b = sup;
                scripts->c = sub;
                scripts->width = base->width;
                scripts->ascent = base->ascent;
                scripts->descent = base->descent;
                const float italic_corr = (base->kind == Glyph && base->font_id == ETCH_FONT_ITALIC) ? size * 0.08f : 0.0f;
                float extra = 0.0f;
                if (sup) {
                    scripts->bx = base->width + italic_corr;
                    const float shift_min = st.cramped ? size * 0.32f : size * 0.42f;
                    const float base_top = (base->kind == Glyph) ? size * 0.65f : base->ascent;
                    scripts->by = -std::fmax(shift_min, base_top - size * 0.25f);
                }
                if (sub) {
                    scripts->cx = base->width;
                    const float base_bot = (base->kind == Glyph) ? size * 0.14f : base->descent;
                    scripts->cy = std::fmax(size * 0.24f, base_bot + sub->ascent * 0.28f);
                }
                // Enforce non-overlapping vertical clearance when both superscript and subscript exist.
                if (sup && sub) {
                    const float sup_bottom = scripts->by + sup->descent;
                    const float sub_top = scripts->cy - sub->ascent;
                    const float min_gap = std::fmax(1.5f, size * 0.06f);
                    const float gap = sub_top - sup_bottom;
                    if (gap < min_gap) {
                        const float push = (min_gap - gap) * 0.5f;
                        scripts->by -= push;
                        scripts->cy += push;
                    }
                }
                if (sup) {
                    extra = std::fmax(extra, italic_corr + sup->width);
                    scripts->ascent = std::fmax(scripts->ascent, sup->ascent - scripts->by);
                    scripts->descent = std::fmax(scripts->descent, scripts->by + sup->descent);
                }
                if (sub) {
                    extra = std::fmax(extra, sub->width);
                    scripts->descent = std::fmax(scripts->descent, scripts->cy + sub->descent);
                    scripts->ascent = std::fmax(scripts->ascent, sub->ascent - scripts->cy);
                }
                scripts->width += extra;
                base = scripts;
            }
        }

        // TeX Rule 5: convert Bin to Ord when preceded by None, Bin, Op, Rel, Open, or Punct.
        if (base->mclass == ClBin && (!has_prev || prev_cls == ClBin || prev_cls == ClOp || prev_cls == ClRel || prev_cls == ClOpen || prev_cls == ClPunct)) {
            base->mclass = ClOrd;
        }
        if (base->kind != Space) {
            if (has_prev) {
                const float sp = class_spacing_em(prev_cls, base->mclass, st.style) * size;
                if (sp > 0.0f) {
                    auto* gap_box = make(p, Space, size);
                    if (!gap_box) break;
                    gap_box->width = sp;
                    append(row, gap_box);
                }
            }
            prev_cls = base->mclass;
            has_prev = true;
        }
        append(row, base);
    }
    --p->depth;
    return row;
}

void draw_rule(float x, float y, float w, float size, EtchColor color) noexcept {
    EtchLineDesc desc = {};
    desc.from = { x, y };
    desc.to = { x + w, y };
    desc.thickness = std::fmax(1.0f, size * 0.055f);
    desc.color = color;
    etch_draw_line(&desc);
}

void draw_delimiter(char mark, float sx, float top, float bottom, float size, bool right_side, EtchColor color) noexcept {
    if (mark == '.') return;
    const float thick = std::fmax(1.0f, size * 0.055f);
    const float w = size * 0.34f;
    if (mark == '[' || mark == ']') {
        EtchLineDesc d = {};
        d.color = color;
        d.thickness = thick;
        const float edge = sx + (right_side ? 0.0f : w * 0.65f);
        const float stem = sx + (right_side ? w * 0.65f : 0.0f);
        d.from = { edge, top }; d.to = { stem, top }; etch_draw_line(&d);
        d.from = d.to; d.to.y = bottom; etch_draw_line(&d);
        d.from = d.to; d.to.x = edge; etch_draw_line(&d);
    } else if (mark == '|') {
        EtchLineDesc d = {};
        d.color = color; d.thickness = thick;
        const float cx = sx + w * 0.5f;
        d.from = { cx, top }; d.to = { cx, bottom }; etch_draw_line(&d);
    } else if (mark == 'V') {
        EtchLineDesc d = {};
        d.color = color; d.thickness = thick;
        const float c1 = sx + w * 0.32f, c2 = sx + w * 0.68f;
        d.from = { c1, top }; d.to = { c1, bottom }; etch_draw_line(&d);
        d.from = { c2, top }; d.to = { c2, bottom }; etch_draw_line(&d);
    } else if (mark == '<' || mark == '>') {
        EtchLineDesc d = {};
        d.color = color; d.thickness = thick;
        const float mid = (top + bottom) * 0.5f;
        const float tip = sx + (right_side ? w * 0.85f : w * 0.15f);
        const float edge = sx + (right_side ? w * 0.15f : w * 0.85f);
        d.from = { edge, top }; d.to = { tip, mid }; etch_draw_line(&d);
        d.from = { tip, mid }; d.to = { edge, bottom }; etch_draw_line(&d);
    } else if (mark == '{' || mark == '}') {
        const float edge = sx + (right_side ? 0.0f : w);
        const float stem = sx + w * 0.5f;
        const float tip = sx + (right_side ? w : 0.0f);
        const float mid = (top + bottom) * 0.5f;
        auto d = etch_bezier_desc_default();
        d.cubic = false; d.thickness = thick; d.color_start = color; d.color_end = color;
        d.p0 = { edge, top }; d.p1 = { stem, top }; d.p2 = { stem, (top + mid) * 0.5f }; etch_draw_bezier(&d);
        d.p0 = d.p2; d.p1 = { stem, mid }; d.p2 = { tip, mid }; etch_draw_bezier(&d);
        d.p0 = d.p2; d.p1 = { stem, mid }; d.p2 = { stem, (bottom + mid) * 0.5f }; etch_draw_bezier(&d);
        d.p0 = d.p2; d.p1 = { stem, bottom }; d.p2 = { edge, bottom }; etch_draw_bezier(&d);
    } else {
        auto d = etch_bezier_desc_default();
        d.p0 = { sx + w * 0.5f, top };
        d.p2 = { sx + w * 0.5f, bottom };
        d.p1 = { sx + (right_side ? w * 1.15f : -w * 0.15f), (top + bottom) * 0.5f };
        d.cubic = false; d.thickness = thick; d.color_start = color; d.color_end = color;
        etch_draw_bezier(&d);
    }
}

void draw(const Box* box, float x, float y, EtchColor color) noexcept {
    if (box->kind == Space) return;
    if (box->kind == Glyph) {
        auto desc = etch_text_draw_desc_default();
        desc.text = box->text;
        desc.font_id = box->font_id;
        desc.pos = { x, y - box->ascent + box->ay };
        desc.size = box->size;
        desc.color = color;
        etch_draw_text(&desc);
        return;
    }
    if (box->kind == Grid || box->kind == Delimited) {
        if (box->kind == Delimited) draw(box->a, x + box->ax, y, color);
        for (auto* child = box->first; child; child = child->next) draw(child, x + child->ax, y + child->ay, color);
        if (box->text.length >= 2) {
            const float top = y - box->ascent;
            const float bottom = y + box->descent;
            draw_delimiter(box->text.chars[0], x, top, bottom, box->size, false, color);
            draw_delimiter(box->text.chars[1], x + box->width - box->size * 0.34f, top, bottom, box->size, true, color);
        }
        return;
    }
    if (box->kind == Row) {
        for (auto* child = box->first; child; child = child->next) {
            draw(child, x, y, color);
            x += child->width;
        }
        return;
    }
    if (box->a) draw(box->a, x + box->ax, y + box->ay, color);
    if (box->b) draw(box->b, x + box->bx, y + box->by, color);
    if (box->c) draw(box->c, x + box->cx, y + box->cy, color);
    if (box->kind == Fraction && !(box->flags & 2u)) {
        draw_rule(x, y - math_axis(box->size), box->width, box->size, color);
    }
    if (box->kind == Root) {
        const float hook_w = box->size * 0.62f;
        const float left = x + box->ax - hook_w;
        const float top = y - box->a->ascent - std::fmax(2.5f, box->size * 0.18f);
        const float bottom = y + box->descent;
        const float thickness = std::fmax(1.0f, box->size * 0.055f);
        draw_rule(x + box->ax, top, box->width - box->ax, box->size, color);
        EtchVec2 points[] = {
            { left, y - box->size * 0.10f },
            { left + hook_w * 0.28f, y - box->size * 0.22f },
            { left + hook_w * 0.48f, bottom - thickness },
            { x + box->ax, top - thickness },
            { x + box->ax + thickness, top },
            { left + hook_w * 0.48f, bottom + thickness },
            { left + hook_w * 0.21f, y - box->size * 0.15f },
        };
        EtchFilledPolygonDesc radical = {};
        radical.points = points;
        radical.point_count = 7;
        radical.fill = color;
        etch_draw_filled_polygon(&radical);
    }
    if (box->kind == Accent) {
        const auto ak = static_cast<AccentKind>(box->sub_kind);
        const float thick = std::fmax(1.0f, box->size * 0.055f);
        if (ak == AccOverline) {
            draw_rule(x, y - box->ascent + box->size * 0.08f, box->width, box->size, color);
        } else if (ak == AccUnderline) {
            draw_rule(x, y + box->descent - box->size * 0.06f, box->width, box->size, color);
        } else if (ak == AccBar) {
            const float bw = std::fmax(box->width * 0.8f, box->size * 0.36f);
            draw_rule(x + (box->width - bw) * 0.5f, y - box->ascent + box->size * 0.09f, bw, box->size, color);
        } else if (ak == AccHat) {
            const float top = y - box->ascent + box->size * 0.05f;
            const float aw = (box->flags & 4u) ? box->width : std::fmin(box->width, box->size * 0.52f);
            const float ax0 = x + (box->width - aw) * 0.5f;
            EtchLineDesc d = {}; d.thickness = thick; d.color = color;
            d.from = { ax0, top + box->size * 0.18f }; d.to = { ax0 + aw * 0.5f, top }; etch_draw_line(&d);
            d.from = d.to; d.to = { ax0 + aw, top + box->size * 0.18f }; etch_draw_line(&d);
        } else if (ak == AccVec) {
            const float top = y - box->ascent + box->size * 0.05f;
            const float mid_y = top + box->size * 0.09f;
            const float head = std::fmin(box->size * 0.18f, box->width * 0.38f);
            EtchLineDesc d = {}; d.thickness = thick; d.color = color;
            d.from = { x, mid_y }; d.to = { x + box->width, mid_y }; etch_draw_line(&d);
            d.from = { x + box->width - head, top }; d.to = { x + box->width, mid_y }; etch_draw_line(&d);
            d.from = { x + box->width - head, top + box->size * 0.18f }; d.to = { x + box->width, mid_y }; etch_draw_line(&d);
        } else if (ak == AccTilde) {
            const float top = y - box->ascent + box->size * 0.14f;
            const float aw = (box->flags & 4u) ? box->width : std::fmin(box->width, box->size * 0.55f);
            const float ax0 = x + (box->width - aw) * 0.5f;
            const float amp = box->size * 0.10f;
            auto bz = etch_bezier_desc_default();
            bz.cubic = true; bz.thickness = thick; bz.color_start = color; bz.color_end = color;
            bz.p0 = { ax0, top };
            bz.p1 = { ax0 + aw * 0.33f, top - amp * 2.0f };
            bz.p2 = { ax0 + aw * 0.67f, top + amp * 2.0f };
            bz.p3 = { ax0 + aw, top };
            etch_draw_bezier(&bz);
        } else if (ak == AccDot || ak == AccDDot) {
            const float r = std::fmax(1.2f, box->size * 0.06f);
            const float cy = y - box->ascent + box->size * 0.12f;
            const float cx = x + box->width * 0.5f;
            auto dot = etch_rect_draw_desc_default();
            dot.fill = color;
            dot.radius = etch_corners_all(r);
            if (ak == AccDot) {
                dot.rect = { cx - r, cy - r, r * 2.0f, r * 2.0f };
                etch_draw_rect(&dot);
            } else {
                const float dx = std::fmax(r * 1.6f, box->size * 0.11f);
                dot.rect = { cx - dx - r, cy - r, r * 2.0f, r * 2.0f };
                etch_draw_rect(&dot);
                dot.rect = { cx + dx - r, cy - r, r * 2.0f, r * 2.0f };
                etch_draw_rect(&dot);
            }
        }
    }
}
} // namespace

bool inlay_math_prepare(EtchStr tex, float font_size_px, bool display_style, InlayMathPrepared* out) noexcept {
    if (!out) return false;
    *out = {};
    auto* context = etch_current_context();
    if (!context || !etch_frame_in_progress(context) || (!tex.chars && tex.length) || tex.length > 65536 ||
        !std::isfinite(font_size_px) || font_size_px < 6 || font_size_px > 256)
    {
        return false;
    }
    Parser parser = { tex, 0, true, 0, font_size_px };
    const StyleState init_st = { display_style ? StyleDisplay : StyleText, false };
    const auto* box = expression(&parser, init_st, false);
    if (!box || !parser.valid) return false;
    out->root = box;
    out->metrics = { box->width, box->ascent + box->descent, box->ascent, true };
    return true;
}

void inlay_math_draw_prepared(const InlayMathPrepared& prep, EtchVec2 pos, EtchColor color) noexcept {
    if (!prep.root || !prep.metrics.valid) return;
    const auto* box = static_cast<const Box*>(prep.root);
    draw(box, pos.x, pos.y + box->ascent, color);
}

extern "C" {
INLAY_API InlayMathResult inlay_measure_math(EtchStr tex, float size) {
    InlayMathPrepared prep = {};
    return inlay_math_prepare(tex, size, true, &prep) ? prep.metrics : InlayMathResult{};
}
INLAY_API InlayMathResult inlay_draw_math(EtchStr tex, const InlayMathDesc* desc_in) {
    InlayMathDesc desc = {};
    if (!inlay_read_abi_struct(desc_in, &desc) || !std::isfinite(desc.pos.x) || !std::isfinite(desc.pos.y)) return {};
    InlayMathPrepared prep = {};
    if (!inlay_math_prepare(tex, desc.font_size_px, true, &prep)) return {};
    inlay_math_draw_prepared(prep, desc.pos, desc.color);
    return prep.metrics;
}
}
