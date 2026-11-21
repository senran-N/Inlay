#include "inlay_internal.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

namespace {

EtchStr slice(EtchStr source, uint32_t start, uint32_t n) noexcept {
 return {source.chars + start, n, false};
}

EtchStr doc_source_str(const InlayDoc* doc) noexcept {
 return {doc->source, doc->source_size, false};
}

EtchStr block_code_str(const InlayDoc* doc, uint32_t block_idx) noexcept {
 const auto& b = doc->blocks[block_idx];
 if (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC) {
  const auto& m = doc->block_meta[block_idx];
  return {doc->ast_text + m.pool_offset, m.pool_length, false};
 }
 return {doc->source + b.content_offset, b.content_length, false};
}

uint32_t strip_quote_prefix(const char* s, uint32_t len, uint16_t max_quotes) noexcept {
 uint32_t p = 0;
 for (uint16_t q = 0; q < max_quotes && p < len; ++q) {
  uint32_t ind = 0;
  while (p + ind < len && ind < 3u && s[p + ind] == ' ') ++ind;
  if (p + ind < len && s[p + ind] == '>') {
   p += ind + 1u;
   if (p < len && s[p] == ' ') ++p;
  } else {
   break;
  }
 }
 return p;
}

void details_range(EtchStr source, const InlayBlock& b, uint32_t* summary, uint32_t* summary_end, uint32_t* body, uint32_t* end) noexcept {
 *end = b.content_offset + b.content_length;
 *summary = b.content_offset;
 while (*summary < *end && !(*summary + 9u <= *end && !std::memcmp(source.chars + *summary, "<summary>", 9))) ++*summary;
 if (*summary < *end) *summary += 9u;
 *summary_end = *summary;
 while (*summary_end < *end && source.chars[*summary_end] != '<') ++*summary_end;
 *body = *summary_end;
 while (*body < *end && source.chars[*body] != '\n') ++*body;
 if (*body < *end) ++*body;
 for (uint32_t pos = *body; pos + 10u <= *end; ++pos) {
  if (!std::memcmp(source.chars + pos, "</details>", 10)) { *end = pos; break; }
 }
}

bool diagram_block(const InlayDoc* doc, const InlayBlock& block) noexcept {
 return block.kind == INLAY_CODE_BLOCK && block.language_length == 7u &&
        !std::memcmp(doc->source + block.language_offset, "mermaid", 7);
}

float custom_fence_estimate(const InlayDocViewDesc& view, const InlayDoc* doc, uint32_t block_idx, float available_width) noexcept {
 const auto& block = doc->blocks[block_idx];
 if (block.kind != INLAY_CODE_BLOCK || !view.custom_fence_estimate_fn || !view.custom_fence_render_fn || diagram_block(doc, block)) return 0.f;
 const EtchStr lang = {doc->source + block.language_offset, block.language_length, false};
 const EtchStr body = block_code_str(doc, block_idx);
 const float h = view.custom_fence_estimate_fn(lang, body, available_width, view.font_size_px, view.custom_fence_user_data);
 return (std::isfinite(h) && h > 0.f) ? std::fmin(h, 4096.f) : 0.f;
}

struct NativeDraw {
 EtchStr source;
 InlayDiagramResult diagram_result;
 InlayDiagramMeta diagram_meta;
 InlayMathPrepared math_prep;
 float font;
 bool diagram;
 EtchTheme theme;
 EtchColor text_color;
};

void draw_native(EtchRect, void* user) {
 const auto& draw = *static_cast<NativeDraw*>(user);
 etch_push_theme(&draw.theme);
 if (draw.diagram) {
  auto desc = inlay_diagram_desc_default();
  desc.font_size_px = draw.font;
  inlay_diagram_draw_ex(draw.diagram_result, desc, &draw.diagram_meta);
 } else {
  const EtchColor col = draw.text_color.a != 0 ? draw.text_color : etch_get_theme(etch_current_context()).text_primary;
  inlay_math_draw_prepared(draw.math_prep, {0, 0}, col);
 }
 etch_pop_theme();
}

void native_box(EtchStr source, float font, bool diagram, uint32_t id, float max_width, EtchColor text_color = {},
                bool display_math = true, const InlayMathPrepared* pre_math = nullptr, float top_pad = 0.f) {
 auto* ctx = etch_current_context();
 EtchStr copied = source;
 if (!pre_math) {
  auto* copy = source.length ? static_cast<char*>(etch_frame_arena_alloc(ctx, source.length, 1)) : nullptr;
  if (source.length && !copy) { etch_text(source, nullptr); return; }
  if (copy) std::memcpy(copy, source.chars, source.length);
  copied = {copy, source.length, false};
 }
 float width = 0, height = 0;
 InlayDiagramResult diagram_result = {};
 InlayDiagramMeta diagram_meta = {};
 InlayMathPrepared math_prep = {};
 if (diagram) {
  auto desc = inlay_diagram_desc_default();
  desc.font_size_px = font;
  diagram_result = inlay_diagram_layout_ex(copied, &desc, &diagram_meta);
  if (!diagram_result.valid) { etch_text(copied, nullptr); return; }
  width = diagram_result.width;
  height = diagram_result.height;
 } else {
  if (pre_math) math_prep = *pre_math;
  else if (!inlay_math_prepare(copied, font, display_math, &math_prep)) { etch_text(copied, nullptr); return; }
  width = math_prep.metrics.width;
  height = math_prep.metrics.height;
 }
 const float extra_h = (diagram || display_math) ? 8.f : 0.f;
 const float box_h = height + extra_h + top_pad;
 auto box = etch_box_desc_default();
 box.id = id;
 box.width = etch_sizing_fixed(std::fmin(width, max_width));
 box.height = etch_sizing_fixed(std::fmin(box_h, 4096.f));
 box.scroll_y = box_h > 4096.f;
 box.scroll_x = width > max_width;
 box.clip_children = true;
 if (top_pad > 0.f) box.padding = {0, 0, top_pad, 0};
 etch_begin_box(&box);
 auto content = etch_box_desc_default();
 content.width = etch_sizing_fixed(width);
 content.height = etch_sizing_fixed(height + extra_h);
 etch_begin_box(&content);
 auto* draw = static_cast<NativeDraw*>(etch_frame_arena_alloc(ctx, sizeof(NativeDraw), alignof(NativeDraw)));
 if (draw) {
  *draw = {copied, diagram_result, diagram_meta, math_prep, font, diagram, etch_get_theme(ctx), text_color};
  etch_draw_after_layout(draw_native, draw, false);
 }
 etch_end_box();
 etch_end_box();
}

InlayDocStyle resolved_style(const InlayDocViewDesc& view) noexcept {
 return view.style ? *view.style : inlay_doc_style_default();
}
float effective_line_scale(const InlayDocStyle& style) noexcept {
 return style.line_height_scale > 0.f ? style.line_height_scale : 1.5f;
}
float effective_heading_font(const InlayDocViewDesc& view, const InlayDocStyle& style, uint32_t level) noexcept {
 if (level >= 1u && level <= 6u && style.heading_scale[level - 1u] > 0.f) return view.font_size_px * style.heading_scale[level - 1u];
 return view.font_size_px + float(7u - level) * 2.f;
}
uint16_t effective_sans_font(const InlayDocStyle& style) noexcept {
 return style.font_id != ETCH_INVALID_FONT ? style.font_id : ETCH_FONT_SANS;
}
uint16_t effective_bold_font(const InlayDocStyle& style) noexcept {
 return style.bold_font_id != ETCH_INVALID_FONT ? style.bold_font_id : ETCH_FONT_BOLD;
}
uint16_t effective_mono_font(const InlayDocStyle& style) noexcept {
 return style.mono_font_id != ETCH_INVALID_FONT ? style.mono_font_id : ETCH_FONT_MONO;
}
float effective_code_font_size(float font, const InlayDocStyle& style) noexcept {
 return (style.code_font_scale > 0.f) ? std::fmax(8.f, font * style.code_font_scale) : font;
}

struct ResolvedImage {
 struct SDL_GPUTexture* texture;
 float width, height, radius;
 bool valid;
};

ResolvedImage resolve_doc_image(const InlayDocViewDesc& view, EtchStr alt, EtchStr src, float max_width) {
 ResolvedImage out = {};
 if (!view.image_resolver_fn) return out;
 InlayImageResult img = {};
 if (!view.image_resolver_fn(alt, src, &img, view.image_resolver_user_data)) return out;
 if (!img.texture || !std::isfinite(img.width_px) || !std::isfinite(img.height_px) || img.width_px <= 0.f || img.height_px <= 0.f) return out;
 float w = img.width_px, h = img.height_px;
 if (w > max_width && max_width > 0.f) {
  const float s = max_width / w;
  w = max_width;
  h *= s;
 }
 out.texture = img.texture;
 out.width = w;
 out.height = h;
 out.radius = std::isfinite(img.radius_px) && img.radius_px > 0.f ? img.radius_px : 0.f;
 out.valid = true;
 return out;
}

void draw_image_fallback(EtchStr alt, uint32_t id, float font, const InlayDocStyle& style, const EtchTheme& theme) {
 const EtchStr label = alt.length ? alt : ETCH_STR_LIT("image");
 auto pill = etch_box_desc_default();
 pill.id = id;
 pill.padding = etch_padding_xy(8, 4);
 pill.background = theme.surface_subtle;
 pill.border_color = theme.border_subtle;
 pill.border_width = 1;
 pill.radius = etch_corners_all(theme.radius_small_px);
 pill.align_y = ETCH_ALIGN_Y_CENTER;
 etch_begin_box(&pill);
 auto td = etch_text_desc_default();
 td.font_size_px = std::fmax(11.f, font * 0.85f);
 td.font_id = effective_sans_font(style);
 td.color = style.muted_color.a != 0 ? style.muted_color : theme.text_muted;
 td.wrap = false;
 auto* ctx = etch_current_context();
 char* buf = static_cast<char*>(etch_frame_arena_alloc(ctx, size_t(label.length) + 2, 1));
 if (buf) {
  buf[0] = '[';
  std::memcpy(buf + 1, label.chars, label.length);
  buf[label.length + 1] = ']';
  etch_text({buf, label.length + 2u, false}, &td);
 } else {
  etch_text(label, &td);
 }
 etch_end_box();
}

struct PendingTaskToggle {
 uint32_t source_offset, block_index, item_index;
 bool checked, triggered;
};

float block_padding(const InlayDocViewDesc& view, InlayBlockKind kind) noexcept {
 return kind == INLAY_CODE_BLOCK || kind == INLAY_ALERT || kind == INLAY_BLOCKQUOTE ? 12.f : view.block_padding_px;
}

float block_nest_indent(const InlayBlock& b, float font) noexcept {
 float x = 0.f;
 if (b.quote_depth > 1u) x += float(b.quote_depth - 1u) * 14.f;
 else if (b.quote_depth == 1u && b.kind != INLAY_BLOCKQUOTE && b.kind != INLAY_ALERT) x += 14.f;
 if (b.list_depth > 0u && b.kind != INLAY_LIST && b.kind != INLAY_TASK_LIST) {
  x += float(b.list_depth) * std::fmax(16.f, font * 1.1f);
 }
 return x;
}

bool details_body(const InlayMeasure& m, const InlayRow& row) noexcept {
 const auto& range = m.details[row.block];
 return m.open[row.block] && row.offset + row.length > range.body && row.offset < range.end;
}

bool skipped_row(const InlayMeasure& m, const InlayRow& row) noexcept {
 return row.separator || (row.kind == INLAY_DETAILS && !row.first && !details_body(m, row));
}

constexpr float collapsed_height = -1.f;
constexpr uint32_t inline_open = 1u << 10, inline_close = 1u << 11;

bool append_inline(InlayMeasure* m, InlayInline token) noexcept {
 if (m->token_count == m->token_capacity) {
  const uint32_t cap = m->token_capacity ? m->token_capacity * 2u : 64u;
  auto* memory = std::realloc(m->tokens, size_t(cap) * sizeof(InlayInline));
  if (!memory) return false;
  m->tokens = static_cast<InlayInline*>(memory);
  m->token_capacity = cap;
 }
 m->tokens[m->token_count++] = token;
 return true;
}

bool inline_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }
bool inline_alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool scan_balanced_paren_url(const char* text, uint32_t url_start, uint32_t end, uint32_t* out_close) noexcept {
 uint32_t p = url_start;
 int depth = 0;
 while (p < end) {
  const char ch = text[p];
  if (ch == '\\' && p + 1u < end) { p += 2u; continue; }
  if (ch == '(') { ++depth; ++p; continue; }
  if (ch == ')') { if (depth == 0) { *out_close = p; return true; } --depth; ++p; continue; }
  ++p;
 }
 return false;
}

bool scan_link_bracket(const char* text, uint32_t inner_start, uint32_t end, uint32_t* out_close) noexcept {
 uint32_t p = inner_start;
 int depth = 0;
 while (p < end) {
  const char ch = text[p];
  if (ch == '\\' && p + 1u < end) { p += 2u; continue; }
  if (ch == '[') { ++depth; ++p; continue; }
  if (ch == ']') { if (depth == 0) { *out_close = p; return true; } --depth; ++p; continue; }
  ++p;
 }
 return false;
}

bool match_autolink_url(const char* text, uint32_t pos, uint32_t end, uint32_t* out_end) noexcept {
 const uint32_t rem = end - pos;
 bool ok_scheme = false;
 if (rem >= 8u && (!std::memcmp(text + pos, "https://", 8) || !std::memcmp(text + pos, "http://", 7))) ok_scheme = true;
 else if (rem >= 5u && !std::memcmp(text + pos, "www.", 4)) ok_scheme = true;
 if (!ok_scheme) return false;
 uint32_t p = pos;
 while (p < end && !inline_space(text[p]) && text[p] != '<' && text[p] != '>' && text[p] != '"' && text[p] != '\'') ++p;
 while (p > pos && (text[p - 1u] == '.' || text[p - 1u] == ',' || text[p - 1u] == ';' ||
                    text[p - 1u] == ':' || text[p - 1u] == '!' || text[p - 1u] == '?')) --p;
 if (p > pos && text[p - 1u] == ')') {
  int bal = 0;
  for (uint32_t k = pos; k < p; ++k) { if (text[k] == '(') ++bal; else if (text[k] == ')') --bal; }
  while (bal < 0 && p > pos && text[p - 1u] == ')') { --p; ++bal; }
 }
 if (p <= pos + 7u) return false;
 *out_end = p;
 return true;
}

bool inline_line(InlayMeasure* m, const char* text, uint32_t pos, uint32_t end,
                 uint32_t base_flags = 0, uint32_t link_url_off = 0, uint32_t link_url_len = 0) noexcept {
 const uint32_t first = m->token_count, line_start = pos;
 uint32_t opens[6], depth = 0;
 bool no_paren_math = false;
 const bool in_link = (base_flags & InlayInlineLink) != 0;
 while (pos < end) {
  const uint32_t start = pos;
  InlayInline token = {pos, 1, link_url_off, link_url_len, base_flags};
  const char c = text[pos];
  if (c == '$' && pos + 1u < end && text[pos + 1u] != '$' && !inline_space(text[pos + 1u])) {
   uint32_t close = pos + 1u;
   while (close < end) {
    if (text[close] == '\\' && close + 1u < end) { close += 2u; continue; }
    if (text[close] == '$') break;
    ++close;
   }
   const bool valid_close = close < end && close > pos + 1u && !inline_space(text[close - 1u]) &&
                            (close + 1u >= end || text[close + 1u] < '0' || text[close + 1u] > '9');
   if (valid_close) {
    token = {pos + 1u, close - pos - 1u, pos, close + 1u - pos, InlayInlineMath | base_flags};
    pos = close + 1u;
   }
  } else if (!no_paren_math && c == '\\' && pos + 1u < end && text[pos + 1u] == '(') {
   uint32_t close = pos + 2u;
   while (close + 1u < end && !(text[close] == '\\' && text[close + 1u] == ')')) ++close;
   if (close + 1u < end) {
    token = {pos + 2u, close - pos - 2u, pos, close + 2u - pos, InlayInlineMath | base_flags};
    pos = close + 2u;
   } else no_paren_math = true;
  } else if (c == '`') {
   uint32_t ticks = 1;
   while (pos + ticks < end && text[pos + ticks] == '`' && ticks < 8u) ++ticks;
   uint32_t scan = pos + ticks, match_pos = end;
   while (scan < end) {
    if (text[scan] == '`') {
     uint32_t run = 1;
     while (scan + run < end && text[scan + run] == '`') ++run;
     if (run == ticks) { match_pos = scan; break; }
     scan += run;
    } else ++scan;
   }
   if (match_pos < end) {
    uint32_t cs = pos + ticks, ce = match_pos;
    if (ticks >= 2u && ce - cs >= 3u && text[cs] == ' ' && text[ce - 1u] == ' ') {
     bool all_sp = true;
     for (uint32_t k = cs; k < ce; ++k) if (text[k] != ' ') { all_sp = false; break; }
     if (!all_sp) { ++cs; --ce; }
    }
    const uint32_t span_off = in_link ? link_url_off : pos;
    const uint32_t span_len = in_link ? link_url_len : (match_pos + ticks - pos);
    token = {cs, ce - cs, span_off, span_len, InlayInlineCode | base_flags};
    pos = match_pos + ticks;
   } else {
    token = {pos, ticks, link_url_off, link_url_len, base_flags};
    pos += ticks;
   }
  } else if (!in_link && c == '<') {
   uint32_t close = pos + 1u;
   while (close < end && text[close] != '>' && !inline_space(text[close])) ++close;
   if (close < end && text[close] == '>' && close > pos + 4u) {
    const uint32_t ulen = close - (pos + 1u);
    if ((ulen >= 8u && (!std::memcmp(text + pos + 1u, "https://", 8) || !std::memcmp(text + pos + 1u, "http://", 7))) ||
        (ulen >= 8u && !std::memcmp(text + pos + 1u, "mailto:", 7))) {
     token = {pos + 1u, ulen, pos + 1u, ulen, InlayInlineLink};
     pos = close + 1u;
    }
   }
  } else if (!in_link && (c == 'h' || c == 'w') && (pos == line_start || inline_space(text[pos - 1u]) || text[pos - 1u] == '(')) {
   uint32_t url_end = 0;
   if (match_autolink_url(text, pos, end, &url_end)) {
    token = {pos, url_end - pos, pos, url_end - pos, InlayInlineLink};
    pos = url_end;
   }
  } else if (!in_link && c == '!' && pos + 1u < end && text[pos + 1u] == '[') {
   uint32_t bracket = 0, paren = 0;
   if (scan_link_bracket(text, pos + 2u, end, &bracket) && bracket + 1u < end && text[bracket + 1u] == '(' &&
       scan_balanced_paren_url(text, bracket + 2u, end, &paren)) {
    token = {pos + 2u, bracket - (pos + 2u), bracket + 2u, paren - (bracket + 2u), InlayInlineImage};
    pos = paren + 1u;
   }
  } else if (!in_link && c == '[') {
   if (pos + 1u < end && text[pos + 1u] == '^') {
    uint32_t fn_end = pos + 2u;
    while (fn_end < end && text[fn_end] != ']' && !inline_space(text[fn_end]) && text[fn_end] != '[') ++fn_end;
    if (fn_end < end && fn_end > pos + 2u && text[fn_end] == ']') {
     token = {pos + 2u, fn_end - (pos + 2u), pos, fn_end + 1u - pos, InlayInlineFootnote};
     pos = fn_end + 1u;
    }
   }
   if (pos == start) {
    uint32_t bracket = 0, paren = 0;
    if (scan_link_bracket(text, pos + 1u, end, &bracket)) {
     bool matched_link = false;
     uint32_t url_off = 0, url_len = 0, next_pos = 0, extra_flags = 0;
     if (bracket + 1u < end && text[bracket + 1u] == '(' &&
         scan_balanced_paren_url(text, bracket + 2u, end, &paren)) {
      url_off = bracket + 2u;
      url_len = paren - url_off;
      next_pos = paren + 1u;
      matched_link = true;
     } else if (m->doc && m->doc->ref_def_count > 0u) {
      // Full [text][label], collapsed [label][], or shortcut [label] reference link!
      const InlayRefDef* ref = nullptr;
      if (bracket + 1u < end && text[bracket + 1u] == '[') {
       uint32_t rclose = 0;
       if (scan_link_bracket(text, bracket + 2u, end, &rclose)) {
        const uint32_t lbl_off = (rclose > bracket + 2u) ? (bracket + 2u) : (pos + 1u);
        const uint32_t lbl_len = (rclose > bracket + 2u) ? (rclose - (bracket + 2u)) : (bracket - (pos + 1u));
        ref = inlay_doc_find_ref_def(m->doc, text + lbl_off, lbl_len);
        if (ref) next_pos = rclose + 1u;
       }
      } else {
       ref = inlay_doc_find_ref_def(m->doc, text + pos + 1u, bracket - (pos + 1u));
       if (ref) next_pos = bracket + 1u;
      }
      if (ref) {
       url_off = ref->url_off;
       url_len = ref->url_len;
       if (ref->url_in_pool) extra_flags = InlayInlineUrlInPool;
       matched_link = true;
      }
     }
     if (matched_link) {
      bool has_nested = false;
      for (uint32_t k = pos + 1u; k < bracket; ++k) {
       if (std::strchr("`*_~=", text[k])) { has_nested = true; break; }
      }
      if (has_nested && bracket > pos + 1u) {
       const uint32_t before_nested = m->token_count;
       if (!inline_line(m, text, pos + 1u, bracket, InlayInlineLink | extra_flags, url_off, url_len)) return false;
       if (m->token_count > before_nested) { pos = next_pos; continue; }
      }
      token = {pos + 1u, bracket - pos - 1u, url_off, url_len, InlayInlineLink | extra_flags};
      pos = next_pos;
     }
    }
   }
  } else if (c == '*' || c == '_' || c == '~' || c == '=') {
   uint32_t stop = pos + 1u;
   while (stop < end && text[stop] == c) ++stop;
   const uint32_t run = stop - pos;
   const bool under = c == '_';
   const bool can_close = pos > line_start && !inline_space(text[pos - 1u]) && (!under || stop == end || !inline_alnum(text[stop]));
   const bool can_open = stop < end && !inline_space(text[stop]) && (!under || pos == line_start || !inline_alnum(text[pos - 1u]));
   if (can_close || (can_open && (((c == '*' || under) && run <= 3u) || ((c == '~' || c == '=') && run == 2u)))) {
    while (pos < stop) {
     const uint32_t left = stop - pos;
     uint32_t flag = c == '~' ? InlayInlineStrike : c == '=' ? InlayInlineHighlight : left >= 2u ? InlayInlineBold : InlayInlineEmphasis;
     uint32_t match = depth;
     if (can_close) {
      for (uint32_t i = depth; i > 0u; --i) {
       const auto& open = m->tokens[opens[i - 1u]];
       if (text[open.offset] == c && (open.length == left || (left >= 3u && open.length < left)) &&
           open.offset + open.length < start) {
        match = i - 1u;
        flag = open.flags;
        break;
       }
      }
     }
     if (match == depth && (left > 3u || ((c == '~' || c == '=') && left != 2u))) {
      if (!append_inline(m, {pos, left, link_url_off, link_url_len, base_flags})) return false;
      break;
     }
     const uint32_t width = flag == InlayInlineEmphasis ? 1u : 2u;
     uint32_t flags = 0;
     if (match < depth) {
      if ((c == '*' || under) && match + 1u < depth) {
       auto& open = m->tokens[opens[match]];
       auto& next = m->tokens[opens[match + 1u]];
       if (open.offset + open.length == next.offset && open.length + next.length == 3u && text[next.offset] == c) {
        const uint32_t outer_width = next.length;
        next.offset = open.offset + outer_width;
        next.length = open.length;
        open.length = outer_width;
        open.flags = next.flags;
        next.flags = flag;
        ++match;
       }
      }
      while (depth > match + 1u) m->tokens[opens[--depth]].flags = base_flags;
      m->tokens[opens[--depth]].flags = flag | inline_open;
      flags = flag | inline_close;
     } else if (can_open && depth < 6u) {
      bool already_open = false;
      for (uint32_t i = 0; i < depth; ++i) already_open |= m->tokens[opens[i]].flags == flag;
      if (!already_open) { opens[depth++] = m->token_count; flags = flag; }
      else flags = base_flags;
     } else {
      flags = base_flags;
     }
     if (!append_inline(m, {pos, width, link_url_off, link_url_len, flags})) return false;
     pos += width;
    }
    pos = stop;
    continue;
   }
   pos = stop;
   token.length = run;
  }
  if (pos == start) {
   if (c == '\\' && pos + 1u < end && std::strchr("\\|`*_~[$=!<", text[pos + 1u])) ++pos;
   ++pos;
   while (pos < end && !std::strchr("`[*_~$\\=!<", text[pos])) {
    if ((text[pos] == 'h' || text[pos] == 'w') && !in_link &&
        (inline_space(text[pos - 1u]) || text[pos - 1u] == '(')) {
     uint32_t dummy = 0;
     if (match_autolink_url(text, pos, end, &dummy)) break;
    }
    ++pos;
   }
   token.length = pos - start;
  }
  if (!append_inline(m, token)) return false;
 }
 while (depth) m->tokens[opens[--depth]].flags = base_flags;
 uint32_t active = 0, out = first;
 for (uint32_t i = first; i < m->token_count; ++i) {
  auto t = m->tokens[i];
  if (t.flags & inline_open) { active |= t.flags & ~inline_open; continue; }
  if (t.flags & inline_close) { active &= ~(t.flags & ~inline_close); continue; }
  if (!t.length && !(t.flags & (InlayInlineMath | InlayInlineImage))) continue;
  t.flags |= active | base_flags;
  if (out > first && !(t.flags & (InlayInlineCode | InlayInlineLink | InlayInlineMath | InlayInlineFootnote | InlayInlineImage))) {
   auto& previous = m->tokens[out - 1u];
   if (previous.flags == t.flags && previous.offset + previous.length == t.offset) {
    previous.length += t.length;
    continue;
   }
  }
  m->tokens[out++] = t;
 }
 m->token_count = out;
 return true;
}

