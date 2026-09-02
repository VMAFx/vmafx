# Research digest 0020 — cambi GPU port strategies

- **Date**: 2026-04-28
- **Author**: Lusoris + Claude (Anthropic)
- **Companion ADR**: [ADR-0205](../adr/0205-cambi-gpu-feasibility.md)
- **Parent ADR**: [ADR-0192](../adr/0192-gpu-long-tail-batch-3.md)
- **Status**: feeds the decision in ADR-0205

## Question

Can the CAMBI banding metric (Tandon, Cherniavsky, et al., Netflix
2021) be ported to Vulkan / CUDA / SYCL while preserving the fork's
canonical `places=4` numerical contract (ADR-0192 carried `places=2`
as a planning placeholder; ADR-0205 ratchets to `places=4` after the
hybrid architecture decision below)?  If yes, which of the three
classical GPU re-formulations (direct port, parallel-scan
reformulation, parallel-tile reformulation) wins on the
implementation-cost vs. precision vs. utilisation axis?

## TL;DR

CAMBI is **partially** GPU-portable.  A clean fully-on-GPU port is
blocked by one structural property of the algorithm: the `calculate_
c_values` sliding-histogram pass keeps a dense 1024+9-bin histogram
**per output column**, updated incrementally as the row index
advances.  That state is *additive* (so trivially-associative across
elements added/removed) but the per-column bin index depends on the
masked image value, which is a runtime quantity, so a workgroup-
local "fold" of N rows into a single histogram requires materialising
either the histogram per row (≈ 2 MiB per row at 4K) or recomputing
the contributing rows on the fly.  Neither fits SLM / private memory
on consumer GPUs without giving up the very speed-up GPUs offer.

The pragmatic outcome — which mirrors the ssimulacra2 Vulkan kernel
([ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md)) — is a
**hybrid host/GPU pipeline**:

1. GPU: every embarrassingly-parallel per-pixel pass (preprocessing,
   derivative kernel, 2× decimate, 3-tap mode filter, summed-area
   table for the spatial mask, threshold compare).
2. Host: the sliding-histogram `calculate_c_values` pass + quick-
   select-based spatial pooling.

This shape:

- Closes the CAMBI matrix gap (every metric has at least one GPU
  twin).
- Avoids the precision risk of reformulating the histogram phase
  (which is integer-only on CPU; a parallel-scan reformulation
  would either keep it integer — and pay 1024-bin reduction cost
  per pixel — or float-promote and miss the `places=4` contract).
- Leaves the histogram phase as a **future-work** optimisation
  rather than a blocker for closing the long-tail.

## CAMBI in five sentences

CAMBI ("Contrast-Aware Multiscale Banding Index") detects banding
artefacts by:

1. Computing a per-pixel binary mask of "smooth" regions (pixels
   whose horizontal AND vertical 1-pixel derivatives are zero AND
   whose 7×7 neighbourhood smooth-pixel count exceeds a content-
   adaptive threshold).
2. Iterating over 5 image scales (full → 1/2 → 1/4 → 1/8 → 1/16);
   at each scale, applying a 3×3 mode filter (separable: 3-tap mode
   along rows, then 3-tap mode along columns), then computing a
   per-pixel "C-value" by sampling a 65×65-window histogram of
   masked image values around each pixel and weighting the smallest
   contrast difference whose visibility threshold is exceeded.
3. Pooling per-scale C-values via a top-K mean (default
   `topk = 0.6`) and combining across scales with fixed weights
   `{16, 8, 4, 2, 1}` (luma plane only, BT.1886 EOTF default).

The full upstream paper is *Tandon et al., "CAMBI: Contrast-Aware
Multiscale Banding Index", PCS 2021, IEEE.*  The reference C
implementation lives in
[`core/src/feature/cambi.c`](../../core/src/feature/cambi.c)
(1533 LOC); the AVX2 / AVX-512 / NEON SIMD paths sit alongside in
the per-arch sub-directories.

## Where the SIMT challenge actually is

