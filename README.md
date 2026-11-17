# Inlay
Native streamed documents on Etch, through its public C99 API. Build with CMake
and Ninja in `build/`; a sibling `../Etch` is used when Etch 0.16.1 is not installed.
C99 includes `<inlay/inlay_c.h>`; C++20 may include `<inlay/inlay.h>`.

Create a document with an Etch context, append UTF-8 chunks, and call
`inlay_doc_view` during that context's frame. Four carry bytes bridge incomplete
codepoints. Invalid input is rejected; clear the document to discard a pending
partial sequence. Frozen blocks retain source offsets while the open tail is
reparsed. Appending the document's own source is supported. Returned slices and
block pointers are borrowed until the next mutation. The context must outlive
documents; do not mutate documents during a view call. Up to 16 view IDs per
document retain stable virtual-list measurement slots.

Views provide selectable text, code pills, bold spans, clickable links, headings,
quoted alerts, task/list markers, native table rows, code-copy buttons with basic
syntax colors, thematic rules and details disclosures. The document model is a
bounded GFM subset, not a general HTML interpreter. Code uses unwrapped lines;
long lines are currently clipped. Nested inline markup and escaped table pipes
are not yet supported. Render rows adapt to view width and font size. Frozen row measurements survive
tail appends; persistent per-view pools grow geometrically as the document grows.

`inlay_document --headless output.png` renders a deterministic document sample.
`inlay_document --stress output.png` checks a 100KiB paragraph and 900-line code block.
Run CTest to verify streaming equivalence, C99, guardrails and downstream builds.

Native math uses `inlay_measure_math` / `inlay_draw_math` during an Etch frame.
Coordinates and returned sizes use logical pixels; `pos` is the formula's top left,
and `baseline` is measured from that top. Supported commands include Greek/operator
symbols, fractions, roots, scripts, accents, matrix/aligned environments and
stretchy delimiters. Unsupported syntax reports `valid=false`; document views
show the original source. Inline `$...$` and `\(...\)` and display `$$` / `\[` fences
render natively. These APIs require Etch 0.16.1 or newer for native font metrics.

`inlay_diagram_layout` returns frame-lifetime node/edge spans; `inlay_diagram_view`
draws them with native geometry and text. Flowcharts accept TD/TB/LR, named nodes,
labels in brackets, directed `-->` chains and cycles; sequence diagrams accept
participants, `as` names and `->>` messages. Inputs are bounded to 16KiB, 256 nodes
and 1024 edges. Use `canvas_id` for an interactive pan/zoom canvas, or call inside
an existing Etch canvas. This is a documented Mermaid subset, not the complete grammar.
`inlay_document --math` and `--diagram` render standalone native examples.


Reactive surfaces use optional QuickJS-NG (`INLAY_ENABLE_SCRIPT=ON`). Create an
`InlaySurface`, set JSON state, and install a script defining `render(state)`.
`h("box", {axis:"column", gap:8, bg:"var(--etch-bg-elevated)", radius:"panel"},
h("text", {text:"Hello"}))` builds cached native nodes. Supported tags are box,
text and button; buttons dispatch their `onClick` action name to `onEvent`.
Scripts may invoke explicitly bound host `action(name,payload)` callbacks.
There are no std/os, filesystem or network globals. Defaults are 8MiB VM memory,
512KiB stack and a 4ms tick budget. Errors display a themed inline card; a new
script recovers the surface. JS allocation occurs only during explicit updates.

Call `inlay_surface_enable_hmr` with a trusted local path and poll HMR from the
host event loop, including during idle periods. Reload retains serialized state;
mtime/size changes trigger evaluation. `setTimeout`/`clearTimeout` are bounded
one-shot timers serviced by view builds; outstanding timers request redraw.
Cached node pointers expire on evaluation. View labels are copied to frame
storage so later state updates cannot invalidate deferred Etch drawing. The
context outlives its surfaces; callbacks may set state but may not destroy or
recursively dispatch their surface. With scripts disabled, creation returns null
and the public ABI remains linkable.

HMR engine timing is reported separately from total filesystem latency in
`last_reload_ms` and `last_reload_total_ms`; local disk/antivirus latency is outside
the VM budget. The regression checks median engine reload below 2ms. Promise jobs
run under the same deadline and settle before native nodes are published.
