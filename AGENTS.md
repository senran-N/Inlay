# Inlay Engineering Contract

Read `CLAUDE.md` first. Follow the Etch `AGENTS.md` contract in `../Etch/AGENTS.md`.
- **Orthodox C++20 Core**: No exceptions, RTTI, virtual widget trees, per-frame STL heap containers, or resource-owning RAII wrappers.
- **Anti-Over-Defensive Precision**: Validate untrusted inputs once at public `inlay_*` / `inlay::` boundaries; trust internal invariants. Persistent pools grow geometrically and stay bounded; per-frame rendering uses Etch's frame arena exclusively.
- **Strict Public-API Boundary**: Never include private Etch headers (`etch/src/*`) or link private Etch internals. Public C99 headers (`include/inlay/inlay_c.h`) include only `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, and `<etch/etch_c.h>`, with `INLAY_API` export macros and versioned `struct_size` ABI descriptors.
- **High-Aesthetic Expressiveness Mandate**:
  - Keep `inlay_ui` in parity with Etch's visual and layout primitives (multi-shadow, multi-stop gradients, vibrancy blur, hover/press/focus, Flexbox/Grid, 2D transforms/transitions, icons, overlays, and native `doc`/`math`/`diagram`/`terminal`/`surface`/`portal` bridges) while preserving zero-idle JS ticks and `< 2 ms` HMR.
  - Enforce publication-grade typography and visual craft across `inlay_doc`, `inlay_math`, `inlay_diagram`, and demos in both Light and Dark themes; visually inspect rendered PNGs under ignored `.ci/` after every layout/styling/rendering change.
- **Memory Discipline**: Keep Inlay's own steady-state memory footprint lean (`< 10 MB` typical above Etch) with bounded caches and zero per-frame heap churn.
- **Permitted Pure-C Kernels**: Single-purpose, MIT/BSD pure-C zero-dependency kernels (`third_party/md4c`, `third_party/quickjs-ng`) are permitted when they strictly outperform hand-rolled prototypes; every `third_party/*` dependency MUST be covered in `THIRD_PARTY_NOTICES.md`.
- **Verification & Hygiene**: Release builds must compile with `/W4 /WX` (zero warnings). Verify C99 and C++20 consumers (installed and `add_subdirectory`), streaming chunk equivalence, repo guardrails, and both GPU backends (`direct3d12` and `vulkan`). Keep screenshots and build caches strictly under ignored `.ci/`. Never commit root PNGs or mark unfinished milestones complete.
