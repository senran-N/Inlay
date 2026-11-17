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

Windowless portals: `inlay_portal_create` accepts a synchronous
C99 backend vtable or defaults to the synthetic tile backend. Poll native dirty
rectangles, build `inlay_portal_view`, then call `inlay_portal_sync_geometry` after
Etch `end_frame` on the portal's owning context/window. Pointer coordinates enter
in logical screen pixels; backend coordinates use backing-store pixels. IME
queries return logical screen rectangles for the host's OS text-input integration.
The portal tracks focus and pointer capture; the host disables input while other
UI covers it using `inlay_portal_set_input_enabled`. Captured release can land
outside the clipped rectangle. Keys, text and wheel require focus. Geometry
visibility is rectangular and cannot establish sibling or rounded occlusion; the
host resolves those cases. Polling does not run a browser event loop. Context outlives portal. The mock has an explicit animation clock (`inlay_portal_mock_advance`); unchanged
phases perform no upload. `cromma_workbench_demo --headless output.png` renders
the integrated streaming document, native math/diagram, sandbox, terminal, shader
surface and zoomed portal. Run without flags for the interactive desktop host.

After submitting or discarding the frame's draw commands, call
`inlay_portal_frame_submitted`. Resize and destroy reject pending views to preserve
textures referenced by deferred Etch commands. A failed destroy leaves ownership
with the caller, who must retry after frame submission.


Windows native web hosting is optional: configure `INLAY_ENABLE_PORTAL_WEBVIEW2=ON`
and `INLAY_WEBVIEW2_INCLUDE_DIR` pointing at the official WebView2 SDK include folder.
The loader/runtime is dynamically probed at creation, with no redistribution or
link-time browser dependency. Select `inlay_portal_webview2_backend()` and pass
`InlayPortalWebView2Options` through `backend_user_data`, including absolute UTF-8
loader/profile paths. The caller initializes STA COM and pumps Windows messages.
The completion callback reports asynchronous creation failures; it must not reenter
or destroy the portal. Destroy may precede completion; pending callbacks remain safe.

This backend supports UNDERLAY only. The host provides a dedicated native HWND
behind an alpha-composited Etch surface with matching client coordinates and owns
native focus, keyboard and OS IME dispatch. Child-HWND content is hidden under
partial clipping; rounded masking and desktop alpha composition require host
window/compositor integration. It does not offer OSR pixels or transformed browser
input; use a texture-producing backend for those. Linux WPE and macOS WKWebView
can implement the same public vtable; the built-in web accessor returns NULL when
disabled or unsupported. No unimplemented native feature is silently mocked.
Set `INLAY_WEBVIEW2_LOADER` to enable the real-runtime async/destruction smoke test.

Release candidate scope: Inlay 0.5 requires Etch 0.18 and remains pre-1.0.
Local release checks cover Windows Vulkan/D3D12, installed/subproject C99 and
C++20 consumers, script/browser enabled and disabled configurations, bounded
portal scratch memory and idle uploads. Metal and Linux browser/compositor
integration still require real-hardware sign-off. AddressSanitizer checks cover
the linked engine and native consumers; instrumented teardown uses a separate
wall-clock allowance while production VM and timing budgets stay unchanged.