void table_row_range(EtchStr* line, uint32_t* start) noexcept {
 while (*start < line->length && inline_space(line->chars[*start])) ++*start;
 while (line->length > *start && inline_space(line->chars[line->length - 1u])) --line->length;
 if (*start < line->length && line->chars[*start] == '|') ++*start;
}

uint32_t table_cell_end(EtchStr line, uint32_t pos) noexcept {
 while (pos < line.length && line.chars[pos] != '|') {
  if (line.chars[pos] == '\\' && pos + 1u < line.length &&
      (line.chars[pos + 1u] == '\\' || line.chars[pos + 1u] == '|')) ++pos;
  ++pos;
 }
 return pos;
}

EtchStr table_line(EtchStr source, const InlayRow& row, uint16_t quote_depth = 0) noexcept {
 auto line = slice(source, row.offset, row.length);
 if (line.length && line.chars[line.length - 1u] == '\n') --line.length;
 if (line.length && line.chars[line.length - 1u] == '\r') --line.length;
 if (quote_depth > 0u) {
  const uint32_t skip = strip_quote_prefix(line.chars, line.length, quote_depth);
  line.chars += skip;
  line.length -= skip;
 }
 return line;
}

uint32_t table_columns(EtchStr line) noexcept {
 uint32_t start = 0, count = 0;
 table_row_range(&line, &start);
 for (; start < line.length; start = table_cell_end(line, start) + 1u) ++count;
 return count;
}

EtchAlignX table_alignment(EtchStr rule, uint32_t* start) noexcept {
 if (*start >= rule.length) return ETCH_ALIGN_X_LEFT;
 const uint32_t end = table_cell_end(rule, *start);
 uint32_t a = *start, b = end;
 while (a < b && inline_space(rule.chars[a])) ++a;
 while (b > a && inline_space(rule.chars[b - 1u])) --b;
 *start = end + 1u;
 const bool right = a < b && rule.chars[b - 1u] == ':';
 return right ? (rule.chars[a] == ':' ? ETCH_ALIGN_X_CENTER : ETCH_ALIGN_X_RIGHT) : ETCH_ALIGN_X_LEFT;
}

float estimate_row(const InlayDocViewDesc& view, const InlayDocStyle& style, float width, const InlayBlock& b,
                   EtchStr text, bool first, bool last, bool diagram, uint32_t columns = 1, EtchStr img_src = {}) {
 const float padding = block_padding(view, b.kind);
 const float eff_w = std::fmax(1.f, width - block_nest_indent(b, view.font_size_px));
 if (diagram) return 200;
 if (b.kind == INLAY_MATH_BLOCK) return view.font_size_px * 4 + 32;
 if (b.kind == INLAY_THEMATIC_BREAK) return 1 + 2 * padding;
 if (b.kind == INLAY_IMAGE) {
  const float avail = std::fmax(1.f, eff_w - 2 * padding);
  const auto img = resolve_doc_image(view, text, img_src, avail);
  return (img.valid ? img.height : std::fmax(11.f, view.font_size_px * 0.85f) * 1.5f + 8.f) + 2 * padding;
 }
 const float line_scale = effective_line_scale(style);
 const float font = b.kind == INLAY_HEADING ? effective_heading_font(view, style, b.level)
  : b.kind == INLAY_FOOTNOTE ? std::fmax(10.f, view.font_size_px * 0.85f) : view.font_size_px;
 const float line_height = font * line_scale;
 if (b.kind == INLAY_TABLE) {
  const float cell_width = std::fmax(1.f, eff_w - 2 * padding) / float(columns);
  const float available = std::fmax(1.f, cell_width - 2 * view.table_cell_padding_x);
  uint32_t start = 0, longest = 0;
  table_row_range(&text, &start);
  for (uint32_t i = 0; i < columns && start < text.length; ++i) {
   const uint32_t stop = table_cell_end(text, start);
   if (stop - start > longest) longest = stop - start;
   start = stop + 1u;
  }
  const float height = std::fmax(1.f, std::ceil(float(longest) * font * .5f / available)) * line_height;
  return std::fmin(4096.f, height + 2 * view.table_cell_padding_y) + (float(first) + float(last)) * padding;
 }
 const bool code = b.kind == INLAY_CODE_BLOCK;
 const float available = std::fmax(1.f, eff_w - 2 * padding);
 float height = 0;
 for (uint32_t pos = 0; pos < text.length;) {
  uint32_t stop = pos;
  while (stop < text.length && text.chars[stop] != '\n') ++stop;
  const float lines = code ? 1.f : std::fmax(1.f, std::ceil(float(stop - pos) * font * .5f / available));
  height += lines * line_height + 4;
  pos = stop + 1u;
 }
 const bool has_code_header = code && first && (!view.hide_code_copy_button || b.language_length || view.code_header_fn);
 const float header = has_code_header
  ? std::fmax((!view.hide_code_copy_button || view.code_header_fn) ? 32.f : 0.f, b.language_length ? line_height : 0.f) + 4.f
  : (b.kind == INLAY_FOOTNOTE && first ? 8.f : 0.f);
 return std::fmax(0.f, height - 4) + 2 * padding + header;
}

float initial_row_height(const InlayMeasure& m, const InlayRow& row) {
 if (skipped_row(m, row)) return m.view.height_px == 0 ? 0.f : 1.f;
 const auto& block = m.doc->blocks[row.block];
 if (row.kind == INLAY_DETAILS && row.first && !details_body(m, row)) return 32 + 2 * m.view.block_padding_px;
 const float custom_h = custom_fence_estimate(m.view, m.doc, row.block, std::fmax(1.f, m.width - 2 * m.view.block_padding_px));
 if (custom_h > 0.f) return custom_h + 2 * m.view.block_padding_px;
 const EtchStr source = doc_source_str(m.doc);
 const EtchStr img_src = block.kind == INLAY_IMAGE ? slice(source, block.language_offset, block.language_length) : EtchStr{};
 EtchStr row_text = (block.flags & INLAY_BLOCK_FLAG_SYNTHETIC)
  ? EtchStr{m.doc->ast_text + m.doc->block_meta[row.block].pool_offset + row.offset, row.length, false}
  : table_line(source, row, block.kind == INLAY_TABLE ? block.quote_depth : 0u);
 return estimate_row(m.view, m.style, m.width, block, row_text,
                     row.first, row.last_in_block, diagram_block(m.doc, block),
                     row.kind == INLAY_TABLE ? m.rows[row.table_header].table_columns : 1u, img_src);
}

float block_height(uint32_t index, void* user) {
 const auto& m = *static_cast<InlayMeasure*>(user);
 if (m.revision != m.doc->revision || index >= m.count) return 28;
 const float height = m.heights[index];
 if (height == collapsed_height) return 0;
 return height == 0 ? m.rows[index].estimate : height;
}

bool cache_inline(InlayMeasure* m, EtchStr source) noexcept {
 const uint32_t tail = m->width == 0 ? 0u : m->parsed_tail;
 while (m->token_count && m->tokens[m->token_count - 1u].offset >= tail) --m->token_count;
 const uint32_t frozen = m->token_count;
 uint32_t low = 0, high = m->doc->block_count;
 while (low < high) {
  const uint32_t mid = low + (high - low) / 2u;
  const auto& b = m->doc->blocks[mid];
  if (b.offset + b.length <= tail) low = mid + 1u; else high = mid;
 }
 for (uint32_t i = low; i < m->doc->block_count; ++i) {
  const auto& b = m->doc->blocks[i];
  if (b.kind == INLAY_CODE_BLOCK || b.kind == INLAY_MATH_BLOCK || b.kind == INLAY_IMAGE ||
      (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC)) continue;
  const uint32_t last = b.offset + b.length;
  const uint32_t start_pos = b.kind == INLAY_FOOTNOTE ? b.content_offset : b.offset;
  for (uint32_t pos = start_pos > tail ? start_pos : tail; pos < last;) {
   uint32_t end = pos;
   while (end < last && source.chars[end] != '\n') ++end;
   uint32_t lpos = pos;
   if (b.quote_depth > 0u) lpos += strip_quote_prefix(source.chars + lpos, end - lpos, b.quote_depth);
   if (b.kind == INLAY_TABLE) {
    auto line = slice(source, lpos, end - lpos);
    uint32_t start = 0;
    table_row_range(&line, &start);
    while (start < line.length) {
     const uint32_t stop = table_cell_end(line, start);
     if (!inline_line(m, source.chars, lpos + start, lpos + stop)) { m->token_count = frozen; return false; }
     start = stop + 1u;
    }
   } else if (!inline_line(m, source.chars, lpos, end)) { m->token_count = frozen; return false; }
   pos = end < last ? end + 1u : end;
  }
 }
 return true;
}

uint32_t finish_inline_row(const InlayMeasure* m, uint32_t pos, uint32_t stop, uint32_t limit, bool decorate) noexcept {
 uint32_t low = 0, high = m->token_count;
 while (low < high) { const uint32_t mid = low + (high - low) / 2u; if (m->tokens[mid].offset <= stop) low = mid + 1u; else high = mid; }
 if (low < m->token_count) {
  const auto& t = m->tokens[low];
  if ((t.flags & InlayInlineMath) && stop > t.url_offset && stop < t.url_offset + t.url_length) return t.url_offset + t.url_length;
 }
 if (low) {
  const auto& t = m->tokens[low - 1u];
  if ((t.flags & InlayInlineMath) && stop > t.url_offset && stop < t.url_offset + t.url_length) return t.url_offset + t.url_length;
  if ((t.flags & InlayInlineFootnote) && stop > t.url_offset && stop < t.url_offset + t.url_length) return t.url_offset + t.url_length;
  const uint32_t text_end = t.offset + t.length;
  const bool link = (t.flags & InlayInlineLink) != 0, image = (t.flags & InlayInlineImage) != 0;
  if (link || image || (t.flags & InlayInlineCode)) {
   const bool inline_url = (link || image) && !(t.flags & InlayInlineUrlInPool) && t.url_offset >= text_end &&
                           t.url_offset + t.url_length + 1u <= m->doc->source_size;
   const uint32_t token_end = inline_url ? t.url_offset + t.url_length + 1u
    : ((t.flags & InlayInlineCode) && !link && t.url_length ? t.url_offset + t.url_length : text_end + 1u);
   const uint32_t token_start = image ? (t.offset >= 2u ? t.offset - 2u : 0u)
    : ((t.flags & InlayInlineCode) && !link && t.url_length ? t.url_offset : (t.offset ? t.offset - 1u : 0u));
   if (stop < token_end) {
    if (stop >= text_end || t.length + 2u <= limit) return token_end;
    if (token_start > pos) return token_start;
    if (decorate || token_end - pos <= 16384u) return token_end;
   }
  }
 }
 return stop;
}

EtchStr table_text(EtchStr text, EtchContext* ctx) {
 uint32_t first = 0;
 while (first + 1u < text.length) {
  if (text.chars[first] == '\\') {
   if (text.chars[first + 1u] == '|') break;
   if (text.chars[first + 1u] == '\\') ++first;
  }
  ++first;
 }
 if (first + 1u >= text.length) return text;
 auto* copy = static_cast<char*>(etch_frame_arena_alloc(ctx, text.length, 1));
 if (!copy) return text;
 std::memcpy(copy, text.chars, first);
 uint32_t count = first;
 for (uint32_t i = first; i < text.length; ++i) {
  if (text.chars[i] == '\\' && i + 1u < text.length) {
   if (text.chars[i + 1u] == '|') ++i;
   else if (text.chars[i + 1u] == '\\') copy[count++] = text.chars[i++];
  }
  copy[count++] = text.chars[i];
 }
 return {copy, count, false};
}

uint32_t table_text_copy(const char* src, uint32_t len, char* dst) noexcept {
 uint32_t count = 0;
 for (uint32_t i = 0; i < len; ++i) {
  if (src[i] == '\\' && i + 1u < len) {
   if (src[i + 1u] == '|') ++i;
   else if (src[i + 1u] == '\\') { if (dst) dst[count] = src[i]; ++count; ++i; }
  }
  if (dst) dst[count] = src[i];
  ++count;
 }
 return count;
}

// ---------- Unified Document Plain-Text & Selection Helpers ----------

bool push_u32(uint32_t** arr, uint32_t* count, uint32_t* cap, uint32_t val) noexcept {
 if (*count == *cap) {
  const uint32_t ncap = *cap ? *cap * 2u : 64u;
  auto* mem = static_cast<uint32_t*>(std::realloc(*arr, size_t(ncap) * sizeof(uint32_t)));
  if (!mem) return false;
  *arr = mem;
  *cap = ncap;
 }
 (*arr)[(*count)++] = val;
 return true;
}

bool id_in_list(const uint32_t* arr, uint32_t count, uint32_t id) noexcept {
 if (!id) return false;
 for (uint32_t i = 0; i < count; ++i) if (arr[i] == id) return true;
 return false;
}

void record_vis_seg(InlayMeasure* m, const InlaySelSegment& seg) noexcept {
 if (!seg.element_id || !seg.doc_len) return;
 if (m->vis_seg_count == m->vis_seg_cap) {
  const uint32_t ncap = m->vis_seg_cap ? m->vis_seg_cap * 2u : 64u;
  auto* mem = static_cast<InlaySelSegment*>(std::realloc(m->vis_segs, size_t(ncap) * sizeof(InlaySelSegment)));
  if (!mem) return;
  m->vis_segs = mem;
  m->vis_seg_cap = ncap;
 }
 m->vis_segs[m->vis_seg_count++] = seg;
}

void apply_segment_selection(const InlayMeasure* m, uint32_t line_id, uint32_t doc_start, uint32_t doc_len) noexcept {
 if (!line_id || !doc_len) return;
 if (m->doc_sel_active && m->doc_sel_anchor != m->doc_sel_focus) {
  const uint32_t lo = m->doc_sel_anchor < m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus;
  const uint32_t hi = m->doc_sel_anchor > m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus;
  const uint32_t ov_lo = lo > doc_start ? lo : doc_start;
  const uint32_t ov_hi = hi < doc_start + doc_len ? hi : doc_start + doc_len;
  if (ov_hi > ov_lo) {
   if (m->doc_sel_sync_needed || !etch_has_text_selection(line_id)) {
    etch_set_text_selection(line_id, ov_lo - doc_start, ov_hi - doc_start);
   }
   return;
  }
 }
 if (etch_has_text_selection(line_id)) {
  etch_clear_text_selection(line_id);
 }
}

uint32_t utf8_clamp(const char* s, uint32_t len, uint32_t pos) noexcept {
 if (pos >= len) return len;
 while (pos > 0u && (uint8_t(s[pos]) & 0xC0u) == 0x80u) --pos;
 return pos;
}

uint32_t utf8_next(const char* s, uint32_t len, uint32_t pos) noexcept {
 if (pos >= len) return len;
 ++pos;
 while (pos < len && (uint8_t(s[pos]) & 0xC0u) == 0x80u) ++pos;
 return pos;
}

uint32_t hit_test_seg_byte(const InlayMeasure* m, const InlaySelSegment& seg, EtchRect r, EtchVec2 ptr) noexcept {
 if (!m->plain_text || seg.doc_start + seg.doc_len > m->plain_len || !seg.doc_len) return seg.doc_start;
 const char* txt = m->plain_text + seg.doc_start;
 const uint32_t n = seg.doc_len;
 const float lx = ptr.x - r.x;
 const float ly = ptr.y - r.y;
 if (ly < 0.f) return seg.doc_start;
 if (ly >= r.h) return seg.doc_start + n;

 // Determine which visual wrapped line ly falls on.
 uint32_t line_beg = 0, line_end = n;
 const float lh = seg.line_height > 1.f ? seg.line_height : (seg.font_size * 1.4f);
 if (seg.wrap && r.h > lh * 1.45f && r.w > 1.f) {
  const uint32_t target_line = uint32_t(std::fmax(0.f, std::floor(ly / lh)));
  uint32_t cur_line = 0, p = 0;
  while (p < n) {
   uint32_t lb = p;
   uint32_t best_break = p;
   while (p < n) {
    uint32_t next_word = p;
    while (next_word < n && txt[next_word] != ' ') next_word = utf8_next(txt, n, next_word);
    uint32_t after_sp = next_word;
    while (after_sp < n && txt[after_sp] == ' ') ++after_sp;
    const float w = etch_measure_text({txt + lb, next_word - lb, false}, seg.font_id, seg.font_size, 0.f).x;
    if (w > r.w && best_break > lb) break;
    best_break = after_sp;
    p = after_sp;
    if (w > r.w) break;
   }
   if (best_break == lb) best_break = utf8_next(txt, n, lb);
   if (cur_line == target_line || best_break >= n) {
    line_beg = lb;
    line_end = best_break;
    break;
   }
   ++cur_line;
   p = best_break;
  }
 }
 if (lx <= 0.f) return seg.doc_start + line_beg;
 uint32_t best_pos = line_beg;
 float best_dist = std::fabs(lx);
 uint32_t p = line_beg;
 while (p < line_end) {
  const uint32_t next = utf8_next(txt, line_end, p);
  const float x = etch_measure_text({txt + line_beg, next - line_beg, false}, seg.font_id, seg.font_size, 0.f).x;
  const float d = std::fabs(x - lx);
  if (d < best_dist) { best_dist = d; best_pos = next; }
  if (x > lx && d > best_dist) break;
  p = next;
 }
 return seg.doc_start + best_pos;
}

