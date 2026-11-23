# Inlay
Native streamed documents, publication-grade math/diagrams, and high-velocity declarative QuickJS-NG UI surfaces on Etch, through its public C99 API. Build with CMake
and Ninja in `build/`; a sibling `../Etch` is used when Etch 0.28 is not installed. Inlay 0.13.0 requires Etch 0.28.
C99 includes `<inlay/inlay_c.h>`; C++20 may include `<inlay/inlay.h>`.

Create a document with an Etch context, append UTF-8 chunks, and call
`inlay_doc_view` during that context's frame. Four carry bytes bridge incomplete
codepoints. Invalid input is rejected; clear the document to discard a pending
partial sequence. Appends received between layout or block queries parse together.
Frozen blocks retain source offsets while the open tail is reparsed. Appending
the document's own source is supported. Returned slices and
block pointers are borrowed until the next mutation. The context must outlive
documents. Mutate a document between view calls. Each view ID has its own
virtual-list measurement state, allocated on demand up to 4,096 views per document.

Document and diagram rendering follows the active Etch theme, including code,
alerts, tables, syntax highlighting, diagram nodes, and feedback edges.
Document views support selectable rich text, nested bold, emphasis and
strikethrough, inline code, clickable links, headings, alerts, lists, tables
with escaped pipes (`\|`), and details disclosures. Wider rows leave a gutter
for the virtual-list scrollbar. Code lines scroll horizontally with the copy
button and language label above the scrolling content. Long fences remain
virtualized in groups of up to 16 lines. Frozen measurements survive tail
appends, and persistent per-view pools grow geometrically.

Set `height_px` to zero for an auto-height document inside a parent layout. Use
`inlay_doc_measure_height` with the same ID, width and font size to retrieve the
cached height or a pre-layout estimate. Begin with `inlay_doc_view_desc_default`
to get the default block and table spacing. Tables share borders and honor GFM
column alignment rules. Inline code and links can be styled with an inline
decorator, including Etch stroke icons, pill padding and custom click IDs.

`inlay_document --headless output.png` renders a deterministic document sample.
`inlay_document --stress output.png` checks a 100KiB paragraph and 900-line code block.
Run CTest to verify streaming equivalence, C99, guardrails and downstream builds.

Native math uses `inlay_measure_math` / `inlay_draw_math` during an Etch frame.
Coordinates and returned sizes use logical pixels; `pos` is the formula's top left,
and `baseline` is measured from that top. Supported commands include Greek/operator
symbols, fractions, roots, scripts, accents, matrix/aligned environments and
stretchy delimiters. Unsupported syntax reports `valid=false`; document views
show the original source. Inline `$...$` and `\(...\)` and display `$$` / `\[` fences
render natively. These APIs use Etch font metrics.

`inlay_diagram_layout` returns frame-lifetime node/edge spans; `inlay_diagram_view`
draws them with native geometry and text. The supported Mermaid subset includes
flowcharts in TD/TB/LR, bracketed node labels, directed chains, cycles, edge labels
(`A -->|label| B` or `A -- label --> B`), and dashed edges (`A -.-> B` or
`A -. label .-> B`). Nodes and edges inside matched `subgraph` / `end` lines
participate in the main layout. Sequence diagrams support participants, `as`
names, solid `->>` messages and dashed `-->>` messages. Inputs are bounded to
16 KiB, 256 nodes and 1024 edges. Use `canvas_id` for an interactive pan/zoom
canvas or call inside an existing Etch canvas.
Use `InlayDiagramDesc::diagnostic` to receive byte ranges for unsupported
directives, unsupported diagram types and parse errors. A partial rendering
reports skipped directives through `InlayDiagramResult::warning_count`.
`inlay_document --math` and `--diagram` render standalone native examples.
Invalid TeX returns `InlayMathResult::error` and `error_offset` so an editor
can underline the failing source location. Unsupported commands and excessive
nesting have separate error kinds.


