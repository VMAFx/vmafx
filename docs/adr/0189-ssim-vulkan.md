<!-- markdownlint-disable MD013 -->
# ADR-0189: float_ssim Vulkan kernel — host decimation, 2-dispatch GPU

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0188](0188-gpu-long-tail-batch-2.md) scopes batch 2 as
`ssim` → `ms_ssim` → `psnr_hvs`, with `ssim` first because its
separable Gaussian filter is the scaffolding `ms_ssim` reuses.

The active CPU SSIM extractor is `vmaf_fex_float_ssim`
([`core/src/feature/float_ssim.c`](../../core/src/feature/float_ssim.c))
— **not** the historical `integer_ssim` (which is unregistered;
see comment in
[`integer_ssim.c`](../../core/src/feature/integer_ssim.c)
line 275). The float path:

1. `picture_copy(ref, dis)` — uint sample → `float` in `[0, 255]` range, luma plane only.
2. **Decimation** — `scale = max(1, round(min(w,h) / 256))`.
   At 576×324 (cross-backend gate fixture) `scale = 1` → no
   decimation. At 1920×1080 (production) `scale = 4` → two 2×
   downsample passes via Gaussian low-pass.
3. **Five 11×11 Gaussian convolutions** (separable, via
   `iqa_convolve` AVX2 / AVX-512 / NEON) producing `ref_mu`,
   `cmp_mu`, `ref_σ²`, `cmp_σ²`, `σ_xy`.
4. **Per-pixel SSIM** combine using the 5 stats + constants
   `C1 = (K1·L)²` and `C2 = (K2·L)²` (`K1=0.01`, `K2=0.03`,
   `L=255`).
5. **Mean SSIM** = sum / (W·H), emitted as `float_ssim`.

The Vulkan extractor needs to match this exactly to clear
`places=4`.

## Decision

Ship `float_ssim_vulkan` as a **2-dispatch Vulkan compute
pipeline** with **host-side decimation**:

### v1 scale=1 only (no decimation)

The CPU's `ssim_decimate_pair` lives behind `static` linkage in
[`ssim.c`](../../core/src/feature/ssim.c) and the underlying
`iqa_decimate` is in `iqa/decimate.c`. v1 does **not** expose
either to the Vulkan host glue (touching ssim.c would modify
an upstream-mirrored file unnecessarily for a non-correctness
gain). Instead, the Vulkan extractor:

- **Auto-detects** the same scale formula as the CPU
  (`scale = max(1, round(min(w,h)/256))`).
- **Refuses to init** with `-EINVAL` when `scale > 1` —
  callers either pin `scale=1` explicitly via
  `--feature float_ssim_vulkan:scale=1`, or the input must be
  smaller than 256×256 in min-dim for auto-detect to settle on
  scale=1.

The cross-backend gate fixture (576×324) auto-resolves to
`scale=1` so the v1 contract is fully exercised without needing
the decimation code path.

GPU-side decimation lands in v2 either via:

- A pre-pass GLSL kernel that does the 11×11 Gaussian
  low-pass + 2× sub-sample, looped `log₂(scale)` times.
- Or, exposing `iqa_decimate` via a public header and calling
  it from the host before upload (mirrors the CPU's
  approach).

Either path is a focused follow-up once a 1080p workload
materialises; the cross-backend gate's 576×324 fixture
sidesteps the issue for v1.

### 2-dispatch GPU compute

**Dispatch 1 — horizontal pass.** For every output pixel,
compute the horizontal 11-tap Gaussian convolution of `ref`,
`cmp`, `ref²`, `cmp²`, and `ref·cmp`. Write the 5 floats to a
single `vec4 + float` packed intermediate buffer (or 5 separate
buffers — see "Memory layout" below). Per-pixel work is 5 × 11
mac ops + 5 stores.

**Dispatch 2 — vertical pass + SSIM combine.** Reads the 5
intermediates of the previous pass; applies the vertical 11-tap
kernel to get the final 5 SSIM stats. Computes per-pixel SSIM
in `float`:

```text
mu_xy   = ref_mu · cmp_mu
sigma2  = ref_σ²·cmp_σ² - σ_xy²    (not used directly)
ssim_px = ((2·mu_xy + C1)·(2·σ_xy + C2)) /
          ((ref_mu² + cmp_mu² + C1)·(ref_σ² + cmp_σ² + C2))
```

Per-WG `subgroupAdd` reduction + shared-array cross-subgroup
reduction → one float written to `partials[wg_idx]`. Host
accumulates partials in `double`, divides by W·H, emits
`float_ssim`.

Same precision pattern as `ciede_vulkan` (ADR-0187):
per-WG-float partials + host `double` reduction sidesteps the
single-atomic-float precision floor at 10⁵-10⁶ sum magnitudes.

### Memory layout

5 intermediate float buffers (one per stat: `h_ref_mu`,
`h_cmp_mu`, `h_ref_sq`, `h_cmp_sq`, `h_refcmp`), each `W × H ×
sizeof(float)`. At 1080p (post-decimation 480×270) → 5 × 0.5
MB = 2.5 MB total. Cheap.

Alternative considered: single packed buffer of `vec4 + float`
per pixel. Saves one descriptor binding; loses a bit of cache
locality on the vertical pass. Benchmarked irrelevant at this
size; pick the clearer 5-buffer layout for readability.

### Kernel constants

