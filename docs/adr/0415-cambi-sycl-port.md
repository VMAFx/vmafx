<!-- markdownlint-disable MD060 -->
# ADR-0415: CAMBI SYCL port — closes last CUDA-to-SYCL parity gap

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu`, `cambi`, `feature-extractor`, `fork-local`, `t3-15`

> **Status update 2026-05-12.** Implementation shipped as
> [`core/src/feature/sycl/integer_cambi_sycl.cpp`](../../core/src/feature/sycl/integer_cambi_sycl.cpp)
> (≈ 37 kB, three SYCL kernels + host fallback for `calculate_c_values`
> and top-K spatial pooling per the hybrid design below). Marks the
> SYCL feature-extractor parity matrix as 17 of 17 versus CUDA.

## Context

The fork ships SYCL ports for 16 of 17 CUDA feature extractors. The sole
remaining gap is `integer_cambi` — CAMBI (Contrast Aware Multiscale Banding
Index), a banding-detection metric. The CUDA twin landed in ADR-0360 using a
"Strategy II hybrid" design: three GPU kernels for the embarrassingly parallel
stages (spatial mask, 2× decimate, 3-tap mode filter) with the
precision-sensitive sliding-histogram `calculate_c_values` pass and top-K
spatial pooling running on the host CPU via `cambi_internal.h` wrappers. The
same design is carried forward to SYCL here, closing the CUDA-to-SYCL feature
parity gap.

The fork must also support both Intel oneAPI (`icpx -fsycl`) and AdaptiveCpp
(`acpp --acpp-targets=…`) since ADR-0335 landed AdaptiveCpp support. Since all
arithmetic in the CAMBI GPU stages is integer-only, the strict-FP flag
difference between the two toolchains (`-fp-model=precise` vs
`-ffp-contract=off`) has no effect on CAMBI's kernels.

## Decision

We implement `integer_cambi_sycl.cpp` as a direct SYCL port of the CUDA twin
(ADR-0360), using the same Strategy II hybrid structure:

- Three SYCL kernels submitted via `sycl::queue::submit` + `nd_range`
  `parallel_for`:
  - `launch_spatial_mask` — derivative + 7×7 box-sum + threshold (port of
    `cambi_spatial_mask_kernel`).
  - `launch_decimate` — strict 2× stride-2 subsample (port of
    `cambi_decimate_kernel`).
  - `launch_filter_mode` — separable 3-tap mode filter, horizontal then
    vertical (port of `cambi_filter_mode_kernel`).
- USM device pointers (`uint16_t *`) via `vmaf_sycl_malloc_device` /
  `vmaf_sycl_malloc_host`; no shared allocations.
- Synchronous per-scale loop in `submit()` (matching the CUDA v1 posture in
  ADR-0360): `q.wait()` between GPU and CPU stages. `collect()` only emits
  the pre-computed score.
- Does not use `vmaf_sycl_graph_register` — the per-frame CPU residual
  serialises frames and is incompatible with the graph-replay model.
- Registered in `feature_extractor.c` under `#if HAVE_SYCL` before the CUDA
  block, so SYCL is preferred when both backends are compiled in.

Precision contract: `places=4` (ULP=0 on emitted score). All GPU stages are
integer-only and bit-exact with the CPU scalar extractor (`vmaf_fex_cambi`).
The host residual runs the exact CPU code from `cambi_internal.h`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full GPU CAMBI (histogram on GPU too) | Eliminates DtoH readback per scale; potentially higher throughput | Sliding histogram is hard to parallelise bit-exactly; would break `places=4` contract; deferred even for CUDA in ADR-0360 | Deferred — Strategy II already acceptable |
| Reuse CUDA .cu kernels via SYCL compatibility layer | Less new code | SYCL compat layer is not in the fork's toolchain; complexity risk with AdaptiveCpp | Not available in this toolchain stack |
| Vulkan compute shaders for CAMBI stages | Maximum portability | Three new GLSL shaders + specialisation constants; the Vulkan twin (ADR-0210) already provides this path | Vulkan twin already covers Vulkan; SYCL twin covers Intel/AMD/NVIDIA SYCL path |

## Consequences

- **Positive**: SYCL backend reaches full feature parity with the CUDA backend
  (17 of 17 extractors). CAMBI scoring on Intel Arc / iGPU / AMD-via-SYCL no
  longer falls back silently to the CPU path when SYCL is active.
- **Negative**: The synchronous per-scale loop in `submit()` means no frame
  pipelining for CAMBI on the SYCL backend (same limitation as CUDA v1 in
  ADR-0360). A v2 async design would need per-scale pinned readback buffers.
- **Neutral / follow-ups**: Smoke test `test_integer_cambi_sycl` verifies
  registration and a non-crash end-to-end run. Full bit-exactness
  (`places=4`) is verified by the cross-backend scoring gate.

## References

- ADR-0360: CAMBI CUDA twin (Strategy II hybrid).
- ADR-0205 / ADR-0210: CAMBI Vulkan twin (original Strategy II precedent).
- ADR-0335: AdaptiveCpp dual-toolchain support.
- ADR-0138 / ADR-0139: Numerical correctness invariants for GPU ports.
- `core/src/feature/cuda/integer_cambi_cuda.c` — CUDA reference.
- `core/src/feature/cuda/integer_cambi/cambi_score.cu` — CUDA kernels.
- `core/src/feature/sycl/integer_cambi_sycl.cpp` — this PR's SYCL port.
- Source: per user direction (agent task brief 2026-05-10).
