<!-- markdownlint-disable MD013 -->
# ADR-0192: GPU long-tail batch 3 — closing every remaining metric gap (motion_v2 / float_ansnr / ssimulacra2 / cambi + float twins)

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, cuda, sycl, vulkan, feature-extractor, fork-local

## Context

Batch 1 ([ADR-0182](0182-gpu-long-tail-batch-1.md)) closed `psnr` /
`float_moment` / `ciede` across all three backends (PR #125 → #137).
Batch 2 ([ADR-0188](0188-gpu-long-tail-batch-2.md)) closed
`float_ssim` / `float_ms_ssim` / `psnr_hvs` (PR #139 → #144). The
remaining matrix gaps after batch 2 fall into two groups:

### Group A — metrics with no GPU twin at all

| Metric | CPU LOC | Notes |
| --- | --- | --- |
| `integer_motion_v2` | 300 | Builds directly on `integer_motion` (already on GPU). Extra step is a per-row absolute difference + clamped accumulation; the convolve scaffolding is shared. |
| `float_ansnr` | 124 | Anti-noise SNR. Per-pixel `(ref - dis)²` accumulator + `noise = mean(dis²)` — two reductions, no spatial filter. The fork ships float-only; no integer variant exists upstream. |
| `ssimulacra2` | 1118 | Recent fork addition (ADR-0130). XYB color transform + 4-stage IIR low-pass + per-stage ssim-style combine. Float-domain throughout. |
| `cambi` | 1533 | Banding detection. Multi-scale per-pixel range tracking with a *sequential* per-row state update (`get_derivative_data_for_row`) — the hardest GPU port in the long tail. |

### Group B — float twins of metrics whose int variant is on GPU

`float_psnr`, `float_adm`, `float_vif`, `float_motion` exist as CPU-
only paths in the matrix while the matching `integer_*` variant ships
on CUDA + SYCL + Vulkan. The Netflix default models use the integer
path; the float variants exist for (1) reproducing Netflix's
published `float_*` scores and (2) the cross-backend gate's
float-vs-int sanity check.

This ADR commits to closing **both groups** in one batch, not split
across two. Per direction taken in the popup decision (option 3 of
the scoping question on 2026-04-27): finish the GPU long-tail in one
logical chunk so the matrix has a clean "every metric has a GPU
twin" terminus.

## Decision

### Per-metric ordering

Ship in **ascending complexity**, mirroring batch 2's pattern:

1. **`integer_motion_v2`** first. Smallest (300 LOC), reuses the
   already-shipped `integer_motion` Vulkan/CUDA/SYCL convolve as a
   subroutine. Validates that "delta on top of an existing kernel"
   composes cleanly across backends before tackling new compute
   shapes.
2. **`float_ansnr`** second. Tiny (124 LOC), no spatial filter —
   pure per-pixel reduction. Same partial-sum pattern as `psnr`,
   second reduction for the `noise` term. Establishes the "two-
   parallel-reductions" idiom that `cambi` later reuses for its
   multi-scale accumulators.
3. **Float twins of int metrics on GPU** — `float_psnr`,
   `float_motion`, `float_vif`, `float_adm`. Shipped in this order:
   smallest first (psnr → motion → vif → adm). Each is structurally
   the float-domain twin of an int kernel that already exists on
   each backend; the kernel work is mostly translating the integer
   accumulator to `float` and the post-processing log/divide. **Not
   aliased** to the int kernels — see alternatives.
4. **`ssimulacra2`** fourth. ~1100 LOC of XYB transform + IIR + per-
   stage SSIM-style compute. Uses the SSIM scaffolding from batch 2
   (ADR-0189 / 0190) as a reusable subroutine for the per-stage
   compute, but ssimulacra2's IIR low-pass is sequential along the
   long axis (forward + backward pass per row). The IIR coefficients
   are runtime-fixed — implement as a per-row dispatch with a
   work-group serial scan.
5. **`cambi`** last. The range-tracking state is sequential per
   row; the GPU port has to re-shape the algorithm into a per-row
   parallel scan (Hillis-Steele or Kogge-Stone) plus the multi-scale
   pyramid. Highest implementation risk; landing it last means every
   other batch-3 metric's review is already closed before cambi
   review starts.

### Per-backend ordering (within each metric)

Same as batches 1 + 2: **Vulkan → CUDA → SYCL**. Vulkan GLSL is the
clean reference; CUDA + SYCL ports follow once the numerical
contract is locked.

### Precision contracts (measured-first per ADR-0188's pattern)

| Metric | Target | Rationale |
| --- | --- | --- |
| `integer_motion_v2` | `places=4` | Integer reduction; matches the `integer_motion` precedent. Bit-exactness possible if convolve reuses the existing kernel. |
| `float_ansnr` | `places=3` | Float-domain log10 final transform compresses per-pixel error. Same shape as `float_psnr` (which we measure first). |
| `float_psnr` / `float_motion` / `float_vif` / `float_adm` | `places=3` | Float accumulators + log10/divide post-process. Looser than the integer twin's `places=4` because the int kernels can keep `int64` partials whereas float partials lose precision in the per-WG reduction. |
| `ssimulacra2` | `places=2` | Multi-stage float pipeline (XYB + IIR + SSIM-combine + log). Each stage's float rounding accumulates. May surprise upward; measure first per ADR-0188. |
| `cambi` | `places=2` | Multi-scale + log post-process. Sequential range-tracking forces a per-row parallel scan that re-orders float adds vs the CPU. Likely the loosest contract. |

Each per-metric ADR (one per Vulkan PR, mirroring batches 1 + 2)
locks the actual measured floor.

### Chroma handling

- `integer_motion_v2`: luma-only (matches `integer_motion`).
- `float_ansnr`: luma-only.
- Float twins: same plane mask as the corresponding int kernel.
- `ssimulacra2`: needs all three planes (XYB color transform).
  Chroma upload via the `0x7` bitmask landed in PR #137.
- `cambi`: luma-only by default, optional chroma extension via the
  existing `enc_bitdepth` knob.

### Per-PR deliverables

Same six deliverables as batches 1 + 2, per
[ADR-0108](0108-deep-dive-deliverables-rule.md):

1. Kernel + host glue.
2. New metric entry in
   [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
   `FEATURE_METRICS` (first backend's PR per metric only).
3. Lavapipe lane step in
   [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
   (Vulkan PRs only).
4. CHANGELOG bullet + matrix update + features.md row update.
5. Per-metric ADR for the Vulkan PR (ADR-0193..0199 reserved
   for batch 3 per-metric ADRs — ssimulacra2 and cambi will likely
   eat ADR slots for sub-stages too, so the actual count may drift).
6. State.md row + rebase-notes entry per CLAUDE §12 r13 / r14.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Defer cambi to batch 4** | Cambi's sequential-state port is the riskiest single chunk; isolating it lets batch 3 ship faster | Splits the long-tail terminus across two batches; the matrix never reaches a "fully closed" state for the duration of the split | User direction (popup, 2026-04-27): finish the long-tail in one batch. The risk-isolation argument is valid but doesn't beat shipping a closed matrix. |
| **Skip float twins (Group B), only do Group A** | Halves the PR count (12 vs 21+); the int twins already serve every production model path | Breaks the cross-backend gate's float-vs-int sanity coverage; Netflix's published `float_*` scores can't be reproduced on GPU | Same user direction — the popup explicitly named "all remaining gaps". The cross-backend gate's value compounds with float twin coverage. |
| **Alias float twins to the int kernel + a host-side post-quantize** | Saves ~12 PRs of kernel work | The int kernel operates on `uint8`/`uint16` quantized samples; the float kernel takes `[0, 255]` linear floats. Aliasing would either silently quantize the float input (wrong) or run two kernels back-to-back (no win) | The two kernels operate on fundamentally different domains; a thin wrapper would mis-represent one as the other. Better to ship native float kernels. |
| **One mega-PR for the whole batch** | One review pass | ~7 metrics × 3 backends ≈ 21 PRs' worth of code in one diff (~10k LOC). Mixed precision contracts. Bisect impossible if a regression slips. | Per-PR granularity is non-negotiable at this scale. Same answer as batches 1 + 2. |
| **Lock all precision contracts at `places=2` upfront** | Easier to set CI thresholds; no per-metric measurement overhead | Sells precision short for the integer-reduction-friendly metrics (motion_v2, the float twins of int kernels). Already shown by batches 1 + 2 that several metrics land at `places=4` empirically. | Measure first, set the contract second — same approach as ciede (ADR-0187), ssim (ADR-0189), ms_ssim (ADR-0190). |

## Consequences

- **Positive**: closes the GPU long-tail in one logical chunk. After
  this batch, every registered feature extractor in the fork has at
  least one GPU twin (lpips remains delegated to ORT execution
  providers per [ADR-0022](0022-inference-runtime-onnx.md)).
  Per-PR scope stays in the ~500-1000 LOC range — same shape as
  batches 1 + 2.
- **Negative**: **largest batch yet by PR count**. 7 metrics × 3
  backends = 21 PRs minimum, plus 7+ per-metric ADRs. ssimulacra2
  and cambi may each split into multiple sub-PRs (XYB + IIR + ssim-
  combine for ssimulacra2; multi-scale + scan for cambi), pushing
  the count toward 30. Total review surface is ~3× batch 2.
- **Negative**: **cambi is the biggest implementation-risk chunk in
  the entire long-tail effort**. Its `get_derivative_data_for_row`
  state update is sequential per row and pixel; the GPU port has
  to either (a) re-implement as a parallel scan with proven-correct
  algebra, or (b) fall back to a per-row dispatch with no
  intra-row parallelism (waste). The choice gets locked in cambi's
  per-metric ADR after a feasibility spike.
- **Neutral / follow-ups**:
  1. `integer_motion_v2_vulkan` first (ADR-0193).
  2. CUDA + SYCL twins follow per batch-2 cadence.
  3. `float_ansnr_vulkan` next (ADR-0194).
  4. Float twins (4 metrics × 3 backends = 12 PRs) ship as a
     middle phase — could be parallelised across the three
     backends if review bandwidth allows.
  5. `ssimulacra2_vulkan` likely splits into 2-3 PRs (XYB +
     IIR + compute) — to be decided in its per-metric ADR
     after reviewing the GLSL shape.
  6. `cambi_vulkan` last (ADR-0199 or later). Must be preceded
     by a feasibility spike (parallel-scan algebra for the
     range-tracking state).
  7. Once batch 3 closes, the matrix at
     `.workingdir2/analysis/metrics-backends-matrix.md` should
     show every row with at least one GPU `✓★`. Subsequent GPU
     work shifts from "long-tail" to "polish" (alternative
     algorithms, perf tuning, half-precision experiments).

## References

- Parent: [ADR-0182](0182-gpu-long-tail-batch-1.md) — batch 1 scope.
- Sibling: [ADR-0188](0188-gpu-long-tail-batch-2.md) — batch 2 scope.
- Per-metric ADRs (batch 1 + 2 precedent): [ADR-0183](0183-ffmpeg-libvmaf-sycl-filter.md)
  ... [ADR-0191](0191-psnr-hvs-vulkan.md).
- CPU references for batch 3:
  [`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c),
  [`float_ansnr.c`](../../core/src/feature/float_ansnr.c),
  [`ssimulacra2.c`](../../core/src/feature/ssimulacra2.c),
  [`cambi.c`](../../core/src/feature/cambi.c).
- User direction: AskUserQuestion popup, 2026-04-27 — "All remaining
  gaps in one batch" / "Yes, draft ADR-0192 now".

### Status update 2026-05-09: T3-15 first port landed

The successor backlog item T3-15 (GPU coverage long-tail batch 4)
shipped its first proof-of-concept port — CUDA `psnr` chroma extension
(`psnr_cuda` now emits `psnr_y` / `psnr_cb` / `psnr_cr`). Cross-backend
gate at `places=4` clears bit-exactly (0/48 mismatches on the Netflix
normal pair across all three planes). Seven follow-up kernels remain
(SYCL PSNR chroma, CUDA + SYCL chroma SSIM / MS-SSIM, CUDA + SYCL
`cambi`); see
[`docs/research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md`](../research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md)
for the corrected gap re-audit and per-kernel ordering. The body of
this ADR remains frozen per ADR-0028 — this status note is in the
References section only.

### Status update 2026-05-09: T3-15 #2 SYCL PSNR chroma

T3-15(b) — the SYCL twin of the CUDA PSNR chroma extension — landed
in the same session. `psnr_sycl` now emits `psnr_y` / `psnr_cb` /
`psnr_cr`. The implementation differs from CUDA in one structural
respect: SYCL's existing shared frame buffer
(`vmaf_sycl_shared_frame_init`) is luma-only by design (see
[`core/src/sycl/common.h`](../../core/src/sycl/common.h)), so
chroma planes ride on per-extractor device buffers populated by
host-side staging copies in the combined-graph `pre_fn`. Luma stays
graph-recorded; chroma kernels run direct in `post_fn` on the same
in-order combined queue. Cross-backend gate
(`scripts/ci/cross_backend_vif_diff.py --feature psnr --backend sycl
--places 4`) clears bit-exactly on Intel Arc A380 (0/48 mismatches
on the Netflix normal pair across all three planes,
`max_abs_diff = 0.0`). Six follow-up kernels remain (CUDA + SYCL
chroma SSIM / MS-SSIM, CUDA + SYCL `cambi`).
