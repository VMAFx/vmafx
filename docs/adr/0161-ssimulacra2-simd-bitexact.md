# ADR-0161: SSIMULACRA 2 SIMD bit-exact ports — AVX2 + AVX-512 + NEON (T3-1 + T3-2)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, avx2, avx512, neon, ssimulacra2, bit-exact, performance

## Context

Backlog items T3-1 (AVX2) + T3-2 (AVX-512 + NEON) called for a
SIMD port of the scalar SSIMULACRA 2 extractor
[`core/src/feature/ssimulacra2.c`](../../core/src/feature/ssimulacra2.c)
(ADR-0130). Per user popup 2026-04-24, scope is **all three ISAs
in a single PR** with the same byte-for-byte bit-exactness
contract as prior fork SIMD ports (ADR-0138 / ADR-0139 /
ADR-0159 / ADR-0160).

The scalar hot path per frame:

1. YUV → linear RGB (2×)
2. Per-scale × 6 scales:
   - Linear RGB → XYB (2×)
   - Elementwise `mul` for σ² maps (3×)
   - 3-pole IIR Gaussian blur (5×)
   - SSIM map pooling (1×, 3 planes × L1/L4 averages)
   - Edge-diff map pooling (1×, 3 planes × art/det × L1/L4)
3. Between-scale 2×2 box downsample (2× per of 5 inter-scale
   transitions)

Call counts per frame:
`picture_to_linear_rgb` ×2, `linear_rgb_to_xyb` ×12,
`multiply_3plane` ×18, `blur_3plane` ×30, `ssim_map` ×6,
`edge_diff_map` ×6, `downsample_2x2` ×10.

Two kernels trip bit-exactness for naïve SIMD:

- `linear_rgb_to_xyb` includes `cbrtf`, which no vector libm
  reliably matches byte-for-byte with the scalar libm.
- `picture_to_linear_rgb` includes `powf` (sRGB EOTF), same
  story.

The IIR `fast_gaussian_1d` / `blur_plane` has a serial
recurrence that can only SIMD across columns on the vertical
pass (per-column state advances independently). Vectorising that
would lift real frame-level wallclock; it is **explicitly
deferred** to a follow-up PR.

## Decision

Port five of the eight pointwise / reduction kernels to all
three ISAs (AVX2 + AVX-512 + NEON) under a byte-for-byte
bit-exactness contract vs the scalar reference:

1. `multiply_3plane` — pure pointwise mul.
2. `linear_rgb_to_xyb` — matmul + per-lane scalar `cbrtf` + XYB
   rescale.
3. `downsample_2x2` — 2×2 box filter with scalar-order sequential
   adds.
4. `ssim_map` — pointwise SIMD with per-lane `double` reduction
   (ADR-0139 pattern).
5. `edge_diff_map` — abs + div + pooling with per-lane `double`
   reduction.

The IIR blur and the YUV→linear-RGB pipeline remain scalar in
this PR — their vectorisation is non-trivial (serial recurrence
and `powf` respectively) and will land as separate PRs.

Bit-exactness strategy:

- **Lane-commutative arithmetic**: pointwise float `add` / `sub`
  / `mul` / `max` are bit-equivalent to scalar under
  `FLT_EVAL_METHOD == 0` because each SIMD lane computes
  independently with the same op. No horizontal reductions on
  single-precision floats.
- **Left-to-right summation order**: all expressions preserve
  scalar's `((a + b) + c) + d` chaining — IEEE-754 add is
  non-associative, and the regression test confirmed that
  `(a + b) + (c + d)` drifts by ~1 ULP.
- **Per-lane scalar libm for transcendentals**: `cbrtf` is
  called per lane via scalar `libm` inside an aligned spill/
  reload. Byte-identical to scalar because the exact same libm
  implementation runs per lane.