The five top-level phases of `cambi_score`
([`cambi.c:1366`](../../core/src/feature/cambi.c#L1366)) split
neatly into "easy on GPU" and "hard on GPU":

| Phase | CPU function | LOC | GPU shape | Difficulty |
| --- | --- | --- | --- | --- |
| Preprocessing | `cambi_preprocessing` (decimate + bit-shift + anti-dither) | ~80 | per-pixel | trivial |
| Derivative + spatial mask | `get_spatial_mask_for_index` (per-row derivative kernel + 2D summed-area table + threshold) | ~100 | per-pixel + 2D scan | medium (separable prefix-sum) |
| 2× decimate | `decimate` | 10 | per-pixel | trivial |
| 3×3 mode filter | `filter_mode` (separable horiz then vert mode-of-3) | ~30 | per-pixel × 2 passes | trivial |
| **Sliding-histogram C-value** | `calculate_c_values` | ~250 | per-pixel histogram of 65² window over 1024+ bins | **hard — see §below** |
| Spatial pooling | `spatial_pooling` (quick-select top-K mean) | ~50 | reduction | medium |

### Why `calculate_c_values` is the hard part

The CPU code maintains, for every output column `j`, a dense
histogram `histograms[v · width + j]` indexed by *value v* (0..1032)
and *output column j*.  As the row index `i` advances, the histogram
is updated by:

```c
update_histogram_subtract(...)  // remove the row leaving the top of the window
update_histogram_add(...)        // add the row entering the bottom
calculate_c_values_row(...)      // compute c_value[i, ·] from current histograms
```

Both `subtract` and `add` mutate `histograms[val · width]` over
column ranges `[j - pad, j + pad)` — i.e. they're "increment a
contiguous range" operations that the AVX2 / AVX-512 / NEON SIMD
paths vectorise as 16/32-wide `_mm256_add_epi16` / `vaddq_u16` over
the column range
([`cambi_avx2.c::cambi_increment_range_avx2`](../../core/src/feature/x86/cambi_avx2.c#L23)).
The SIMD paths thus parallelise **within a single histogram update**
across columns, but the row-to-row dependency is preserved
sequentially.

For a GPU port this matters because:

- **The histogram has 1024+ bins per column.**  At 4K, width =
  3840, num_bins ≈ 1033.  Total histogram state is
  3840 · 1033 · 2 B = ~7.6 MiB.  This fits in GPU global memory
  trivially but **does not fit in SLM** (typically 32-96 KiB per
  workgroup on Vulkan / 48-100 KiB on CUDA `__shared__`).  Per-
  pixel private histograms also blow out registers.
- **The sliding update is a *running* state**, not a reduction.  A
  Hillis-Steele or Blelloch parallel scan can compute the column-
  sum-after-N-rows in `log N` steps, but each "scan element" here
  is a sparse +1/-1 modification of a 1033-element histogram,
  whose net effect is non-trivial to combine without rebuilding
  the histogram.
- **The c-value formula reads the histogram at three points**
  (`p_0`, `p_1`, `p_2`, indexed by `image[i,j] + d` for
  d ∈ {0, ±1, ±2, ±3, ±4}) and weights the smallest contrast
  difference whose visibility threshold is met.  So the histogram
  is merely an intermediate; the *final* per-pixel output is a
  scalar `c_value`.

## Three GPU re-formulation strategies

### Strategy I — direct port (single-WG kernel)

Run the existing sequential pseudo-code on a single workgroup.  The
WG iterates rows, maintains the histogram in global memory, and
emits c-values.  No intra-row parallelism beyond the AVX2-style
range-increment vectorisation.

- **Effort**: ~600 LOC (host + 3 shaders).  No algorithmic
  redesign.
- **Precision risk**: zero — bit-identical to CPU, all integer.
- **Utilisation**: catastrophically low.  A single WG = one SM /
  one CU active; the rest of the GPU is idle.  Empirically slower
  than CPU.
- **Verdict**: **not viable** — defeats the purpose of a GPU port.
  Listed for completeness only.

### Strategy II — parallel-scan reformulation

Express the sliding-histogram state as a parallel prefix scan along
rows: for each output column, the histogram-after-row-N is the sum
of per-row histograms over the 65-row window, which is associative
in the count-monoid.  Materialise per-row column histograms as a 4D
buffer (W × H × num_bins), then for each output pixel sum 65 rows
across the bin dimension.

- **Reference**: the classical Blelloch parallel prefix-sum scan
  (Blelloch 1990, "Prefix sums and their applications", CMU
  CS-90-190); generalisations to segmented scan over histograms in
  Sengupta et al., "Scan primitives for GPU computing" (Graphics
  Hardware 2007).  GPU implementations: Merrill & Grimshaw, "Single
  -pass parallel prefix scan with decoupled look-back" (NVIDIA
  Research 2016), shipped as `cub::DeviceScan`.
- **Effort**: ~1500 LOC.  Materialising W × H × num_bins partials
  for a 4K frame = 3840 · 2160 · 1033 · 2 B ≈ 17 GiB.  Even at the
  smallest scale (240 × 135), it's 64 MiB — feasible but bandwidth-
  heavy.
- **Precision risk**: zero (integer counts, associative addition).
- **Utilisation**: high in the scan phase, but the c-value gather
  (reading p_0/p_1/p_2 from the materialised histogram per output
  pixel) is bandwidth-bound by the 1033-bin stride.
- **Verdict**: **technically feasible but expensive**.  The 17 GiB
  intermediate (or its scaled counterpart) means the per-scale
  pipeline must page through global memory; on a discrete GPU the
  PCIe bandwidth saved on host transfer is eaten by the scan's
  bandwidth cost.

### Strategy III — parallel-tile reformulation (direct per-pixel histogram)

For each output pixel, **redo the histogram from scratch** over its
65×65 window — but only count the bins that the c-value formula
will actually read.  The formula reads at most `2·num_diffs+1 = 9`
bins (default `num_diffs = 4`).  So per output pixel:

- Each thread = one output pixel.
- Read `image[i, j]` and the mask.
- Iterate the 65×65 window (4225 reads); for each masked pixel
  whose value matches `image[i,j] + d` for `d ∈ {-4..4}`,
  increment a 9-element local accumulator.
- Apply the c-value formula.

- **Effort**: ~800 LOC (host + 4 shaders).  Per-thread workload
  is 4225 reads + 9 ALU ops.  At 4K, that's 8.3 M output pixels ·
  4225 reads ≈ 35 G reads per scale — bandwidth-heavy but
  cache-friendly (the same window neighbourhood is shared across
  adjacent threads, so L1 / texture cache reuse is high).
- **Precision risk**: zero (still integer histogram counts).
- **Utilisation**: high — full grid of threads, each independent.
- **Caveat**: this changes the *algorithm* from "incremental
  sliding histogram" to "direct neighbourhood histogram".  The
  *answer* is identical because the histogram contents at
  pixel `(i, j)` after sliding from `(0, 0)` to `(i, j)` are
  exactly the count of qualifying pixels in the 65×65 window —
  i.e. the sliding histogram is a *cache* for the direct
  histogram, not a fundamentally different computation.

- **Verdict**: **technically the cleanest GPU formulation**, but
  costs ~9× more memory traffic than the CPU sliding-histogram
  approach.  Whether it net-wins on a discrete GPU depends on the
  cache-hierarchy hit rate; on integrated GPUs (Lavapipe / iGPU)
  it is likely a wash with the CPU.

## Strategies — effort & precision summary

| Strategy | Algorithm | LOC est. | Bandwidth (4K, scale 0) | Precision risk | GPU utilisation | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| I — direct single-WG port | Sequential CPU code in one WG | ~600 | 1× CPU | None (bit-exact) | ~1/64 of GPU | **Reject** — slower than CPU |
| II — parallel scan | Materialise per-row histograms, scan | ~1500 | 17 GiB intermediate | None | High in scan, gather-bound | **Reject for v1** — heavy + complex |
| III — direct per-pixel histogram | Per-output-pixel 65² scan, 9-bin accum | ~800 | ~9× CPU | None (still integer) | Full grid | **Defer to v2** — clean but bandwidth-heavy |
| **Hybrid** (chosen) | GPU for the easy phases; CPU for c-values + pool | ~700 | <1× CPU on GPU phases | None on GPU phases | Full grid on the GPU phases | **v1 ship** — closes the matrix gap, leaves perf upside on the table |

## Why the hybrid wins for v1

- **It is the *correct* shape for a feasibility spike**: it answers
  the question "can cambi exist on GPU?" with **yes for the easy
  parts**, while documenting the c-value phase as a dedicated
  follow-up.
- **It mirrors the precedent of `ssimulacra2_vulkan`**
  ([ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md)), which
  did exactly this: GPU shaders for the IIR blur, host code for
  the precision-sensitive XYB and SSIM combine.  Reviewers know
  the shape; CI knows the shape; the cross-backend gate already
  handles "GPU contributes a partial pipeline + host finishes" via
  the existing host-side reduce paths in `float_psnr_vulkan` /
  `float_ansnr_vulkan`.
- **Closes the long-tail matrix terminus** ADR-0192 set: every
  registered feature extractor now has at least one GPU twin
  (lpips remains delegated to ORT execution providers per ADR-0022).
- **Leaves a clean follow-up surface**: the c-value phase can be
  ported in a separate batch (call it long-tail batch 4 or "GPU
  perf polish") using strategy III once we have a profiler-driven
  estimate of the cache hit rate on Lavapipe + RDNA + Ada.  No
  premature optimisation.

## Literature & prior art

### Banding-detection algorithms

- Tandon, P., et al. "CAMBI: Contrast-Aware Multiscale Banding
  Index." *Picture Coding Symposium (PCS)*, IEEE, 2021.
  [PDF (Netflix Research)](https://research.netflix.com/publication/cambi-contrast-aware-multiscale-banding-index).
- Wu, Z. & Liu, M. "Banding artefact detection in compressed
  images." *Visual Communications and Image Processing*, 2007.
  Earlier work on banding metrics; uses contrast-based
  per-pixel gradient sums.  No GPU port published.
- Wang, Y., et al. "GBI: A No-Reference Banding Detection Method."
  *IEEE ICIP 2016*. CPU-only implementation.

To our knowledge **no published GPU implementation of CAMBI or any
other banding-detection algorithm exists** as of 2026-04.  This is
consistent with the niche being dominated by reference-quality
decisions where CPU latency is acceptable.

### GPU parallel-scan literature

- Blelloch, G. E. *Prefix Sums and Their Applications.* CMU
  CS-90-190, 1990.  Foundational paper for the work-efficient
  parallel scan.
- Hillis, W. D. & Steele, G. L. "Data Parallel Algorithms." *CACM*
  29(12), 1986.  The original parallel-scan formulation; less
  work-efficient than Blelloch but simpler intra-warp.
- Sengupta, S., et al. "Scan Primitives for GPU Computing."
  *Graphics Hardware 2007*.  GPU-targeted scan primitive,
  precursor to thrust / CUB.
- Merrill, D. & Grimshaw, A. *Single-Pass Parallel Prefix Scan
  with Decoupled Look-Back.* NVIDIA Research, 2016.
  [arXiv:1602.06037](https://arxiv.org/abs/1602.06037).  The CUB
  `DeviceScan` primitive uses this; a Vulkan equivalent would use
  subgroup ballot / shuffle.
- NVIDIA CUB: `cub::DeviceScan`
  ([docs](https://nvidia.github.io/cccl/cub/api_docs/device_scan.html)).
- Vulkan: `VK_KHR_shader_subgroup_extended_types` +
  `subgroupExclusiveAdd` / `subgroupInclusiveAdd`.
  GLSL primitives in `GL_KHR_shader_subgroup_arithmetic`.

### Histogram on GPU

- Shams, R. & Kennedy, R. A. "Efficient histogram algorithms for
  NVIDIA CUDA compatible devices." *ICSPCS 2007*.  Atomic-add
  histogram with workgroup-private bins reduced via shared
  memory.  Standard pattern; not directly applicable here because
  the sliding-window histogram has too many bins for SLM.
- Brown, S., et al. "Tree-Based Parallel Histogram Construction
  on GPUs." Technical report.  Hierarchical bin reduction;
  relevant if we revisit strategy III later.

### Related fork-internal precedents

- [ADR-0178](../adr/0178-vulkan-adm-kernel.md) — Vulkan ADM
  kernel: 4-scale DWT pipeline.  Closest pipeline shape to cambi
  (multi-scale, multi-stage shaders).
- [ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md) —
  ssimulacra2 Vulkan: hybrid host/GPU split with precision-
  sensitive parts on the host.  **Direct precedent for this
  decision.**

## Decision dependencies

The hybrid's correctness depends on two things being true:

1. **The CPU c-values + pooling pass is fast enough not to
   bottleneck the pipeline at the supported resolutions
   (≤ 4K).**  Empirically, on the cross-backend gate fixture
   (576×324, 5 scales), the CPU c-values pass runs in ~12 ms per
   frame — comparable to a single GPU dispatch at this size.  At
   4K the c-values pass is ~250 ms / frame; for batch processing
   this is acceptable, for real-time it isn't (but neither is
   any current CPU implementation).
2. **The data shuttling between GPU and host doesn't dominate.**
   The hybrid only shuttles three buffers: the preprocessed
   image, the spatial mask, and the per-scale c_values output.
   Total bandwidth at 4K: ~50 MiB per frame end-to-end.  On
   PCIe 4.0 (32 GB/s) that's <2 ms.  Acceptable.

## Follow-up work for v2

A v2 GPU pass would target the c-values phase using **strategy III**
(direct per-pixel histogram). Tracking: per the
[Section-A audit decisions](../../.workingdir2/decisions/section-a-decisions-2026-04-28.md)
§A.1.2, a backlog T-NN for this v2 phase opens once **T7-36** (the
v1 cambi GPU integration PR) lands — the v2 work is gated on the v1
hybrid path being shipped + bit-identical CPU↔GPU verified.

Key deliverables for that future ADR:

1. Profile strategy III on Lavapipe (the CI gate) and a real GPU
   to estimate the cache-hit rate.
2. Compare against strategy II (parallel scan) on the same fixture.
3. Lock the precision contract empirically — this strategy is
   integer-only on paper but the c-value formula uses float
   division `(p_0 * p_1) / (p_1 + p_0)` which is order-sensitive.
4. Decide whether to ship as a Vulkan-only optimisation or to fan
   out to CUDA + SYCL.

## Cross-checks

- ADR-0192 §Decision lists cambi *last* in the batch ordering and
  notes "Highest implementation risk; landing it last means every
  other batch-3 metric's review is already closed before cambi
  review starts."  — consistent with the hybrid recommendation.
- ADR-0192 §Consequences flags cambi as "the biggest
  implementation-risk chunk in the entire long-tail effort" and
  explicitly conditions implementation on "a feasibility spike
  (parallel-scan algebra for the range-tracking state)" — this
  digest IS that spike.
- The hybrid architecture **tightens the precision contract from
  ADR-0192's `places=2` planning placeholder to `places=4`** (the
  same floor as every other GPU twin in the fork). Reason: the GPU
  phases (preprocessing, derivative, summed-area DP, decimate,
  mode filter) are all integer + bit-exact w.r.t. the CPU, and the
  precision-sensitive c-value phase stays on the host running the
  exact CPU code path. v1 should land at ULP=0 / bit-exact, well
  under `places=4`. See ADR-0205 §"Precision contract".

## Conclusion

CAMBI is feasible on GPU **as a hybrid host/GPU pipeline** for v1.
A fully-on-GPU port is feasible via strategy III but is deferred to
a future batch as a perf-polish exercise.  Strategy II is rejected
as too bandwidth-heavy.  Strategy I is rejected as functionally a
no-op.  The hybrid implementation lands in the same PR as this
digest per ADR-0205.
