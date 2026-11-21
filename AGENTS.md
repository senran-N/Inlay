# Inlay Engineering Contract
Read `CLAUDE.md` first. Follow the Etch `AGENTS.md` contract in `../Etch/AGENTS.md`.
- Orthodox C++20 only: no exceptions, RTTI, virtual widget trees, per-frame STL heap containers, or resource-owning RAII.
- Validate public inputs once; trust private invariants. Persistent pools grow geometrically; rendering uses Etch frame arenas.
- No private Etch headers or Etch implementation dependencies.
- Public C99 headers (`include/inlay/inlay_c.h`) include only `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, and `<etch/etch_c.h>`, with `INLAY_API` export macros and versioned `struct_size` ABI descriptors.
- Single-purpose, MIT/BSD pure-C zero-dependency kernels (such as `third_party/md4c` for CommonMark+GFM AST and `third_party/quickjs-ng` for sandboxed UI) are permitted when they strictly outperform hand-rolled prototypes; every `third_party/*` dependency MUST be covered in `THIRD_PARTY_NOTICES.md`.
- Release builds must compile with `/W4 /WX` (zero warnings). Test C99 and C++20 consumers with installed packages and `add_subdirectory`, streaming chunk equivalence, and both GPU backends (`direct3d12` and `vulkan`).
- Keep screenshots and build caches strictly under ignored `.ci/`. Never commit root PNGs or mark unfinished milestones complete.