- **Double-precision reductions**: `ssim_map` and
  `edge_diff_map` accumulate into `double` via a scalar tail over
  each 8-/16-/4-lane SIMD batch. The SIMD arithmetic fills the
  per-lane spill scratch, then the scalar tail loop hits `double`
  exactly as scalar does. Mirrors the ADR-0139 pattern.
- **No FMA contraction**: `#pragma STDC FP_CONTRACT OFF` at each
  TU header. GCC on aarch64 emits `-Wunknown-pragmas` (non-fatal;
  default FP-contract is "off" on aarch64 anyway) — the pragma
  is kept for portability with clang / MSVC.

Runtime dispatch in
[`ssimulacra2.c`](../../core/src/feature/ssimulacra2.c)
via a `init_simd_dispatch()` helper: scalar default → AVX2 if
host supports it → AVX-512 overrides AVX2 if supported (x86) →
NEON on aarch64 if supported. Kept as a separate helper so
`init()` stays under the ADR-0141 function-size threshold.

Lint carve-outs (all NOLINT with ADR citation inline):

- `linear_rgb_to_xyb_{avx2,avx512,neon}` exceed
  `readability-function-size` — matmul + per-lane cbrtf + XYB
  rescale kept together for scalar-diff audit.
- `ssim_map_{avx2,avx512}` exceed the same threshold — the SIMD
  pointwise + double-accumulator tail are semantically coupled
  per ADR-0139.
- `downsample_2x2_neon` similarly coupled.
- Test TU's `ref_downsample_2x2` is a line-for-line scalar copy,
  carried for auditability.
- Test TU's `memcmp` on float buffers: byte-exact equality is
  precisely the contract we're testing — NOLINTNEXTLINE with
  `bugprone-suspicious-memory-comparison` citation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Bit-exact pointwise + per-lane cbrtf (this ADR)** | Matches fork's SIMD contract; no tolerance ADR; five of eight kernels vectorised; all three ISAs in one PR | IIR blur deferred (not yet vectorised) | **Chosen** — preserves the "SIMD = scalar" discipline; partial kernel coverage ships real wins on the pointwise hot paths |
| **Vectorise `cbrtf` with a polynomial** | 3-8× speedup on the `linear_rgb_to_xyb` kernel | Any polynomial approximation drifts from scalar libm's `cbrtf` by 1-2 ULP; requires a separate tolerance ADR + new snapshot | Rejected — ADR-0130 snapshot gate (T3-3) is still pending; opening tolerance now compounds the verification debt |
| **Vectorise the IIR blur too (now)** | Biggest frame-level wallclock win; 30×/frame call count | Serial recurrence limits SIMD to per-column batching on the vertical pass; AVX2/NEON variants need different column-chunking; doubles the PR surface | Deferred — shipping the pointwise wins today lets reviewers audit the simpler kernels first; IIR follows as a focused PR |
| **AVX2 only this PR, AVX-512 + NEON later** | Smaller PR | User popup explicitly asked for all three ISAs in one PR | Rejected per user direction |
| **Use existing SIMD DX helpers (H1/H2/H3)** | Reuses the fork's T2-3 framework | Helpers target horizontal reductions + mirror tails, neither of which applies here; cbrtf spill/reload is a novel pattern | Deferred — IIR blur port is a better fit for those helpers |

## Consequences

- **Positive**:
  - SSIMULACRA 2 on CPU gets a SIMD fast path on x86 (AVX2 +
    AVX-512) and aarch64 (NEON). Byte-identical to scalar by
    construction; no tolerance loosening on the upstream
    libjxl reference pipeline.
  - ISA-parity matrix: scalar + AVX2 + AVX-512 + NEON.
  - Five AVX2 kernels + five AVX-512 kernels + five NEON kernels
    all covered by a new unit test `test_ssimulacra2_simd.c`
    with reproducible xorshift32 inputs; 5/5 subtests pass on
    x86 (AVX2 + AVX-512 on the same host) and 5/5 pass under
    `qemu-aarch64-static` (NEON).
  - All touched files clang-tidy clean under build + build-aarch64.
