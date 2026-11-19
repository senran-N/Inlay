# Changelog

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
