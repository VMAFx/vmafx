<!-- markdownlint-disable MD013 -->
# ADR-0188: GPU long-tail batch 2 — psnr_hvs / ssim / ms_ssim across CUDA / SYCL / Vulkan

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, cuda, sycl, vulkan, feature-extractor, fork-local

## Context

[ADR-0182](0182-gpu-long-tail-batch-1.md) defined a three-batch
GPU long-tail rollout. Batch 1 (psnr / moment / ciede) closed
between PR #125 and PR #137 — every batch-1 metric now has a
twin on all three GPU backends. Empirical contracts:
`places=4` for the integer-reduction kernels (psnr / moment),
`places=4` for ciede on real hardware after the per-WG-float

- host-double pattern locked in (ADR-0187).

Batch 2 picks up the **next-most-impactful** trio:

- **`ssim`** — single-scale SSIM. CPU reference is the Xiph
  integer port at
  [`core/src/feature/integer_ssim.c`](../../core/src/feature/integer_ssim.c)
  (286 LOC) with a separable Gaussian filter (kernel size
  computed dynamically from σ; ~9 taps at σ=1.5) and per-pixel
  ΔE-style combine over 6 weighted moments (μx, μy, x², xy,
  y², w). Float twin
  ([`float_ssim.c`](../../core/src/feature/float_ssim.c),
  211 LOC) follows the same shape with `float` accumulators.
- **`ms_ssim`** — multi-scale SSIM. Stack of 5 SSIM levels with
  decimation between them
  ([`ms_ssim.c`](../../core/src/feature/ms_ssim.c) +
  [`ms_ssim_decimate.c`](../../core/src/feature/ms_ssim_decimate.c)).
  Each level is a 2× downsample + SSIM compute. Final score is
  a weighted product of per-scale SSIMs. Approximately 4× the
  work of single-scale SSIM.
- **`psnr_hvs`** — DCT-based perceptual PSNR. CPU reference is
  the Xiph third-party port at
  [`core/src/feature/third_party/xiph/psnr_hvs.c`](../../core/src/feature/third_party/xiph/psnr_hvs.c)
  (473 LOC). Per-block 8×8 DCT with quantization-table-weighted
  squared-error accumulation. Largest of the three —
  introduces the first DCT compute kernel in the fork's GPU
  set.

## Decision

