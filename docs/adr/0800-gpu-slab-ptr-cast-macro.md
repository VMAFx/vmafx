# ADR-0800: GPU slab pointer-cast macro (`SLAB_FIELD`)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: cuda, hip, lint, cleanup, gpu

## Context

GPU backends (CUDA, HIP) allocate one contiguous device buffer per feature-extractor
state at init time, then carve it into typed sub-regions by casting the integer device
pointer (`CUdeviceptr` / `uintptr_t`) to a typed host-visible pointer:

```c
field = (T *)slab;
slab += stride;
```

clang-tidy's `performance-no-int-to-ptr` fires at every such cast site.
ADR-0278 requires every NOLINT to carry an inline citation; ADR-0141 established
that this cast cluster is an upstream-parity exception (the CUDA/HIP Driver APIs
expose device buffer addresses as integer types and there is no refactor that removes
the cast without changing the public libvmaf-GPU ABI).

Before this ADR the three GPU VIF/ADM files collectively held 21 bare
`performance-no-int-to-ptr` NOLINTs (a mix of `NOLINTNEXTLINE` and
`NOLINTBEGIN/END` blocks) with no inline citation, violating ADR-0278.
PR #127 flagged this as a hygiene finding.

## Decision

Introduce `core/src/feature/gpu_slab.h` containing a single macro:

```c
#define SLAB_FIELD(dst, type, slab) ((dst) = (type *)(uintptr_t)(slab)) /* NOLINT(performance-no-int-to-ptr) */
```

The NOLINT at the macro definition site carries the ADR-0800 citation and suppresses
the diagnostic at every expansion site, centralising the justification in one place.
Replace all 21 bare NOLINTs in `integer_vif_hip.c`, `integer_vif_cuda.c`, and
`integer_adm_cuda.c` with `SLAB_FIELD` calls.

The stride advance (`slab += …`) is intentionally kept outside the macro so that each
call site's arithmetic remains auditable without expanding the macro.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep per-site `NOLINTNEXTLINE` + add citation comment | No new header | 21 comments to maintain; comment drift likely; blocks of 15+ consecutive NOLINTs remain visually noisy | Rejected — citation scatter was the original complaint |
| Use `NOLINTBEGIN/END` blocks + citation comment | Fewer annotation lines | Still spreads suppression across many call sites; does not express the shared semantic | Rejected — same scatter problem with less visibility per cast |
| Change `VifBufferHip` / `VifBufferCuda` fields to `uintptr_t` then cast at kernel-launch | Eliminates int-to-ptr at carve time | Moves casts to launch hot paths; worsens readability there; no net NOLINT reduction | Rejected — does not eliminate the warning, only moves it |
| `static inline` cast helpers per type | Type-safe, no macro | C has no generic functions; one helper per type (int16_t, uint16_t, uint32_t, int32_t, int64_t, uint64_t) = 6 functions; each still needs one NOLINT | Rejected — more code, same number of suppressions |

## Consequences

- **Positive**: zero bare `performance-no-int-to-ptr` NOLINTs in the three GPU
  feature-extractor files; single authoritative citation; future GPU backends can
  include `gpu_slab.h` and get the same guarantee.
- **Negative**: callers must `#include "gpu_slab.h"` — minor header dependency.
- **Neutral / follow-ups**: new GPU backends (e.g. Vulkan compute, Metal) should
  include `gpu_slab.h` when they carve device buffers in the same pattern.
  No math change; ADR-0214 GPU-parity gate unaffected.

## References

- PR #127 (finding that triggered this ADR).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — upstream-parity exception policy.
- [ADR-0278](0278-t7-5-nolint-sweep.md) — every NOLINT must carry an inline citation.
- req: "Bare suppressions violate ADR-0278 (need citations). Fix: introduce
  `core/src/feature/gpu_slab.h` with a `SLAB_FIELD(slab, type, off)` macro that does
  the int-to-ptr cast in one place with one citation. Replace 21 call sites with the
  macro."
