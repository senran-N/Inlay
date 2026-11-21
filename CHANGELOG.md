# Changelog

## [0.11.0] - 2026-11-22

- Require Etch 0.25 and upgrade the public C99 ABI (`include/inlay/inlay_c.h`) with
  `INLAY_ABI_VERSION`, `INLAY_API` export macros, `inlay_abi_version()`, and versioned
  `uint32_t struct_size` descriptors across all public C input structs.
- Vendor `third_party/md4c` (v0.6.0 MIT) to power a full CommonMark + GFM nested AST
  (nested lists, blockquotes/GitHub alerts, tables/code inside containers, reference
  links, autolinks, display math, `<details>`, footnotes, and custom/Mermaid fences)
  with O(1) amortized incremental prefix freezing and byte-by-byte streaming equivalence.
- Add unified character-level cross-block document selection and copy
  (`InlayDocSelection`, `inlay_doc_view_selection`, `inlay_doc_view_set_selection`,
  `inlay_doc_view_select_all`, `inlay_doc_view_clear_selection`,
  `inlay_doc_view_copy_plain_text`, `inlay_doc_view_copy_markdown`, mouse drag across
  blocks, `Ctrl+A`, `Ctrl+C`, and `Escape`).
- Upgrade the Windows WebView2 portal backend with dynamic `dcomp.dll`
  DirectComposition (`ICoreWebView2CompositionController`, DPI-scaled rounded-corner
  and partial viewport clipping, and `SendMouseInput` forwarding) with graceful
  automatic fallback to classic `ICoreWebView2Controller`.

## [0.10.1] - 2026-11-20

- Require Etch 0.24 so downstream `find_package(Inlay 0.10)` consumers adopt Etch 0.24 native file/folder dialogs and configurable desktop drawers cleanly.

## [0.10.0] - 2026-11-20

- Require Etch 0.23 and forward smart sticky-bottom streaming (`InlayDocViewDesc::stick_to_bottom`, `InlayDocResult::at_bottom`, `inlay_doc_view_is_at_bottom`, and `inlay_doc_view_scroll_to_bottom`).
- Add host custom fenced-block rendering hooks (`InlayCustomFenceEstimateFn` / `InlayCustomFenceRenderFn`) keeping claimed custom fences as single unsplit virtualized rows, plus a `InlayCodeHeaderFn` action slot on a unified horizontal code-block header bar.
- Expand multi-language syntax highlighting in fenced code blocks (`//`, `#`, `--`, `/* */` comments; single/double/backtick strings; hex/binary/float literals; C/C++/Rust/TS/JS/Python/Shell/Go keywords; and semantic `+`/`-`/`@@` coloring for `diff`/`patch` blocks).

## [0.9.0] - 2026-11-19

- Require Etch 0.22 and add `InlayDocStyle` (`background` with `has_background`, custom block/text/link/selection/highlight colors, custom `font_id`/`bold_font_id`/`mono_font_id`, `line_height_scale`, and `heading_scale[6]`) with reflow detection and `inlay_doc_measure_height` integration.
- Add `![alt](src)` block and inline image parsing with `InlayImageResolverFn` callback and `[alt]` fallback pills, interactive task-list checkboxes with `InlayTaskToggleFn`, `==highlight==` inline spans, and `[^label]` / `[^label]: text` footnotes.
- Expand `InlaySurface` QuickJS primitives with `ui.scroll`, `ui.progress`, `ui.slider`, `ui.badge`, `ui.divider`, and `ui.image` plus `opacity`, `value`, `min`, `max`, `scrollX`/`scroll_x`, and `scrollY`/`scroll_y` properties.
- Add first-class zero-cast C++20 `etch::Context*` helper overloads and type aliases in `<inlay/inlay.h>`.

## [0.8.0] - 2026-11-19

