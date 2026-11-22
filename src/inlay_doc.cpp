#include "inlay_internal.h"
#include <md4c.h>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {
constexpr uint32_t max_bytes = 64u * 1024u * 1024u;

bool reserve_source(InlayDoc* d, uint32_t needed) noexcept {
 if (needed <= d->source_capacity) return true;
 uint32_t cap = d->source_capacity ? d->source_capacity : 4096u;
 while (cap < needed) cap = cap < max_bytes / 2u ? cap * 2u : max_bytes;
 void* data = std::realloc(d->source, cap);
 if (!data) return false;
 d->source = static_cast<char*>(data);
 d->source_capacity = cap;
 return true;
}

bool reserve_blocks(InlayDoc* d, uint32_t needed) noexcept {
 if (needed <= d->block_capacity) return true;
 uint32_t cap = d->block_capacity ? d->block_capacity : 64u;
 while (cap < needed) cap *= 2u;
 auto* blks = static_cast<InlayBlock*>(std::realloc(d->blocks, size_t(cap) * sizeof(InlayBlock)));
 if (!blks) return false;
 d->blocks = blks;
 auto* meta = static_cast<InlayBlockMeta*>(std::realloc(d->block_meta, size_t(cap) * sizeof(InlayBlockMeta)));
 if (!meta) return false;
 d->block_meta = meta;
 d->block_capacity = cap;
 return true;
}

bool reserve_ast(InlayDoc* d, uint32_t needed) noexcept {
 if (needed <= d->ast_node_capacity) return true;
 uint32_t cap = d->ast_node_capacity ? d->ast_node_capacity : 64u;
 while (cap < needed) cap *= 2u;
 auto* mem = static_cast<InlayAstNode*>(std::realloc(d->ast_nodes, size_t(cap) * sizeof(InlayAstNode)));
 if (!mem) return false;
 d->ast_nodes = mem;
 d->ast_node_capacity = cap;
 return true;
}

bool reserve_list_items(InlayDoc* d, uint32_t needed) noexcept {
 if (needed <= d->list_item_capacity) return true;
 uint32_t cap = d->list_item_capacity ? d->list_item_capacity : 64u;
 while (cap < needed) cap *= 2u;
 auto* mem = static_cast<InlayListItemMeta*>(std::realloc(d->list_items, size_t(cap) * sizeof(InlayListItemMeta)));
 if (!mem) return false;
 d->list_items = mem;
 d->list_item_capacity = cap;
 return true;
}

bool reserve_ast_text(InlayDoc* d, uint32_t needed) noexcept {
 if (needed <= d->ast_text_capacity) return true;
 uint32_t cap = d->ast_text_capacity ? d->ast_text_capacity : 1024u;
 while (cap < needed) cap *= 2u;
 auto* mem = static_cast<char*>(std::realloc(d->ast_text, cap));
 if (!mem) return false;
 d->ast_text = mem;
 d->ast_text_capacity = cap;
 return true;
}

uint32_t append_ast_text(InlayDoc* d, const char* s, uint32_t n) noexcept {
 if (!n) return d->ast_text_size;
 if (!reserve_ast_text(d, d->ast_text_size + n)) return UINT32_MAX;
 const uint32_t off = d->ast_text_size;
 std::memcpy(d->ast_text + off, s, n);
 d->ast_text_size += n;
 return off;
}

bool reserve_ref_defs(InlayDoc* d, uint32_t needed) noexcept {
 if (needed <= d->ref_def_capacity) return true;
 uint32_t cap = d->ref_def_capacity ? d->ref_def_capacity : 16u;
 while (cap < needed) cap *= 2u;
 auto* mem = static_cast<InlayRefDef*>(std::realloc(d->ref_defs, size_t(cap) * sizeof(InlayRefDef)));
 if (!mem) return false;
 d->ref_defs = mem;
 d->ref_def_capacity = cap;
 return true;
}

bool starts(const char* text, uint32_t length, const char* prefix) noexcept {
 const size_t n = std::strlen(prefix);
 return n <= length && std::memcmp(text, prefix, n) == 0;
}

bool is_blank_line(const char* s, uint32_t len) noexcept {
 for (uint32_t i = 0; i < len; ++i) {
  if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return false;
 }
 return true;
}

uint32_t line_start_of(const char* src, uint32_t pos, uint32_t min_pos) noexcept {
 while (pos > min_pos && src[pos - 1] != '\n') --pos;
 return pos;
}

uint32_t line_end_of(const char* src, uint32_t pos, uint32_t max_pos) noexcept {
 while (pos < max_pos && src[pos] != '\n') ++pos;
 return pos;
}

uint32_t line_end_with_nl(const char* src, uint32_t pos, uint32_t max_pos) noexcept {
 const uint32_t e = line_end_of(src, pos, max_pos);
 return e < max_pos ? e + 1u : e;
}

uint32_t excl_end_to_line_end(const char* src, uint32_t first_off, uint32_t excl_end, uint32_t max_pos) noexcept {
 if (excl_end > first_off && src[excl_end - 1u] == '\n') return excl_end - 1u;
 return line_end_of(src, excl_end, max_pos);
}

uint32_t excl_end_to_line_end_nl(const char* src, uint32_t first_off, uint32_t excl_end, uint32_t max_pos) noexcept {
 if (excl_end > first_off && src[excl_end - 1u] == '\n') return excl_end;
 return line_end_with_nl(src, excl_end, max_pos);
}

bool is_code_fence_open(const char* line, uint32_t len, char* out_char, uint32_t* out_count, uint32_t* out_indent) noexcept {
 uint32_t ind = 0;
 while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
 if (ind >= len || (line[ind] != '`' && line[ind] != '~')) return false;
 const char ch = line[ind];
 uint32_t cnt = 0;
 while (ind + cnt < len && line[ind + cnt] == ch) ++cnt;
 if (cnt < 3u) return false;
 if (ch == '`') {
  for (uint32_t i = ind + cnt; i < len; ++i) {
   if (line[i] == '`') return false;
  }
 }
 if (out_char) *out_char = ch;
 if (out_count) *out_count = cnt;
 if (out_indent) *out_indent = ind;
 return true;
}

bool is_code_fence_close(const char* line, uint32_t len, char fence_char, uint32_t fence_len) noexcept {
 uint32_t ind = 0;
 while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
 uint32_t cnt = 0;
 while (ind + cnt < len && line[ind + cnt] == fence_char) ++cnt;
 if (cnt < fence_len) return false;
 for (uint32_t i = ind + cnt; i < len; ++i) {
  if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return false;
 }
 return true;
}

bool is_thematic_break_line(const char* line, uint32_t len) noexcept {
 uint32_t ind = 0;
 while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
 if (ind >= len) return false;
 const char ch = line[ind];
 if (ch != '-' && ch != '*' && ch != '_') return false;
 uint32_t marks = 0;
 for (uint32_t i = ind; i < len; ++i) {
  if (line[i] == ch) ++marks;
  else if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return false;
 }
 return marks >= 3u;
}

bool is_list_starter_line(const char* line, uint32_t len) noexcept {
 uint32_t ind = 0;
 while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
 const char* s = line + ind;
 const uint32_t n = len - ind;
 if (n >= 2u && (s[0] == '-' || s[0] == '*' || s[0] == '+') && (s[1] == ' ' || s[1] == '\t')) return true;
 if (n == 1u && (s[0] == '-' || s[0] == '*' || s[0] == '+')) return true;
 uint32_t d = 0;
 while (d < n && d < 9u && s[d] >= '0' && s[d] <= '9') ++d;
 return d > 0u && d < n && (s[d] == '.' || s[d] == ')') && (d + 1u == n || s[d + 1u] == ' ' || s[d + 1u] == '\t');
}

bool is_footnote_def_line(const char* line, uint32_t len, uint32_t* out_indent,
                          uint32_t* out_label_off, uint32_t* out_label_len, uint32_t* out_body_off) noexcept {
 uint32_t ind = 0;
 while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
 const char* s = line + ind;
 const uint32_t n = len - ind;
 if (n < 4u || s[0] != '[' || s[1] != '^') return false;
 uint32_t k = 2u;
 while (k < n && s[k] != ']' && s[k] != ' ' && s[k] != '\t' && s[k] != '[' && s[k] != '\r') ++k;
 if (k <= 2u || k + 1u >= n || s[k] != ']' || s[k + 1u] != ':') return false;
 uint32_t body = k + 2u;
 while (body < n && (s[body] == ' ' || s[body] == '\t')) ++body;
 if (out_indent) *out_indent = ind;
 if (out_label_off) *out_label_off = ind + 2u;
 if (out_label_len) *out_label_len = k - 2u;
 if (out_body_off) *out_body_off = ind + body;
 return true;
}

uint32_t normalize_ref_label(const char* src, uint32_t len, char* dst, uint32_t cap) noexcept {
 uint32_t i = 0, out = 0;
 while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) ++i;
 bool prev_space = false;
 while (i < len && out < cap) {
  char c = src[i++];
  if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
   prev_space = true;
   continue;
  }
  if (prev_space && out > 0u) {
   if (out >= cap) break;
   dst[out++] = ' ';
  }
  prev_space = false;
  if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
  dst[out++] = c;
 }
 return out;
}

bool line_looks_like_ref_def(const char* line, uint32_t len) noexcept {
 uint32_t ind = 0;
 while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
 if (ind + 4u > len || line[ind] != '[') return false;
 if (ind + 1u < len && line[ind + 1u] == '^') return false;
 for (uint32_t i = ind + 1u; i + 1u < len; ++i) {
  if (line[i] == '\\' && i + 1u < len) { ++i; continue; }
  if (line[i] == '[') return false;
  if (line[i] == ']') return line[i + 1u] == ':';
 }
 return false;
}

