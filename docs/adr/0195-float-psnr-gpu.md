<!-- markdownlint-disable MD013 MD060 -->
# ADR-0195: float_psnr GPU kernels — single-dispatch diff² with float partials, bit-exact vs CPU

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, cuda, sycl, gpu, feature-extractor, fork-local, bit-exact

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) lists `float_psnr` as the
first **Group B** metric — float twin of an int kernel that already
ships on GPU (`psnr_y` on CUDA / SYCL / Vulkan emits the int-domain
luma PSNR). The CPU reference is
[`float_psnr.c`](../../core/src/feature/float_psnr.c) (170 LOC):

1. `picture_copy(offset=0)` normalises raw samples to float in
   `[0, peak]` (peak = 255.0 for bpc=8, etc.).
2. Per-pixel `(ref - dis)²` accumulated in `double` per row.
3. `noise = total / (w · h)`.
4. `score = MIN(10·log10(peak² / max(noise, 1e-10)), psnr_max)`.

Single emitted metric: `float_psnr`. The features.md table prior to
this PR claimed `float_psnr_y / _cb / _cr` plane outputs — **that
was wrong**; the CPU extractor only emits `float_psnr`. This PR
fixes the docs alongside the new GPU kernels.

## Decision

Ship `float_psnr_{vulkan,cuda,sycl}` as **single-dispatch kernels**
that compute per-pixel `(ref - dis)²` in float and emit per-WG float
partials. Host accumulates in `double` and applies the CPU formula.

### Single dispatch, no halo, no shared tile

The kernel doesn't need a halo or shared tile because every pixel
is independent:

- 16×16 WG.
- Per thread: read its ref + dis pixel, compute `(ref - dis)²`.
- Sub-group `reduce_over_group` → SLM cross-subgroup → single float
  per WG into the output partial buffer.

Smallest GPU kernel in the long-tail by a wide margin: ~120 LOC of
GLSL, ~110 LOC of CUDA PTX, ~150 LOC of SYCL.

### Float math, no bit-exactness *engineering*, but empirical 0.0 drift

Float per-pixel + float per-WG reduction + `log10` final transform
are normally a recipe for ULP drift vs the CPU's left-to-right
accumulator. Empirically the drift is **zero** on the cross-backend
gate fixture:

| Backend                                    | Bit depth | Frames | `float_psnr` max_abs_diff |
|--------------------------------------------|-----------|-------:|--------------------------:|
| Vulkan (Intel Arc A380 + Mesa anv)         |  8-bit    |     48 |             **0.00e+00** |
| Vulkan                                     | 10-bit    |      3 |             **0.00e+00** |
| CUDA (NVIDIA RTX 4090)                     |  8-bit    |     48 |             **0.00e+00** |
| CUDA                                       | 10-bit    |      3 |             **0.00e+00** |
| SYCL (Intel Arc A380 + oneAPI 2025.3)      |  8-bit    |     48 |             **0.00e+00** |
| SYCL                                       | 10-bit    |      3 |             **0.00e+00** |

The exactness is not engineered — it falls out of (a) the kernel's
trivial structure (one square per pixel, one summation), and (b) the
host-side `double` accumulation absorbing any per-WG ULP noise into
a value whose `log10` rounds to the same `double` as the CPU's. The
contract per ADR-0192 was nominally `places=3`; the empirical floor
is `places=∞` (exact JSON-level agreement).

This **isn't** the place to lock a "bit-exact forever" contract —
future driver / kernel changes could reintroduce ULP drift, and the
gate's `places=4` threshold is the safety margin for that. ADR-0192's
`places=3` precision contract still applies to the gate; reality
just exceeds it.

### Mirror padding: N/A

float_psnr has no convolution — every pixel is independent — so the
mirror-divergence-from-motion footgun called out in ADR-0193 / 0194
doesn't apply here. (Worth noting because the corresponding
*motion*-family kernels share the directory tree but the mirror
contract is per-kernel.)

### Float twins kept native, not aliased

ADR-0192 considered aliasing `float_psnr` to the existing `psnr_y`
kernel. Rejected: `psnr_y` reads quantized uint pixels and emits
the dB score; `float_psnr` reads pixels normalised by the bpc-
specific scaler (1 / 4 / 16 / 256). The kernels operate on
fundamentally different input domains. Aliasing would either
silently quantize the float input (wrong) or run two kernels
back-to-back (no win). Native kernel is cleaner.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Alias `float_psnr_*` to the existing `psnr_y` kernel + post-quantize on the host** | Saves 3 small kernel files | Fundamentally different input domains (raw uint vs `[0, peak]` float). Aliasing changes semantics. | Per ADR-0192 alternatives table |
| **Atomic float adds to a single global `noise` accumulator** | One global write, no per-WG slots | Cross-WG associativity drift (the very thing per-WG + double host accum sidesteps); float atomicAdd extension dependency on Vulkan | Per-WG floats + host double is the canonical pattern for this fork (ciede, ansnr, ...) |
| **Two passes (compute squared diffs in pass 1 → write to buffer; reduce in pass 2)** | Pass 2 can use parallel-tree reduction with deeper double accumulation | The kernel is trivially short already; pass 2 buys nothing | YAGNI; single-pass with sub-group + SLM reduce is sufficient |
| **Defer to "after `motion_v2` lands" instead of bundling into batch 3 part 3** | Smaller PR queue at any moment | Doesn't change the merge surface; the four float twins all share scaffolding | Match ADR-0192's per-metric ordering: smallest first (`float_psnr` < `float_motion` < `float_vif` < `float_adm`) |

## Consequences

- **Positive**: closes the first of four matrix gaps for Group B
  float twins. The next three (`float_motion`, `float_vif`,
  `float_adm`) follow the same shape — float per-pixel + per-WG
  partials + host `double` reduction.
- **Positive**: ~120 LOC GLSL + ~110 LOC PTX + ~150 LOC SYCL +
  ~250 LOC of host glue per backend. Smallest GPU twin batch by a
  wide margin.
- **Positive**: empirically bit-exact on all three backends —
  strongest possible numerical contract for a float-domain kernel.
- **Neutral / follow-ups**:
  1. CHANGELOG + features.md updates ship in the same PR per
     ADR-0100.
  2. features.md row 39 corrected: `float_psnr` emits one metric
     (`float_psnr`), not three plane outputs.
  3. Lavapipe lane gains a `float_psnr` step.
  4. Next batch 3 metric: `float_motion` (ADR-0196 to come).

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch 3 scope.
- Sibling: [ADR-0193](0193-motion-v2-vulkan.md), [ADR-0194](0194-float-ansnr-gpu.md).
- CPU reference:
  [`float_psnr.c`](../../core/src/feature/float_psnr.c) (170 LOC).
- Verification: cross-backend gate
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  with `--feature float_psnr --places 4`. New step in the lavapipe
  lane of
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
