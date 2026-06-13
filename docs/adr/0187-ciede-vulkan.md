# ADR-0187: ciede2000 Vulkan kernel — float-precision per-pixel ΔE

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, gpu, feature-extractor, fork-local, places-2

## Context

[ADR-0182](0182-gpu-long-tail-batch-1.md) scoped a three-batch
GPU long-tail rollout (psnr / moment / ciede across CUDA, SYCL,
Vulkan). Batches 1a (psnr Vulkan, PR #125), 1b (psnr CUDA + SYCL,
PR #129–#130), and 1d (moment ×3, PR #133 + PR #135) shipped
with **bit-exact** cross-backend gates at `places=4`. All four
extractors operate on integer YUV and accumulate `int64` sums —
no transcendentals, deterministic reductions.

ciede2000 (batch 1c) is structurally different. Per pixel, the
CPU reference (`core/src/feature/ciede.c`) does:

1. YUV → BT.709 RGB (3 multiplies + adds).
2. RGB → linear via the sRGB transfer (`pow(x, 2.4)` piecewise).
3. Linear RGB → CIE XYZ (3×3 matrix).
4. XYZ → L\*a\*b\* (`pow(t, 1/3)` for the inner nonlinearity).
5. CIEDE2000 ΔE (a tangle of `pow(c, 7)`, `sqrt`, `sin`,
   `atan2` operations with hue branches).
6. Sum-of-ΔE across pixels, divide by W·H, then
   `score = 45 - 20·log10(mean_ΔE)`.

Each pixel uses ~40 transcendental float ops. GPU
transcendentals are accurate to ~2 ULP per op; libm runs at full
double precision in the CPU path's intermediate computations.
Bit-exactness is therefore not on the table.

## Decision

Ship `ciede_vulkan` as the **first non-bit-exact** GPU extractor
in the fork. Single dispatch per frame; per-WG `float` partial
sums reduced into a `partials` buffer; host accumulates partials
in `double`, divides by W·H, and applies the CPU's logarithmic
transform `45 - 20·log10(mean_ΔE)` for the final `ciede2000`
metric. Cross-backend gate target: **`places=4`**.

The `places=4` target was set after empirical measurement, not
guessed:

- Initial budget per the popup framing: `places=2`
  (≤ 0.005 abs).
- Empirical: Intel Arc A380 + Mesa anv → `max_abs = 1.0e-5`
  across 48 frames at 576×324.
- That sits comfortably under the `places=4` threshold
  (≤ 5e-5), so the gate is set to `places=4` for parity with
  the other Vulkan kernels and to catch any *future*
  regression promptly.

If the lavapipe lane ever produces > 5e-5 (e.g. from a Mesa
update that changes `pow` accuracy), the gate will fail loudly
and we relax to `places=3` with a note here. The empirical
floor on a real driver is the contract; the script's `--places`
arg can be overridden per-lane.

### Chroma handling

ciede needs full-resolution YUV. `picture_vulkan`'s existing
`VmafVulkanBuffer` API is plane-agnostic, so no public-API
change is needed:

- 6 storage-buffer bindings: ref Y/U/V + dis Y/U/V, each at
  full luma resolution.
- Host upscales chroma planes to luma resolution before upload
  (mirrors `ciede.c::scale_chroma_planes` — nearest-neighbour
  for both `YUV420P` and `YUV422P`; `YUV444P` is a straight
  copy). 8-bit and 10/12/16-bit paths handled separately.
- Shader: same packed-uint32 layout as `psnr.comp` /
  `moment.comp` (4 pixels per uint32 at 8bpc, 2 uint16 per
  uint32 at HBD). Six independent accessor functions because
  GLSL pre-`VK_EXT_buffer_device_address` doesn't allow
  passing buffer blocks as parameters.

### Logarithmic score transform

The CPU emits `score = 45 - 20*log10(de00_sum / (W*H))`, not
the raw mean ΔE. Easy to miss; the GPU host's first cut omitted
the transform and the gate reported `max_abs = 33` (CPU score
~35, GPU raw mean ΔE ~3). Fix is one line on the host side.
Documented here so the CUDA + SYCL twins don't repeat the
mistake.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Float64 atomics in the kernel** (`VK_EXT_shader_atomic_float2`) | Single dispatch, no host reduction | Extension not universally available (lavapipe yes, NVIDIA Vulkan partial); needs feature gating | Per-WG partial sums + host reduction is portable and the host work is negligible (~8000 floats at 1080p) |
| **Per-pixel atomic float to a single counter** (`VK_EXT_shader_atomic_float`) | Same logic as the CUDA `atomicAdd(float*)` path | Massive contention on a single atomic — measured 10× slowdown vs. the partials approach in early prototyping; precision worse because every atomicAdd loses ULPs | Per-WG partials win on both axes |
| **Tighter per-pixel `double` math** in the shader | Higher accuracy | Vulkan compute requires `shaderFloat64` feature + explicit `double`/`f64vec3`; halves the ALU throughput on most consumer GPUs | Empirical Arc A380 result already lands at `places=4` — no upside |
| **Defer to ciede CUDA / SYCL parts 2+3** (skip Vulkan first) | Faster initial bring-up on a single backend | Loses the "Vulkan as reference" precedent established by moment / psnr; CUDA / SYCL ports without a working Vulkan template are harder to validate | Same reason batch 1d started with Vulkan |

## Consequences

- **Positive**: closes the last per-metric GPU long-tail row
  (ciede) on the Vulkan side. Empirical `max_abs = 1e-5` on
  Intel Arc A380 — places=4 contract holds. Host upscale path
  matches the CPU's `scale_chroma_planes` exactly.
- **Negative**: this is the first non-bit-exact GPU extractor;
  contributors looking at moment / psnr won't find an `int64`
  pattern to copy. The ADR explicitly flags this and points
  readers at the per-WG-float-partial pattern in
  `ciede_vulkan.c`. Six storage-buffer bindings is more than
  the existing extractors (max was 3); no architectural
  consequence — Vulkan supports up to 8 in a single descriptor
  set on every relevant device.
- **Neutral / follow-ups**:
  1. **Batch 1c part 2 — DONE.** `ciede_cuda` shipped in the
     batch 1c parts 2 + 3 bundle (this PR's sibling). Per-block
     float partials, host accumulates in `double`. The simpler
     atomic-float reduction empirically lost ~30% precision at
     1080p (10⁷ sum magnitude vs float7 floor). Surfaced a
     latent `vmaf_cuda_picture_upload_async` bug: bitmask was
     hardcoded to `0x1` (luma only) — fixed to `0x7` for any
     pix_fmt other than YUV400P, unblocking chroma-aware CUDA
     extractors.
  2. **Batch 1c part 3 — DONE.** `ciede_sycl` shipped alongside
     part 2. Self-contained submit/collect (NOT graph_register
     — `shared_frame` is luma-only). `nd_range<2>` kernel with
     `sycl::reduce_over_group` builds per-WG float partials;
     host accumulates in `double`. fp64-free because Intel Arc
     A380 lacks native fp64 (initial `sycl::reduction<double>`
     attempt threw at runtime).
  3. **Closes ADR-0182** — every metric in the GPU long-tail
     scope now has at least one GPU twin; ciede has all three
     (Vulkan + CUDA + SYCL). The matrix in
     `.workingdir2/analysis/metrics-backends-matrix.md` reflects
     this.

## Verification

- 48 frames at 576×324 on Intel Arc A380 vs CPU scalar:
  `max_abs = 1.0e-5`, `0/48 places=4 mismatches` via
  `scripts/ci/cross_backend_vif_diff.py --feature ciede
  --backend vulkan --places 4`.
- New CI step `ciede cross-backend diff (CPU vs Vulkan/lavapipe)`
  in `.github/workflows/tests-and-quality-gates.yml` runs the
  same gate on the lavapipe lane.

## References

- Parent: [ADR-0182](0182-gpu-long-tail-batch-1.md) — GPU
  long-tail batch scope.
- Sibling kernels: psnr Vulkan (PR #125), moment Vulkan
  (PR #133), moment CUDA + SYCL (PR #135).
- CPU reference: [`core/src/feature/ciede.c`](../../core/src/feature/ciede.c).
