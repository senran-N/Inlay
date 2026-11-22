# Inlay Architecture Constitution

## 1. Strategic Identity & Relationship to Etch
`Inlay` (`inlay::`, C99 prefix `inlay_` / `Inlay`, macro `INLAY_`, target `Inlay::inlay`) is a strict public C99 API consumer of `Etch` (with optional zero-cast C++20 convenience aliases). Together, **Etch + Inlay** deliver a **self-controlled, zero-Electron, C99-embeddable native tech stack capable of rapidly building high-aesthetic desktop applications**:
- **`Etch`** provides the ultra-light, high-visual-ceiling GPU rendering, layout, text, input/IME, A11y, and motion kernel.
- **`Inlay`** delivers two co-equal pillars on top of Etch's public C99 ABI:
  1. **High-Velocity High-Aesthetic Declarative & HMR Layer (`inlay_ui`)**: Enables developers (and AI agents) to rapidly author Linear / macOS / shadcn-grade native desktop UIs and safe generative UI cards with `< 2 ms` hot reload — without recompiling C++ and without shipping Electron.
  2. **Publication-Grade Zero-Browser Native Rich Content Stack (`inlay_doc`, `inlay_math`, `inlay_diagram`, `inlay_portal`)**: Native streaming Markdown/GFM, native TeX math, native Mermaid diagrams, and on-demand native WebView portals.

---

## 2. Pillar 1: High-Velocity Declarative & HMR UI Layer (`src/inlay_ui.cpp`)
- Optional behind `INLAY_ENABLE_SCRIPT` (powered by MIT pure-C `third_party/quickjs-ng` with hard memory limit, stack limit, and tick interrupt watchdog; zero `std`/`os`/filesystem/network globals).
- **Full High-Aesthetic Parity with Etch** (Non-Negotiable):
  - `inlay_ui` MUST NOT be a stripped-down toy subset that hides Etch's visual power. Every visual and layout capability exposed in `<etch/etch_c.h>` must be cleanly reachable from declarative views:
    - **Visual styling**: multi-layer & inset shadows (`elevation` / `shadow`), multi-stop gradients, optical vibrancy `backdropBlur`, independent corner radii & continuous corners, per-side borders, `opacity`, 2D affine transforms (`translate`, `scale`, `rotate`), hover/press/focus style overrides, and spring/easing `transition`s.
    - **Layout**: full Flexbox (`row`/`col`, `wrap`, `flexGrow`/`flexShrink`, `justify`, `alignItems`, `alignSelf`, `gap`, `margin`, per-side `padding`, `min/max` sizing, `aspectRatio`), 2D Grid (`gridCols`, `gridRows`, `colSpan`, `rowSpan`), and floating anchor positioning.
    - **Design System & Shorthands**: First-class semantic theme tokens (`var(--etch-*)`, elevation/radius/typography presets) and concise ergonomic shorthands so clean, modern UIs take dozens of lines instead of verbose boilerplate.
    - **Composite & Native Bridge Elements**: Standard modern UI primitives (`box`, `text`, `icon`, `image`, `button`, `input`, `toggle`, `slider`, `progress`, `badge`, `divider`, `scroll`, `card`, `tabs`, `popover`, `modal`, `tooltip`, `split`, `vlist`) PLUS first-class native embedding of Inlay/Etch's unique engines (`doc`, `math`, `diagram`, `terminal`, `shaderSurface`, `portal`).
- **Performance & Safety Invariants**:
  - Idle frames reuse cached native POD node pools with **zero JavaScript execution** and zero per-frame heap allocations.
  - Sub-2ms HMR engine reload preserves state; bounded one-shot timers expose exact wake deadlines (`inlay_surface_next_timer_ms`) for 0%-CPU idle sleep.
  - Stable keyed/sibling-scoped IDs (`id` > `key` > `(slot, kind)`), first-class JS closure handlers (`onClick`/`onChange`), `useState` hooks, single-tick event batching, and safe inline error recovery.

---

## 3. Pillar 2: Publication-Grade Native Rich Content Stack
- **Document Engine (`src/inlay_doc.cpp`, `src/inlay_view.cpp`, `third_party/md4c`)**:
  - Owns UTF-8 source and flat POD AST/block pools. Appends validate UTF-8 once with 4-byte incomplete-sequence carry.
  - Uses pure-C `third_party/md4c` for full CommonMark + GFM nested block & inline parsing (nested lists, blockquotes/GitHub alerts, tables, fenced code, task lists, math spans/blocks, footnotes, images, details) with incremental prefix freezing and strict byte-by-byte streaming equivalence.
  - Unified cross-block character selection & plain-text/Markdown copy, virtualized long-document rendering, sticky-bottom LLM streaming scroll, custom fenced-block hooks, and multi-language syntax highlighting.
- **Native Math Engine (`src/inlay_math.cpp`)**:
  - TeX 4-level style hierarchy (`Display`/`Text`/`Script`/`ScriptScript` + `cramped`), 7-class `MathClass` operator spacing, optical italic variables, balanced superscript/subscript kerning, clean radical curves, big operator limits centered on the math axis, matrices/`aligned`/`cases`, single-pass frame-arena layout, and baseline-aligned inline math.
- **Native Diagram Engine (`src/inlay_diagram.cpp`)**:
  - Sugiyama layered layout with distinct node shapes, nested `subgraph` cluster frames, virtual dummy nodes, barycentric crossing reduction, multi-lane feedback routing, clean edge-label pill avoidance, and sequence diagrams.
- **Windowless Portals & DirectComposition WebView2 (`src/inlay_portal*.cpp`)**:
  - Clean on-demand escape hatch for full web content: separates host occlusion policy (`host_input_enabled`) from geometric visibility (`geom_visible`), prefers `ICoreWebView2CompositionController` + DirectComposition rounded/partial clipping on Windows with automatic classic fallback, and supports texture/underlay C99 backend vtables for macOS/Linux.

---

## 4. Visual Craft, Memory & Packaging Standards
- **Publication-Grade Visual Craft (Enforced on Every Render Change)**:
  - Never judge visual output merely by "non-zero pixels" or "no crash". Inspect actual Light and Dark headless renders for typographic rhythm, heading/body/code scale contrast, comfortable spacing (task lists, tables, code cards, callouts), crisp math proportions, refined Mermaid node elevation/contrast, and seamless light/dark integration (no raw unstyled blocks pasted onto light surfaces).
- **Memory Budget**:
  - Inlay's own overhead on top of Etch MUST remain minimal (`< 10 MB` typical for full Workbench with Doc + Math + Diagram + QuickJS-NG). Persistent pools grow geometrically and stay bounded; per-frame drawing allocates strictly from Etch's frame arena.
- **Packaging & ABI**:
  - Out-of-source Release builds with zero warnings (`/W4 /WX`). Versioned pure C99 ABI (`INLAY_API`, `INLAY_ABI_VERSION`, `struct_size` descriptors) including only `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, and `<etch/etch_c.h>`. Matching versions across `CMakeLists.txt`, `include/inlay/inlay_c.h`, and `CHANGELOG.md`.
