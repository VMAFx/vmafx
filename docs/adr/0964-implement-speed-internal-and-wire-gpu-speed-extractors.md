<!-- markdownlint-disable MD013 MD060 -->
# ADR-0964: Implement `speed_internal.c` and wire `speed_{chroma,temporal}_{hip,sycl}`

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude (cross-backend audit gap-fill)
- **Tags**: `cuda`, `hip`, `sycl`, `feature-extractor`, `cross-backend-parity`, `speed`

## Context

The fork has `core/src/feature/speed_internal.h` (introduced for the SpEED
GPU split described in ADR-0567), and ships GPU twin translation units
for `speed_chroma` and `speed_temporal` on three backends:

| Backend | Path                                                | Status before this PR |
|---------|-----------------------------------------------------|------------------------|
| HIP     | `core/src/feature/hip/speed_{chroma,temporal}_hip.c` | Present, not in meson |
| SYCL    | `core/src/feature/sycl/speed_{chroma,temporal}_sycl.cpp` | Present, not in meson |
| CUDA    | `core/src/feature/cuda/speed_{chroma,temporal}_cuda.c` | Present, not in meson |

All six TUs `#include "feature/speed_internal.h"` and call seven of its
nine declared functions:

```text
speed_internal_init_dimensions
speed_internal_float_stride
speed_internal_filter_and_downscale
speed_internal_compute_eigenvalues
speed_internal_is_matrix_regular
speed_internal_qr_factorize
speed_internal_qt_multiply
```

There is **no `core/src/feature/speed_internal.c`** in tree.  Until this
PR, wiring any GPU twin into a meson archive would have produced
undefined-symbol link failures.  The round-4 HIP coverage audit
(PR #466, ADR-0958) and the round-4 SYCL audit (PR #465, ADR-0957)
independently rediscovered the same gap; the round-3 HIP audit
misdiagnosed it as "no CPU scalar reference exists" — which is wrong,
the CPU twins ship as `vmaf_fex_speed_chroma` (`speed.c:1335`) and
`vmaf_fex_speed_temporal` (`speed.c:1559`).

The CUDA TUs surface additional bugs once wired (see § Context-followups
below) and are deferred to a follow-up; HIP and SYCL build cleanly.

## Decision

Implement `core/src/feature/speed_internal.c` with all nine functions
declared in `speed_internal.h`, wire the file into the CPU feature
archive (`libvmaf_feature` static lib), wire the HIP TUs into
`hip_sources`, wire the SYCL TUs into `sycl_feature_sources`, register
the four extractor symbols (`vmaf_fex_speed_{chroma,temporal}_{hip,sycl}`)
in `feature_extractor.c`, and add CPU-vs-SYCL parity tests for both
extractors.  CUDA wiring is deferred to a follow-up PR after the CUDA
TUs are repaired.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Implement `speed_internal.c` as a new TU duplicating speed.c's static helpers** *(chosen)* | Self-contained; no risk of dirtying the Netflix-mirrored `speed.c`; bounded duplication (well-tested pure math, ~620 LOC); CPU golden gate untouched | Code duplication — two copies of eigendecomp + QR | Picked: smallest blast radius, preserves the CPU golden-data gate per CLAUDE.md §8 Rule 1 |
| Drop `static` qualifiers in `speed.c`, expose helpers via `speed_internal.h` | Single source of truth | Dirties an upstream-tracked TU (Netflix copyright header) on every rebase; raises the cost of `/sync-upstream` runs | Operational cost outweighs the duplication |
| Refactor `speed.c` to consume `speed_internal.c` (single source of truth, fork-side) | Eliminates duplication while keeping `speed.c` thin | Touches the Netflix-mirrored file with non-trivial reshuffling; not in scope for a wiring PR | Deferred — can land as a follow-up once the GPU twins prove stable in production |
| Wire all three backends in this PR | Closes the gap in one shot | CUDA TUs have real undefined-symbol bugs (`CHECK_CUDA` missing, `CudaFunctions->cuMemAllocHost` not a member) that need their own repair pass; mixing wiring + repair grows PR scope and bisect cost | Followed brief's "shrink to working backends" guidance |

## Consequences

- **Positive**:
  - `vmaf_get_feature_extractor_by_name("speed_chroma_hip")` and the
    sycl/temporal variants now resolve.
  - HIP and SYCL builds compile cleanly with the new wiring.
  - Two new CPU-vs-SYCL parity gates (`test_sycl_speed_chroma_parity`,
    `test_sycl_speed_temporal_parity`) prevent regressions to within
    places=4 (ADR-0214 cross-backend tolerance) on machines with an
    Intel Arc / Level-Zero device.
  - CPU SpEED golden-data gate is untouched (separate TU).
- **Negative**:
  - `speed_internal.c` duplicates ~600 LOC of pure math from `speed.c`.
    Risk: a future fix to one path that doesn't get mirrored to the
    other.  Mitigation: ADR comment in `speed_internal.c` pointing at
    `speed.c` as the canonical reference; cross-backend parity tests
    flag divergence at CI time.
  - CUDA wiring still missing; tracked as
    `T-CUDA-SPEED-TU-REPAIR-2026-05-31` in `docs/state.md`.
- **Neutral / follow-ups**:
  - Repair `core/src/feature/cuda/speed_{chroma,temporal}_cuda.c`
    (`CHECK_CUDA` -> `CHECK_CUDA_RETURN`, `cuMemAllocHost` -> real
    driver API call), then wire them into `libvmaf_feature_sources`
    and add `test_cuda_speed_*` parity gates mirroring the SYCL ones.
  - Once stable, consider unifying `speed.c` and `speed_internal.c`
    behind a single source of truth.

## Context-followups (CUDA repair scope)

While wiring, the CUDA TUs produced compile errors:

```text
error: implicit declaration of function 'CHECK_CUDA'      (use CHECK_CUDA_RETURN / _GOTO)
error: 'CudaFunctions' has no member named 'cuMemAllocHost'  (use cuMemHostAlloc)
warning: unused label 'fail'                              (left over from CHECK_CUDA_GOTO migration)
```

These are independent of `speed_internal.c`; they were latent because
the TUs were never compiled.  The CUDA repair PR will mirror the HIP
twin's `hipHostMalloc` pattern and the working CUDA twins'
`CHECK_CUDA_RETURN` macro usage.

## References

- ADR-0567 — SpEED CPU/GPU algorithm split rationale.
- ADR-0214 — cross-backend places=4 numeric-parity gate.
- ADR-0533 — full HIP-extractor registration sweep pattern (mirrored here).
- PR #465 — SYCL r4 coverage audit (independent discovery of the gap).
- PR #466 — HIP r4 coverage audit (independent discovery of the gap).
- ADR-0957 — SYCL r4 audit closeout.
- ADR-0958 — HIP r4 audit closeout.
- Source: `req` — operator brief 2026-05-31 ("Implement the missing
  `core/src/feature/speed_internal.c` and wire up the
  `speed_chroma_{hip,cuda,sycl}` + `speed_temporal_{hip,cuda,sycl}`
  extractors. This unblocks speed-family parity gates on ALL THREE GPU
  backends simultaneously.")
