<!-- markdownlint-disable MD013 -->
# ADR-0162: SSIMULACRA 2 IIR blur SIMD ports — AVX2 + AVX-512 + NEON (T3-1 phase 2)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, avx2, avx512, neon, ssimulacra2, bit-exact, iir-blur, performance

## Context

Phase-1 SIMD port of SSIMULACRA 2 ([ADR-0161](0161-ssimulacra2-simd-bitexact.md))
vectorised 5 pointwise kernels across AVX2 + AVX-512 + NEON, leaving
the **FastGaussian IIR blur** (`fast_gaussian_1d` + `blur_plane`) and
`picture_to_linear_rgb` scalar. The blur is called **30× per frame**
(5 blur-pass combinations × 6 scales) and is the single largest
wall-clock cost in the extractor.

Scalar blur structure:

1. `blur_plane` runs a 2-pass separable IIR:
   - **Horizontal**: `fast_gaussian_1d` once per row (`h` calls).
     3-pole recursive Gaussian (Charalampidis 2016), serial across
     columns.
   - **Vertical**: per-column IIR state maintained in
     `s->col_state` (6 × `w` floats). Iterates rows top-to-bottom
     with `(lrow + rrow)` input + per-column state updates.
2. Both passes share the same coefficients
   (`rg_n2[3]`, `rg_d1[3]`, `rg_radius`).

Vectorisation challenges:

- **Horizontal pass**: recurrence is serial across columns *within
  a row*. Cannot vectorise within a row. Must vectorise *across
  rows* — batch N rows together, run the IIR on all N in lock-step.
  Each lane holds one row's state.
- **Vertical pass**: per-column state is already columnar in
  `col_state`. Naturally SIMD across `w` via contiguous loads/stores.

## Decision

Port `blur_plane` to all three ISAs as a single SIMD entry point
(`ssimulacra2_blur_plane_{avx2,avx512,neon}`) that handles both
passes under the ADR-0161 bit-exactness contract.

**Horizontal pass** — row-batching:

- AVX2: 8 rows / batch. Use `_mm256_i32gather_ps` with a stride-`w`
  index vector to read column `n` from 8 consecutive rows into
  one `__m256`.
- AVX-512: 16 rows / batch. Use `_mm512_i32gather_ps` (same shape).
- NEON: 4 rows / batch. aarch64 has no gather — assemble the lane
  vector via 4 explicit `vsetq_lane_f32` calls (one per lane).

After SIMD IIR math per iteration of `n`, output is stored via
`{_mm256,_mm512,vst1q}_store_ps` to an aligned scratch buffer
followed by N scalar per-row stores (AVX2/AVX-512 have no scatter;
NEON naturally uses scalar stores). Tail rows (< N) handled by
calling the same function with a `row_count` argument — inactive
lanes don't write to output.

**Vertical pass** — column-SIMD:

- 8 / 16 / 4 columns per iter. Contiguous aligned loads from
  `prev1_*`/`prev2_*` arrays, IIR math, aligned stores.
- Scalar tail for leftover columns (identical to scalar reference).

Bit-exactness preserved because:

- Per-lane SIMD arithmetic is IEEE-754 lane-commutative (each lane
  computes the exact scalar sequence in isolation).
- Summation order is preserved: `n2_k * sum - d1_k * prev1_k - prev2_k`
  keeps its left-to-right form per lane.
- Output pooling uses `(o0 + o1) + o2` in the same left-to-right
  order as scalar's `o0 + o1 + o2`.
- Scalar tails in the vertical pass match the scalar reference
  body verbatim.

Dispatch: new `blur_fn` pointer in `Ssimu2State`, assigned in
`init_simd_dispatch()` alongside the 5 phase-1 pointers. `NULL`
keeps the scalar fallback; non-NULL replaces both
`blur_plane` + `fast_gaussian_1d` with the single SIMD entry point.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Row-batching + column-SIMD, both passes (this ADR)** | Covers 30× per-frame calls; bit-exact by construction; both hot loops vectorised | Horizontal pass needs gather (AVX2/AVX-512) / per-lane lane-sets (NEON) — not the cleanest SIMD | **Chosen** — full coverage + bit-exact, real wall-clock win |
| **Vertical pass only, horizontal stays scalar** | Simpler port (~50% of the code); vertical pass is naturally columnar | Horizontal pass is 50% of blur time; leaving it scalar loses real wall-clock on the largest hot path | Rejected per user popup — "both passes via row-batching" |
| **Transpose 8×W chunks to scratch, run column-SIMD across n for both passes** | Horizontal can reuse vertical's column-SIMD path | Transpose doubles memory traffic; not clearly faster than gather | Rejected — gather is cheap enough on modern hardware, avoids transpose complexity |
| **3-pole SIMD within a single row** | No row-batching needed | Only 3 lanes active in a 4-wide vector (or fewer); < 1.5× speedup; not worth the complexity | Rejected — trivial speedup, breaks the "meaningful wall-clock" bar |
| **Defer to follow-up PRs per ISA** | Smaller PRs | Splinter the commit history; user asked for all three in one PR | Rejected per user direction |

