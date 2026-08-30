# ADR-0201: ssimulacra2 Vulkan kernel

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: <lusoris@pm.me>
- **Tags**: vulkan, gpu, ssimulacra2, precision

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) scopes GPU long-tail batch 3,
which targets a Vulkan twin for every CPU extractor that still lacks
one. ssimulacra2 — the SSIMULACRA 2 perceptual quality metric ported
from libjxl ([ADR-0130](0130-ssimulacra2-feature-extractor.md)) — is
the second-most complex remaining metric after cambi (deferred for a
feasibility spike). Its CPU pipeline (full-resolution YUV → linear
RGB → 6-scale pyramid; per scale: linear-RGB → XYB, separable
3-pole IIR Gaussian blur of 5 statistics, per-pixel SSIM + edge-diff
stats, host accumulation of ~108 weighted norms + cubic polynomial +
power transform) makes a single fused GPU kernel impractical.

Closing the Vulkan slot for ssimulacra2 unblocks
[ADR-0192](0192-gpu-long-tail-batch-3.md)'s Group A coverage matrix
(no GPU twin yet) and provides the reference layout that the CUDA +
SYCL twins (a follow-up PR) will mirror.

## Decision

We land `ssimulacra2_vulkan` as a **hybrid host/GPU pipeline** with
the IIR blur + per-pixel multiplies on the GPU and the
precision-sensitive XYB pre-pass + per-pixel SSIM combine on the
host. The GPU shaders are:

1. `ssimulacra2_mul.comp` — elementwise 3-plane multiply (mirrors
   `multiply_3plane` in `ssimulacra2.c`) for the ref², dis², and
   ref·dis pre-blur products.
2. `ssimulacra2_blur.comp` — separable Charalampidis 2016 3-pole
   recursive IIR blur with sigma=1.5. The IIR is sequential along
   the scan axis, so we use **one workgroup per row** for the
   horizontal pass (`local_size = 1`, dispatch `(1, H, 1)`) and
   **one workgroup per column** for the vertical pass (dispatch
   `(1, W, 1)`). Per-channel offsets in the 3-plane buffer come
   from push constants (`in_offset`, `out_offset`) so the
   descriptor set is bound once per (in_buf, out_buf) pair —
   updating descriptors between recorded vkCmdDispatch calls only
   leaves the LAST-written binding visible at submit time, a
   pitfall we hit during development.

Host responsibilities (in `ssimulacra2_vulkan.c`):

- YUV → linear RGB at full resolution, using the same scalar libjxl
  port as `ssimulacra2.c::picture_to_linear_rgb` (deterministic
  sRGB EOTF LUT from [ADR-0164](0164-ssimulacra2-deterministic-eotf-cbrt.md)).
- 2×2 box downsample between scales (cheap vs the GPU work; keeps
  the GPU dispatch chain focused on the per-scale blur pipeline).
  The downsample uses the **full-resolution plane stride**
  consistently — every pyramid level keeps its 3 planes in their
  full-resolution slots with the active data at the head of the
  slot — so the GPU shaders' channel offsets
  (`c * full_w * full_h`) line up across scales.
- **linear-RGB → XYB** at every scale via `ss2v_host_linear_rgb_to_xyb`
  (verbatim port of `ssimulacra2.c::linear_rgb_to_xyb`). Bit-exact
  with the CPU extractor — see §Precision investigation for why
  this is required rather than running XYB on the GPU.
- **Per-pixel SSIMMap + EdgeDiffMap** combine in double precision
  over the GPU-blurred mu/sigma buffers (which are HOST_VISIBLE +
  MAPPED via VMA). Mirrors `ssim_map` + `edge_diff_map` exactly,
  including the `(double)num_m * (double)num_s / (double)denom_s`
  promotion at the divide site.
- 108-weighted-norm pool + cubic polynomial + power 0.6276 transform
  (mirrors `pool_score`).

The shader source files `ssimulacra2_xyb.comp` and
`ssimulacra2_ssim.comp` are kept in-tree as references for future
follow-up work (§Consequences). The pipelines they configure are
still allocated at init for forward-compatibility but never
dispatched in v1.

Min-dim guard: the host loop early-exits when the current scale
falls below 8×8 (matches the CPU `if (cw < 8u || ch < 8u) break`).
Init rejects inputs below 8×8.

Strict-mode SPIR-V compilation (`-O0`): all 4 ssimulacra2 shaders
build with `-O0` to disable the SPIR-V optimizer's FMA contraction.
The IIR blur in particular carries state across iterations — even
one compiler-introduced FMA per pixel would compound across the
blur radius and worsen the per-scale SSIM stats drift. The IIR
shader additionally carries `precise` qualifiers on every state
variable + per-pixel intermediate to block driver-side FMA fusion
that the SPIR-V `NoContraction` decoration alone did not catch on
lavapipe / Mesa anv / RADV (see §Precision investigation).

