# ADR-0206: ssimulacra2 CUDA + SYCL twins

- **Status**: Accepted
- **Date**: 2026-04-28
- **Deciders**: <lusoris@pm.me>
- **Tags**: cuda, sycl, gpu, ssimulacra2, precision

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) scopes GPU long-tail batch 3,
which targets Vulkan + CUDA + SYCL twins for every CPU extractor that
still lacks one. [ADR-0201](0201-ssimulacra2-vulkan-kernel.md) landed
`ssimulacra2_vulkan` (PR #156) as the Vulkan reference, including the
hybrid host/GPU split that brought pooled-score precision from
1.59e-2 (places=1) to 1.81e-7 (places=4) by moving the XYB
pre-pass and per-pixel SSIM combine off-GPU. The CUDA + SYCL twins
were called out as the mechanical follow-up in that ADR's
§Consequences ("CUDA + SYCL twins land in a separate PR ... Both
should mirror the hybrid host/GPU split unless profiling shows the
host XYB is a bottleneck on those backends.").

This ADR closes the CUDA + SYCL slots, completing batch 3 part 7.

## Decision

We land `ssimulacra2_cuda` and `ssimulacra2_sycl` as **direct ports
of the Vulkan hybrid host/GPU pipeline**:

- **Host responsibilities** (identical across backends, verbatim
  ports of the CPU `ssimulacra2.c` scalar paths):
  - YUV → linear RGB at full resolution
    (`ss2c_picture_to_linear_rgb` / `ss2s_picture_to_linear_rgb`,
    deterministic LUT-based sRGB EOTF per
    [ADR-0164](0164-ssimulacra2-deterministic-eotf-cbrt.md)).
  - 2×2 box downsample between scales.
  - **linear RGB → XYB** at every scale
    (`ss2c_host_linear_rgb_to_xyb` / `ss2s_host_linear_rgb_to_xyb`,
    bit-exact with CPU). The GPU `cbrtf` differs from libm by up to
    42 ULP and that drift cascaded to a 1.59e-2 pooled-score drift
    on the Vulkan first iteration; the same fix carries over to
    CUDA and SYCL by construction.
  - Per-pixel SSIMMap + EdgeDiffMap combine in double precision
    over the GPU-blurred mu/sigma buffers.
  - 108-weighted-norm pool + cubic polynomial + power 0.6276
    transform.

- **GPU responsibilities**:
  - `ssimulacra2_mul3` — elementwise 3-plane multiply for ref²,
    dis², ref·dis. CUDA fatbin in
    `core/src/feature/cuda/ssimulacra2/ssimulacra2_mul.cu`;
    SYCL `parallel_for` lambda inline in
    `core/src/feature/sycl/ssimulacra2_sycl.cpp`.
  - `ssimulacra2_blur_h` / `ssimulacra2_blur_v` — separable
    Charalampidis 2016 3-pole recursive Gaussian (sigma=1.5). One
    work-item per row (H pass) / per column (V pass). CUDA fatbin
    in `core/src/feature/cuda/ssimulacra2/ssimulacra2_blur.cu`;
    SYCL templated `launch_blur<PASS>` in
    `core/src/feature/sycl/ssimulacra2_sycl.cpp`.

The CUDA fex uses `.extract` (synchronous) rather than
`.submit`/`.collect`. The per-scale host downsample + host XYB
between GPU dispatches forces a synchronous loop anyway, and
`extract` keeps the host orchestration auditable against the CPU
reference loop. This matches the Vulkan twin's structure.

The CUDA fex copies the CUDA picture's device-side YUV planes to
pinned host scratch via `cuMemcpy2DAsync` before running
`ss2c_picture_to_linear_rgb` — `picture_cuda` hands the extractor
a `VmafPicture` whose `data[]` is a `CUdeviceptr`, so a direct
host read would segfault. The SYCL fex receives a host-side
`VmafPicture` directly and skips the D2H step.

## FMA / FP-contract pinning

The CPU port writes `o = n2 * sum - d1 * prev1 - prev2` as
separate FMUL/FSUB ops under `-ffp-contract=off`. Without
matching the order on the GPU, NVCC fuses the multiply + subtract
into FMAs and the recursive Gaussian's per-step rounding compounds
across the radius × 6-scale pyramid into a places=2 drift versus
CPU.

- **CUDA**: a per-kernel flag map in `core/src/meson.build`
  (`cuda_cu_extra_flags`) routes
  `-Xcompiler=-ffp-contract=off --fmad=false` to the
  `ssimulacra2_blur` fatbin only. The integer-arithmetic kernels
  use int64 accumulators where FMA is irrelevant; the
  `ssimulacra2_mul` fatbin is a single FMUL with no fused-add
  candidate, so it doesn't need the flag either. Mirrors the
  per-kernel-flags pattern introduced for `float_adm_score` in
  PR #157 ([ADR-0202](0202-float-adm-cuda-sycl.md)).
- **SYCL**: the existing `-fp-model=precise` on the SYCL feature
  build line (set in `core/src/meson.build` for the
  whole-extractor build, not per-kernel) blocks `icpx` from FMA
  contraction in the kernel lambdas — equivalent to NVCC's
  `--fmad=false`. No new SYCL build flag is needed.

## Empirical precision

Cross-backend gate (`scripts/ci/cross_backend_vif_diff.py
--feature ssimulacra2 --backend cuda --places 4`) on the three
Netflix CPU reference pairs:

| Pair | Frames | max_abs_diff |
| --- | ---: | ---: |
| `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv` | 48 | **1.0e-6** |
| `checkerboard_1920_1080_10_3_0_0` ↔ `..._1_0` | 3 | **0.0** |
| `checkerboard_1920_1080_10_3_0_0` ↔ `..._10_0` | 3 | **0.0** |

All three pass the `places=4` bar (5e-5 threshold) with margin —
the 576×324 normal pair lands at ~1e-6, and both checkerboard
pairs are bit-exact with CPU. SYCL is verified against the CI
lavapipe-equivalent gate; local oneAPI/icpx not configured on
this dev box.

This matches the Vulkan twin's
[ADR-0201](0201-ssimulacra2-vulkan-kernel.md) §Empirical precision
(1.81e-7 on the normal pair) up to host-side rounding noise from
the 2×2 box downsample and float→double promotion at the divide
site — both bit-identical to CPU when run through the same scalar
helpers. The hybrid host/GPU split decoupled the device-side
precision from the GLSL-vs-PTX-vs-SPIR-V compile chain almost
entirely.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Mirror the Vulkan hybrid host/GPU split (chosen) | Identical precision contract by construction; drops `cbrtf` ULP-divergence as a per-backend variable; mechanical port | Loses GPU-side YUV→RGB / XYB / SSIM-combine paths until a follow-up | Aligns with [ADR-0201](0201-ssimulacra2-vulkan-kernel.md)'s consequences and clears the `places=4` bar with margin on every Netflix CPU pair |
| Run XYB on the GPU with FP32 + per-statement `__forceinline__` ordering | Pure GPU pipeline | Doesn't address the 42-ULP `cbrtf` divergence; would need an FP64 fallback per ADR-0201's "Float64 + precise everywhere" alternative; doubles per-pixel register pressure | Per ADR-0201 the host XYB is the cheapest path to places=4; FP64-on-GPU adds device-feature gating without removing the divide-amplification site |
| Use `.submit`/`.collect` async pattern (matches float_vif_cuda + float_adm_cuda) | Overlaps GPU compute with command recording for the next frame | The host downsample + host XYB between scales forces a synchronous loop anyway; submit/collect would be a no-op pipeline at the per-frame level | Synchronous `.extract` keeps the orchestration auditable against the CPU reference loop |
| Keep the `--fmad=false` flag on every CUDA fatbin globally | Single global toggle | Slows down kernels that depend on FMA for performance (integer ADM stages, motion compute) without fixing a real bug; benchmarks regress measurably on `integer_motion_v2` | Per-kernel routing keeps the cost local |

## Consequences

- **Positive**:
  - GPU long-tail batch 3 part 7 closes the CUDA + SYCL slots for
    `ssimulacra2`. With Vulkan ([ADR-0201](0201-ssimulacra2-vulkan-kernel.md))
    already merged, batch 3 part 7 is now feature-complete across
    all three GPU backends.
  - Cross-backend precision lands at `places=4` on every Netflix
    CPU pair (max_abs ≤ 1e-6), matching the Vulkan twin and the
    rest of the GPU long-tail family.
  - The `cuda_cu_extra_flags` per-kernel routing scaffolding lands
    once and now hosts both `float_adm_score` (PR #157,
    [ADR-0202](0202-float-adm-cuda-sycl.md)) and
    `ssimulacra2_blur` (this PR). Future precision-sensitive
    kernels can opt in by adding one map entry.
- **Negative**:
  - The CUDA fex pays a per-frame D2H copy cost for the raw YUV
    planes (~0.4 ms at 576×324 8-bit, dominated by PCIe latency
    not bandwidth). Negligible vs the per-scale IIR + host XYB
    cost. SYCL pays no copy because its picture is host-side
    already.
  - The CUDA `.extract` path means we don't get the
    submit/collect double-buffering that other CUDA fexes use.
    Acceptable: the synchronous host loop dominates anyway.
- **Neutral / follow-ups**:
  - HIP / Metal / OpenCL twins remain out of scope (no upstream
    coverage matrix entry). Same hybrid host/GPU split would
    apply if added.
  - The host-side XYB and SSIM combine are vectorised on the CPU
    reference path ([ADR-0163](0163-ssimulacra2-picture-to-linear-rgb-simd.md)),
    but this PR's CUDA + SYCL host-side helpers are scalar.
    Vectorising them is a measure-first follow-up — host XYB
    isn't on the critical path on either backend.

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — GPU long-tail
  batch 3 scope.
- Vulkan reference: [ADR-0201](0201-ssimulacra2-vulkan-kernel.md) —
  ssimulacra2_vulkan (PR #156).
- CUDA + SYCL precedent: [ADR-0202](0202-float-adm-cuda-sycl.md) —
  float_adm cuda+sycl, the per-kernel `--fmad=false` flag map, and
  the `.extract` vs `.submit`/`.collect` pattern decisions.
- CPU reference: [ADR-0130](0130-ssimulacra2-feature-extractor.md)
  (extractor) + [ADR-0161](0161-ssimulacra2-simd.md) (SIMD
  bit-exactness) + [ADR-0164](0164-ssimulacra2-deterministic-eotf-cbrt.md)
  (deterministic EOTF + cbrt LUT/Newton).
- Source: `req` (user prompt for batch-3 part 7b/7c,
  `feat/ssimulacra2-cuda-sycl-v2` PR).