- Require Etch 0.20 and stabilize streaming document `row_id` without `content_length` so chunked appends preserve `StateMap` and code-block horizontal scroll.
- Evaluate Mermaid diagram layout once per frame in document views while keeping frame-arena source copies safe for deferred drawing.
- Forward full box, button, and text-input style properties (`bg`, `color`, `borderColor`, `borderWidth`, `borderTop`/`Right`/`Bottom`/`Left`, `radius`, `width`, `height`, `padding`, `fontSize`, `alignX`/`justifyContent`, `alignY`/`alignItems`) in `InlaySurface`.

## [0.7.0] - 2026-11-18

- Require Etch 0.19 and add auto-height document flow with a cached height query.
- Render contiguous GFM tables with column alignment and configurable spacing.
- Add inline code and link decorators, optional code copy buttons, and italic emphasis.
- Initialize WebView2 STA COM in the backend and support default DLL discovery.

## [0.6.0] - 2026-11-18

- Follow the active Etch theme in documents and diagrams, reserve scrollbar
  clearance, and make unwrapped code content horizontally scrollable.
- Support nested inline styles, strikethrough, emphasis, and escaped table pipes.
- Add Mermaid edge labels, dashed arrows, and flowchart subgraph syntax.
- Add controlled input and toggle nodes with JSON change events and zero idle JS ticks.
- Add Release CI for CPU tests, C99, guardrails, and downstream consumers.

## [0.5.0] - 2026-11-18

- Require Etch 0.18 for the pre-1.0 cross-repository release candidate.
- Enforce root/source artifact hygiene with isolated negative guardrail fixtures.
- Verify stable portal frame-arena use, idle sleeping and zero repeat uploads
  over 32 frames; retain the normal timing budgets in non-ASan builds.
- Add an ASan-specific native teardown timing allowance while retaining VM
  interruption, error recovery and memory checks.

## [0.4.0] - 2026-11-17
- Add public portal backend ABI, dirty tile retries, texture/underlay views, transformed
  geometry, focus/capture routing, explicit occlusion policy and IME coordinates.
- Add deterministic synthetic tiles/caret/animation and optional asynchronous native
  WebView2 underlay with safe cancellation and platform-disabled stubs.
- Add integrated Cromma workbench and dual-backend portal/composition regressions.
- Resolve default native math color from the active theme for light-mode contrast.

## [0.3.0] - 2026-11-17

- Add optional QuickJS-NG sandboxed reactive surfaces with memory, stack and tick limits.
- Cache native UI nodes between events, resolve live theme tokens, bind typed host actions and preserve JSON state through file HMR.
- Cover timeout/OOM recovery, zero idle JS ticks, timers, native label lifetimes and C99/C++ consumption.

## [0.2.0] - 2026-11-17

- Add frame-arena native math measurement and drawing with actual font baselines, fractions, radicals, scripts, accents, matrices, stretchy delimiters and Greek/operator symbols.
- Parse inline and display math with byte-wise streaming and render Mermaid flowcharts and sequence diagrams inside native document views.
- Add bounded iterative graph layout with feedback-edge reversal, longest-path ranks, barycentric ordering, LR/TD coordinates, arrowheads and optional interactive Etch canvas.
- Cover mathematical metrics, malformed recursion, fence streaming, cyclic graphs, self-loop bounds, sequence participants and C99/C++ consumers.

## [0.1.0] - 2026-11-17

- Add the public C99 document API and C++20 aliases, CMake install/export and sibling Etch integration.
- Validate streamed UTF-8 with incomplete-codepoint carry; freeze completed blocks and reparse only the open tail.
- Render selectable paragraphs, headings, inline bold/code/links, fenced code with copy and syntax colors, task/list markers, quote alerts, tables, thematic rules and details disclosures.
- Virtualize width/font-bounded render rows with persistent per-view measurements and shared details state across continuation rows.
- Cover byte-wise 32KB Unicode streaming, source self-append, frozen-block stability, append scroll anchors, idle scheduling, C99 ABI and installed/subproject consumers.