uint32_t hit_test_doc_offset(const InlayMeasure* m, EtchVec2 ptr) noexcept {
 if (!m || !m->vis_seg_count) return 0u;
 int32_t exact = -1;
 int32_t best_band = -1;
 float best_band_dx = 1e30f;
 int32_t nearest_above = -1;
 float above_dist = 1e30f;
 int32_t nearest_below = -1;
 float below_dist = 1e30f;

 for (uint32_t i = 0; i < m->vis_seg_count; ++i) {
  const auto& s = m->vis_segs[i];
  const EtchRect r = etch_element_bounds(s.element_id);
  if (r.w <= 0.f || r.h <= 0.f) continue;
  if (ptr.x >= r.x && ptr.x <= r.x + r.w && ptr.y >= r.y && ptr.y <= r.y + r.h) {
   exact = int32_t(i);
   break;
  }
  if (ptr.y >= r.y && ptr.y <= r.y + r.h) {
   const float dx = ptr.x < r.x ? (r.x - ptr.x) : (ptr.x - (r.x + r.w));
   if (dx < best_band_dx) { best_band_dx = dx; best_band = int32_t(i); }
  } else if (r.y + r.h < ptr.y) {
   const float dy = ptr.y - (r.y + r.h);
   if (dy < above_dist || (std::fabs(dy - above_dist) < 1.f)) { above_dist = dy; nearest_above = int32_t(i); }
  } else {
   const float dy = r.y - ptr.y;
   if (dy < below_dist) { below_dist = dy; nearest_below = int32_t(i); }
  }
 }
 if (exact >= 0) {
  const auto& s = m->vis_segs[exact];
  return hit_test_seg_byte(m, s, etch_element_bounds(s.element_id), ptr);
 }
 if (best_band >= 0) {
  const auto& s = m->vis_segs[best_band];
  const EtchRect r = etch_element_bounds(s.element_id);
  return ptr.x <= r.x ? s.doc_start : (s.doc_start + s.doc_len);
 }
 if (nearest_above < 0 && nearest_below >= 0) {
  return m->vis_segs[nearest_below].doc_start;
 }
 if (nearest_below < 0 && nearest_above >= 0) {
  const auto& s = m->vis_segs[nearest_above];
  return s.doc_start + s.doc_len;
 }
 if (nearest_above >= 0 && nearest_below >= 0) {
  if (above_dist <= below_dist) {
   const auto& s = m->vis_segs[nearest_above];
   return s.doc_start + s.doc_len;
  }
  return m->vis_segs[nearest_below].doc_start;
 }
 return 0u;
}

bool append_plain_bytes(InlayMeasure* m, const char* s, uint32_t n) noexcept {
 if (!n) return true;
 if (m->plain_len + n > m->plain_cap) {
  uint32_t cap = m->plain_cap ? m->plain_cap : 1024u;
  while (cap < m->plain_len + n) cap *= 2u;
  auto* mem = static_cast<char*>(std::realloc(m->plain_text, cap));
  if (!mem) return false;
  m->plain_text = mem;
  m->plain_cap = cap;
 }
 std::memcpy(m->plain_text + m->plain_len, s, n);
 m->plain_len += n;
 return true;
}

uint32_t extract_inline_range_plain(const InlayMeasure* m, EtchStr source, uint32_t start, uint32_t end, bool table, char* out) noexcept {
 uint32_t low = 0, high = m->token_count;
 while (low < high) {
  const uint32_t mid = low + (high - low) / 2u;
  if (m->tokens[mid].offset + m->tokens[mid].length <= start) low = mid + 1u; else high = mid;
 }
 uint32_t wp = 0;
 for (uint32_t i = low; i < m->token_count && m->tokens[i].offset < end; ++i) {
  const auto& t = m->tokens[i];
  const uint32_t a = t.offset > start ? t.offset : start;
  const uint32_t b = t.offset + t.length < end ? t.offset + t.length : end;
  if (b <= a) continue;
  if (table) {
   wp += table_text_copy(source.chars + a, b - a, out ? out + wp : nullptr);
  } else {
   if (out) std::memcpy(out + wp, source.chars + a, b - a);
   wp += b - a;
  }
 }
 return wp;
}

bool ascii_ieq(EtchStr a, const char* lit) noexcept {
 const size_t n = std::strlen(lit);
 if (a.length != n) return false;
 for (size_t i = 0; i < n; ++i) {
  char c = a.chars[i];
  if (c >= 'A' && c <= 'Z') c = char(c + 'a' - 'A');
  if (c != lit[i]) return false;
 }
 return true;
}

EtchColor alert_accent_from_kind(uint8_t alert_kind, const EtchTheme& theme) noexcept {
 switch (alert_kind) {
  case INLAY_ALERT_CAUTION: return theme.danger;
  case INLAY_ALERT_WARNING: return theme.warning;
  case INLAY_ALERT_TIP: return theme.syntax_string;
  case INLAY_ALERT_IMPORTANT: return theme.accent_secondary.a != 0 ? theme.accent_secondary : theme.accent;
  default: return theme.accent;
 }
}

EtchColor alert_accent_color(EtchStr source, const InlayBlock& full, const EtchTheme& theme) noexcept {
 if (full.alert_kind != INLAY_ALERT_NONE) return alert_accent_from_kind(full.alert_kind, theme);
 uint32_t pos = full.content_offset, end = pos + full.content_length;
 if (pos < end && source.chars[pos] == '>') ++pos;
 if (pos < end && source.chars[pos] == ' ') ++pos;
 if (pos + 2u < end && source.chars[pos] == '[' && source.chars[pos + 1u] == '!') {
  uint32_t s = pos + 2u, e = s;
  while (e < end && source.chars[e] != ']' && source.chars[e] != '\n' && source.chars[e] != '\r') ++e;
  const EtchStr tag = slice(source, s, e - s);
  if (ascii_ieq(tag, "CAUTION")) return theme.danger;
  if (ascii_ieq(tag, "WARNING")) return theme.warning;
  if (ascii_ieq(tag, "TIP")) return theme.syntax_string;
  if (ascii_ieq(tag, "IMPORTANT")) return theme.accent_secondary.a != 0 ? theme.accent_secondary : theme.accent;
  if (ascii_ieq(tag, "NOTE")) return theme.accent;
 }
 return theme.accent;
}

// Compute trimmed list line bounds (after stripping quote prefix, indent, and list marker)
void strip_list_marker(EtchStr* line, const InlayListItemMeta* meta, bool* out_ordered, bool* out_task, EtchStr* out_marker, uint32_t* out_trim) noexcept {
 const EtchStr bullets[3] = {ETCH_STR_LIT("•"), ETCH_STR_LIT("◦"), ETCH_STR_LIT("▪")};
 uint32_t indent_bytes = 0, indent_spaces = 0;
 while (indent_bytes < line->length && indent_spaces < 32u &&
        (line->chars[indent_bytes] == ' ' || line->chars[indent_bytes] == '\t')) {
  indent_spaces += line->chars[indent_bytes] == '\t' ? 2u : 1u;
  ++indent_bytes;
 }
 line->chars += indent_bytes;
 line->length -= indent_bytes;
 const uint32_t nest = meta ? (meta->list_depth > 0u ? uint32_t(meta->list_depth - 1u) : 0u)
                            : (indent_spaces / 2u > 6u ? 6u : indent_spaces / 2u);
 EtchStr marker = bullets[nest < 2u ? nest : 2u];
 uint32_t trim = 0;
 bool ordered = false;
 bool task = false;
 if (line->length >= 6u && (line->chars[0] == '-' || line->chars[0] == '*' || line->chars[0] == '+') &&
     line->chars[1] == ' ' && line->chars[2] == '[' && line->chars[4] == ']' && line->chars[5] == ' ') {
  task = true;
  trim = 6u;
 } else if (line->length >= 2u && (line->chars[0] == '-' || line->chars[0] == '*' || line->chars[0] == '+') &&
            (line->chars[1] == ' ' || line->chars[1] == '\t')) {
  trim = 2u;
 } else {
  uint32_t digits = 0;
  while (digits < line->length && line->chars[digits] >= '0' && line->chars[digits] <= '9') ++digits;
  if (digits && digits + 1u < line->length && (line->chars[digits] == '.' || line->chars[digits] == ')') &&
      (line->chars[digits + 1u] == ' ' || line->chars[digits + 1u] == '\t')) {
   marker = {line->chars, digits + 1u, false};
   trim = digits + 2u;
   ordered = true;
   if (line->length >= trim + 4u && line->chars[trim] == '[' && line->chars[trim + 2u] == ']' && line->chars[trim + 3u] == ' ') {
    task = true;
    trim += 4u;
   }
  } else if (meta && meta->is_task) {
   task = true;
  }
 }
 if (out_ordered) *out_ordered = ordered;
 if (out_task) *out_task = task;
 if (out_marker) *out_marker = marker;
 if (out_trim) *out_trim = trim;
}

void build_plain_text_index(InlayMeasure* m) noexcept {
 m->plain_len = 0;
 const InlayDoc* doc = m->doc;
 const EtchStr source = doc_source_str(doc);
 uint32_t prev_block = UINT32_MAX;
 bool prev_had_text = false;

 for (uint32_t i = 0; i < m->count; ++i) {
  auto& row = m->rows[i];
  row.doc_text_offset = m->plain_len;
  row.doc_text_length = 0;
  if (skipped_row(*m, row)) continue;
  const auto& full_blk = doc->blocks[row.block];
  if (full_blk.kind == INLAY_THEMATIC_BREAK || full_blk.kind == INLAY_IMAGE || full_blk.kind == INLAY_MATH_BLOCK ||
      diagram_block(doc, full_blk) ||
      custom_fence_estimate(m->view, doc, row.block, std::fmax(1.f, m->width - 2 * m->view.block_padding_px)) > 0.f) {
   continue;
  }
  const bool new_block = row.block != prev_block;
  const bool sep_nl = !new_block && row.offset > 0u &&
   ((full_blk.flags & INLAY_BLOCK_FLAG_SYNTHETIC)
     ? (doc->ast_text[doc->block_meta[row.block].pool_offset + row.offset - 1u] == '\n')
     : (source.chars[row.offset - 1u] == '\n'));

  // Compute row text length first so we only insert block separators for non-empty rows!
  const uint32_t saved_len = m->plain_len;
  if (prev_had_text) {
   if (new_block) append_plain_bytes(m, "\n\n", 2u);
   else if (sep_nl) append_plain_bytes(m, "\n", 1u);
  }
  const uint32_t row_start = m->plain_len;

  if (full_blk.kind == INLAY_CODE_BLOCK) {
   const EtchStr code_src = block_code_str(doc, row.block);
   const uint32_t base_off = (full_blk.flags & INLAY_BLOCK_FLAG_SYNTHETIC) ? 0u : full_blk.content_offset;
   uint32_t pos = row.offset >= base_off ? (row.offset - base_off) : 0u;
   const uint32_t end = pos + row.length <= code_src.length ? pos + row.length : code_src.length;
   bool first_line = true;
   while (pos < end) {
    uint32_t next = pos;
    while (next < end && code_src.chars[next] != '\n') ++next;
    uint32_t ll = next - pos;
    if (ll && code_src.chars[pos + ll - 1u] == '\r') --ll;
    if (!first_line) append_plain_bytes(m, "\n", 1u);
    first_line = false;
    append_plain_bytes(m, code_src.chars + pos, ll);
    pos = next < end ? next + 1u : end;
   }
  } else if (full_blk.kind == INLAY_TABLE) {
   EtchStr line = table_line(source, row, full_blk.quote_depth);
   const uint32_t columns = m->rows[row.table_header].table_columns;
   uint32_t start = 0;
   table_row_range(&line, &start);
   for (uint32_t col = 0; col < columns && start < line.length; ++col) {
    const uint32_t stop = table_cell_end(line, start);
    if (col > 0u) append_plain_bytes(m, "\t", 1u);
    const uint32_t abs_a = uint32_t((line.chars + start) - source.chars);
    const uint32_t abs_b = uint32_t((line.chars + stop) - source.chars);
    const uint32_t need = extract_inline_range_plain(m, source, abs_a, abs_b, true, nullptr);
    if (need > 0u) {
     const uint32_t dst_off = m->plain_len;
     if (append_plain_bytes(m, source.chars, need)) {
      extract_inline_range_plain(m, source, abs_a, abs_b, true, m->plain_text + dst_off);
     }
    }
    start = stop + 1u;
   }
  } else if (full_blk.kind == INLAY_DETAILS) {
   if (m->open[row.block]) {
    const auto& range = m->details[row.block];
    const uint32_t s = row.offset > range.body ? row.offset : range.body;
    const uint32_t e = row.offset + row.length < range.end ? row.offset + row.length : range.end;
    uint32_t pos = s;
    bool first_line = true;
    while (pos < e) {
     uint32_t next = pos;
     while (next < e && source.chars[next] != '\n') ++next;
     if (!first_line) append_plain_bytes(m, "\n", 1u);
     first_line = false;
     const uint32_t need = extract_inline_range_plain(m, source, pos, next, false, nullptr);
     if (need > 0u) {
      const uint32_t dst_off = m->plain_len;
      if (append_plain_bytes(m, source.chars, need)) {
       extract_inline_range_plain(m, source, pos, next, false, m->plain_text + dst_off);
      }
     }
     pos = next < e ? next + 1u : e;
    }
   }
  } else {
   uint32_t pos = row.offset;
   const uint32_t end = pos + row.length;
   bool first_line = true;
   while (pos < end) {
    uint32_t next = pos;
    while (next < end && source.chars[next] != '\n') ++next;
    uint32_t logical = pos, logical_end = next;
    if (logical_end > logical && source.chars[logical_end - 1u] == '\r') --logical_end;
    uint32_t content_start = full_blk.offset;
    if (full_blk.quote_depth > 0u) {
     content_start += strip_quote_prefix(source.chars + content_start, source.length - content_start, full_blk.quote_depth);
    }
    if (full_blk.kind == INLAY_HEADING) content_start = full_blk.content_offset;
    if (full_blk.kind == INLAY_FOOTNOTE) content_start = full_blk.content_offset;
    const bool continuation = pos > content_start && pos > 0u && source.chars[pos - 1u] != '\n';
    if (!first_line) append_plain_bytes(m, "\n", 1u);
    first_line = false;
    EtchStr line = slice(source, logical, logical_end - logical);
    if (!continuation && full_blk.quote_depth > 0u) {
     const uint32_t qskip = strip_quote_prefix(line.chars, line.length, full_blk.quote_depth);
     line.chars += qskip;
     line.length -= qskip;
    }
    if (full_blk.kind == INLAY_ALERT && !continuation && line.length >= 3u && line.chars[0] == '[' && line.chars[1] == '!') {
     // Alert header tag (not emitted as selectable rich_text, skip in plain text or keep?)
     // Keep consistent with etch_text (non-selectable label) -> 0 selectable bytes!
     pos = next < end ? next + 1u : end;
     continue;
    }
    if ((full_blk.kind == INLAY_LIST || full_blk.kind == INLAY_TASK_LIST) && !continuation) {
     const InlayListItemMeta* meta = (row.list_item_index != UINT32_MAX && row.list_item_index < doc->list_item_count)
      ? &doc->list_items[row.list_item_index] : nullptr;
     uint32_t trim = 0;
     strip_list_marker(&line, meta, nullptr, nullptr, nullptr, &trim);
     line.chars += trim;
     line.length -= trim;
    }
    const uint32_t off = uint32_t(line.chars - source.chars);
    const uint32_t r_start = pos > off ? pos : off;
    const uint32_t r_end = logical_end < off + line.length ? logical_end : (off + line.length);
    if (r_end > r_start) {
     const uint32_t need = extract_inline_range_plain(m, source, r_start, r_end, false, nullptr);
     if (need > 0u) {
      const uint32_t dst_off = m->plain_len;
      if (append_plain_bytes(m, source.chars, need)) {
       extract_inline_range_plain(m, source, r_start, r_end, false, m->plain_text + dst_off);
      }
     }
    }
    pos = next < end ? next + 1u : end;
   }
  }

  if (m->plain_len > row_start) {
   row.doc_text_offset = row_start;
   row.doc_text_length = m->plain_len - row_start;
   prev_block = row.block;
   prev_had_text = true;
  } else {
   m->plain_len = saved_len;
   row.doc_text_offset = saved_len;
   row.doc_text_length = 0;
  }
 }
}

struct InlineRowBox {
 const EtchBoxDesc* pending;
 bool suppressed;
};

void begin_inline_row(InlineRowBox& row) {
 if (row.pending) {
  etch_begin_box(row.pending);
  row.pending = nullptr;
 }
}

