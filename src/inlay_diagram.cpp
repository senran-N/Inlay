#include "inlay_internal.h"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {
struct DiagramChunk {
    DiagramChunk* next;
    size_t used;
    size_t cap;
    size_t reserved;
    uint8_t data[16];
};

struct DiagramHeapArena {
    DiagramChunk* head;
    void* alloc(size_t bytes, size_t align) noexcept {
        if (align < 1u) align = 1u;
        if (bytes == 0u) bytes = 1u;
        if (head) {
            const size_t cur = (head->used + align - 1u) & ~(align - 1u);
            if (cur + bytes <= head->cap) {
                head->used = cur + bytes;
                return head->data + cur;
            }
        }
        const size_t need = bytes + align + 4096u;
        const size_t chunk_cap = need > 16384u ? need : 16384u;
        auto* c = static_cast<DiagramChunk*>(SDL_malloc(offsetof(DiagramChunk, data) + chunk_cap));
        if (!c) return nullptr;
        c->next = head;
        c->used = 0u;
        c->cap = chunk_cap;
        head = c;
        const size_t cur = (c->used + align - 1u) & ~(align - 1u);
        c->used = cur + bytes;
        return c->data + cur;
    }
    void destroy() noexcept {
        while (head) {
            DiagramChunk* nx = head->next;
            SDL_free(head);
            head = nx;
        }
    }
};

struct DiagramEnv {
    EtchContext* ctx;
    DiagramHeapArena* heap;
    bool use_etch_fonts;

    void* alloc(size_t bytes, size_t align) const noexcept {
        if (heap) return heap->alloc(bytes, align);
        return etch_frame_arena_alloc(ctx, bytes, align);
    }
    float line_height(uint16_t font_id, float font_size) const noexcept {
        if (use_etch_fonts) return std::fmax(etch_font_metrics(font_id, font_size).line_height, font_size * 1.25f);
        return font_size * 1.25f;
    }
    EtchVec2 measure_text(EtchStr s, uint16_t font_id, float font_size) const noexcept {
        if (use_etch_fonts) return etch_measure_text(s, font_id, font_size, 0);
        size_t pos = 0u;
        float units = 0.0f;
        while (pos < s.length) {
            const uint32_t cp = inlay_utf8_next(s.chars, s.length, &pos);
            units += (cp > 0x2E7Fu) ? 1.0f : 0.56f;
        }
        return { units * font_size, font_size * 1.25f };
    }
};

struct Graph {
    EtchStr source;
    uint32_t pos;
    InlayDiagramNode* nodes;
    InlayDiagramShape* shapes;
    int32_t* node_subgraph;
    InlayDiagramEdge* edges;
    uint8_t* edge_styles;
    InlayDiagramSubgraph* subgraphs;
    uint32_t node_count, node_capacity, edge_count, subgraph_count;
    int32_t active_subgraph;
    bool valid, lr, reverse_dir;
};

void spaces(Graph* g) noexcept {
    while (g->pos < g->source.length &&
           (g->source.chars[g->pos] == ' ' || g->source.chars[g->pos] == '\t' || g->source.chars[g->pos] == '\r'))
    {
        ++g->pos;
    }
}

bool word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool equal(EtchStr a, EtchStr b) noexcept {
    return a.length == b.length && !std::memcmp(a.chars, b.chars, a.length);
}

bool starts(const Graph& g, EtchStr marker) noexcept {
    return marker.length <= g.source.length - g.pos && !std::memcmp(g.source.chars + g.pos, marker.chars, marker.length);
}

bool statement_end(const Graph& g) noexcept {
    return g.pos == g.source.length || g.source.chars[g.pos] == '\n' || g.source.chars[g.pos] == ';';
}

EtchStr trim_slice(EtchStr source, uint32_t start, uint32_t end) noexcept {
    while (start < end && (source.chars[start] == ' ' || source.chars[start] == '\t')) ++start;
    while (end > start && (source.chars[end - 1] == ' ' || source.chars[end - 1] == '\t' || source.chars[end - 1] == '\r')) --end;
    return { source.chars + start, end - start, false };
}

EtchStr strip_outer_quotes(EtchStr s) noexcept {
    if (s.length >= 2u && s.chars[0] == '"' && s.chars[s.length - 1u] == '"') {
        return { s.chars + 1u, s.length - 2u, false };
    }
    return s;
}

uint32_t match_br_tag(const char* s, uint32_t len, uint32_t pos) noexcept {
    if (pos >= len) return 0u;
    if (s[pos] == '\n') return 1u;
    if (s[pos] == '\\' && pos + 1u < len && s[pos + 1u] == 'n') return 2u;
    if (s[pos] == '<' && pos + 3u < len) {
        const char b = s[pos + 1u];
        const char r = s[pos + 2u];
        if ((b == 'b' || b == 'B') && (r == 'r' || r == 'R')) {
            uint32_t p = pos + 3u;
            while (p < len && (s[p] == ' ' || s[p] == '\t')) ++p;
            if (p < len && s[p] == '/') ++p;
            while (p < len && (s[p] == ' ' || s[p] == '\t')) ++p;
            if (p < len && s[p] == '>') return (p + 1u) - pos;
        }
    }
    return 0u;
}

EtchVec2 measure_label_env(EtchStr label, uint16_t font_id, float font_size, const DiagramEnv& env, float* out_line_h = nullptr) noexcept {
    const float line_h = env.line_height(font_id, font_size);
    if (out_line_h) *out_line_h = line_h;
    if (!label.chars || !label.length) return { 0.0f, 0.0f };
    bool multiline = false;
    for (uint32_t i = 0; i < label.length; ++i) {
        if (match_br_tag(label.chars, label.length, i) > 0u) { multiline = true; break; }
    }
    if (!multiline) {
        return env.measure_text(label, font_id, font_size);
    }
    float max_w = 0.0f;
    uint32_t lines = 0;
    uint32_t pos = 0;
    while (pos <= label.length) {
        uint32_t stop = pos;
        uint32_t tag_len = 0;
        while (stop < label.length) {
            tag_len = match_br_tag(label.chars, label.length, stop);
            if (tag_len > 0u) break;
            ++stop;
        }
        const EtchStr seg = trim_slice(label, pos, stop);
        if (seg.length) {
            const float w = env.measure_text(seg, font_id, font_size).x;
            max_w = std::fmax(max_w, w);
        }
        ++lines;
        if (tag_len == 0u) break;
        pos = stop + tag_len;
    }
    return { max_w, float(lines ? lines : 1u) * line_h };
}

EtchVec2 measure_label(EtchStr label, uint16_t font_id, float font_size, float* out_line_h = nullptr) noexcept {
    const DiagramEnv env = { nullptr, nullptr, true };
    return measure_label_env(label, font_id, font_size, env, out_line_h);
}

void draw_label(EtchStr label, EtchVec2 top_pos, uint16_t font_id, float font_size, EtchColor color,
                EtchAlignX align_x = ETCH_ALIGN_X_CENTER) noexcept {
    if (!label.chars || !label.length) return;
    bool multiline = false;
    for (uint32_t i = 0; i < label.length; ++i) {
        if (match_br_tag(label.chars, label.length, i) > 0u) { multiline = true; break; }
    }
    if (!multiline) {
        auto text = etch_text_draw_desc_default();
        text.text = label;
        text.font_id = font_id;
        text.pos = top_pos;
        text.align_x = align_x;
        text.size = font_size;
        text.color = color;
        etch_draw_text(&text);
        return;
    }
    const float line_h = std::fmax(etch_font_metrics(font_id, font_size).line_height, font_size * 1.25f);
    uint32_t pos = 0;
    uint32_t line_idx = 0;
    while (pos <= label.length) {
        uint32_t stop = pos;
        uint32_t tag_len = 0;
        while (stop < label.length) {
            tag_len = match_br_tag(label.chars, label.length, stop);
            if (tag_len > 0u) break;
            ++stop;
        }
        const EtchStr seg = trim_slice(label, pos, stop);
        if (seg.length) {
            auto text = etch_text_draw_desc_default();
            text.text = seg;
            text.font_id = font_id;
            text.pos = { top_pos.x, top_pos.y + float(line_idx) * line_h };
            text.align_x = align_x;
            text.size = font_size;
            text.color = color;
            etch_draw_text(&text);
        }
        ++line_idx;
        if (tag_len == 0u) break;
        pos = stop + tag_len;
    }
}

uint16_t resolve_body_font(const InlayDiagramDesc& desc) noexcept {
    if (desc.has_custom_fonts && desc.font_id != ETCH_INVALID_FONT) return desc.font_id;
    return static_cast<uint16_t>(ETCH_FONT_SANS);
}

uint16_t resolve_bold_font(const InlayDiagramDesc& desc) noexcept {
    if (desc.has_custom_fonts && desc.bold_font_id != ETCH_INVALID_FONT) return desc.bold_font_id;
    return static_cast<uint16_t>(ETCH_FONT_SEMIBOLD);
}

uint32_t node(Graph* g) noexcept {
    spaces(g);
    const uint32_t start = g->pos;
    while (g->pos < g->source.length) {
        const char c = g->source.chars[g->pos];
        if (word(c)) { ++g->pos; continue; }
        if (c == '-' && g->pos + 1u < g->source.length && word(g->source.chars[g->pos + 1u])) { ++g->pos; continue; }
        break;
    }
    if (start == g->pos) { g->valid = false; return 0; }
    const EtchStr id = { g->source.chars + start, g->pos - start, false };
    uint32_t index = 0;
    while (index < g->node_count && !equal(g->nodes[index].id, id)) ++index;
    if (index == g->node_count) {
        if (index == g->node_capacity) { g->valid = false; return 0; }
        g->nodes[index] = {};
        g->nodes[index].id = id;
        g->nodes[index].label = id;
        g->shapes[index] = InlayShapeRect;
        g->node_subgraph[index] = g->active_subgraph;
        ++g->node_count;
    } else if (g->node_subgraph[index] < 0 && g->active_subgraph >= 0) {
        g->node_subgraph[index] = g->active_subgraph;
    }
    spaces(g);
    if (g->pos < g->source.length &&
        (g->source.chars[g->pos] == '[' || g->source.chars[g->pos] == '(' || g->source.chars[g->pos] == '{'))
    {
        const char open = g->source.chars[g->pos++];
        char close = open == '[' ? ']' : open == '(' ? ')' : '}';
        InlayDiagramShape shape = open == '[' ? InlayShapeRect : open == '(' ? InlayShapeRound : InlayShapeDiamond;
        bool double_close = false;
        char second_close = 0;
        if (open == '(' && g->pos < g->source.length && g->source.chars[g->pos] == '(') {
            ++g->pos;
            shape = InlayShapeCircle;
            double_close = true;
            second_close = ')';
        } else if (open == '(' && g->pos < g->source.length && g->source.chars[g->pos] == '[') {
            ++g->pos;
            shape = InlayShapeRound;
            close = ']';
            double_close = true;
            second_close = ')';
        } else if (open == '[' && g->pos < g->source.length && g->source.chars[g->pos] == '[') {
            ++g->pos;
            shape = InlayShapeRect;
            double_close = true;
            second_close = ']';
        } else if (open == '[' && g->pos < g->source.length && g->source.chars[g->pos] == '(') {
            ++g->pos;
            shape = InlayShapeRound;
            close = ')';
            double_close = true;
            second_close = ']';
        } else if (open == '{' && g->pos < g->source.length && g->source.chars[g->pos] == '{') {
            ++g->pos;
            shape = InlayShapeDiamond;
            double_close = true;
            second_close = '}';
        }
        spaces(g);
        if (g->pos < g->source.length && g->source.chars[g->pos] == '"') {
            ++g->pos;
            const uint32_t label = g->pos;
            while (g->pos < g->source.length && g->source.chars[g->pos] != '\n') {
                if (g->source.chars[g->pos] == '\\' && g->pos + 1u < g->source.length && g->source.chars[g->pos + 1u] == '"') {
                    g->pos += 2u;
                    continue;
                }
                if (g->source.chars[g->pos] == '"') break;
                ++g->pos;
            }
            if (g->pos == g->source.length || g->source.chars[g->pos] != '"') { g->valid = false; return index; }
            g->nodes[index].label = { g->source.chars + label, g->pos - label, false };
            ++g->pos;
            spaces(g);
            if (g->pos == g->source.length || g->source.chars[g->pos] != close) { g->valid = false; return index; }
            g->shapes[index] = shape;
            ++g->pos;
        } else {
            const uint32_t label = g->pos;
            while (g->pos < g->source.length && g->source.chars[g->pos] != close && g->source.chars[g->pos] != '\n') ++g->pos;
            if (g->pos == g->source.length || g->source.chars[g->pos] != close) { g->valid = false; return index; }
            g->nodes[index].label = trim_slice(g->source, label, g->pos);
            g->shapes[index] = shape;
            ++g->pos;
        }
        if (double_close) {
            if (g->pos == g->source.length || g->source.chars[g->pos] != second_close) { g->valid = false; return index; }
            ++g->pos;
        }
    }
    if (starts(*g, ETCH_STR_LIT(":::"))) {
        g->pos += 3u;
        while (g->pos < g->source.length && (word(g->source.chars[g->pos]) || g->source.chars[g->pos] == '-')) ++g->pos;
    }
    return index;
}

bool read_pipe_label(Graph* g, InlayDiagramEdge* edge) noexcept {
    spaces(g);
    if (!statement_end(*g) && g->source.chars[g->pos] == '|') {
        const uint32_t start = ++g->pos;
        while (g->pos < g->source.length && g->source.chars[g->pos] != '|' && g->source.chars[g->pos] != '\n') ++g->pos;
        if (g->pos == g->source.length || g->source.chars[g->pos] != '|') return false;
        edge->label = strip_outer_quotes(trim_slice(g->source, start, g->pos++));
    }
    return true;
}

bool starts_arrow_hint(const Graph& g) noexcept {
    return starts(g, ETCH_STR_LIT("--")) || starts(g, ETCH_STR_LIT("-.")) ||
           starts(g, ETCH_STR_LIT("==")) || starts(g, ETCH_STR_LIT("<--")) ||
           starts(g, ETCH_STR_LIT("<==")) || starts(g, ETCH_STR_LIT("<-."));
}

bool flow_arrow(Graph* g, InlayDiagramEdge* edge, uint8_t* out_style) noexcept {
    *out_style = InlayEdgeStyleNone;
    edge->dashed = false;

    // 1. Bidirectional arrows: <--> (or <--->), <==>, <-.->
    if (starts(*g, ETCH_STR_LIT("<--"))) {
        uint32_t p = g->pos + 1u;
        while (p < g->source.length && g->source.chars[p] == '-') ++p;
        if (p < g->source.length && g->source.chars[p] == '>') {
            g->pos = p + 1u;
            *out_style = InlayEdgeStyleBidir;
            return read_pipe_label(g, edge);
        }
        return false;
    }
    if (starts(*g, ETCH_STR_LIT("<=="))) {
        uint32_t p = g->pos + 1u;
        while (p < g->source.length && g->source.chars[p] == '=') ++p;
        if (p < g->source.length && g->source.chars[p] == '>') {
            g->pos = p + 1u;
            *out_style = InlayEdgeStyleThick | InlayEdgeStyleBidir;
            return read_pipe_label(g, edge);
        }
        return false;
    }
    if (starts(*g, ETCH_STR_LIT("<-."))) {
        uint32_t p = g->pos + 3u;
        while (p < g->source.length && (g->source.chars[p] == '.' || g->source.chars[p] == '-')) ++p;
        if (p < g->source.length && g->source.chars[p] == '>') {
            g->pos = p + 1u;
            edge->dashed = true;
            *out_style = InlayEdgeStyleBidir;
            return read_pipe_label(g, edge);
        }
        return false;
    }

    // 2. Thick edges: ==>, ===, == label ==>, == label ===
    if (starts(*g, ETCH_STR_LIT("=="))) {
        *out_style = InlayEdgeStyleThick;
        uint32_t p = g->pos;
        while (p < g->source.length && g->source.chars[p] == '=') ++p;
        const uint32_t eqs = p - g->pos;
        if (p < g->source.length && g->source.chars[p] == '>') {
            g->pos = p + 1u;
            return read_pipe_label(g, edge);
        }
        if (eqs >= 3u) {
            g->pos = p;
            *out_style = InlayEdgeStyleThick | InlayEdgeStyleNoArrow;
            return read_pipe_label(g, edge);
        }
        // Embedded thick label: == label ==> or == label ===
        g->pos += 2u;
        const uint32_t start = g->pos;
        while (!statement_end(*g) && !starts(*g, ETCH_STR_LIT("==>")) && !starts(*g, ETCH_STR_LIT("==="))) ++g->pos;
        if (statement_end(*g)) return false;
        edge->label = strip_outer_quotes(trim_slice(g->source, start, g->pos));
        if (starts(*g, ETCH_STR_LIT("==>"))) {
            g->pos += 3u;
        } else {
            while (g->pos < g->source.length && g->source.chars[g->pos] == '=') ++g->pos;
            *out_style = InlayEdgeStyleThick | InlayEdgeStyleNoArrow;
        }
        return edge->label.length != 0;
    }

    // 3. Dashed edges: -.->, -.-, -. label .->, -. label .-
    if (starts(*g, ETCH_STR_LIT("-."))) {
        edge->dashed = true;
        if (starts(*g, ETCH_STR_LIT("-.->"))) {
            g->pos += 4u;
            return read_pipe_label(g, edge);
        }
        if (starts(*g, ETCH_STR_LIT("-.-"))) {
            uint32_t p = g->pos + 3u;
            while (p < g->source.length && g->source.chars[p] == '-') ++p;
            if (p < g->source.length && g->source.chars[p] == '>') {
                g->pos = p + 1u;
            } else {
                g->pos = p;
                *out_style = InlayEdgeStyleNoArrow;
            }
            return read_pipe_label(g, edge);
        }
        g->pos += 2u;
        const uint32_t start = g->pos;
        while (!statement_end(*g) && !starts(*g, ETCH_STR_LIT(".->")) && !starts(*g, ETCH_STR_LIT(".-"))) ++g->pos;
        if (statement_end(*g)) return false;
        edge->label = strip_outer_quotes(trim_slice(g->source, start, g->pos));
        if (starts(*g, ETCH_STR_LIT(".->"))) {
            g->pos += 3u;
        } else {
            g->pos += 2u;
            if (g->pos < g->source.length && g->source.chars[g->pos] == '>') ++g->pos;
            else *out_style = InlayEdgeStyleNoArrow;
        }
        return edge->label.length != 0;
    }

    // 4. Solid edges: --> (or --->), ---, -- label -->, -- label ---
    if (!starts(*g, ETCH_STR_LIT("--"))) return false;
    {
        uint32_t p = g->pos;
        while (p < g->source.length && g->source.chars[p] == '-') ++p;
        const uint32_t dashes = p - g->pos;
        if (p < g->source.length && g->source.chars[p] == '>') {
            g->pos = p + 1u;
            return read_pipe_label(g, edge);
        }
        if (dashes >= 3u) {
            g->pos = p;
            *out_style = InlayEdgeStyleNoArrow;
            return read_pipe_label(g, edge);
        }
    }
    g->pos += 2u;
    const uint32_t start = g->pos;
    while (!statement_end(*g) && !starts(*g, ETCH_STR_LIT("-->")) && !starts(*g, ETCH_STR_LIT("---"))) ++g->pos;
    if (statement_end(*g)) return false;
    edge->label = strip_outer_quotes(trim_slice(g->source, start, g->pos));
    if (starts(*g, ETCH_STR_LIT("-->"))) {
        g->pos += 3u;
    } else {
        uint32_t p = g->pos;
        while (p < g->source.length && g->source.chars[p] == '-') ++p;
        if (p < g->source.length && g->source.chars[p] == '>') {
            g->pos = p + 1u;
        } else {
            g->pos = p;
            *out_style = InlayEdgeStyleNoArrow;
        }
    }
    return edge->label.length != 0;
}

bool match_directive_keyword(const Graph& g, EtchStr kw) noexcept {
    if (!starts(g, kw)) return false;
    if (g.pos + kw.length == g.source.length) return true;
    const char next = g.source.chars[g.pos + kw.length];
    return next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == ';';
}

EtchStr skip_flowchart_directive(Graph* g) noexcept {
    static constexpr EtchStr directives[] = {
        ETCH_STR_LIT("classDef"),
        ETCH_STR_LIT("linkStyle"),
        ETCH_STR_LIT("class"),
        ETCH_STR_LIT("style"),
        ETCH_STR_LIT("direction"),
        ETCH_STR_LIT("click"),
    };
    for (const EtchStr kw : directives) {
        if (!match_directive_keyword(*g, kw)) continue;
        const uint32_t saved = g->pos;
        g->pos += kw.length;
        spaces(g);
        // Only treat as directive if not immediately followed by node shape or edge arrow.
        if (!statement_end(*g)) {
            const char c = g->source.chars[g->pos];
            if (c == '[' || c == '(' || c == '{' || starts_arrow_hint(*g)) {
                g->pos = saved;
                return {};
            }
        }
        while (!statement_end(*g)) ++g->pos;
        return kw;
    }
    return {};
}

EtchVec2 curve_point(const EtchBezierDesc& b, float t) noexcept {
    const float u = 1.0f - t, a = u * u * u, c = 3.0f * u * u * t, d = 3.0f * u * t * t, e = t * t * t;
    return {
        a * b.p0.x + c * b.p1.x + d * b.p2.x + e * b.p3.x,
        a * b.p0.y + c * b.p1.y + d * b.p2.y + e * b.p3.y
    };
}

EtchVec2 curve_tangent(const EtchBezierDesc& b, float t) noexcept {
    const float u = 1.0f - t, a = u * u, c = 2.0f * u * t, d = t * t;
    return {
        a * (b.p1.x - b.p0.x) + c * (b.p2.x - b.p1.x) + d * (b.p3.x - b.p2.x),
        a * (b.p1.y - b.p0.y) + c * (b.p2.y - b.p1.y) + d * (b.p3.y - b.p2.y)
    };
}

void draw_dashed(const EtchBezierDesc& b, float scale) noexcept {
    constexpr uint32_t samples = 32;
    float distance[samples + 1] = {};
    auto previous = b.p0;
    for (uint32_t i = 1; i <= samples; ++i) {
        const auto point = curve_point(b, float(i) / samples);
        distance[i] = distance[i - 1] + std::hypot(point.x - previous.x, point.y - previous.y);
        previous = point;
    }
    const float length = distance[samples];
    if (length <= 0.0f) return;
    const float period = std::fmax(12.0f * scale, length / 128.0f);
    uint32_t sample = 1;
    const auto parameter = [&](float at) noexcept {
        while (sample < samples && distance[sample] <= at) ++sample;
        const float step = distance[sample] - distance[sample - 1];
        return step > 0.0f ? (float(sample - 1) + (at - distance[sample - 1]) / step) / samples : 1.0f;
    };
    for (float start = 0.0f; start < length; start += period) {
        const float t0 = parameter(start), t1 = parameter(std::fmin(length, start + period * 2.0f / 3.0f));
        const float dt = t1 - t0;
        auto dash = b;
        dash.p0 = curve_point(b, t0);
        dash.p3 = curve_point(b, t1);
        const auto v0 = curve_tangent(b, t0), v1 = curve_tangent(b, t1);
        dash.p1 = { dash.p0.x + v0.x * dt, dash.p0.y + v0.y * dt };
        dash.p2 = { dash.p3.x - v1.x * dt, dash.p3.y - v1.y * dt };
        etch_draw_bezier(&dash);
    }
}

EtchColor shift_color(EtchColor c, int delta) noexcept {
    const auto ch = [delta](uint8_t v) noexcept -> uint8_t {
        const int s = int(v) + delta;
        return uint8_t(s < 0 ? 0 : (s > 255 ? 255 : s));
    };
    return { ch(c.r), ch(c.g), ch(c.b), c.a };
}