void scan_ref_defs(InlayDoc* d) noexcept {
 d->ref_def_count = 0;
 uint32_t pos = 0;
 bool in_code = false;
 char fence_ch = 0;
 uint32_t fence_len = 0;
 bool in_math = false;
 bool math_bracket = false;
 bool in_details = false;
 while (pos < d->source_size) {
  uint32_t end = line_end_of(d->source, pos, d->source_size);
  uint32_t len = end - pos;
  if (len && d->source[end - 1u] == '\r') --len;
  const char* line = d->source + pos;
  const uint32_t after = end < d->source_size ? end + 1u : end;
  if (in_code) {
   if (is_code_fence_close(line, len, fence_ch, fence_len)) in_code = false;
   pos = after;
   continue;
  }
  if (in_math) {
   uint32_t ind = 0; while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
   if (starts(line + ind, len - ind, math_bracket ? "\\]" : "$$")) in_math = false;
   pos = after;
   continue;
  }
  if (in_details) {
   uint32_t ind = 0; while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
   if (starts(line + ind, len - ind, "</details>")) in_details = false;
   pos = after;
   continue;
  }
  if (is_code_fence_open(line, len, &fence_ch, &fence_len, nullptr)) {
   in_code = true;
   pos = after;
   continue;
  }
  {
   uint32_t ind = 0; while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
   const char* s = line + ind; const uint32_t n = len - ind;
   if (starts(s, n, "$$") || starts(s, n, "\\[")) {
    const bool br = s[0] == '\\';
    uint32_t tr = n; while (tr && (s[tr - 1u] == ' ' || s[tr - 1u] == '\t')) --tr;
    if (!(tr >= 4u && std::memcmp(s + tr - 2u, br ? "\\]" : "$$", 2) == 0)) {
     in_math = true; math_bracket = br;
    }
    pos = after;
    continue;
   }
   if (starts(s, n, "<details")) {
    in_details = true;
    pos = after;
    continue;
   }
  }
  uint32_t ind = 0;
  while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
  if (ind < len && line[ind] == '[' && !(ind + 1u < len && line[ind + 1u] == '^')) {
   uint32_t close = ind + 1u;
   bool valid = false;
   while (close + 1u < len) {
    if (line[close] == '\\' && close + 1u < len) { close += 2u; continue; }
    if (line[close] == '[') break;
    if (line[close] == ']' && line[close + 1u] == ':') { valid = true; break; }
    ++close;
   }
   if (valid && close > ind + 1u) {
    char norm[256];
    const uint32_t nlen = normalize_ref_label(line + ind + 1u, close - (ind + 1u), norm, sizeof(norm));
    if (nlen > 0u) {
     uint32_t p = pos + close + 2u;
     while (p < d->source_size && (d->source[p] == ' ' || d->source[p] == '\t')) ++p;
     if (p < d->source_size && d->source[p] == '\r') ++p;
     if (p < d->source_size && d->source[p] == '\n') {
      ++p;
      while (p < d->source_size && (d->source[p] == ' ' || d->source[p] == '\t')) ++p;
     }
     uint32_t url_off = 0, url_len = 0;
     if (p < d->source_size && d->source[p] == '<') {
      ++p;
      url_off = p;
      while (p < d->source_size && d->source[p] != '>' && d->source[p] != '\n') ++p;
      if (p < d->source_size && d->source[p] == '>') url_len = p - url_off;
     } else if (p < d->source_size && d->source[p] != '\n') {
      url_off = p;
      int parens = 0;
      while (p < d->source_size && d->source[p] != ' ' && d->source[p] != '\t' &&
             d->source[p] != '\r' && d->source[p] != '\n') {
       if (d->source[p] == '\\' && p + 1u < d->source_size) { p += 2u; continue; }
       if (d->source[p] == '(') ++parens;
       else if (d->source[p] == ')') { if (parens == 0) break; --parens; }
       ++p;
      }
      url_len = p - url_off;
     }
     if (url_len > 0u && !inlay_doc_find_ref_def(d, line + ind + 1u, close - (ind + 1u))) {
      const uint32_t loff = append_ast_text(d, norm, nlen);
      if (loff != UINT32_MAX && reserve_ref_defs(d, d->ref_def_count + 1u)) {
       d->ref_defs[d->ref_def_count++] = {loff, nlen, url_off, url_len, false};
      }
     }
    }
   }
  }
  pos = after;
 }
}

int32_t alloc_ast_node(InlayDoc* d, InlayAstKind kind, int32_t parent) noexcept {
 if (!reserve_ast(d, d->ast_node_count + 1u)) return -1;
 const int32_t idx = int32_t(d->ast_node_count++);
 InlayAstNode& n = d->ast_nodes[idx];
 n = {};
 n.kind = kind;
 n.parent = parent;
 n.first_child = -1;
 n.next_sibling = -1;
 n.block_index = UINT32_MAX;
 n.task_mark_offset = UINT32_MAX;
 if (parent >= 0 && uint32_t(parent) < d->ast_node_count) {
  int32_t* link = &d->ast_nodes[parent].first_child;
  while (*link >= 0) link = &d->ast_nodes[*link].next_sibling;
  *link = idx;
 }
 return idx;
}

uint8_t map_alert_kind(const MD_CHAR* s, MD_SIZE n) noexcept {
 if (n == 4 && !std::memcmp(s, "note", 4)) return INLAY_ALERT_NOTE;
 if (n == 3 && !std::memcmp(s, "tip", 3)) return INLAY_ALERT_TIP;
 if (n == 9 && !std::memcmp(s, "important", 9)) return INLAY_ALERT_IMPORTANT;
 if (n == 7 && !std::memcmp(s, "warning", 7)) return INLAY_ALERT_WARNING;
 if (n == 7 && !std::memcmp(s, "caution", 7)) return INLAY_ALERT_CAUTION;
 return INLAY_ALERT_NOTE;
}

const char* alert_tag_name(uint8_t k) noexcept {
 switch (k) {
  case INLAY_ALERT_TIP: return "[!TIP]";
  case INLAY_ALERT_IMPORTANT: return "[!IMPORTANT]";
  case INLAY_ALERT_WARNING: return "[!WARNING]";
  case INLAY_ALERT_CAUTION: return "[!CAUTION]";
  default: return "[!NOTE]";
 }
}

struct AstExtra {
 uint32_t content_off, content_len;
 uint32_t own_off, own_len;
 uint32_t lang_off, lang_len;
 uint32_t pool_off, pool_len;
 uint32_t img_src_off, img_src_len;
 uint32_t fence_len;
 bool synthetic, closed_nl;
};

struct MdFrame {
 int32_t ast_idx;
 MD_BLOCKTYPE type;
 uint32_t next_ordinal;
 uint32_t first_text_off, last_text_end;
 uint32_t own_first_off, own_last_end;
 bool has_text, has_own_text;
 uint32_t img_count, non_img_count;
 bool in_img;
 uint32_t img_alt_off, img_alt_len, img_src_off, img_src_len;
 uint32_t code_pool_off, code_pool_len;
 uint32_t lang_off, lang_len;
 char fence_char;
 uint32_t table_cols;
};

struct MdBuilder {
 InlayDoc* doc;
 uint32_t seg_start, seg_end;
 bool at_eof;
 uint32_t ast_base;
 uint32_t scan_cursor;
 AstExtra* extras;
 uint32_t extra_cap;
 MdFrame stack[64];
 uint32_t sp;
 uint16_t quote_depth, list_depth;
 uint8_t alert_stack[32];
 uint32_t alert_sp;
 bool failed;

 bool ensure_extra(uint32_t rel_idx) noexcept {
  if (rel_idx < extra_cap) return true;
  uint32_t cap = extra_cap ? extra_cap * 2u : 64u;
  while (cap <= rel_idx) cap *= 2u;
  auto* mem = static_cast<AstExtra*>(std::realloc(extras, size_t(cap) * sizeof(AstExtra)));
  if (!mem) { failed = true; return false; }
  std::memset(mem + extra_cap, 0, size_t(cap - extra_cap) * sizeof(AstExtra));
  extras = mem;
  extra_cap = cap;
  return true;
 }

 AstExtra& extra_for(int32_t ast_idx) noexcept {
  return extras[uint32_t(ast_idx) - ast_base];
 }

 uint8_t current_alert() const noexcept {
  return alert_sp ? alert_stack[alert_sp - 1u] : INLAY_ALERT_NONE;
 }
};

void propagate_range_up(MdBuilder* b, uint32_t off, uint32_t end) noexcept {
 for (uint32_t i = 0; i < b->sp; ++i) {
  auto& f = b->stack[i];
  if (!f.has_text || off < f.first_text_off) f.first_text_off = off;
  if (!f.has_text || end > f.last_text_end) f.last_text_end = end;
  f.has_text = true;
 }
}

