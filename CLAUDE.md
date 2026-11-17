# Inlay architecture
Inlay is a public C99 API consumer of Etch, with optional C++ convenience aliases.
The document engine owns UTF-8 source and flat POD block pools. Appends validate
UTF-8 once and retain incomplete sequences in a four-byte carry. Completed blocks
remain frozen; only the trailing block is reparsed. Source offsets survive realloc.
Document views use Etch variable-height virtual lists, selectable rich text and
frame-arena scratch. Context must outlive documents; documents must outlive views.
Use out-of-source Release builds. C99 ABI remains strict C99; all versions match
CMake, both public headers and CHANGELOG. QuickJS-NG is the only future script VM.
