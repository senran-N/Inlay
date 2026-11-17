# Changelog

## [0.1.0] - 2026-11-17

- Add the public C99 document API and C++20 aliases, CMake install/export and sibling Etch integration.
- Validate streamed UTF-8 with incomplete-codepoint carry; freeze completed blocks and reparse only the open tail.
- Render selectable paragraphs, headings, inline bold/code/links, fenced code with copy and syntax colors, task/list markers, quote alerts, tables, thematic rules and details disclosures.
- Virtualize width/font-bounded render rows with persistent per-view measurements and shared details state across continuation rows.
- Cover byte-wise 32KB Unicode streaming, source self-append, frozen-block stability, append scroll anchors, idle scheduling, C99 ABI and installed/subproject consumers.
