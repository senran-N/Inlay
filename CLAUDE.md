# Inlay architecture
Inlay is a public C99 API consumer of Etch, with optional C++ convenience aliases.
The document engine owns UTF-8 source and flat POD block pools. Appends validate
UTF-8 once and retain incomplete sequences in a four-byte carry. Completed blocks
remain frozen; only the trailing block is reparsed. Source offsets survive realloc.
Document views use Etch variable-height virtual lists or auto-height flow with
selectable rich text and frame-arena scratch. Context must outlive documents;
documents must outlive views.
Use out-of-source Release builds. C99 ABI remains strict C99; all versions match
CMake, both public headers and CHANGELOG. QuickJS-NG is the only script VM; it is optional behind INLAY_ENABLE_SCRIPT.
Surfaces own persistent bounded native node/text pools. Only state/events/timers/HMR
evaluate JavaScript; idle views consume cached nodes. Host callbacks may set state
but must not destroy the surface or recursively dispatch it. Host code polls HMR
periodically while the UI sleeps; timers advance when the view is built.
Native math uses frame-arena box trees and Etch font metrics. Diagrams use a bounded
arena graph with feedback-edge reversal, layering and barycentric ordering. Document
virtual lists estimate unseen native rows and replace estimates with visible measured
bounds. Flow lays out all blocks at their natural height. Native callbacks copy source into the frame
arena so document mutation before end_frame cannot invalidate deferred drawing.

Portals own backend state and texture storage. Poll dirty regions outside a queued
frame, build view, synchronize geometry after end_frame, and acknowledge frame
submission before resize/destruction. Input focus/capture are local; host controls
occlusion policy. The optional WebView2 UNDERLAY initializes STA COM in the backend.
The host owns the window, message pump and IME.

Release candidate 0.10.0 requires the public Etch 0.23 package. Keep the pre-1.0
release boundary until real-hardware cross-platform and adoption sign-off.