Reactive surfaces use optional QuickJS-NG (`INLAY_ENABLE_SCRIPT=ON`). Create an
`InlaySurface`, set JSON state, and install a script defining `render(state)`.
Both `h(tag, props, ...children)` and ergonomic `ui.*` builders (`box`, `row`, `col`,
`card`, `spacer`, `text`, `button`, `input`, `toggle`, `checkbox`, `tabs`, `combobox`,
`scroll`, `progress`, `slider`, `badge`, `divider`, `image`, `modal`, `popover`,
`tooltip`, `split`, `math`, `diagram`, `slot`) compile into cached native Etch nodes
with full Etch 0.27 styling (`elevation`, `shadow`/`shadows`, `insetShadow`, multi-stop
Oklab `gradient`, `backdropBlur` vibrancy, `continuousCorners`, per-corner `radius`,
`topRimAlpha`, `hoverBg`/`hoverBorderColor`/`hoverScale`/`pressScale`, `rotate`/`scale`/`translate`/`opacity`,
Flexbox + 2D Grid + floating anchor/zIndex). Host code can bind native panels (`InlayDoc`,
`InlayPortal`, `etch_terminal_grid`, custom GPU shaders) directly into declarative
`ui.slot({name})` nodes via `inlay_surface_bind_slot`. Buttons dispatch `onClick` to
`onEvent` or inline JS callbacks; inputs, toggles, checkboxes, sliders, tabs, and
comboboxes are controlled by script state.

```js
h("input", {value: s.query, placeholder: "Filter...", onChange: "setQuery", width: -1})
h("toggle", {label: "Live", checked: !!s.live, onChange: "setLive"})
```

An edit dispatches a string to `onEvent(name, payload, state)` and a toggle
dispatches a boolean. Update the corresponding state field in `onEvent` to
keep the new value. Input supports IME, `password` (also `type: "password"`),
`multiline`, `wordWrap`, `readOnly`, `onSubmit`, and up to 8191 UTF-8 bytes.
Password values are masked for accessibility and omitted from public node text.
Idle views
reuse the cached nodes without running JavaScript, and repeated `cls` utility
strings hit a persistent 2-way parsed style cache across rebuilds and theme
switches (`hover:`, `active:`, `focus:`, and live `dark:` prefixes). Declarative
functional components support call-path and `key`-scoped `useState`, `useRef`,
`useMemo`, `useCallback`, and `useEffect` (with automatic unmount cleanup).
Define `const Panel = memo(PanelBody)` at script scope to reuse a keyed
component's JS tree while its props remain shallow-equal. Pass external
state as props, including revisions for mutable data. Setters inside a
memoized subtree invalidate its memoized ancestors. An optional second
argument compares the previous and next props. Native nodes still rebuild
on a surface update.
Wrap a panel in `<ErrorBoundary render={() => <Panel/>} fallback={(err, retry) => <Button text={String(err)} onClick={retry}/>}/>`
or pass the same props to `ui.errorBoundary`. A synchronous render exception
unmounts that subtree and displays the fallback. Sibling panels keep their
state. Invoke `retry()` or change `resetKey` to mount the panel again.
Event handlers, async jobs, effects and the surface watchdog retain their surface-level error handling.
Color props also accept `var(--etch-border-subtle)` and `var(--etch-text-muted)`.
Bind a host-owned `InlayDoc` with `inlay_surface_bind_document` and use
`ui.doc({document: "name"})` to render it directly. Appending UTF-8 to the document
updates the view without re-evaluating the script. Unbind before destroying the
document while the surface is live.
String-backed document widgets reuse their parsed document across views of
the same keyed node. `InlaySurfaceStats` reports resident cached documents
and cumulative cache misses.

Bind a host-owned `EtchTextBuffer` with `inlay_surface_bind_text_buffer` and
render `<CodeEditor buffer="source" onChange={handleEdit} />`. It supports
`readOnly` and `wordWrap`. Edits stay in the native buffer, including undo
history. Each change reports `cursorByte`, `anchorByte`, `byteLength` and
`lineCount` to the handler. Unbind the buffer, render a frame without its editor
to retire the native selection and accessibility snapshots, then free it.

At the end of a byte stream, use `inlay_doc_stream_status` to detect a partial
UTF-8 sequence or an open fence. Search the rendered view's plain text with
`inlay_doc_view_find_text`. Pass the returned offsets to the selection API and
queue `inlay_doc_view_reveal_text` to scroll a fixed-height view to the match.
Search again after changing the document and rendering its updated view.

