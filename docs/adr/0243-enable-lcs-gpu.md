<!-- markdownlint-disable MD013 MD060 -->
# ADR-0243: `enable_lcs` MS-SSIM extras on CUDA + Vulkan

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris (user), Claude (agent)
- **Tags**: cuda, vulkan, gpu, metrics, ms-ssim, fork-local

## Context

The `float_ms_ssim` extractor's `enable_lcs` option (defined in
[`core/src/feature/float_ms_ssim.c`](../../core/src/feature/float_ms_ssim.c))
emits 15 extra per-scale metrics — `float_ms_ssim_{l,c,s}_scale{0..4}` —
on top of the combined Wang-product score. The fork's GPU twins
(`float_ms_ssim_cuda` from
[ADR-0190](0190-float-ms-ssim-cuda.md) / PR #157;
`float_ms_ssim_vulkan` from
[ADR-0190](0190-float-ms-ssim-cuda.md) / PR #141) shipped the
combined score only — both deferred `enable_lcs` to a follow-up
(see the file-header comment at
[`integer_ms_ssim_cuda.c:29`](../../core/src/feature/cuda/integer_ms_ssim_cuda.c#L29)
and the option help-text reading "(reserved; not yet implemented in
the GPU path)" on the Vulkan side).

Per user direction 2026-04-28 (T7-35): implement, do not de-advertise.
The kernels already produce the per-scale `l_means[i]`, `c_means[i]`,
`s_means[i]` doubles — the `vert_lcs` CUDA kernel literally has "lcs"
in its name; the Vulkan vertical pass emits 3 per-WG partials for the
same triple. The combine step on host then forms the Wang product.
The 15 metrics are therefore already computed; only the
`feature_collector_append` calls were missing.

## Decision

We extend the CUDA and Vulkan MS-SSIM extractors to honour
`enable_lcs` by gating 15 additional `vmaf_feature_collector_append`
calls on the existing per-scale L/C/S means. No kernel changes; no
shader changes; no new device buffers. The default-path output
(`enable_lcs=false`) stays bit-identical to the pre-T7-35 binary.

The Vulkan option help text is rewritten to drop the "(reserved;
not yet implemented)" caveat. The CUDA file header comment is
updated to remove the "v1 does NOT implement enable_lcs" deferral.
A new pseudo-feature `float_ms_ssim_lcs` is added to the cross-
backend gate (`scripts/ci/cross_backend_vif_diff.py` and the
matrix gate in `cross_backend_parity_gate.py`); it pins the 16
emitted metrics at `places=4` against the CPU reference per the
existing `float_ms_ssim` contract from ADR-0190.

The SYCL MS-SSIM twin
([`integer_ms_ssim_sycl.cpp`](../../core/src/feature/sycl/integer_ms_ssim_sycl.cpp))
does not currently expose `enable_lcs` (its `options` table is
empty). It therefore stays out-of-scope for this ADR; if SYCL ever
adopts the option-bool, the same wiring applies (the SYCL kernel
already computes the same per-scale means).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A. Gate 15 host-side `feature_collector_append` calls on the existing `enable_lcs` bool** (chosen) | No kernel changes; default path bit-identical; one ADR, ~30 LOC | None of substance | This is the trivial extension — the GPU vert kernel already emits L/C/S means; only the host-side emission was missing. |
| B. Add 15 separate device readback buffers (one per metric) for parity with the CPU's metric-by-metric `compute_ms_ssim` API | Conceptual symmetry with CPU `l_scores[]` / `c_scores[]` / `s_scores[]` arrays | 15× the D2H bandwidth; allocates ~60 MB of pinned host buffers at 4K; redundant — the per-WG-block partials already reduce to per-scale means at host | Wasteful and not faster; the per-scale double accumulator already runs every frame. |
| C. Treat LCS as a separate feature extractor (`float_ms_ssim_lcs_cuda` / `_vulkan`) | Cleaner registration; one extractor = one set of metrics | Forces a second pyramid + intermediates allocation; doubles VRAM; breaks API parity (CPU is one extractor with an option, not two) | API-parity with CPU is a hard constraint per [ADR-0190](0190-float-ms-ssim-cuda.md). |
| D. Order the emitted metric names metric-wise (`{l_scale0..4, c_scale0..4, s_scale0..4}`) vs scale-wise (`{l_scale0, c_scale0, s_scale0, l_scale1, ...}`) | Either ordering works | Metric-wise matches CPU [`float_ms_ssim.c:189`](../../core/src/feature/float_ms_ssim.c#L189-L221) — that's what consumers (`pip install meson-python`) see today | We chose metric-wise to mirror the CPU emission order; downstream JSON consumers see identical key ordering across all backends. |

## Consequences

- **Positive**:
  - The Vulkan option help text no longer lies — `enable_lcs` is
    now real on the GPU.
  - Cross-backend gate (per ADR-0214 / ADR-0125) now covers all 16
    MS-SSIM metrics, not just the combined score; future LCS
    regressions on the GPU side surface immediately.
  - No measurable cost when `enable_lcs=false` (the bool is checked
    once per frame; the kernels are unchanged).
  - The CPU/CUDA/Vulkan triplet stays API-symmetric — one extractor,
    one option, same metric names.
- **Negative**:
  - SYCL stays asymmetric (no `enable_lcs` exposed) until follow-up
    work; documented in [features.md](../metrics/features.md).
  - Cross-backend matrix gate gains one cell (`float_ms_ssim_lcs ×
    {vulkan, cuda, sycl-skipped}`); CI cost is ~1 extra second per
    PR for the lavapipe lane.
- **Neutral / follow-ups**:
  - SYCL `integer_ms_ssim_sycl.cpp` should grow the `enable_lcs`
    option in a follow-up PR (T7-35 SYCL coda); the kernel already
    has the per-scale means.
  - When the parity matrix gate gets a CUDA / hardware-Vulkan
    self-hosted runner, the `float_ms_ssim_lcs` cell becomes
    enforcing rather than advisory.

## References

- T7-35 entry in `.workingdir2/BACKLOG.md`.
- [ADR-0190](0190-float-ms-ssim-cuda.md) — original Vulkan/CUDA MS-SSIM extractors.
- [ADR-0125](0125-ms-ssim-decimate-simd.md) — the SIMD decimate framework that defines the LCS split.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — matrix gate that picks up the new pseudo-feature.
- Source: `req` — user direction 2026-04-28: "implement, do not de-advertise".
- Implementation files:
  [`core/src/feature/cuda/integer_ms_ssim_cuda.c`](../../core/src/feature/cuda/integer_ms_ssim_cuda.c),
  [`core/src/feature/vulkan/ms_ssim_vulkan.c`](../../core/src/feature/vulkan/ms_ssim_vulkan.c).
