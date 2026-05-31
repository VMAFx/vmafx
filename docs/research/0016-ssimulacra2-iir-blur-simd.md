# Research digest 0016 — SSIMULACRA 2 IIR blur SIMD (T3-1 phase 2)

- **Status**: Active (captures the decision path for ADR-0162)
- **Related ADRs**: [ADR-0161](../adr/0161-ssimulacra2-simd-bitexact.md)
  (phase-1 pointwise), [ADR-0162](../adr/0162-ssimulacra2-iir-blur-simd.md)
  (this PR), [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md),
  [ADR-0141](../adr/0141-touched-file-cleanup-rule.md)

## The question

How do we vectorise the 3-pole FastGaussian IIR blur across 3 ISAs
without breaking scalar bit-exactness, given the serial recurrence
that rules out within-row vectorisation?

## Axis 1 — horizontal pass: row-batching vs transpose vs scalar

The recurrence `o = n2 * (l + r) - d1 * prev1 - prev2` is serial
within a row. To SIMD:

**Row-batching** (chosen): process N rows together, each SIMD lane
holding one row's state. Requires strided column-n loads:

- AVX2: `_mm256_i32gather_ps` with stride-w indices — 8 rows / batch.
- AVX-512: `_mm512_i32gather_ps` — 16 rows / batch.
- NEON: 4 explicit `vsetq_lane_f32` per input vector (no gather
  on aarch64).

Output store: AVX2/AVX-512 have no scatter — spill SIMD vector
to aligned scratch, then N scalar stores to the N row outputs.
NEON naturally uses scalar stores.

**Transpose** (rejected): copy 8×W chunks into columnar scratch,
run column-SIMD IIR, transpose back. Roughly 2× memory traffic.
Gather is cheap enough on modern HW to beat this in practice.

**Scalar horizontal + SIMD vertical only** (rejected per user
popup): leaves ~50% of blur time scalar.

## Axis 2 — vertical pass: columnar IIR

Scalar stores per-column state in `s->col_state` (6 × w floats).
Columns are already contiguous → naturally SIMD. Process 8 / 16 / 4
columns per iter. Scalar tail handles `w % N` leftover columns.

## Axis 3 — bit-exactness

Per-lane SIMD arithmetic is IEEE-754 lane-commutative: each lane
computes the same sequential `n2*sum - d1*prev1 - prev2` on its own
row's data, identical to scalar. Summation order `(o0 + o1) + o2`
at output time preserves scalar's `o0 + o1 + o2`.

Tested with `test_blur` in `test_ssimulacra2_simd.c`:

- W=33, H=21 (deliberately non-multiples of 4/8/16 → exercises tails)
- xorshift32-seeded random input in `[-0.5, 0.5]`
- Realistic sigma=1.5 IIR coefficients
- `memcmp` of output buffers with NOLINT for ADR-0162 byte-exact
  contract
- Passes on AVX-512 host (auto-dispatched), under QEMU aarch64
  (NEON).

## Axis 4 — function-size carve-outs

Both `hblur_Nrows_*` and `vblur_simd_*cols_*` exceed the fork's
`readability-function-size` ceiling. ADR-0141 NOLINT citations
inline: the SIMD main loop + scalar tail share IIR state that
spilling across function boundaries would force to memory,
defeating the vectorisation.

## Outcome

Shipped as `simd/ssimulacra2-blur-iir` branch → PR TBD. One new
kernel function per ISA (3 TUs × 2 helpers + 1 public entry
point), ~700 LoC total, plus test harness + scalar reference copy
for bit-exactness audit.

## Open questions / follow-ups

- **`picture_to_linear_rgb` SIMD** — 2 calls per frame, per-lane
  scalar `powf`. Low ROI but mechanical. Last scalar hot path in
  the extractor.
- **T3-3 snapshot-JSON regression test** — still pending.
- **SVE2 port** — shipped under [ADR-0213](../adr/0213-ssimulacra2-sve2.md)
  with qemu validation; CI hardware is not on the critical path. The
  fixed 4-lane `svwhilelt_b32(0, 4)` predicate keeps the SVE2 IIR
  blur byte-identical to the NEON sibling regardless of runtime vector
  length, so the per-lane reduction order from this digest carries
  over verbatim.
- **Benchmark on native aarch64** — QEMU timings aren't
  representative; real hardware numbers are a follow-up once the
  native-aarch64 runner is routinely available.