For annotations, store a nonzero host ID and a half-open UTF-8 source range
with `inlay_doc_set_anchor`. Range edits move the endpoints. A point at EOF
follows streamed text as complete characters arrive. Enumerate anchors with
`inlay_doc_anchor_at` to save them alongside the source, then restore them
after loading the matching document.

For editors and patch previews, `inlay_doc_replace_utf8` applies an insertion,
deletion, or replacement by source byte range. Endpoints must be UTF-8
boundaries. A pending partial streaming sequence must be completed first.
Use `inlay_doc_revision` to detect changes and refresh the view's search offsets.

`InlayInbox` passes copied byte messages from workers to an owner thread with
limits on both message count and payload bytes. Set `wake_event_type` to an SDL
user event allocated by the host and drain the inbox before rendering. If the
frame's work budget leaves messages queued, keep pumping while
`inlay_inbox_pending` is nonzero. Use the message kind and ID to route document
chunks or async results to the corresponding owner-thread API. Treat
`INLAY_INBOX_FULL` as backpressure. On shutdown, close the inbox, join its
producers, then drain or discard the remaining messages and destroy it.

Scripts may invoke explicitly bound host `action(name,payload)` callbacks.
For background work, authorize names with `inlay_surface_allow_async_action`.
In script, `request(name, payload)` returns a Promise with a `cancel()` method.
On the owner thread, poll `inlay_surface_poll_async_action` after script updates
until it returns `INLAY_ASYNC_ACTION_NONE`. Copy a start event's name and JSON
before handing the work to a worker. Deliver completion on the owner thread
with `inlay_surface_complete_async_action`. Report progress through regular
surface events. A cancel event identifies the work to cancel. Script replacement
and close discard queued work and emit cancellation for work already taken.
Call `inlay_surface_close_async_actions` and drain those events before destroying
a surface that has background work. Each surface supports up to 32 active
requests and up to 64 KiB of JSON per request or result.

There are no std/os, filesystem or network globals. Defaults are 8MiB VM memory,
512KiB stack and a 4ms tick budget. Errors display a themed inline card; a new
script recovers the surface. JS allocation occurs only during explicit updates.

Call `inlay_surface_enable_hmr` with a trusted local path and poll HMR from the
host event loop, including during idle periods. Once a script is running, unreadable files and JavaScript or JSX
syntax failures leave the current UI, handlers and host requests active. The
view shows a reload diagnostic until the next successful reload.
Reload retains serialized state. Changes to modification time or size trigger
evaluation. `setTimeout`/`clearTimeout` are bounded
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
`InlayPortalWebView2Options` through `backend_user_data`, including an absolute UTF-8
profile path and an optional absolute loader path. A null or empty loader path
searches the application and default DLL directories for WebView2Loader.dll.
Create and destroy on the window thread. The backend initializes STA COM and
the host pumps Windows messages.
The completion callback reports asynchronous creation failures; it must not reenter
or destroy the portal. Destroy may precede completion; pending callbacks remain safe.

This backend supports UNDERLAY mode. On Windows it prefers windowless
DirectComposition hosting (`ICoreWebView2CompositionController` +
`IDCompositionRectangleClip`) for rounded and partial scroll clipping with
automatic classic child-HWND fallback when composition is unavailable. It does
not offer OSR pixels or transformed browser input; use a texture-producing
backend for those. Linux WPE and macOS WKWebView can implement the same public
vtable; the built-in web accessor returns NULL when disabled or unsupported. No
unimplemented native feature is silently mocked.
Set `INLAY_WEBVIEW2_LOADER` to enable the real-runtime async/destruction smoke test.

Release candidate scope: Inlay 0.13.0 requires Etch 0.28 and remains pre-1.0.
Local release checks cover Windows Vulkan/D3D12, installed/subproject C99 and
C++20 consumers, script/browser enabled and disabled configurations, bounded
portal scratch memory and idle uploads. Metal and Linux browser/compositor
integration still require real-hardware sign-off. AddressSanitizer checks cover
the linked engine and native consumers; instrumented teardown uses a separate
wall-clock allowance while production VM and timing budgets stay unchanged.
