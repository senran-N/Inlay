# Inlay engineering contract
Read CLAUDE.md first. Follow the Etch AGENTS.md contract in ../Etch/AGENTS.md.
Orthodox C++20 only: no exceptions, RTTI, heap containers or resource-owning RAII.
Validate public inputs once; trust private invariants. Persistent pools grow geometrically;
rendering uses Etch frame arenas. No private Etch headers or implementation dependencies.
Public C99 headers include only stdbool.h, stddef.h, stdint.h and etch/etch_c.h.
Release builds must have zero warnings. Test C99 and C++20 consumers with installed
packages and add_subdirectory, streaming chunk equivalence, and both GPU backends.
Before commits run codex review --uncommitted and FlowGuard flow_verify.
Keep screenshots and build caches under ignored .ci/. Never mark unfinished milestones complete.
