# Inlay
Native streamed documents on Etch, through its public C99 API. Build with CMake
and Ninja in `build/`; a sibling `../Etch` is used when Etch 0.16 is not installed.
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