- **Negative**:
  - **IIR blur + YUV→linear-RGB not vectorised yet** — the
    biggest wallclock cost per frame. Tracked as follow-up work:
    `blur_plane` vertical-pass column batching (biggest ROI) and
    `picture_to_linear_rgb` per-lane scalar `powf` (lower ROI).
  - The three SIMD TUs are structurally similar but not
    templatised — future kernel additions must be mirrored in
    all three. Mitigated by the unit test catching any
    skew-from-scalar.
- **Neutral / follow-ups**:
  - T3-3 SSIMULACRA 2 snapshot-JSON regression test still
    pending (gated on `tools/ssimulacra2` availability).
  - A follow-up PR should vectorise the IIR blur + `picture_to_
    linear_rgb`. `fast_gaussian_1d` per-pole parallelism and
    `blur_plane` per-column batching are the obvious targets.
  - `#pragma STDC FP_CONTRACT OFF` ignored on aarch64 GCC —
    tracked in ADR-0160 already; no action here.

## Verification

- `meson test -C build` — 37/37 pass (36 prior + new
  `test_ssimulacra2_simd`).
- `test_ssimulacra2_simd` — 5/5 subtests pass on AVX-512 host
  (auto-dispatches to AVX-512; AVX2 exercised via the same code
  path when AVX-512 is absent).
- `qemu-aarch64-static build-aarch64/test/test_ssimulacra2_simd`
  — 5/5 subtests pass (NEON).
- clang-tidy `-p build` on all three SIMD TUs + dispatch TU
  test TU: zero un-NOLINT'd warnings. NOLINTs all cite
  ADR-0141 / ADR-0139 / this ADR.
- assertion-density PASS, check-copyright PASS, pre-commit PASS.

## References

- [ADR-0130](0130-ssimulacra2-scalar-implementation.md) — scalar
  SSIMULACRA 2 port (libjxl FastGaussian IIR reference).
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — AVX2
  bit-exact via `__m256d` precedent.
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — per-lane scalar
  reduction pattern for bit-exact float accumulators.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule + NOLINT scope.
- [ADR-0159](0159-psnr-hvs-avx2-bitexact.md) — AVX2 bit-exact
  precedent for a similar "scalar reference + per-lane SIMD"
  pattern.
- [ADR-0160](0160-psnr-hvs-neon-bitexact.md) — NEON sister-port
  precedent; same half-wide / full-wide split idioms re-used.
- libjxl SSIMULACRA 2 reference:
  [`tools/ssimulacra2.cc`](https://github.com/libjxl/libjxl/blob/main/tools/ssimulacra2.cc).
- Research digest: [`docs/research/0015-ssimulacra2-simd.md`](../research/0015-ssimulacra2-simd.md).
- User direction 2026-04-24 popup: "Alles in einem PR durchziehen,
  egal wie lang" (all three ISAs in one PR).

### AVX-512 audit 2026-05-09: AUDIT-PASS at 1.461x

T3-9 sub-row (b) bench-first audit on Ryzen 9 9950X3D (Zen 5,
AVX-512F/BW/VL). 480-frame Netflix normal pair fixture, single-thread
median of 3 wall-clock runs across the full ssimulacra2 pipeline
(PTLR + IIR blur + scoring): AVX2 4.681 s vs AVX-512 3.203 s =
**1.461x** (clears 1.3x ship threshold).

Bit-exactness: AVX-512 vs AVX2 score JSON byte-identical at full
precision (`--precision max`); 0/48 frames diverge for any feature.
`test_ssimulacra2_simd` 13/13 subtests pass on the audit build.
Cross-backend gate clean.

Re-affirms phase-1 ADR-0161 + phase-2 ADR-0162 + phase-3 ADR-0163
ship decisions on a faster machine; no post-merge cross-host snapshot
drift detected. Closes former T3-10 backlog row (residual ULP audit).

See [Research-0089](../research/0089-avx512-audit-sweep-2026-05-09.md)
for the full bench table.