int md_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
 auto* b = static_cast<MdBuilder*>(userdata);
 if (b->failed) return -1;
 if (type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY || type == MD_BLOCK_TR ||
     type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
  return 0;
 }
 if (b->sp >= 64u) return 0;
 InlayAstKind ak = InlayAstParagraph;
 uint8_t alert_k = b->current_alert();
 uint8_t flags = 0;
 uint16_t level = 0;
 uint32_t ordinal = 0;
 uint32_t task_off = UINT32_MAX;
 uint32_t next_ord = 1u;
 if (b->quote_depth) flags |= INLAY_BLOCK_FLAG_IN_QUOTE;
 if (alert_k != INLAY_ALERT_NONE) flags |= INLAY_BLOCK_FLAG_IN_ALERT;
 if (b->list_depth) flags |= INLAY_BLOCK_FLAG_IN_LIST;

 switch (type) {
  case MD_BLOCK_DOC: ak = InlayAstDoc; break;
  case MD_BLOCK_QUOTE:
   ak = InlayAstQuote;
   ++b->quote_depth;
   break;
  case MD_BLOCK_ADMONITION: {
   ak = InlayAstAlert;
   ++b->quote_depth;
   const auto* det = static_cast<const MD_BLOCK_ADMONITION_DETAIL*>(detail);
   alert_k = (det && det->type.text) ? map_alert_kind(det->type.text, det->type.size) : INLAY_ALERT_NOTE;
   if (b->alert_sp < 32u) b->alert_stack[b->alert_sp++] = alert_k;
   flags |= INLAY_BLOCK_FLAG_IN_ALERT;
   break;
  }
  case MD_BLOCK_UL:
   ak = InlayAstUl;
   ++b->list_depth;
   break;
  case MD_BLOCK_OL: {
   ak = InlayAstOl;
   ++b->list_depth;
   flags |= INLAY_BLOCK_FLAG_ORDERED;
   const auto* det = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
   ordinal = det ? det->start : 1u;
   next_ord = ordinal;
   break;
  }
  case MD_BLOCK_LI: {
   ak = InlayAstLi;
   const auto* det = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
   if (b->sp > 0u && b->stack[b->sp - 1u].type == MD_BLOCK_OL) {
    flags |= INLAY_BLOCK_FLAG_ORDERED;
    ordinal = b->stack[b->sp - 1u].next_ordinal++;
   }
   if (det && det->is_task) {
    flags |= INLAY_BLOCK_FLAG_TASK;
    if (det->task_mark == 'x' || det->task_mark == 'X') flags |= INLAY_BLOCK_FLAG_CHECKED;
    task_off = b->seg_start + det->task_mark_offset;
   }
   break;
  }
  case MD_BLOCK_H: {
   ak = InlayAstHeading;
   const auto* det = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
   level = det ? uint16_t(det->level) : 1u;
   break;
  }
  case MD_BLOCK_CODE: ak = InlayAstCode; break;
  case MD_BLOCK_TABLE: ak = InlayAstTable; break;
  case MD_BLOCK_HR: ak = InlayAstThematicBreak; break;
  case MD_BLOCK_P: ak = InlayAstParagraph; break;
  case MD_BLOCK_HTML: ak = InlayAstParagraph; break;
  default: ak = InlayAstParagraph; break;
 }

 const int32_t parent = b->sp > 0u ? b->stack[b->sp - 1u].ast_idx : -1;
 const int32_t idx = alloc_ast_node(b->doc, ak, parent);
 if (idx < 0 || !b->ensure_extra(uint32_t(idx) - b->ast_base)) {
  b->failed = true;
  return -1;
 }
 auto& node = b->doc->ast_nodes[idx];
 node.alert_kind = alert_k;
 node.flags = flags;
 node.depth = b->sp > 0u ? uint16_t(b->sp - 1u) : 0u;
 node.quote_depth = b->quote_depth;
 node.list_depth = b->list_depth;
 node.level = level;
 node.ordinal = ordinal;
 node.task_mark_offset = task_off;

 MdFrame f = {};
 f.ast_idx = idx;
 f.type = type;
 f.next_ordinal = next_ord;
 f.first_text_off = UINT32_MAX;
 f.own_first_off = UINT32_MAX;
 if (type == MD_BLOCK_CODE) {
  f.code_pool_off = b->doc->ast_text_size;
  if (detail) {
   const auto* det = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
   f.fence_char = det->fence_char;
   if (det->lang.text && det->lang.size > 0u) {
    const char* base = b->doc->source;
    if (det->lang.text >= base && det->lang.text + det->lang.size <= base + b->doc->source_size) {
     f.lang_off = uint32_t(det->lang.text - base);
     f.lang_len = det->lang.size;
    }
   }
  }
 } else if (type == MD_BLOCK_TABLE && detail) {
  const auto* det = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail);
  f.table_cols = det->col_count;
 }
 if (type == MD_BLOCK_LI && task_off != UINT32_MAX) {
  propagate_range_up(b, task_off, task_off + 1u);
  f.first_text_off = task_off;
  f.last_text_end = task_off + 1u;
  f.own_first_off = task_off;
  f.own_last_end = task_off + 1u;
  f.has_text = true;
  f.has_own_text = true;
 }
 b->stack[b->sp++] = f;
 return 0;
}

int md_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
 auto* b = static_cast<MdBuilder*>(userdata);
 if (!b->sp) return 0;
 auto& f = b->stack[b->sp - 1u];
 if (f.type == MD_BLOCK_P) {
  if (type == MD_SPAN_IMG) {
   ++f.img_count;
   f.in_img = true;
   f.img_alt_off = UINT32_MAX;
   f.img_alt_len = 0;
   const auto* det = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
   if (det && det->src.text) {
    const char* base = b->doc->source;
    if (det->src.text >= base && det->src.text + det->src.size <= base + b->doc->source_size) {
     f.img_src_off = uint32_t(det->src.text - base);
     f.img_src_len = det->src.size;
     propagate_range_up(b, f.img_src_off, f.img_src_off + f.img_src_len);
    }
   }
  } else if (!f.in_img) {
   ++f.non_img_count;
  }
 }
 return 0;
}

int md_leave_span(MD_SPANTYPE type, void*, void* userdata) {
 auto* b = static_cast<MdBuilder*>(userdata);
 if (!b->sp) return 0;
 auto& f = b->stack[b->sp - 1u];
 if (f.type == MD_BLOCK_P && type == MD_SPAN_IMG) {
  f.in_img = false;
 }
 return 0;
}

int md_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
 auto* b = static_cast<MdBuilder*>(userdata);
 if (b->failed || !b->sp || !text || !size) return 0;
 auto& f = b->stack[b->sp - 1u];
 const char* base = b->doc->source;
 const bool in_src = text >= base && text + size <= base + b->doc->source_size;
 const uint32_t off = in_src ? uint32_t(text - base) : UINT32_MAX;

 if (f.type == MD_BLOCK_CODE) {
  const auto& node = b->doc->ast_nodes[f.ast_idx];
  if (node.quote_depth > 0u || node.list_depth > 0u) {
   const uint32_t po = append_ast_text(b->doc, text, size);
   if (po == UINT32_MAX) { b->failed = true; return -1; }
   if (f.code_pool_len == 0u) f.code_pool_off = po;
   f.code_pool_len += size;
  }
  if (in_src) propagate_range_up(b, off, off + size);
  return 0;
 }

 if (f.type == MD_BLOCK_P) {
  if (f.in_img) {
   if (in_src) {
    if (f.img_alt_off == UINT32_MAX) {
     f.img_alt_off = off;
     f.img_alt_len = size;
    } else {
     f.img_alt_len = (off + size) - f.img_alt_off;
    }
   }
  } else if (type != MD_TEXT_SOFTBR) {
   ++f.non_img_count;
  }
 }

 if (in_src) {
  if (!f.has_own_text || off < f.own_first_off) f.own_first_off = off;
  if (!f.has_own_text || off + size > f.own_last_end) f.own_last_end = off + size;
  f.has_own_text = true;
  propagate_range_up(b, off, off + size);
 }
 return 0;
}

uint32_t strip_quote_prefix_on_line(const char* src, uint32_t line_start, uint32_t line_end, uint16_t max_quotes) noexcept {
 uint32_t p = line_start;
 for (uint16_t q = 0; q < max_quotes && p < line_end; ++q) {
  uint32_t ind = 0;
  while (p + ind < line_end && ind < 3u && src[p + ind] == ' ') ++ind;
  if (p + ind < line_end && src[p + ind] == '>') {
   p += ind + 1u;
   if (p < line_end && src[p] == ' ') ++p;
  } else {
   break;
  }
 }
 return p;
}