## Empirical precision

[ADR-0192](0192-gpu-long-tail-batch-3.md) sets `places=2` as the
nominal precision target for ssimulacra2 with the
"measure first; may surprise upward" qualifier inherited from
[ADR-0188](0188-gpu-long-tail-batch-2.md). Final achieved precision
on the Netflix normal pair (`src01_hrc00_576x324.yuv` ↔
`src01_hrc01_576x324.yuv`, 576×324, 48 frames):

- **Pooled `ssimulacra2` score** (full `--precision max` output):
  max abs diff = **1.81e-7**, mean = 3.65e-8, P95 = 1.56e-7.
  Cross-backend gate runs at `places=4` (5e-5 threshold;
  0/48 mismatches), matching the rest of the Vulkan VIF/MS-SSIM
  family. We exceed the parent's `places=2` target by ~5 decimal
  places.
- **Per-stage CPU↔GPU equivalence** (verified by per-pixel buffer
  dumps on frame 0):
  - XYB plane: bit-exact (host-side XYB).
  - IIR-blurred mu/sigma planes: bit-exact (proven by feeding the
    GPU's XYB output through CPU's `fast_gaussian_1d` and
    comparing to the GPU's blur output — 0 ULP diff across all
    576×324 pixels in plane Y).
  - Per-pixel SSIM + EdgeDiff `d` value: bit-exact (host-side
    combine in double precision).

## Precision investigation

The first iteration of `ssimulacra2_vulkan` shipped XYB and the
per-pixel SSIM combine on the GPU and produced a pooled-score
drift of **1.59e-2** (places=1 only). Driving the contract to
the planned `places=2` (≤5e-3) — and beyond, to `places=4` — took
five staged measurements:

| Tactic | Pooled max_abs | Notes |
| --- | ---: | --- |
| Baseline (in-shader XYB + in-shader SSIM, all `float`) | 1.59e-2 | `--places 1` only. |
| Add `precise` qualifier + explicit FMA-blocking temp staging on the XYB matmul + cube-root + SSIM `d` compute | 1.54e-2 | ~3% improvement; `precise` + `NoContraction` decorations confirmed in `spirv-dis` output, but lavapipe / Mesa anv / RADV still produced ~1.7e-6 max per-pixel drift on the X plane. |
| Move per-pixel SSIM combine to host (double precision over GPU-blurred buffers) | 1.54e-2 | No improvement: per-pixel `d` is dominated by upstream mu/sigma drift, not by the divide's float-vs-double precision. |
| Per-pixel buffer dump + decompose CPU/GPU difference: confirm IIR is bit-exact (CPU `fast_gaussian_1d` on GPU XYB == GPU IIR output to 0 ULP) | — | Diagnostic only: isolated XYB as the sole drift source. |
| **Move XYB to host** (bit-exact port of `linear_rgb_to_xyb`) | **1.81e-7** | `places=6` effective. Final fix. |

The driver-side compile chain (lavapipe, Mesa anv, RADV — all
tested) does not in practice preserve the exact float operation
order required for ULP-equivalent XYB even with `precise` on every
intermediate and `NoContraction` on every `OpFMul`/`OpFAdd`. The
worst-case 42-ULP X-plane drift comes from cancellation in
`0.5 * (cbrt(l) - cbrt(m))` when `l ≈ m`; even sub-ULP per-input
deviation in the matmul `kM00*r + m01*g + kM02*b + kOpsinBias`
amplifies through the cancellation site. Host-side XYB sidesteps
the entire compile-chain by running the canonical CPU port.

The IIR blur and SSIM combine both turned out to be bit-exact
when fed bit-exact inputs — so once XYB matches CPU exactly, the
rest of the pipeline matches by construction.

