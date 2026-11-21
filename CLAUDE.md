# Inlay Architecture Constitution (v0.11+ Blueprint)
Inlay is a public C99 API consumer of Etch (with optional zero-cast C++20 convenience aliases) designed as a production-grade native rich-content & agent-workbench stack:

- **Document Engine (`md4c` CommonMark+GFM AST + Streaming Equivalence)**:
  - Owns UTF-8 source and flat POD AST/block pools. Appends validate UTF-8 once and retain incomplete sequences in a 4-byte carry.
  - Uses pure-C `third_party/md4c` for full CommonMark + GFM nested block & inline parsing (nested lists, blockquotes containing lists/tables/code/alerts, reference links, autolinks, strikethrough, task lists, math spans/blocks, footnotes, images) while preserving streaming chunk equivalence via incremental tail reparse over frozen completed prefixes.
  - Supports unified document-wide character-level cross-block selection and clean plain-text / Markdown clipboard copy, cached table column widths, dynamic view pools, and lockstep horizontal scrolling + cross-chunk lexical highlighting for long code blocks.
- **Native Math Engine (`src/inlay_math.cpp`)**:
  - TeX 4-level style hierarchy (`Display`/`Text`/`Script`/`ScriptScript` + `cramped`), 7-class `MathClass` operator spacing, italic variables, Display above/below limits on big operators centered on the math axis, matrices/`aligned`/`cases`, single-pass prepared frame-arena layout, and baseline-aligned inline math.
- **Native Diagram Engine (`src/inlay_diagram.cpp`)**:
  - Sugiyama layered layout with distinct node shapes, nested `subgraph` cluster frames, virtual dummy nodes for multi-rank edges, alternating barycentric sweeps, cross-axis rank centering, multi-lane feedback routing, and sequence diagrams.
- **Sandboxed QuickJS-NG Reactive Surfaces (`src/inlay_ui.cpp`)**:
  - Optional behind `INLAY_ENABLE_SCRIPT`. Owns persistent bounded native node/text/handler pools.
  - Zero-idle timer deadline (`inlay_surface_next_timer_ms`), stable keyed & sibling-scoped node IDs (`id` > `key` > `(slot, kind)`), first-class JS closure handlers (`onClick`/`onChange`) + backward-compatible `onEvent`, lightweight built-in `useState` hooks, single-tick event batching, and lazy state JSON serialization.
- **Windowless Portals & DirectComposition WebView2 (`src/inlay_portal*.cpp`)**:
  - Separates host occlusion policy (`host_input_enabled`) from geometric visibility (`geom_visible`).
  - Windows native WebView2 backend uses atomic COM reference counting, rounded DPI bounds, and prefers `ICoreWebView2CompositionController` + DirectComposition (`IDCompositionRectangleClip` with real rounded/partial clipping) with automatic fallback to classic `ICoreWebView2Controller` + `SetWindowRgn`.
- **Packaging & ABI**:
  - Out-of-source Release builds with zero warnings (`/W4 /WX`). Versioned pure C99 ABI (`INLAY_API`, `struct_size`), matching versions across CMake, `inlay_c.h`, and `CHANGELOG.md`.
