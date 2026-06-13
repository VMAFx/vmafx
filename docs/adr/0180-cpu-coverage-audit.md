# ADR-0180: CPU coverage matrix audit — close 5 stale gaps

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, audit, fork-local, doc-correction

## Context

While picking the next CPU SIMD gap-fill from the post-T7-19
metrics-backends matrix, four supposed gaps and one open backlog
item all turned out to be either already-shipped work or stale
audit notes from earlier matrix snapshots. Filling them as code
PRs would have been wasted effort; treating the matrix as
authoritative without re-verification would have left the same
phantom gaps in the audit dossier indefinitely.

The five items audited:

1. **T7-22 — `ms_ssim` SSIM-per-scale SIMD**: backlog said "still
   scalar"; in fact `float_ms_ssim::init` already dispatches
   `iqa_ssim_set_dispatch` to AVX2 / AVX-512 / NEON variants of
   `ssim_precompute` / `ssim_variance` / `ssim_accumulate` per
   ADR-0138 / 0139 / 0140. Empirical: 3.2× wall-clock speedup vs
   `--cpumask 0xfffffffe` on Netflix normal pair (576×324, 48
   frames).
2. **CAMBI scalar fallback**: matrix bullet claimed "no pure-C
   scalar path"; in fact
   [`cambi.c`](../../core/src/feature/cambi.c)
   ships `increment_range` / `decrement_range` /
   `get_derivative_data_for_row` as scalar defaults in `init`,
   overridden only when AVX2 / AVX-512 / NEON is detected.
3. **motion_v2 NEON**: matrix bullet claimed "x86 SIMD but no
   NEON"; in fact
   [`arm64/motion_v2_neon.c`](../../core/src/feature/arm64/motion_v2_neon.c)
   is a 13kB bit-exact NEON port, fully wired in dispatch and
   meson sources, shipped since 2026-04.
4. **integer `ansnr`** matrix row: there is no `integer_ansnr`
   registered extractor; the row was a misread of `ansnr_tools.c`
   (helpers used by ADM, not a standalone feature). Only
   `float_ansnr` ships.
5. **T7-21 — `psnr_hvs` AVX-512**: this is a real audit
   question, not a phantom gap. The benchmark below settles it.

## Decision

We close all five items in one audit ADR, update the
metrics-backends matrix to reflect ground truth, and document the
psnr_hvs AVX-512 verdict empirically rather than by extrapolation.

**Verdict on psnr_hvs AVX-512**: **AVX2 ceiling — do not
implement.** Wall-clock benchmark on the Netflix normal pair
(576×324, 48 frames):

| path | wall-time | speedup vs scalar |
| --- | --- | --- |
| `psnr_hvs` scalar (`--cpumask 0xfffffffe`) | 0.123 s | — |
| `psnr_hvs` AVX2 (default) | 0.105 s | 1.17× |

The 8-wide AVX2 DCT (`od_bin_fdct8x8_avx2`) is already
bandwidth-amortised — going to 16 lanes (AVX-512) would force a
2-block batch in the host loop, double the register pressure, and
deliver no measurable wall-clock benefit because the per-block
scalar reductions (variance, mask, error) remain scalar by design
(ADR-0138 / 0139 / 0140 keep float reductions per-lane scalar for
bit-exactness vs scalar). The kernel-side gain would be more
than offset by the host-side complication.

The same conclusion already applies to the deferred
**float_moment AVX-512** path (ADR-0179 § Alternatives
considered): "AVX-512 wouldn't deliver a measurable win over AVX2
here" — the moment reductions are memory-bound on a 4096-pixel
frame. AVX-512 closes as AVX2 ceiling for both kernels.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Close each item with its own ADR + matrix edit | One audit decision per ADR — cleaner per-decision history | 5 ADRs for what is one audit pass; bulk of each ADR would be the same "matrix was stale, code is fine" reasoning | Bundling into one audit ADR captures the **lesson** (re-verify before code work) instead of distributing it across five micro-decisions |
| Implement psnr_hvs AVX-512 anyway | Closes the matrix row to ✓ | 1.17× scalar speedup of AVX2 already shows the DCT is not the bottleneck on practical fixtures; AVX-512 widening to 2-block batch adds host-loop complexity without payoff | Code work without measurable benefit; "AVX2 ceiling" is the correct close-out per the T7-21 backlog description's own escape clause |
| Implement float_moment AVX-512 anyway | Symmetry with AVX2 + NEON pair shipped in PR #122 | Memory-bound reduction on a 4096-pixel frame has no headroom for wider lanes (already documented in ADR-0179 § Alternatives) | Same as psnr_hvs — AVX2 ceiling |

## Consequences

- **Positive**: matrix is now ground-truth; downstream backlog
  reflects actual code reality; no engineering time wasted on
  AVX-512 ports that wouldn't move the needle. Saves ~2-3 PRs
  worth of motion that would have closed as "no measurable
  difference" anyway.
- **Negative**: confidence in the matrix as a source of truth
  drops slightly — five stale entries means the matrix audit
  cadence needs tightening. Mitigation: every PR that closes a
  matrix gap now updates the matrix in the same diff (ADR-0179
  set the precedent; T7-19 closure carries the example).
- **Neutral / follow-ups**: matrix corrections land in
  [`.workingdir2/analysis/metrics-backends-matrix.md`](../../.workingdir2/analysis/metrics-backends-matrix.md)
  (gitignored — planning surface). Backlog corrections land in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md):
  T7-21 → CLOSED (AVX2 ceiling), T7-22 → CLOSED (already done),
  the cambi/motion_v2/integer_ansnr items had no backlog rows.
  The remaining real backlog items in the CPU SIMD column are
  zero; the next gap surface is GPU long-tail
  (T7-23..T7-25 + everything else not on
  CUDA / SYCL / Vulkan).

## References

- Source: user direction 2026-04-26 ("why not fill the cpu gaps
  across the table?") plus the matrix-vs-code re-verification
  pass that exposed the staleness.
- Pattern parent: [ADR-0179](0179-float-moment-simd.md) § "no
  AVX-512 — kernel is memory-bound" reasoning, applied to two
  more kernels with the same shape.
- Backlog rows: T7-21, T7-22 in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md) —
  both closed by this ADR.
- Matrix: [`metrics-backends-matrix.md`](../../.workingdir2/analysis/metrics-backends-matrix.md)
  — five rows / bullets corrected.
- Files re-verified during audit:
  [`float_ms_ssim.c:88`](../../core/src/feature/float_ms_ssim.c#L88),
  [`cambi.c:446-460`](../../core/src/feature/cambi.c#L446),
  [`arm64/motion_v2_neon.c`](../../core/src/feature/arm64/motion_v2_neon.c),
  [`integer_motion_v2.c:202-204`](../../core/src/feature/integer_motion_v2.c#L202).