void draw_arrowhead(EtchVec2 tip, EtchVec2 tangent, EtchColor color, float scale) noexcept {
    const float len = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
    if (len <= 0.001f) return;
    tangent.x /= len;
    tangent.y /= len;
    const float alen = std::fmax(6.0f, 8.5f * scale);
    const float ahalf = std::fmax(3.0f, 4.2f * scale);
    const float notch = alen * 0.78f;
    EtchVec2 points[] = {
        tip,
        { tip.x - tangent.x * alen - tangent.y * ahalf, tip.y - tangent.y * alen + tangent.x * ahalf },
        { tip.x - tangent.x * notch, tip.y - tangent.y * notch },
        { tip.x - tangent.x * alen + tangent.y * ahalf, tip.y - tangent.y * alen - tangent.x * ahalf },
    };
    EtchFilledPolygonDesc arrow = {};
    arrow.points = points;
    arrow.point_count = 4;
    arrow.fill = color;
    etch_draw_filled_polygon(&arrow);
}
InlayDiagramResult diagram_layout_core(EtchStr source, const InlayDiagramDesc* input, InlayDiagramMeta* out_meta, const DiagramEnv& env) noexcept {
    if (out_meta) *out_meta = {};
    if (!input || !source.chars || source.length > 16384 ||
        !std::isfinite(input->pos.x) || !std::isfinite(input->pos.y) ||
        !std::isfinite(input->font_size_px) || input->font_size_px < 6 || input->font_size_px > 128 ||
        !std::isfinite(input->node_gap_px) || input->node_gap_px < 0 || input->node_gap_px > 4096 ||
        !std::isfinite(input->rank_gap_px) || input->rank_gap_px < 0 || input->rank_gap_px > 4096 ||
        !std::isfinite(input->max_width_px) || input->max_width_px < 0 || input->max_width_px > 16384 ||
        !std::isfinite(input->min_scale) || input->min_scale < 0 || input->min_scale > 1.0f ||
        (input->canvas_id && (!std::isfinite(input->viewport_width_px) || !std::isfinite(input->viewport_height_px) ||
                              input->viewport_width_px < 1 || input->viewport_height_px < 1)))
    {
        return {};
    }
    const float min_scale = (input->min_scale > 0.0f) ? std::fmax(0.25f, std::fmin(1.0f, input->min_scale)) : 0.62f;
    const uint16_t body_font = resolve_body_font(*input);
    const uint16_t bold_font = resolve_bold_font(*input);

    Graph g = {};
    g.source = source;
    g.active_subgraph = -1;
    g.valid = true;
    uint32_t warnings = 0u;
    const auto diagnostic = [&](InlayDiagramDiagnosticKind kind, uint32_t offset, uint32_t length) noexcept {
        if (input->diagnostic) input->diagnostic(kind, offset, length, input->diagnostic_user_data);
    };
    const auto syntax_failure = [&](uint32_t offset) noexcept {
        diagnostic(INLAY_DIAGRAM_INVALID_SYNTAX, offset, 0u);
        InlayDiagramResult result = {};
        result.warning_count = warnings;
        return result;
    };
    // Skip leading whitespace, blank lines, and Mermaid %% comment/init lines.
    while (g.pos < source.length) {
        spaces(&g);
        if (g.pos < source.length && source.chars[g.pos] == '\n') { ++g.pos; continue; }
        if (starts(g, ETCH_STR_LIT("%%"))) {
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            continue;
        }
        break;
    }
    const uint32_t header = g.pos;
    while (g.pos < source.length && (word(source.chars[g.pos]) || source.chars[g.pos] == '-')) ++g.pos;
    const EtchStr kind = { source.chars + header, g.pos - header, false };
    const bool sequence = equal(kind, ETCH_STR_LIT("sequenceDiagram"));
    const bool state_diag = equal(kind, ETCH_STR_LIT("stateDiagram-v2")) || equal(kind, ETCH_STR_LIT("stateDiagram"));
    const bool class_diag = equal(kind, ETCH_STR_LIT("classDiagram")) || equal(kind, ETCH_STR_LIT("classDiagram-v2"));
    const bool er_diag = equal(kind, ETCH_STR_LIT("erDiagram"));
    const bool pie_diag = equal(kind, ETCH_STR_LIT("pie"));
    const bool gantt_diag = equal(kind, ETCH_STR_LIT("gantt"));
    const bool git_diag = equal(kind, ETCH_STR_LIT("gitGraph"));
    const bool mindmap_diag = equal(kind, ETCH_STR_LIT("mindmap"));
    const bool timeline_diag = equal(kind, ETCH_STR_LIT("timeline"));

    if (!sequence && !state_diag && !class_diag && !er_diag && !pie_diag && !gantt_diag &&
        !git_diag && !mindmap_diag && !timeline_diag) {
        if (!equal(kind, ETCH_STR_LIT("flowchart")) && !equal(kind, ETCH_STR_LIT("graph"))) {
            diagnostic(INLAY_DIAGRAM_UNSUPPORTED_KIND, header, kind.length);
            return {};
        }
        spaces(&g);
        const uint32_t direction = g.pos;
        while (g.pos < source.length && word(source.chars[g.pos])) ++g.pos;
        const EtchStr dir = { source.chars + direction, g.pos - direction, false };
        const bool is_lr = equal(dir, ETCH_STR_LIT("LR"));
        const bool is_rl = equal(dir, ETCH_STR_LIT("RL"));
        const bool is_td = equal(dir, ETCH_STR_LIT("TD")) || equal(dir, ETCH_STR_LIT("TB"));
        const bool is_bt = equal(dir, ETCH_STR_LIT("BT"));
        if (!is_lr && !is_rl && !is_td && !is_bt) {
            diagnostic(INLAY_DIAGRAM_INVALID_DIRECTION, direction, dir.length);
            return {};
        }
        g.lr = is_lr || is_rl;
        g.reverse_dir = is_rl || is_bt;
    }

    // ---- Mermaid pie chart ----
    if (pie_diag) {
        bool show_data = false;
        EtchStr title = {};
        // Optional "showData" or "title ..." on same line after "pie"
        spaces(&g);
        if (starts(g, ETCH_STR_LIT("showData"))) {
            show_data = true;
            g.pos += 8u;
            spaces(&g);
        }
        if (starts(g, ETCH_STR_LIT("title")) && g.pos + 5u < source.length &&
            (source.chars[g.pos + 5u] == ' ' || source.chars[g.pos + 5u] == '\t'))
        {
            g.pos += 5u;
            const uint32_t ts = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            title = trim_slice(source, ts, g.pos);
        }
        constexpr uint32_t kMaxSlices = 32u;
        auto* slices = static_cast<InlayDiagramPieSlice*>(env.alloc(kMaxSlices * sizeof(InlayDiagramPieSlice), alignof(InlayDiagramPieSlice)));
        auto* pnodes = static_cast<InlayDiagramNode*>(env.alloc(kMaxSlices * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
        auto* pshapes = static_cast<InlayDiagramShape*>(env.alloc(kMaxSlices * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
        if (!slices || !pnodes || !pshapes) return {};
        uint32_t slice_count = 0u;
        float total_val = 0.0f;
        while (g.pos < source.length) {
            spaces(&g);
            if (g.pos >= source.length) break;
            if (source.chars[g.pos] == '\n' || source.chars[g.pos] == ';') { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("title")) && g.pos + 5u < source.length &&
                (source.chars[g.pos + 5u] == ' ' || source.chars[g.pos + 5u] == '\t'))
            {
                g.pos += 5u;
                const uint32_t ts = g.pos;
                while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
                title = trim_slice(source, ts, g.pos);
                continue;
            }
            if (starts(g, ETCH_STR_LIT("showData"))) {
                show_data = true;
                while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
                continue;
            }
            if (source.chars[g.pos] != '"') {
                while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
                continue;
            }
            ++g.pos;
            const uint32_t ls = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '"' && source.chars[g.pos] != '\n') ++g.pos;
            if (g.pos >= source.length || source.chars[g.pos] != '"') return syntax_failure(ls);
            const EtchStr lbl = { source.chars + ls, g.pos - ls, false };
            ++g.pos;
            spaces(&g);
            if (g.pos >= source.length || source.chars[g.pos] != ':') return syntax_failure(g.pos);
            ++g.pos;
            spaces(&g);
            const uint32_t vs = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n' && source.chars[g.pos] != ';') ++g.pos;
            const EtchStr vstr = trim_slice(source, vs, g.pos);
            char num_buf[48] = {};
            const uint32_t vn = vstr.length < 47u ? vstr.length : 47u;
            if (vn > 0u) std::memcpy(num_buf, vstr.chars, vn);
            num_buf[vn] = '\0';
            char* endp = nullptr;
            const float val = std::strtof(num_buf, &endp);
            if (endp == num_buf || !std::isfinite(val) || val < 0.0f) return syntax_failure(vs);
            if (slice_count < kMaxSlices) {
                slices[slice_count] = { lbl, val, 0.0f, 0.0f, uint8_t(slice_count % 8u) };
                pnodes[slice_count] = { lbl, lbl, {}, 0u };
                pshapes[slice_count] = InlayShapePieSlice;
                total_val += val;
                ++slice_count;
            }
        }
        if (slice_count == 0u || total_val <= 0.0f) return syntax_failure(g.pos);
        constexpr float kTwoPi = 6.28318530718f;
        float cur_ang = -1.57079632679f;
        for (uint32_t i = 0u; i < slice_count; ++i) {
            const float sweep = kTwoPi * (slices[i].value / total_val);
            slices[i].start_rad = cur_ang;
            slices[i].sweep_rad = sweep;
            cur_ang += sweep;
            if (show_data) {
                char tmp[128] = {};
                const float pct = (slices[i].value / total_val) * 100.0f;
                const int n = std::snprintf(tmp, sizeof(tmp), "%.*s (%.1f, %.0f%%)",
                    int(slices[i].label.length < 72u ? slices[i].label.length : 72u),
                    slices[i].label.chars ? slices[i].label.chars : "", double(slices[i].value), double(pct));
                if (n > 0) {
                    const uint32_t clen = uint32_t(n < int(sizeof(tmp) - 1) ? n : int(sizeof(tmp) - 1));
                    if (char* copy = static_cast<char*>(env.alloc(clen + 1u, 1u))) {
                        std::memcpy(copy, tmp, clen);
                        copy[clen] = '\0';
                        pnodes[i].label = { copy, clen, false };
                    }
                }
            } else {
                char tmp[128] = {};
                const float pct = (slices[i].value / total_val) * 100.0f;
                const int n = std::snprintf(tmp, sizeof(tmp), "%.*s (%.0f%%)",
                    int(slices[i].label.length < 88u ? slices[i].label.length : 88u),
                    slices[i].label.chars ? slices[i].label.chars : "", double(pct));
                if (n > 0) {
                    const uint32_t clen = uint32_t(n < int(sizeof(tmp) - 1) ? n : int(sizeof(tmp) - 1));
                    if (char* copy = static_cast<char*>(env.alloc(clen + 1u, 1u))) {
                        std::memcpy(copy, tmp, clen);
                        copy[clen] = '\0';
                        pnodes[i].label = { copy, clen, false };
                    }
                }
            }
        }
        EtchVec2 pie_center = {};
        float pie_radius = 74.0f;
        const auto place_pie = [&](float s) noexcept -> EtchVec2 {
            const float eff_font = std::fmax(8.0f, input->font_size_px * s);
            const float title_h = title.length ? (eff_font * 1.8f + 8.0f * s) : 0.0f;
            pie_radius = 76.0f * s;
            const float pad = 14.0f * s;
            const float row_h = std::fmax(eff_font * 1.45f, 18.0f * s);
            const float legend_h = float(slice_count) * row_h;
            const float body_h = std::fmax(pie_radius * 2.0f, legend_h);
            pie_center = { input->pos.x + pad + pie_radius, input->pos.y + title_h + pad + body_h * 0.5f };
            const float legend_x = input->pos.x + pad + pie_radius * 2.0f + 24.0f * s;
            const float legend_y0 = input->pos.y + title_h + pad + (body_h - legend_h) * 0.5f;
            float max_lw = 80.0f * s;
            for (uint32_t i = 0u; i < slice_count; ++i) {
                const float tw = env.measure_text(pnodes[i].label, body_font, eff_font * 0.9f).x + 22.0f * s;
                if (tw > max_lw) max_lw = tw;
                pnodes[i].bounds = { legend_x, legend_y0 + float(i) * row_h, tw, row_h };
            }
            const float title_w = title.length ? (env.measure_text(title, bold_font, eff_font * 1.1f).x + 2.0f * pad) : 0.0f;
            const float total_w = std::fmax(title_w, (legend_x - input->pos.x) + max_lw + pad);
            const float total_h = title_h + body_h + 2.0f * pad;
            return { total_w, total_h };
        };
        const EtchVec2 nat = place_pie(1.0f);
        float final_scale = 1.0f;
        EtchVec2 final_sz = nat;
        if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
            float s = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
            final_sz = place_pie(s);
            final_scale = s;
        }
        if (out_meta) {
            out_meta->shapes = pshapes;
            out_meta->diagram_kind = InlayDiagramPie;
            out_meta->title = title;
            out_meta->pie_center = pie_center;
            out_meta->pie_radius = pie_radius;
            out_meta->pie_slices = slices;
            out_meta->pie_slice_count = slice_count;
        }
        return { pnodes, nullptr, slice_count, 0u, final_sz.x, final_sz.y, true, false, false, final_scale, nat.x, nat.y, warnings };
    }

    // ---- Mermaid gantt chart ----
    if (gantt_diag) {
        EtchStr title = {};
        EtchStr cur_section = {};
        bool cur_section_fresh = false;
        constexpr uint32_t kMaxTasks = 64u;
        struct GanttTaskTmp {
            EtchStr id;
            EtchStr label;
            EtchStr section;
            float start_u;
            float dur_u;
            uint8_t status;
            bool section_first;
        };
        auto* tasks = static_cast<GanttTaskTmp*>(env.alloc(kMaxTasks * sizeof(GanttTaskTmp), alignof(GanttTaskTmp)));
        auto* gnodes = static_cast<InlayDiagramNode*>(env.alloc(kMaxTasks * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
        auto* gshapes = static_cast<InlayDiagramShape*>(env.alloc(kMaxTasks * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
        auto* gbars = static_cast<InlayDiagramGanttBar*>(env.alloc(kMaxTasks * sizeof(InlayDiagramGanttBar), alignof(InlayDiagramGanttBar)));
        if (!tasks || !gnodes || !gshapes || !gbars) return {};
        uint32_t task_count = 0u;
        float cursor_u = 0.0f;
        bool has_base_day = false;
        int32_t base_day = 0;
        const auto parse_ymd_days = [](EtchStr s, int32_t* out_days) noexcept -> bool {
            if (s.length < 10u || s.chars[4] != '-' || s.chars[7] != '-') return false;
            auto rd = [&](uint32_t off, uint32_t cnt) noexcept -> int32_t {
                int32_t v = 0;
                for (uint32_t i = 0u; i < cnt; ++i) {
                    const char ch = s.chars[off + i];
                    if (ch < '0' || ch > '9') return -1;
                    v = v * 10 + (ch - '0');
                }
                return v;
            };
            const int32_t y = rd(0u, 4u), m = rd(5u, 2u), d = rd(8u, 2u);
            if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
            *out_days = y * 365 + (m - 1) * 30 + (d - 1);
            return true;
        };
        while (g.pos < source.length) {
            spaces(&g);
            if (g.pos >= source.length) break;
            if (source.chars[g.pos] == '\n') { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            const uint32_t ls = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            const EtchStr line = trim_slice(source, ls, g.pos);
            if (line.length == 0u) continue;
            auto starts_kw = [&](const char* kw) noexcept -> bool {
                const uint32_t kl = static_cast<uint32_t>(std::strlen(kw));
                return line.length > kl && std::memcmp(line.chars, kw, kl) == 0 &&
                       (line.chars[kl] == ' ' || line.chars[kl] == '\t');
            };
            if (starts_kw("title")) {
                title = trim_slice(line, 5u, line.length);
                continue;
            }
            if (starts_kw("section")) {
                cur_section = trim_slice(line, 7u, line.length);
                cur_section_fresh = true;
                continue;
            }
            if (starts_kw("dateFormat") || starts_kw("axisFormat") || starts_kw("excludes") ||
                starts_kw("todayMarker") || starts_kw("tickInterval") || starts_kw("weekday")) {
                continue;
            }
            uint32_t colon = UINT32_MAX;
            for (uint32_t ci = 0u; ci < line.length; ++ci) {
                if (line.chars[ci] == ':') { colon = ci; break; }
            }
            if (colon == UINT32_MAX || task_count >= kMaxTasks) continue;
            const EtchStr tlabel = trim_slice(line, 0u, colon);
            if (tlabel.length == 0u) continue;
            const EtchStr meta_s = trim_slice(line, colon + 1u, line.length);
            uint8_t status = 0u;
            EtchStr tid = {};
            float start_u = cursor_u;
            float dur_u = 3.0f;
            bool explicit_start = false;
            uint32_t mp = 0u;
            while (mp <= meta_s.length) {
                uint32_t me = mp;
                while (me < meta_s.length && meta_s.chars[me] != ',') ++me;
                const EtchStr tok = trim_slice(meta_s, mp, me);
                if (tok.length > 0u) {
                    int32_t ymd = 0;
                    if (equal(tok, ETCH_STR_LIT("done"))) status = 1u;
                    else if (equal(tok, ETCH_STR_LIT("active"))) status = 2u;
                    else if (equal(tok, ETCH_STR_LIT("crit"))) status = 3u;
                    else if (equal(tok, ETCH_STR_LIT("milestone"))) { dur_u = 0.6f; }
                    else if (tok.length > 6u && std::memcmp(tok.chars, "after ", 6u) == 0) {
                        const EtchStr ref = trim_slice(tok, 6u, tok.length);
                        for (uint32_t ti = 0u; ti < task_count; ++ti) {
                            if (equal(tasks[ti].id, ref) || equal(tasks[ti].label, ref)) {
                                start_u = tasks[ti].start_u + tasks[ti].dur_u;
                                explicit_start = true;
                                break;
                            }
                        }
                    } else if (parse_ymd_days(tok, &ymd)) {
                        if (!has_base_day) { has_base_day = true; base_day = ymd; }
                        start_u = float(ymd - base_day);
                        explicit_start = true;
                    } else if (tok.chars[0] >= '0' && tok.chars[0] <= '9') {
                        char nb[32] = {};
                        const uint32_t nn = tok.length < 31u ? tok.length : 31u;
                        std::memcpy(nb, tok.chars, nn);
                        char* ep = nullptr;
                        const float v = std::strtof(nb, &ep);
                        if (ep > nb && std::isfinite(v) && v > 0.0f) {
                            float mult = 1.0f;
                            if (*ep == 'w' || *ep == 'W') mult = 7.0f;
                            else if (*ep == 'h' || *ep == 'H') mult = 1.0f / 24.0f;
                            dur_u = std::fmax(0.4f, v * mult);
                        }
                    } else if (tid.length == 0u) {
                        tid = tok;
                    }
                }
                if (me >= meta_s.length) break;
                mp = me + 1u;
            }
            if (!explicit_start) start_u = cursor_u;
            cursor_u = start_u + dur_u;
            tasks[task_count] = { tid.length ? tid : tlabel, tlabel, cur_section, start_u, dur_u, status, cur_section_fresh };
            cur_section_fresh = false;
            ++task_count;
        }
        if (task_count == 0u) return syntax_failure(g.pos);
        float min_u = tasks[0].start_u, max_u = tasks[0].start_u + tasks[0].dur_u;
        for (uint32_t i = 1u; i < task_count; ++i) {
            if (tasks[i].start_u < min_u) min_u = tasks[i].start_u;
            if (tasks[i].start_u + tasks[i].dur_u > max_u) max_u = tasks[i].start_u + tasks[i].dur_u;
        }
        const float span_u = std::fmax(1.0f, max_u - min_u);
        float gantt_lbl_w = 120.0f;
        const auto place_gantt = [&](float s) noexcept -> EtchVec2 {
            const float eff_font = std::fmax(8.0f, input->font_size_px * s);
            const float pad = 14.0f * s;
            const float title_h = title.length ? (eff_font * 1.8f + 6.0f * s) : 0.0f;
            const float sec_h = eff_font * 1.45f + 6.0f * s;
            const float row_h = std::fmax(eff_font * 1.65f, 24.0f * s);
            float lw = 96.0f * s;
            for (uint32_t i = 0u; i < task_count; ++i) {
                const float tw = env.measure_text(tasks[i].label, body_font, eff_font * 0.92f).x + 20.0f * s;
                if (tw > lw) lw = tw;
            }
            if (lw > 210.0f * s) lw = 210.0f * s;
            gantt_lbl_w = lw;
            const float chart_w = std::fmax(240.0f * s, std::fmin(520.0f * s, span_u * 26.0f * s));
            const float total_w = pad * 2.0f + lw + chart_w;
            float cur_y = input->pos.y + pad + title_h;
            for (uint32_t i = 0u; i < task_count; ++i) {
                if (tasks[i].section_first && tasks[i].section.length > 0u) {
                    cur_y += sec_h;
                }
                const float bx = input->pos.x + pad + lw + ((tasks[i].start_u - min_u) / span_u) * chart_w;
                const float bw = std::fmax(10.0f * s, (tasks[i].dur_u / span_u) * chart_w);
                gnodes[i] = { tasks[i].id, tasks[i].label, { input->pos.x + pad, cur_y, lw + chart_w, row_h }, 0u };
                gshapes[i] = InlayShapeGanttTask;
                gbars[i] = { tasks[i].section, { bx, cur_y + 3.0f * s, bw, row_h - 6.0f * s }, tasks[i].status, tasks[i].section_first };
                cur_y += row_h + 2.0f * s;
            }
            const float total_h = (cur_y - input->pos.y) + pad;
            return { total_w, total_h };
        };
        const EtchVec2 nat = place_gantt(1.0f);
        float final_scale = 1.0f;
        EtchVec2 final_sz = nat;
        if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
            float s = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
            final_sz = place_gantt(s);
            final_scale = s;
        }
        if (out_meta) {
            out_meta->shapes = gshapes;
            out_meta->diagram_kind = InlayDiagramGantt;
            out_meta->title = title;
            out_meta->gantt_bars = gbars;
            out_meta->gantt_bar_count = task_count;
            out_meta->gantt_label_width = gantt_lbl_w;
        }
        return { gnodes, nullptr, task_count, 0u, final_sz.x, final_sz.y, true, true, false, final_scale, nat.x, nat.y, warnings };
    }

    // ---- Mermaid gitGraph ----
    if (git_diag) {
        while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
        constexpr uint32_t kMaxBranches = 16u;
        constexpr uint32_t kMaxCommits = 128u;
        constexpr uint32_t kMaxGitEdges = 256u;
        struct BranchRec { EtchStr name; int32_t head; };
        BranchRec branches[kMaxBranches] = {};
        uint32_t branch_count = 1u;
        branches[0] = { ETCH_STR_LIT("main"), -1 };
        uint32_t cur_branch = 0u;
        auto* cnodes = static_cast<InlayDiagramNode*>(env.alloc(kMaxCommits * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
        auto* cshapes = static_cast<InlayDiagramShape*>(env.alloc(kMaxCommits * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
        auto* cedges = static_cast<InlayDiagramEdge*>(env.alloc(kMaxGitEdges * sizeof(InlayDiagramEdge), alignof(InlayDiagramEdge)));
        auto* cstyles = static_cast<uint8_t*>(env.alloc(kMaxGitEdges, 1));
        auto* blanes = static_cast<InlayDiagramSubgraph*>(env.alloc(kMaxBranches * sizeof(InlayDiagramSubgraph), alignof(InlayDiagramSubgraph)));
        if (!cnodes || !cshapes || !cedges || !cstyles || !blanes) return {};
        uint32_t commit_count = 0u, edge_count = 0u;

        auto find_or_add_branch = [&](EtchStr bname, bool create_if_missing) noexcept -> int32_t {
            for (uint32_t bi = 0u; bi < branch_count; ++bi) {
                if (equal(branches[bi].name, bname)) return static_cast<int32_t>(bi);
            }
            if (!create_if_missing || branch_count >= kMaxBranches) return -1;
            const uint32_t ni = branch_count++;
            branches[ni] = { bname, branches[cur_branch].head };
            return static_cast<int32_t>(ni);
        };
        auto extract_quoted_field = [&](EtchStr line, const char* key) noexcept -> EtchStr {
            const uint32_t kl = static_cast<uint32_t>(std::strlen(key));
            for (uint32_t i = 0u; i + kl + 2u < line.length; ++i) {
                if (std::memcmp(line.chars + i, key, kl) == 0 && (i == 0u || line.chars[i - 1u] == ' ' || line.chars[i - 1u] == '\t')) {
                    uint32_t p = i + kl;
                    while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
                    if (p < line.length && line.chars[p] == ':') {
                        ++p;
                        while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
                        if (p < line.length && (line.chars[p] == '"' || line.chars[p] == '\'')) {
                            const char q = line.chars[p++];
                            const uint32_t vs = p;
                            while (p < line.length && line.chars[p] != q) ++p;
                            return { line.chars + vs, p - vs, false };
                        }
                    }
                }
            }
            return {};
        };

        while (g.pos < source.length) {
            spaces(&g);
            if (g.pos >= source.length) break;
            if (source.chars[g.pos] == '\n') { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            const uint32_t ls = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            const EtchStr line = trim_slice(source, ls, g.pos);
            if (line.length == 0u) continue;
            uint32_t kw_end = 0u;
            while (kw_end < line.length && line.chars[kw_end] != ' ' && line.chars[kw_end] != '\t' && line.chars[kw_end] != ':') ++kw_end;
            const EtchStr kw = { line.chars, kw_end, false };
            const EtchStr rest = trim_slice(line, kw_end, line.length);
            if (equal(kw, ETCH_STR_LIT("branch")) && rest.length > 0u) {
                uint32_t ne = 0u;
                while (ne < rest.length && rest.chars[ne] != ' ' && rest.chars[ne] != '\t') ++ne;
                const EtchStr bname = { rest.chars, ne, false };
                const int32_t bi = find_or_add_branch(bname, true);
                if (bi >= 0) cur_branch = static_cast<uint32_t>(bi);
            } else if ((equal(kw, ETCH_STR_LIT("checkout")) || equal(kw, ETCH_STR_LIT("switch"))) && rest.length > 0u) {
                uint32_t ne = 0u;
                while (ne < rest.length && rest.chars[ne] != ' ' && rest.chars[ne] != '\t') ++ne;
                const int32_t bi = find_or_add_branch({ rest.chars, ne, false }, true);
                if (bi >= 0) cur_branch = static_cast<uint32_t>(bi);
            } else if ((equal(kw, ETCH_STR_LIT("commit")) || equal(kw, ETCH_STR_LIT("merge")) || equal(kw, ETCH_STR_LIT("cherry-pick"))) &&
                       commit_count < kMaxCommits) {
                const bool is_merge = equal(kw, ETCH_STR_LIT("merge"));
                int32_t merge_from_head = -1;
                if (is_merge && rest.length > 0u) {
                    uint32_t ne = 0u;
                    while (ne < rest.length && rest.chars[ne] != ' ' && rest.chars[ne] != '\t') ++ne;
                    const int32_t mbi = find_or_add_branch({ rest.chars, ne, false }, false);
                    if (mbi >= 0) merge_from_head = branches[mbi].head;
                }
                EtchStr cid = extract_quoted_field(line, "id");
                const EtchStr tag = extract_quoted_field(line, "tag");
                if (cid.length == 0u) {
                    auto* idbuf = static_cast<char*>(env.alloc(16u, 1u));
                    if (idbuf) {
                        const int n = std::snprintf(idbuf, 16u, "c%u", commit_count + 1u);
                        if (n > 0) cid = { idbuf, static_cast<uint32_t>(n), false };
                    }
                }
                EtchStr lbl = cid;
                if (tag.length > 0u) {
                    const uint32_t llen = cid.length + tag.length + 3u;
                    auto* lbuf = static_cast<char*>(env.alloc(llen, 1u));
                    if (lbuf) {
                        std::memcpy(lbuf, cid.chars, cid.length);
                        lbuf[cid.length] = '\n';
                        lbuf[cid.length + 1u] = '[';
                        std::memcpy(lbuf + cid.length + 2u, tag.chars, tag.length);
                        lbuf[cid.length + 2u + tag.length] = ']';
                        lbl = { lbuf, llen, false };
                    }
                }
                const uint32_t ci = commit_count++;
                cnodes[ci] = { cid, lbl, {}, cur_branch };
                cshapes[ci] = InlayShapeGitCommit;
                if (branches[cur_branch].head >= 0 && edge_count < kMaxGitEdges) {
                    cedges[edge_count] = { static_cast<uint32_t>(branches[cur_branch].head), ci, false, false, {} };
                    cstyles[edge_count] = InlayEdgeStyleNoArrow;
                    ++edge_count;
                }
                if (is_merge && merge_from_head >= 0 && merge_from_head != branches[cur_branch].head && edge_count < kMaxGitEdges) {
                    cedges[edge_count] = { static_cast<uint32_t>(merge_from_head), ci, false, true, {} };
                    cstyles[edge_count] = InlayEdgeStyleNoArrow;
                    ++edge_count;
                }
                branches[cur_branch].head = static_cast<int32_t>(ci);
            }
        }
        if (commit_count == 0u) return syntax_failure(g.pos);
        const auto place_git = [&](float s) noexcept -> EtchVec2 {
            const float eff_font = std::fmax(8.0f, input->font_size_px * s);
            const float pad = 16.0f * s;
            float lbl_w = 64.0f * s;
            for (uint32_t bi = 0u; bi < branch_count; ++bi) {
                const float tw = env.measure_text(branches[bi].name, bold_font, eff_font * 0.85f).x + 22.0f * s;
                if (tw > lbl_w) lbl_w = tw;
            }
            const float step_x = std::fmax(56.0f * s, eff_font * 3.8f);
            const float lane_h = std::fmax(52.0f * s, eff_font * 3.4f);
            const float r_dia = std::fmax(14.0f * s, eff_font * 1.05f);
            const float start_x = input->pos.x + pad + lbl_w + 18.0f * s;
            const float total_w = pad * 2.0f + lbl_w + 18.0f * s + float(commit_count) * step_x + 20.0f * s;
            const float total_h = pad * 2.0f + float(branch_count) * lane_h + 12.0f * s;
            for (uint32_t bi = 0u; bi < branch_count; ++bi) {
                const float ly = input->pos.y + pad + float(bi) * lane_h + lane_h * 0.38f;
                blanes[bi] = { branches[bi].name, { input->pos.x + pad, ly - 11.0f * s, total_w - pad * 2.0f, 22.0f * s }, -1, true };
            }
            for (uint32_t ci = 0u; ci < commit_count; ++ci) {
                const uint32_t bi = cnodes[ci].rank;
                const float cx = start_x + float(ci) * step_x;
                const float cy = input->pos.y + pad + float(bi) * lane_h + lane_h * 0.38f;
                cnodes[ci].bounds = { cx - r_dia * 0.5f, cy - r_dia * 0.5f, r_dia, r_dia };
            }
            return { total_w, total_h };
        };
        const EtchVec2 nat = place_git(1.0f);
        float final_scale = 1.0f;
        EtchVec2 final_sz = nat;
        if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
            final_scale = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
            final_sz = place_git(final_scale);
        }
        if (out_meta) {
            out_meta->shapes = cshapes;
            out_meta->edge_styles = cstyles;
            out_meta->subgraphs = blanes;
            out_meta->subgraph_count = branch_count;
            out_meta->diagram_kind = InlayDiagramGitGraph;
        }
        return { cnodes, cedges, commit_count, edge_count, final_sz.x, final_sz.y, true, true, false, final_scale, nat.x, nat.y, warnings };
    }

    // ---- Mermaid mindmap ----
    if (mindmap_diag) {
        while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
        constexpr uint32_t kMaxMindNodes = 128u;
        struct MindItem { EtchStr label; int32_t parent; uint32_t depth; float sub_h; };
        auto* items = static_cast<MindItem*>(env.alloc(kMaxMindNodes * sizeof(MindItem), alignof(MindItem)));
        auto* mnodes = static_cast<InlayDiagramNode*>(env.alloc(kMaxMindNodes * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
        auto* mshapes = static_cast<InlayDiagramShape*>(env.alloc(kMaxMindNodes * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
        auto* medges = static_cast<InlayDiagramEdge*>(env.alloc(kMaxMindNodes * sizeof(InlayDiagramEdge), alignof(InlayDiagramEdge)));
        auto* mstyles = static_cast<uint8_t*>(env.alloc(kMaxMindNodes, 1));
        if (!items || !mnodes || !mshapes || !medges || !mstyles) return {};
        uint32_t mcount = 0u, ecount = 0u;
        struct StackEnt { uint32_t indent; uint32_t idx; };
        StackEnt stk[32] = {};
        uint32_t sp = 0u;
        auto strip_mind_shape = [](EtchStr s) noexcept -> EtchStr {
            // Strip leading `root` prefix before `((...))` / `(...)` / `[...]`
            if (s.length > 4u && std::memcmp(s.chars, "root", 4u) == 0 &&
                (s.chars[4] == '(' || s.chars[4] == '[' || s.chars[4] == '{')) {
                s = { s.chars + 4u, s.length - 4u, false };
            }
            for (int pass = 0; pass < 2 && s.length >= 2u; ++pass) {
                const char a = s.chars[0], b = s.chars[s.length - 1u];
                if ((a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') || (a == ')' && b == '(')) {
                    s = { s.chars + 1u, s.length - 2u, false };
                } else break;
            }
            return trim_slice(s, 0u, s.length);
        };
        while (g.pos < source.length && mcount < kMaxMindNodes) {
            const uint32_t ls = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            const uint32_t le = g.pos;
            if (g.pos < source.length) ++g.pos;
            uint32_t ind = 0u, p = ls;
            while (p < le && (source.chars[p] == ' ' || source.chars[p] == '\t')) {
                ind += (source.chars[p] == '\t') ? 2u : 1u;
                ++p;
            }
            const EtchStr raw = trim_slice(source, p, le);
            if (raw.length == 0u || (raw.length >= 2u && raw.chars[0] == '%' && raw.chars[1] == '%')) continue;
            if (raw.length >= 2u && raw.chars[0] == ':' && raw.chars[1] == ':') continue; // skip ::icon(...)
            const EtchStr lbl = strip_mind_shape(raw);
            if (lbl.length == 0u) continue;
            while (sp > 0u && stk[sp - 1u].indent >= ind) --sp;
            const int32_t par = sp > 0u ? static_cast<int32_t>(stk[sp - 1u].idx) : -1;
            const uint32_t dep = sp;
            const uint32_t ni = mcount++;
            items[ni] = { lbl, par, dep, 0.0f };
            mnodes[ni] = { lbl, lbl, {}, dep };
            mshapes[ni] = InlayShapeMindmapNode;
            if (par >= 0 && ecount < kMaxMindNodes) {
                medges[ecount] = { static_cast<uint32_t>(par), ni, false, false, {} };
                mstyles[ecount] = InlayEdgeStyleNoArrow;
                ++ecount;
            }
            if (sp < 32u) stk[sp++] = { ind, ni };
        }
        if (mcount == 0u) return syntax_failure(g.pos);
        const auto place_mindmap = [&](float s) noexcept -> EtchVec2 {
            const float eff_font = std::fmax(8.0f, input->font_size_px * s);
            const float pad = 16.0f * s;
            const float h_gap = std::fmax(28.0f * s, input->rank_gap_px * 0.72f * s);
            const float v_gap = std::fmax(10.0f * s, input->node_gap_px * 0.5f * s);
            float col_w[32] = {};
            uint32_t max_dep = 0u;
            for (uint32_t i = 0u; i < mcount; ++i) {
                const uint32_t d = items[i].depth < 31u ? items[i].depth : 31u;
                if (d > max_dep) max_dep = d;
                const uint16_t f = (d == 0u) ? bold_font : body_font;
                const float fsz = (d == 0u) ? (eff_font * 1.05f) : (eff_font * 0.92f);
                const EtchVec2 tsz = env.measure_text(items[i].label, f, fsz);
                const float nw = tsz.x + (d == 0u ? 30.0f : 22.0f) * s;
                const float nh = std::fmax(tsz.y + (d == 0u ? 16.0f : 12.0f) * s, 26.0f * s);
                mnodes[i].bounds = { 0.0f, 0.0f, nw, nh };
                if (nw > col_w[d]) col_w[d] = nw;
            }
            float col_x[32] = {};
            col_x[0] = input->pos.x + pad;
            for (uint32_t d = 1u; d <= max_dep; ++d) col_x[d] = col_x[d - 1u] + col_w[d - 1u] + h_gap;
            // Bottom-up subtree height
            for (uint32_t i = mcount; i-- > 0u;) {
                float ch_sum = 0.0f;
                uint32_t ch_cnt = 0u;
                for (uint32_t j = i + 1u; j < mcount; ++j) {
                    if (items[j].parent == static_cast<int32_t>(i)) {
                        ch_sum += items[j].sub_h;
                        ++ch_cnt;
                    }
                }
                if (ch_cnt > 1u) ch_sum += float(ch_cnt - 1u) * v_gap;
                items[i].sub_h = std::fmax(mnodes[i].bounds.h, ch_sum);
            }
            // Top-down vertical placement
            float cur_root_y = input->pos.y + pad;
            auto place_sub = [&](auto&& self, uint32_t idx, float top_y) noexcept -> void {
                const uint32_t d = items[idx].depth < 31u ? items[idx].depth : 31u;
                mnodes[idx].bounds.x = col_x[d];
                mnodes[idx].bounds.y = top_y + (items[idx].sub_h - mnodes[idx].bounds.h) * 0.5f;
                float cy = top_y;
                for (uint32_t j = idx + 1u; j < mcount; ++j) {
                    if (items[j].parent == static_cast<int32_t>(idx)) {
                        self(self, j, cy);
                        cy += items[j].sub_h + v_gap;
                    }
                }
            };
            for (uint32_t i = 0u; i < mcount; ++i) {
                if (items[i].parent < 0) {
                    place_sub(place_sub, i, cur_root_y);
                    cur_root_y += items[i].sub_h + v_gap * 1.5f;
                }
            }
            const float total_w = (col_x[max_dep] + col_w[max_dep] - input->pos.x) + pad;
            const float total_h = (cur_root_y - v_gap * 1.5f - input->pos.y) + pad;
            return { total_w, std::fmax(48.0f * s, total_h) };
        };
        const EtchVec2 nat = place_mindmap(1.0f);
        float final_scale = 1.0f;
        EtchVec2 final_sz = nat;
        if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
            final_scale = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
            final_sz = place_mindmap(final_scale);
        }
        if (out_meta) {
            out_meta->shapes = mshapes;
            out_meta->edge_styles = mstyles;
            out_meta->diagram_kind = InlayDiagramMindmap;
        }
        return { mnodes, medges, mcount, ecount, final_sz.x, final_sz.y, true, true, false, final_scale, nat.x, nat.y, warnings };
    }

    // ---- Mermaid timeline ----
    if (timeline_diag) {
        while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
        constexpr uint32_t kMaxPeriods = 32u;
        constexpr uint32_t kMaxTlNodes = 160u;
        constexpr uint32_t kMaxSections = 16u;
        EtchStr title = {};
        struct SecRec { EtchStr name; uint32_t first_col; uint32_t last_col; };
        SecRec secs[kMaxSections] = {};
        uint32_t sec_count = 0u;
        auto* tnodes = static_cast<InlayDiagramNode*>(env.alloc(kMaxTlNodes * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
        auto* tshapes = static_cast<InlayDiagramShape*>(env.alloc(kMaxTlNodes * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
        auto* tsgs = static_cast<InlayDiagramSubgraph*>(env.alloc(kMaxSections * sizeof(InlayDiagramSubgraph), alignof(InlayDiagramSubgraph)));
        if (!tnodes || !tshapes || !tsgs) return {};
        uint32_t tcount = 0u;
        uint32_t col_count = 0u;
        uint32_t col_events[kMaxPeriods] = {};
        while (g.pos < source.length && tcount < kMaxTlNodes) {
            spaces(&g);
            if (g.pos >= source.length) break;
            if (source.chars[g.pos] == '\n') { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            const uint32_t ls = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            const EtchStr line = trim_slice(source, ls, g.pos);
            if (line.length == 0u) continue;
            if (line.length > 6u && std::memcmp(line.chars, "title", 5u) == 0 && (line.chars[5] == ' ' || line.chars[5] == '\t')) {
                title = trim_slice(line, 6u, line.length);
                continue;
            }
            if (line.length > 8u && std::memcmp(line.chars, "section", 7u) == 0 && (line.chars[7] == ' ' || line.chars[7] == '\t')) {
                if (sec_count < kMaxSections) {
                    secs[sec_count++] = { trim_slice(line, 8u, line.length), col_count, col_count };
                }
                continue;
            }
            // Split on `:` -> first part is period (or empty for continuation `: event`), rest are events
            uint32_t colon = UINT32_MAX;
            for (uint32_t ci = 0u; ci < line.length; ++ci) {
                if (line.chars[ci] == ':') { colon = ci; break; }
            }
            const EtchStr pstr = (colon == UINT32_MAX) ? line : trim_slice(line, 0u, colon);
            if (pstr.length > 0u) {
                if (col_count >= kMaxPeriods) continue;
                const uint32_t cidx = col_count++;
                tnodes[tcount] = { pstr, pstr, {}, cidx };
                tshapes[tcount] = InlayShapeRound;
                ++tcount;
                if (sec_count > 0u) secs[sec_count - 1u].last_col = col_count;
            }
            if (colon != UINT32_MAX && col_count > 0u) {
                const uint32_t cidx = col_count - 1u;
                uint32_t ep = colon + 1u;
                while (ep <= line.length && tcount < kMaxTlNodes) {
                    uint32_t nxt = ep;
                    while (nxt < line.length && line.chars[nxt] != ':') ++nxt;
                    const EtchStr ev = trim_slice(line, ep, nxt);
                    if (ev.length > 0u) {
                        tnodes[tcount] = { ev, ev, {}, cidx };
                        tshapes[tcount] = InlayShapeTimelineCard;
                        ++tcount;
                        ++col_events[cidx];
                    }
                    if (nxt >= line.length) break;
                    ep = nxt + 1u;
                }
            }
        }
        if (col_count == 0u) return syntax_failure(g.pos);
        const auto place_timeline = [&](float s) noexcept -> EtchVec2 {
            const float eff_font = std::fmax(8.0f, input->font_size_px * s);
            const float pad = 16.0f * s;
            const float title_h = title.length ? (eff_font * 1.8f + 6.0f * s) : 0.0f;
            const float sec_h = (sec_count > 0u) ? (eff_font * 1.6f + 8.0f * s) : 0.0f;
            const float hdr_h = std::fmax(eff_font * 1.7f + 8.0f * s, 28.0f * s);
            const float card_h = std::fmax(eff_font * 1.55f + 10.0f * s, 28.0f * s);
            const float col_gap = 18.0f * s;
            const float card_gap = 8.0f * s;
            float col_w[kMaxPeriods] = {};
            for (uint32_t c = 0u; c < col_count; ++c) col_w[c] = 96.0f * s;
            for (uint32_t i = 0u; i < tcount; ++i) {
                const uint32_t c = tnodes[i].rank;
                const uint16_t f = (tshapes[i] == InlayShapeRound) ? bold_font : body_font;
                const float tw = env.measure_text(tnodes[i].label, f, eff_font * 0.9f).x + 24.0f * s;
                if (tw > col_w[c]) col_w[c] = std::fmin(220.0f * s, tw);
            }
            float col_x[kMaxPeriods] = {};
            float cx = input->pos.x + pad;
            for (uint32_t c = 0u; c < col_count; ++c) { col_x[c] = cx; cx += col_w[c] + col_gap; }
            const float hdr_y = input->pos.y + pad + title_h + sec_h;
            uint32_t row_idx[kMaxPeriods] = {};
            float max_y = hdr_y + hdr_h;
            for (uint32_t i = 0u; i < tcount; ++i) {
                const uint32_t c = tnodes[i].rank;
                if (tshapes[i] == InlayShapeRound) {
                    tnodes[i].bounds = { col_x[c], hdr_y, col_w[c], hdr_h };
                } else {
                    const uint32_t r = row_idx[c]++;
                    const float ey = hdr_y + hdr_h + 16.0f * s + float(r) * (card_h + card_gap);
                    tnodes[i].bounds = { col_x[c], ey, col_w[c], card_h };
                    if (ey + card_h > max_y) max_y = ey + card_h;
                }
            }
            for (uint32_t si = 0u; si < sec_count; ++si) {
                const bool valid_s = secs[si].last_col > secs[si].first_col && secs[si].first_col < col_count;
                const uint32_t c0 = valid_s ? secs[si].first_col : 0u;
                const uint32_t c1 = valid_s ? (secs[si].last_col - 1u) : 0u;
                const float sx = col_x[c0];
                const float sw = (col_x[c1] + col_w[c1]) - sx;
                tsgs[si] = { secs[si].name, { sx, input->pos.y + pad + title_h, sw, sec_h - 6.0f * s }, -1, valid_s };
            }
            return { (cx - col_gap - input->pos.x) + pad, (max_y - input->pos.y) + pad };
        };
        const EtchVec2 nat = place_timeline(1.0f);
        float final_scale = 1.0f;
        EtchVec2 final_sz = nat;
        if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
            final_scale = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
            final_sz = place_timeline(final_scale);
        }
        if (out_meta) {
            out_meta->shapes = tshapes;
            out_meta->subgraphs = sec_count ? tsgs : nullptr;
            out_meta->subgraph_count = sec_count;
            out_meta->diagram_kind = InlayDiagramTimeline;
            out_meta->title = title;
        }
        return { tnodes, nullptr, tcount, 0u, final_sz.x, final_sz.y, true, true, false, final_scale, nat.x, nat.y, warnings };
    }

    const uint32_t req_nodes = input->max_nodes > 0u ? (input->max_nodes > 2048u ? 2048u : input->max_nodes) : 256u;
    const uint32_t req_edges = input->max_edges > 0u ? (input->max_edges > 8192u ? 8192u : input->max_edges) : 1024u;
    constexpr uint32_t kSubgraphCap = 32;
    // Each parsed entry consumes at least one byte. Small diagrams need only
    // reserve enough space for their remaining input.
    const uint32_t body = source.length - g.pos;
    if (!body) return syntax_failure(g.pos);
    g.node_capacity = body < req_nodes ? body : req_nodes;
    const uint32_t edge_cap = body < req_edges ? body : req_edges;
    const uint32_t subgraph_cap = body < kSubgraphCap ? body : kSubgraphCap;
    auto* nodes = static_cast<InlayDiagramNode*>(env.alloc(size_t(g.node_capacity) * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
    auto* edges = static_cast<InlayDiagramEdge*>(env.alloc(size_t(edge_cap) * sizeof(InlayDiagramEdge), alignof(InlayDiagramEdge)));
    auto* edge_styles = static_cast<uint8_t*>(env.alloc(size_t(edge_cap), 1));
    auto* shapes = static_cast<InlayDiagramShape*>(env.alloc(size_t(g.node_capacity) * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
    auto* node_sub = static_cast<int32_t*>(env.alloc(size_t(g.node_capacity) * sizeof(int32_t), alignof(int32_t)));
    auto* subgraphs = static_cast<InlayDiagramSubgraph*>(env.alloc(size_t(subgraph_cap) * sizeof(InlayDiagramSubgraph), alignof(InlayDiagramSubgraph)));
    if (!nodes || !edges || !edge_styles || !shapes || !node_sub || !subgraphs) return {};
    g.nodes = nodes;
    g.shapes = shapes;
    g.node_subgraph = node_sub;
    g.edges = edges;
    g.edge_styles = edge_styles;
    g.subgraphs = subgraphs;

    if (sequence) {
        while (g.valid && g.pos < source.length) {
            spaces(&g);
            if (g.pos == source.length) break;
            if (source.chars[g.pos] == '\n' || source.chars[g.pos] == ';') { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            if (g.pos + 12 <= source.length && !std::memcmp(source.chars + g.pos, "participant ", 12)) {
                g.pos += 12;
                const uint32_t who = node(&g);
                if (!g.valid) break;
                spaces(&g);
                if (g.pos + 3 <= source.length && !std::memcmp(source.chars + g.pos, "as ", 3)) {
                    g.pos += 3;
                    const uint32_t name = g.pos;
                    while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
                    nodes[who].label = strip_outer_quotes(trim_slice(source, name, g.pos));
                }
                continue;
            }
            const uint32_t from = node(&g);
            spaces(&g);
            const bool dashed = starts(g, ETCH_STR_LIT("-->>"));
            if (!dashed && !starts(g, ETCH_STR_LIT("->>"))) { g.valid = false; break; }
            g.pos += dashed ? 4 : 3;
            const uint32_t to = node(&g);
            spaces(&g);
            if (!g.valid || g.pos == source.length || source.chars[g.pos++] != ':' || g.edge_count == edge_cap) { g.valid = false; break; }
            spaces(&g);
            const uint32_t label = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            edge_styles[g.edge_count] = InlayEdgeStyleNone;
            edges[g.edge_count++] = { from, to, false, dashed, trim_slice(source, label, g.pos) };
        }
        if (!g.valid || !g.node_count) return syntax_failure(g.pos);

        const auto place_sequence = [&](float s) noexcept -> EtchVec2 {
            const float eff_font = std::fmax(7.0f, input->font_size_px * s);
            const float caption_font = std::fmax(7.0f, std::fmax(10.0f * s, eff_font * 0.82f));
            const float eff_node_gap = input->node_gap_px * s;
            const float eff_rank_gap = input->rank_gap_px * s;
            float width = 0.0f, max_message = 0.0f, max_box_h = eff_font * 1.5f + 24.0f * s;
            for (uint32_t i = 0; i < g.edge_count; ++i) {
                const auto msz = measure_label_env(edges[i].label, body_font, caption_font, env);
                max_message = std::fmax(max_message, msz.x + 24.0f * s);
            }
            for (uint32_t i = 0; i < g.node_count; ++i) {
                const auto sz = measure_label_env(nodes[i].label, body_font, eff_font, env);
                const float bh = std::fmax(eff_font * 1.5f, sz.y) + 24.0f * s;
                max_box_h = std::fmax(max_box_h, bh);
                nodes[i].bounds = { input->pos.x + width, input->pos.y, std::fmax(sz.x + 32.0f * s, max_message), bh };
                width += nodes[i].bounds.w + eff_node_gap;
            }
            for (uint32_t i = 0; i < g.node_count; ++i) nodes[i].bounds.h = max_box_h;
            float total_w = width - eff_node_gap;
            for (uint32_t i = 0; i < g.edge_count; ++i) if (edges[i].from == edges[i].to) { total_w += 40.0f * s; break; }
            const float height = max_box_h + float(g.edge_count + 1) * eff_rank_gap;
            return { total_w, height };
        };

        const EtchVec2 nat = place_sequence(1.0f);
        float final_scale = 1.0f;
        EtchVec2 final_sz = nat;
        if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
            float s = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
            final_sz = place_sequence(s);
            for (int iter = 0; iter < 3 && final_sz.x > input->max_width_px && s > min_scale; ++iter) {
                s = std::fmax(min_scale, s * (input->max_width_px / final_sz.x) * 0.992f);
                final_sz = place_sequence(s);
            }
            final_scale = s;
        }
        if (out_meta) {
            out_meta->shapes = shapes;
            out_meta->edge_styles = edge_styles;
        }
        return { nodes, edges, g.node_count, g.edge_count, final_sz.x, final_sz.y, true, false, true, final_scale, nat.x, nat.y, warnings };
    }

    int32_t sub_stack[kSubgraphCap] = {};
    uint32_t sub_depth = 0;
    if (state_diag) {
        const auto add_state_node = [&](EtchStr nid, EtchStr lbl, InlayDiagramShape sh) noexcept -> uint32_t {
            uint32_t index = 0u;
            while (index < g.node_count && !equal(g.nodes[index].id, nid)) ++index;
            if (index == g.node_count) {
                if (index == g.node_capacity) { g.valid = false; return 0u; }
                g.nodes[index] = {};
                g.nodes[index].id = nid;
                g.nodes[index].label = lbl;
                g.shapes[index] = sh;
                g.node_subgraph[index] = g.active_subgraph;
                ++g.node_count;
            } else {
                if (g.node_subgraph[index] < 0 && g.active_subgraph >= 0) g.node_subgraph[index] = g.active_subgraph;
                if (lbl.length > 0u && equal(g.nodes[index].label, g.nodes[index].id)) g.nodes[index].label = lbl;
            }
            return index;
        };
        const auto read_state_id = [&](bool is_source) noexcept -> uint32_t {
            spaces(&g);
            if (starts(g, ETCH_STR_LIT("[*]"))) {
                g.pos += 3u;
                return is_source
                    ? add_state_node(ETCH_STR_LIT("__start"), ETCH_STR_LIT(""), InlayShapeStateStart)
                    : add_state_node(ETCH_STR_LIT("__end"), ETCH_STR_LIT(""), InlayShapeStateEnd);
            }
            const uint32_t start = g.pos;
            while (g.pos < source.length) {
                const char c = source.chars[g.pos];
                if (word(c)) { ++g.pos; continue; }
                if (c == '-' && g.pos + 1u < source.length && word(source.chars[g.pos + 1u])) { ++g.pos; continue; }
                break;
            }
            if (start == g.pos) { g.valid = false; return 0u; }
            const EtchStr sid = { source.chars + start, g.pos - start, false };
            return add_state_node(sid, sid, InlayShapeRound);
        };

        while (g.valid && g.pos < source.length) {
            spaces(&g);
            if (g.pos == source.length) break;
            if (statement_end(g)) { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            const uint32_t stmt = g.pos;
            if (source.chars[g.pos] == '}') {
                ++g.pos;
                if (!sub_depth) return syntax_failure(stmt);
                g.active_subgraph = sub_stack[--sub_depth];
                continue;
            }
            if (starts(g, ETCH_STR_LIT("direction")) && g.pos + 9u < source.length &&
                (source.chars[g.pos + 9u] == ' ' || source.chars[g.pos + 9u] == '\t'))
            {
                g.pos += 9u;
                spaces(&g);
                const uint32_t dpos = g.pos;
                while (g.pos < source.length && word(source.chars[g.pos])) ++g.pos;
                const EtchStr dir = { source.chars + dpos, g.pos - dpos, false };
                const bool is_lr = equal(dir, ETCH_STR_LIT("LR"));
                const bool is_rl = equal(dir, ETCH_STR_LIT("RL"));
                const bool is_td = equal(dir, ETCH_STR_LIT("TD")) || equal(dir, ETCH_STR_LIT("TB"));
                const bool is_bt = equal(dir, ETCH_STR_LIT("BT"));
                if (!is_lr && !is_rl && !is_td && !is_bt) {
                    diagnostic(INLAY_DIAGRAM_INVALID_DIRECTION, dpos, dir.length);
                    return {};
                }
                g.lr = is_lr || is_rl;
                g.reverse_dir = is_rl || is_bt;
                continue;
            }
            if ((starts(g, ETCH_STR_LIT("note ")) || starts(g, ETCH_STR_LIT("classDef ")) || starts(g, ETCH_STR_LIT("class ")))) {
                while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
                ++warnings;
                diagnostic(INLAY_DIAGRAM_UNSUPPORTED_DIRECTIVE, stmt, g.pos - stmt);
                continue;
            }
            if (starts(g, ETCH_STR_LIT("state")) && g.pos + 5u < source.length &&
                (source.chars[g.pos + 5u] == ' ' || source.chars[g.pos + 5u] == '\t'))
            {
                g.pos += 5u;
                spaces(&g);
                EtchStr alias_label = {};
                if (g.pos < source.length && source.chars[g.pos] == '"') {
                    ++g.pos;
                    const uint32_t ls = g.pos;
                    while (g.pos < source.length && source.chars[g.pos] != '"' && source.chars[g.pos] != '\n') ++g.pos;
                    if (g.pos == source.length || source.chars[g.pos] != '"') return syntax_failure(stmt);
                    alias_label = { source.chars + ls, g.pos - ls, false };
                    ++g.pos;
                    spaces(&g);
                    if (starts(g, ETCH_STR_LIT("as")) && g.pos + 2u < source.length &&
                        (source.chars[g.pos + 2u] == ' ' || source.chars[g.pos + 2u] == '\t'))
                    {
                        g.pos += 2u;
                        spaces(&g);
                    }
                }
                const uint32_t id_s = g.pos;
                while (g.pos < source.length && (word(source.chars[g.pos]) || source.chars[g.pos] == '-')) ++g.pos;
                if (g.pos == id_s) return syntax_failure(stmt);
                const EtchStr sid = { source.chars + id_s, g.pos - id_s, false };
                spaces(&g);
                if (g.pos < source.length && source.chars[g.pos] == '{') {
                    ++g.pos;
                    if (g.subgraph_count == subgraph_cap) return syntax_failure(stmt);
                    const int32_t idx = int32_t(g.subgraph_count++);
                    subgraphs[idx] = { alias_label.length ? alias_label : sid, {}, g.active_subgraph, false };
                    sub_stack[sub_depth++] = g.active_subgraph;
                    g.active_subgraph = idx;
                    continue;
                }
                const uint32_t ni = add_state_node(sid, alias_label.length ? alias_label : sid, InlayShapeRound);
                if (!g.valid) break;
                if (alias_label.length) g.nodes[ni].label = alias_label;
                while (!statement_end(g)) ++g.pos;
                continue;
            }
            const uint32_t from = read_state_id(true);
            if (!g.valid) break;
            spaces(&g);
            if (starts(g, ETCH_STR_LIT("-->"))) {
                g.pos += 3u;
                const uint32_t to = read_state_id(false);
                if (!g.valid || g.edge_count == edge_cap) { g.valid = false; break; }
                spaces(&g);
                EtchStr elbl = {};
                if (g.pos < source.length && source.chars[g.pos] == ':') {
                    ++g.pos;
                    spaces(&g);
                    const uint32_t ls = g.pos;
                    while (!statement_end(g)) ++g.pos;
                    elbl = trim_slice(source, ls, g.pos);
                }
                edge_styles[g.edge_count] = InlayEdgeStyleNone;
                edges[g.edge_count++] = { from, to, false, false, elbl };
            } else if (g.pos < source.length && source.chars[g.pos] == ':') {
                ++g.pos;
                spaces(&g);
                const uint32_t ls = g.pos;
                while (!statement_end(g)) ++g.pos;
                const EtchStr desc_lbl = trim_slice(source, ls, g.pos);
                if (desc_lbl.length) g.nodes[from].label = desc_lbl;
            } else if (!statement_end(g)) {
                g.valid = false;
                break;
            }
        }
    } else if (class_diag || er_diag) {
        const auto add_box_node = [&](EtchStr nid) noexcept -> uint32_t {
            uint32_t idx = 0u;
            while (idx < g.node_count && !equal(g.nodes[idx].id, nid)) ++idx;
            if (idx == g.node_count) {
                if (idx == g.node_capacity) { g.valid = false; return 0u; }
                g.nodes[idx] = {};
                g.nodes[idx].id = nid;
                g.nodes[idx].label = nid;
                g.shapes[idx] = InlayShapeClassBox;
                g.node_subgraph[idx] = g.active_subgraph;
                ++g.node_count;
            } else if (g.node_subgraph[idx] < 0 && g.active_subgraph >= 0) {
                g.node_subgraph[idx] = g.active_subgraph;
            }
            return idx;
        };
        const auto append_member = [&](uint32_t idx, EtchStr member) noexcept {
            if (!member.chars || member.length == 0u || idx >= g.node_count) return;
            const EtchStr cur = g.nodes[idx].label;
            const uint32_t total = cur.length + 1u + member.length;
            if (char* buf = static_cast<char*>(env.alloc(total + 1u, 1u))) {
                if (cur.length > 0u && cur.chars) std::memcpy(buf, cur.chars, cur.length);
                buf[cur.length] = '\n';
                std::memcpy(buf + cur.length + 1u, member.chars, member.length);
                buf[total] = '\0';
                g.nodes[idx].label = { buf, total, false };
            }
        };
        const auto read_ident = [&](uint32_t* p, const char* s, uint32_t len) noexcept -> EtchStr {
            while (*p < len && (s[*p] == ' ' || s[*p] == '\t')) ++(*p);
            const uint32_t st = *p;
            while (*p < len) {
                const char ch = s[*p];
                if (word(ch) || ch == '-' || ch == '~' || ch == '$' || ch == '<' || ch == '>') {
                    if (ch == '-' && *p + 1u < len && (s[*p + 1u] == '-' || s[*p + 1u] == '>' || s[*p + 1u] == '|' || s[*p + 1u] == '*' || s[*p + 1u] == 'o')) break;
                    if (ch == '<' && *p + 1u < len && (s[*p + 1u] == '|' || s[*p + 1u] == '-' || s[*p + 1u] == '.')) break;
                    if (ch == '>' && *p > st && (s[*p - 1u] == '-' || s[*p - 1u] == '.')) break;
                    ++(*p);
                } else break;
            }
            return { s + st, *p - st, false };
        };
        int32_t open_box_node = -1;
        while (g.valid && g.pos < source.length) {
            spaces(&g);
            if (g.pos >= source.length) break;
            if (source.chars[g.pos] == '\n' || source.chars[g.pos] == ';') { ++g.pos; continue; }
            if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
            const uint32_t ls = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            const EtchStr line = trim_slice(source, ls, g.pos);
            if (line.length == 0u) continue;
            if (open_box_node >= 0) {
                if (line.chars[0] == '}') {
                    open_box_node = -1;
                    continue;
                }
                // Strip trailing '}' if inline close
                EtchStr mem = line;
                if (mem.chars[mem.length - 1u] == '}') {
                    mem = trim_slice(mem, 0u, mem.length - 1u);
                    if (mem.length > 0u) append_member(uint32_t(open_box_node), mem);
                    open_box_node = -1;
                } else {
                    append_member(uint32_t(open_box_node), mem);
                }
                continue;
            }
            if (line.chars[0] == '}' && sub_depth > 0u) {
                g.active_subgraph = sub_stack[--sub_depth];
                continue;
            }
            if (class_diag && line.length > 10u && std::memcmp(line.chars, "direction ", 10u) == 0) {
                const EtchStr dir = trim_slice(line, 10u, line.length);
                const bool is_lr = equal(dir, ETCH_STR_LIT("LR"));
                const bool is_rl = equal(dir, ETCH_STR_LIT("RL"));
                const bool is_bt = equal(dir, ETCH_STR_LIT("BT"));
                g.lr = is_lr || is_rl;
                g.reverse_dir = is_rl || is_bt;
                continue;
            }
            if (class_diag && line.length > 10u && std::memcmp(line.chars, "namespace ", 10u) == 0) {
                uint32_t p = 10u;
                const EtchStr ns = read_ident(&p, line.chars, line.length);
                if (ns.length > 0u && g.subgraph_count < subgraph_cap) {
                    const int32_t idx = int32_t(g.subgraph_count++);
                    subgraphs[idx] = { ns, {}, g.active_subgraph, false };
                    sub_stack[sub_depth++] = g.active_subgraph;
                    g.active_subgraph = idx;
                }
                continue;
            }
            if (class_diag && line.length > 6u && std::memcmp(line.chars, "class ", 6u) == 0) {
                uint32_t p = 6u;
                const EtchStr cname = read_ident(&p, line.chars, line.length);
                if (cname.length > 0u) {
                    const uint32_t ci = add_box_node(cname);
                    while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
                    if (p < line.length && line.chars[p] == '{') {
                        const EtchStr rest = trim_slice(line, p + 1u, line.length);
                        if (rest.length > 0u && rest.chars[rest.length - 1u] == '}') {
                            const EtchStr inner = trim_slice(rest, 0u, rest.length - 1u);
                            if (inner.length > 0u) append_member(ci, inner);
                        } else {
                            if (rest.length > 0u) append_member(ci, rest);
                            open_box_node = int32_t(ci);
                        }
                    }
                }
                continue;
            }
            // Check ER entity attribute block: ENTITY { ...
            if (er_diag) {
                uint32_t p = 0u;
                const EtchStr ename = read_ident(&p, line.chars, line.length);
                while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
                if (ename.length > 0u && p < line.length && line.chars[p] == '{') {
                    const uint32_t ei = add_box_node(ename);
                    const EtchStr rest = trim_slice(line, p + 1u, line.length);
                    if (rest.length > 0u && rest.chars[rest.length - 1u] == '}') {
                        const EtchStr inner = trim_slice(rest, 0u, rest.length - 1u);
                        if (inner.length > 0u) append_member(ei, inner);
                    } else {
                        if (rest.length > 0u) append_member(ei, rest);
                        open_box_node = int32_t(ei);
                    }
                    continue;
                }
            }
            // Check class member line: ClassName : +member
            if (class_diag) {
                uint32_t p = 0u;
                const EtchStr cname = read_ident(&p, line.chars, line.length);
                while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
                if (cname.length > 0u && p < line.length && line.chars[p] == ':') {
                    const uint32_t ci = add_box_node(cname);
                    append_member(ci, trim_slice(line, p + 1u, line.length));
                    continue;
                }
            }
            // Relationship edge line
            uint32_t p = 0u;
            const EtchStr lhs = read_ident(&p, line.chars, line.length);
            if (lhs.length == 0u) continue;
            while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
            if (p < line.length && line.chars[p] == '"') {
                ++p;
                while (p < line.length && line.chars[p] != '"') ++p;
                if (p < line.length) ++p;
                while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
            }
            const uint32_t rel_s = p;
            while (p < line.length && (line.chars[p] == '<' || line.chars[p] == '>' || line.chars[p] == '|' ||
                   line.chars[p] == '-' || line.chars[p] == '.' || line.chars[p] == '*' || line.chars[p] == 'o' ||
                   line.chars[p] == '{' || line.chars[p] == '}')) {
                ++p;
            }
            const EtchStr rel = { line.chars + rel_s, p - rel_s, false };
            if (rel.length < 2u) {
                (void)add_box_node(lhs);
                continue;
            }
            while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
            if (p < line.length && line.chars[p] == '"') {
                ++p;
                while (p < line.length && line.chars[p] != '"') ++p;
                if (p < line.length) ++p;
                while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
            }
            const EtchStr rhs = read_ident(&p, line.chars, line.length);
            if (rhs.length == 0u) continue;
            EtchStr elbl = {};
            while (p < line.length && (line.chars[p] == ' ' || line.chars[p] == '\t')) ++p;
            if (p < line.length && line.chars[p] == ':') {
                elbl = strip_outer_quotes(trim_slice(line, p + 1u, line.length));
            }
            uint32_t from = add_box_node(lhs);
            uint32_t to = add_box_node(rhs);
            bool dashed = false;
            for (uint32_t ri = 0u; ri + 1u < rel.length; ++ri) {
                if (rel.chars[ri] == '.' && rel.chars[ri + 1u] == '.') { dashed = true; break; }
            }
            uint8_t estyle = InlayEdgeStyleNone;
            if (er_diag) {
                estyle = InlayEdgeStyleNoArrow;
            } else {
                const bool left_arrow = (rel.chars[0] == '<');
                const bool right_arrow = (rel.chars[rel.length - 1u] == '>');
                if (left_arrow && right_arrow) {
                    estyle = InlayEdgeStyleBidir;
                } else if (left_arrow && !right_arrow) {
                    const uint32_t tmp = from; from = to; to = tmp;
                } else if (!left_arrow && !right_arrow) {
                    estyle = InlayEdgeStyleNoArrow;
                }
            }
            if (g.valid && g.edge_count < edge_cap) {
                edge_styles[g.edge_count] = estyle;
                edges[g.edge_count++] = { from, to, false, dashed, elbl };
            }
        }
    } else while (g.valid && g.pos < source.length) {
        spaces(&g);
        if (g.pos == source.length) break;
        if (statement_end(g)) { ++g.pos; continue; }
        if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
        const uint32_t statement = g.pos;
        const EtchStr directive = skip_flowchart_directive(&g);
        if (directive.length) {
            ++warnings;
            diagnostic(INLAY_DIAGRAM_UNSUPPORTED_DIRECTIVE, statement, directive.length);
            continue;
        }
        if (starts(g, ETCH_STR_LIT("subgraph")) && g.pos + 8 < source.length &&
            (source.chars[g.pos + 8] == ' ' || source.chars[g.pos + 8] == '\t'))
        {
            g.pos += 8;
            spaces(&g);
            if (statement_end(g)) return syntax_failure(g.pos);
            const uint32_t title_start = g.pos;
            while (!statement_end(g)) ++g.pos;
            EtchStr title = trim_slice(source, title_start, g.pos);
            // Support optional Mermaid `subgraph id [Title]` or `subgraph id ["Title"]` syntax.
            for (uint32_t k = 0; k < title.length; ++k) {
                if (title.chars[k] == '[' && title.chars[title.length - 1] == ']') {
                    title = trim_slice(title, k + 1, title.length - 1);
                    break;
                }
            }
            title = strip_outer_quotes(title);
            if (g.subgraph_count == subgraph_cap) return syntax_failure(statement);
            const int32_t idx = int32_t(g.subgraph_count++);
            subgraphs[idx] = { title, {}, g.active_subgraph, false };
            sub_stack[sub_depth++] = g.active_subgraph;
            g.active_subgraph = idx;
            continue;
        }
        if (starts(g, ETCH_STR_LIT("end")) && (g.pos + 3 == source.length || !word(source.chars[g.pos + 3]))) {
            const uint32_t start = g.pos;
            g.pos += 3;
            spaces(&g);
            if (statement_end(g)) {
                if (!sub_depth) return syntax_failure(start);
                g.active_subgraph = sub_stack[--sub_depth];
                continue;
            }
            g.pos = start;
        }
        uint32_t from = node(&g);
        while (g.valid) {
            spaces(&g);
            if (statement_end(g)) break;
            InlayDiagramEdge edge = {};
            edge.from = from;
            uint8_t estyle = InlayEdgeStyleNone;
            if (g.edge_count == edge_cap || !flow_arrow(&g, &edge, &estyle)) { g.valid = false; break; }
            edge.to = node(&g);
            if (!g.valid) break;
            edge_styles[g.edge_count] = estyle;
            edges[g.edge_count++] = edge;
            from = edge.to;
        }
    }
    if (!g.valid || !g.node_count || sub_depth) return syntax_failure(g.pos);

    // 1. Iterative DFS reverses feedback edges so remaining orientation is acyclic.
    auto* colors = static_cast<uint8_t*>(env.alloc(g.node_count, 1));
    auto* stack = static_cast<uint32_t*>(env.alloc(size_t(g.node_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* cursor = static_cast<uint32_t*>(env.alloc(size_t(g.node_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* topo = static_cast<uint32_t*>(env.alloc(size_t(g.node_count) * sizeof(uint32_t), alignof(uint32_t)));
    if (!colors || !stack || !cursor || !topo) return {};
    std::memset(colors, 0, g.node_count);
    std::memset(cursor, 0, size_t(g.node_count) * sizeof(uint32_t));
    uint32_t finished = 0;
    for (uint32_t root = 0; root < g.node_count; ++root) if (!colors[root]) {
        uint32_t depth = 1;
        stack[0] = root;
        colors[root] = 1;
        while (depth) {
            const uint32_t at = stack[depth - 1];
            uint32_t& edge = cursor[at];
            while (edge < g.edge_count && (edges[edge].from != at || edges[edge].from == edges[edge].to)) ++edge;
            if (edge == g.edge_count) { colors[at] = 2; topo[finished++] = at; --depth; continue; }
            auto& e = edges[edge++];
            if (colors[e.to] == 1) { e.reversed = true; continue; }
            if (!colors[e.to]) { colors[e.to] = 1; stack[depth++] = e.to; }
        }
    }

    // 2. Longest-path rank assignment.
    uint32_t max_rank = 0;
    for (uint32_t reverse = finished; reverse > 0; --reverse) {
        const uint32_t at = topo[reverse - 1];
        for (uint32_t edge = 0; edge < g.edge_count; ++edge) {
            const auto& e = edges[edge];
            if (e.from == e.to) continue;
            const uint32_t from = e.reversed ? e.to : e.from, to = e.reversed ? e.from : e.to;
            if (from == at && nodes[to].rank <= nodes[from].rank) nodes[to].rank = nodes[from].rank + 1;
        }
    }
    for (uint32_t i = 0; i < g.node_count; ++i) if (nodes[i].rank > max_rank) max_rank = nodes[i].rank;

    // 3. Build Sugiyama virtual-dummy graph for multi-rank non-reversed long edges.
    struct VEdge { uint32_t u, v; };
    uint32_t dummy_count = 0;
    const uint32_t dummy_cap = edge_cap > 1024u ? 2048u : 512u;
    for (uint32_t i = 0; i < g.edge_count; ++i) {
        const auto& e = edges[i];
        if (e.from == e.to || e.reversed) continue;
        if (nodes[e.to].rank > nodes[e.from].rank + 1) {
            const uint32_t span = nodes[e.to].rank - nodes[e.from].rank - 1;
            if (dummy_count + span <= dummy_cap) dummy_count += span;
        }
    }
    const uint32_t vnode_count = g.node_count + dummy_count;
    const uint32_t vedge_cap = g.edge_count + dummy_count;
    auto* vrank = static_cast<uint32_t*>(env.alloc(size_t(vnode_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* order = static_cast<uint32_t*>(env.alloc(size_t(vnode_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* pos_in_rank = static_cast<uint32_t*>(env.alloc(size_t(vnode_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* bary = static_cast<float*>(env.alloc(size_t(vnode_count) * sizeof(float), alignof(float)));
    auto* vedges = static_cast<VEdge*>(env.alloc(size_t(vedge_cap ? vedge_cap : 1u) * sizeof(VEdge), alignof(VEdge)));
    auto* edge_paths = static_cast<InlayDiagramEdgePath*>(env.alloc(size_t(g.edge_count ? g.edge_count : 1u) * sizeof(InlayDiagramEdgePath), alignof(InlayDiagramEdgePath)));
    auto* waypoints = static_cast<EtchVec2*>(env.alloc(size_t(dummy_count ? dummy_count : 1u) * sizeof(EtchVec2), alignof(EtchVec2)));
    auto* dummy_to_wp = static_cast<uint32_t*>(env.alloc(size_t(dummy_count ? dummy_count : 1u) * sizeof(uint32_t), alignof(uint32_t)));
    if (!vrank || !order || !pos_in_rank || !bary || !vedges || !edge_paths || !waypoints || !dummy_to_wp) return {};

    for (uint32_t i = 0; i < g.node_count; ++i) vrank[i] = nodes[i].rank;
    uint32_t next_dummy = g.node_count;
    uint32_t vedge_count = 0;
    uint32_t wp_used = 0;
    int32_t feedback_lanes = 0;
    for (uint32_t i = 0; i < g.edge_count; ++i) {
        const auto& e = edges[i];
        edge_paths[i] = { 0, 0, -1 };
        if (e.from == e.to) continue;
        if (e.reversed) {
            edge_paths[i].feedback_lane = feedback_lanes++;
            vedges[vedge_count++] = { e.to, e.from };
            continue;
        }
        const uint32_t r0 = nodes[e.from].rank, r1 = nodes[e.to].rank;
        if (r1 > r0 + 1 && (next_dummy - g.node_count) + (r1 - r0 - 1) <= dummy_count) {
            edge_paths[i].point_offset = wp_used;
            edge_paths[i].point_count = r1 - r0 - 1;
            uint32_t prev = e.from;
            for (uint32_t r = r0 + 1; r < r1; ++r) {
                const uint32_t d = next_dummy++;
                vrank[d] = r;
                dummy_to_wp[d - g.node_count] = wp_used++;
                vedges[vedge_count++] = { prev, d };
                prev = d;
            }
            vedges[vedge_count++] = { prev, e.to };
        } else {
            vedges[vedge_count++] = { e.from, e.to };
        }
    }

    // 4. Bidirectional barycentric crossing minimization (downward + upward alternating sweeps).
    for (uint32_t i = 0; i < vnode_count; ++i) order[i] = i;
    const auto sort_by_rank_bary = [&]() noexcept {
        for (uint32_t i = 1; i < vnode_count; ++i) {
            const uint32_t val = order[i];
            uint32_t j = i;
            while (j && (vrank[order[j - 1]] > vrank[val] ||
                         (vrank[order[j - 1]] == vrank[val] && bary[order[j - 1]] > bary[val])))
            {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = val;
        }
    };
    for (uint32_t i = 0; i < vnode_count; ++i) bary[i] = float(i);
    sort_by_rank_bary();

    for (uint32_t pass = 0; pass < 6; ++pass) {
        const bool downward = (pass & 1u) == 0u;
        for (uint32_t i = 0; i < vnode_count; ++i) pos_in_rank[order[i]] = i;
        for (uint32_t i = 0; i < vnode_count; ++i) {
            const uint32_t v = order[i];
            float sum = 0.0f;
            uint32_t cnt = 0;
            for (uint32_t ei = 0; ei < vedge_count; ++ei) {
                if (downward) {
                    if (vedges[ei].v == v) { sum += float(pos_in_rank[vedges[ei].u]); ++cnt; }
                } else {
                    if (vedges[ei].u == v) { sum += float(pos_in_rank[vedges[ei].v]); ++cnt; }
                }
            }
            bary[v] = cnt ? (sum / float(cnt)) : float(pos_in_rank[v]);
        }
        sort_by_rank_bary();
    }

    // 5-7. Geometry placement (reusable in-place for native layout-space auto-fit).
    auto* rank_gaps = static_cast<float*>(env.alloc(size_t(max_rank ? max_rank : 1u) * sizeof(float), alignof(float)));
    auto* rank_cross = static_cast<float*>(env.alloc(size_t(max_rank + 1) * sizeof(float), alignof(float)));
    auto* rank_start_pos = static_cast<float*>(env.alloc(size_t(max_rank + 1) * sizeof(float), alignof(float)));
    auto* rank_depth = static_cast<float*>(env.alloc(size_t(max_rank + 1) * sizeof(float), alignof(float)));
    if (!rank_gaps || !rank_cross || !rank_start_pos || !rank_depth) return {};

    const auto place_geometry = [&](float s) noexcept -> EtchVec2 {
        const float eff_font = std::fmax(7.0f, input->font_size_px * s);
        const float caption_font = std::fmax(7.0f, std::fmax(10.0f * s, eff_font * 0.82f));
        const float eff_node_gap = input->node_gap_px * s;
        const float eff_rank_gap = input->rank_gap_px * s;
        const float label_pad_x = 7.0f * s;
        const float label_pad_y = 2.5f * s;

        for (uint32_t r = 0; r < max_rank; ++r) rank_gaps[r] = eff_rank_gap;
        float label_width = 0.0f;
        for (uint32_t i = 0; i < g.edge_count; ++i) if (edges[i].label.length) {
            const EtchVec2 lsz = measure_label_env(edges[i].label, body_font, caption_font, env);
            const float plate_w = lsz.x + 2.0f * label_pad_x;
            const float plate_h = lsz.y + 2.0f * label_pad_y;
            label_width = std::fmax(label_width, plate_w);
            if (!edges[i].reversed && edges[i].from != edges[i].to) {
                const uint32_t r0 = nodes[edges[i].from].rank;
                const uint32_t r1 = nodes[edges[i].to].rank;
                if (r1 > r0) {
                    const uint32_t r_mid = r0 + (r1 - r0) / 2u;
                    if (r_mid < max_rank) {
                        if (g.lr) rank_gaps[r_mid] = std::fmax(rank_gaps[r_mid], plate_w + 16.0f * s);
                        else rank_gaps[r_mid] = std::fmax(rank_gaps[r_mid], plate_h + 12.0f * s);
                    }
                }
            }
        }

        for (uint32_t i = 0; i < g.node_count; ++i) {
            if (shapes[i] == InlayShapeStateStart) {
                const float d = std::fmax(12.0f, 16.0f * s);
                nodes[i].bounds = { 0.0f, 0.0f, d, d };
                continue;
            }
            if (shapes[i] == InlayShapeStateEnd) {
                const float d = std::fmax(16.0f, 22.0f * s);
                nodes[i].bounds = { 0.0f, 0.0f, d, d };
                continue;
            }
            const auto text = measure_label_env(nodes[i].label, body_font, eff_font, env);
            float w = text.x + 32.0f * s;
            float h = std::fmax(eff_font * 1.5f, text.y) + 24.0f * s;
            if (shapes[i] == InlayShapeDiamond) {
                w = text.x + 56.0f * s;
                h = std::fmax(h * 1.35f, std::fmax(eff_font * 2.4f, text.y * 1.45f) + 24.0f * s);
            } else if (shapes[i] == InlayShapeCircle) {
                const float d = std::fmax(w, h);
                w = d;
                h = d;
            } else if (shapes[i] == InlayShapeClassBox) {
                bool has_members = false;
                for (uint32_t li = 0u; li < nodes[i].label.length; ++li) {
                    if (nodes[i].label.chars[li] == '\n') { has_members = true; break; }
                }
                w = std::fmax(w, 128.0f * s);
                if (has_members) h += 12.0f * s;
            }
            nodes[i].bounds = { 0.0f, 0.0f, w, h };
        }

        float rank_position = 0.0f, total_cross = 0.0f;
        const float dummy_span = 14.0f * s;
        const float dummy_depth = 16.0f * s;
        for (uint32_t r = 0; r <= max_rank; ++r) {
            float cross = 0.0f, rsize = 0.0f;
            uint32_t in_r = 0;
            for (uint32_t i = 0; i < vnode_count; ++i) {
                const uint32_t vid = order[i];
                if (vrank[vid] != r) continue;
                if (vid < g.node_count) {
                    auto& n = nodes[vid];
                    n.bounds.x = input->pos.x + (g.lr ? rank_position : cross);
                    n.bounds.y = input->pos.y + (g.lr ? cross : rank_position);
                    cross += (g.lr ? n.bounds.h : n.bounds.w) + eff_node_gap;
                    rsize = std::fmax(rsize, g.lr ? n.bounds.w : n.bounds.h);
                } else {
                    const uint32_t wpi = dummy_to_wp[vid - g.node_count];
                    waypoints[wpi] = {
                        input->pos.x + (g.lr ? rank_position : cross + dummy_span * 0.5f),
                        input->pos.y + (g.lr ? cross + dummy_span * 0.5f : rank_position)
                    };
                    cross += dummy_span + eff_node_gap;
                    rsize = std::fmax(rsize, dummy_depth);
                }
                ++in_r;
            }
            const float used_cross = (in_r > 0u && cross > eff_node_gap) ? (cross - eff_node_gap) : 0.0f;
            rank_cross[r] = used_cross;
            rank_start_pos[r] = rank_position;
            rank_depth[r] = rsize;
            total_cross = std::fmax(total_cross, used_cross);
            rank_position += rsize + (r < max_rank ? rank_gaps[r] : 0.0f);
        }

        // Center each rank along the cross axis and align virtual waypoints to rank midlines.
        for (uint32_t i = 0; i < vnode_count; ++i) {
            const uint32_t vid = order[i];
            const uint32_t r = vrank[vid];
            const float shift = (total_cross - rank_cross[r]) * 0.5f;
            if (vid < g.node_count) {
                if (g.lr) nodes[vid].bounds.y += shift;
                else nodes[vid].bounds.x += shift;
            } else {
                const uint32_t wpi = dummy_to_wp[vid - g.node_count];
                if (g.lr) {
                    waypoints[wpi].x = input->pos.x + rank_start_pos[r] + rank_depth[r] * 0.5f;
                    waypoints[wpi].y += shift;
                } else {
                    waypoints[wpi].x += shift;
                    waypoints[wpi].y = input->pos.y + rank_start_pos[r] + rank_depth[r] * 0.5f;
                }
            }
        }

        // Mirror main axis for RL / BT directions.
        if (g.reverse_dir) {
            const float total_main = rank_position;
            for (uint32_t i = 0; i < g.node_count; ++i) {
                if (g.lr) nodes[i].bounds.x = input->pos.x + total_main - (nodes[i].bounds.x - input->pos.x) - nodes[i].bounds.w;
                else nodes[i].bounds.y = input->pos.y + total_main - (nodes[i].bounds.y - input->pos.y) - nodes[i].bounds.h;
            }
            for (uint32_t i = 0; i < wp_used; ++i) {
                if (g.lr) waypoints[i].x = input->pos.x + total_main - (waypoints[i].x - input->pos.x);
                else waypoints[i].y = input->pos.y + total_main - (waypoints[i].y - input->pos.y);
            }
        }

        // Compute subgraph cluster bounding boxes (bottom-up for nested subgraphs).
        if (g.subgraph_count > 0) {
            for (uint32_t si = 0; si < g.subgraph_count; ++si) {
                subgraphs[si].bounds = {};
                subgraphs[si].has_nodes = false;
            }
            const float title_font = std::fmax(7.5f, std::fmax(11.0f * s, eff_font * 0.85f));
            const float title_h = eff_font * 1.4f;
            const float pad_x = 14.0f * s, pad_top = title_h + 10.0f * s, pad_bot = 14.0f * s;
            const auto expand_rect = [](EtchRect* dst, bool* has, EtchRect r) noexcept {
                if (!*has) { *dst = r; *has = true; return; }
                const float x0 = std::fmin(dst->x, r.x), y0 = std::fmin(dst->y, r.y);
                const float x1 = std::fmax(dst->x + dst->w, r.x + r.w);
                const float y1 = std::fmax(dst->y + dst->h, r.y + r.h);
                *dst = { x0, y0, x1 - x0, y1 - y0 };
            };
            for (uint32_t i = 0; i < g.node_count; ++i) {
                const int32_t sg = node_sub[i];
                if (sg >= 0 && uint32_t(sg) < g.subgraph_count) {
                    expand_rect(&subgraphs[sg].bounds, &subgraphs[sg].has_nodes, nodes[i].bounds);
                }
            }
            for (int32_t si = int32_t(g.subgraph_count) - 1; si >= 0; --si) {
                auto& sg = subgraphs[si];
                if (!sg.has_nodes) continue;
                const float title_w = env.measure_text(sg.title, bold_font, title_font).x + 30.0f * s;
                sg.bounds.x -= pad_x;
                sg.bounds.y -= pad_top;
                sg.bounds.w = std::fmax(sg.bounds.w + pad_x * 2.0f, title_w);
                sg.bounds.h += pad_top + pad_bot;
                if (sg.parent >= 0 && sg.parent < si) {
                    expand_rect(&subgraphs[sg.parent].bounds, &subgraphs[sg.parent].has_nodes, sg.bounds);
                }
            }
            float min_x = input->pos.x, min_y = input->pos.y;
            for (uint32_t si = 0; si < g.subgraph_count; ++si) if (subgraphs[si].has_nodes) {
                min_x = std::fmin(min_x, subgraphs[si].bounds.x);
                min_y = std::fmin(min_y, subgraphs[si].bounds.y);
            }
            const float dx = input->pos.x - min_x, dy = input->pos.y - min_y;
            if (dx > 0.0f || dy > 0.0f) {
                for (uint32_t i = 0; i < g.node_count; ++i) { nodes[i].bounds.x += dx; nodes[i].bounds.y += dy; }
                for (uint32_t i = 0; i < wp_used; ++i) { waypoints[i].x += dx; waypoints[i].y += dy; }
                for (uint32_t si = 0; si < g.subgraph_count; ++si) if (subgraphs[si].has_nodes) {
                    subgraphs[si].bounds.x += dx;
                    subgraphs[si].bounds.y += dy;
                }
            }
        }

        float width = 0.0f, height = 0.0f;
        for (uint32_t i = 0; i < g.node_count; ++i) {
            width = std::fmax(width, nodes[i].bounds.x - input->pos.x + nodes[i].bounds.w);
            height = std::fmax(height, nodes[i].bounds.y - input->pos.y + nodes[i].bounds.h);
        }
        for (uint32_t si = 0; si < g.subgraph_count; ++si) if (subgraphs[si].has_nodes) {
            width = std::fmax(width, subgraphs[si].bounds.x - input->pos.x + subgraphs[si].bounds.w);
            height = std::fmax(height, subgraphs[si].bounds.y - input->pos.y + subgraphs[si].bounds.h);
        }
        for (uint32_t i = 0; i < g.edge_count; ++i) {
            if (edges[i].from == edges[i].to) {
                width = std::fmax(width, nodes[edges[i].from].bounds.x - input->pos.x + nodes[edges[i].from].bounds.w + 40.0f * s);
            }
        }
        if (feedback_lanes > 0) {
            const float lane_extra = (22.0f + float(feedback_lanes) * 18.0f) * s;
            if (g.lr) height += lane_extra; else width += lane_extra;
        }
        return { std::fmax(width, label_width), height };
    };

    const EtchVec2 nat = place_geometry(1.0f);
    float final_scale = 1.0f;
    EtchVec2 final_sz = nat;
    if (input->max_width_px > 0.0f && nat.x > input->max_width_px) {
        float s = std::fmax(min_scale, std::fmin(1.0f, input->max_width_px / nat.x));
        final_sz = place_geometry(s);
        for (int iter = 0; iter < 3 && final_sz.x > input->max_width_px && s > min_scale; ++iter) {
            s = std::fmax(min_scale, s * (input->max_width_px / final_sz.x) * 0.992f);
            final_sz = place_geometry(s);
        }
        final_scale = s;
    }

    if (out_meta) {
        out_meta->shapes = shapes;
        out_meta->edge_styles = edge_styles;
        out_meta->subgraphs = subgraphs;
        out_meta->subgraph_count = g.subgraph_count;
        out_meta->edge_paths = edge_paths;
        out_meta->waypoints = waypoints;
        out_meta->waypoint_count = wp_used;
        out_meta->reversed_dir = g.reverse_dir;
        out_meta->diagram_kind = class_diag ? InlayDiagramClass : (er_diag ? InlayDiagramEr : (state_diag ? InlayDiagramState : InlayDiagramFlowchart));
    }
    return { nodes, edges, g.node_count, g.edge_count, final_sz.x, final_sz.y, true, g.lr, false, final_scale, nat.x, nat.y, warnings };
}

struct DiagramSvgOut {
    char* buf;
    uint32_t cap;
    uint32_t len;

    void write(const char* s, uint32_t n) noexcept {
        if (!n) return;
        if (buf && cap > 0u && len + 1u < cap) {
            const uint32_t avail = (cap - 1u) - len;
            const uint32_t cp = n < avail ? n : avail;
            if (cp > 0u) std::memcpy(buf + len, s, cp);
        }
        len += n;
    }
    void write_lit(const char* lit) noexcept {
        write(lit, static_cast<uint32_t>(std::strlen(lit)));
    }
    void write_num(float v) noexcept {
        char tmp[32];
        const int n = std::snprintf(tmp, sizeof(tmp), "%.2f", std::isfinite(v) ? v : 0.0f);
        if (n <= 0) return;
        uint32_t l = static_cast<uint32_t>(n);
        while (l > 1u && tmp[l - 1u] == '0') --l;
        if (l > 1u && tmp[l - 1u] == '.') --l;
        write(tmp, l);
    }
    void write_escaped(const char* s, uint32_t n) noexcept {
        uint32_t run = 0u;
        for (uint32_t i = 0u; i < n; ++i) {
            const char* rep = nullptr;
            switch (s[i]) {
                case '&': rep = "&amp;"; break;
                case '<': rep = "&lt;"; break;
                case '>': rep = "&gt;"; break;
                case '"': rep = "&quot;"; break;
                default: break;
            }
            if (rep) {
                if (i > run) write(s + run, i - run);
                write_lit(rep);
                run = i + 1u;
            }
        }
        if (n > run) write(s + run, n - run);
    }
};

void svg_emit_diagram_label(DiagramSvgOut* out, EtchStr label, float cx, float top_y, float line_h,
                            float font_size, const char* fill_hex, const char* weight = nullptr,
                            const char* anchor = "middle") noexcept {
    if (!label.chars || !label.length) return;
    uint32_t pos = 0u;
    uint32_t line_idx = 0u;
    while (pos <= label.length) {
        uint32_t stop = pos;
        uint32_t tag_len = 0u;
        while (stop < label.length) {
            tag_len = match_br_tag(label.chars, label.length, stop);
            if (tag_len > 0u) break;
            ++stop;
        }
        const EtchStr seg = trim_slice(label, pos, stop);
        if (seg.length) {
            const float y = top_y + float(line_idx) * line_h + font_size * 0.85f;
            out->write_lit("<text x=\""); out->write_num(cx);
            out->write_lit("\" y=\""); out->write_num(y);
            out->write_lit("\" text-anchor=\""); out->write_lit(anchor);
            out->write_lit("\" font-family=\"system-ui, -apple-system, sans-serif\" font-size=\"");
            out->write_num(font_size);
            if (weight) { out->write_lit("\" font-weight=\""); out->write_lit(weight); }
            out->write_lit("\" fill=\""); out->write_lit(fill_hex);
            out->write_lit("\">");
            out->write_escaped(seg.chars, seg.length);
            out->write_lit("</text>\n");
        }
        ++line_idx;
        if (tag_len == 0u) break;
        pos = stop + tag_len;
    }
}

void svg_emit_bezier_polyline(DiagramSvgOut* out, const EtchBezierDesc& bz, float ox, float oy,
                              const char* stroke, float stroke_w, bool dashed,
                              const char* marker_start, const char* marker_end) noexcept {
    out->write_lit("<polyline fill=\"none\" stroke=\""); out->write_lit(stroke);
    out->write_lit("\" stroke-width=\""); out->write_num(stroke_w);
    if (dashed) out->write_lit("\" stroke-dasharray=\"6,4");
    if (marker_start) { out->write_lit("\" marker-start=\"url(#"); out->write_lit(marker_start); out->write_lit(")"); }
    if (marker_end) { out->write_lit("\" marker-end=\"url(#"); out->write_lit(marker_end); out->write_lit(")"); }
    out->write_lit("\" points=\"");
    constexpr uint32_t kSteps = 10u;
    for (uint32_t k = 0u; k <= kSteps; ++k) {
        const EtchVec2 pt = curve_point(bz, float(k) / float(kSteps));
        if (k > 0u) out->write_lit(" ");
        out->write_num(pt.x + ox);
        out->write_lit(",");
        out->write_num(pt.y + oy);
    }
    out->write_lit("\"/>\n");
}
} // namespace

InlayDiagramResult inlay_diagram_layout_ex(EtchStr source, const InlayDiagramDesc* input, InlayDiagramMeta* out_meta) noexcept {
    auto* ctx = etch_current_context();
    if (!ctx || !etch_frame_in_progress(ctx)) {
        if (out_meta) *out_meta = {};
        return {};
    }
    const DiagramEnv env = { ctx, nullptr, true };
    return diagram_layout_core(source, input, out_meta, env);
}

void inlay_diagram_draw_ex(const InlayDiagramResult& result, const InlayDiagramDesc& input, const InlayDiagramMeta* meta) noexcept {
    const auto theme = etch_get_theme(etch_current_context());
    const bool dark = inlay_is_dark_theme(theme);
    const float scale = (result.scale > 0.0f && std::isfinite(result.scale)) ? std::fmin(1.0f, result.scale) : 1.0f;
    const float eff_font = std::fmax(7.0f, input.font_size_px * scale);
    const float caption_font = std::fmax(7.0f, std::fmax(10.0f * scale, eff_font * 0.82f));
    const float eff_rank_gap = input.rank_gap_px * scale;
    const uint16_t body_font = resolve_body_font(input);
    const uint16_t bold_font = resolve_bold_font(input);
    const float stroke_normal = std::fmax(1.5f, 1.85f * scale);
    const float stroke_thick = std::fmax(2.25f, 2.85f * scale);
    const float border_w = std::fmax(0.85f, 1.0f * scale);

    if (input.canvas_id) {
        auto canvas = etch_canvas_desc_default();
        canvas.id = input.canvas_id;
        canvas.width = etch_sizing_fixed(input.viewport_width_px);
        canvas.height = etch_sizing_fixed(input.viewport_height_px);
        etch_begin_canvas(&canvas);
    }

    // Specialized Pie & Gantt rendering paths
    if (meta && meta->diagram_kind == InlayDiagramPie && meta->pie_slices && meta->pie_slice_count > 0u) {
        static constexpr EtchColor kPieColors[8] = {
            {59, 130, 246, 255}, {16, 185, 129, 255}, {245, 158, 11, 255}, {239, 68, 68, 255},
            {139, 92, 246, 255}, {6, 182, 212, 255}, {249, 115, 22, 255}, {236, 72, 153, 255}
        };
        if (meta->title.length > 0u) {
            draw_label(meta->title, { input.pos.x + result.width * 0.5f, input.pos.y + 8.0f * scale },
                       bold_font, eff_font * 1.08f, theme.text_primary, ETCH_ALIGN_X_CENTER);
        }
        const EtchVec2 c = meta->pie_center;
        const float r = meta->pie_radius;
        for (uint32_t i = 0u; i < meta->pie_slice_count; ++i) {
            const auto& sl = meta->pie_slices[i];
            const EtchColor col = kPieColors[sl.color_index % 8u];
            uint32_t segs = static_cast<uint32_t>(std::ceil(std::fabs(sl.sweep_rad) / 0.16f));
            if (segs < 4u) segs = 4u;
            if (segs > 44u) segs = 44u;
            EtchVec2 pts[46] = {};
            pts[0] = c;
            for (uint32_t k = 0u; k <= segs; ++k) {
                const float a = sl.start_rad + sl.sweep_rad * (float(k) / float(segs));
                pts[k + 1u] = { c.x + std::cos(a) * r, c.y + std::sin(a) * r };
            }
            EtchFilledPolygonDesc poly = etch_filled_polygon_desc_default();
            poly.points = pts;
            poly.point_count = segs + 2u;
            poly.fill = col;
            poly.stroke_color = theme.bg_surface;
            poly.stroke_width = std::fmax(1.0f, 1.5f * scale);
            etch_draw_filled_polygon(&poly);
            if (i < result.node_count) {
                const auto& nr = result.nodes[i];
                const float sw = 10.0f * scale;
                auto swatch = etch_rect_draw_desc_default();
                swatch.rect = { nr.bounds.x, nr.bounds.y + (nr.bounds.h - sw) * 0.5f, sw, sw };
                swatch.fill = col;
                swatch.radius = etch_corners_all(2.5f * scale);
                etch_draw_rect(&swatch);
                draw_label(nr.label, { nr.bounds.x + sw + 7.0f * scale, nr.bounds.y + (nr.bounds.h - eff_font * 0.9f) * 0.35f },
                           body_font, eff_font * 0.9f, theme.text_primary, ETCH_ALIGN_X_LEFT);
            }
        }
        if (input.canvas_id) etch_end_canvas();
        return;
    }
    if (meta && meta->diagram_kind == InlayDiagramGantt && meta->gantt_bars && meta->gantt_bar_count > 0u) {
        const float pad = 14.0f * scale;
        const float chart_x = input.pos.x + pad + meta->gantt_label_width;
        const float chart_w = std::fmax(40.0f, result.width - 2.0f * pad - meta->gantt_label_width);
        const float top_y = input.pos.y + pad + (meta->title.length ? (eff_font * 1.8f + 6.0f * scale) : 0.0f);
        const float bot_y = input.pos.y + result.height - pad;
        if (meta->title.length > 0u) {
            draw_label(meta->title, { input.pos.x + result.width * 0.5f, input.pos.y + 8.0f * scale },
                       bold_font, eff_font * 1.08f, theme.text_primary, ETCH_ALIGN_X_CENTER);
        }
        // Subtle vertical timeline grid guides
        for (uint32_t g = 0u; g <= 4u; ++g) {
            const float gx = chart_x + chart_w * (float(g) / 4.0f);
            EtchLineDesc gl = {};
            gl.from = { gx, top_y };
            gl.to = { gx, bot_y };
            gl.color = { theme.border_subtle.r, theme.border_subtle.g, theme.border_subtle.b, uint8_t(dark ? 90u : 130u) };
            gl.thickness = std::fmax(0.75f, 0.9f * scale);
            etch_draw_line(&gl);
        }
        const float sec_h = eff_font * 1.45f + 6.0f * scale;
        for (uint32_t i = 0u; i < meta->gantt_bar_count && i < result.node_count; ++i) {
            const auto& nr = result.nodes[i];
            const auto& bar = meta->gantt_bars[i];
            if (bar.section_first && bar.section.length > 0u) {
                auto sb = etch_rect_draw_desc_default();
                sb.rect = { nr.bounds.x, nr.bounds.y - sec_h + 2.0f * scale, nr.bounds.w, sec_h - 4.0f * scale };
                sb.fill = { theme.surface_subtle.r, theme.surface_subtle.g, theme.surface_subtle.b, uint8_t(dark ? 110u : 160u) };
                sb.radius = etch_corners_all(4.0f * scale);
                etch_draw_rect(&sb);
                draw_label(bar.section, { sb.rect.x + 8.0f * scale, sb.rect.y + 3.0f * scale },
                           bold_font, eff_font * 0.88f, theme.text_secondary, ETCH_ALIGN_X_LEFT);
            }
            draw_label(nr.label, { nr.bounds.x + 6.0f * scale, nr.bounds.y + (nr.bounds.h - eff_font * 0.92f) * 0.35f },
                       body_font, eff_font * 0.92f, theme.text_primary, ETCH_ALIGN_X_LEFT);
            EtchColor bcol = theme.accent;
            if (bar.status == 1u) bcol = etch_rgba8(100, 116, 139, 210);
            else if (bar.status == 2u) bcol = etch_rgba8(59, 130, 246, 255);
            else if (bar.status == 3u) bcol = etch_rgba8(239, 68, 68, 255);
            else bcol = etch_rgba8(16, 185, 129, 235);
            auto brect = etch_rect_draw_desc_default();
            brect.rect = bar.bar_bounds;
            brect.fill = bcol;
            brect.radius = etch_corners_all(4.0f * scale);
            brect.continuous_corners = true;
            etch_draw_rect(&brect);
        }
        if (input.canvas_id) etch_end_canvas();
        return;
    }
    if (meta && meta->diagram_kind == InlayDiagramGitGraph && result.node_count > 0u) {
        static constexpr EtchColor kLaneCols[8] = {
            {59, 130, 246, 255}, {16, 185, 129, 255}, {245, 158, 11, 255}, {139, 92, 246, 255},
            {239, 68, 68, 255}, {6, 182, 212, 255}, {236, 72, 153, 255}, {249, 115, 22, 255}
        };
        // 1. Branch lane guide lines & badges
        for (uint32_t bi = 0u; bi < meta->subgraph_count && meta->subgraphs; ++bi) {
            const auto& sg = meta->subgraphs[bi];
            const EtchColor lcol = kLaneCols[bi % 8u];
            const float cy = sg.bounds.y + sg.bounds.h * 0.5f;
            EtchLineDesc gl = {};
            gl.from = { sg.bounds.x, cy };
            gl.to = { sg.bounds.x + sg.bounds.w, cy };
            gl.color = { lcol.r, lcol.g, lcol.b, uint8_t(dark ? 65u : 85u) };
            gl.thickness = std::fmax(1.0f, 1.4f * scale);
            etch_draw_line(&gl);
            const EtchVec2 tsz = etch_measure_text(sg.title, bold_font, eff_font * 0.82f, 0);
            auto badge = etch_rect_draw_desc_default();
            badge.rect = { sg.bounds.x, cy - (tsz.y * 0.5f + 3.0f * scale), tsz.x + 14.0f * scale, tsz.y + 6.0f * scale };
            badge.fill = { lcol.r, lcol.g, lcol.b, uint8_t(dark ? 45u : 32u) };
            badge.border_color = lcol;
            badge.border_width = std::fmax(0.8f, 1.0f * scale);
            badge.radius = etch_corners_all(badge.rect.h * 0.5f);
            etch_draw_rect(&badge);
            draw_label(sg.title, { badge.rect.x + 7.0f * scale, badge.rect.y + 3.0f * scale },
                       bold_font, eff_font * 0.82f, lcol, ETCH_ALIGN_X_LEFT);
        }
        // 2. Commit & merge curves
        for (uint32_t ei = 0u; ei < result.edge_count; ++ei) {
            const auto& ed = result.edges[ei];
            const auto& a = result.nodes[ed.from];
            const auto& b = result.nodes[ed.to];
            const EtchColor ecol = kLaneCols[b.rank % 8u];
            EtchBezierDesc bz = {};
            bz.p0 = { a.bounds.x + a.bounds.w * 0.5f, a.bounds.y + a.bounds.h * 0.5f };
            bz.p3 = { b.bounds.x + b.bounds.w * 0.5f, b.bounds.y + b.bounds.h * 0.5f };
            bz.p1 = { (bz.p0.x + bz.p3.x) * 0.5f, bz.p0.y };
            bz.p2 = { (bz.p0.x + bz.p3.x) * 0.5f, bz.p3.y };
            bz.cubic = true;
            bz.color_start = ecol;
            bz.color_end = ecol;
            bz.thickness = std::fmax(1.8f, 2.4f * scale);
            if (ed.dashed) draw_dashed(bz, scale);
            else etch_draw_bezier(&bz);
        }
        // 3. Commit nodes & captions
        for (uint32_t ci = 0u; ci < result.node_count; ++ci) {
            const auto& n = result.nodes[ci];
            const EtchColor ccol = kLaneCols[n.rank % 8u];
            auto dot = etch_rect_draw_desc_default();
            dot.rect = n.bounds;
            dot.fill = ccol;
            dot.border_color = theme.bg_surface;
            dot.border_width = std::fmax(1.5f, 2.0f * scale);
            dot.radius = etch_corners_all(n.bounds.w * 0.5f);
            etch_draw_rect(&dot);
            draw_label(n.label, { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + n.bounds.h + 4.0f * scale },
                       body_font, caption_font, theme.text_secondary, ETCH_ALIGN_X_CENTER);
        }
        if (input.canvas_id) etch_end_canvas();
        return;
    }
    if (meta && meta->diagram_kind == InlayDiagramMindmap && result.node_count > 0u) {
        static constexpr EtchColor kDepthCols[6] = {
            {59, 130, 246, 255}, {16, 185, 129, 255}, {139, 92, 246, 255},
            {245, 158, 11, 255}, {6, 182, 212, 255}, {236, 72, 153, 255}
        };
        for (uint32_t ei = 0u; ei < result.edge_count; ++ei) {
            const auto& ed = result.edges[ei];
            const auto& a = result.nodes[ed.from];
            const auto& b = result.nodes[ed.to];
            const EtchColor ecol = kDepthCols[b.rank % 6u];
            EtchBezierDesc bz = {};
            bz.p0 = { a.bounds.x + a.bounds.w, a.bounds.y + a.bounds.h * 0.5f };
            bz.p3 = { b.bounds.x, b.bounds.y + b.bounds.h * 0.5f };
            const float mx = (bz.p0.x + bz.p3.x) * 0.5f;
            bz.p1 = { mx, bz.p0.y };
            bz.p2 = { mx, bz.p3.y };
            bz.cubic = true;
            bz.color_start = { ecol.r, ecol.g, ecol.b, 200u };
            bz.color_end = bz.color_start;
            bz.thickness = std::fmax(1.4f, 1.9f * scale);
            etch_draw_bezier(&bz);
        }
        for (uint32_t ni = 0u; ni < result.node_count; ++ni) {
            const auto& n = result.nodes[ni];
            const EtchColor ncol = kDepthCols[n.rank % 6u];
            const bool is_root = (n.rank == 0u);
            auto pill = etch_rect_draw_desc_default();
            pill.rect = n.bounds;
            pill.fill = is_root ? ncol : EtchColor{ ncol.r, ncol.g, ncol.b, uint8_t(dark ? 42u : 28u) };
            pill.border_color = ncol;
            pill.border_width = std::fmax(1.0f, 1.35f * scale);
            pill.radius = etch_corners_all(is_root ? (n.bounds.h * 0.5f) : (8.0f * scale));
            pill.continuous_corners = true;
            etch_draw_rect(&pill);
            const uint16_t f = is_root ? bold_font : body_font;
            const float fsz = is_root ? (eff_font * 1.05f) : (eff_font * 0.92f);
            const EtchVec2 tsz = etch_measure_text(n.label, f, fsz, 0);
            draw_label(n.label, { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + (n.bounds.h - tsz.y) * 0.5f },
                       f, fsz, is_root ? EtchColor{255, 255, 255, 255} : theme.text_primary, ETCH_ALIGN_X_CENTER);
        }
        if (input.canvas_id) etch_end_canvas();
        return;
    }
    if (meta && meta->diagram_kind == InlayDiagramTimeline && result.node_count > 0u) {
        static constexpr EtchColor kColCols[6] = {
            {59, 130, 246, 255}, {16, 185, 129, 255}, {139, 92, 246, 255},
            {245, 158, 11, 255}, {6, 182, 212, 255}, {236, 72, 153, 255}
        };
        if (meta->title.length > 0u) {
            draw_label(meta->title, { input.pos.x + result.width * 0.5f, input.pos.y + 8.0f * scale },
                       bold_font, eff_font * 1.08f, theme.text_primary, ETCH_ALIGN_X_CENTER);
        }
        for (uint32_t si = 0u; si < meta->subgraph_count && meta->subgraphs; ++si) {
            const auto& sg = meta->subgraphs[si];
            if (!sg.has_nodes) continue;
            auto sb = etch_rect_draw_desc_default();
            sb.rect = sg.bounds;
            sb.fill = { theme.surface_subtle.r, theme.surface_subtle.g, theme.surface_subtle.b, uint8_t(dark ? 120u : 180u) };
            sb.radius = etch_corners_all(5.0f * scale);
            etch_draw_rect(&sb);
            draw_label(sg.title, { sg.bounds.x + sg.bounds.w * 0.5f, sg.bounds.y + 3.0f * scale },
                       bold_font, eff_font * 0.86f, theme.text_secondary, ETCH_ALIGN_X_CENTER);
        }
        // Horizontal timeline axis connecting period headers
        float axis_y = 0.0f, axis_x0 = 1e9f, axis_x1 = -1e9f;
        for (uint32_t i = 0u; i < result.node_count; ++i) {
            if (meta->shapes && meta->shapes[i] == InlayShapeRound) {
                const auto& b = result.nodes[i].bounds;
                axis_y = b.y + b.h * 0.5f;
                if (b.x < axis_x0) axis_x0 = b.x;
                if (b.x + b.w > axis_x1) axis_x1 = b.x + b.w;
            }
        }
        if (axis_x1 > axis_x0) {
            EtchLineDesc ax = {};
            ax.from = { axis_x0, axis_y };
            ax.to = { axis_x1 + 10.0f * scale, axis_y };
            ax.color = theme.border_control.a ? theme.border_control : theme.border_line;
            ax.thickness = std::fmax(1.5f, 2.0f * scale);
            etch_draw_line(&ax);
        }
        for (uint32_t i = 0u; i < result.node_count; ++i) {
            const auto& n = result.nodes[i];
            const EtchColor ccol = kColCols[n.rank % 6u];
            const bool is_hdr = meta->shapes && meta->shapes[i] == InlayShapeRound;
            auto r = etch_rect_draw_desc_default();
            r.rect = n.bounds;
            r.fill = is_hdr ? ccol : theme.bg_elevated;
            r.border_color = is_hdr ? ccol : EtchColor{ ccol.r, ccol.g, ccol.b, 160u };
            r.border_width = std::fmax(1.0f, 1.2f * scale);
            r.radius = etch_corners_all(is_hdr ? (n.bounds.h * 0.5f) : (6.0f * scale));
            r.continuous_corners = true;
            etch_draw_rect(&r);
            const uint16_t f = is_hdr ? bold_font : body_font;
            const float fsz = eff_font * 0.9f;
            const EtchVec2 tsz = etch_measure_text(n.label, f, fsz, 0);
            draw_label(n.label, { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + (n.bounds.h - tsz.y) * 0.5f },
                       f, fsz, is_hdr ? EtchColor{255, 255, 255, 255} : theme.text_primary, ETCH_ALIGN_X_CENTER);
        }
        if (input.canvas_id) etch_end_canvas();
        return;
    }

    // 1. Subgraph cluster frames (outer before inner).
    if (meta && meta->subgraphs && meta->subgraph_count > 0) {
        const float title_font = std::fmax(7.5f, std::fmax(11.0f * scale, eff_font * 0.85f));
        const uint8_t sg_alpha = uint8_t(dark ? 128u : 168u);
        for (uint32_t si = 0; si < meta->subgraph_count; ++si) {
            const auto& sg = meta->subgraphs[si];
            if (!sg.has_nodes) continue;
            auto frame = etch_rect_draw_desc_default();
            frame.rect = sg.bounds;
            frame.fill = { theme.surface_subtle.r, theme.surface_subtle.g, theme.surface_subtle.b, sg_alpha };
            frame.border_color = theme.border_control.a != 0 ? theme.border_control : theme.border_subtle;
            frame.border_width = border_w;
            frame.radius = etch_corners_all(theme.radius_panel_px * scale);
            frame.continuous_corners = true;
            etch_draw_rect(&frame);
            if (sg.title.length) {
                const EtchVec2 tsz = etch_measure_text(sg.title, bold_font, title_font, 0);
                const float pill_px = 6.5f * scale;
                const float pill_py = 2.0f * scale;
                const float max_pw = std::fmax(1.0f, sg.bounds.w - 16.0f * scale);
                auto pill = etch_rect_draw_desc_default();
                pill.rect = {
                    sg.bounds.x + 8.0f * scale,
                    sg.bounds.y + 4.5f * scale,
                    std::fmin(max_pw, tsz.x + 2.0f * pill_px),
                    tsz.y + 2.0f * pill_py
                };
                pill.fill = dark
                    ? etch_color_blend_over(theme.bg_elevated, { theme.surface_subtle.r, theme.surface_subtle.g, theme.surface_subtle.b, 180 })
                    : theme.bg_elevated;
                pill.border_color = theme.border_subtle;
                pill.border_width = std::fmax(0.5f, 0.75f * scale);
                pill.radius = etch_corners_all(theme.radius_small_px * 0.75f * scale);
                pill.continuous_corners = true;
                etch_draw_rect(&pill);
                auto td = etch_text_draw_desc_default();
                td.text = sg.title;
                td.font_id = bold_font;
                td.pos = { pill.rect.x + pill_px, pill.rect.y + pill_py };
                td.size = title_font;
                td.color = theme.text_secondary;
                etch_draw_text(&td);
            }
        }
    }

    // 2. Sequence diagram lifelines.
    if (result.sequence) {
        const EtchColor life_col = theme.border_control.a != 0 ? theme.border_control : theme.border_line;
        for (uint32_t i = 0; i < result.node_count; ++i) {
            const auto b = result.nodes[i].bounds;
            EtchLineDesc line = {};
            line.from = { b.x + b.w * 0.5f, b.y + b.h };
            line.to = { line.from.x, input.pos.y + result.height };
            line.color = life_col;
            line.thickness = border_w;
            etch_draw_line(&line);
        }
    }

    // 3. Edges (including multi-rank waypoint splines, RL/BT direction, thick/no-arrow/bidir, & separated feedback lanes).
    for (uint32_t i = 0; i < result.edge_count; ++i) {
        const auto& edge = result.edges[i];
        const auto a = result.nodes[edge.from].bounds, b = result.nodes[edge.to].bounds;
        const EtchColor col = edge.reversed ? theme.accent_secondary : theme.accent;
        const InlayDiagramEdgePath path = (meta && meta->edge_paths) ? meta->edge_paths[i] : InlayDiagramEdgePath{ 0, 0, edge.reversed ? 0 : -1 };
        const uint8_t estyle = (meta && meta->edge_styles) ? meta->edge_styles[i] : uint8_t(InlayEdgeStyleNone);
        const bool no_arrow = (estyle & InlayEdgeStyleNoArrow) != 0;
        const bool bidir = (estyle & InlayEdgeStyleBidir) != 0;
        const float thickness = (estyle & InlayEdgeStyleThick) ? stroke_thick : stroke_normal;
        const bool rev_dir = (meta && meta->reversed_dir) ||
            (!result.sequence && !edge.reversed && edge.from != edge.to &&
             (result.horizontal ? (a.x > b.x + b.w * 0.5f) : (a.y > b.y + b.h * 0.5f)));

        EtchVec2 label_mid = {};
        if (!result.sequence && !edge.reversed && edge.from != edge.to && path.point_count > 0 && meta && meta->waypoints) {
            // Multi-rank routed spline through Sugiyama virtual waypoints.
            const uint32_t segs = path.point_count + 1;
            const EtchVec2 start_pt = result.horizontal
                ? (rev_dir ? EtchVec2{ a.x, a.y + a.h * 0.5f } : EtchVec2{ a.x + a.w, a.y + a.h * 0.5f })
                : (rev_dir ? EtchVec2{ a.x + a.w * 0.5f, a.y } : EtchVec2{ a.x + a.w * 0.5f, a.y + a.h });
            const EtchVec2 end_pt = result.horizontal
                ? (rev_dir ? EtchVec2{ b.x + b.w, b.y + b.h * 0.5f } : EtchVec2{ b.x, b.y + b.h * 0.5f })
                : (rev_dir ? EtchVec2{ b.x + b.w * 0.5f, b.y + b.h } : EtchVec2{ b.x + b.w * 0.5f, b.y });
            EtchVec2 first_tan = { 0.0f, -1.0f };
            EtchVec2 last_tan = { 0.0f, 1.0f };
            for (uint32_t s = 0; s < segs; ++s) {
                const EtchVec2 p0 = (s == 0) ? start_pt : meta->waypoints[path.point_offset + s - 1];
                const EtchVec2 p3 = (s + 1 == segs) ? end_pt : meta->waypoints[path.point_offset + s];
                auto bz = etch_bezier_desc_default();
                bz.p0 = p0;
                bz.p3 = p3;
                bz.p1 = result.horizontal ? EtchVec2{ (p0.x + p3.x) * 0.5f, p0.y } : EtchVec2{ p0.x, (p0.y + p3.y) * 0.5f };
                bz.p2 = result.horizontal ? EtchVec2{ bz.p1.x, p3.y } : EtchVec2{ p3.x, bz.p1.y };
                bz.cubic = true;
                bz.color_start = col;
                bz.color_end = col;
                bz.thickness = thickness;
                if (edge.dashed) draw_dashed(bz, scale); else etch_draw_bezier(&bz);
                if (s == 0) first_tan = { bz.p0.x - bz.p1.x, bz.p0.y - bz.p1.y };
                if (s == segs / 2) label_mid = curve_point(bz, 0.5f);
                if (s + 1 == segs) last_tan = { bz.p3.x - bz.p2.x, bz.p3.y - bz.p2.y };
            }
            if (!no_arrow) draw_arrowhead(end_pt, last_tan, col, scale);
            if (bidir) draw_arrowhead(start_pt, first_tan, col, scale);
        } else {
            auto bezier = etch_bezier_desc_default();
            if (result.horizontal) {
                if (rev_dir) {
                    bezier.p0 = { a.x, a.y + a.h * 0.5f };
                    bezier.p3 = { b.x + b.w, b.y + b.h * 0.5f };
                } else {
                    bezier.p0 = { a.x + a.w, a.y + a.h * 0.5f };
                    bezier.p3 = { b.x, b.y + b.h * 0.5f };
                }
            } else {
                if (rev_dir) {
                    bezier.p0 = { a.x + a.w * 0.5f, a.y };
                    bezier.p3 = { b.x + b.w * 0.5f, b.y + b.h };
                } else {
                    bezier.p0 = { a.x + a.w * 0.5f, a.y + a.h };
                    bezier.p3 = { b.x + b.w * 0.5f, b.y };
                }
            }
            if (result.sequence) {
                const float y = input.pos.y + a.h + float(i + 1) * eff_rank_gap;
                bezier.p0 = { a.x + a.w * 0.5f, y };
                bezier.p3 = { b.x + b.w * 0.5f, y };
            }
            bezier.p1 = result.horizontal ? EtchVec2{ (bezier.p0.x + bezier.p3.x) * 0.5f, bezier.p0.y } : EtchVec2{ bezier.p0.x, (bezier.p0.y + bezier.p3.y) * 0.5f };
            bezier.p2 = result.horizontal ? EtchVec2{ bezier.p1.x, bezier.p3.y } : EtchVec2{ bezier.p3.x, bezier.p1.y };
            if (result.sequence) {
                const float third = (bezier.p3.x - bezier.p0.x) / 3.0f;
                bezier.p1 = { bezier.p0.x + third, bezier.p0.y };
                bezier.p2 = { bezier.p3.x - third, bezier.p3.y };
            }
            if (edge.reversed) {
                const float lane_off = float(path.feedback_lane >= 0 ? path.feedback_lane : 0) * 18.0f * scale;
                if (result.horizontal) {
                    const float route = input.pos.y + result.height - 8.0f * scale - lane_off;
                    bezier.p0 = { a.x + a.w * 0.5f, a.y + a.h };
                    bezier.p3 = { b.x + b.w * 0.5f, b.y + b.h };
                    bezier.p1 = { bezier.p0.x, route };
                    bezier.p2 = { bezier.p3.x, route };
                } else {
                    const float route = input.pos.x + result.width - 8.0f * scale - lane_off;
                    bezier.p0 = { a.x + a.w, a.y + a.h * 0.5f };
                    bezier.p3 = { b.x + b.w, b.y + b.h * 0.5f };
                    bezier.p1 = { route, bezier.p0.y };
                    bezier.p2 = { route, bezier.p3.y };
                }
            }
            if (edge.from == edge.to) {
                const float loop_y = result.sequence ? bezier.p0.y : a.y + a.h * 0.5f;
                const float loop_x = result.sequence ? a.x + a.w * 0.5f : a.x + a.w;
                const float loop_r = 40.0f * scale;
                const float loop_dy = 12.0f * scale;
                bezier.p0 = { loop_x, loop_y - loop_dy };
                bezier.p3 = { loop_x, loop_y + loop_dy };
                bezier.p1 = { loop_x + loop_r, bezier.p0.y };
                bezier.p2 = { loop_x + loop_r, bezier.p3.y };
            }
            bezier.cubic = true;
            bezier.color_start = col;
            bezier.color_end = col;
            bezier.thickness = thickness;
            if (edge.dashed) draw_dashed(bezier, scale); else etch_draw_bezier(&bezier);
            EtchVec2 tangent = { bezier.p3.x - bezier.p2.x, bezier.p3.y - bezier.p2.y };
            if (result.sequence && edge.from != edge.to) tangent = { bezier.p3.x - bezier.p0.x, 0.0f };
            if (!no_arrow) draw_arrowhead(bezier.p3, tangent, col, scale);
            if (bidir) {
                const EtchVec2 start_tan = { bezier.p0.x - bezier.p1.x, bezier.p0.y - bezier.p1.y };
                draw_arrowhead(bezier.p0, start_tan, col, scale);
            }
            label_mid = curve_point(bezier, 0.5f);
        }

        if (edge.label.length) {
            const EtchVec2 sz = measure_label(edge.label, body_font, caption_font);
            const float pad_x = 7.0f * scale;
            const float pad_y = 2.5f * scale;
            const float plate_w = sz.x + 2.0f * pad_x;
            const float plate_h = sz.y + 2.0f * pad_y;
            const float half = plate_w * 0.5f;
            const float x = std::fmax(input.pos.x + half, std::fmin(label_mid.x, input.pos.x + result.width - half));
            const float raw_y = (result.sequence || (result.horizontal && !edge.reversed && edge.from != edge.to))
                                    ? (label_mid.y - plate_h - 4.0f * scale)
                                    : (label_mid.y - plate_h * 0.5f);
            const float y = std::fmax(input.pos.y, std::fmin(raw_y, input.pos.y + std::fmax(0.0f, result.height - plate_h)));
            auto plate = etch_rect_draw_desc_default();
            plate.rect = { x - half, y, plate_w, plate_h };
            plate.fill = theme.bg_elevated;
            plate.border_color = theme.border_subtle;
            plate.border_width = std::fmax(0.65f, 0.85f * scale);
            plate.radius = etch_corners_all(theme.radius_small_px * scale);
            plate.continuous_corners = true;
            etch_draw_rect(&plate);
            draw_label(edge.label, { x, y + pad_y }, body_font, caption_font, theme.text_secondary, ETCH_ALIGN_X_CENTER);
        }
    }

    // 4. Nodes (shape-aware elevated surface rendering).
    const EtchColor node_top = dark ? shift_color(theme.bg_elevated, 4) : theme.bg_elevated;
    const EtchColor node_bot = dark ? shift_color(theme.bg_elevated, -2) : shift_color(theme.bg_elevated, -4);
    const EtchColor sh_ambient = dark ? etch_rgba8(0, 0, 0, 34) : etch_rgba8(15, 23, 42, 13);
    const EtchColor sh_contact = dark ? etch_rgba8(0, 0, 0, 44) : etch_rgba8(15, 23, 42, 17);
    for (uint32_t i = 0; i < result.node_count; ++i) {
        const auto& n = result.nodes[i];
        const InlayDiagramShape shape = (meta && meta->shapes) ? meta->shapes[i] : InlayShapeRect;
        const auto tsz = measure_label(n.label, body_font, eff_font);
        if (shape == InlayShapeStateStart || shape == InlayShapeStateEnd) {
            const float r = n.bounds.w * 0.5f;
            auto outer = etch_rect_draw_desc_default();
            outer.rect = n.bounds;
            outer.radius = etch_corners_all(r);
            if (shape == InlayShapeStateStart) {
                outer.fill = theme.text_primary;
                etch_draw_rect(&outer);
            } else {
                outer.fill = theme.bg_elevated;
                outer.border_color = theme.text_primary;
                outer.border_width = std::fmax(1.5f, 2.0f * scale);
                etch_draw_rect(&outer);
                const float inset = std::fmax(3.5f, n.bounds.w * 0.24f);
                auto inner = etch_rect_draw_desc_default();
                inner.rect = { n.bounds.x + inset, n.bounds.y + inset,
                               std::fmax(2.0f, n.bounds.w - 2.0f * inset),
                               std::fmax(2.0f, n.bounds.h - 2.0f * inset) };
                inner.fill = theme.text_primary;
                inner.radius = etch_corners_all(inner.rect.w * 0.5f);
                etch_draw_rect(&inner);
            }
        } else if (shape == InlayShapeDiamond) {
            const float cx = n.bounds.x + n.bounds.w * 0.5f;
            const float cy = n.bounds.y + n.bounds.h * 0.5f;
            const float sy_off = 2.0f * scale;
            EtchVec2 sh_pts[4] = {
                { cx, n.bounds.y + sy_off },
                { n.bounds.x + n.bounds.w + 0.5f * scale, cy + sy_off },
                { cx, n.bounds.y + n.bounds.h + sy_off + 0.5f * scale },
                { n.bounds.x - 0.5f * scale, cy + sy_off },
            };
            EtchFilledPolygonDesc sh_poly = {};
            sh_poly.points = sh_pts;
            sh_poly.point_count = 4;
            sh_poly.fill = sh_contact;
            etch_draw_filled_polygon(&sh_poly);
            EtchVec2 pts[4] = {
                { cx, n.bounds.y },
                { n.bounds.x + n.bounds.w, cy },
                { cx, n.bounds.y + n.bounds.h },
                { n.bounds.x, cy },
            };
            EtchFilledPolygonDesc poly = {};
            poly.points = pts;
            poly.point_count = 4;
            poly.fill = node_top;
            poly.fill_end = node_bot;
            poly.gradient_dir = { 0.0f, 1.0f };
            etch_draw_filled_polygon(&poly);
            EtchLineDesc edge_l = {};
            edge_l.color = theme.border_node;
            edge_l.thickness = std::fmax(0.9f, 1.15f * scale);
            for (int k = 0; k < 4; ++k) {
                edge_l.from = pts[k];
                edge_l.to = pts[(k + 1) & 3];
                etch_draw_line(&edge_l);
            }
            draw_label(n.label, { cx, cy - tsz.y * 0.5f }, body_font, eff_font, theme.text_primary, ETCH_ALIGN_X_CENTER);
        } else {
            const bool is_circle = (shape == InlayShapeCircle);
            const bool is_round = (shape == InlayShapeRound);
            const bool cont = !is_circle && !is_round;
            const float base_r = is_circle ? (n.bounds.w * 0.5f)
                : (is_round ? (n.bounds.h * 0.5f) : (theme.radius_control_px * scale));

            // Dual-layer subtle elevation shadow backdrop (ambient + tight contact)
            const float amb_pad = 1.5f * scale;
            auto sh1 = etch_rect_draw_desc_default();
            sh1.rect = { n.bounds.x - amb_pad, n.bounds.y + 2.0f * scale - amb_pad,
                         n.bounds.w + 2.0f * amb_pad, n.bounds.h + 2.0f * amb_pad };
            sh1.fill = sh_ambient;
            sh1.radius = etch_corners_all(base_r + amb_pad);
            sh1.softness = std::fmax(1.5f, 3.5f * scale);
            sh1.continuous_corners = cont;
            etch_draw_rect(&sh1);

            const float con_pad = 0.5f * scale;
            auto sh2 = etch_rect_draw_desc_default();
            sh2.rect = { n.bounds.x - con_pad, n.bounds.y + 1.0f * scale - con_pad,
                         n.bounds.w + 2.0f * con_pad, n.bounds.h + 2.0f * con_pad };
            sh2.fill = sh_contact;
            sh2.radius = etch_corners_all(base_r + con_pad);
            sh2.softness = std::fmax(1.0f, 1.8f * scale);
            sh2.continuous_corners = cont;
            etch_draw_rect(&sh2);

            auto rect = etch_rect_draw_desc_default();
            rect.rect = n.bounds;
            rect.fill = node_top;
            rect.fill_end = node_bot;
            rect.gradient_dir = { 0.0f, 1.0f };
            rect.border_color = theme.border_node;
            rect.border_width = border_w;
            rect.radius = etch_corners_all(base_r);
            rect.continuous_corners = cont;
            etch_draw_rect(&rect);
            if (shape == InlayShapeClassBox) {
                uint32_t nl = UINT32_MAX;
                for (uint32_t ci = 0u; ci < n.label.length; ++ci) {
                    if (n.label.chars[ci] == '\n') { nl = ci; break; }
                }
                const float line_h = std::fmax(etch_font_metrics(body_font, eff_font).line_height, eff_font * 1.25f);
                if (nl != UINT32_MAX) {
                    const EtchStr hdr = { n.label.chars, nl, false };
                    const EtchStr members = { n.label.chars + nl + 1u, n.label.length - (nl + 1u), false };
                    const float hdr_h = line_h + 14.0f * scale;
                    auto hband = etch_rect_draw_desc_default();
                    hband.rect = { n.bounds.x + border_w, n.bounds.y + border_w,
                                   std::fmax(0.0f, n.bounds.w - 2.0f * border_w), std::fmax(0.0f, hdr_h - border_w) };
                    hband.fill = { theme.accent.r, theme.accent.g, theme.accent.b, uint8_t(dark ? 44u : 28u) };
                    hband.radius = { base_r, base_r, 0.0f, 0.0f };
                    etch_draw_rect(&hband);
                    EtchLineDesc sep = {};
                    sep.from = { n.bounds.x, n.bounds.y + hdr_h };
                    sep.to = { n.bounds.x + n.bounds.w, n.bounds.y + hdr_h };
                    sep.color = theme.border_node;
                    sep.thickness = border_w;
                    etch_draw_line(&sep);
                    draw_label(hdr, { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + 7.0f * scale },
                               bold_font, eff_font, theme.text_primary, ETCH_ALIGN_X_CENTER);
                    draw_label(members, { n.bounds.x + 12.0f * scale, n.bounds.y + hdr_h + 6.0f * scale },
                               body_font, eff_font * 0.92f, theme.text_secondary, ETCH_ALIGN_X_LEFT);
                } else {
                    draw_label(n.label, { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + (n.bounds.h - tsz.y) * 0.5f },
                               bold_font, eff_font, theme.text_primary, ETCH_ALIGN_X_CENTER);
                }
            } else {
                draw_label(n.label, { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + (n.bounds.h - tsz.y) * 0.5f },
                           body_font, eff_font, theme.text_primary, ETCH_ALIGN_X_CENTER);
            }
        }
    }
    if (input.canvas_id) etch_end_canvas();
}

void inlay_diagram_draw(const InlayDiagramResult& result, const InlayDiagramDesc& input) {
    inlay_diagram_draw_ex(result, input, nullptr);
}

extern "C" {
INLAY_API InlayDiagramResult inlay_diagram_layout(EtchStr source, const InlayDiagramDesc* desc_in) {
    InlayDiagramDesc desc = inlay_diagram_desc_default();
    if (!inlay_read_abi_struct(desc_in, &desc)) return {};
    return inlay_diagram_layout_ex(source, &desc, nullptr);
}
INLAY_API InlayDiagramResult inlay_diagram_view(EtchStr source, const InlayDiagramDesc* input_in) {
    InlayDiagramDesc input = inlay_diagram_desc_default();
    if (!inlay_read_abi_struct(input_in, &input)) return {};
    InlayDiagramMeta meta = {};
    auto result = inlay_diagram_layout_ex(source, &input, &meta);
    if (!result.valid) return result;
    inlay_diagram_draw_ex(result, input, &meta);
    return result;
}

INLAY_API uint32_t inlay_diagram_export_svg(EtchStr source, const InlayDiagramDesc* desc_in, char* out_buf, uint32_t cap) {
    InlayDiagramDesc desc = inlay_diagram_desc_default();
    if (desc_in && !inlay_read_abi_struct(desc_in, &desc)) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }
    desc.canvas_id = 0u;
    auto* ctx = etch_current_context();
    const bool in_frame = ctx && etch_frame_in_progress(ctx);
    DiagramHeapArena heap = {};
    const DiagramEnv env = { ctx, &heap, in_frame };
    InlayDiagramMeta meta = {};
    const InlayDiagramResult result = diagram_layout_core(source, &desc, &meta, env);
    if (!result.valid) {
        heap.destroy();
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }

    const float scale = (result.scale > 0.0f && std::isfinite(result.scale)) ? std::fmin(1.0f, result.scale) : 1.0f;
    const float eff_font = std::fmax(7.0f, desc.font_size_px * scale);
    const float caption_font = std::fmax(7.0f, std::fmax(10.0f * scale, eff_font * 0.82f));
    const float eff_rank_gap = desc.rank_gap_px * scale;
    const uint16_t body_font = resolve_body_font(desc);
    const uint16_t bold_font = resolve_bold_font(desc);
    const float stroke_normal = std::fmax(1.5f, 1.85f * scale);
    const float stroke_thick = std::fmax(2.25f, 2.85f * scale);
    const float border_w = std::fmax(0.85f, 1.0f * scale);

    const float pad = 12.0f;
    const float ox = pad - desc.pos.x;
    const float oy = pad - desc.pos.y;
    const float svg_w = std::fmax(1.0f, result.width + 2.0f * pad);
    const float svg_h = std::fmax(1.0f, result.height + 2.0f * pad);

    DiagramSvgOut out = { out_buf, cap, 0u };
    out.write_lit("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ");
    out.write_num(svg_w); out.write_lit(" "); out.write_num(svg_h);
    out.write_lit("\" width=\""); out.write_num(svg_w);
    out.write_lit("\" height=\""); out.write_num(svg_h);
    out.write_lit("\">\n<defs>\n"
                  "<marker id=\"arrow\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"7\" markerHeight=\"7\" orient=\"auto-start-reverse\">"
                  "<path d=\"M 0 1 L 10 5 L 0 9 z\" fill=\"#4f6ef7\"/></marker>\n"
                  "<marker id=\"arrow-rev\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"7\" markerHeight=\"7\" orient=\"auto-start-reverse\">"
                  "<path d=\"M 0 1 L 10 5 L 0 9 z\" fill=\"#8b5cf6\"/></marker>\n"
                  "</defs>\n");

    // Specialized SVG output for Pie and Gantt diagrams
    if (meta.diagram_kind == InlayDiagramPie && meta.pie_slices && meta.pie_slice_count > 0u) {
        static constexpr const char* kPieHex[8] = {
            "#3b82f6", "#10b981", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4", "#f97316", "#ec4899"
        };
        if (meta.title.length > 0u) {
            svg_emit_diagram_label(&out, meta.title, desc.pos.x + result.width * 0.5f + ox, desc.pos.y + 8.0f * scale + oy,
                                   env.line_height(bold_font, eff_font * 1.08f), eff_font * 1.08f, "#0f172a", "600", "middle");
        }
        const float cx = meta.pie_center.x + ox, cy = meta.pie_center.y + oy, r = meta.pie_radius;
        for (uint32_t i = 0u; i < meta.pie_slice_count; ++i) {
            const auto& sl = meta.pie_slices[i];
            const char* hex = kPieHex[sl.color_index % 8u];
            if (sl.sweep_rad >= 6.27f) {
                out.write_lit("<circle cx=\""); out.write_num(cx); out.write_lit("\" cy=\""); out.write_num(cy);
                out.write_lit("\" r=\""); out.write_num(r); out.write_lit("\" fill=\""); out.write_lit(hex); out.write_lit("\"/>\n");
            } else {
                const float x0 = cx + std::cos(sl.start_rad) * r;
                const float y0 = cy + std::sin(sl.start_rad) * r;
                const float x1 = cx + std::cos(sl.start_rad + sl.sweep_rad) * r;
                const float y1 = cy + std::sin(sl.start_rad + sl.sweep_rad) * r;
                const int large_arc = (sl.sweep_rad > 3.14159265f) ? 1 : 0;
                out.write_lit("<path d=\"M "); out.write_num(cx); out.write_lit(" "); out.write_num(cy);
                out.write_lit(" L "); out.write_num(x0); out.write_lit(" "); out.write_num(y0);
                out.write_lit(" A "); out.write_num(r); out.write_lit(" "); out.write_num(r);
                out.write_lit(large_arc ? " 0 1 1 " : " 0 0 1 ");
                out.write_num(x1); out.write_lit(" "); out.write_num(y1);
                out.write_lit(" Z\" fill=\""); out.write_lit(hex); out.write_lit("\" stroke=\"#ffffff\" stroke-width=\"1.5\"/>\n");
            }
            if (i < result.node_count) {
                const auto& nr = result.nodes[i];
                const float sw = 10.0f * scale;
                out.write_lit("<rect x=\""); out.write_num(nr.bounds.x + ox);
                out.write_lit("\" y=\""); out.write_num(nr.bounds.y + (nr.bounds.h - sw) * 0.5f + oy);
                out.write_lit("\" width=\""); out.write_num(sw); out.write_lit("\" height=\""); out.write_num(sw);
                out.write_lit("\" rx=\"2.5\" fill=\""); out.write_lit(hex); out.write_lit("\"/>\n");
                svg_emit_diagram_label(&out, nr.label, nr.bounds.x + sw + 7.0f * scale + ox, nr.bounds.y + oy,
                                       env.line_height(body_font, eff_font * 0.9f), eff_font * 0.9f, "#0f172a", nullptr, "start");
            }
        }
        out.write_lit("</svg>\n");
        heap.destroy();
        if (out_buf && cap > 0u) out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = '\0';
        return out.len;
    }
    if (meta.diagram_kind == InlayDiagramGantt && meta.gantt_bars && meta.gantt_bar_count > 0u) {
        if (meta.title.length > 0u) {
            svg_emit_diagram_label(&out, meta.title, desc.pos.x + result.width * 0.5f + ox, desc.pos.y + 8.0f * scale + oy,
                                   env.line_height(bold_font, eff_font * 1.08f), eff_font * 1.08f, "#0f172a", "600", "middle");
        }
        const float sec_h = eff_font * 1.45f + 6.0f * scale;
        for (uint32_t i = 0u; i < meta.gantt_bar_count && i < result.node_count; ++i) {
            const auto& nr = result.nodes[i];
            const auto& bar = meta.gantt_bars[i];
            if (bar.section_first && bar.section.length > 0u) {
                out.write_lit("<rect x=\""); out.write_num(nr.bounds.x + ox);
                out.write_lit("\" y=\""); out.write_num(nr.bounds.y - sec_h + 2.0f * scale + oy);
                out.write_lit("\" width=\""); out.write_num(nr.bounds.w);
                out.write_lit("\" height=\""); out.write_num(sec_h - 4.0f * scale);
                out.write_lit("\" rx=\"4\" fill=\"#f1f5f9\"/>\n");
                svg_emit_diagram_label(&out, bar.section, nr.bounds.x + 8.0f * scale + ox, nr.bounds.y - sec_h + 4.0f * scale + oy,
                                       env.line_height(bold_font, eff_font * 0.88f), eff_font * 0.88f, "#475569", "600", "start");
            }
            svg_emit_diagram_label(&out, nr.label, nr.bounds.x + 6.0f * scale + ox, nr.bounds.y + 2.0f * scale + oy,
                                   env.line_height(body_font, eff_font * 0.92f), eff_font * 0.92f, "#0f172a", nullptr, "start");
            const char* bhex = (bar.status == 1u) ? "#64748b" : (bar.status == 2u ? "#3b82f6" : (bar.status == 3u ? "#ef4444" : "#10b981"));
            out.write_lit("<rect x=\""); out.write_num(bar.bar_bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(bar.bar_bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(bar.bar_bounds.w);
            out.write_lit("\" height=\""); out.write_num(bar.bar_bounds.h);
            out.write_lit("\" rx=\"4\" fill=\""); out.write_lit(bhex); out.write_lit("\"/>\n");
        }
        out.write_lit("</svg>\n");
        heap.destroy();
        if (out_buf && cap > 0u) out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = '\0';
        return out.len;
    }
    if (meta.diagram_kind == InlayDiagramGitGraph && result.node_count > 0u) {
        static constexpr const char* kLaneHex[8] = {
            "#3b82f6", "#10b981", "#f59e0b", "#8b5cf6", "#ef4444", "#06b6d4", "#ec4899", "#f97316"
        };
        for (uint32_t bi = 0u; bi < meta.subgraph_count && meta.subgraphs; ++bi) {
            const auto& sg = meta.subgraphs[bi];
            const char* hex = kLaneHex[bi % 8u];
            const float cy = sg.bounds.y + sg.bounds.h * 0.5f + oy;
            out.write_lit("<line x1=\""); out.write_num(sg.bounds.x + ox);
            out.write_lit("\" y1=\""); out.write_num(cy);
            out.write_lit("\" x2=\""); out.write_num(sg.bounds.x + sg.bounds.w + ox);
            out.write_lit("\" y2=\""); out.write_num(cy);
            out.write_lit("\" stroke=\""); out.write_lit(hex);
            out.write_lit("\" stroke-opacity=\"0.35\" stroke-width=\"1.5\"/>\n");
            svg_emit_diagram_label(&out, sg.title, sg.bounds.x + 6.0f * scale + ox, cy - eff_font * 0.5f,
                                   env.line_height(bold_font, eff_font * 0.82f), eff_font * 0.82f, hex, "600", "start");
        }
        for (uint32_t ei = 0u; ei < result.edge_count; ++ei) {
            const auto& ed = result.edges[ei];
            const auto& a = result.nodes[ed.from];
            const auto& b = result.nodes[ed.to];
            EtchBezierDesc bz = {};
            bz.p0 = { a.bounds.x + a.bounds.w * 0.5f, a.bounds.y + a.bounds.h * 0.5f };
            bz.p3 = { b.bounds.x + b.bounds.w * 0.5f, b.bounds.y + b.bounds.h * 0.5f };
            bz.p1 = { (bz.p0.x + bz.p3.x) * 0.5f, bz.p0.y };
            bz.p2 = { (bz.p0.x + bz.p3.x) * 0.5f, bz.p3.y };
            svg_emit_bezier_polyline(&out, bz, ox, oy, kLaneHex[b.rank % 8u], std::fmax(1.8f, 2.2f * scale), ed.dashed, nullptr, nullptr);
        }
        for (uint32_t ci = 0u; ci < result.node_count; ++ci) {
            const auto& n = result.nodes[ci];
            const float cx = n.bounds.x + n.bounds.w * 0.5f + ox;
            const float cy = n.bounds.y + n.bounds.h * 0.5f + oy;
            out.write_lit("<circle cx=\""); out.write_num(cx); out.write_lit("\" cy=\""); out.write_num(cy);
            out.write_lit("\" r=\""); out.write_num(n.bounds.w * 0.5f);
            out.write_lit("\" fill=\""); out.write_lit(kLaneHex[n.rank % 8u]);
            out.write_lit("\" stroke=\"#ffffff\" stroke-width=\"2\"/>\n");
            svg_emit_diagram_label(&out, n.label, cx, n.bounds.y + n.bounds.h + 4.0f * scale + oy,
                                   env.line_height(body_font, caption_font), caption_font, "#334155", nullptr, "middle");
        }
        out.write_lit("</svg>\n");
        heap.destroy();
        if (out_buf && cap > 0u) out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = '\0';
        return out.len;
    }
    if (meta.diagram_kind == InlayDiagramMindmap && result.node_count > 0u) {
        static constexpr const char* kDepthHex[6] = {
            "#3b82f6", "#10b981", "#8b5cf6", "#f59e0b", "#06b6d4", "#ec4899"
        };
        for (uint32_t ei = 0u; ei < result.edge_count; ++ei) {
            const auto& ed = result.edges[ei];
            const auto& a = result.nodes[ed.from];
            const auto& b = result.nodes[ed.to];
            EtchBezierDesc bz = {};
            bz.p0 = { a.bounds.x + a.bounds.w, a.bounds.y + a.bounds.h * 0.5f };
            bz.p3 = { b.bounds.x, b.bounds.y + b.bounds.h * 0.5f };
            const float mx = (bz.p0.x + bz.p3.x) * 0.5f;
            bz.p1 = { mx, bz.p0.y }; bz.p2 = { mx, bz.p3.y };
            svg_emit_bezier_polyline(&out, bz, ox, oy, kDepthHex[b.rank % 6u], std::fmax(1.4f, 1.8f * scale), false, nullptr, nullptr);
        }
        for (uint32_t ni = 0u; ni < result.node_count; ++ni) {
            const auto& n = result.nodes[ni];
            const char* hex = kDepthHex[n.rank % 6u];
            const bool is_root = (n.rank == 0u);
            const float rad = is_root ? (n.bounds.h * 0.5f) : (8.0f * scale);
            out.write_lit("<rect x=\""); out.write_num(n.bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(n.bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(n.bounds.w);
            out.write_lit("\" height=\""); out.write_num(n.bounds.h);
            out.write_lit("\" rx=\""); out.write_num(rad);
            out.write_lit("\" fill=\""); out.write_lit(is_root ? hex : "#f8fafc");
            out.write_lit("\" stroke=\""); out.write_lit(hex); out.write_lit("\" stroke-width=\"1.4\"/>\n");
            const float fsz = is_root ? (eff_font * 1.05f) : (eff_font * 0.92f);
            const float lh = env.line_height(is_root ? bold_font : body_font, fsz);
            svg_emit_diagram_label(&out, n.label, n.bounds.x + n.bounds.w * 0.5f + ox,
                                   n.bounds.y + (n.bounds.h - lh) * 0.5f + oy, lh, fsz,
                                   is_root ? "#ffffff" : "#0f172a", is_root ? "600" : nullptr, "middle");
        }
        out.write_lit("</svg>\n");
        heap.destroy();
        if (out_buf && cap > 0u) out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = '\0';
        return out.len;
    }
    if (meta.diagram_kind == InlayDiagramTimeline && result.node_count > 0u) {
        static constexpr const char* kColHex[6] = {
            "#3b82f6", "#10b981", "#8b5cf6", "#f59e0b", "#06b6d4", "#ec4899"
        };
        if (meta.title.length > 0u) {
            svg_emit_diagram_label(&out, meta.title, desc.pos.x + result.width * 0.5f + ox, desc.pos.y + 8.0f * scale + oy,
                                   env.line_height(bold_font, eff_font * 1.08f), eff_font * 1.08f, "#0f172a", "600", "middle");
        }
        for (uint32_t si = 0u; si < meta.subgraph_count && meta.subgraphs; ++si) {
            const auto& sg = meta.subgraphs[si];
            if (!sg.has_nodes) continue;
            out.write_lit("<rect x=\""); out.write_num(sg.bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(sg.bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(sg.bounds.w);
            out.write_lit("\" height=\""); out.write_num(sg.bounds.h);
            out.write_lit("\" rx=\"5\" fill=\"#f1f5f9\"/>\n");
            svg_emit_diagram_label(&out, sg.title, sg.bounds.x + sg.bounds.w * 0.5f + ox, sg.bounds.y + 3.0f * scale + oy,
                                   env.line_height(bold_font, eff_font * 0.86f), eff_font * 0.86f, "#475569", "600", "middle");
        }
        for (uint32_t i = 0u; i < result.node_count; ++i) {
            const auto& n = result.nodes[i];
            const char* hex = kColHex[n.rank % 6u];
            const bool is_hdr = meta.shapes && meta.shapes[i] == InlayShapeRound;
            const float rad = is_hdr ? (n.bounds.h * 0.5f) : (6.0f * scale);
            out.write_lit("<rect x=\""); out.write_num(n.bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(n.bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(n.bounds.w);
            out.write_lit("\" height=\""); out.write_num(n.bounds.h);
            out.write_lit("\" rx=\""); out.write_num(rad);
            out.write_lit("\" fill=\""); out.write_lit(is_hdr ? hex : "#ffffff");
            out.write_lit("\" stroke=\""); out.write_lit(hex); out.write_lit("\" stroke-width=\"1.2\"/>\n");
            const float fsz = eff_font * 0.9f;
            const float lh = env.line_height(is_hdr ? bold_font : body_font, fsz);
            svg_emit_diagram_label(&out, n.label, n.bounds.x + n.bounds.w * 0.5f + ox,
                                   n.bounds.y + (n.bounds.h - lh) * 0.5f + oy, lh, fsz,
                                   is_hdr ? "#ffffff" : "#0f172a", is_hdr ? "600" : nullptr, "middle");
        }
        out.write_lit("</svg>\n");
        heap.destroy();
        if (out_buf && cap > 0u) out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = '\0';
        return out.len;
    }

    // 1. Subgraphs
    if (meta.subgraphs && meta.subgraph_count > 0u) {
        const float title_font = std::fmax(7.5f, std::fmax(11.0f * scale, eff_font * 0.85f));
        for (uint32_t si = 0u; si < meta.subgraph_count; ++si) {
            const auto& sg = meta.subgraphs[si];
            if (!sg.has_nodes) continue;
            out.write_lit("<rect x=\""); out.write_num(sg.bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(sg.bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(sg.bounds.w);
            out.write_lit("\" height=\""); out.write_num(sg.bounds.h);
            out.write_lit("\" rx=\"8\" ry=\"8\" fill=\"#f1f5f9\" stroke=\"#cbd5e1\" stroke-width=\"");
            out.write_num(border_w); out.write_lit("\"/>\n");
            if (sg.title.length) {
                const float line_h = env.line_height(bold_font, title_font);
                svg_emit_diagram_label(&out, sg.title, sg.bounds.x + ox + 12.0f * scale,
                                       sg.bounds.y + oy + 6.0f * scale, line_h, title_font, "#475569", "600", "start");
            }
        }
    }

    // 2. Sequence lifelines
    if (result.sequence) {
        for (uint32_t i = 0u; i < result.node_count; ++i) {
            const auto b = result.nodes[i].bounds;
            const float lx = b.x + b.w * 0.5f + ox;
            out.write_lit("<line x1=\""); out.write_num(lx);
            out.write_lit("\" y1=\""); out.write_num(b.y + b.h + oy);
            out.write_lit("\" x2=\""); out.write_num(lx);
            out.write_lit("\" y2=\""); out.write_num(desc.pos.y + result.height + oy);
            out.write_lit("\" stroke=\"#cbd5e1\" stroke-width=\""); out.write_num(border_w);
            out.write_lit("\" stroke-dasharray=\"4,4\"/>\n");
        }
    }

    // 3. Edges
    for (uint32_t i = 0u; i < result.edge_count; ++i) {
        const auto& edge = result.edges[i];
        const auto a = result.nodes[edge.from].bounds, b = result.nodes[edge.to].bounds;
        const char* stroke_col = edge.reversed ? "#8b5cf6" : "#4f6ef7";
        const char* marker_id = edge.reversed ? "arrow-rev" : "arrow";
        const InlayDiagramEdgePath path = meta.edge_paths ? meta.edge_paths[i] : InlayDiagramEdgePath{ 0, 0, edge.reversed ? 0 : -1 };
        const uint8_t estyle = meta.edge_styles ? meta.edge_styles[i] : uint8_t(InlayEdgeStyleNone);
        const bool no_arrow = (estyle & InlayEdgeStyleNoArrow) != 0;
        const bool bidir = (estyle & InlayEdgeStyleBidir) != 0;
        const float thickness = (estyle & InlayEdgeStyleThick) ? stroke_thick : stroke_normal;
        const bool rev_dir = meta.reversed_dir ||
            (!result.sequence && !edge.reversed && edge.from != edge.to &&
             (result.horizontal ? (a.x > b.x + b.w * 0.5f) : (a.y > b.y + b.h * 0.5f)));

        EtchVec2 label_mid = {};
        if (!result.sequence && !edge.reversed && edge.from != edge.to && path.point_count > 0u && meta.waypoints) {
            const uint32_t segs = path.point_count + 1u;
            const EtchVec2 start_pt = result.horizontal
                ? (rev_dir ? EtchVec2{ a.x, a.y + a.h * 0.5f } : EtchVec2{ a.x + a.w, a.y + a.h * 0.5f })
                : (rev_dir ? EtchVec2{ a.x + a.w * 0.5f, a.y } : EtchVec2{ a.x + a.w * 0.5f, a.y + a.h });
            const EtchVec2 end_pt = result.horizontal
                ? (rev_dir ? EtchVec2{ b.x + b.w, b.y + b.h * 0.5f } : EtchVec2{ b.x, b.y + b.h * 0.5f })
                : (rev_dir ? EtchVec2{ b.x + b.w * 0.5f, b.y + b.h } : EtchVec2{ b.x + b.w * 0.5f, b.y });
            out.write_lit("<polyline fill=\"none\" stroke=\""); out.write_lit(stroke_col);
            out.write_lit("\" stroke-width=\""); out.write_num(thickness);
            if (edge.dashed) out.write_lit("\" stroke-dasharray=\"6,4");
            if (bidir) { out.write_lit("\" marker-start=\"url(#"); out.write_lit(marker_id); out.write_lit(")"); }
            if (!no_arrow) { out.write_lit("\" marker-end=\"url(#"); out.write_lit(marker_id); out.write_lit(")"); }
            out.write_lit("\" points=\"");
            for (uint32_t s = 0u; s < segs; ++s) {
                const EtchVec2 p0 = (s == 0u) ? start_pt : meta.waypoints[path.point_offset + s - 1u];
                const EtchVec2 p3 = (s + 1u == segs) ? end_pt : meta.waypoints[path.point_offset + s];
                EtchBezierDesc bz = {};
                bz.p0 = p0; bz.p3 = p3;
                bz.p1 = result.horizontal ? EtchVec2{ (p0.x + p3.x) * 0.5f, p0.y } : EtchVec2{ p0.x, (p0.y + p3.y) * 0.5f };
                bz.p2 = result.horizontal ? EtchVec2{ bz.p1.x, p3.y } : EtchVec2{ p3.x, bz.p1.y };
                if (s == segs / 2u) label_mid = curve_point(bz, 0.5f);
                constexpr uint32_t kSub = 6u;
                for (uint32_t k = (s == 0u ? 0u : 1u); k <= kSub; ++k) {
                    const EtchVec2 pt = curve_point(bz, float(k) / float(kSub));
                    if (s > 0u || k > 0u) out.write_lit(" ");
                    out.write_num(pt.x + ox); out.write_lit(","); out.write_num(pt.y + oy);
                }
            }
            out.write_lit("\"/>\n");
        } else if (result.sequence && edge.from != edge.to) {
            const float y = desc.pos.y + a.h + float(i + 1u) * eff_rank_gap;
            const float x0 = a.x + a.w * 0.5f;
            const float x1 = b.x + b.w * 0.5f;
            out.write_lit("<line x1=\""); out.write_num(x0 + ox);
            out.write_lit("\" y1=\""); out.write_num(y + oy);
            out.write_lit("\" x2=\""); out.write_num(x1 + ox);
            out.write_lit("\" y2=\""); out.write_num(y + oy);
            out.write_lit("\" stroke=\""); out.write_lit(stroke_col);
            out.write_lit("\" stroke-width=\""); out.write_num(thickness);
            if (edge.dashed) out.write_lit("\" stroke-dasharray=\"6,4");
            if (!no_arrow) { out.write_lit("\" marker-end=\"url(#"); out.write_lit(marker_id); out.write_lit(")"); }
            out.write_lit("\"/>\n");
            label_mid = { (x0 + x1) * 0.5f, y };
        } else {
            EtchBezierDesc bz = {};
            if (result.horizontal) {
                bz.p0 = rev_dir ? EtchVec2{ a.x, a.y + a.h * 0.5f } : EtchVec2{ a.x + a.w, a.y + a.h * 0.5f };
                bz.p3 = rev_dir ? EtchVec2{ b.x + b.w, b.y + b.h * 0.5f } : EtchVec2{ b.x, b.y + b.h * 0.5f };
            } else {
                bz.p0 = rev_dir ? EtchVec2{ a.x + a.w * 0.5f, a.y } : EtchVec2{ a.x + a.w * 0.5f, a.y + a.h };
                bz.p3 = rev_dir ? EtchVec2{ b.x + b.w * 0.5f, b.y + b.h } : EtchVec2{ b.x + b.w * 0.5f, b.y };
            }
            bz.p1 = result.horizontal ? EtchVec2{ (bz.p0.x + bz.p3.x) * 0.5f, bz.p0.y } : EtchVec2{ bz.p0.x, (bz.p0.y + bz.p3.y) * 0.5f };
            bz.p2 = result.horizontal ? EtchVec2{ bz.p1.x, bz.p3.y } : EtchVec2{ bz.p3.x, bz.p1.y };
            if (edge.reversed) {
                const float lane_off = float(path.feedback_lane >= 0 ? path.feedback_lane : 0) * 18.0f * scale;
                if (result.horizontal) {
                    const float route = desc.pos.y + result.height - 8.0f * scale - lane_off;
                    bz.p0 = { a.x + a.w * 0.5f, a.y + a.h };
                    bz.p3 = { b.x + b.w * 0.5f, b.y + b.h };
                    bz.p1 = { bz.p0.x, route };
                    bz.p2 = { bz.p3.x, route };
                } else {
                    const float route = desc.pos.x + result.width - 8.0f * scale - lane_off;
                    bz.p0 = { a.x + a.w, a.y + a.h * 0.5f };
                    bz.p3 = { b.x + b.w, b.y + b.h * 0.5f };
                    bz.p1 = { route, bz.p0.y };
                    bz.p2 = { route, bz.p3.y };
                }
            }
            if (edge.from == edge.to) {
                const float loop_y = result.sequence ? (desc.pos.y + a.h + float(i + 1u) * eff_rank_gap) : (a.y + a.h * 0.5f);
                const float loop_x = result.sequence ? (a.x + a.w * 0.5f) : (a.x + a.w);
                const float loop_r = 40.0f * scale;
                const float loop_dy = 12.0f * scale;
                bz.p0 = { loop_x, loop_y - loop_dy };
                bz.p3 = { loop_x, loop_y + loop_dy };
                bz.p1 = { loop_x + loop_r, bz.p0.y };
                bz.p2 = { loop_x + loop_r, bz.p3.y };
            }
            svg_emit_bezier_polyline(&out, bz, ox, oy, stroke_col, thickness, edge.dashed,
                                     bidir ? marker_id : nullptr, no_arrow ? nullptr : marker_id);
            label_mid = curve_point(bz, 0.5f);
        }

        if (edge.label.length) {
            float l_lh = 0.0f;
            const EtchVec2 sz = measure_label_env(edge.label, body_font, caption_font, env, &l_lh);
            const float pad_x = 7.0f * scale;
            const float pad_y = 2.5f * scale;
            const float plate_w = sz.x + 2.0f * pad_x;
            const float plate_h = sz.y + 2.0f * pad_y;
            const float half = plate_w * 0.5f;
            const float lx = std::fmax(desc.pos.x + half, std::fmin(label_mid.x, desc.pos.x + result.width - half));
            const float raw_y = (result.sequence || (result.horizontal && !edge.reversed && edge.from != edge.to))
                                    ? (label_mid.y - plate_h - 4.0f * scale)
                                    : (label_mid.y - plate_h * 0.5f);
            const float ly = std::fmax(desc.pos.y, std::fmin(raw_y, desc.pos.y + std::fmax(0.0f, result.height - plate_h)));
            out.write_lit("<rect x=\""); out.write_num(lx - half + ox);
            out.write_lit("\" y=\""); out.write_num(ly + oy);
            out.write_lit("\" width=\""); out.write_num(plate_w);
            out.write_lit("\" height=\""); out.write_num(plate_h);
            out.write_lit("\" rx=\"4\" ry=\"4\" fill=\"#ffffff\" stroke=\"#e2e8f0\" stroke-width=\"0.8\"/>\n");
            svg_emit_diagram_label(&out, edge.label, lx + ox, ly + pad_y + oy, l_lh, caption_font, "#475569");
        }
    }

    // 4. Nodes
    for (uint32_t i = 0u; i < result.node_count; ++i) {
        const auto& n = result.nodes[i];
        const InlayDiagramShape shape = meta.shapes ? meta.shapes[i] : InlayShapeRect;
        float n_lh = 0.0f;
        const EtchVec2 tsz = measure_label_env(n.label, body_font, eff_font, env, &n_lh);
        const float cx = n.bounds.x + n.bounds.w * 0.5f + ox;
        const float cy = n.bounds.y + n.bounds.h * 0.5f + oy;
        if (shape == InlayShapeStateStart || shape == InlayShapeStateEnd) {
            const float r = n.bounds.w * 0.5f;
            if (shape == InlayShapeStateStart) {
                out.write_lit("<circle cx=\""); out.write_num(cx);
                out.write_lit("\" cy=\""); out.write_num(cy);
                out.write_lit("\" r=\""); out.write_num(r);
                out.write_lit("\" fill=\"#0f172a\"/>\n");
            } else {
                out.write_lit("<circle cx=\""); out.write_num(cx);
                out.write_lit("\" cy=\""); out.write_num(cy);
                out.write_lit("\" r=\""); out.write_num(r);
                out.write_lit("\" fill=\"#ffffff\" stroke=\"#0f172a\" stroke-width=\"2\"/>\n");
                out.write_lit("<circle cx=\""); out.write_num(cx);
                out.write_lit("\" cy=\""); out.write_num(cy);
                out.write_lit("\" r=\""); out.write_num(r * 0.52f);
                out.write_lit("\" fill=\"#0f172a\"/>\n");
            }
            continue;
        }
        if (shape == InlayShapeDiamond) {
            out.write_lit("<polygon points=\"");
            out.write_num(cx); out.write_lit(","); out.write_num(n.bounds.y + oy); out.write_lit(" ");
            out.write_num(n.bounds.x + n.bounds.w + ox); out.write_lit(","); out.write_num(cy); out.write_lit(" ");
            out.write_num(cx); out.write_lit(","); out.write_num(n.bounds.y + n.bounds.h + oy); out.write_lit(" ");
            out.write_num(n.bounds.x + ox); out.write_lit(","); out.write_num(cy);
            out.write_lit("\" fill=\"#ffffff\" stroke=\"#94a3b8\" stroke-width=\"");
            out.write_num(std::fmax(0.9f, 1.15f * scale)); out.write_lit("\"/>\n");
        } else if (shape == InlayShapeCircle) {
            out.write_lit("<circle cx=\""); out.write_num(cx);
            out.write_lit("\" cy=\""); out.write_num(cy);
            out.write_lit("\" r=\""); out.write_num(n.bounds.w * 0.5f);
            out.write_lit("\" fill=\"#ffffff\" stroke=\"#94a3b8\" stroke-width=\"");
            out.write_num(border_w); out.write_lit("\"/>\n");
        } else if (shape == InlayShapeClassBox) {
            const float rad = 6.0f * scale;
            out.write_lit("<rect x=\""); out.write_num(n.bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(n.bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(n.bounds.w);
            out.write_lit("\" height=\""); out.write_num(n.bounds.h);
            out.write_lit("\" rx=\""); out.write_num(rad);
            out.write_lit("\" ry=\""); out.write_num(rad);
            out.write_lit("\" fill=\"#ffffff\" stroke=\"#94a3b8\" stroke-width=\"");
            out.write_num(border_w); out.write_lit("\"/>\n");
            uint32_t nl = UINT32_MAX;
            for (uint32_t ci = 0u; ci < n.label.length; ++ci) {
                if (n.label.chars[ci] == '\n') { nl = ci; break; }
            }
            if (nl != UINT32_MAX) {
                const EtchStr hdr = { n.label.chars, nl, false };
                const EtchStr members = { n.label.chars + nl + 1u, n.label.length - (nl + 1u), false };
                const float hdr_h = n_lh + 14.0f * scale;
                out.write_lit("<line x1=\""); out.write_num(n.bounds.x + ox);
                out.write_lit("\" y1=\""); out.write_num(n.bounds.y + hdr_h + oy);
                out.write_lit("\" x2=\""); out.write_num(n.bounds.x + n.bounds.w + ox);
                out.write_lit("\" y2=\""); out.write_num(n.bounds.y + hdr_h + oy);
                out.write_lit("\" stroke=\"#cbd5e1\" stroke-width=\""); out.write_num(border_w); out.write_lit("\"/>\n");
                svg_emit_diagram_label(&out, hdr, cx, n.bounds.y + 7.0f * scale + oy, n_lh, eff_font, "#0f172a", "600", "middle");
                svg_emit_diagram_label(&out, members, n.bounds.x + 12.0f * scale + ox, n.bounds.y + hdr_h + 6.0f * scale + oy,
                                       n_lh, eff_font * 0.92f, "#334155", nullptr, "start");
            } else {
                svg_emit_diagram_label(&out, n.label, cx, cy - tsz.y * 0.5f, n_lh, eff_font, "#0f172a", "600", "middle");
            }
            continue;
        } else {
            const float rad = (shape == InlayShapeRound) ? (n.bounds.h * 0.5f) : (6.0f * scale);
            out.write_lit("<rect x=\""); out.write_num(n.bounds.x + ox);
            out.write_lit("\" y=\""); out.write_num(n.bounds.y + oy);
            out.write_lit("\" width=\""); out.write_num(n.bounds.w);
            out.write_lit("\" height=\""); out.write_num(n.bounds.h);
            out.write_lit("\" rx=\""); out.write_num(rad);
            out.write_lit("\" ry=\""); out.write_num(rad);
            out.write_lit("\" fill=\"#ffffff\" stroke=\"#94a3b8\" stroke-width=\"");
            out.write_num(border_w); out.write_lit("\"/>\n");
        }
        svg_emit_diagram_label(&out, n.label, cx, cy - tsz.y * 0.5f, n_lh, eff_font, "#0f172a");
    }

    out.write_lit("</svg>\n");
    heap.destroy();
    if (out_buf && cap > 0u) {
        out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = '\0';
    }
    return out.len;
}
}