int md_leave_block(MD_BLOCKTYPE type, void*, void* userdata) {
 auto* b = static_cast<MdBuilder*>(userdata);
 if (b->failed) return -1;
 if (type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY || type == MD_BLOCK_TR ||
     type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
  return 0;
 }
 if (!b->sp) return 0;
 MdFrame f = b->stack[--b->sp];
 auto& node = b->doc->ast_nodes[f.ast_idx];
 auto& ex = b->extra_for(f.ast_idx);
 const char* src = b->doc->source;

 if (type == MD_BLOCK_QUOTE) {
  if (b->quote_depth) --b->quote_depth;
 } else if (type == MD_BLOCK_ADMONITION) {
  if (b->quote_depth) --b->quote_depth;
  if (b->alert_sp) --b->alert_sp;
 } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
  if (b->list_depth) --b->list_depth;
 }

 if (type == MD_BLOCK_HR) {
  uint32_t p = b->scan_cursor;
  while (p < b->seg_end) {
   uint32_t e = line_end_of(src, p, b->seg_end);
   uint32_t l = e - p;
   if (l && src[e - 1u] == '\r') --l;
   const uint32_t stripped = strip_quote_prefix_on_line(src, p, p + l, node.quote_depth);
   const uint32_t after = e < b->seg_end ? e + 1u : e;
   if (is_thematic_break_line(src + stripped, (p + l) - stripped)) {
    node.src_offset = p;
    node.src_length = after - p;
    ex.content_off = stripped;
    ex.content_len = (p + l) - stripped;
    ex.closed_nl = e < b->seg_end;
    b->scan_cursor = after;
    propagate_range_up(b, p, after);
    return 0;
   }
   p = after;
  }
  return 0;
 }

 if (type == MD_BLOCK_CODE) {
  uint32_t open_line = UINT32_MAX;
  if (f.lang_len > 0u) {
   open_line = line_start_of(src, f.lang_off, b->seg_start);
  } else if (f.has_text) {
   uint32_t first_line = line_start_of(src, f.first_text_off, b->seg_start);
   if (first_line > b->seg_start) {
    uint32_t prev = line_start_of(src, first_line - 1u, b->seg_start);
    uint32_t pe = line_end_of(src, prev, b->seg_end);
    uint32_t pl = pe - prev; if (pl && src[pe - 1u] == '\r') --pl;
    const uint32_t st = strip_quote_prefix_on_line(src, prev, prev + pl, node.quote_depth);
    char fc = 0; uint32_t fl = 0;
    if (is_code_fence_open(src + st, (prev + pl) - st, &fc, &fl, nullptr)) {
     open_line = prev;
    }
   }
  }
  if (open_line == UINT32_MAX) {
   uint32_t p = b->scan_cursor;
   while (p < b->seg_end) {
    uint32_t e = line_end_of(src, p, b->seg_end);
    uint32_t l = e - p; if (l && src[e - 1u] == '\r') --l;
    const uint32_t st = strip_quote_prefix_on_line(src, p, p + l, node.quote_depth);
    if (is_code_fence_open(src + st, (p + l) - st, nullptr, nullptr, nullptr)) {
     open_line = p;
     break;
    }
    p = e < b->seg_end ? e + 1u : e;
   }
  }
  if (open_line == UINT32_MAX) open_line = b->scan_cursor;
  uint32_t open_e = line_end_of(src, open_line, b->seg_end);
  uint32_t open_l = open_e - open_line;
  if (open_l && src[open_e - 1u] == '\r') --open_l;
  const uint32_t open_st = strip_quote_prefix_on_line(src, open_line, open_line + open_l, node.quote_depth);
  char fc = '`'; uint32_t fl = 3u; uint32_t fi = 0;
  if (is_code_fence_open(src + open_st, (open_line + open_l) - open_st, &fc, &fl, &fi)) {
   ex.fence_len = fl;
   if (f.lang_len == 0u) {
    uint32_t lo = open_st + fi + fl;
    uint32_t le = open_line + open_l;
    while (lo < le && (src[lo] == ' ' || src[lo] == '\t')) ++lo;
    while (le > lo && (src[le - 1u] == ' ' || src[le - 1u] == '\t')) --le;
    if (le > lo) { f.lang_off = lo; f.lang_len = le - lo; }
   }
  }
  const uint32_t after_open = open_e < b->seg_end ? open_e + 1u : open_e;
  ex.lang_off = f.lang_off;
  ex.lang_len = f.lang_len;
  uint32_t block_end = after_open;
  if (node.quote_depth == 0u && node.list_depth == 0u) {
   // Scan directly in source from after_open to matching closing fence line for exact byte fidelity.
   ex.content_off = after_open;
   ex.content_len = 0u;
   uint32_t scan = after_open;
   while (scan < b->seg_end) {
    const uint32_t ce = line_end_of(src, scan, b->seg_end);
    uint32_t cl = ce - scan; if (cl && src[ce - 1u] == '\r') --cl;
    const uint32_t cafter = ce < b->seg_end ? ce + 1u : ce;
    if (is_code_fence_close(src + scan, cl, fc, fl)) {
     ex.content_len = scan - after_open;
     block_end = cafter;
     ex.closed_nl = ce < b->seg_end;
     break;
    }
    ex.content_len = cafter - after_open;
    block_end = cafter;
    scan = cafter;
   }
  } else {
   ex.synthetic = true;
   ex.pool_off = f.code_pool_off;
   ex.pool_len = f.code_pool_len;
   ex.content_off = after_open;
   ex.content_len = f.code_pool_len;
   block_end = f.has_text ? excl_end_to_line_end_nl(src, f.first_text_off, f.last_text_end, b->seg_end) : after_open;
   if (block_end < b->seg_end) {
    const uint32_t ce = line_end_of(src, block_end, b->seg_end);
    uint32_t cl = ce - block_end; if (cl && src[ce - 1u] == '\r') --cl;
    const uint32_t cst = strip_quote_prefix_on_line(src, block_end, block_end + cl, node.quote_depth);
    if (is_code_fence_close(src + cst, (block_end + cl) - cst, fc, fl)) {
     block_end = ce < b->seg_end ? ce + 1u : ce;
     ex.closed_nl = ce < b->seg_end;
    }
   }
  }
  node.src_offset = open_line;
  node.src_length = block_end - open_line;
  if (block_end > b->scan_cursor) b->scan_cursor = block_end;
  propagate_range_up(b, open_line, block_end);
  return 0;
 }

 if (type == MD_BLOCK_H) {
  if (!f.has_text) return 0;
  const uint32_t ls = line_start_of(src, f.first_text_off, b->seg_start);
  const uint32_t le = excl_end_to_line_end(src, f.first_text_off, f.last_text_end, b->seg_end);
  const uint32_t after = le < b->seg_end ? le + 1u : le;
  const uint32_t st = strip_quote_prefix_on_line(src, ls, le, node.quote_depth);
  uint32_t p = st;
  while (p < le && p - st < 3u && src[p] == ' ') ++p;
  if (p < le && src[p] == '#') {
   uint32_t h = 0;
   while (p + h < le && src[p + h] == '#') ++h;
   uint32_t co = p + h;
   if (co < le && (src[co] == ' ' || src[co] == '\t')) ++co;
   uint32_t ce = le;
   if (ce > co && src[ce - 1u] == '\r') --ce;
   node.src_offset = ls;
   node.src_length = after - ls;
   ex.content_off = co;
   ex.content_len = ce > co ? ce - co : 0u;
   ex.closed_nl = le < b->seg_end;
   if (after > b->scan_cursor) b->scan_cursor = after;
   propagate_range_up(b, ls, after);
  } else {
   // Setext heading: consume the underline line below.
   uint32_t ul_end = after < b->seg_end ? line_end_with_nl(src, after, b->seg_end) : after;
   uint32_t ce = le;
   if (ce > st && src[ce - 1u] == '\r') --ce;
   node.src_offset = ls;
   node.src_length = ul_end - ls;
   ex.content_off = st;
   ex.content_len = ce > st ? ce - st : 0u;
   ex.closed_nl = ul_end <= b->seg_end && ul_end > after && src[ul_end - 1u] == '\n';
   if (ul_end > b->scan_cursor) b->scan_cursor = ul_end;
   propagate_range_up(b, ls, ul_end);
  }
  return 0;
 }

 if (type == MD_BLOCK_TABLE) {
  if (!f.has_text) return 0;
  const uint32_t ls = line_start_of(src, f.first_text_off, b->seg_start);
  uint32_t te = excl_end_to_line_end_nl(src, f.first_text_off, f.last_text_end, b->seg_end);
  // If table has only a header row (no body rows), include the delimiter rule line right after header.
  const uint32_t hdr_after = line_end_with_nl(src, ls, b->seg_end);
  if (te <= hdr_after && hdr_after < b->seg_end) {
   te = line_end_with_nl(src, hdr_after, b->seg_end);
  }
  node.src_offset = ls;
  node.src_length = te - ls;
  ex.content_off = ls;
  ex.content_len = te - ls;
  ex.closed_nl = te > ls && src[te - 1u] == '\n';
  if (te > b->scan_cursor) b->scan_cursor = te;
  propagate_range_up(b, ls, te);
  return 0;
 }

 if (type == MD_BLOCK_P || type == MD_BLOCK_HTML) {
  if (!f.has_text) return 0;
  const uint32_t ls = line_start_of(src, f.first_text_off, b->seg_start);
  const uint32_t le = excl_end_to_line_end(src, f.first_text_off, f.last_text_end, b->seg_end);
  const uint32_t after = le < b->seg_end ? le + 1u : le;
  node.src_offset = ls;
  node.src_length = after - ls;
  ex.content_off = ls;
  ex.content_len = after - ls;
  ex.closed_nl = le < b->seg_end;
  // Promote standalone ![alt](src) top-level paragraph to INLAY_IMAGE.
  if (type == MD_BLOCK_P && node.quote_depth == 0u && node.list_depth == 0u &&
      f.img_count == 1u && f.non_img_count == 0u && f.img_src_len > 0u) {
   uint32_t line_len = le - ls;
   if (line_len && src[le - 1u] == '\r') --line_len;
   uint32_t a = ls, c = ls + line_len;
   while (a < c && (src[a] == ' ' || src[a] == '\t')) ++a;
   while (c > a && (src[c - 1u] == ' ' || src[c - 1u] == '\t')) --c;
   if (c - a >= 5u && src[a] == '!' && src[a + 1u] == '[' && src[c - 1u] == ')') {
    node.kind = InlayAstImage;
    ex.content_off = f.img_alt_off != UINT32_MAX ? f.img_alt_off : a + 2u;
    ex.content_len = f.img_alt_off != UINT32_MAX ? f.img_alt_len : 0u;
    ex.img_src_off = f.img_src_off;
    ex.img_src_len = f.img_src_len;
   }
  }
  if (after > b->scan_cursor) b->scan_cursor = after;
  propagate_range_up(b, ls, after);
  return 0;
 }

 // Containers: MD_BLOCK_QUOTE, MD_BLOCK_ADMONITION, MD_BLOCK_UL, MD_BLOCK_OL, MD_BLOCK_LI
 if (type == MD_BLOCK_ADMONITION) {
  // Include the opening `> [!TYPE]` line before the first child text!
  uint32_t start = b->scan_cursor;
  if (f.has_text) {
   uint32_t cur = line_start_of(src, f.first_text_off, b->seg_start);
   while (cur > b->seg_start) {
    uint32_t prev = line_start_of(src, cur - 1u, b->seg_start);
    uint32_t pe = line_end_of(src, prev, b->seg_end);
    uint32_t pl = pe - prev; if (pl && src[pe - 1u] == '\r') --pl;
    const uint32_t st = strip_quote_prefix_on_line(src, prev, prev + pl, uint16_t(node.quote_depth));
    if (starts(src + st, (prev + pl) - st, "[!")) {
     start = prev;
     break;
    }
    if (st == prev) break;
    cur = prev;
   }
   const uint32_t end = excl_end_to_line_end_nl(src, f.first_text_off, f.last_text_end, b->seg_end);
   node.src_offset = start;
   node.src_length = end > start ? end - start : 0u;
   ex.content_off = strip_quote_prefix_on_line(src, start, end, uint16_t(node.quote_depth));
   ex.content_len = end > ex.content_off ? end - ex.content_off : 0u;
   if (end > b->scan_cursor) b->scan_cursor = end;
   propagate_range_up(b, start, end);
  } else {
   // Empty admonition containing only `> [!NOTE]`
   uint32_t p = b->scan_cursor;
   while (p < b->seg_end) {
    uint32_t e = line_end_of(src, p, b->seg_end);
    uint32_t l = e - p; if (l && src[e - 1u] == '\r') --l;
    const uint32_t st = strip_quote_prefix_on_line(src, p, p + l, uint16_t(node.quote_depth));
    const uint32_t after = e < b->seg_end ? e + 1u : e;
    if (starts(src + st, (p + l) - st, "[!")) {
     node.src_offset = p;
     node.src_length = after - p;
     ex.content_off = st;
     ex.content_len = after - st;
     b->scan_cursor = after;
     propagate_range_up(b, p, after);
     break;
    }
    p = after;
   }
  }
  return 0;
 }

 if (f.has_own_text) {
  const uint32_t ols = line_start_of(src, f.own_first_off, b->seg_start);
  const uint32_t oend = excl_end_to_line_end_nl(src, f.own_first_off, f.own_last_end, b->seg_end);
  ex.own_off = ols;
  ex.own_len = oend - ols;
 }
 if (f.has_text) {
  const uint32_t ls = line_start_of(src, f.first_text_off, b->seg_start);
  const uint32_t end = excl_end_to_line_end_nl(src, f.first_text_off, f.last_text_end, b->seg_end);
  node.src_offset = ls;
  node.src_length = end - ls;
  ex.content_off = (type == MD_BLOCK_QUOTE)
   ? strip_quote_prefix_on_line(src, ls, end, uint16_t(node.quote_depth))
   : ls;
  ex.content_len = end > ex.content_off ? end - ex.content_off : 0u;
  if (end > b->scan_cursor) b->scan_cursor = end;
  propagate_range_up(b, ls, end);
 }
 return 0;
}