Wall-time impact (576×324, lavapipe): host XYB adds ~3 ms per
scale 0 frame (vs <1 ms for the GPU dispatch); host SSIM combine
adds ~1.5 ms per scale per channel. Total per-frame extract time
is dominated by the IIR (still GPU). Net wall-time impact under
2% on the Netflix normal pair.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Single fused kernel (XYB + blur + SSIM in one shader) | Fewer dispatches, lower CPU overhead | The IIR blur has carry-state along the scan axis incompatible with the SSIM stats' 2D parallel layout; would require expensive shared-memory thread-pinning per row/column | Correctness-first: separating the IIR into its own shader makes the per-stage data flow auditable against the CPU reference |
| Keep XYB on GPU, accept `places=1` contract | Pure GPU per-scale pipeline; minimal host work | Diverges from ADR-0192's nominal `places=2` and from the rest of the Vulkan VIF/MS-SSIM family (all at `places=4`) | Rejected by the user (verbatim, paraphrased to neutral English: "places=1 is not good"); the §Precision investigation table shows host-side XYB clears the entire family's `places=4` bar with no measurable wall-time cost |
| Host-side IIR blur (GPU does only XYB + SSIM) | Simpler host code, no IIR shader | Defeats the purpose — IIR is the dominant per-scale cost (~50% of CPU time) | Not chosen — would leave the hottest stage on CPU |
| GPU-side YUV → linear RGB (sRGB EOTF on GPU) | Pure GPU pipeline, no host pre-pass | Requires uploading the 1024-entry sRGB EOTF LUT and an ifelse-heavy YUV-matrix dispatcher into the shader | Not chosen for v1 — host YUV→RGB is fast (already SIMD'd via [ADR-0163](0163-ssimulacra2-picture-to-linear-rgb-simd.md)); follow-up if profiling shows it's a bottleneck |
| Pack 3 channels into a single `vec3` per dispatch | One dispatch processes all 3 channels at once | Doubles per-pixel register pressure; the IIR's 6 prev-state floats + 6 outputs × 3 channels = 36 live floats per lane, exceeding most GPUs' register budget | Not chosen — per-channel iteration is simpler and matches the CPU reference one-for-one |
| GPU-side XYB with `Float64` capability + `precise` everywhere | Could in principle keep XYB on GPU and still match CPU bit-for-bit | Requires `shaderFloat64` (not core on Vulkan 1.0; not supported on every consumer GPU); doubles the cube-root cost; the divide-amplification site (`0.5*(cbrt(l)-cbrt(m))`) still cancels in float at the consumer if the output buffer is float | Not chosen — host XYB is bit-exact by construction without any device-feature gating |

## Consequences

- **Positive**:
  - GPU long-tail batch 3 part 7 closes the Vulkan slot for `ssimulacra2`
    (Group A coverage per [ADR-0192](0192-gpu-long-tail-batch-3.md)).
  - Cross-backend precision lands at `places=4` (max abs 1.81e-7 on
    the Netflix normal pair), matching the rest of the Vulkan
    VIF/MS-SSIM family rather than holding `places=1` as the v1
    iteration first attempted.
  - The hybrid host/GPU layout becomes the reference template for
    the CUDA and SYCL twins (follow-up PR per
    [ADR-0192](0192-gpu-long-tail-batch-3.md) §scope).
- **Negative**:
  - The `ssimulacra2_xyb.comp` and `ssimulacra2_ssim.comp` shaders
    and their pipelines are kept in-tree but not dispatched in v1.
    A follow-up PR can either delete them entirely (simplest) or
    keep them behind a `Float64`-gated optional GPU-only mode.
  - Host-side XYB and SSIM combine adds ~5% CPU time per scale but
    moves it off-GPU; net wall-time impact is under 2% on
    lavapipe/ANV/RADV given the IIR remains on GPU.
  - One workgroup per row / column for the IIR blur is
    conservative — performance follow-ups can re-bin multiple
    rows/columns per WG once the empirical contract is in place.
- **Neutral / follow-ups**:
  - CUDA + SYCL twins land in a separate PR (this PR is Vulkan-only
    per the user's scope direction). Both should mirror the
    hybrid host/GPU split unless profiling shows the host XYB is
    a bottleneck on those backends.
  - GPU-side YUV → linear-RGB pre-pass and GPU-side downsample
    are deferrable optimisations — measure-first if profiling
    flags them.
  - The `psnr_hvs_strict_shaders` list in
    `core/src/vulkan/meson.build` grows by 4 entries; the list
    name is now misnamed but renaming it is out of scope for this
    PR (rename in a follow-up).

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — GPU long-tail
  batch 3 scope.
- CPU reference: [ADR-0130](0130-ssimulacra2-feature-extractor.md)
  (extractor) + [ADR-0161](0161-ssimulacra2-simd.md) (SIMD
  bit-exactness) + [ADR-0162](0162-ssimulacra2-blur-simd.md) (blur
  SIMD) + [ADR-0163](0163-ssimulacra2-picture-to-linear-rgb-simd.md)
  (YUV→RGB SIMD) + [ADR-0164](0164-ssimulacra2-deterministic-eotf-cbrt.md)
  (deterministic EOTF + cbrt LUT/Newton).
- Vulkan precedent: [ADR-0190](0190-float-ms-ssim-vulkan.md) —
  ms_ssim_vulkan, the closest precedent (5-level pyramid + per-scale
  SSIM stats with per-WG partials).
- Min-dim guard precedent: [ADR-0153](0153-ms-ssim-min-dim-guard.md).
- Source: `req` (user prompt for batch-3 part 7,
  `feat/ssimulacra2-vulkan` PR).