## Consequences

- **Positive**:
  - Single biggest SSIMULACRA 2 wall-clock target vectorised on
    all three ISAs, bit-exact to scalar.
  - Phase-1 + phase-2 together cover 7 of 8 hot kernels; only
    `picture_to_linear_rgb` (`powf` EOTF, called 2× per frame)
    remains scalar — lowest ROI, follow-up PR.
  - `Ssimu2State` now carries a complete SIMD dispatch table; new
    hot kernels slot in via `init_simd_dispatch()`.
  - New `test_blur` subtest in `test_ssimulacra2_simd.c` pins the
    bit-exactness contract on reproducible xorshift32 inputs.
- **Negative**:
  - Three SIMD TUs now carry ~700 LoC of IIR blur code each. The
    gather + scalar-store pattern for the horizontal pass is not
    the cleanest SIMD — readability carve-out via `NOLINT`
    (ADR-0141) cited inline.
  - NEON horizontal pass uses 4 explicit `vsetq_lane_f32` calls
    per input vector (no native gather on aarch64); this is the
    established pattern but slower than a true gather.
- **Neutral / follow-ups**:
  - `picture_to_linear_rgb` SIMD port — per-lane scalar `powf`
    tmul SIMD. Small ROI (2 calls / frame).
  - T3-3 SSIMULACRA 2 snapshot-JSON regression test — still
    pending (gated on `tools/ssimulacra2` availability).
  - `-ffp-contract=off` already scoped to the per-ISA ssimulacra2
    static libs (ADR-0161 fix); no change in this PR.

## Verification

- `test_ssimulacra2_simd` — **6/6 subtests pass** on AVX-512 host
  (blur auto-dispatches to AVX-512; AVX2 exercised when AVX-512
  absent).
- `qemu-aarch64-static build-aarch64/test/test_ssimulacra2_simd`
  — **6/6 subtests pass** (NEON, including `test_blur`).
- `meson test -C build` x86: clean (no prior-test regression).
- clang-tidy clean on the three SIMD TUs + dispatch TU + test TU
  (all NOLINT cite ADR-0141 inline).

## References

- [ADR-0130](0130-ssimulacra2-scalar-implementation.md) — scalar
  SSIMULACRA 2 port.
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — AVX2
  bit-exact precedent.
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — per-lane scalar
  reduction pattern.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — NOLINT scope.
- [ADR-0161](0161-ssimulacra2-simd-bitexact.md) — phase-1 pointwise
  reduction SIMD ports (this ADR is phase 2).
- libjxl FastGaussian reference:
  [`lib/jxl/gauss_blur.cc`](https://github.com/libjxl/libjxl/blob/main/lib/jxl/gauss_blur.cc).
- Research digest: [`docs/research/0016-ssimulacra2-iir-blur-simd.md`](../research/0016-ssimulacra2-iir-blur-simd.md).
- User popup 2026-04-24: "Both passes (vertical + horizontal via
  row-batching)".

### AVX-512 audit 2026-05-09: AUDIT-PASS at 1.461x (full ssimulacra2 pipeline)

T3-9 sub-row (b) bench-first audit on Ryzen 9 9950X3D (Zen 5,
AVX-512F/BW/VL). The IIR-blur AVX-512 phase ships in this ADR's
companion source (`ssimulacra2_avx512.c`); audit-mode wall-clock on
the full ssimulacra2 pipeline (PTLR + IIR blur + scoring) at the
Netflix normal pair (480 frames, single-thread, median of 3) shows
AVX2 4.681 s vs AVX-512 3.203 s = **1.461x** — clears the 1.3x
ship threshold. The IIR blur is the dominant kernel by cost share
per the original ADR-0162 profile, so the speedup is dominated by
this phase. NEON path on `qemu-aarch64-static` was not re-run in
this audit (no host-NEON regression suspected; T3-9 scope is
AVX-512-on-host).

Bit-exactness: AVX-512 vs AVX2 score JSON byte-identical at full
precision (`--precision max`); 0/48 frames diverge.
`test_ssimulacra2_simd::test_blur` passes on the audit build.

See [Research-0089](../research/0089-avx512-audit-sweep-2026-05-09.md)
for the full bench table.