bool emit_block(InlayDoc* d, const InlayBlock& blk, const InlayBlockMeta& meta) noexcept {
 if (!reserve_blocks(d, d->block_count + 1u)) return false;
 const uint32_t bi = d->block_count++;
 d->blocks[bi] = blk;
 d->block_meta[bi] = meta;
 if (meta.ast_index >= 0 && uint32_t(meta.ast_index) < d->ast_node_count) {
  d->ast_nodes[meta.ast_index].block_index = bi;
 }
 return true;
}

bool subtree_is_pure_list(const InlayDoc* d, int32_t idx) noexcept {
 const auto& n = d->ast_nodes[idx];
 if (n.kind != InlayAstUl && n.kind != InlayAstOl) return false;
 for (int32_t c = n.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
  const auto& li = d->ast_nodes[c];
  if (li.kind != InlayAstLi) return false;
  uint32_t para_count = 0;
  for (int32_t gc = li.first_child; gc >= 0; gc = d->ast_nodes[gc].next_sibling) {
   const auto& g = d->ast_nodes[gc];
   if (g.kind == InlayAstParagraph) {
    ++para_count;
    if (para_count > 1u) return false;
    // Multi-line single paragraph inside a tight list is still fine only if it's 1 line;
    // if multi-line continuation exists we still render cleanly, so check newlines.
   } else if (g.kind == InlayAstUl || g.kind == InlayAstOl) {
    if (!subtree_is_pure_list(d, gc)) return false;
   } else {
    return false;
   }
  }
 }
 return true;
}

void collect_list_items_recursive(MdBuilder* b, int32_t list_idx) noexcept {
 InlayDoc* d = b->doc;
 const auto& list = d->ast_nodes[list_idx];
 for (int32_t c = list.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
  const auto& li = d->ast_nodes[c];
  if (li.kind != InlayAstLi) continue;
  // Find direct paragraph child (if any) for this item's own text line(s).
  int32_t p_idx = -1;
  for (int32_t gc = li.first_child; gc >= 0; gc = d->ast_nodes[gc].next_sibling) {
   if (d->ast_nodes[gc].kind == InlayAstParagraph) { p_idx = gc; break; }
  }
  const auto& li_ex = b->extra_for(c);
  const uint32_t line_off = p_idx >= 0 ? d->ast_nodes[p_idx].src_offset
   : (li_ex.own_len > 0u ? li_ex.own_off : li.src_offset);
  const uint32_t line_len = p_idx >= 0 ? d->ast_nodes[p_idx].src_length
   : (li_ex.own_len > 0u ? li_ex.own_len : li.src_length);
  if (line_len > 0u && reserve_list_items(d, d->list_item_count + 1u)) {
   InlayListItemMeta m = {};
   m.line_offset = line_off;
   m.line_length = line_len;
   m.content_offset = line_off;
   m.content_length = line_len;
   m.ordinal = li.ordinal;
   m.task_mark_offset = li.task_mark_offset;
   m.ast_index = c;
   m.list_depth = li.list_depth;
   m.ordered = (li.flags & INLAY_BLOCK_FLAG_ORDERED) ? 1u : 0u;
   m.is_task = (li.flags & INLAY_BLOCK_FLAG_TASK) ? 1u : 0u;
   m.checked = (li.flags & INLAY_BLOCK_FLAG_CHECKED) ? 1u : 0u;
   d->list_items[d->list_item_count++] = m;
  }
  for (int32_t gc = li.first_child; gc >= 0; gc = d->ast_nodes[gc].next_sibling) {
   const auto& g = d->ast_nodes[gc];
   if (g.kind == InlayAstUl || g.kind == InlayAstOl) {
    collect_list_items_recursive(b, gc);
   }
  }
 }
}

bool list_has_task_item(const InlayDoc* d, int32_t list_idx) noexcept {
 const auto& list = d->ast_nodes[list_idx];
 for (int32_t c = list.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
  if (d->ast_nodes[c].flags & INLAY_BLOCK_FLAG_TASK) return true;
 }
 return false;
}

bool simple_quote_or_alert(const InlayDoc* d, int32_t q_idx) noexcept {
 const auto& q = d->ast_nodes[q_idx];
 if (q.quote_depth != 1u || q.list_depth != 0u) return false;
 const int32_t c = q.first_child;
 if (c < 0) return q.kind == InlayAstAlert;
 const auto& child = d->ast_nodes[c];
 return child.next_sibling < 0 && child.kind == InlayAstParagraph;
}

void project_ast_node(MdBuilder* b, int32_t idx, bool in_complex_li, int32_t li_ast_idx, bool* li_marker_emitted) noexcept {
 InlayDoc* d = b->doc;
 const auto& n = d->ast_nodes[idx];
 const auto& ex = b->extra_for(idx);
 auto make_base = [&](InlayBlockKind kind) noexcept {
  InlayBlock blk = {};
  blk.kind = kind;
  blk.offset = n.src_offset;
  blk.length = n.src_length;
  blk.content_offset = ex.content_off ? ex.content_off : n.src_offset;
  blk.content_length = ex.content_len ? ex.content_len : n.src_length;
  blk.level = n.level;
  blk.parent_index = n.parent;
  blk.depth = n.depth;
  blk.quote_depth = n.quote_depth;
  blk.list_depth = n.list_depth;
  blk.alert_kind = n.alert_kind;
  blk.flags = n.flags;
  blk.list_ordinal = n.ordinal;
  blk.task_mark_offset = n.task_mark_offset;
  return blk;
 };

 switch (n.kind) {
  case InlayAstDoc:
   for (int32_t c = n.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
    project_ast_node(b, c, false, -1, nullptr);
   }
   break;

  case InlayAstQuote:
  case InlayAstAlert: {
   if (simple_quote_or_alert(d, idx)) {
    InlayBlock blk = make_base(n.kind == InlayAstAlert ? INLAY_ALERT : INLAY_BLOCKQUOTE);
    blk.content_offset = ex.content_off;
    blk.content_length = ex.content_len;
    InlayBlockMeta meta = {idx, 0, 0, 0, 0};
    emit_block(d, blk, meta);
    break;
   }
   if (n.kind == InlayAstAlert) {
    // Emit a dedicated alert title header block for complex multi-block alerts!
    const char* tag = alert_tag_name(n.alert_kind);
    const uint32_t tlen = uint32_t(std::strlen(tag));
    uint32_t h_off = ex.content_off;
    uint32_t h_end = line_end_with_nl(d->source, n.src_offset, b->seg_end);
    InlayBlock hdr = make_base(INLAY_ALERT);
    hdr.length = h_end > hdr.offset ? h_end - hdr.offset : 0u;
    if (h_off < h_end && starts(d->source + h_off, h_end - h_off, "[!")) {
     hdr.content_offset = h_off;
     hdr.content_length = h_end - h_off;
     InlayBlockMeta meta = {idx, 0, 0, 0, 0};
     emit_block(d, hdr, meta);
    } else {
     const uint32_t po = append_ast_text(d, tag, tlen);
     if (po != UINT32_MAX) {
      hdr.flags |= INLAY_BLOCK_FLAG_SYNTHETIC;
      hdr.content_offset = 0;
      hdr.content_length = tlen;
      InlayBlockMeta meta = {idx, 0, 0, po, tlen};
      emit_block(d, hdr, meta);
     }
    }
   }
   for (int32_t c = n.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
    project_ast_node(b, c, false, -1, nullptr);
   }
   break;
  }

  case InlayAstUl:
  case InlayAstOl: {
   if (subtree_is_pure_list(d, idx)) {
    const uint32_t first_item = d->list_item_count;
    collect_list_items_recursive(b, idx);
    const uint32_t item_cnt = d->list_item_count - first_item;
    const bool is_task = list_has_task_item(d, idx);
    InlayBlock blk = make_base(is_task ? INLAY_TASK_LIST : INLAY_LIST);
    blk.content_offset = n.src_offset;
    blk.content_length = n.src_length;
    if (is_task) blk.flags |= INLAY_BLOCK_FLAG_TASK;
    InlayBlockMeta meta = {idx, first_item, item_cnt, 0, 0};
    emit_block(d, blk, meta);
   } else {
    for (int32_t c = n.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
     bool emitted = false;
     project_ast_node(b, c, true, c, &emitted);
    }
   }
   break;
  }

  case InlayAstLi: {
   bool emitted = false;
   if (ex.own_len > 0u) {
    // Tight list item with direct text before nested code/table/list children.
    emitted = true;
    const bool is_task = (n.flags & INLAY_BLOCK_FLAG_TASK) != 0;
    const uint32_t first_item = d->list_item_count;
    if (reserve_list_items(d, d->list_item_count + 1u)) {
     InlayListItemMeta m = {};
     m.line_offset = ex.own_off;
     m.line_length = ex.own_len;
     m.content_offset = ex.own_off;
     m.content_length = ex.own_len;
     m.ordinal = n.ordinal;
     m.task_mark_offset = n.task_mark_offset;
     m.ast_index = idx;
     m.list_depth = n.list_depth;
     m.ordered = (n.flags & INLAY_BLOCK_FLAG_ORDERED) ? 1u : 0u;
     m.is_task = is_task ? 1u : 0u;
     m.checked = (n.flags & INLAY_BLOCK_FLAG_CHECKED) ? 1u : 0u;
     d->list_items[d->list_item_count++] = m;
    }
    InlayBlock blk = make_base(is_task ? INLAY_TASK_LIST : INLAY_LIST);
    blk.offset = ex.own_off;
    blk.length = ex.own_len;
    blk.content_offset = ex.own_off;
    blk.content_length = ex.own_len;
    InlayBlockMeta meta = {idx, first_item, d->list_item_count - first_item, 0, 0};
    emit_block(d, blk, meta);
   }
   for (int32_t c = n.first_child; c >= 0; c = d->ast_nodes[c].next_sibling) {
    project_ast_node(b, c, true, idx, &emitted);
   }
   break;
  }

  case InlayAstHeading: {
   InlayBlock blk = make_base(INLAY_HEADING);
   blk.content_offset = ex.content_off;
   blk.content_length = ex.content_len;
   blk.complete = ex.closed_nl;
   InlayBlockMeta meta = {idx, 0, 0, 0, 0};
   emit_block(d, blk, meta);
   break;
  }

  case InlayAstCode: {
   InlayBlock blk = make_base(INLAY_CODE_BLOCK);
   blk.level = ex.fence_len;
   blk.language_offset = ex.lang_off;
   blk.language_length = ex.lang_len;
   blk.complete = ex.closed_nl;
   InlayBlockMeta meta = {idx, 0, 0, 0, 0};
   if (ex.synthetic) {
    blk.flags |= INLAY_BLOCK_FLAG_SYNTHETIC;
    blk.content_offset = 0;
    blk.content_length = ex.pool_len;
    meta.pool_offset = ex.pool_off;
    meta.pool_length = ex.pool_len;
   } else {
    blk.content_offset = ex.content_off;
    blk.content_length = ex.content_len;
   }
   emit_block(d, blk, meta);
   break;
  }

  case InlayAstTable: {
   InlayBlock blk = make_base(INLAY_TABLE);
   blk.content_offset = ex.content_off;
   blk.content_length = ex.content_len;
   InlayBlockMeta meta = {idx, 0, 0, 0, 0};
   emit_block(d, blk, meta);
   break;
  }

  case InlayAstThematicBreak: {
   InlayBlock blk = make_base(INLAY_THEMATIC_BREAK);
   blk.content_offset = ex.content_off;
   blk.content_length = ex.content_len;
   blk.complete = ex.closed_nl;
   InlayBlockMeta meta = {idx, 0, 0, 0, 0};
   emit_block(d, blk, meta);
   break;
  }

  case InlayAstImage: {
   InlayBlock blk = make_base(INLAY_IMAGE);
   blk.content_offset = ex.content_off;
   blk.content_length = ex.content_len;
   blk.language_offset = ex.img_src_off;
   blk.language_length = ex.img_src_len;
   blk.complete = ex.closed_nl;
   InlayBlockMeta meta = {idx, 0, 0, 0, 0};
   emit_block(d, blk, meta);
   break;
  }

  case InlayAstParagraph: {
   if (in_complex_li && li_ast_idx >= 0 && li_marker_emitted && !*li_marker_emitted) {
    *li_marker_emitted = true;
    const auto& li = d->ast_nodes[li_ast_idx];
    const bool is_task = (li.flags & INLAY_BLOCK_FLAG_TASK) != 0;
    const uint32_t first_item = d->list_item_count;
    if (reserve_list_items(d, d->list_item_count + 1u)) {
     InlayListItemMeta m = {};
     m.line_offset = n.src_offset;
     m.line_length = n.src_length;
     m.content_offset = n.src_offset;
     m.content_length = n.src_length;
     m.ordinal = li.ordinal;
     m.task_mark_offset = li.task_mark_offset;
     m.ast_index = li_ast_idx;
     m.list_depth = li.list_depth;
     m.ordered = (li.flags & INLAY_BLOCK_FLAG_ORDERED) ? 1u : 0u;
     m.is_task = is_task ? 1u : 0u;
     m.checked = (li.flags & INLAY_BLOCK_FLAG_CHECKED) ? 1u : 0u;
     d->list_items[d->list_item_count++] = m;
    }
    InlayBlock blk = make_base(is_task ? INLAY_TASK_LIST : INLAY_LIST);
    blk.flags = li.flags;
    blk.list_ordinal = li.ordinal;
    blk.task_mark_offset = li.task_mark_offset;
    blk.list_depth = li.list_depth;
    InlayBlockMeta meta = {idx, first_item, d->list_item_count - first_item, 0, 0};
    emit_block(d, blk, meta);
   } else {
    const InlayBlockKind k = n.quote_depth > 0u
     ? (n.alert_kind != INLAY_ALERT_NONE ? INLAY_ALERT : INLAY_BLOCKQUOTE)
     : INLAY_PARAGRAPH;
    InlayBlock blk = make_base(k);
    blk.content_offset = n.src_offset;
    blk.content_length = n.src_length;
    InlayBlockMeta meta = {idx, 0, 0, 0, 0};
    emit_block(d, blk, meta);
   }
   break;
  }

  default: break;
 }
}

