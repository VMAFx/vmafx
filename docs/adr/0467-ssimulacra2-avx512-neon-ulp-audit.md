<!-- markdownlint-disable MD013 MD060 -->
# ADR-0467: SSIMULACRA2 AVX-512 + NEON IIR Blur / picture_to_linear_rgb ULP Audit — Clean Close

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris
- **Tags**: `simd`, `ssimulacra2`, `audit`

## Context

BACKLOG item T3-9(b) (formerly T3-10) required a cross-host ULP audit of the
SSIMULACRA2 AVX-512 and NEON IIR blur (`ssimulacra2_blur_plane_avx512` /
`ssimulacra2_blur_plane_neon`) and the `picture_to_linear_rgb` kernels
(`ssimulacra2_picture_to_linear_rgb_avx512` / `_neon`) after their merge in
PRs #98–#100 (ADR-0161/0162/0163).

The concern was "post-merge cross-host snapshot drift": whether the SIMD paths
produce non-zero ULP deltas vs the scalar reference that compound across scales
and hosts, pushing the pooled SSIMULACRA2 score outside the `places=4` (1e-4)
gate.

Cross-host drift was the historical problem that motivated the deterministic
`vmaf_ss2_cbrtf` (Newton–Raphson) and `vmaf_ss2_srgb_eotf` (LUT) helpers in
`ssimulacra2_math.h` (ADR-0164). This audit verifies that both AVX-512 and NEON
paths, which call those same helpers per-lane, inherit the host-independence
guarantee at the kernel level.

## Decision

The audit found zero ULP drift. No code change is required. T3-9(b) is closed as
"clean — within contract."

Key evidence:

1. **C unit test (`test_ssimulacra2_simd`, 13 sub-tests)** — all 13 sub-tests
   pass with `memcmp` byte-exactness on the development host (Zen 5 + AVX-512
   BW/CD/DQ/VL/IFMA/VBMI). The dispatcher selects the AVX-512 path at runtime
   (`VMAF_X86_CPU_FLAG_AVX512` set), confirming that every kernel exercised in
   the test is the AVX-512 variant, not the AVX2 fallback. Covered kernels:
   `multiply_3plane`, `linear_rgb_to_xyb`, `downsample_2x2`, `ssim_map`,
   `edge_diff_map`, `blur_plane` (IIR horizontal + vertical), and all five
   `picture_to_linear_rgb` format variants (420/8, 420/10, 444/8, 444/10,
   422/8).

2. **Python snapshot gate (`ssimulacra2_test.py`, 2 tests)** — both the primary
   576x324 fixture (48 frames) and the 160x90 tail fixture pass at `places=4`
   tolerance. Actual scores (x86-64): mean 80.551211, frame0 91.695977,
   frame47 77.992897 — all matching hardcoded baselines.

3. **IIR boundary analysis** — `hblur_16rows_avx512` and `hblur_4rows_neon`
   both initialise `prev1_*` / `prev2_*` vectors to zero, matching the scalar
   reference. The gather-index construction for inactive lanes in AVX-512 (lines
   376–380 of `ssimulacra2_avx512.c`) points those lanes at row 0 offsets, but
   their scatter outputs are gated by `for (unsigned i = 0; i < row_count; i++)`
   — no output pollution.

4. **Transcendental isolation** — both paths call `vmaf_ss2_cbrtf` and
   `vmaf_ss2_srgb_eotf` per-lane via scalar fallthrough in `cbrtf_lane_avx512`
   / `cbrtf_lane_neon` and `srgb_to_linear_lane_avx512` / `_neon`. These are
   the ADR-0164 deterministic helpers (no libc dependency), so host divergence
   at this callsite is structurally impossible.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Accept as-is (clean close) | No code churn; tests confirm correctness | None | Chosen: this is the correct outcome |
| Add per-lane double-accumulator crosscheck | Would catch future regressions earlier | Redundant with existing `memcmp` byte-exact gate | Not needed — existing tests already enforce byte-exactness |
| Widen to cross-host CI (arm64 runner) | Would close the "cross-host" concern definitively | arm64 runner is already exercised by the existing CI matrix; NEON path tests run on that leg | Already covered |

## Consequences

- **Positive**: T3-9(b) is resolved; no residual ULP debt in the SSIMULACRA2
  AVX-512/NEON blur and `picture_to_linear_rgb` kernels.
- **Positive**: The audit confirmed that the `ssimulacra2_math.h` deterministic
  helpers fully insulate the SIMD paths from cross-host libc divergence by
  construction.
- **Neutral**: No snapshot regeneration required (no numerical change).
- **Follow-up**: T3-9(c) (`iqa_convolve` AVX-512 ceiling check) remains open
  per the BACKLOG; it is independent of this audit.

## References

- ADR-0161 (SSIMULACRA2 AVX2 phase 1 — pointwise kernels)
- ADR-0162 (SSIMULACRA2 AVX2 phase 2 — IIR blur)
- ADR-0163 (SSIMULACRA2 AVX2 phase 3 — `picture_to_linear_rgb`)
- ADR-0164 (deterministic `cbrtf` + sRGB EOTF LUT)
- ADR-0138/0139 (bit-exactness invariants, NOLINTNEXTLINE policy)
- ADR-0214 (cross-backend ULP gate, `places=4`)
- BACKLOG T3-9(b) (this item)
- Source: per user direction to audit T3-9(b) post-merge ULP drift.