Ship batch 2 as **per-metric, per-backend PRs** (mirroring
batch 1's natural granularity), but with a tighter scoping
decision up-front than batch 1 had:

### Per-metric ordering

1. **ssim** first across all three backends. Smallest kernel,
   provides the separable-filter scaffolding that ms_ssim
   reuses.
2. **ms_ssim** second. Builds directly on top of the ssim
   kernel — each scale is one ssim invocation; the only new
   piece is the 2× decimation + per-scale-weight combine.
3. **psnr_hvs** last. DCT is structurally different from the
   filter+reduce pattern; landing it after the others
   keeps the dependency graph simple (no metric depends on
   psnr_hvs scaffolding).

### Per-backend ordering (within each metric)

Same as batch 1: **Vulkan → CUDA → SYCL**. Vulkan's GLSL is the
cleanest reference shape; CUDA + SYCL ports follow once the
numerical contract is locked.

### Precision contract

- **ssim / ms_ssim**: target `places=4` against the CPU
  scalar reference. Both extractors do `float`-domain combine
  over int64-accumulated moments (Xiph's integer SSIM is
  exact within `int64`'s range; the float version uses
  `float`/`double`). Per-WG partial sums of the per-pixel
  SSIM contribution + host `double` reduction (same pattern
  as ciede) for the final mean. **Bit-exactness is NOT the
  contract** — the divisions inside the SSIM combine
  (`(2μxμy + C1)(2σxy + C2) / ((μx² + μy² + C1)(σx² + σy² + C2))`)
  introduce float rounding the CPU scalar references avoid by
  staying in `double`. Empirical floor will be measured per
  PR and either holds at `places=4` or the contract relaxes
  to `places=3` with a note in the per-metric ADR (mirrors
  ciede's "measured first, set the contract second" approach).
- **psnr_hvs**: target `places=2`. The DCT block math + per-
  block masking thresholds combine many `float` operations;
  bit-exactness is not realistic. Final score is `10·log10`
  of a sum, so per-pixel error gets compressed in the log
  transform — the same effect that let ciede land at
  `places=4` may or may not apply here. Measure first.

### Chroma handling

- **ssim / ms_ssim**: luma-only on the GPU side (matches
  Xiph's primary use case — most published SSIM scores are
  luma). Chroma SSIM is a follow-up if a model demands it.
- **psnr_hvs**: emits `psnr_hvs_y / psnr_hvs_cb / psnr_hvs_cr
  / psnr_hvs`. The combined `psnr_hvs` metric needs all three
  planes. CUDA path can use the `0x7` upload bitmask landed
  in PR #137 (T7-23 batch 1c parts 2 + 3) for free; Vulkan +
  SYCL allocate chroma staging at init time.

### Per-PR deliverables

Each per-metric/per-backend PR ships:

1. The kernel + host glue (mirrors batch 1's per-PR shape).
2. A new metric entry in
   [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
   `FEATURE_METRICS` (only on the **first** backend's PR per
   metric — subsequent backends reuse the entry).
3. A new step in the lavapipe lane of
   [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
   (Vulkan PRs only — CUDA + SYCL gates run locally; no
   self-hosted CI lane today).
4. CHANGELOG bullet + matrix update + features.md row update.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **One mega-PR for all 3 metrics × 3 backends** | One review pass | ~3000+ LOC of kernel + host across 9 files, mixed precision contracts, harder to bisect numerical regressions | Per-PR granularity matches batch 1's shipping cadence; reviewers can focus on one kernel at a time |
| **Skip `ms_ssim`, only do `ssim` + `psnr_hvs`** | Less work | ms_ssim is in widespread use (default model variants) — leaving it CPU-only when ssim is on GPU is asymmetric | ms_ssim builds on ssim's filter scaffolding, marginal extra work |
| **Defer `psnr_hvs` to batch 3** | Skip the DCT compute kernel for now | `psnr_hvs` is the last per-metric gap on the matrix; finishing batch 2 closes more rows of the GPU coverage matrix in one logical chunk | DCT kernels are well-understood (every video codec has them); not worth a separate batch |
| **Target `places=2` everywhere from the start** | Looser contract, fewer surprises | We already shipped `places=4` for the integer-reduction kernels in batch 1; weakening to `places=2` for ssim/ms_ssim without measuring first sells the precision short | Set by measurement, not by guessing — same approach as ciede (ADR-0187) |

## Consequences

- **Positive**: every per-metric / per-backend PR stays in the
  ~500-1000 LOC range — same shape as batch 1's PRs. Reviewers
  can focus on one kernel + numerical contract at a time. The
  ssim → ms_ssim ordering means ms_ssim reuses an already-
  reviewed filter implementation.
- **Negative**: 9 PRs to close batch 2 (3 metrics × 3 backends).
  Bigger total review surface than batch 1's 7 PRs. Each PR
  also drags the per-metric ADR (lock the precision contract
  per kernel), so 3 small follow-up ADRs in addition to this
  scoping ADR.
- **Neutral / follow-ups**:
  1. `ssim` Vulkan first PR. Then CUDA, then SYCL. Per
     [ADR-0182](0182-gpu-long-tail-batch-1.md)'s precedent.
  2. `ms_ssim` follows the same per-backend cadence.
  3. `psnr_hvs` follows the same per-backend cadence.
  4. Each per-metric Vulkan PR ships a per-metric ADR
     (ADR-0189 for ssim_vulkan, etc.).
  5. Once batch 2 closes, the GPU long-tail matrix (see
     `.workingdir2/analysis/metrics-backends-matrix.md`)
     should have all metrics with at least one GPU twin.
     Remaining gaps per backend become batch 3 scope.

## References

- Parent: [ADR-0182](0182-gpu-long-tail-batch-1.md) — GPU
  long-tail batch 1 scope (closed by PR #137).
- Sibling kernels: psnr (Vulkan PR #125, CUDA #129, SYCL #130),
  moment (Vulkan #133, CUDA + SYCL #135), ciede
  (Vulkan #136, CUDA + SYCL #137).
- CPU references:
  [`integer_ssim.c`](../../core/src/feature/integer_ssim.c),
  [`ms_ssim.c`](../../core/src/feature/ms_ssim.c),
  [`third_party/xiph/psnr_hvs.c`](../../core/src/feature/third_party/xiph/psnr_hvs.c).