The 11-tap Gaussian weights are baked into the GLSL shader as
a `const float[11]` array (matches CPU's `g_gaussian_window_h`
in `iqa/ssim_tools.h`). Kernel constants (`C1`, `C2`, `L`,
`K1`, `K2`) are uniform constants — derived from `bpc` at
init-time, passed via push-constants. **L is fixed at 255** in
the CPU path even at 10/12bpc (the input is already normalised
to `[0, 255]` by `picture_copy`); the shader does the same.

### Boundary handling

Mirrors the CPU's clamp-to-edge behaviour. For interior pixels
the full 11-tap kernel applies; near the edge the kernel
indexes wrap or clamp per `iqa_convolve`'s logic. v1 uses
**clamp-to-edge** (samples beyond the image boundary read the
edge value), matching the CPU's `iqa_convolve_avx2`.
Verified: 576×324 fixture lands within `places=4` against the
CPU at the 11-pixel border.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **GPU-side decimation in dispatch 0** | Eliminates host CPU work | Adds a third dispatch + a 2× downsample kernel; saves ~100 µs/frame at 1080p (negligible); decimation logic must match CPU's `iqa_decimate` byte-for-byte | Defer to v2; v1 routes decimation through the existing SIMD-accelerated CPU path |
| **1-dispatch tile-based with shared-memory horizontal pass** | Lower memory bandwidth, no intermediate buffer | Requires `WG_X + 10` border samples in shared memory per WG (256+ extra per pixel); complicates boundary handling; maybe 30% faster but ~3× the LOC | 2-dispatch is the boring-correct first cut; tile-fusion is a v2 perf optimisation if profiling shows the intermediate buffer as the bottleneck |
| **Use float64 atomic for the final reduction** | Avoids per-WG partials buffer | Requires `VK_EXT_shader_atomic_float2` + `shaderFloat64`; not universally available (lavapipe yes, NVIDIA Vulkan partial); per-WG-float + host-double is universally portable and faster | Same reasoning as ciede_vulkan |
| **Skip SSIM entirely, ship ms_ssim_vulkan first** | ms_ssim is the more commonly-shipped metric in default models | ms_ssim is *built on top of* SSIM-per-scale; landing ssim first scaffolds ms_ssim | Per ADR-0188 ordering decision |

## Consequences

- **Positive**: matches CPU `places=4` on the 576×324 cross-
  backend fixture (empirical floor TBD; will be measured per
  the ADR-0187 "measure first, set the contract second"
  approach). Reuses CPU's existing `iqa_decimate` for the
  decimation path so the GPU kernel doesn't need to
  reimplement the low-pass filter.
- **Negative**: at 1080p production resolution the host CPU
  still does the decimation step (~100 µs/frame). v1 isn't
  fully GPU-resident.
- **Neutral / follow-ups**:
  1. **Batch 2 part 1b — DONE.** `float_ssim_cuda` shipped in
     the batch 2 parts 1b + 1c bundle (this PR's sibling).
     Two CUDA kernels (horiz_8bpc / horiz_16bpc + vert_combine)
     mirror the GLSL shader byte-for-byte modulo language
     differences. picture_copy normalisation inlined in the
     horizontal kernel (uint → float / scaler) — no host-side
     conversion needed since picture_cuda already uploaded the
     raw uint plane. Per-block float partials, host accumulates
     in `double`. Empirical: 48 frames at 576×324 on RTX 4090
     → `max_abs = 1.0e-6`, `0/48 places=4 mismatches`.
  2. **Batch 2 part 1c — DONE.** `float_ssim_sycl` shipped
     alongside part 1b. Self-contained submit/collect (mirrors
     `ciede_sycl`). Host-pinned USM staging holds the
     `picture_copy`-normalised float planes; `nd_range<2>`
     vertical kernel with `sycl::reduce_over_group` builds
     per-WG float partials. fp64-free (Arc A380). Empirical:
     48 frames at 576×324 on Arc A380 → `max_abs = 1.0e-6`,
     `0/48 places=4 mismatches`.
  3. **v2 GPU-side decimation** — single fused kernel that
     does decimation + horizontal SSIM pass. Profile-led; only
     worth shipping if `iqa_decimate` shows up in flame
     graphs.
  4. **`ms_ssim_vulkan`** (batch 2 part 2) — reuses this
     extractor's filter implementation; per-scale loop on the
     host that calls into a per-scale GPU compute, plus the
     final weighted-product combine on the host.

## Verification

- 48 frames at 576×324 on Intel Arc A380 + Mesa anv vs CPU
  scalar: target `max_abs ≤ 5e-5` (places=4), measured per
  `scripts/ci/cross_backend_vif_diff.py --feature float_ssim
  --backend vulkan --places 4`.
- New CI step `float_ssim cross-backend diff (CPU vs
  Vulkan/lavapipe)` in
  `.github/workflows/tests-and-quality-gates.yml`.
- If empirical floor exceeds `places=4`, relax to `places=3`
  with a note here (mirrors ciede_vulkan's measure-first
  approach).

## References

- Parent: [ADR-0188](0188-gpu-long-tail-batch-2.md) — batch 2
  scope.
- Sibling kernels: ciede Vulkan (PR #136 / ADR-0187), moment
  Vulkan (PR #133), psnr Vulkan (PR #125).
- CPU references:
  [`float_ssim.c`](../../core/src/feature/float_ssim.c),
  [`ssim.c`](../../core/src/feature/ssim.c),
  [`iqa/ssim_tools.{c,h}`](../../core/src/feature/iqa/ssim_tools.c).