void run_md4c_segment(InlayDoc* d, uint32_t seg_start, uint32_t seg_end, bool at_eof) noexcept {
 if (seg_end <= seg_start) return;
 MdBuilder builder = {};
 builder.doc = d;
 builder.seg_start = seg_start;
 builder.seg_end = seg_end;
 builder.at_eof = at_eof;
 builder.ast_base = d->ast_node_count;
 builder.scan_cursor = seg_start;

 MD_PARSER parser = {};
 parser.abi_version = 0;
 parser.flags = MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
                MD_FLAG_TASKLISTS | MD_FLAG_ADMONITIONS | MD_FLAG_HIGHLIGHT |
                MD_FLAG_NOINDENTEDCODEBLOCKS;
 parser.enter_block = md_enter_block;
 parser.leave_block = md_leave_block;
 parser.enter_span = md_enter_span;
 parser.leave_span = md_leave_span;
 parser.text = md_text;

 const int rc = md_parse(d->source + seg_start, seg_end - seg_start, &parser, &builder);
 if (rc == 0 && !builder.failed && d->ast_node_count > builder.ast_base) {
  project_ast_node(&builder, int32_t(builder.ast_base), false, -1, nullptr);
 }
 std::free(builder.extras);
}

bool has_blank_line_between(const char* src, uint32_t from, uint32_t to, uint32_t* out_after_blank) noexcept {
 uint32_t p = from;
 while (p < to) {
  const uint32_t e = line_end_of(src, p, to);
  if (e >= to) return false;
  uint32_t l = e - p;
  if (l && src[e - 1u] == '\r') --l;
  const uint32_t after = e + 1u;
  if (is_blank_line(src + p, l)) {
   // Consume any consecutive complete blank lines
   uint32_t cur = after;
   while (cur < to) {
    const uint32_t ce = line_end_of(src, cur, to);
    if (ce >= to) break;
    uint32_t cl = ce - cur;
    if (cl && src[ce - 1u] == '\r') --cl;
    if (!is_blank_line(src + cur, cl)) break;
    cur = ce + 1u;
   }
   if (out_after_blank) *out_after_blank = cur;
   return true;
  }
  return false;
 }
 return false;
}

