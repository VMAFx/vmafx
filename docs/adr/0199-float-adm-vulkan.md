# ADR-0199: float_adm Vulkan kernel — sixth Group B float twin

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) lists `float_adm` as the
sixth and final Group B float twin in the GPU long-tail batch 3
roadmap. CPU reference:
[`float_adm.c`](../../core/src/feature/float_adm.c) (thin wrapper)

- [`adm.c::compute_adm`](../../core/src/feature/adm.c) (4-scale
orchestration) +
[`adm_tools.c`](../../core/src/feature/adm_tools.c) (the float
`_s`-suffixed primitives: `adm_dwt2_s`, `adm_decouple_s`, `adm_csf_s`,
`adm_csf_den_scale_s`, `adm_cm_s`, `adm_sum_cube_s`).

The integer ADM Vulkan kernel already ships
([ADR-0178](0178-integer-adm-vulkan.md), 1099 LOC C + 677 LOC GLSL).
float_adm is a **separate** CPU extractor with float buffers and
double host accumulators — same algorithmic shape (4-scale CDF 9/7
DB2 wavelet → decouple → CSF → contrast measure), different precision
pipeline (no Q-shifts, no division-LUT, no int64 packed accumulators).

This is the same feature surface CPU's `float_adm` produces — five
output metrics: combined `VMAF_feature_adm2_score` and four
`VMAF_feature_adm_scaleN_score` per-scale ratios; plus the standard
debug suffixes (`adm`, `adm_num`, `adm_den`, `adm_num_scaleN`,
`adm_den_scaleN`).

## Decision

Ship `float_adm_vulkan` as the float twin of integer_adm_vulkan.
**16 pipelines** (4 stages × 4 scales) and the same wave-of-stages
host driver. Per-frame dispatch:

| stage | role                                                | dispatch (per scale)             |
|------:|-----------------------------------------------------|----------------------------------|
| 0     | DWT vertical (ref+dis fused)                        | `(cur_w/16, half_h/16, 2)`       |
| 1     | DWT horizontal (ref+dis fused)                      | `(half_w/16, half_h/16, 2)`      |
| 2     | Decouple + CSF (writes csf_a + csf_f)               | `(half_w/16, half_h/16, 1)`      |
| 3     | CSF denominator + Contrast Measure fused            | `(3 × num_active_rows, 1, 1)`    |

Stage 3 emits **6 float partials per WG** (csf_h/v/d, cm_h/v/d) into
a per-scale accumulator. Each WG handles one (band, row), so per-WG
only 2 of 6 slots are non-zero — the other slots stay zero (host
clears the buffer before each frame). The host CPU promotes to
`double` when reducing across WGs and runs the same scoring helpers
as the CPU reference (`powf((float)accum, 1/3) + powf(area/32, 1/3)`).

### Mirror-asymmetry status — NOT present in float_adm

[ADR-0197](0197-float-vif-gpu.md) documents the mirror trap that bit
the float_vif port: `vif_tools.c::vif_mirror_tap_h` returns
`2 * sup - idx - 1` while the AVX2 fast-path's
`convolution_internal.h::convolution_edge_s` returns
`2 * sup - idx - 2`, and float_vif's GPU port had to follow the AVX2
form (`-2`) to hit places=4.

float_adm has **no such asymmetry**. Both the scalar `adm_dwt2_s`
(adm_tools.c:1029) and the AVX2 path
(`x86/float_adm_avx2.c::float_adm_dwt2_avx2`) consume the same
`ind_y` / `ind_x` index buffer built by `dwt2_src_indices_filt_s`
(adm_tools.c:961). That helper uses `2 * sup - idx - 1` for both
axes. We reproduce the `-1` form on GPU and sit identical to both
CPU paths.

A short comment block in `float_adm.comp::dev_mirror` cites this ADR
contrastively against ADR-0197 so future maintainers don't "fix" the
GPU mirror by analogy with float_vif.

### Precision contract: places=4 across all 5 outputs

Lavapipe lane (`scripts/ci/cross_backend_vif_diff.py
--feature float_adm --places 4`) gates the kernel against the CPU
scalar reference at the same threshold as the other batch 3 metrics.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Cross-backend script integration only — no per-step CI** | Cheaper CI minutes | The lavapipe lane was the catch-all that flushed psnr_hvs / float_vif drift; we want the same coverage for float_adm | Add the lane step (matches every other batch-3 metric) |
| **`double`-precision accumulators inside the shader** | Could close any drift > AVX2 reference | `float64` capability is patchy across drivers (Mesa lavapipe + older Arc are flaky); pure float matches CPU's float-then-double pattern | Float partials + host double sum is the same shape CPU uses |
| **Atomic float adds for cross-WG accumulation** | Single stage; no host reduction | `VK_KHR_shader_atomic_float` not portable (lavapipe lacks it); per-WG slots are essentially free | Per-WG slot pattern matches every other Vulkan kernel in the fork |
| **Loosen the gate to `places=3`** | Quick green | Weakens correctness; user direction explicitly forbids weakening gates to make red turn green | Per CLAUDE.md / project memory: never lower thresholds |
| **Ship CUDA + SYCL twins in same PR** | Three-backend symmetry in one shot | Larger diff; the float_vif PR (a6bd365c) already pioneered the three-backend shape and is in flight; float_adm twins land as a focused follow-up | Vulkan-only here; CUDA + SYCL queued |

## Consequences

- **Positive**: closes the Vulkan slot for the last batch-3 metric.
  Group B float twins on Vulkan are fully covered.
- **Positive**: pure mechanical adaptation of ADR-0178 + ADR-0197
  patterns — no novel algorithmic territory; small surface for
  reviewer churn.
- **Negative**: 16 dispatches per frame is heavier than the float_vif
  shape (7 dispatches). Per-scale resolution decreases geometrically
  so the total compute stays bounded; lavapipe should still complete
  the gate within the existing job timeout.
- **Negative**: `adm_csf_mode` is parsed but only mode 0 (the default)
  is supported — non-zero values return `-EINVAL` at init. Documented
  in the option's help string. The CPU reference has the same default;
  the alternative modes are unused in production.
- **Neutral / follow-ups**:
  1. CHANGELOG + features.md updates ship in this PR per ADR-0100.
  2. Lavapipe lane gains a `float_adm` step at `places=4`.
  3. CUDA + SYCL twins land in the next PR (mechanical port from
     this Vulkan kernel; integer ADM has the precedent for both).

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch 3 scope.
- Integer precedent: [ADR-0178](0178-integer-adm-vulkan.md) — host
  driver structure and 16-pipeline pattern.
- Mirror-trap precedent: [ADR-0197](0197-float-vif-gpu.md) — float_vif
  GPU port's `-2` mirror; this ADR documents that float_adm does NOT
  hit the same trap (both CPU paths use `-1`).
- CPU reference:
  [`float_adm.c`](../../core/src/feature/float_adm.c),
  [`adm.c`](../../core/src/feature/adm.c),
  [`adm_tools.c`](../../core/src/feature/adm_tools.c).
- AVX2 fast path:
  [`x86/float_adm_avx2.c`](../../core/src/feature/x86/float_adm_avx2.c).
- Verification: cross-backend gate
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  with `--feature float_adm --places 4`. New step in the lavapipe
  lane of
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
- User direction: 2026-04-27 — close the Vulkan slot for float_adm,
  scope to Vulkan only, CUDA/SYCL twins follow in a separate PR.