float rich_line(InlayMeasure* m, EtchStr line, uint32_t base_id, float font, InlayDocResult* result, const EtchTheme& theme,
                const InlayDocStyle& style, uint32_t range_start, uint32_t range_end, float width, bool estimate, InlineRowBox& row_box,
                uint32_t row_index, uint32_t block_index, uint32_t* io_doc_cursor,
                bool table = false, bool header = false, EtchAlignX align = ETCH_ALIGN_X_LEFT, EtchColor color_override = {}) {
 const EtchStr source = doc_source_str(m->doc);
 const uint32_t offset = uint32_t(line.chars - source.chars);
 const uint32_t start = offset + range_start, end = offset + (range_end < line.length ? range_end : line.length);
 uint32_t low = 0, high = m->token_count;
 while (low < high) {
  const uint32_t mid = low + (high - low) / 2u;
  const auto& t = m->tokens[mid];
  if (t.offset + t.length <= start) low = mid + 1u; else high = mid;
 }
 uint32_t count = 0;
 for (uint32_t i = low; i < m->token_count && m->tokens[i].offset < end; ++i) ++count;
 if (!count) return 0;
 auto* ctx = etch_current_context();
 bool math = false, has_image = false;
 const uint16_t sans_font = effective_sans_font(style);
 const uint16_t bold_font = effective_bold_font(style);
 const uint16_t mono_font = effective_mono_font(style);
 const uint16_t base_font = header ? (style.bold_font_id != ETCH_INVALID_FONT ? style.bold_font_id : static_cast<uint16_t>(ETCH_FONT_SEMIBOLD)) : sans_font;
 float line_height = font * effective_line_scale(style);
 auto* spans = static_cast<EtchTextSpan*>(etch_frame_arena_alloc(ctx, size_t(count) * sizeof(EtchTextSpan), alignof(EtchTextSpan)));
 if (!spans) return 0;
 uint32_t emitted = count;
 uint32_t total_text_bytes = 0;
 for (uint32_t i = 0; i < count; ++i) {
  const auto& t = m->tokens[low + i];
  auto span = etch_text_span_default();
  span.font_size_px = font;
  const uint32_t a = t.offset > start ? t.offset : start, b = t.offset + t.length < end ? t.offset + t.length : end;
  span.text = {source.chars + a, b - a, false};
  if (table) span.text = table_text(span.text, ctx);
  total_text_bytes += span.text.length;
  if (t.flags & InlayInlineEmphasis) span.font_id = ETCH_FONT_ITALIC;
  if (t.flags & InlayInlineBold) span.font_id = bold_font;
  span.strikethrough = (t.flags & InlayInlineStrike) != 0;
  if (t.flags & InlayInlineHighlight) {
   span.background = style.highlight_bg.a != 0 ? style.highlight_bg : EtchColor{theme.accent.r, theme.accent.g, theme.accent.b, 56};
   span.padding_x = 4;
   span.bg_radius = theme.radius_small_px;
  }
  if (t.flags & InlayInlineCode) {
   span.font_id = mono_font;
   span.font_size_px = effective_code_font_size(font, style);
   span.background = style.code_bg.a != 0 ? style.code_bg : theme.surface_subtle;
   if (style.code_border.a != 0) span.border_color = style.code_border;
   span.color = theme.text_code;
   span.padding_x = 4;
   span.padding_y = 1;
   span.bg_radius = theme.radius_small_px;
  }
  if (t.flags & InlayInlineFootnote) {
   const EtchColor fn_col = style.link_color.a != 0 ? style.link_color : theme.accent;
   span.color = fn_col;
   span.background = {fn_col.r, fn_col.g, fn_col.b, 32};
   span.border_color = {fn_col.r, fn_col.g, fn_col.b, 96};
   span.font_size_px = std::fmax(10.f, font * 0.78f);
   span.padding_x = 4;
   span.padding_y = 1;
   span.bg_radius = theme.radius_small_px;
   span.id = etch_id_index(base_id, t.offset + 1u);
   line_height = std::fmax(line_height, etch_font_metrics(base_font, span.font_size_px).line_height + 2 * span.padding_y);
   if (span.id) {
    push_u32(&m->vis_span_ids, &m->vis_span_count, &m->vis_span_cap, span.id);
    if (etch_hovered(span.id)) etch_set_cursor(ETCH_CURSOR_HAND);
    if (etch_clicked(span.id)) result->clicked_link = slice(source, t.url_offset, t.url_length);
   }
  }
  if (t.flags & (InlayInlineLink | InlayInlineCode)) {
   const bool link = (t.flags & InlayInlineLink) != 0;
   EtchStr url = {};
   if (link) {
    url = (t.flags & InlayInlineUrlInPool)
     ? EtchStr{m->doc->ast_text + t.url_offset, t.url_length, false}
     : slice(source, t.url_offset, t.url_length);
    span.color = style.link_color.a != 0 ? style.link_color : theme.accent;
    span.underline = true;
    span.id = etch_id_index(base_id, t.offset + 1u);
    if (span.id) {
     push_u32(&m->vis_span_ids, &m->vis_span_count, &m->vis_span_cap, span.id);
     if (etch_hovered(span.id)) etch_set_cursor(ETCH_CURSOR_HAND);
     if (url.length) etch_set_tooltip_text(span.id, url);
    }
   }
   EtchStr clicked_text = span.text;
   if (m->view.inline_decorator_fn) {
    const bool continuation = a > t.offset;
    const bool split = continuation || b < t.offset + t.length;
    const EtchStr full_text = split ? slice(source, t.offset, t.length) : span.text;
    auto decorated = span;
    decorated.text = full_text;
    if (m->view.inline_decorator_fn(link ? INLAY_INLINE_LINK : INLAY_INLINE_CODE, full_text, url,
                                    &decorated, m->view.inline_decorator_user_data)) {
     const bool replaced = decorated.text.chars != full_text.chars || decorated.text.length != full_text.length;
     if (replaced && continuation) {
      spans[i] = etch_text_span_default();
      --emitted;
      continue;
     }
     clicked_text = decorated.text;
     if (!replaced && split) decorated.text = span.text;
     if (continuation) decorated.icon_id = ETCH_INVALID_ICON;
     span = decorated;
     const float size = span.font_size_px > 0 ? span.font_size_px : font;
     const uint16_t face = span.font_id == ETCH_INVALID_FONT ? base_font : span.font_id;
     const float icon = span.icon_id == ETCH_INVALID_ICON ? 0.f : (span.icon_size_px > 0 ? span.icon_size_px : size * .9f);
     line_height = std::fmax(line_height, std::fmax(etch_font_metrics(face, size).line_height, icon) + 2 * std::fmax(0.f, span.padding_y));
    }
   }
   if (span.id) {
    push_u32(&m->vis_span_ids, &m->vis_span_count, &m->vis_span_cap, span.id);
    if (etch_clicked(span.id)) result->clicked_link = link ? url : clicked_text;
   }
  }
  math |= (t.flags & InlayInlineMath) != 0;
  has_image |= (t.flags & InlayInlineImage) != 0;
  spans[i] = span;
 }
 if (!emitted) { row_box.suppressed = true; return 0; }
 begin_inline_row(row_box);
 auto desc = etch_rich_text_desc_default();
 desc.id = base_id;
 desc.selectable = true;
 desc.font_id = base_font;
 desc.font_size_px = font;
 desc.line_height_px = line_height;
 desc.align_x = align;
 if (color_override.a != 0) desc.color = color_override;
 else if (style.text_color.a != 0) desc.color = style.text_color;
 if (style.selection_color.a != 0) desc.selection_color = style.selection_color;

 if (math || has_image) {
  const auto text_fm = etch_font_metrics(base_font, font);
  const float text_base = text_fm.ascent;
  const float text_desc = text_fm.descent;
  auto* preps = math ? static_cast<InlayMathPrepared*>(etch_frame_arena_alloc(ctx, size_t(count) * sizeof(InlayMathPrepared), alignof(InlayMathPrepared))) : nullptr;
  auto* math_copies = math ? static_cast<EtchStr*>(etch_frame_arena_alloc(ctx, size_t(count) * sizeof(EtchStr), alignof(EtchStr))) : nullptr;
  float max_ascent = text_base, max_descent = text_desc;
  if (preps && math_copies) {
   for (uint32_t i = 0; i < count; ++i) {
    preps[i] = {};
    math_copies[i] = {};
    if (!(m->tokens[low + i].flags & InlayInlineMath)) continue;
    const EtchStr src_m = spans[i].text;
    char* cpy = src_m.length ? static_cast<char*>(etch_frame_arena_alloc(ctx, src_m.length, 1)) : nullptr;
    if (cpy) std::memcpy(cpy, src_m.chars, src_m.length);
    math_copies[i] = {cpy, cpy ? src_m.length : 0u, false};
    if (inlay_math_prepare(math_copies[i], font, false, &preps[i])) {
     max_ascent = std::fmax(max_ascent, preps[i].metrics.baseline);
     max_descent = std::fmax(max_descent, std::fmax(0.f, preps[i].metrics.height - preps[i].metrics.baseline));
    }
   }
  }
  const float total_line_h = max_ascent + max_descent;
  auto row = etch_box_desc_default();
  row.width = etch_sizing_fixed(width);
  row.wrap = true;
  row.align_x = align;
  row.align_y = ETCH_ALIGN_Y_TOP;
  row.gap = 4;
  etch_begin_box(&row);
  for (uint32_t i = 0; i < count; ++i) {
   desc.id = etch_id_index(base_id, i + 1u);
   const auto& t = m->tokens[low + i];
   const uint32_t tok_len = spans[i].text.length;
   const uint32_t seg_start = io_doc_cursor ? *io_doc_cursor : 0u;
   if (t.flags & InlayInlineMath) {
    const bool ok_prep = preps && preps[i].metrics.valid;
    const float top_pad = ok_prep ? std::fmax(0.f, max_ascent - preps[i].metrics.baseline) : 0.f;
    native_box(ok_prep ? math_copies[i] : spans[i].text, font, false, desc.id, width,
               desc.color.a != 0 && !etch_color_is_theme_auto(desc.color) ? desc.color : EtchColor{},
               false, ok_prep ? &preps[i] : nullptr, top_pad);
   } else if (t.flags & InlayInlineImage) {
    const EtchStr alt = slice(source, t.offset, t.length);
    const EtchStr src = slice(source, t.url_offset, t.url_length);
    const auto img = resolve_doc_image(m->view, alt, src, width);
    const float img_h = img.valid ? img.height : std::fmax(11.f, font * 0.85f) * 1.5f + 8.f;
    const float top_pad = std::fmax(0.f, (total_line_h - img_h) * 0.5f);
    if (top_pad > 0.5f) { auto wrap = etch_box_desc_default(); wrap.padding = {0, 0, top_pad, 0}; etch_begin_box(&wrap); }
    if (img.valid) {
     auto idesc = etch_image_desc_default();
     idesc.id = desc.id;
     idesc.texture = img.texture;
     idesc.width = etch_sizing_fixed(img.width);
     idesc.height = etch_sizing_fixed(img.height);
     idesc.radius = etch_corners_all(img.radius);
     etch_image(&idesc);
    } else draw_image_fallback(alt, desc.id, font, style, theme);
    if (top_pad > 0.5f) etch_end_box();
   } else {
    const float top_pad = std::fmax(0.f, max_ascent - text_base);
    if (top_pad > 0.5f) { auto wrap = etch_box_desc_default(); wrap.padding = {0, 0, top_pad, 0}; etch_begin_box(&wrap); }
    etch_text_spans(spans + i, 1, &desc);
    if (io_doc_cursor) {
     apply_segment_selection(m, desc.id, seg_start, tok_len);
     record_vis_seg(m, {desc.id, row_index, block_index, seg_start, tok_len, base_font, 0, font, line_height, true});
    }
    if (top_pad > 0.5f) etch_end_box();
   }
   if (io_doc_cursor) *io_doc_cursor += tok_len;
  }
  etch_end_box();
 } else {
  const uint32_t seg_start = io_doc_cursor ? *io_doc_cursor : 0u;
  etch_text_spans(spans, count, &desc);
  if (io_doc_cursor) {
   apply_segment_selection(m, desc.id, seg_start, total_text_bytes);
   record_vis_seg(m, {desc.id, row_index, block_index, seg_start, total_text_bytes, base_font, 0, font, line_height, true});
   *io_doc_cursor += total_text_bytes;
  }
 }
 if (!estimate) return 0;
 float advance = 0, extra_height = 0;
 for (uint32_t i = 0; i < count; ++i) {
  const auto& t = m->tokens[low + i];
  if (t.flags & InlayInlineImage) {
   const EtchStr alt = slice(source, t.offset, t.length);
   const EtchStr src = slice(source, t.url_offset, t.url_length);
   const auto img = resolve_doc_image(m->view, alt, src, width);
   if (img.valid) { advance += img.width + 4; extra_height = std::fmax(extra_height, img.height); }
   else advance += etch_measure_text(alt.length ? alt : ETCH_STR_LIT("image"), sans_font, std::fmax(11.f, font * 0.85f), 0).x + 24;
   continue;
  }
  const auto& span = spans[i];
  const float size = span.font_size_px > 0 ? span.font_size_px : font;
  const uint16_t face = span.font_id == ETCH_INVALID_FONT ? base_font : span.font_id;
  advance += etch_measure_text(span.text, face, size, span.letter_spacing).x + 2 * std::fmax(0.f, span.padding_x);
  if (span.icon_id != ETCH_INVALID_ICON) {
   advance += span.icon_size_px > 0 ? span.icon_size_px : size * .9f;
   if (span.text.length) advance += std::fmax(0.f, span.icon_gap_px);
  }
 }
 return std::fmax(extra_height, std::fmax(1.f, std::ceil(advance / width)) * line_height);
}