void parse_tail(InlayDoc* d) noexcept {
 if (d->has_ref_defs && d->tail_start == 0u) {
  d->ast_text_size = 0;
  scan_ref_defs(d);
  d->frozen_ast_text_size = d->ast_text_size;
 }
 d->block_count = d->frozen_count;
 d->ast_node_count = d->frozen_ast_count;
 d->list_item_count = d->frozen_list_item_count;
 d->ast_text_size = d->frozen_ast_text_size;

 const uint32_t start_blocks = d->block_count;
 uint32_t pos = d->tail_start;
 uint32_t md_seg_start = pos;
 bool in_code = false;
 char fence_ch = 0;
 uint32_t fence_len = 0;

 while (pos < d->source_size) {
  const uint32_t end = line_end_of(d->source, pos, d->source_size);
  const bool full = end < d->source_size;
  uint32_t len = end - pos;
  if (len && d->source[end - 1u] == '\r') --len;
  const char* line = d->source + pos;
  const uint32_t after = full ? end + 1u : end;

  if (in_code) {
   if (is_code_fence_close(line, len, fence_ch, fence_len)) in_code = false;
   pos = after;
   continue;
  }

  if (is_code_fence_open(line, len, &fence_ch, &fence_len, nullptr)) {
   in_code = true;
   pos = after;
   continue;
  }

  uint32_t ind = 0;
  while (ind < len && ind < 3u && line[ind] == ' ') ++ind;
  const char* s = line + ind;
  const uint32_t n = len - ind;

  // Special block 1: Display math block ($$ ... $$ or \[ ... \])
  if (starts(s, n, "$$") || starts(s, n, "\\[")) {
   if (pos > md_seg_start) run_md4c_segment(d, md_seg_start, pos, false);
   const bool bracket = s[0] == '\\';
   const char* close = bracket ? "\\]" : "$$";
   const uint32_t open_off = pos;
   const uint32_t content_off = pos + ind + 2u;
   uint32_t content_len = 0;
   uint32_t block_end = after;
   bool closed = false;

   uint32_t trimmed = len;
   while (trimmed && (line[trimmed - 1u] == ' ' || line[trimmed - 1u] == '\t')) --trimmed;
   if (trimmed >= ind + 4u && std::memcmp(line + trimmed - 2u, close, 2) == 0) {
    content_len = (pos + trimmed - 2u) - content_off;
    closed = full;
   } else {
    uint32_t scan = after;
    content_len = after - content_off;
    while (scan < d->source_size) {
     const uint32_t se = line_end_of(d->source, scan, d->source_size);
     const bool sfull = se < d->source_size;
     uint32_t sl = se - scan;
     if (sl && d->source[se - 1u] == '\r') --sl;
     const uint32_t safter = sfull ? se + 1u : se;
     uint32_t sind = 0;
     while (sind < sl && sind < 3u && d->source[scan + sind] == ' ') ++sind;
     if (starts(d->source + scan + sind, sl - sind, close)) {
      content_len = scan - content_off;
      block_end = safter;
      closed = sfull;
      break;
     }
     block_end = safter;
     content_len = safter - content_off;
     scan = safter;
    }
   }
   const int32_t ast_idx = alloc_ast_node(d, InlayAstMathBlock, -1);
   if (ast_idx >= 0) {
    d->ast_nodes[ast_idx].src_offset = open_off;
    d->ast_nodes[ast_idx].src_length = block_end - open_off;
   }
   InlayBlock blk = {};
   blk.kind = INLAY_MATH_BLOCK;
   blk.offset = open_off;
   blk.length = block_end - open_off;
   blk.content_offset = content_off;
   blk.content_length = content_len;
   blk.level = bracket ? 1u : 0u;
   blk.complete = closed;
   blk.parent_index = -1;
   blk.task_mark_offset = UINT32_MAX;
   emit_block(d, blk, {ast_idx, 0, 0, 0, 0});
   pos = block_end;
   md_seg_start = block_end;
   continue;
  }

  // Special block 2: <details> ... </details>
  if (starts(s, n, "<details")) {
   if (pos > md_seg_start) run_md4c_segment(d, md_seg_start, pos, false);
   const uint32_t open_off = pos;
   uint32_t block_end = after;
   bool closed = false;
   if (starts(s, n, "</details>") && full) {
    closed = true;
   } else {
    uint32_t scan = after;
    while (scan < d->source_size) {
     const uint32_t se = line_end_of(d->source, scan, d->source_size);
     const bool sfull = se < d->source_size;
     uint32_t sl = se - scan;
     if (sl && d->source[se - 1u] == '\r') --sl;
     const uint32_t safter = sfull ? se + 1u : se;
     uint32_t sind = 0;
     while (sind < sl && sind < 3u && d->source[scan + sind] == ' ') ++sind;
     block_end = safter;
     if (starts(d->source + scan + sind, sl - sind, "</details>")) {
      closed = sfull;
      break;
     }
     scan = safter;
    }
   }
   const int32_t ast_idx = alloc_ast_node(d, InlayAstDetails, -1);
   if (ast_idx >= 0) {
    d->ast_nodes[ast_idx].src_offset = open_off;
    d->ast_nodes[ast_idx].src_length = block_end - open_off;
   }
   InlayBlock blk = {};
   blk.kind = INLAY_DETAILS;
   blk.offset = open_off;
   blk.length = block_end - open_off;
   blk.content_offset = open_off;
   blk.content_length = block_end - open_off;
   blk.complete = closed;
   blk.parent_index = -1;
   blk.task_mark_offset = UINT32_MAX;
   emit_block(d, blk, {ast_idx, 0, 0, 0, 0});
   pos = block_end;
   md_seg_start = block_end;
   continue;
  }

  // Special block 3: Footnote definition [^label]: body (+ 2-space/tab continuation lines)
  uint32_t fn_ind = 0, fn_lbl_off = 0, fn_lbl_len = 0, fn_body_off = 0;
  if (is_footnote_def_line(line, len, &fn_ind, &fn_lbl_off, &fn_lbl_len, &fn_body_off)) {
   if (pos > md_seg_start) run_md4c_segment(d, md_seg_start, pos, false);
   const uint32_t open_off = pos;
   uint32_t block_end = after;
   uint32_t scan = after;
   while (scan < d->source_size) {
    const uint32_t se = line_end_of(d->source, scan, d->source_size);
    const bool sfull = se < d->source_size;
    uint32_t sl = se - scan;
    if (sl && d->source[se - 1u] == '\r') --sl;
    const uint32_t safter = sfull ? se + 1u : se;
    if (sl >= 2u && (d->source[scan] == ' ' || d->source[scan] == '\t') &&
        !is_blank_line(d->source + scan, sl)) {
     block_end = safter;
     scan = safter;
     continue;
    }
    break;
   }
   const int32_t ast_idx = alloc_ast_node(d, InlayAstFootnote, -1);
   if (ast_idx >= 0) {
    d->ast_nodes[ast_idx].src_offset = open_off;
    d->ast_nodes[ast_idx].src_length = block_end - open_off;
   }
   InlayBlock blk = {};
   blk.kind = INLAY_FOOTNOTE;
   blk.offset = open_off;
   blk.length = block_end - open_off;
   blk.language_offset = open_off + fn_lbl_off;
   blk.language_length = fn_lbl_len;
   blk.content_offset = open_off + fn_body_off;
   blk.content_length = block_end - blk.content_offset;
   blk.parent_index = -1;
   blk.task_mark_offset = UINT32_MAX;
   emit_block(d, blk, {ast_idx, 0, 0, 0, 0});
   pos = block_end;
   md_seg_start = block_end;
   continue;
  }

  pos = after;
 }

 if (d->source_size > md_seg_start) {
  run_md4c_segment(d, md_seg_start, d->source_size, true);
 }

 // Finalize `complete` flags for newly parsed blocks and advance incremental freeze boundary.
 for (uint32_t i = start_blocks; i < d->block_count; ++i) {
  auto& blk = d->blocks[i];
  const uint32_t blk_end = blk.offset + blk.length;
  const bool has_next = (i + 1u < d->block_count);
  uint32_t after_blank = blk_end;
  const bool blank_after = has_blank_line_between(d->source, blk_end, d->source_size, &after_blank);
  if (!blk.complete) {
   if (blk.kind == INLAY_CODE_BLOCK || blk.kind == INLAY_MATH_BLOCK || blk.kind == INLAY_DETAILS) {
    // Only complete when explicitly closed with trailing newline (already set above).
   } else if (has_next || blank_after) {
    blk.complete = true;
   }
  }
 }

 // Advance frozen boundary across safe, parse-independent top-level commits!
 // Group consecutive blocks sharing the same top-level AST ancestor (e.g. a complex blockquote or list).
 uint32_t i = start_blocks;
 while (i < d->block_count) {
  // Find the top-level span starting at block i.
  uint32_t j = i + 1u;
  if (d->blocks[i].quote_depth > 0u || d->blocks[i].depth > 0u) {
   int32_t top_ast = d->block_meta[i].ast_index;
   while (top_ast >= 0 && d->ast_nodes[top_ast].parent >= 0 &&
          d->ast_nodes[d->ast_nodes[top_ast].parent].kind != InlayAstDoc) {
    top_ast = d->ast_nodes[top_ast].parent;
   }
   while (j < d->block_count) {
    int32_t next_top = d->block_meta[j].ast_index;
    while (next_top >= 0 && d->ast_nodes[next_top].parent >= 0 &&
           d->ast_nodes[d->ast_nodes[next_top].parent].kind != InlayAstDoc) {
     next_top = d->ast_nodes[next_top].parent;
    }
    if (next_top != top_ast) break;
    ++j;
   }
  }

  // Last block in this top-level unit is j - 1.
  const auto& first_blk = d->blocks[i];
  const auto& last_blk = d->blocks[j - 1u];
  uint32_t group_end = last_blk.offset + last_blk.length;
  for (uint32_t k = i; k < j; ++k) {
   const uint32_t e = d->blocks[k].offset + d->blocks[k].length;
   if (e > group_end) group_end = e;
  }
  uint32_t after_blank = group_end;
  const bool blank_after = has_blank_line_between(d->source, group_end, d->source_size, &after_blank);
  bool can_freeze = false;
  uint32_t freeze_pos = after_blank;

  if (first_blk.quote_depth == 0u && first_blk.depth == 0u && j == i + 1u) {
   const auto k = first_blk.kind;
   if ((k == INLAY_CODE_BLOCK || k == INLAY_MATH_BLOCK || k == INLAY_DETAILS ||
        k == INLAY_HEADING || k == INLAY_THEMATIC_BREAK) && first_blk.complete) {
    can_freeze = true;
    freeze_pos = blank_after ? after_blank : group_end;
   } else if ((k == INLAY_PARAGRAPH || k == INLAY_TABLE || k == INLAY_IMAGE) && blank_after) {
    can_freeze = true;
    freeze_pos = after_blank;
   } else if ((k == INLAY_LIST || k == INLAY_TASK_LIST || k == INLAY_FOOTNOTE) && blank_after && j < d->block_count) {
    // Only freeze a list/footnote after blank line once a non-indented, non-list top-level block follows!
    const uint32_t nl_end = line_end_of(d->source, after_blank, d->source_size);
    uint32_t nl_len = nl_end - after_blank;
    if (nl_len && d->source[nl_end - 1u] == '\r') --nl_len;
    const char* next_line = d->source + after_blank;
    const bool indented = nl_len > 0u && (next_line[0] == ' ' || next_line[0] == '\t');
    if (nl_end < d->source_size && !indented && !is_list_starter_line(next_line, nl_len)) {
     can_freeze = true;
     freeze_pos = after_blank;
    }
   }
  } else if (blank_after) {
   // Top-level blockquote/alert or complex list container followed by blank line.
   const bool is_list_group = (first_blk.quote_depth == 0u && first_blk.list_depth > 0u);
   if (!is_list_group) {
    can_freeze = true;
    freeze_pos = after_blank;
   } else if (j < d->block_count) {
    const uint32_t nl_end = line_end_of(d->source, after_blank, d->source_size);
    uint32_t nl_len = nl_end - after_blank;
    if (nl_len && d->source[nl_end - 1u] == '\r') --nl_len;
    const char* next_line = d->source + after_blank;
    const bool indented = nl_len > 0u && (next_line[0] == ' ' || next_line[0] == '\t');
    if (nl_end < d->source_size && !indented && !is_list_starter_line(next_line, nl_len)) {
     can_freeze = true;
     freeze_pos = after_blank;
    }
   }
  }

  if (!can_freeze) break;

  for (uint32_t k = i; k < j; ++k) d->blocks[k].complete = true;
  // Only advance frozen_count if ALL subsequent AST/list/pool entries belong strictly after j (i.e. j == d->block_count
  // OR we are freezing the prefix cleanly). Since we don't want partial-segment AST truncation mid-parse,
  // compute the exact AST/list/pool watermark for blocks [0..j)!
  if (j == d->block_count) {
   d->frozen_count = j;
   d->frozen_ast_count = d->ast_node_count;
   d->frozen_list_item_count = d->list_item_count;
   d->frozen_ast_text_size = d->ast_text_size;
   d->tail_start = freeze_pos;
  } else {
   // Check if blocks [j..block_count) were produced in the same md4c segment.
   // Keep frozen watermark safely at exact next block's AST/list/pool start.
   const uint32_t next_ast = d->block_meta[j].ast_index >= 0 ? uint32_t(d->block_meta[j].ast_index) : d->ast_node_count;
   bool safe_watermark = true;
   for (uint32_t k = i; k < j; ++k) {
    if (d->block_meta[k].ast_index >= 0 && uint32_t(d->block_meta[k].ast_index) >= next_ast) safe_watermark = false;
   }
   if (safe_watermark && freeze_pos <= d->blocks[j].offset) {
    // Also check no block in [0..j) uses ast_text past next block's pool offset, and keep ast_text watermark safe.
    uint32_t max_li = d->frozen_list_item_count;
    uint32_t max_pool = d->frozen_ast_text_size;
    for (uint32_t k = i; k < j; ++k) {
     const uint32_t li_end = d->block_meta[k].list_item_first + d->block_meta[k].list_item_count;
     if (d->block_meta[k].list_item_count && li_end > max_li) max_li = li_end;
     if (d->blocks[k].flags & INLAY_BLOCK_FLAG_SYNTHETIC) {
      const uint32_t pe = d->block_meta[k].pool_offset + d->block_meta[k].pool_length;
      if (pe > max_pool) max_pool = pe;
     }
    }
    bool pool_ok = true;
    for (uint32_t k = j; k < d->block_count; ++k) {
     if (d->block_meta[k].list_item_count && d->block_meta[k].list_item_first < max_li) pool_ok = false;
     if ((d->blocks[k].flags & INLAY_BLOCK_FLAG_SYNTHETIC) && d->block_meta[k].pool_offset < max_pool) pool_ok = false;
    }
    if (pool_ok) {
     d->frozen_count = j;
     d->frozen_ast_count = next_ast;
     d->frozen_list_item_count = max_li;
     d->frozen_ast_text_size = max_pool;
     d->tail_start = freeze_pos;
    } else {
     break;
    }
   } else {
    break;
   }
  }
  i = j;
 }
}
} // namespace

