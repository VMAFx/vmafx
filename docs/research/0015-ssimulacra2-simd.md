# Research digest 0015 — SSIMULACRA 2 SIMD ports (T3-1 + T3-2)

- **Status**: Active (captures the decision path for ADR-0161)
- **Related ADRs**: [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
  (scalar port, prior art), [ADR-0161](../adr/0161-ssimulacra2-simd-bitexact.md)
  (this PR), [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md)
  (per-lane double reduction), [ADR-0141](../adr/0141-touched-file-cleanup-rule.md)
  (NOLINT scope)

## The question

How do we vectorise the scalar SSIMULACRA 2 pipeline across AVX2 +
AVX-512 + NEON under the fork's byte-for-byte bit-exactness contract,
given that two of the six hot kernels call transcendentals (`cbrtf`,
`powf`) that vector libm cannot match byte-exact, and one kernel
(`fast_gaussian_1d`) is a serial IIR recurrence?

## Scalar profile

Per-frame call counts in `extract`:

| Kernel | Calls / frame | Vectorizable cleanly? |
| --- | --- | --- |
| `picture_to_linear_rgb` | 2 | Matmul yes, but `srgb_to_linear` uses `powf` |
| `linear_rgb_to_xyb` | 12 (2 × 6 scales) | Matmul yes; `cbrtf` per-lane scalar |
| `multiply_3plane` | 18 | Trivial |
| `blur_3plane` (IIR) | 30 | Horizontal pass: serial; vertical pass: per-column parallel |
| `ssim_map` | 6 | Yes (pointwise SIMD + scalar tail reduction) |
| `edge_diff_map` | 6 | Yes |
| `downsample_2x2` | 10 | Yes |

The IIR blur dominates real frame-level cost (30 × 3 planes × two
1D passes). The pointwise kernels are individually cheaper but
together take ~40% of the non-IIR frame time.

## Key design axes

### Axis 1 — bit-exactness strategy for `cbrtf`

Options:

1. **Per-lane scalar libm inside SIMD loop** (picked). Spill
   vector to aligned scratch, apply scalar `cbrtf`, reload.
   Byte-identical to scalar by construction. Cost: a few loads /
   stores per 8/16/4 lanes; the surrounding matmul + rescale
   still vectorises.
2. **Polynomial approximation in SIMD**. ~3-8× the throughput,
   but typically 1-2 ULP drift vs scalar libm — would require a
   tolerance ADR. Rejected: SSIMULACRA 2 doesn't have a Netflix
   golden pinning today (ADR-0130 deferred T3-3 snapshot gate);
   opening tolerance now compounds verification debt.
3. **Keep the whole kernel scalar**. Simpler but forgoes the
   matmul + XYB-rescale speedup on 12 calls/frame.

Option 1 is clean, bit-exact, and still delivers a meaningful
fraction of the SIMD win on `linear_rgb_to_xyb` (matmul is 3
dot-products per pixel; cbrtf is 3 libm calls — the SIMD vs
scalar delta is on the matmul plumbing, not the transcendental).

### Axis 2 — summation order for matmul and downsample

IEEE-754 add is non-associative. Scalar expressions like
`kM00 * r + m01 * g + kM02 * b + kOpsinBias` parse left-to-right
as `((a + b) + c) + d`. Naïve SIMD pairing
`((a + b) + (c + d))` drifts by 1 ULP.

Fix: mirror the scalar chain exactly. Three sequential SIMD
`add_ps` ops per 8/16/4-lane vector. Extra instructions, zero
correctness risk. **Caught at first integration run** by the
bit-exact test — the cost of re-running the test once saved
a CI round-trip.

### Axis 3 — reduction pattern for ssim_map / edge_diff_map

Scalar reduces per-pixel `double` contributions into running
`double` accumulators, using `(double)` casts at each float-to-
double site. SIMD-reducing float-lane-then-lift-to-double
breaks the summation tree.

Picked: **per-lane scalar tail**. SIMD computes the pointwise
float arithmetic (`mu1*mu1`, `mu2*mu2`, etc.), spills the
intermediate vectors to aligned scratch, then the scalar inner
loop iterates over 8 (AVX2) / 16 (AVX-512) / 4 (NEON) lanes
consuming the same `(double)` casts and accumulator variables
as the scalar reference. Matches ADR-0139 verbatim.

### Axis 4 — 2×2 downsample deinterleave

Scalar pairs up 4 input pixels per output; SIMD wants to process
8/16/4 output lanes per iter. Deinterleave even/odd positions
via:

- AVX2: `vshufps` (imm 0x88 / 0xDD) + `vpermpd` (imm 0xD8) to
  straighten cross-lane.
- AVX-512: `vpermt2ps` with explicit index vectors (cleaner,
  fewer ops).
- NEON: `vuzp1q_f32` / `vuzp2q_f32` (direct deinterleave).

All three preserve scalar's
`((r0_even + r0_odd) + r1_even) + r1_odd` summation order.

### Axis 5 — what NOT to vectorise in this PR

Deliberately **scalar** in this PR:

- `fast_gaussian_1d` + `blur_plane` — serial recurrence on
  horizontal pass; per-column IIR state on vertical pass
  (SIMD-able but requires a separate column-batch rewrite). Biggest
  single-PR scope bump if included.
- `picture_to_linear_rgb` — `powf` per lane would be the same
  spill/reload trick as `cbrtf`, but Y/U/V unpack + BT.709
  matmul + clamp + sRGB EOTF is a 50-line vectorised loop; a
  follow-up PR buys better review granularity.

Follow-up PR targets: (1) IIR blur vertical-pass column batching
— biggest wallclock ROI. (2) YUV → linear RGB. (3) Optional:
`fast_gaussian_1d` per-pole parallelism via 3-wide state vector.

## Verification plan

1. **Unit test** `test_ssimulacra2_simd` — bit-exact vs inline
   scalar reference on 5 SIMD kernels with reproducible xorshift32
   inputs at W=33 × H=21 (deliberately non-multiple of 8/16/4 to
   exercise tails).
2. **Runtime dispatch** — AVX-512 overrides AVX2 on x86; NEON on
   aarch64; scalar fallback when no SIMD is available.
3. **Cross-backend check** — planned for a follow-up with the
   IIR blur port since blur is where scalar drift would first
   compound.
4. **CI** — `Netflix CPU Golden Tests` doesn't currently exercise
   ssimulacra2 (ADR-0130 is a fork extension), so the bit-exact
   test is the gate.

## Outcome

Shipped as `simd/ssimulacra2-avx2-avx512-neon` branch →
PR (TBD). 15 new kernel functions across three SIMD TUs + 1
test harness + 2 meson edits + 1 dispatch helper in the
scalar TU + 5 docs updates.

## Open questions / follow-ups

- **IIR blur vectorisation** — plan a separate PR focused on
  `blur_plane` vertical-pass column batching (4 / 8 / 16
  columns' IIR state advancing in lock-step per input row).
- **`powf` per-lane in `picture_to_linear_rgb`** — duplicates
  the `cbrtf` pattern; worth ~2% of frame time; not urgent.
- **AVX-512 `vpermt2ps` vs AVX2 `vshufps+vpermpd`** — the
  AVX-512 form is cleaner; consider refactoring the AVX2 code
  to match if a future kernel needs the same deinterleave
  (blur column gather, maybe).
- **SVE / SVE2 port** — same deferral as ADR-0160; revisit when
  native SVE2 CI hardware is routine.
- **T3-3 snapshot gate** — separate PR; gated on
  `tools/ssimulacra2` availability.