bool is_code_ident_start(char c) noexcept {
 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_code_ident_char(char c) noexcept {
 return is_code_ident_start(c) || (c >= '0' && c <= '9');
}
bool is_code_keyword(const char* s, uint32_t len, bool sql_mode) noexcept {
 static constexpr const char* kWords[] = {
  "const","let","var","mut","fn","func","function","def","class","struct","enum","union",
  "trait","impl","interface","type","typedef","namespace","using","import","export","from","as",
  "pub","public","private","protected","static","extern","inline","constexpr","consteval","virtual","override",
  "final","noexcept","template","typename","concept","requires","auto","void","bool","char","int","long","short",
  "float","double","unsigned","signed","size_t","uint8_t","uint16_t","uint32_t","uint64_t",
  "int32_t","int64_t","usize","isize","u8","u16","u32","u64","i32","i64","f32","f64","str",
  "string","number","boolean","any","unknown","never","readonly","declare","extends","implements","abstract",
  "keyof","typeof","infer","satisfies","if","else","elif","for","while","loop","do","switch","match","case",
  "default","break","continue","return","yield","async","await","co_await","co_return","co_yield",
  "try","catch","finally","throw","raise","with","in","of","is","not","and","or","true","false",
  "null","nullptr","nil","None","True","False","self","this","super","new","delete","sizeof",
  "alignof","where","unsafe","move","ref","pass","lambda","package","module","local","then","fi","done",
  "defer","chan","go","range","select","map","export","echo","set","unset","exit","cmake_minimum_required",
  "project","add_executable","add_library","target_link_libraries","target_include_directories","find_package","option"
 };
 for (const char* word : kWords) {
  if (std::strlen(word) == len && !std::memcmp(word, s, len)) return true;
 }
 if (sql_mode) {
  static constexpr const char* kSql[] = {
   "SELECT","FROM","WHERE","JOIN","LEFT","RIGHT","INNER","OUTER","ON","GROUP","ORDER","BY",
   "HAVING","LIMIT","OFFSET","INSERT","INTO","VALUES","UPDATE","SET","DELETE","CREATE","ALTER",
   "DROP","TABLE","INDEX","VIEW","PRIMARY","KEY","FOREIGN","REFERENCES","NOT","NULL","UNIQUE",
   "AS","AND","OR","IN","IS","LIKE","DISTINCT","UNION","ALL","CASE","WHEN","THEN","ELSE","END"
  };
  for (const char* word : kSql) {
   if (std::strlen(word) == len) {
    bool eq = true;
    for (uint32_t k = 0; k < len; ++k) {
     char ch = s[k];
     if (ch >= 'a' && ch <= 'z') ch = char(ch - ('a' - 'A'));
     if (ch != word[k]) { eq = false; break; }
    }
    if (eq) return true;
   }
  }
 }
 return false;
}
bool code_supports_block_comment(EtchStr language) noexcept {
 return !(ascii_ieq(language, "diff") || ascii_ieq(language, "patch") || ascii_ieq(language, "sh") ||
          ascii_ieq(language, "bash") || ascii_ieq(language, "zsh") || ascii_ieq(language, "shell") ||
          ascii_ieq(language, "yaml") || ascii_ieq(language, "yml") || ascii_ieq(language, "toml") ||
          ascii_ieq(language, "python") || ascii_ieq(language, "py"));
}
bool code_supports_triple_string(EtchStr language) noexcept {
 return ascii_ieq(language, "python") || ascii_ieq(language, "py") || ascii_ieq(language, "toml");
}
uint8_t advance_code_lex_state(EtchStr line, EtchStr language, uint8_t state) noexcept {
 if (ascii_ieq(language, "diff") || ascii_ieq(language, "patch")) return 0;
 const bool blk_cm = code_supports_block_comment(language);
 const bool tri_str = code_supports_triple_string(language);
 if (!blk_cm && !tri_str) return 0;
 uint32_t pos = 0;
 while (pos < line.length) {
  if (state == 1) {
   while (pos + 1u < line.length && !(line.chars[pos] == '*' && line.chars[pos + 1u] == '/')) ++pos;
   if (pos + 1u < line.length) { pos += 2u; state = 0; continue; }
   return 1;
  }
  if (state == 2 || state == 3) {
   const char q = state == 2 ? '"' : '\'';
   while (pos < line.length) {
    if (line.chars[pos] == '\\' && pos + 1u < line.length) { pos += 2u; continue; }
    if (pos + 2u < line.length && line.chars[pos] == q && line.chars[pos + 1u] == q && line.chars[pos + 2u] == q) { pos += 3u; state = 0; break; }
    ++pos;
   }
   if (state != 0) return state;
   continue;
  }
  const char c = line.chars[pos];
  if (c == '/' && pos + 1u < line.length && line.chars[pos + 1u] == '/') return 0;
  if (blk_cm && c == '/' && pos + 1u < line.length && line.chars[pos + 1u] == '*') {
   pos += 2u;
   while (pos + 1u < line.length && !(line.chars[pos] == '*' && line.chars[pos + 1u] == '/')) ++pos;
   if (pos + 1u < line.length) pos += 2u; else return 1;
   continue;
  }
  if (tri_str && (c == '"' || c == '\'') && pos + 2u < line.length && line.chars[pos + 1u] == c && line.chars[pos + 2u] == c) {
   pos += 3u;
   const uint8_t target = c == '"' ? 2u : 3u;
   while (pos < line.length) {
    if (line.chars[pos] == '\\' && pos + 1u < line.length) { pos += 2u; continue; }
    if (pos + 2u < line.length && line.chars[pos] == c && line.chars[pos + 1u] == c && line.chars[pos + 2u] == c) { pos += 3u; break; }
    ++pos;
   }
   if (pos >= line.length) {
    if (!(pos >= 6u && line.chars[pos - 1u] == c && line.chars[pos - 2u] == c && line.chars[pos - 3u] == c)) return target;
   }
   continue;
  }
  if (c == '"' || c == '\'' || c == '`') {
   ++pos;
   while (pos < line.length) { const char nx = line.chars[pos++]; if (nx == '\\' && pos < line.length) ++pos; else if (nx == c) break; }
   continue;
  }
  ++pos;
 }
 return state;
}

void code_line(InlayMeasure* m, EtchStr line, EtchStr language, uint32_t id, float font, const EtchTheme& theme,
               const InlayDocStyle& style, uint32_t row_index, uint32_t block_index, uint32_t* io_doc_cursor,
               bool wrap_lines = false, uint8_t* io_lex_state = nullptr) {
 const uint16_t mono_font = effective_mono_font(style);
 const float code_font = effective_code_font_size(font, style);
 const float line_height = code_font * effective_line_scale(style);
 if (!line.length) {
  auto spacer = etch_box_desc_default();
  spacer.height = etch_sizing_fixed(line_height);
  etch_box(&spacer);
  return;
 }
 const uint32_t seg_start = io_doc_cursor ? *io_doc_cursor : 0u;
 if (ascii_ieq(language, "diff") || ascii_ieq(language, "patch")) {
  auto span = etch_text_span_default();
  span.font_id = mono_font;
  span.font_size_px = code_font;
  span.text = line;
  span.color = theme.text_code;
  if (line.length >= 2u && line.chars[0] == '@' && line.chars[1] == '@') span.color = theme.accent;
  else if (line.length >= 3u && ((line.chars[0] == '+' && line.chars[1] == '+' && line.chars[2] == '+') ||
                                 (line.chars[0] == '-' && line.chars[1] == '-' && line.chars[2] == '-'))) span.color = theme.syntax_keyword;
  else if (line.chars[0] == '+') span.color = theme.syntax_string;
  else if (line.chars[0] == '-') span.color = theme.danger;
  auto desc = etch_rich_text_desc_default();
  desc.id = id;
  desc.selectable = true;
  desc.font_id = mono_font;
  desc.font_size_px = code_font;
  desc.line_height_px = line_height;
  desc.wrap = wrap_lines;
  if (style.selection_color.a != 0) desc.selection_color = style.selection_color;
  etch_text_spans(&span, 1, &desc);
  if (io_doc_cursor) {
   apply_segment_selection(m, id, seg_start, line.length);
   record_vis_seg(m, {id, row_index, block_index, seg_start, line.length, mono_font, 0, code_font, line_height, wrap_lines});
   *io_doc_cursor += line.length;
  }
  return;
 }
 const bool c_like = ascii_ieq(language, "c") || ascii_ieq(language, "cpp") || ascii_ieq(language, "c++") ||
                     ascii_ieq(language, "cxx") || ascii_ieq(language, "cc") || ascii_ieq(language, "h") ||
                     ascii_ieq(language, "hpp") || ascii_ieq(language, "hlsl") || ascii_ieq(language, "glsl");
 const bool shell_like = ascii_ieq(language, "sh") || ascii_ieq(language, "bash") || ascii_ieq(language, "zsh") ||
                         ascii_ieq(language, "shell") || ascii_ieq(language, "ps1") || ascii_ieq(language, "powershell");
 const bool json_like = ascii_ieq(language, "json") || ascii_ieq(language, "jsonc");
 const bool yaml_like = ascii_ieq(language, "yaml") || ascii_ieq(language, "yml") || ascii_ieq(language, "toml");
 const bool sql_like = ascii_ieq(language, "sql");
 const bool blk_cm = code_supports_block_comment(language);
 const bool tri_str = code_supports_triple_string(language);
 const uint8_t entry_state = io_lex_state ? *io_lex_state : 0u;
 uint8_t end_state = entry_state;
 const EtchColor type_col = theme.accent_secondary.a != 0 ? theme.accent_secondary : theme.accent;
 EtchTextSpan* spans = nullptr;
 uint32_t count = 0, pos = 0;
 for (uint32_t pass = 0; pass < 2u; ++pass) {
  count = 0;
  pos = 0;
  uint8_t state = entry_state;
  while (pos < line.length) {
   uint32_t start = pos;
   auto span = etch_text_span_default();
   span.font_id = mono_font;
   span.font_size_px = code_font;
   span.color = theme.text_code;
   if (state == 1) {
    while (pos + 1u < line.length && !(line.chars[pos] == '*' && line.chars[pos + 1u] == '/')) ++pos;
    if (pos + 1u < line.length) { pos += 2u; state = 0; } else pos = line.length;
    span.color = theme.syntax_comment;
    span.text = {line.chars + start, pos - start, false};
    if (spans) spans[count] = span;
    ++count;
    continue;
   }
   if (state == 2 || state == 3) {
    const char q = state == 2 ? '"' : '\'';
    while (pos < line.length) {
     if (line.chars[pos] == '\\' && pos + 1u < line.length) { pos += 2u; continue; }
     if (pos + 2u < line.length && line.chars[pos] == q && line.chars[pos + 1u] == q && line.chars[pos + 2u] == q) { pos += 3u; state = 0; break; }
     ++pos;
    }
    span.color = theme.syntax_string;
    span.text = {line.chars + start, pos - start, false};
    if (spans) spans[count] = span;
    ++count;
    continue;
   }
   const char c = line.chars[pos];
   if (uint8_t(c) >= 0x80u) { ++pos; while (pos < line.length && (uint8_t(line.chars[pos]) & 0xc0u) == 0x80u) ++pos; }
   else if ((c == '/' && pos + 1u < line.length && line.chars[pos + 1u] == '/') ||
            (c == '-' && pos + 1u < line.length && line.chars[pos + 1u] == '-' && (pos + 2u == line.length || line.chars[pos + 2u] == ' ' || line.chars[pos + 2u] == '-'))) {
    pos = line.length;
    span.color = theme.syntax_comment;
   } else if (blk_cm && c == '/' && pos + 1u < line.length && line.chars[pos + 1u] == '*') {
    pos += 2u;
    while (pos + 1u < line.length && !(line.chars[pos] == '*' && line.chars[pos + 1u] == '/')) ++pos;
    if (pos + 1u < line.length) pos += 2u; else { pos = line.length; state = 1; }
    span.color = theme.syntax_comment;
   } else if (c == '#') {
    if (c_like) { ++pos; while (pos < line.length && is_code_ident_char(line.chars[pos])) ++pos; span.color = theme.syntax_keyword; }
    else if (pos + 1u < line.length && line.chars[pos + 1u] == '[') { ++pos; span.color = theme.accent; }
    else { pos = line.length; span.color = theme.syntax_comment; }
   } else if (c == '@' && pos + 1u < line.length && is_code_ident_start(line.chars[pos + 1u])) {
    ++pos;
    while (pos < line.length && is_code_ident_char(line.chars[pos])) ++pos;
    span.color = theme.accent;
   } else if (shell_like && c == '$' && pos + 1u < line.length && (is_code_ident_start(line.chars[pos + 1u]) || line.chars[pos + 1u] == '{')) {
    ++pos;
    if (line.chars[pos] == '{') { ++pos; while (pos < line.length && line.chars[pos] != '}') ++pos; if (pos < line.length) ++pos; }
    else { while (pos < line.length && is_code_ident_char(line.chars[pos])) ++pos; }
    span.color = theme.syntax_number;
   } else if (shell_like && c == '-' && pos + 1u < line.length && (line.chars[pos + 1u] == '-' || is_code_ident_start(line.chars[pos + 1u])) &&
              (start == 0 || line.chars[start - 1u] == ' ')) {
    ++pos;
    while (pos < line.length && (is_code_ident_char(line.chars[pos]) || line.chars[pos] == '-')) ++pos;
    span.color = type_col;
   } else if (tri_str && (c == '"' || c == '\'') && pos + 2u < line.length && line.chars[pos + 1u] == c && line.chars[pos + 2u] == c) {
    pos += 3u;
    bool closed = false;
    while (pos < line.length) {
     if (line.chars[pos] == '\\' && pos + 1u < line.length) { pos += 2u; continue; }
     if (pos + 2u < line.length && line.chars[pos] == c && line.chars[pos + 1u] == c && line.chars[pos + 2u] == c) { pos += 3u; closed = true; break; }
     ++pos;
    }
    if (!closed) state = c == '"' ? 2u : 3u;
    span.color = theme.syntax_string;
   } else if (c == '"' || c == '\'' || c == '`') {
    ++pos;
    while (pos < line.length) { const char next = line.chars[pos++]; if (next == '\\' && pos < line.length) ++pos; else if (next == c) break; }
    uint32_t peek = pos;
    while (peek < line.length && line.chars[peek] == ' ') ++peek;
    span.color = (json_like && c == '"' && peek < line.length && line.chars[peek] == ':') ? type_col : theme.syntax_string;
   } else if (c >= '0' && c <= '9') {
    ++pos;
    if (c == '0' && pos < line.length && (line.chars[pos] == 'x' || line.chars[pos] == 'X' || line.chars[pos] == 'b' || line.chars[pos] == 'B')) {
     ++pos;
     while (pos < line.length && ((line.chars[pos] >= '0' && line.chars[pos] <= '9') || (line.chars[pos] >= 'a' && line.chars[pos] <= 'f') ||
                                  (line.chars[pos] >= 'A' && line.chars[pos] <= 'F') || line.chars[pos] == '_')) ++pos;
    } else {
     while (pos < line.length && ((line.chars[pos] >= '0' && line.chars[pos] <= '9') || line.chars[pos] == '.' || line.chars[pos] == '_')) ++pos;
    }
    while (pos < line.length && (line.chars[pos] == 'u' || line.chars[pos] == 'U' || line.chars[pos] == 'l' || line.chars[pos] == 'L' || line.chars[pos] == 'f' || line.chars[pos] == 'F')) ++pos;
    span.color = theme.syntax_number;
   } else if (is_code_ident_start(c)) {
    ++pos;
    while (pos < line.length && (is_code_ident_char(line.chars[pos]) || (yaml_like && line.chars[pos] == '-'))) ++pos;
    const uint32_t tok_len = pos - start;
    uint32_t peek = pos;
    while (peek < line.length && line.chars[peek] == ' ') ++peek;
    if (yaml_like && peek < line.length && line.chars[peek] == ':') span.color = type_col;
    else if (is_code_keyword(line.chars + start, tok_len, sql_like)) span.color = theme.syntax_keyword;
    else if (c >= 'A' && c <= 'Z' && tok_len > 1u) {
     bool has_lower = false;
     for (uint32_t k = start + 1u; k < pos; ++k) if (line.chars[k] >= 'a' && line.chars[k] <= 'z') { has_lower = true; break; }
     if (has_lower) span.color = type_col;
    }
   } else {
    ++pos;
   }
   span.text = {line.chars + start, pos - start, false};
   if (spans) spans[count] = span;
   ++count;
  }
  end_state = state;
  if (pass == 0) {
   spans = static_cast<EtchTextSpan*>(etch_frame_arena_alloc(etch_current_context(), size_t(count) * sizeof(EtchTextSpan), alignof(EtchTextSpan)));
   if (!spans) return;
  }
 }
 if (io_lex_state) *io_lex_state = end_state;
 auto desc = etch_rich_text_desc_default();
 desc.id = id;
 desc.selectable = true;
 desc.font_id = mono_font;
 desc.font_size_px = code_font;
 desc.line_height_px = line_height;
 desc.wrap = wrap_lines;
 if (style.selection_color.a != 0) desc.selection_color = style.selection_color;
 etch_text_spans(spans, count, &desc);
 if (io_doc_cursor) {
  apply_segment_selection(m, id, seg_start, line.length);
  record_vis_seg(m, {id, row_index, block_index, seg_start, line.length, mono_font, 0, code_font, line_height, wrap_lines});
  *io_doc_cursor += line.length;
 }
}

void compute_table_col_widths(const InlayMeasure* measure, EtchStr source, const InlayRow& info, uint32_t columns,
                              float width, float font, const InlayDocStyle& style, const InlayDocViewDesc& view,
                              float* out_widths) noexcept {
 if (!columns) return;
 if (columns == 1u) { out_widths[0] = width; return; }
 constexpr uint32_t kMaxAutoCols = 32;
 if (columns > kMaxAutoCols) {
  const float eq = width / float(columns);
  for (uint32_t c = 0; c < columns; ++c) out_widths[c] = eq;
  return;
 }
 float natural[kMaxAutoCols] = {};
 const uint16_t sans_fid = effective_sans_font(style);
 const uint16_t bold_fid = effective_bold_font(style);
 const float min_col = std::fmin(52.f, width / float(columns));
 const float max_col = std::fmax(min_col, width * 0.62f);
 for (uint32_t c = 0; c < columns; ++c) natural[c] = min_col;
 const uint16_t qd = measure->doc->blocks[info.block].quote_depth;
 uint32_t scanned = 0;
 for (uint32_t r = info.table_header; r < measure->count && measure->rows[r].block == info.block && scanned < 32u; ++r) {
  const auto& row = measure->rows[r];
  if (row.separator) continue;
  ++scanned;
  EtchStr rstr = table_line(source, row, qd);
  uint32_t cstart = 0;
  table_row_range(&rstr, &cstart);
  const float cf = row.first ? std::fmax(11.f, font * 0.88f) : font;
  const uint16_t fid = row.first ? bold_fid : sans_fid;
  for (uint32_t c = 0; c < columns && cstart < rstr.length; ++c) {
   const uint32_t cstop = table_cell_end(rstr, cstart);
   char buf[256];
   uint32_t blen = 0;
   for (uint32_t k = cstart; k < cstop && blen < 254u; ++k) {
    const char ch = rstr.chars[k];
    if (ch == '`' || ch == '*' || ch == '$') continue;
    if (ch == '\\' && k + 1u < cstop && ((rstr.chars[k + 1u] >= 'a' && rstr.chars[k + 1u] <= 'z') || (rstr.chars[k + 1u] >= 'A' && rstr.chars[k + 1u] <= 'Z'))) {
     ++k;
     while (k + 1u < cstop && ((rstr.chars[k + 1u] >= 'a' && rstr.chars[k + 1u] <= 'z') || (rstr.chars[k + 1u] >= 'A' && rstr.chars[k + 1u] <= 'Z'))) ++k;
     buf[blen++] = ' ';
     continue;
    }
    if (ch == '{' || ch == '}') continue;
    buf[blen++] = ch;
   }
   while (blen > 0u && buf[blen - 1u] == ' ') --blen;
   uint32_t b0 = 0;
   while (b0 < blen && buf[b0] == ' ') ++b0;
   const float tw = blen > b0 ? etch_measure_text({buf + b0, blen - b0, false}, fid, cf, 0.f).x : 0.f;
   const float cw = std::fmin(max_col, std::fmax(min_col, tw + 2.f * view.table_cell_padding_x + 16.f));
   natural[c] = std::fmax(natural[c], cw);
   cstart = cstop + 1u;
  }
 }
 float sum = 0.f;
 for (uint32_t c = 0; c < columns; ++c) sum += natural[c];
 if (sum <= 0.f) {
  const float eq = width / float(columns);
  for (uint32_t c = 0; c < columns; ++c) out_widths[c] = eq;
  return;
 }
 float used = 0.f;
 for (uint32_t c = 0; c < columns; ++c) {
  const float w = (c + 1u == columns) ? std::fmax(1.f, width - used) : std::fmax(min_col, width * (natural[c] / sum));
  out_widths[c] = w;
  used += w;
 }
}

void get_table_col_widths(InlayMeasure* measure, EtchStr source, const InlayRow& info, uint32_t columns,
                          float width, float font, const InlayDocStyle& style, const InlayDocViewDesc& view,
                          float* out_widths) noexcept {
 if (!columns) return;
 if (columns > 32u) { compute_table_col_widths(measure, source, info, columns, width, font, style, view, out_widths); return; }
 for (uint32_t i = 0; i < measure->table_cols_count; ++i) {
  if (measure->table_cols[i].header_row == info.table_header && measure->table_cols[i].columns == columns) {
   std::memcpy(out_widths, measure->table_cols[i].widths, size_t(columns) * sizeof(float));
   return;
  }
 }
 compute_table_col_widths(measure, source, info, columns, width, font, style, view, out_widths);
 if (measure->table_cols_count == measure->table_cols_capacity) {
  const uint32_t cap = measure->table_cols_capacity ? measure->table_cols_capacity * 2u : 16u;
  auto* mem = static_cast<InlayTableCols*>(std::realloc(measure->table_cols, size_t(cap) * sizeof(InlayTableCols)));
  if (!mem) return;
  measure->table_cols = mem;
  measure->table_cols_capacity = cap;
 }
 auto& entry = measure->table_cols[measure->table_cols_count++];
 entry.header_row = info.table_header;
 entry.columns = columns;
 std::memcpy(entry.widths, out_widths, size_t(columns) * sizeof(float));
}

float block_lines(InlayMeasure* measure, const InlayBlock& b, EtchStr source, uint32_t id, float font,
                  InlayDocResult* result, const EtchTheme& theme, const InlayDocStyle& style, const InlayRow& info,
                  uint32_t row_index, float avail_width, bool estimate = false,
                  const EtchBoxDesc* pending_box = nullptr, PendingTaskToggle* toggle_out = nullptr, EtchColor color_override = {},
                  bool wrap_code = false, uint32_t code_start_line = 0, float code_gutter_w = 0.f, float code_row_w = 0.f) {
 const auto& view = measure->view;
 const float padding = block_padding(view, b.kind);
 const float width = avail_width > 0.f ? avail_width : std::fmax(1.f, measure->width - 2 * padding);
 float height = 0;
 InlineRowBox row_box = {pending_box, false};
 uint8_t lex_state = info.code_lex_state;
 uint32_t doc_cursor = info.doc_text_offset;

 if (b.kind == INLAY_CODE_BLOCK) {
  const EtchStr code_src = block_code_str(measure->doc, info.block);
  const auto& full_blk = measure->doc->blocks[info.block];
  const uint32_t base_off = (full_blk.flags & INLAY_BLOCK_FLAG_SYNTHETIC) ? 0u : full_blk.content_offset;
  uint32_t pos = info.offset >= base_off ? (info.offset - base_off) : 0u;
  const uint32_t end = pos + info.length <= code_src.length ? pos + info.length : code_src.length;
  uint32_t line_index = 0;
  const EtchStr lang = slice(source, full_blk.language_offset, full_blk.language_length);
  while (pos < end) {
   uint32_t next = pos;
   while (next < end && code_src.chars[next] != '\n') ++next;
   uint32_t ll = next - pos;
   if (ll && code_src.chars[pos + ll - 1u] == '\r') --ll;
   if (line_index > 0u) ++doc_cursor; // '\n' separator between lines in same code row
   const EtchStr line = {code_src.chars + pos, ll, false};
   const uint32_t line_id = etch_id_index(id, ++line_index);
   if (wrap_code && code_gutter_w > 0.f) {
    const float code_font = effective_code_font_size(font, style);
    const float line_h = code_font * effective_line_scale(style);
    auto wrow = etch_box_desc_default();
    wrow.axis = ETCH_AXIS_ROW;
    wrow.width = etch_sizing_fixed(std::fmax(1.f, code_row_w));
    wrow.height = etch_sizing_fit(0, 0);
    wrow.gap = 10;
    wrow.align_y = ETCH_ALIGN_Y_TOP;
    etch_begin_box(&wrow);
    auto gbox = etch_box_desc_default();
    gbox.width = etch_sizing_fixed(code_gutter_w);
    gbox.height = etch_sizing_fit(0, 0);
    gbox.padding = {0, 8, 0, 0};
    gbox.align_x = ETCH_ALIGN_X_RIGHT;
    gbox.border_color = style.code_border.a != 0 ? style.code_border : theme.border_subtle;
    gbox.border_sides = {0, 1, 0, 0};
    etch_begin_box(&gbox);
    char nb[16];
    const int n = std::snprintf(nb, sizeof(nb), "%u", code_start_line + line_index - 1u);
    if (n > 0) {
     char* arena_s = static_cast<char*>(etch_frame_arena_alloc(measure->doc->context, size_t(n), 1));
     if (arena_s) {
      std::memcpy(arena_s, nb, size_t(n));
      auto lnd = etch_text_desc_default();
      lnd.font_id = effective_mono_font(style);
      lnd.font_size_px = std::fmax(10.f, code_font * 0.92f);
      lnd.line_height_px = line_h;
      lnd.color = style.muted_color.a != 0 ? style.muted_color : theme.text_muted;
      lnd.wrap = false;
      etch_text({arena_s, uint32_t(n), false}, &lnd);
     }
    }
    etch_end_box();
    auto cbox = etch_box_desc_default();
    cbox.width = etch_sizing_grow(0, 0);
    cbox.height = etch_sizing_fit(0, 0);
    cbox.shrink_width = true;
    etch_begin_box(&cbox);
    code_line(measure, line, lang, line_id, font, theme, style, row_index, info.block, &doc_cursor, true, &lex_state);
    etch_end_box();
    etch_end_box();
   } else {
    code_line(measure, line, lang, line_id, font, theme, style, row_index, info.block, &doc_cursor, wrap_code, &lex_state);
   }
   pos = next < end ? next + 1u : end;
  }
  return 0.f;
 }

 uint32_t pos = b.content_offset, end = pos + b.content_length, line_index = 0;
 bool emitted_plain_line = false;
 while (pos < end) {
  uint32_t next = pos;
  while (next < end && source.chars[next] != '\n') ++next;
  uint32_t logical = pos, logical_end = next;
  if (logical_end > logical && source.chars[logical_end - 1u] == '\r') --logical_end;
  const auto& full_blk = measure->doc->blocks[info.block];
  uint32_t content_start = full_blk.offset;
  if (full_blk.quote_depth > 0u) {
   content_start += strip_quote_prefix(source.chars + content_start, source.length - content_start, full_blk.quote_depth);
  }
  if (b.kind == INLAY_HEADING || b.kind == INLAY_FOOTNOTE) content_start = full_blk.content_offset;
  const bool continuation = pos > content_start && pos > 0u && source.chars[pos - 1u] != '\n';
  EtchStr line = slice(source, logical, logical_end - logical);
  const uint32_t line_id = etch_id_index(id, ++line_index);
  const auto render_rich = [&](EtchStr text, float avail_override = 0.f) {
   if (emitted_plain_line) ++doc_cursor;
   emitted_plain_line = true;
   const uint32_t offset = uint32_t(text.chars - source.chars);
   const float available = avail_override > 0.f ? avail_override : (b.kind == INLAY_LIST || b.kind == INLAY_TASK_LIST ? std::fmax(1.f, width - 8 - std::fmax(14.f, font)) : width);
   const float line_height = rich_line(measure, text, line_id, font, result, theme, style,
                                       pos > offset ? pos - offset : 0u, logical_end > offset ? logical_end - offset : 0u,
                                       available, estimate, row_box, row_index, info.block, &doc_cursor,
                                       false, false, ETCH_ALIGN_X_LEFT, color_override);
   if (line_height > 0) height += (height > 0 ? 4.f : 0.f) + line_height;
  };

  if (b.kind == INLAY_TABLE) {
   const auto& header = measure->rows[info.table_header];
   const uint32_t columns = header.table_columns;
   if (full_blk.quote_depth > 0u) {
    const uint32_t qskip = strip_quote_prefix(line.chars, line.length, full_blk.quote_depth);
    line.chars += qskip;
    line.length -= qskip;
   }
   EtchStr rule = {};
   uint32_t rule_start = 0;
   if (info.table_header + 1u < measure->count && measure->rows[info.table_header + 1u].separator &&
       measure->rows[info.table_header + 1u].block == info.block) {
    rule = table_line(source, measure->rows[info.table_header + 1u], full_blk.quote_depth);
    table_row_range(&rule, &rule_start);
   }
   auto row = etch_box_desc_default();
   row.id = line_id;
   row.width = etch_sizing_fixed(width);
   row.height = etch_sizing_fit(0, 4096);
   row.scroll_y = true;
   row.clip_children = true;
   const EtchColor hdr_bg = style.table_header_bg.a != 0 ? style.table_header_bg : theme.bg_elevated;
   row.background = info.first ? hdr_bg : info.zebra
    ? etch_color_blend_over(theme.bg_table, theme.zebra_row) : theme.bg_table;
   row.border_color = style.table_border.a != 0 ? style.table_border : theme.border_subtle;
   row.border_sides = {1, 1, info.first ? 1.f : 0.f, 1};
   const float radius = view.table_radius_px;
   const float top = info.first ? radius : 0.f, bottom = info.last_in_block ? radius : 0.f;
   row.radius = {top, top, bottom, bottom};
   etch_begin_box(&row);
   uint32_t start = 0;
   table_row_range(&line, &start);
   float col_widths_stack[32] = {};
   float* col_widths = columns <= 32u ? col_widths_stack : nullptr;
   if (col_widths) get_table_col_widths(measure, source, info, columns, width, font, style, view, col_widths);
   const float eq_width = width / float(columns);
   float tallest = 0;
   for (uint32_t column = 0; column < columns; ++column) {
    const float cell_width = col_widths ? col_widths[column] : eq_width;
    const float pad_x = std::fmin(view.table_cell_padding_x, cell_width * .5f);
    const auto align = table_alignment(rule, &rule_start);
    auto cell = etch_box_desc_default();
    cell.width = etch_sizing_fixed(cell_width);
    cell.axis = ETCH_AXIS_COL;
    cell.align_x = align;
    cell.height = etch_sizing_fit(0, 0);
    cell.shrink_width = true;
    cell.padding = etch_padding_xy(pad_x, view.table_cell_padding_y);
    etch_begin_box(&cell);
    if (start < line.length) {
     if (column > 0u) ++doc_cursor; // '\t' separator between cells
     const uint32_t stop = table_cell_end(line, start);
     const float cell_font = info.first ? std::fmax(11.f, font * 0.88f) : font;
     const EtchColor cell_col = (info.first && style.muted_color.a != 0) ? style.muted_color : color_override;
     const float cell_height = rich_line(measure, {line.chars + start, stop - start, false}, etch_id_index(line_id, column + 1u),
                                         cell_font, result, theme, style, 0, stop - start, std::fmax(1.f, cell_width - 2 * pad_x),
                                         estimate, row_box, row_index, info.block, &doc_cursor, true, info.first, align, cell_col);
     tallest = std::fmax(tallest, cell_height);
     start = stop + 1u;
    }
    etch_end_box();
   }
   etch_end_box();
   if (estimate) return std::fmin(4096.f, tallest + 2 * view.table_cell_padding_y) +
    (float(info.first) + float(info.last_in_block)) * padding;
  } else {
   bool is_alert_tag = false;
   if (!continuation && full_blk.quote_depth > 0u) {
    const uint32_t qskip = strip_quote_prefix(line.chars, line.length, full_blk.quote_depth);
    line.chars += qskip;
    line.length -= qskip;
   }
   if (b.kind == INLAY_ALERT && !continuation && line.length >= 3u && line.chars[0] == '[' && line.chars[1] == '!') {
    line.chars += 2;
    line.length -= 2;
    if (line.length && line.chars[line.length - 1u] == ']') --line.length;
    is_alert_tag = true;
   }
   if (is_alert_tag) {
    begin_inline_row(row_box);
    auto ad = etch_text_desc_default();
    ad.font_id = effective_bold_font(style);
    ad.font_size_px = std::fmax(11.f, font * 0.86f);
    ad.line_height_px = ad.font_size_px * 1.3f;
    ad.color = alert_accent_color(source, full_blk, theme);
    ad.wrap = false;
    etch_text(line, &ad);
    if (estimate) height += (height > 0 ? 4.f : 0.f) + ad.line_height_px;
   } else if ((b.kind == INLAY_LIST || b.kind == INLAY_TASK_LIST) && !continuation) {
    begin_inline_row(row_box);
    const InlayListItemMeta* meta = (info.list_item_index != UINT32_MAX && info.list_item_index < measure->doc->list_item_count)
     ? &measure->doc->list_items[info.list_item_index] : nullptr;
    uint32_t indent_spaces = 0;
    for (uint32_t k = 0; k < line.length && indent_spaces < 32u && (line.chars[k] == ' ' || line.chars[k] == '\t'); ++k) {
     indent_spaces += line.chars[k] == '\t' ? 2u : 1u;
    }
    const uint32_t nest_level = meta ? (meta->list_depth > 0u ? uint32_t(meta->list_depth - 1u) : 0u)
                                     : (indent_spaces / 2u > 6u ? 6u : indent_spaces / 2u);
    const float nest_pad = float(nest_level) * std::fmax(16.f, font * 1.1f);
    auto row = etch_box_desc_default();
    row.width = etch_sizing_fixed(width);
    row.gap = 8;
    row.align_y = ETCH_ALIGN_Y_TOP;
    row.shrink_width = true;
    if (nest_pad > 0.f) row.padding = {nest_pad, 0, 0, 0};
    etch_begin_box(&row);

    bool ordered = false, task = false;
    EtchStr marker = {};
    uint32_t trim = 0;
    strip_list_marker(&line, meta, &ordered, &task, &marker, &trim);
    const float marker_w = ordered ? std::fmax(20.f, font * 1.4f) : std::fmax(14.f, font);

    if (pos == logical) {
     if (task) {
      const uint32_t check_offset = (meta && meta->task_mark_offset != UINT32_MAX)
       ? meta->task_mark_offset
       : uint32_t((line.chars + (trim >= 3u ? trim - 3u : 3u)) - source.chars);
      bool checked = check_offset < source.length ? (source.chars[check_offset] != ' ') : (meta && meta->checked != 0);
      auto cb = etch_checkbox_desc_default();
      cb.id = etch_id_index(line_id, 0x5441u);
      push_u32(&measure->vis_ctrl_ids, &measure->vis_ctrl_count, &measure->vis_ctrl_cap, cb.id);
      cb.label = {};
      cb.checked = &checked;
      cb.box_size_px = std::fmax(14.f, font * 0.95f);
      cb.font_size_px = font;
      cb.font_id = effective_sans_font(style);
      if (style.link_color.a != 0) cb.accent = style.link_color;
      if (etch_checkbox(&cb) && toggle_out) {
       uint32_t item_index = 0;
       const auto& bm = measure->doc->block_meta[info.block];
       if (meta && bm.list_item_count > 0u) {
        for (uint32_t k = bm.list_item_first; k < info.list_item_index && k < bm.list_item_first + bm.list_item_count; ++k) {
         if (measure->doc->list_items[k].is_task) ++item_index;
        }
       }
       *toggle_out = {check_offset, info.block, item_index, checked, true};
      }
     } else {
      if (ordered) {
       auto mbox = etch_box_desc_default();
       mbox.width = etch_sizing_fit(marker_w, 0);
       mbox.align_x = ETCH_ALIGN_X_RIGHT;
       etch_begin_box(&mbox);
      }
      auto md = etch_text_desc_default();
      md.font_size_px = font;
      md.font_id = effective_sans_font(style);
      md.wrap = false;
      md.line_height_px = font * effective_line_scale(style);
      md.color = style.muted_color.a != 0 ? style.muted_color : (style.text_color.a != 0 ? style.text_color : theme.text_muted);
      etch_text(marker, &md);
      if (ordered) etch_end_box();
     }
    }
    auto item_body = etch_box_desc_default();
    item_body.axis = ETCH_AXIS_COL;
    item_body.width = etch_sizing_grow(0, 0);
    item_body.height = etch_sizing_fit(0, 0);
    item_body.shrink_width = true;
    etch_begin_box(&item_body);
    line.chars += trim;
    line.length -= trim;
    render_rich(line, std::fmax(1.f, width - nest_pad - 8.f - marker_w));
    etch_end_box();
    etch_end_box();
   } else {
    render_rich(line);
   }
  }
  pos = next < end ? next + 1u : end;
 }
 if (pending_box) {
  if (row_box.pending && row_box.suppressed) return collapsed_height;
  begin_inline_row(row_box);
  etch_end_box();
 }
 return estimate ? height + 2 * padding : 0.f;
}

uint32_t row_end(const InlayMeasure* m, const InlayDocViewDesc& view, float row_width, const InlayBlock& b, uint32_t block, uint32_t pos, uint32_t limit, bool decorate) noexcept {
 const auto* doc = m->doc;
 if (b.kind == INLAY_CODE_BLOCK && (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC)) {
  const auto& meta = doc->block_meta[block];
  const uint32_t end = meta.pool_length;
  if (diagram_block(doc, b) || custom_fence_estimate(view, doc, block, std::fmax(1.f, row_width - 2 * view.block_padding_px)) > 0.f) return end;
  uint32_t stop = pos;
  for (uint32_t line = 0; line < 16u && stop < end; ++line) {
   while (stop < end && doc->ast_text[meta.pool_offset + stop] != '\n') ++stop;
   if (stop < end) ++stop;
  }
  return stop;
 }
 const uint32_t end = b.content_offset + b.content_length;
 if ((b.kind == INLAY_DETAILS && !m->open[block]) || b.kind == INLAY_MATH_BLOCK || b.kind == INLAY_IMAGE || b.kind == INLAY_FOOTNOTE ||
     diagram_block(doc, b) || custom_fence_estimate(view, doc, block, std::fmax(1.f, row_width - 2 * view.block_padding_px)) > 0.f) return end;
 uint32_t stop = pos;
 if (b.kind == INLAY_CODE_BLOCK) {
  for (uint32_t line = 0; line < 16u && stop < end; ++line) {
   while (stop < end && doc->source[stop] != '\n') ++stop;
   if (stop < end) ++stop;
  }
  return stop;
 }
 while (stop < end && doc->source[stop] != '\n' && (b.kind == INLAY_TABLE || stop - pos < limit)) ++stop;
 while (stop < end && (uint8_t(doc->source[stop]) & 0xc0u) == 0x80u) ++stop;
 if (b.kind != INLAY_TABLE && stop < end) stop = finish_inline_row(m, pos, stop, limit, decorate);
 if (stop < end && doc->source[stop] == '\n') ++stop;
 return stop;
}

uint32_t find_list_item_for_offset(const InlayDoc* doc, uint32_t block_idx, uint32_t off) noexcept {
 const auto& bm = doc->block_meta[block_idx];
 for (uint32_t k = bm.list_item_first; k < bm.list_item_first + bm.list_item_count && k < doc->list_item_count; ++k) {
  const auto& li = doc->list_items[k];
  if (off >= li.line_offset && off < li.line_offset + li.line_length) return k;
 }
 return UINT32_MAX;
}

bool prepare_view(InlayDoc* doc, InlayMeasure* measure, const InlayDocViewDesc& view, float row_width, bool reflow) {
 if (doc->block_count > measure->open_capacity) {
  uint32_t cap = measure->open_capacity ? measure->open_capacity : 64u;
  while (cap < doc->block_count) cap *= 2u;
  auto* ranges = static_cast<InlayDetails*>(std::realloc(measure->details, size_t(cap) * sizeof(InlayDetails)));
  if (!ranges) return false;
  measure->details = ranges;
  auto* data = static_cast<bool*>(std::realloc(measure->open, cap * sizeof(bool)));
  if (!data) return false;
  measure->open = data;
  auto* wraps = static_cast<bool*>(std::realloc(measure->code_wrap, cap * sizeof(bool)));
  if (!wraps) return false;
  measure->code_wrap = wraps;
  auto* scx = static_cast<float*>(std::realloc(measure->code_scroll_x, size_t(cap) * sizeof(float)));
  if (!scx) return false;
  for (uint32_t i = measure->open_capacity; i < cap; ++i) { data[i] = true; wraps[i] = false; scx[i] = 0.f; }
  measure->code_scroll_x = scx;
  measure->open_capacity = cap;
 }
 if (measure->width == 0) {
  for (uint32_t i = 0; i < measure->open_capacity; ++i) { measure->open[i] = true; measure->code_wrap[i] = false; measure->code_scroll_x[i] = 0.f; }
  measure->doc_sel_active = false;
  measure->doc_sel_dragging = false;
  measure->doc_sel_pending_span = false;
 }
 measure->table_cols_count = 0;
 measure->doc = doc;
 const EtchStr source = doc_source_str(doc);
 for (uint32_t block = measure->width == 0 ? 0u : measure->frozen_count; block < doc->block_count; ++block) {
  if (doc->blocks[block].kind == INLAY_DETAILS) {
   auto& r = measure->details[block];
   details_range(source, doc->blocks[block], &r.summary, &r.summary_end, &r.body, &r.end);
  }
 }
 if (!cache_inline(measure, source)) return false;
 const uint32_t limit = uint32_t(std::fmin(2048.f, std::fmax(32.f, (row_width - 48) / (view.font_size_px + 12) * 24)));
 const bool decorate = view.inline_decorator_fn != nullptr;
 uint32_t needed = 0;
 for (uint32_t block = 0; block < doc->block_count; ++block) {
  const auto& b = doc->blocks[block];
  const bool synth = (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC) != 0;
  uint32_t pos = synth ? 0u : b.content_offset;
  const uint32_t end = synth ? doc->block_meta[block].pool_length : (pos + b.content_length);
  do {
   const uint32_t stop = row_end(measure, view, row_width, b, block, pos, limit, decorate);
   ++needed;
   pos = stop;
  } while (pos < end);
 }
 if (needed > measure->capacity) {
  uint32_t cap = measure->capacity ? measure->capacity : 64u;
  while (cap < needed) cap *= 2u;
  auto* rows = static_cast<InlayRow*>(std::malloc(size_t(cap) * sizeof(InlayRow)));
  auto* heights = static_cast<float*>(std::calloc(cap, sizeof(float)));
  if (!rows || !heights) { std::free(rows); std::free(heights); return false; }
  if (measure->count) {
   std::memcpy(rows, measure->rows, size_t(measure->count) * sizeof(InlayRow));
   std::memcpy(heights, measure->heights, size_t(measure->count) * sizeof(float));
  }
  std::free(measure->rows);
  std::free(measure->heights);
  measure->rows = rows;
  measure->heights = heights;
  measure->capacity = cap;
 }
 uint32_t row = 0;
 for (uint32_t block = 0; block < doc->block_count; ++block) {
  const auto& b = doc->blocks[block];
  const bool synth = (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC) != 0;
  uint32_t pos = synth ? 0u : b.content_offset;
  const uint32_t end = synth ? doc->block_meta[block].pool_length : (pos + b.content_length);
  const uint32_t header = row;
  uint32_t last = row;
  bool zebra = false, need_separator = b.kind == INLAY_TABLE;
  const bool is_code = b.kind == INLAY_CODE_BLOCK && !diagram_block(doc, b);
  const EtchStr lang = is_code ? slice(source, b.language_offset, b.language_length) : EtchStr{};
  const EtchStr code_src = is_code ? block_code_str(doc, block) : EtchStr{};
  const uint32_t code_base = synth ? 0u : b.content_offset;
  uint32_t code_line_num = 1, max_line_off = 0, max_line_len = 0;
  uint8_t lex_st = 0;
  do {
   const uint32_t stop = row_end(measure, view, row_width, b, block, pos, limit, decorate);
   const bool first = pos == (synth ? 0u : b.content_offset);
   bool separator = false;
   if (need_separator && !first) {
    const uint32_t qskip = b.quote_depth ? strip_quote_prefix(source.chars + pos, stop - pos, b.quote_depth) : 0u;
    separator = inlay_table_rule(source.chars + pos + qskip, (stop - pos) - qskip);
   }
   const uint32_t li_idx = (b.kind == INLAY_LIST || b.kind == INLAY_TASK_LIST) ? find_list_item_for_offset(doc, block, pos) : UINT32_MAX;
   InlayRow next = {block, pos, stop - pos, header, 0, code_line_num, 1, 0, 0, li_idx, 0.f, 0.f, 0.f, lex_st, first, zebra, separator, false, b.kind};
   if (is_code) {
    uint32_t lp = pos >= code_base ? (pos - code_base) : 0u;
    const uint32_t le = stop >= code_base ? (stop - code_base) : 0u;
    while (lp < le && lp < code_src.length) {
     uint32_t ln = lp;
     while (ln < le && ln < code_src.length && code_src.chars[ln] != '\n') ++ln;
     if (ln - lp > max_line_len) { max_line_off = lp; max_line_len = ln - lp; }
     lex_st = advance_code_lex_state({code_src.chars + lp, ln - lp, false}, lang, lex_st);
     ++code_line_num;
     lp = ln < le ? ln + 1u : le;
    }
   }
   if (first && b.kind == INLAY_TABLE) {
    const uint32_t columns = table_columns(table_line(source, next, b.quote_depth));
    next.table_columns = columns ? columns : 1u;
   }
   if (!first) need_separator = false;
   if (!separator) { last = row; if (!first) zebra = !zebra; }
   const bool same = !reflow && block < measure->frozen_count && row < measure->count &&
    measure->rows[row].block == block && measure->rows[row].offset == pos &&
    measure->rows[row].length == stop - pos && measure->rows[row].kind == b.kind;
   if (same) { next.estimate = measure->rows[row].estimate; next.last_scroll_x = measure->rows[row].last_scroll_x; }
   else measure->heights[row] = 0;
   measure->rows[row++] = next;
   pos = stop;
  } while (pos < end);
  if (is_code) {
   const uint32_t total_lines = code_line_num > 1u ? code_line_num - 1u : 1u;
   float min_w = 0.f;
   if (row - header > 1u && max_line_len > 0u) {
    const InlayDocStyle rstyle = resolved_style(view);
    min_w = std::ceil(etch_measure_text({code_src.chars + max_line_off, max_line_len, false},
                                        effective_mono_font(rstyle),
                                        effective_code_font_size(view.font_size_px, rstyle), 0.f).x);
   }
   for (uint32_t r = header; r < row; ++r) {
    measure->rows[r].code_total_lines = total_lines;
    measure->rows[r].code_min_width = min_w;
   }
  }
  measure->rows[last].last_in_block = true;
 }
 measure->count = row;
 measure->frozen_count = doc->frozen_count;
 measure->id = view.id;
 measure->width = row_width;
 measure->font = view.font_size_px;
 measure->revision = doc->revision;
 measure->parsed_tail = doc->tail_start;
 measure->style = resolved_style(view);
 measure->view = view;
 measure->view.style = &measure->style;
 build_plain_text_index(measure);
 if (measure->doc_sel_anchor > measure->plain_len) measure->doc_sel_anchor = measure->plain_len;
 if (measure->doc_sel_focus > measure->plain_len) measure->doc_sel_focus = measure->plain_len;
 measure->total_height = 0;
 for (uint32_t i = 0; i < measure->count; ++i) {
  if (measure->rows[i].estimate == 0) measure->rows[i].estimate = initial_row_height(*measure, measure->rows[i]);
  measure->total_height += block_height(i, measure);
  if (view.height_px == 0 && i && measure->rows[i].first) measure->total_height += view.block_gap_px;
 }
 return true;
}

bool valid_dimensions(uint32_t id, float width, float font) noexcept {
 return id && std::isfinite(width) && width >= 32 && std::isfinite(font) && font >= 6 && font <= 128;
}
bool valid_spacing(float value) noexcept { return std::isfinite(value) && value >= 0; }
bool valid_style(const InlayDocStyle* s) noexcept {
 if (!s) return true;
 if (!std::isfinite(s->line_height_scale) || s->line_height_scale < 0.f || s->line_height_scale > 10.f) return false;
 if (!std::isfinite(s->code_font_scale) || s->code_font_scale < 0.f || s->code_font_scale > 4.f) return false;
 for (float hs : s->heading_scale) if (!std::isfinite(hs) || hs < 0.f || hs > 10.f) return false;
 return true;
}
bool style_reflow_changed(const InlayDocStyle& a, const InlayDocStyle& b) noexcept {
 if (a.font_id != b.font_id || a.bold_font_id != b.bold_font_id || a.mono_font_id != b.mono_font_id ||
     a.line_height_scale != b.line_height_scale || a.code_font_scale != b.code_font_scale) return true;
 for (int i = 0; i < 6; ++i) if (a.heading_scale[i] != b.heading_scale[i]) return true;
 return false;
}

InlayMeasure* find_view(const InlayDoc* doc, uint32_t view_id) noexcept {
 if (!doc || !view_id) return nullptr;
 for (uint32_t i = 0; i < doc->view_count; ++i) if (doc->views[i].id == view_id) return &doc->views[i];
 return nullptr;
}

InlayMeasure* find_or_add_view(InlayDoc* doc, uint32_t view_id) noexcept {
 if (auto* existing = find_view(doc, view_id)) return existing;
 if (doc->view_count == doc->view_capacity) {
  const uint32_t cap = doc->view_capacity ? doc->view_capacity * 2u : 4u;
  auto* mem = static_cast<InlayMeasure*>(std::realloc(doc->views, size_t(cap) * sizeof(InlayMeasure)));
  if (!mem) return nullptr;
  std::memset(mem + doc->view_capacity, 0, size_t(cap - doc->view_capacity) * sizeof(InlayMeasure));
  doc->views = mem;
  doc->view_capacity = cap;
 }
 auto* slot = &doc->views[doc->view_count++];
 *slot = {};
 return slot;
}

void update_doc_selection(InlayDoc* doc, InlayMeasure* m, const InlayDocViewDesc& view) noexcept {
 const uint32_t prev_anchor = m->doc_sel_anchor;
 const uint32_t prev_focus = m->doc_sel_focus;
 const bool prev_active = m->doc_sel_active;
 if (m->doc_sel_anchor > m->plain_len) m->doc_sel_anchor = m->plain_len;
 if (m->doc_sel_focus > m->plain_len) m->doc_sel_focus = m->plain_len;
 const uint32_t act = etch_active_id();
 const EtchVec2 ptr = etch_pointer_pos();
 const EtchRect vb = etch_element_bounds(view.id);
 const bool ptr_in_view = vb.w > 0.f && vb.h > 0.f && ptr.x >= vb.x && ptr.x <= vb.x + vb.w && ptr.y >= vb.y && ptr.y <= vb.y + vb.h;
 const bool view_hovered = etch_hovered(view.id) || ptr_in_view;

 // 1. Escape clears selection.
 if (m->doc_sel_active && etch_key_pressed(27u, false)) {
  m->doc_sel_active = false;
  m->doc_sel_dragging = false;
  m->doc_sel_pending_span = false;
  etch_request_redraw(doc->context);
 }

 // 2. Ctrl+A selects the entire unified document.
 const uint32_t foc = etch_focused_id();
 const bool can_select_all = (foc == view.id) || m->doc_sel_active || view_hovered || (foc == 0u && doc->view_count <= 1u);
 if (can_select_all && m->plain_len > 0u && etch_consume_shortcut(ETCH_MOD_PRIMARY, 'a', false)) {
  m->doc_sel_anchor = 0u;
  m->doc_sel_focus = m->plain_len;
  m->doc_sel_active = true;
  m->doc_sel_dragging = false;
  m->doc_sel_pending_span = false;
  etch_request_focus(view.id);
  etch_request_redraw(doc->context);
 }

 // 3. Mouse press & drag selection across single or arbitrary multiple blocks.
 const bool is_ctrl = id_in_list(m->vis_ctrl_ids, m->vis_ctrl_count, act);
 const bool is_span = id_in_list(m->vis_span_ids, m->vis_span_count, act);
 bool is_seg = false;
 for (uint32_t i = 0; i < m->vis_seg_count; ++i) {
  if (m->vis_segs[i].element_id == act) { is_seg = true; break; }
 }
 const bool is_doc_hit = (act == view.id) || is_seg || id_in_list(m->vis_row_ids, m->vis_row_count, act);

 if (act != 0u && act != m->doc_sel_last_active) {
  if (is_ctrl) {
   m->doc_sel_dragging = false;
   m->doc_sel_pending_span = false;
  } else if (is_span) {
   const uint32_t hit = hit_test_doc_offset(m, ptr);
   m->doc_sel_anchor = hit;
   m->doc_sel_focus = hit;
   m->doc_sel_press_pos = ptr;
   m->doc_sel_dragging = true;
   m->doc_sel_pending_span = true;
   m->doc_sel_drag_active_id = act;
  } else if (is_doc_hit && m->vis_seg_count > 0u) {
   const uint32_t hit = hit_test_doc_offset(m, ptr);
   if ((etch_key_mods() & ETCH_MOD_SHIFT) && m->doc_sel_active) {
    m->doc_sel_focus = hit;
    m->doc_sel_active = (m->doc_sel_focus != m->doc_sel_anchor);
   } else {
    m->doc_sel_anchor = hit;
    m->doc_sel_focus = hit;
    m->doc_sel_active = false;
   }
   m->doc_sel_press_pos = ptr;
   m->doc_sel_dragging = true;
   m->doc_sel_pending_span = false;
   m->doc_sel_drag_active_id = view.id;
   etch_set_active(view.id);
   etch_request_focus(view.id);
   etch_request_redraw(doc->context);
  } else if (m->doc_sel_active) {
   m->doc_sel_active = false;
   m->doc_sel_dragging = false;
   m->doc_sel_pending_span = false;
   etch_request_redraw(doc->context);
  }
 }

 if (m->doc_sel_dragging) {
  const uint32_t cur_act = etch_active_id();
  if (cur_act == 0u) {
   m->doc_sel_dragging = false;
   m->doc_sel_pending_span = false;
  } else if (cur_act == m->doc_sel_drag_active_id || cur_act == view.id) {
   const float dx = ptr.x - m->doc_sel_press_pos.x;
   const float dy = ptr.y - m->doc_sel_press_pos.y;
   if (!m->doc_sel_pending_span || dx * dx + dy * dy >= 9.f) {
    if (m->doc_sel_pending_span) {
     m->doc_sel_pending_span = false;
     m->doc_sel_drag_active_id = view.id;
     etch_set_active(view.id);
     etch_request_focus(view.id);
    }
    const uint32_t hit = hit_test_doc_offset(m, ptr);
    if (hit != m->doc_sel_focus) {
     m->doc_sel_focus = hit;
     m->doc_sel_active = (m->doc_sel_focus != m->doc_sel_anchor);
     etch_request_redraw(doc->context);
    }
   }
  } else {
   m->doc_sel_dragging = false;
   m->doc_sel_pending_span = false;
  }
 }
 m->doc_sel_last_active = etch_active_id();

 // 4. Ctrl+C copies clean plain text across all selected spans/blocks.
 if (m->doc_sel_active && m->doc_sel_anchor != m->doc_sel_focus && etch_consume_shortcut(ETCH_MOD_PRIMARY, 'c', false)) {
  const uint32_t lo = utf8_clamp(m->plain_text, m->plain_len, m->doc_sel_anchor < m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus);
  const uint32_t hi = utf8_clamp(m->plain_text, m->plain_len, m->doc_sel_anchor > m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus);
  if (hi > lo) {
   char* copy = static_cast<char*>(etch_frame_arena_alloc(doc->context, size_t(hi - lo) + 1u, 1));
   if (copy) {
    std::memcpy(copy, m->plain_text + lo, hi - lo);
    copy[hi - lo] = 0;
    etch_set_clipboard_text(copy);
   }
  }
 }
 if (m->doc_sel_anchor != prev_anchor || m->doc_sel_focus != prev_focus || m->doc_sel_active != prev_active) {
  m->doc_sel_sync_needed = true;
 }
}

// Balanced Markdown writer for inlay_doc_view_copy_markdown
struct MdOut {
 char* buf;
 uint32_t cap;
 uint32_t len;
 void write(const char* s, uint32_t n) noexcept {
  if (!n) return;
  if (buf && cap > 1u && len < cap - 1u) {
   const uint32_t avail = (cap - 1u) - len;
   const uint32_t cp = n < avail ? n : avail;
   std::memcpy(buf + len, s, cp);
  }
  len += n;
 }
 void write_lit(const char* s) noexcept { write(s, uint32_t(std::strlen(s))); }
};

void emit_inline_markdown_slice(const InlayMeasure* m, EtchStr source, uint32_t line_src_a, uint32_t line_src_b,
                                uint32_t local_lo, uint32_t local_hi, bool table, MdOut* out) noexcept {
 uint32_t low = 0, high = m->token_count;
 while (low < high) {
  const uint32_t mid = low + (high - low) / 2u;
  if (m->tokens[mid].offset + m->tokens[mid].length <= line_src_a) low = mid + 1u; else high = mid;
 }
 uint32_t pcur = 0;
 for (uint32_t i = low; i < m->token_count && m->tokens[i].offset < line_src_b; ++i) {
  const auto& t = m->tokens[i];
  const uint32_t a = t.offset > line_src_a ? t.offset : line_src_a;
  const uint32_t b = t.offset + t.length < line_src_b ? t.offset + t.length : line_src_b;
  if (b <= a) continue;
  const uint32_t plen = table ? table_text_copy(source.chars + a, b - a, nullptr) : (b - a);
  const uint32_t ov_lo = local_lo > pcur ? local_lo : pcur;
  const uint32_t ov_hi = local_hi < pcur + plen ? local_hi : (pcur + plen);
  if (ov_hi > ov_lo) {
   const uint32_t sub_a = a + (ov_lo - pcur);
   const uint32_t sub_n = ov_hi - ov_lo;
   const bool link = (t.flags & InlayInlineLink) != 0;
   const bool bold = (t.flags & InlayInlineBold) != 0;
   const bool em = (t.flags & InlayInlineEmphasis) != 0;
   const bool strike = (t.flags & InlayInlineStrike) != 0;
   const bool hl = (t.flags & InlayInlineHighlight) != 0;
   const bool code = (t.flags & InlayInlineCode) != 0;
   if (link) out->write_lit("[");
   if (bold) out->write_lit("**");
   if (em) out->write_lit("*");
   if (strike) out->write_lit("~~");
   if (hl) out->write_lit("==");
   if (code) out->write_lit("`");
   out->write(source.chars + sub_a, sub_a + sub_n <= b ? sub_n : (b - sub_a));
   if (code) out->write_lit("`");
   if (hl) out->write_lit("==");
   if (strike) out->write_lit("~~");
   if (em) out->write_lit("*");
   if (bold) out->write_lit("**");
   if (link) {
    out->write_lit("](");
    const EtchStr url = (t.flags & InlayInlineUrlInPool)
     ? EtchStr{m->doc->ast_text + t.url_offset, t.url_length, false}
     : slice(source, t.url_offset, t.url_length);
    out->write(url.chars, url.length);
    out->write_lit(")");
   }
  }
  pcur += plen;
 }
}

} // namespace

