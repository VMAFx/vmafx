<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# AGENTS.md — core/src/metal

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

Orientation for agents working on Metal (Apple Silicon) backend.
Parent: [../../AGENTS.md](../../AGENTS.md). Mirrors
[`core/src/hip/AGENTS.md`](../hip/AGENTS.md) — HIP and Metal backends
share audit-first scaffold story. Metal's `MTLDevice` /
`MTLCommandQueue` / `MTLBuffer` API parallels HIP's `hipDevice_t` /
`hipStream_t` / `hipMalloc` surface closely enough that rebase /
refactor contracts on this side track HIP scaffold's.

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `meson.build`, `/core/meson_options.txt` | [adrs-and-build](AGENTS.d/adrs-and-build.md) | Governing Metal backend ADRs and macOS versus non-macOS Meson build commands. |
| `common.h`, `common.mm`, `state_priv.h`, `picture_metal.mm`, `kernel_template.mm` | [context-and-ownership](AGENTS.d/context-and-ownership.md) | Filtered Apple7+ device numbering, state_priv.h VmafMetalContext layout, and ARC bridge-cast ownership. |
| `**` | [ground-rules](AGENTS.d/ground-rules.md) | Apple7+ device gate, Metal/Foundation probes, uintptr_t header boundary, and unified-memory template shape. |
| `iosurface_layout.h`, `picture_import.mm`, `dispatch_strategy.c`, `kernel_template.h`, `/core/src/feature/metal/**` | [iosurface-and-dispatch](AGENTS.d/iosurface-and-dispatch.md) | IOSurface bi-planar layout planning, exact dispatch feature-key allowlist, and HIP/Metal template symmetry. |
| `common.h`, `common.mm`, `picture_metal.h`, `picture_metal.mm`, `dispatch_strategy.c`, `kernel_template.mm`, `stubs.c`, `meson.build` | [scope-and-status](AGENTS.d/scope-and-status.md) | Metal runtime scope, Apple7+ context/picture/template status, and landed kernel dispatch coverage. |