const InlayRefDef* inlay_doc_find_ref_def(const InlayDoc* doc, const char* label, uint32_t len) noexcept {
 if (!doc || !doc->ref_def_count || !label || !len) return nullptr;
 char norm[256];
 const uint32_t nlen = normalize_ref_label(label, len, norm, sizeof(norm));
 if (!nlen) return nullptr;
 for (uint32_t i = 0; i < doc->ref_def_count; ++i) {
  const auto& r = doc->ref_defs[i];
  if (r.norm_label_len == nlen && std::memcmp(doc->ast_text + r.norm_label_off, norm, nlen) == 0) {
   return &r;
  }
 }
 return nullptr;
}

extern "C" {
INLAY_API InlayDoc* inlay_doc_create(EtchContext* context, const InlayDocConfig* config) {
 if (!context) return nullptr;
 InlayDocConfig c = inlay_doc_config_default();
 if (config && !inlay_read_abi_struct(config, &c)) return nullptr;
 if (c.initial_source_bytes == 0u) c.initial_source_bytes = 4096u;
 if (c.initial_blocks == 0u) c.initial_blocks = 64u;
 if (c.initial_source_bytes < 4u || c.initial_source_bytes > max_bytes || c.initial_blocks > max_bytes) return nullptr;
 auto* d = static_cast<InlayDoc*>(std::calloc(1, sizeof(InlayDoc)));
 if (!d) return nullptr;
 d->context = context;
 d->source_capacity = c.initial_source_bytes;
 d->block_capacity = c.initial_blocks;
 d->source = static_cast<char*>(std::malloc(d->source_capacity));
 d->blocks = static_cast<InlayBlock*>(std::malloc(size_t(d->block_capacity) * sizeof(InlayBlock)));
 d->block_meta = static_cast<InlayBlockMeta*>(std::calloc(d->block_capacity, sizeof(InlayBlockMeta)));
 if (!d->source || !d->blocks || !d->block_meta) {
  inlay_doc_destroy(d);
  return nullptr;
 }
 return d;
}

INLAY_API void inlay_doc_destroy(InlayDoc* d) {
 if (!d) return;
 for (uint32_t i = 0; i < d->view_count; ++i) {
  auto& view = d->views[i];
  std::free(view.rows);
  std::free(view.heights);
  std::free(view.open);
  std::free(view.code_wrap);
  std::free(view.code_scroll_x);
  std::free(view.diagram_full_scale);
  std::free(view.copied_until_ms);
  std::free(view.details);
  std::free(view.table_cols);
  std::free(view.plain_text);
  std::free(view.vis_segs);
  std::free(view.vis_row_ids);
  std::free(view.vis_ctrl_ids);
  std::free(view.vis_span_ids);
  std::free(view.tokens);
 }
 std::free(d->views);
 std::free(d->source);
 std::free(d->blocks);
 std::free(d->block_meta);
 std::free(d->ast_nodes);
 std::free(d->list_items);
 std::free(d->ast_text);
 std::free(d->ref_defs);
 std::free(d);
}

INLAY_API void inlay_doc_clear(InlayDoc* d) {
 if (!d) return;
 d->source_size = 0;
 d->block_count = 0;
 d->frozen_count = 0;
 d->tail_start = 0;
 d->ast_node_count = 0;
 d->frozen_ast_count = 0;
 d->list_item_count = 0;
 d->frozen_list_item_count = 0;
 d->ast_text_size = 0;
 d->frozen_ast_text_size = 0;
 d->ref_def_count = 0;
 d->has_ref_defs = false;
 d->utf8_carry_len = 0;
 for (uint32_t i = 0; i < d->view_count; ++i) {
  auto& view = d->views[i];
  view.width = 0;
  view.table_cols_count = 0;
  view.plain_len = 0;
  view.vis_seg_count = 0;
  view.doc_sel_active = false;
  view.doc_sel_dragging = false;
  view.doc_sel_pending_span = false;
 }
 ++d->revision;
 etch_request_redraw(d->context);
}

INLAY_API bool inlay_doc_append_utf8(InlayDoc* d, EtchStr chunk) {
 if (!d || (!chunk.chars && chunk.length) || chunk.length > max_bytes - d->source_size - d->utf8_carry_len) return false;
 const uintptr_t ptr = reinterpret_cast<uintptr_t>(chunk.chars), base = reinterpret_cast<uintptr_t>(d->source);
 const bool aliases = ptr >= base && ptr - base < d->source_size;
 const uint32_t source_offset = aliases ? uint32_t(ptr - base) : 0u;
 if (aliases && chunk.length > d->source_size - source_offset) return false;
 const uint32_t total = d->utf8_carry_len + chunk.length;
 const auto byte = [&](uint32_t i) noexcept { return uint8_t(i < d->utf8_carry_len ? d->utf8_carry[i] : chunk.chars[i - d->utf8_carry_len]); };
 uint32_t valid = 0, newlines = 0;
 while (valid < total) {
  const uint8_t lead = byte(valid);
  uint32_t n = 1, cp = lead;
  if (lead >= 0xc2 && lead <= 0xdf) { n = 2; cp = lead & 31; }
  else if (lead >= 0xe0 && lead <= 0xef) { n = 3; cp = lead & 15; }
  else if (lead >= 0xf0 && lead <= 0xf4) { n = 4; cp = lead & 7; }
  else if (lead >= 0x80 || lead == 0) return false;
  for (uint32_t j = 1; j < n && valid + j < total; ++j) if ((byte(valid + j) & 0xc0) != 0x80) return false;
  if (total - valid < n) break;
  for (uint32_t j = 1; j < n; ++j) cp = (cp << 6) | (byte(valid + j) & 63);
  if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  if (lead == '\n') ++newlines;
  valid += n;
 }
 if (!reserve_source(d, d->source_size + valid) || !reserve_blocks(d, d->block_count + newlines + 4u)) return false;
 if (aliases) chunk.chars = d->source + source_offset;
 char carry[4] = {};
 const uint32_t carry_count = total - valid;
 for (uint32_t i = 0; i < carry_count; ++i) carry[i] = char(byte(valid + i));
 const uint32_t old_size = d->source_size;
 for (uint32_t i = 0; i < valid; ++i) d->source[d->source_size + i] = char(byte(i));
 d->source_size += valid;
 d->utf8_carry_len = uint8_t(carry_count);
 std::memcpy(d->utf8_carry, carry, 4);

 // If any newly touched line introduces a CommonMark reference link definition, reparse from start
 // so forward references in earlier frozen blocks resolve identically to one-shot parsing!
 if (valid > 0u) {
  uint32_t scan = line_start_of(d->source, old_size, 0u);
  bool found_ref = false;
  while (scan < d->source_size) {
   const uint32_t le = line_end_of(d->source, scan, d->source_size);
   uint32_t ll = le - scan;
   if (ll && d->source[le - 1u] == '\r') --ll;
   if (line_looks_like_ref_def(d->source + scan, ll)) { found_ref = true; break; }
   scan = le < d->source_size ? le + 1u : le;
  }
  if (found_ref) {
   d->has_ref_defs = true;
   d->frozen_count = 0;
   d->frozen_ast_count = 0;
   d->frozen_list_item_count = 0;
   d->frozen_ast_text_size = 0;
   d->tail_start = 0;
   for (uint32_t vi = 0; vi < d->view_count; ++vi) {
    d->views[vi].width = 0;
   }
  }
 }

 parse_tail(d);
 ++d->revision;
 etch_request_redraw(d->context);
 return true;
}

INLAY_API bool inlay_doc_set_utf8(InlayDoc* d, EtchStr text) {
 if (!d) return false;
 inlay_doc_clear(d);
 return inlay_doc_append_utf8(d, text);
}

INLAY_API uint32_t inlay_doc_block_count(const InlayDoc* d) { return d ? d->block_count : 0u; }
INLAY_API const InlayBlock* inlay_doc_block(const InlayDoc* d, uint32_t i) { return d && i < d->block_count ? d->blocks + i : nullptr; }
INLAY_API EtchStr inlay_doc_source(const InlayDoc* d) { return d ? EtchStr{d->source, d->source_size, false} : EtchStr{}; }
}