extern "C" {
INLAY_API float inlay_doc_measure_height(InlayDoc* doc, uint32_t view_id, float width_px, float font_size_px) {
 if (!doc || !valid_dimensions(view_id, width_px, font_size_px)) return 0;
 const InlayMeasure* matched = nullptr;
 for (uint32_t vi = 0; vi < doc->view_count; ++vi) {
  auto& slot = doc->views[vi];
  if (slot.id != view_id || slot.view.width_px != width_px || slot.font != font_size_px) continue;
  matched = &slot;
  if (slot.width && slot.revision == doc->revision) {
   if (!slot.view.height_px && etch_current_context() == doc->context &&
       (slot.layout_settled || !etch_frame_in_progress(doc->context))) {
    const float height = etch_element_bounds(view_id).h;
    if (height > 0) {
     slot.layout_settled = true;
     return height;
    }
   }
   return float(slot.total_height);
  }
  break;
 }
 auto view = matched ? matched->view : inlay_doc_view_desc_default();
 const auto style = matched ? matched->style : inlay_doc_style_default();
 if (!matched) view.height_px = 0;
 view.font_size_px = font_size_px;
 const float width = view.height_px > 0 && width_px > 48 ? width_px - 12 : width_px;
 const EtchStr source = doc_source_str(doc);
 double total = 0;
 for (uint32_t i = 0; i < doc->block_count; ++i) {
  const auto& b = doc->blocks[i];
  if (i && view.height_px == 0) total += view.block_gap_px;
  if (b.kind != INLAY_TABLE) {
   const float custom_h = custom_fence_estimate(view, doc, i, std::fmax(1.f, width - 2 * view.block_padding_px));
   if (custom_h > 0.f) {
    total += custom_h + 2 * view.block_padding_px;
    continue;
   }
   const EtchStr img_src = b.kind == INLAY_IMAGE ? slice(source, b.language_offset, b.language_length) : EtchStr{};
   const EtchStr body = (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC) ? block_code_str(doc, i) : slice(source, b.content_offset, b.content_length);
   total += estimate_row(view, style, width, b, body, true, true, diagram_block(doc, b), 1, img_src);
   continue;
  }
  uint32_t pos = b.content_offset, end = pos + b.content_length, columns = 1;
  bool first = true, rule = false;
  while (pos < end) {
   uint32_t stop = pos;
   while (stop < end && source.chars[stop] != '\n') ++stop;
   const uint32_t next = stop < end ? stop + 1u : stop;
   EtchStr line = slice(source, pos, stop - pos);
   if (b.quote_depth) {
    const uint32_t qskip = strip_quote_prefix(line.chars, line.length, b.quote_depth);
    line.chars += qskip;
    line.length -= qskip;
   }
   if (first) {
    columns = table_columns(line);
    if (!columns) columns = 1;
   }
   const bool separator = rule && inlay_table_rule(line.chars, line.length);
   const bool last = next == end || (first && !std::memchr(source.chars + next, '\n', end - next - 1u) &&
                                     inlay_table_rule(source.chars + next, end - next));
   if (separator) total += view.height_px == 0 ? 0.f : 1.f;
   else total += estimate_row(view, style, width, b, line, first, last, false, columns);
   rule = first;
   first = false;
   pos = next;
  }
 }
 return float(total);
}

INLAY_API bool inlay_doc_view_is_at_bottom(uint32_t view_id) {
 return view_id ? etch_virtual_list_is_at_bottom(view_id) : true;
}

INLAY_API void inlay_doc_view_scroll_to_bottom(uint32_t view_id, bool stop_momentum) {
 if (view_id) etch_virtual_list_scroll_to_bottom(view_id, stop_momentum);
}

INLAY_API InlayDocSelection inlay_doc_view_selection(const InlayDoc* doc, uint32_t view_id) {
 InlayDocSelection s = {};
 const InlayMeasure* m = find_view(doc, view_id);
 if (!m) return s;
 s.anchor = m->doc_sel_anchor;
 s.focus = m->doc_sel_focus;
 s.total_bytes = m->plain_len;
 s.active = m->doc_sel_active && m->doc_sel_anchor != m->doc_sel_focus;
 return s;
}

INLAY_API void inlay_doc_view_set_selection(InlayDoc* doc, uint32_t view_id, uint32_t anchor, uint32_t focus) {
 InlayMeasure* m = find_view(doc, view_id);
 if (!m) return;
 m->doc_sel_anchor = utf8_clamp(m->plain_text, m->plain_len, anchor);
 m->doc_sel_focus = utf8_clamp(m->plain_text, m->plain_len, focus);
 m->doc_sel_active = (m->doc_sel_anchor != m->doc_sel_focus);
 m->doc_sel_dragging = false;
 m->doc_sel_pending_span = false;
 m->doc_sel_sync_needed = true;
 etch_request_redraw(doc->context);
}

INLAY_API void inlay_doc_view_select_all(InlayDoc* doc, uint32_t view_id) {
 InlayMeasure* m = find_view(doc, view_id);
 if (!m) return;
 m->doc_sel_anchor = 0u;
 m->doc_sel_focus = m->plain_len;
 m->doc_sel_active = m->plain_len > 0u;
 m->doc_sel_dragging = false;
 m->doc_sel_pending_span = false;
 m->doc_sel_sync_needed = true;
 etch_request_redraw(doc->context);
}

INLAY_API void inlay_doc_view_clear_selection(InlayDoc* doc, uint32_t view_id) {
 InlayMeasure* m = find_view(doc, view_id);
 if (!m) return;
 m->doc_sel_active = false;
 m->doc_sel_dragging = false;
 m->doc_sel_pending_span = false;
 m->doc_sel_sync_needed = true;
 m->doc_sel_anchor = m->doc_sel_focus = 0u;
 etch_request_redraw(doc->context);
}

INLAY_API uint32_t inlay_doc_view_copy_plain_text(const InlayDoc* doc, uint32_t view_id, char* out_buf, uint32_t cap) {
 const InlayMeasure* m = find_view(doc, view_id);
 if (!m || !m->doc_sel_active || m->doc_sel_anchor == m->doc_sel_focus || !m->plain_text) {
  if (out_buf && cap > 0u) out_buf[0] = 0;
  return 0u;
 }
 const uint32_t lo = utf8_clamp(m->plain_text, m->plain_len, m->doc_sel_anchor < m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus);
 const uint32_t hi = utf8_clamp(m->plain_text, m->plain_len, m->doc_sel_anchor > m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus);
 const uint32_t n = hi > lo ? (hi - lo) : 0u;
 if (out_buf && cap > 0u) {
  const uint32_t cp = n < cap - 1u ? n : (cap - 1u);
  if (cp > 0u) std::memcpy(out_buf, m->plain_text + lo, cp);
  out_buf[cp] = 0;
 }
 return n;
}

INLAY_API uint32_t inlay_doc_view_copy_markdown(const InlayDoc* doc, uint32_t view_id, char* out_buf, uint32_t cap) {
 const InlayMeasure* m = find_view(doc, view_id);
 if (!m || !m->doc_sel_active || m->doc_sel_anchor == m->doc_sel_focus || !m->plain_text) {
  if (out_buf && cap > 0u) out_buf[0] = 0;
  return 0u;
 }
 const uint32_t lo = utf8_clamp(m->plain_text, m->plain_len, m->doc_sel_anchor < m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus);
 const uint32_t hi = utf8_clamp(m->plain_text, m->plain_len, m->doc_sel_anchor > m->doc_sel_focus ? m->doc_sel_anchor : m->doc_sel_focus);
 if (hi <= lo) {
  if (out_buf && cap > 0u) out_buf[0] = 0;
  return 0u;
 }
 MdOut out = {out_buf, cap, 0u};
 const EtchStr source = doc_source_str(doc);
 uint32_t prev_blk = UINT32_MAX;
 bool in_code_fence = false;
 bool emitted_any = false;

 for (uint32_t r = 0; r < m->count; ++r) {
  const auto& row = m->rows[r];
  if (row.doc_text_length == 0u) continue;
  const uint32_t r_lo = row.doc_text_offset;
  const uint32_t r_hi = r_lo + row.doc_text_length;
  if (r_hi <= lo || r_lo >= hi) continue;
  const uint32_t ov_lo = lo > r_lo ? (lo - r_lo) : 0u;
  const uint32_t ov_hi = hi < r_hi ? (hi - r_lo) : row.doc_text_length;
  const auto& blk = doc->blocks[row.block];
  const bool new_blk = row.block != prev_blk;
  if (new_blk && in_code_fence) {
   out.write_lit("\n```");
   in_code_fence = false;
  }
  if (emitted_any) {
   if (new_blk) out.write_lit("\n\n");
   else out.write_lit("\n");
  }
  emitted_any = true;
  prev_blk = row.block;

  if (blk.kind == INLAY_CODE_BLOCK) {
   if (new_blk) {
    out.write_lit("```");
    if (blk.language_length) out.write(source.chars + blk.language_offset, blk.language_length);
    out.write_lit("\n");
    in_code_fence = true;
   }
   out.write(m->plain_text + r_lo + ov_lo, ov_hi - ov_lo);
   continue;
  }

  for (uint16_t q = 0; q < blk.quote_depth; ++q) out.write_lit("> ");
  if (blk.kind == INLAY_HEADING && ov_lo == 0u && row.first) {
   for (uint32_t h = 0; h < blk.level && h < 6u; ++h) out.write_lit("#");
   out.write_lit(" ");
  } else if ((blk.kind == INLAY_LIST || blk.kind == INLAY_TASK_LIST) && ov_lo == 0u) {
   const InlayListItemMeta* meta = (row.list_item_index != UINT32_MAX && row.list_item_index < doc->list_item_count)
    ? &doc->list_items[row.list_item_index] : nullptr;
   const uint32_t ind = meta && meta->list_depth > 1u ? uint32_t(meta->list_depth - 1u) * 2u : 0u;
   for (uint32_t k = 0; k < ind; ++k) out.write_lit(" ");
   if (meta && meta->ordered) {
    char nb[16];
    const int n = std::snprintf(nb, sizeof(nb), "%u. ", meta->ordinal ? meta->ordinal : 1u);
    if (n > 0) out.write(nb, uint32_t(n));
   } else {
    out.write_lit("- ");
   }
   if (meta && meta->is_task) {
    const bool chk = (meta->task_mark_offset < source.length) ? (source.chars[meta->task_mark_offset] != ' ') : (meta->checked != 0);
    out.write_lit(chk ? "[x] " : "[ ] ");
   }
  }

  if (blk.kind == INLAY_TABLE) {
   out.write(m->plain_text + r_lo + ov_lo, ov_hi - ov_lo);
  } else {
   uint32_t src_a = row.offset, src_b = row.offset + row.length;
   if (src_b > src_a && source.chars[src_b - 1u] == '\n') --src_b;
   if (src_b > src_a && source.chars[src_b - 1u] == '\r') --src_b;
   if (blk.quote_depth) src_a += strip_quote_prefix(source.chars + src_a, src_b - src_a, blk.quote_depth);
   if (blk.kind == INLAY_LIST || blk.kind == INLAY_TASK_LIST) {
    EtchStr line = {source.chars + src_a, src_b - src_a, false};
    const InlayListItemMeta* meta = (row.list_item_index != UINT32_MAX && row.list_item_index < doc->list_item_count)
     ? &doc->list_items[row.list_item_index] : nullptr;
    uint32_t trim = 0;
    strip_list_marker(&line, meta, nullptr, nullptr, nullptr, &trim);
    src_a = uint32_t((line.chars + trim) - source.chars);
   }
   emit_inline_markdown_slice(m, source, src_a, src_b, ov_lo, ov_hi, false, &out);
  }
 }
 if (in_code_fence) out.write_lit("\n```");
 if (out_buf && cap > 0u) out_buf[out.len < cap - 1u ? out.len : (cap - 1u)] = 0;
 return out.len;
}

INLAY_API InlayDocResult inlay_doc_view(InlayDoc* doc, const InlayDocViewDesc* input_in) {
 InlayDocResult result = {};
 InlayDocViewDesc view = {};
 if (!doc || !inlay_read_abi_struct(input_in, &view) || !valid_dimensions(view.id, view.width_px, view.font_size_px) ||
     !std::isfinite(view.height_px) || view.height_px < 0 || (view.height_px > 0 && view.height_px < 1) ||
     !valid_spacing(view.block_padding_px) || !valid_spacing(view.block_gap_px) ||
     !valid_spacing(view.table_cell_padding_x) || !valid_spacing(view.table_cell_padding_y) ||
     !valid_spacing(view.table_radius_px) ||
     etch_current_context() != doc->context || !etch_frame_in_progress(doc->context)) return result;

 InlayDocStyle style = inlay_doc_style_default();
 if (view.style) {
  if (!inlay_read_abi_struct(view.style, &style) || !valid_style(&style)) return result;
  view.style = &style;
 }

 InlayMeasure* measure = find_or_add_view(doc, view.id);
 if (!measure) return result;
 const bool flow = view.height_px == 0;
 const float row_width = !flow && view.width_px > 48 ? view.width_px - 12 : view.width_px;
 const auto& old = measure->view;
 const bool reflow = measure->width != row_width || measure->font != view.font_size_px ||
  flow != (old.height_px == 0) || old.block_padding_px != view.block_padding_px || old.block_gap_px != view.block_gap_px ||
  old.table_cell_padding_x != view.table_cell_padding_x || old.table_cell_padding_y != view.table_cell_padding_y ||
  old.hide_code_copy_button != view.hide_code_copy_button || old.inline_decorator_fn != view.inline_decorator_fn ||
  old.inline_decorator_user_data != view.inline_decorator_user_data ||
  old.image_resolver_fn != view.image_resolver_fn || old.image_resolver_user_data != view.image_resolver_user_data ||
  old.custom_fence_estimate_fn != view.custom_fence_estimate_fn ||
  old.custom_fence_render_fn != view.custom_fence_render_fn ||
  old.custom_fence_user_data != view.custom_fence_user_data ||
  old.code_header_fn != view.code_header_fn ||
  old.code_header_user_data != view.code_header_user_data ||
  (measure->width != 0 && style_reflow_changed(measure->style, style));
 const bool refresh = reflow || measure->revision != doc->revision;
 float previous_height = flow && !refresh ? etch_element_bounds(view.id).h : 0.f;
 measure->layout_settled = previous_height > 0;
 if (refresh) {
  if (!prepare_view(doc, measure, view, row_width, reflow)) return result;
  if (!flow) etch_virtual_list_invalidate_heights(view.id);
  if (measure->doc_sel_active) measure->doc_sel_sync_needed = true;
 }
 measure->style = style;
 measure->view = view;
 measure->view.style = &measure->style;

 // Update unified document character-level cross-block selection BEFORE rendering so highlights appear on this frame!
 update_doc_selection(doc, measure, view);
 measure->vis_seg_count = 0;
 measure->vis_row_count = 0;
 measure->vis_ctrl_count = 0;
 measure->vis_span_count = 0;

 result.total_rows = measure->count;
 if (flow) {
  auto box = etch_box_desc_default();
  box.id = view.id;
  box.axis = ETCH_AXIS_COL;
  box.width = etch_sizing_fixed(view.width_px);
  box.height = etch_sizing_fit(0, 0);
  box.gap = view.block_gap_px;
  if (style.has_background || style.background.a != 0) box.background = style.background;
  etch_begin_box(&box);
  result.visible_end = measure->count;
  result.at_bottom = true;
 } else {
  auto list = etch_virtual_list_desc_default();
  list.id = view.id;
  list.item_count = measure->count;
  list.variable_height = true;
  list.width = etch_sizing_fixed(view.width_px);
  list.height = etch_sizing_fixed(view.height_px);
  list.scroll_fade_px = 20;
  list.measure_height_fn = block_height;
  list.measure_user_data = measure;
  list.stick_to_bottom = view.stick_to_bottom;
  if (style.has_background || style.background.a != 0) list.background = style.background;
  const auto visible = etch_begin_virtual_list(&list);
  result.visible_first = visible.first_index;
  result.visible_end = visible.end_index;
  result.at_bottom = visible.at_bottom;
 }
 const auto theme = etch_get_theme(doc->context);
 const EtchStr source = doc_source_str(doc);
 PendingTaskToggle pending_toggle = {};
 bool group_open = false;

 if (measure->code_scroll_x) {
  for (uint32_t i = result.visible_first; i < result.visible_end; ++i) {
   const auto& rinfo = measure->rows[i];
   if (rinfo.kind != INLAY_CODE_BLOCK || rinfo.block >= measure->open_capacity) continue;
   if (measure->code_wrap && measure->code_wrap[rinfo.block]) continue;
   const uint32_t base_off = (doc->blocks[rinfo.block].flags & INLAY_BLOCK_FLAG_SYNTHETIC) ? 0u : doc->blocks[rinfo.block].content_offset;
   const uint32_t rid = etch_id_index(etch_id_index(view.id, rinfo.block + 1u), (rinfo.offset - base_off) + 1u);
   const float cur_x = etch_scroll_offset(etch_id_index(rid, 2)).x;
   if (std::fabs(cur_x - rinfo.last_scroll_x) > 0.5f) {
    measure->code_scroll_x[rinfo.block] = cur_x;
   }
  }
 }

 for (uint32_t i = result.visible_first; i < result.visible_end; ++i) {
  const auto& info = measure->rows[i];
  auto b = doc->blocks[info.block];
  const uint32_t base_off = (b.flags & INLAY_BLOCK_FLAG_SYNTHETIC) ? 0u : b.content_offset;
  b.content_offset = info.offset;
  b.content_length = info.length;
  const uint32_t row_id = etch_id_index(etch_id_index(view.id, info.block + 1u), (info.offset - base_off) + 1u);
  if (flow && info.first) {
   if (group_open) etch_end_box();
   auto group = etch_box_desc_default();
   group.axis = ETCH_AXIS_COL;
   group.width = etch_sizing_fixed(row_width);
   group.height = etch_sizing_fit(0, 0);
   etch_begin_box(&group);
   group_open = true;
  }
  if (skipped_row(*measure, info)) {
   if (!flow) {
    auto spacer = etch_box_desc_default();
    spacer.height = etch_sizing_fixed(1);
    if (info.separator && !measure->rows[info.table_header].last_in_block) {
     spacer.width = etch_sizing_fixed(row_width);
     spacer.padding = etch_padding_xy(view.block_padding_px, 0);
     etch_begin_box(&spacer);
     auto seam = etch_box_desc_default();
     seam.width = etch_sizing_grow(0, 0);
     seam.height = etch_sizing_fixed(1);
     seam.background = style.table_border.a != 0 ? style.table_border : theme.border_subtle;
     etch_box(&seam);
     etch_end_box();
    } else etch_box(&spacer);
   }
   continue;
  }
  const bool is_custom_fence = b.kind == INLAY_CODE_BLOCK && info.first &&
   custom_fence_estimate(view, doc, info.block, std::fmax(1.f, row_width - 2 * view.block_padding_px)) > 0.f;
  if (!is_custom_fence) push_u32(&measure->vis_row_ids, &measure->vis_row_count, &measure->vis_row_cap, row_id);

  // Nested blockquote/alert/list indentation wrapper for nested AST nodes!
  const uint16_t extra_quotes = (b.kind == INLAY_BLOCKQUOTE || b.kind == INLAY_ALERT)
   ? (b.quote_depth > 1u ? uint16_t(b.quote_depth - 1u) : 0u)
   : b.quote_depth;
  const float list_nest_indent = (b.list_depth > 0u && b.kind != INLAY_LIST && b.kind != INLAY_TASK_LIST)
   ? float(b.list_depth) * std::fmax(16.f, view.font_size_px * 1.1f) : 0.f;
  const float outer_indent = float(extra_quotes) * 14.f + list_nest_indent;
  const float eff_row_width = std::fmax(32.f, row_width - outer_indent);

  uint32_t open_wrappers = 0;
  if (list_nest_indent > 0.f) {
   auto lw = etch_box_desc_default();
   lw.axis = ETCH_AXIS_COL;
   lw.width = etch_sizing_fixed(row_width);
   lw.height = etch_sizing_fit(0, 0);
   lw.padding = {list_nest_indent, 0.f, 0.f, 0.f};
   etch_begin_box(&lw);
   ++open_wrappers;
  }
  for (uint16_t q = 0; q < extra_quotes; ++q) {
   auto qw = etch_box_desc_default();
   qw.axis = ETCH_AXIS_COL;
   qw.width = etch_sizing_fixed(std::fmax(32.f, row_width - list_nest_indent - float(q) * 14.f));
   qw.height = etch_sizing_fit(0, 0);
   qw.padding = {12.f, 0.f, 2.f, 2.f};
   qw.border_color = b.alert_kind != INLAY_ALERT_NONE
    ? (style.quote_border.a != 0 ? style.quote_border : alert_accent_from_kind(b.alert_kind, theme))
    : (style.quote_border.a != 0 ? style.quote_border : (theme.border_control.a != 0 ? theme.border_control : theme.border_subtle));
   qw.border_sides = {2.5f, 0.f, 0.f, 0.f};
   etch_begin_box(&qw);
   ++open_wrappers;
  }

  const float padding = is_custom_fence ? view.block_padding_px : block_padding(view, b.kind);
  auto box = etch_box_desc_default();
  box.id = row_id;
  box.axis = ETCH_AXIS_COL;
  box.width = etch_sizing_fixed(eff_row_width);
  box.height = etch_sizing_fit(0, 0);
  box.padding = etch_padding_all(padding);
  box.gap = 4;
  box.clip_children = true;
  box.shrink_width = true;
  if (b.kind == INLAY_TABLE) {
   box.padding = {padding, padding, info.first ? padding : 0.f, info.last_in_block ? padding : 0.f};
   box.gap = 0;
  }
  if (b.kind == INLAY_CODE_BLOCK && !is_custom_fence) {
   box.background = style.code_bg.a != 0 ? style.code_bg : theme.bg_elevated;
   box.border_color = style.code_border.a != 0 ? style.code_border : theme.border_subtle;
   box.border_width = 1;
   box.radius = etch_corners_all(theme.radius_panel_px);
  } else if (b.kind == INLAY_ALERT) {
   const EtchColor acol = style.quote_border.a != 0 ? style.quote_border : alert_accent_color(source, doc->blocks[info.block], theme);
   box.background = style.quote_bg.a != 0 ? style.quote_bg : theme.bg_elevated;
   box.border_color = acol;
   box.border_sides = {3.f, 0.f, 0.f, 0.f};
   box.radius = {2.f, 6.f, 6.f, 2.f};
   box.padding = {padding + 4.f, padding, padding * 0.85f, padding * 0.85f};
  } else if (b.kind == INLAY_BLOCKQUOTE) {
   box.background = style.quote_bg.a != 0 ? style.quote_bg : theme.bg_elevated;
   box.border_color = style.quote_border.a != 0 ? style.quote_border : (theme.border_control.a != 0 ? theme.border_control : theme.border_subtle);
   box.border_sides = {2.5f, 0.f, 0.f, 0.f};
   box.radius = {2.f, 6.f, 6.f, 2.f};
   box.padding = {padding + 4.f, padding, padding * 0.85f, padding * 0.85f};
  }

  const float inner_w = std::fmax(1.f, eff_row_width - box.padding.left - box.padding.right);
  const EtchBoxDesc* pending_box = !info.first && view.inline_decorator_fn && !(b.flags & INLAY_BLOCK_FLAG_SYNTHETIC) &&
   info.offset > 0u && source.chars[info.offset - 1u] != '\n' &&
   b.kind != INLAY_CODE_BLOCK && b.kind != INLAY_TABLE && b.kind != INLAY_THEMATIC_BREAK && b.kind != INLAY_IMAGE && b.kind != INLAY_FOOTNOTE ? &box : nullptr;
  if (!pending_box) etch_begin_box(&box);

  float styled_height = 0;
  if (b.kind == INLAY_THEMATIC_BREAK) {
   auto rule = etch_box_desc_default();
   rule.width = etch_sizing_grow(0, 0);
   rule.height = etch_sizing_fixed(1);
   rule.background = theme.border_line;
   etch_box(&rule);
  } else if (b.kind == INLAY_MATH_BLOCK || diagram_block(doc, b)) {
   const EtchStr body = (b.kind == INLAY_CODE_BLOCK) ? block_code_str(doc, info.block) : slice(source, b.content_offset, b.content_length);
   native_box(body, view.font_size_px, b.kind != INLAY_MATH_BLOCK, etch_id_index(row_id, 7), inner_w, style.text_color);
  } else if (b.kind == INLAY_IMAGE) {
   const auto& full = doc->blocks[info.block];
   const EtchStr alt = slice(source, full.content_offset, full.content_length);
   const EtchStr src = slice(source, full.language_offset, full.language_length);
   const auto img = resolve_doc_image(view, alt, src, inner_w);
   if (img.valid) {
    auto idesc = etch_image_desc_default();
    idesc.id = etch_id_index(row_id, 1);
    idesc.texture = img.texture;
    idesc.width = etch_sizing_fixed(img.width);
    idesc.height = etch_sizing_fixed(img.height);
    idesc.radius = etch_corners_all(img.radius);
    etch_image(&idesc);
   } else draw_image_fallback(alt, etch_id_index(row_id, 1), view.font_size_px, style, theme);
  } else if (b.kind == INLAY_FOOTNOTE) {
   const float fn_font = std::fmax(10.f, view.font_size_px * 0.85f);
   const EtchColor muted = style.muted_color.a != 0 ? style.muted_color : theme.text_muted;
   if (info.first) {
    auto div = etch_box_desc_default();
    div.width = etch_sizing_grow(0, 0);
    div.height = etch_sizing_fixed(1);
    div.background = theme.border_subtle;
    etch_box(&div);
   }
   auto fn_row = etch_box_desc_default();
   fn_row.width = etch_sizing_fixed(inner_w);
   fn_row.gap = 8;
   fn_row.align_y = ETCH_ALIGN_Y_TOP;
   etch_begin_box(&fn_row);
   if (info.first && b.language_length) {
    auto lbl = etch_text_desc_default();
    lbl.font_size_px = fn_font;
    lbl.font_id = effective_sans_font(style);
    lbl.color = style.link_color.a != 0 ? style.link_color : muted;
    lbl.wrap = false;
    char* lbuf = static_cast<char*>(etch_frame_arena_alloc(doc->context, size_t(b.language_length) + 3, 1));
    if (lbuf) {
     lbuf[0] = '[';
     std::memcpy(lbuf + 1, source.chars + b.language_offset, b.language_length);
     lbuf[b.language_length + 1u] = ']';
     lbuf[b.language_length + 2u] = ':';
     etch_text({lbuf, b.language_length + 3u, false}, &lbl);
    } else etch_text(slice(source, b.language_offset, b.language_length), &lbl);
   }
   auto fn_body = etch_box_desc_default();
   fn_body.axis = ETCH_AXIS_COL;
   fn_body.width = etch_sizing_grow(0, 0);
   fn_body.height = etch_sizing_fit(0, 0);
   fn_body.gap = 4;
   etch_begin_box(&fn_body);
   styled_height = block_lines(measure, b, source, row_id, fn_font, &result, theme, style, info, i, inner_w, false, pending_box, nullptr, muted);
   etch_end_box();
   etch_end_box();
  } else if (b.kind == INLAY_DETAILS) {
   const auto& range = measure->details[info.block];
   const uint32_t summary = range.summary, summary_end = range.summary_end, body = range.body, end = range.end;
   if (info.first) {
    auto button = etch_button_desc_default();
    button.id = etch_id_index(row_id, 900);
    push_u32(&measure->vis_ctrl_ids, &measure->vis_ctrl_count, &measure->vis_ctrl_cap, button.id);
    button.label = summary_end > summary ? slice(source, summary, summary_end - summary) : ETCH_STR_LIT("Details");
    if (etch_button(&button)) {
     measure->open[info.block] = !measure->open[info.block];
     measure->revision = doc->revision - 1u;
     for (uint32_t j = i; j < measure->count && measure->rows[j].block == info.block; ++j) {
      measure->heights[j] = 0;
      measure->rows[j].estimate = 0;
     }
     etch_request_redraw(doc->context);
    }
   }
   if (measure->open[info.block]) {
    const uint32_t start = b.content_offset > body ? b.content_offset : body;
    const uint32_t stop = b.content_offset + b.content_length < end ? b.content_offset + b.content_length : end;
    if (stop > start) {
     b.kind = INLAY_PARAGRAPH;
     b.content_offset = start;
     b.content_length = stop - start;
     styled_height = block_lines(measure, b, source, row_id, view.font_size_px, &result, theme, style, info, i, inner_w, false, pending_box);
    }
   }
  } else if (is_custom_fence) {
   const auto& full = doc->blocks[info.block];
   const EtchStr lang = slice(source, full.language_offset, full.language_length);
   const EtchStr body = block_code_str(doc, info.block);
   view.custom_fence_render_fn(info.block, row_id, lang, body, inner_w, view.font_size_px, view.custom_fence_user_data);
  } else if (b.kind == INLAY_CODE_BLOCK) {
   const auto& full = doc->blocks[info.block];
   const float code_inner_w = inner_w;
   const bool wrap_code = info.block < measure->open_capacity && measure->code_wrap && measure->code_wrap[info.block];
   const EtchStr full_code = block_code_str(doc, info.block);
   if (info.first && (!view.hide_code_copy_button || b.language_length || view.code_header_fn)) {
    const EtchStr lang = slice(source, full.language_offset, full.language_length);
    auto hdr = etch_box_desc_default();
    hdr.width = etch_sizing_fixed(code_inner_w);
    hdr.height = etch_sizing_fit(0, 0);
    hdr.padding = {0, 0, 0, 6};
    hdr.align_y = ETCH_ALIGN_Y_CENTER;
    hdr.gap = 6;
    hdr.border_color = style.code_border.a != 0 ? style.code_border : theme.border_subtle;
    hdr.border_sides = {0, 0, 0, 1};
    etch_begin_box(&hdr);
    if (lang.length) {
     auto ld = etch_text_desc_default();
     ld.font_id = effective_mono_font(style);
     ld.font_size_px = std::fmax(11.f, view.font_size_px * 0.82f);
     ld.color = style.muted_color.a != 0 ? style.muted_color : theme.text_muted;
     ld.wrap = false;
     etch_text(lang, &ld);
    }
    auto sp = etch_box_desc_default();
    sp.width = etch_sizing_grow(0, 0);
    sp.height = etch_sizing_fixed(1);
    sp.shrink_width = true;
    etch_box(&sp);
    if (info.block < measure->open_capacity && measure->code_wrap) {
     const uint32_t wrap_id = etch_id_index(row_id, 3);
     push_u32(&measure->vis_ctrl_ids, &measure->vis_ctrl_count, &measure->vis_ctrl_cap, wrap_id);
     auto wbtn = etch_button_desc_default();
     wbtn.id = wrap_id;
     wbtn.variant = ETCH_BUTTON_GHOST;
     wbtn.label = wrap_code ? ETCH_STR_LIT("Wrap: On") : ETCH_STR_LIT("Wrap");
     wbtn.font_size_px = std::fmax(11.f, view.font_size_px * 0.78f);
     wbtn.padding = etch_padding_xy(6, 2);
     if (etch_button(&wbtn)) {
      measure->code_wrap[info.block] = !measure->code_wrap[info.block];
      for (uint32_t j = i; j < measure->count && measure->rows[j].block == info.block; ++j) {
       measure->heights[j] = 0;
       measure->rows[j].estimate = 0;
      }
      if (!flow) etch_virtual_list_invalidate_heights(view.id);
      etch_request_redraw(doc->context);
     }
    }
    if (view.code_header_fn) {
     view.code_header_fn(info.block, row_id, lang, full_code, view.code_header_user_data);
    }
    if (!view.hide_code_copy_button) {
     auto button = etch_button_desc_default();
     button.id = etch_id_index(row_id, 1);
     push_u32(&measure->vis_ctrl_ids, &measure->vis_ctrl_count, &measure->vis_ctrl_cap, button.id);
     button.label = ETCH_STR_LIT("Copy code");
     if (etch_button(&button)) {
      char* copy = static_cast<char*>(etch_frame_arena_alloc(doc->context, size_t(full_code.length) + 1u, 1));
      if (copy) { std::memcpy(copy, full_code.chars, full_code.length); copy[full_code.length] = 0; etch_set_clipboard_text(copy); }
     }
    }
    etch_end_box();
   }
   const bool show_ln = view.show_code_line_numbers;
   const uint32_t start_line = info.code_start_line ? info.code_start_line : 1u;
   const uint32_t total_lines = info.code_total_lines ? info.code_total_lines : 1u;
   uint32_t chunk_lines = 0;
   float gutter_w = 0.f;
   if (show_ln) {
    uint32_t lp = info.offset >= base_off ? (info.offset - base_off) : 0u;
    const uint32_t le = lp + info.length <= full_code.length ? lp + info.length : full_code.length;
    while (lp < le) {
     uint32_t ln = lp;
     while (ln < le && full_code.chars[ln] != '\n') ++ln;
     ++chunk_lines;
     lp = ln < le ? ln + 1u : le;
    }
    gutter_w = total_lines >= 1000u ? 40.f : (total_lines >= 100u ? 32.f : 24.f);
   }
   if (wrap_code) {
    auto scroll = etch_box_desc_default();
    scroll.id = etch_id_index(row_id, 2);
    scroll.axis = ETCH_AXIS_COL;
    scroll.width = etch_sizing_fixed(code_inner_w);
    scroll.height = etch_sizing_fit(0, 0);
    scroll.gap = 4;
    scroll.clip_children = true;
    scroll.shrink_width = true;
    etch_begin_box(&scroll);
    block_lines(measure, b, source, scroll.id, view.font_size_px, &result, theme, style, info, i, code_inner_w,
                false, nullptr, nullptr, {}, true, start_line, show_ln ? gutter_w : 0.f, code_inner_w);
    etch_end_box();
   } else {
    float scroll_w = code_inner_w;
    if (show_ln) {
     const float code_font = effective_code_font_size(view.font_size_px, style);
     const float line_h = code_font * effective_line_scale(style);
     scroll_w = std::fmax(1.f, code_inner_w - gutter_w - 10.f);
     auto row_wrap = etch_box_desc_default();
     row_wrap.axis = ETCH_AXIS_ROW;
     row_wrap.width = etch_sizing_fixed(code_inner_w);
     row_wrap.height = etch_sizing_fit(0, 0);
     row_wrap.gap = 10;
     etch_begin_box(&row_wrap);
     auto gutter = etch_box_desc_default();
     gutter.axis = ETCH_AXIS_COL;
     gutter.width = etch_sizing_fixed(gutter_w);
     gutter.height = etch_sizing_fit(0, 0);
     gutter.padding = {0, 8, 0, 0};
     gutter.gap = 4;
     gutter.align_x = ETCH_ALIGN_X_RIGHT;
     gutter.border_color = style.code_border.a != 0 ? style.code_border : theme.border_subtle;
     gutter.border_sides = {0, 1, 0, 0};
     etch_begin_box(&gutter);
     auto lnd = etch_text_desc_default();
     lnd.font_id = effective_mono_font(style);
     lnd.font_size_px = std::fmax(10.f, code_font * 0.92f);
     lnd.line_height_px = line_h;
     lnd.color = style.muted_color.a != 0 ? style.muted_color : theme.text_muted;
     lnd.wrap = false;
     for (uint32_t li = 0; li < chunk_lines; ++li) {
      char nb[16];
      const int n = std::snprintf(nb, sizeof(nb), "%u", start_line + li);
      if (n > 0) {
       char* arena_s = static_cast<char*>(etch_frame_arena_alloc(doc->context, size_t(n), 1));
       if (arena_s) { std::memcpy(arena_s, nb, size_t(n)); etch_text({arena_s, uint32_t(n), false}, &lnd); }
      }
     }
     etch_end_box();
    }
    const uint32_t chunk_scroll_id = etch_id_index(row_id, 2);
    if (info.block < measure->open_capacity && measure->code_scroll_x) {
     const float target_x = measure->code_scroll_x[info.block];
     const float cur_x = etch_scroll_offset(chunk_scroll_id).x;
     if (std::fabs(cur_x - target_x) > 0.5f) {
      etch_set_scroll_offset(chunk_scroll_id, {target_x, 0.f}, true);
     }
     measure->rows[i].last_scroll_x = target_x;
    }
    auto scroll = etch_box_desc_default();
    scroll.id = chunk_scroll_id;
    scroll.width = etch_sizing_fixed(scroll_w);
    scroll.height = etch_sizing_fit(0, 0);
    scroll.scroll_x = true;
    scroll.clip_children = true;
    etch_begin_box(&scroll);
    auto content = etch_box_desc_default();
    content.axis = ETCH_AXIS_COL;
    content.width = etch_sizing_fit(info.code_min_width, 0);
    content.height = etch_sizing_fit(0, 0);
    content.gap = 4;
    etch_begin_box(&content);
    block_lines(measure, b, source, scroll.id, view.font_size_px, &result, theme, style, info, i, scroll_w);
    etch_end_box();
    etch_end_box();
    if (show_ln) etch_end_box();
   }
  } else {
   const bool estimate = view.inline_decorator_fn && (measure->heights[i] == collapsed_height ||
                         (measure->heights[i] == 0 && (refresh || etch_element_bounds(row_id).h <= 0)));
   const float font = b.kind == INLAY_HEADING ? effective_heading_font(view, style, b.level) : view.font_size_px;
   const EtchColor color_ov = b.kind == INLAY_HEADING ? style.heading_color
    : (b.kind == INLAY_BLOCKQUOTE ? style.muted_color : EtchColor{});
   styled_height = block_lines(measure, b, source, row_id, font, &result, theme, style, info, i, inner_w, estimate, pending_box, &pending_toggle, color_ov);
  }
  if (!pending_box) etch_end_box();
  while (open_wrappers--) etch_end_box();

  const float cached = measure->heights[i];
  if (measure->revision == doc->revision && styled_height != cached &&
      (styled_height != 0 || cached == collapsed_height)) {
   const float before = block_height(i, measure);
   measure->heights[i] = styled_height;
   const float after = block_height(i, measure);
   measure->total_height += after - before;
   if (!flow) etch_virtual_list_set_item_height(view.id, i, std::fmax(1.f, after));
   if (styled_height == collapsed_height || cached == collapsed_height) {
    previous_height = 0;
    measure->layout_settled = false;
   }
  }
  if (styled_height == collapsed_height) continue;
  if (!reflow && cached != collapsed_height && measure->revision == doc->revision) {
   const EtchRect previous = etch_element_bounds(row_id);
   if (previous.h > 0 && std::fabs(previous.h - measure->heights[i]) > .01f) {
    measure->total_height += previous.h - block_height(i, measure);
    measure->heights[i] = previous.h;
    if (!flow) etch_virtual_list_set_item_height(view.id, i, previous.h);
   }
  }
 }
 if (flow) {
  if (group_open) etch_end_box();
  etch_end_box();
 } else etch_end_virtual_list();
 measure->doc_sel_sync_needed = false;

 result.measured_height_px = previous_height > 0 ? previous_height : float(measure->total_height);
 if (pending_toggle.triggered && pending_toggle.source_offset < doc->source_size) {
  doc->source[pending_toggle.source_offset] = pending_toggle.checked ? 'x' : ' ';
  for (uint32_t k = 0; k < doc->list_item_count; ++k) {
   if (doc->list_items[k].task_mark_offset == pending_toggle.source_offset) {
    doc->list_items[k].checked = pending_toggle.checked ? 1u : 0u;
   }
  }
  ++doc->revision;
  etch_request_redraw(doc->context);
  if (view.task_toggle_fn) {
   view.task_toggle_fn(pending_toggle.block_index, pending_toggle.item_index, pending_toggle.checked, view.task_toggle_user_data);
  }
 }
 return result;
}
}
