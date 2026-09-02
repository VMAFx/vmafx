<!-- markdownlint-disable MD013 MD060 -->
# ADR-0965: CUDA SpEED TU repair — align with current CudaFunctions table (closes T-CUDA-SPEED-TU-REPAIR-2026-05-31)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude (CUDA TU repair)
- **Tags**: `cuda`, `feature-extractor`, `cross-backend-parity`, `speed`, `fork-local`

## Context

ADR-0964 (PR #473) wired the HIP and SYCL SpEED twins into meson and the
feature-extractor registry, but explicitly deferred the CUDA wiring because
the CUDA TUs at `core/src/feature/cuda/speed_chroma_cuda.c` (868 LOC) and
`core/src/feature/cuda/speed_temporal_cuda.c` (670 LOC) contained three
classes of latent bugs that surfaced the moment the TUs were compiled:

1. **`CHECK_CUDA(cu_f, CALL)` — undefined macro.** Every working CUDA TU in
   the fork uses `CHECK_CUDA_GOTO(cu_f, CALL, label)` (when cleanup is
   pending) or `CHECK_CUDA_RETURN(cu_f, CALL)` (when no cleanup is needed).
   The bare `CHECK_CUDA` form was never defined in this tree; it existed in
   an earlier iteration of `cuda_helper.cuh` that was removed when
   Netflix#1420 was addressed (abort-on-error replaced by errno-return).

2. **`cuMemAllocHost` — not a `CudaFunctions` member.** The pinned-host
   allocation API in the fork's CUDA function table is `cuMemHostAlloc`
   (with a flags argument). `cuMemAllocHost` is the older driver-API variant
   and is not exposed through the `CudaFunctions` dispatch struct. Every
   working CUDA TU that allocates pinned memory calls `cuMemHostAlloc` (see
   `picture_cuda.c:150`, `common.c:334`).

3. **Copyright header** included `"and Claude (Anthropic)"` — the project's
   copyright policy (decided 2026-05-27, memory entry
   `project_copyright_lusoris_only.md`) uses `Copyright 2026 Lusoris` only.

The bugs were latent because the TUs were never compiled before this PR.

## Decision

Repair both TUs:

- Replace all `CHECK_CUDA(cu_f, CALL)` calls with `CHECK_CUDA_GOTO(cu_f, CALL, fail)`.
- Replace `cuMemAllocHost((void **)&ptr, sz)` with `cuMemHostAlloc((void **)&ptr, sz, 0x01u)`.
- Fix copyright headers to `Copyright 2026 Lusoris`.

Wire the repaired TUs into `core/src/meson.build` (under `if is_cuda_enabled`),
declare the extern symbols and add registry rows in
`core/src/feature/feature_extractor.c` (under `#if HAVE_CUDA`), and add
CPU-vs-CUDA parity tests `test_cuda_speed_chroma_parity.c` and
`test_cuda_speed_temporal_parity.c` mirroring the SYCL parity tests from
ADR-0964.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Repair in-place + wire (chosen)** | Minimal blast radius; all algorithmic content (GPU kernels, CPU linalg path) was correct — only the error-handling and host-alloc API were wrong | None at the chosen scope | Chosen |
| Rewrite CUDA TUs using `vmaf_cuda_buffer_host_alloc` wrapper from `common.c` | Consistent with newer CUDA features (e.g. `float_adm_cuda.c`) | Wider diff; the direct `cuMemHostAlloc` call is already the pattern used by `picture_cuda.c` and is correct | Higher cost, no functional benefit |
| Drop CUDA SpEED twins, rely on HIP/SYCL only | Simpler CUDA build | Breaks CUDA-only deployments that want SpEED scores; creates asymmetry between backends | Rejected |

## Consequences

- **Positive**:
  - `vmaf_get_feature_extractor_by_name("speed_chroma_cuda")` and
    `"speed_temporal_cuda"` now resolve on CUDA-enabled builds.
  - Two new CPU-vs-CUDA parity gates (`test_cuda_speed_chroma_parity`,
    `test_cuda_speed_temporal_parity`) catch regressions to within places=4
    (ADR-0214 cross-backend tolerance).
  - `T-CUDA-SPEED-TU-REPAIR-2026-05-31` in `docs/state.md` is closed.
- **Negative**:
  - None at the current scope.
- **Neutral / follow-ups**:
  - The parity tests skip cleanly when no CUDA device is visible (same NaN
    sentinel pattern as `test_cuda_motion3_parity.c`); they will only fire on
    hardware CI lanes.

## References

- ADR-0964 — `speed_internal.c` implementation + HIP/SYCL wiring (the PR
  that deferred this repair and opened the tracking ticket).
- ADR-0214 — cross-backend parity tolerance (places=4).
- `core/src/cuda/cuda_helper.cuh` — `CHECK_CUDA_GOTO` / `CHECK_CUDA_RETURN`
  definitions.
