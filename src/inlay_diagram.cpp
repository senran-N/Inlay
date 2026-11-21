#include "inlay_internal.h"
#include <cmath>
#include <cstring>

namespace {
struct Graph {
    EtchStr source;
    uint32_t pos;
    InlayDiagramNode* nodes;
    InlayDiagramShape* shapes;
    int32_t* node_subgraph;
    InlayDiagramEdge* edges;
    InlayDiagramSubgraph* subgraphs;
    uint32_t node_count, edge_count, subgraph_count;
    int32_t active_subgraph;
    bool valid, lr;
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

uint32_t node(Graph* g) noexcept {
    spaces(g);
    const uint32_t start = g->pos;
    while (g->pos < g->source.length && word(g->source.chars[g->pos])) ++g->pos;
    if (start == g->pos) { g->valid = false; return 0; }
    const EtchStr id = { g->source.chars + start, g->pos - start, false };
    uint32_t index = 0;
    while (index < g->node_count && !equal(g->nodes[index].id, id)) ++index;
    if (index == g->node_count) {
        if (index == 256) { g->valid = false; return 0; }
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
        }
        const uint32_t label = g->pos;
        while (g->pos < g->source.length && g->source.chars[g->pos] != close && g->source.chars[g->pos] != '\n') ++g->pos;
        if (g->pos == g->source.length || g->source.chars[g->pos] != close) { g->valid = false; return index; }
        g->nodes[index].label = { g->source.chars + label, g->pos - label, false };
        g->shapes[index] = shape;
        ++g->pos;
        if (double_close) {
            if (g->pos == g->source.length || g->source.chars[g->pos] != second_close) { g->valid = false; return index; }
            ++g->pos;
        }
    }
    return index;
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

bool flow_arrow(Graph* g, InlayDiagramEdge* edge) noexcept {
    edge->dashed = starts(*g, ETCH_STR_LIT("-."));
    const auto arrow = edge->dashed ? ETCH_STR_LIT("-.->") : ETCH_STR_LIT("-->");
    if (starts(*g, arrow)) {
        g->pos += arrow.length;
        spaces(g);
        if (!statement_end(*g) && g->source.chars[g->pos] == '|') {
            const uint32_t start = ++g->pos;
            while (g->pos < g->source.length && g->source.chars[g->pos] != '|' && g->source.chars[g->pos] != '\n') ++g->pos;
            if (g->pos == g->source.length || g->source.chars[g->pos] != '|') return false;
            edge->label = trim_slice(g->source, start, g->pos++);
        }
        return true;
    }
    if (!edge->dashed && !starts(*g, ETCH_STR_LIT("--"))) return false;
    g->pos += 2;
    const uint32_t start = g->pos;
    const auto close = edge->dashed ? ETCH_STR_LIT(".->") : ETCH_STR_LIT("-->");
    while (!statement_end(*g) && !starts(*g, close)) ++g->pos;
    if (statement_end(*g)) return false;
    edge->label = trim_slice(g->source, start, g->pos);
    g->pos += close.length;
    return edge->label.length != 0;
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

void draw_dashed(const EtchBezierDesc& b) noexcept {
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
    const float period = std::fmax(12.0f, length / 128.0f);
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

void draw_arrowhead(EtchVec2 tip, EtchVec2 tangent, EtchColor color) noexcept {
    const float len = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
    if (len <= 0.001f) return;
    tangent.x /= len;
    tangent.y /= len;
    EtchVec2 points[] = {
        tip,
        { tip.x - tangent.x * 8.0f - tangent.y * 4.0f, tip.y - tangent.y * 8.0f + tangent.x * 4.0f },
        { tip.x - tangent.x * 8.0f + tangent.y * 4.0f, tip.y - tangent.y * 8.0f - tangent.x * 4.0f },
    };
    EtchFilledPolygonDesc arrow = {};
    arrow.points = points;
    arrow.point_count = 3;
    arrow.fill = color;
    etch_draw_filled_polygon(&arrow);
}
} // namespace

InlayDiagramResult inlay_diagram_layout_ex(EtchStr source, const InlayDiagramDesc* input, InlayDiagramMeta* out_meta) noexcept {
    if (out_meta) *out_meta = {};
    auto* ctx = etch_current_context();
    if (!ctx || !etch_frame_in_progress(ctx) || !input || !source.chars || source.length > 16384 ||
        !std::isfinite(input->pos.x) || !std::isfinite(input->pos.y) ||
        !std::isfinite(input->font_size_px) || input->font_size_px < 6 || input->font_size_px > 128 ||
        !std::isfinite(input->node_gap_px) || input->node_gap_px < 0 || input->node_gap_px > 4096 ||
        !std::isfinite(input->rank_gap_px) || input->rank_gap_px < 0 || input->rank_gap_px > 4096 ||
        (input->canvas_id && (!std::isfinite(input->viewport_width_px) || !std::isfinite(input->viewport_height_px) ||
                              input->viewport_width_px < 1 || input->viewport_height_px < 1)))
    {
        return {};
    }
    constexpr uint32_t kNodeCap = 256;
    constexpr uint32_t kEdgeCap = 1024;
    constexpr uint32_t kSubgraphCap = 32;
    auto* nodes = static_cast<InlayDiagramNode*>(etch_frame_arena_alloc(ctx, size_t(kNodeCap) * sizeof(InlayDiagramNode), alignof(InlayDiagramNode)));
    auto* edges = static_cast<InlayDiagramEdge*>(etch_frame_arena_alloc(ctx, size_t(kEdgeCap) * sizeof(InlayDiagramEdge), alignof(InlayDiagramEdge)));
    auto* shapes = static_cast<InlayDiagramShape*>(etch_frame_arena_alloc(ctx, size_t(kNodeCap) * sizeof(InlayDiagramShape), alignof(InlayDiagramShape)));
    auto* node_sub = static_cast<int32_t*>(etch_frame_arena_alloc(ctx, size_t(kNodeCap) * sizeof(int32_t), alignof(int32_t)));
    auto* subgraphs = static_cast<InlayDiagramSubgraph*>(etch_frame_arena_alloc(ctx, size_t(kSubgraphCap) * sizeof(InlayDiagramSubgraph), alignof(InlayDiagramSubgraph)));
    if (!nodes || !edges || !shapes || !node_sub || !subgraphs) return {};

    Graph g = { source, 0, nodes, shapes, node_sub, edges, subgraphs, 0, 0, 0, -1, true, false };
    spaces(&g);
    const uint32_t header = g.pos;
    while (g.pos < source.length && word(source.chars[g.pos])) ++g.pos;
    const EtchStr kind = { source.chars + header, g.pos - header, false };

    if (equal(kind, ETCH_STR_LIT("sequenceDiagram"))) {
        while (g.valid && g.pos < source.length) {
            spaces(&g);
            if (g.pos == source.length) break;
            if (source.chars[g.pos] == '\n' || source.chars[g.pos] == ';') { ++g.pos; continue; }
            if (g.pos + 12 <= source.length && !std::memcmp(source.chars + g.pos, "participant ", 12)) {
                g.pos += 12;
                const uint32_t who = node(&g);
                spaces(&g);
                if (g.pos + 3 <= source.length && !std::memcmp(source.chars + g.pos, "as ", 3)) {
                    g.pos += 3;
                    const uint32_t name = g.pos;
                    while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
                    nodes[who].label = trim_slice(source, name, g.pos);
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
            if (!g.valid || g.pos == source.length || source.chars[g.pos++] != ':' || g.edge_count == kEdgeCap) { g.valid = false; break; }
            spaces(&g);
            const uint32_t label = g.pos;
            while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos;
            edges[g.edge_count++] = { from, to, false, dashed, trim_slice(source, label, g.pos) };
        }
        if (!g.valid || !g.node_count) return {};
        float width = 0, max_message = 0;
        for (uint32_t i = 0; i < g.edge_count; ++i) {
            max_message = std::fmax(max_message, etch_measure_text(edges[i].label, ETCH_FONT_SANS, input->font_size_px, 0).x + 24.0f);
        }
        for (uint32_t i = 0; i < g.node_count; ++i) {
            const auto sz = etch_measure_text(nodes[i].label, ETCH_FONT_SANS, input->font_size_px, 0);
            nodes[i].bounds = { input->pos.x + width, input->pos.y, std::fmax(sz.x + 32.0f, max_message), input->font_size_px * 1.5f + 24.0f };
            width += nodes[i].bounds.w + input->node_gap_px;
        }
        const float height = input->font_size_px * 1.5f + 24.0f + float(g.edge_count + 1) * input->rank_gap_px;
        for (uint32_t i = 0; i < g.edge_count; ++i) if (edges[i].from == edges[i].to) { width += 40.0f; break; }
        if (out_meta) { out_meta->shapes = shapes; }
        return { nodes, edges, g.node_count, g.edge_count, width - input->node_gap_px, height, true, false, true };
    }

    if (!equal(kind, ETCH_STR_LIT("flowchart")) && !equal(kind, ETCH_STR_LIT("graph"))) return {};
    spaces(&g);
    const uint32_t direction = g.pos;
    while (g.pos < source.length && word(source.chars[g.pos])) ++g.pos;
    const EtchStr dir = { source.chars + direction, g.pos - direction, false };
    g.lr = equal(dir, ETCH_STR_LIT("LR"));
    if (!g.lr && !equal(dir, ETCH_STR_LIT("TD")) && !equal(dir, ETCH_STR_LIT("TB"))) return {};

    int32_t sub_stack[kSubgraphCap] = {};
    uint32_t sub_depth = 0;
    while (g.valid && g.pos < source.length) {
        spaces(&g);
        if (g.pos == source.length) break;
        if (statement_end(g)) { ++g.pos; continue; }
        if (starts(g, ETCH_STR_LIT("%%"))) { while (g.pos < source.length && source.chars[g.pos] != '\n') ++g.pos; continue; }
        if (starts(g, ETCH_STR_LIT("subgraph")) && g.pos + 8 < source.length &&
            (source.chars[g.pos + 8] == ' ' || source.chars[g.pos + 8] == '\t'))
        {
            g.pos += 8;
            spaces(&g);
            if (statement_end(g)) return {};
            const uint32_t title_start = g.pos;
            while (!statement_end(g)) ++g.pos;
            EtchStr title = trim_slice(source, title_start, g.pos);
            // Support optional Mermaid `subgraph id [Title]` bracketed label syntax.
            for (uint32_t k = 0; k < title.length; ++k) {
                if (title.chars[k] == '[' && title.chars[title.length - 1] == ']') {
                    title = trim_slice(title, k + 1, title.length - 1);
                    break;
                }
            }
            int32_t idx = -1;
            if (g.subgraph_count < kSubgraphCap) {
                idx = int32_t(g.subgraph_count++);
                subgraphs[idx] = { title, {}, g.active_subgraph, false };
            }
            if (sub_depth < kSubgraphCap) sub_stack[sub_depth++] = g.active_subgraph;
            else return {};
            g.active_subgraph = idx;
            continue;
        }
        if (starts(g, ETCH_STR_LIT("end")) && (g.pos + 3 == source.length || !word(source.chars[g.pos + 3]))) {
            const uint32_t start = g.pos;
            g.pos += 3;
            spaces(&g);
            if (statement_end(g)) {
                if (!sub_depth) return {};
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
            if (g.edge_count == kEdgeCap || !flow_arrow(&g, &edge)) { g.valid = false; break; }
            edge.to = node(&g);
            if (!g.valid) break;
            edges[g.edge_count++] = edge;
            from = edge.to;
        }
    }
    if (!g.valid || !g.node_count || sub_depth) return {};

    // 1. Iterative DFS reverses feedback edges so remaining orientation is acyclic.
    auto* colors = static_cast<uint8_t*>(etch_frame_arena_alloc(ctx, g.node_count, 1));
    auto* stack = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(g.node_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* cursor = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(g.node_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* topo = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(g.node_count) * sizeof(uint32_t), alignof(uint32_t)));
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
    for (uint32_t i = 0; i < g.edge_count; ++i) {
        const auto& e = edges[i];
        if (e.from == e.to || e.reversed) continue;
        if (nodes[e.to].rank > nodes[e.from].rank + 1) {
            const uint32_t span = nodes[e.to].rank - nodes[e.from].rank - 1;
            if (dummy_count + span <= 512u) dummy_count += span;
        }
    }
    const uint32_t vnode_count = g.node_count + dummy_count;
    const uint32_t vedge_cap = g.edge_count + dummy_count;
    auto* vrank = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(vnode_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* order = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(vnode_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* pos_in_rank = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(vnode_count) * sizeof(uint32_t), alignof(uint32_t)));
    auto* bary = static_cast<float*>(etch_frame_arena_alloc(ctx, size_t(vnode_count) * sizeof(float), alignof(float)));
    auto* vedges = static_cast<VEdge*>(etch_frame_arena_alloc(ctx, size_t(vedge_cap ? vedge_cap : 1u) * sizeof(VEdge), alignof(VEdge)));
    auto* edge_paths = static_cast<InlayDiagramEdgePath*>(etch_frame_arena_alloc(ctx, size_t(g.edge_count ? g.edge_count : 1u) * sizeof(InlayDiagramEdgePath), alignof(InlayDiagramEdgePath)));
    auto* waypoints = static_cast<EtchVec2*>(etch_frame_arena_alloc(ctx, size_t(dummy_count ? dummy_count : 1u) * sizeof(EtchVec2), alignof(EtchVec2)));
    auto* dummy_to_wp = static_cast<uint32_t*>(etch_frame_arena_alloc(ctx, size_t(dummy_count ? dummy_count : 1u) * sizeof(uint32_t), alignof(uint32_t)));
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

    // 5. Measure node sizes (shape-aware) and rank gaps.
    float label_width = 0.0f, rank_gap = input->rank_gap_px;
    for (uint32_t i = 0; i < g.edge_count; ++i) if (edges[i].label.length) {
        const float w = etch_measure_text(edges[i].label, ETCH_FONT_SANS, input->font_size_px, 0).x + 12.0f;
        label_width = std::fmax(label_width, w);
        if (g.lr && !edges[i].reversed && edges[i].from != edges[i].to) rank_gap = std::fmax(rank_gap, w);
    }
    for (uint32_t i = 0; i < g.node_count; ++i) {
        const auto text = etch_measure_text(nodes[i].label, ETCH_FONT_SANS, input->font_size_px, 0);
        float w = text.x + 32.0f;
        float h = input->font_size_px * 1.5f + 24.0f;
        if (shapes[i] == InlayShapeDiamond) {
            w = text.x + 56.0f;
            h = std::fmax(h * 1.35f, input->font_size_px * 2.4f + 24.0f);
        } else if (shapes[i] == InlayShapeCircle) {
            const float d = std::fmax(w, h);
            w = d;
            h = d;
        }
        nodes[i].bounds = { 0.0f, 0.0f, w, h };
    }

    // 6. Place ranks with cross-axis centering and virtual waypoint coordinates.
    auto* rank_cross = static_cast<float*>(etch_frame_arena_alloc(ctx, size_t(max_rank + 1) * sizeof(float), alignof(float)));
    auto* rank_start_pos = static_cast<float*>(etch_frame_arena_alloc(ctx, size_t(max_rank + 1) * sizeof(float), alignof(float)));
    auto* rank_depth = static_cast<float*>(etch_frame_arena_alloc(ctx, size_t(max_rank + 1) * sizeof(float), alignof(float)));
    if (!rank_cross || !rank_start_pos || !rank_depth) return {};

    float rank_position = 0.0f, total_cross = 0.0f;
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
                cross += (g.lr ? n.bounds.h : n.bounds.w) + input->node_gap_px;
                rsize = std::fmax(rsize, g.lr ? n.bounds.w : n.bounds.h);
            } else {
                const float dummy_span = 14.0f;
                const uint32_t wpi = dummy_to_wp[vid - g.node_count];
                waypoints[wpi] = {
                    input->pos.x + (g.lr ? rank_position : cross + dummy_span * 0.5f),
                    input->pos.y + (g.lr ? cross + dummy_span * 0.5f : rank_position)
                };
                cross += dummy_span + input->node_gap_px;
                rsize = std::fmax(rsize, 16.0f);
            }
            ++in_r;
        }
        const float used_cross = (in_r > 0u && cross > input->node_gap_px) ? (cross - input->node_gap_px) : 0.0f;
        rank_cross[r] = used_cross;
        rank_start_pos[r] = rank_position;
        rank_depth[r] = rsize;
        total_cross = std::fmax(total_cross, used_cross);
        rank_position += rsize + (r < max_rank ? rank_gap : 0.0f);
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

    // 7. Compute subgraph cluster bounding boxes (bottom-up for nested subgraphs).
    if (g.subgraph_count > 0) {
        const float title_h = input->font_size_px * 1.4f;
        const float pad_x = 14.0f, pad_top = title_h + 10.0f, pad_bot = 14.0f;
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
        // Propagate inner subgraph padded bounds up into parent subgraphs (child index > parent index).
        for (int32_t si = int32_t(g.subgraph_count) - 1; si >= 0; --si) {
            auto& s = subgraphs[si];
            if (!s.has_nodes) continue;
            const float title_w = etch_measure_text(s.title, ETCH_FONT_SEMIBOLD, std::fmax(11.0f, input->font_size_px * 0.85f), 0).x + 20.0f;
            s.bounds.x -= pad_x;
            s.bounds.y -= pad_top;
            s.bounds.w = std::fmax(s.bounds.w + pad_x * 2.0f, title_w);
            s.bounds.h += pad_top + pad_bot;
            if (s.parent >= 0 && s.parent < si) {
                expand_rect(&subgraphs[s.parent].bounds, &subgraphs[s.parent].has_nodes, s.bounds);
            }
        }
        // Shift whole diagram if subgraph frames extend above/left of input->pos.
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
            width = std::fmax(width, nodes[edges[i].from].bounds.x - input->pos.x + nodes[edges[i].from].bounds.w + 40.0f);
        }
    }
    if (feedback_lanes > 0) {
        const float lane_extra = 22.0f + float(feedback_lanes) * 18.0f;
        if (g.lr) height += lane_extra; else width += lane_extra;
    }

    if (out_meta) {
        out_meta->shapes = shapes;
        out_meta->subgraphs = subgraphs;
        out_meta->subgraph_count = g.subgraph_count;
        out_meta->edge_paths = edge_paths;
        out_meta->waypoints = waypoints;
        out_meta->waypoint_count = wp_used;
    }
    return { nodes, edges, g.node_count, g.edge_count, std::fmax(width, label_width), height, true, g.lr, false };
}

void inlay_diagram_draw_ex(const InlayDiagramResult& result, const InlayDiagramDesc& input, const InlayDiagramMeta* meta) noexcept {
    const auto theme = etch_get_theme(etch_current_context());
    if (input.canvas_id) {
        auto canvas = etch_canvas_desc_default();
        canvas.id = input.canvas_id;
        canvas.width = etch_sizing_fixed(input.viewport_width_px);
        canvas.height = etch_sizing_fixed(input.viewport_height_px);
        etch_begin_canvas(&canvas);
    }

    // 1. Subgraph cluster frames (outer before inner).
    if (meta && meta->subgraphs && meta->subgraph_count > 0) {
        const float title_font = std::fmax(11.0f, input.font_size_px * 0.85f);
        for (uint32_t si = 0; si < meta->subgraph_count; ++si) {
            const auto& sg = meta->subgraphs[si];
            if (!sg.has_nodes) continue;
            auto frame = etch_rect_draw_desc_default();
            frame.rect = sg.bounds;
            frame.fill = { theme.surface_subtle.r, theme.surface_subtle.g, theme.surface_subtle.b, 88 };
            frame.border_color = theme.border_control.a != 0 ? theme.border_control : theme.border_subtle;
            frame.border_width = 1.0f;
            frame.radius = etch_corners_all(theme.radius_panel_px);
            frame.continuous_corners = true;
            etch_draw_rect(&frame);
            if (sg.title.length) {
                auto td = etch_text_draw_desc_default();
                td.text = sg.title;
                td.font_id = ETCH_FONT_SEMIBOLD;
                td.pos = { sg.bounds.x + 10.0f, sg.bounds.y + 6.0f };
                td.size = title_font;
                td.color = theme.text_secondary;
                etch_draw_text(&td);
            }
        }
    }

    // 2. Sequence diagram lifelines.
    if (result.sequence) {
        for (uint32_t i = 0; i < result.node_count; ++i) {
            const auto b = result.nodes[i].bounds;
            EtchLineDesc line = {};
            line.from = { b.x + b.w * 0.5f, b.y + b.h };
            line.to = { line.from.x, input.pos.y + result.height };
            line.color = theme.border_line;
            line.thickness = 1.0f;
            etch_draw_line(&line);
        }
    }

    // 3. Edges (including multi-rank waypoint splines & separated feedback lanes).
    for (uint32_t i = 0; i < result.edge_count; ++i) {
        const auto& edge = result.edges[i];
        const auto a = result.nodes[edge.from].bounds, b = result.nodes[edge.to].bounds;
        const EtchColor col = edge.reversed ? theme.accent_secondary : theme.accent;
        const InlayDiagramEdgePath path = (meta && meta->edge_paths) ? meta->edge_paths[i] : InlayDiagramEdgePath{ 0, 0, edge.reversed ? 0 : -1 };

        EtchVec2 label_mid = {};
        if (!result.sequence && !edge.reversed && edge.from != edge.to && path.point_count > 0 && meta && meta->waypoints) {
            // Multi-rank routed spline through Sugiyama virtual waypoints.
            const uint32_t segs = path.point_count + 1;
            const EtchVec2 start_pt = result.horizontal
                ? EtchVec2{ a.x + a.w, a.y + a.h * 0.5f }
                : EtchVec2{ a.x + a.w * 0.5f, a.y + a.h };
            const EtchVec2 end_pt = result.horizontal
                ? EtchVec2{ b.x, b.y + b.h * 0.5f }
                : EtchVec2{ b.x + b.w * 0.5f, b.y };
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
                bz.thickness = 2.0f;
                if (edge.dashed) draw_dashed(bz); else etch_draw_bezier(&bz);
                if (s == segs / 2) label_mid = curve_point(bz, 0.5f);
                if (s + 1 == segs) last_tan = { bz.p3.x - bz.p2.x, bz.p3.y - bz.p2.y };
            }
            draw_arrowhead(end_pt, last_tan, col);
        } else {
            auto bezier = etch_bezier_desc_default();
            bezier.p0 = { a.x + a.w * 0.5f, a.y + a.h };
            bezier.p3 = { b.x + b.w * 0.5f, b.y };
            if (result.horizontal) {
                bezier.p0 = { a.x + a.w, a.y + a.h * 0.5f };
                bezier.p3 = { b.x, b.y + b.h * 0.5f };
            }
            if (result.sequence) {
                const float y = input.pos.y + a.h + float(i + 1) * input.rank_gap_px;
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
                const float lane_off = float(path.feedback_lane >= 0 ? path.feedback_lane : 0) * 18.0f;
                if (result.horizontal) {
                    const float route = input.pos.y + result.height - 8.0f - lane_off;
                    bezier.p0 = { a.x + a.w * 0.5f, a.y + a.h };
                    bezier.p3 = { b.x + b.w * 0.5f, b.y + b.h };
                    bezier.p1 = { bezier.p0.x, route };
                    bezier.p2 = { bezier.p3.x, route };
                } else {
                    const float route = input.pos.x + result.width - 8.0f - lane_off;
                    bezier.p0 = { a.x + a.w, a.y + a.h * 0.5f };
                    bezier.p3 = { b.x + b.w, b.y + b.h * 0.5f };
                    bezier.p1 = { route, bezier.p0.y };
                    bezier.p2 = { route, bezier.p3.y };
                }
            }
            if (edge.from == edge.to) {
                const float loop_y = result.sequence ? bezier.p0.y : a.y + a.h * 0.5f;
                const float loop_x = result.sequence ? a.x + a.w * 0.5f : a.x + a.w;
                bezier.p0 = { loop_x, loop_y - 12.0f };
                bezier.p3 = { loop_x, loop_y + 12.0f };
                bezier.p1 = { loop_x + 40.0f, bezier.p0.y };
                bezier.p2 = { loop_x + 40.0f, bezier.p3.y };
            }
            bezier.cubic = true;
            bezier.color_start = col;
            bezier.color_end = col;
            bezier.thickness = 2.0f;
            if (edge.dashed) draw_dashed(bezier); else etch_draw_bezier(&bezier);
            EtchVec2 tangent = { bezier.p3.x - bezier.p2.x, bezier.p3.y - bezier.p2.y };
            if (result.sequence && edge.from != edge.to) tangent = { bezier.p3.x - bezier.p0.x, 0.0f };
            draw_arrowhead(bezier.p3, tangent, col);
            label_mid = curve_point(bezier, 0.5f);
        }

        if (edge.label.length) {
            const float font = input.font_size_px;
            const auto sz = etch_measure_text(edge.label, ETCH_FONT_SANS, font, 0);
            const float half = sz.x * 0.5f + 4.0f;
            const float x = std::fmax(input.pos.x + half, std::fmin(label_mid.x, input.pos.x + result.width - half));
            const float lift = result.sequence || result.horizontal ? sz.y + 4.0f : sz.y * 0.5f;
            const float y = std::fmax(input.pos.y, label_mid.y - lift);
            auto plate = etch_rect_draw_desc_default();
            plate.rect = { x - half, y, half * 2.0f, sz.y };
            plate.fill = theme.bg_elevated;
            plate.radius = etch_corners_all(theme.radius_small_px);
            etch_draw_rect(&plate);
            auto text = etch_text_draw_desc_default();
            text.text = edge.label;
            text.pos = { x, y };
            text.align_x = ETCH_ALIGN_X_CENTER;
            text.size = font;
            text.color = theme.text_secondary;
            etch_draw_text(&text);
        }
    }

    // 4. Nodes (shape-aware rendering).
    for (uint32_t i = 0; i < result.node_count; ++i) {
        const auto& n = result.nodes[i];
        const InlayDiagramShape shape = (meta && meta->shapes) ? meta->shapes[i] : InlayShapeRect;
        const auto tsz = etch_measure_text(n.label, ETCH_FONT_SANS, input.font_size_px, 0);
        if (shape == InlayShapeDiamond) {
            const float cx = n.bounds.x + n.bounds.w * 0.5f;
            const float cy = n.bounds.y + n.bounds.h * 0.5f;
            EtchVec2 pts[4] = {
                { cx, n.bounds.y },
                { n.bounds.x + n.bounds.w, cy },
                { cx, n.bounds.y + n.bounds.h },
                { n.bounds.x, cy },
            };
            EtchFilledPolygonDesc poly = {};
            poly.points = pts;
            poly.point_count = 4;
            poly.fill = theme.bg_elevated;
            etch_draw_filled_polygon(&poly);
            EtchLineDesc edge_l = {};
            edge_l.color = theme.border_node;
            edge_l.thickness = 1.25f;
            for (int k = 0; k < 4; ++k) {
                edge_l.from = pts[k];
                edge_l.to = pts[(k + 1) & 3];
                etch_draw_line(&edge_l);
            }
            auto text = etch_text_draw_desc_default();
            text.text = n.label;
            text.pos = { cx, cy - tsz.y * 0.5f };
            text.align_x = ETCH_ALIGN_X_CENTER;
            text.size = input.font_size_px;
            text.color = theme.text_primary;
            etch_draw_text(&text);
        } else {
            auto rect = etch_rect_draw_desc_default();
            rect.rect = n.bounds;
            rect.fill = theme.bg_elevated;
            rect.border_color = theme.border_node;
            rect.border_width = 1.0f;
            if (shape == InlayShapeCircle) {
                rect.radius = etch_corners_all(n.bounds.w * 0.5f);
                rect.continuous_corners = false;
            } else if (shape == InlayShapeRound) {
                rect.radius = etch_corners_all(n.bounds.h * 0.5f);
                rect.continuous_corners = false;
            } else {
                rect.radius = etch_corners_all(theme.radius_control_px);
                rect.continuous_corners = true;
            }
            etch_draw_rect(&rect);
            auto text = etch_text_draw_desc_default();
            text.text = n.label;
            text.pos = { n.bounds.x + n.bounds.w * 0.5f, n.bounds.y + (n.bounds.h - tsz.y) * 0.5f };
            text.align_x = ETCH_ALIGN_X_CENTER;
            text.size = input.font_size_px;
            text.color = theme.text_primary;
            etch_draw_text(&text);
        }
    }
    if (input.canvas_id) etch_end_canvas();
}

void inlay_diagram_draw(const InlayDiagramResult& result, const InlayDiagramDesc& input) {
    inlay_diagram_draw_ex(result, input, nullptr);
}

extern "C" {
InlayDiagramDesc inlay_diagram_desc_default(void) { return { {0, 0}, 16, 24, 48, 0, 640, 400 }; }
InlayDiagramResult inlay_diagram_layout(EtchStr source, const InlayDiagramDesc* desc) {
    return inlay_diagram_layout_ex(source, desc, nullptr);
}
InlayDiagramResult inlay_diagram_view(EtchStr source, const InlayDiagramDesc* input) {
    InlayDiagramMeta meta = {};
    auto result = inlay_diagram_layout_ex(source, input, &meta);
    if (!result.valid) return result;
    inlay_diagram_draw_ex(result, *input, &meta);
    return result;
}
}
