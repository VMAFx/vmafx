# ADR-0190: float_ms_ssim Vulkan kernel — 5-level pyramid + Wang product on host

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0188](0188-gpu-long-tail-batch-2.md) scopes batch 2 as
`ssim` → `ms_ssim` → `psnr_hvs`. ssim (Vulkan + CUDA + SYCL)
landed in PR #139 + PR #140 with empirical `places=4`,
`max_abs = 1.0e-6`. ms_ssim picks up where ssim left off.

The active CPU `float_ms_ssim` extractor
([`float_ms_ssim.c`](../../core/src/feature/float_ms_ssim.c))
runs the Wang multi-scale variant:

1. `picture_copy` → float ref/cmp at `[0, 255]` per ssim's
   contract.
2. **Build a 5-level pyramid** with 2× decimation between
   levels via a 9-tap 9/7 biorthogonal separable low-pass
   filter
   (`ms_ssim_decimate_scalar` /
   `ms_ssim_decimate_avx2` /
   `ms_ssim_decimate_avx512` /
   `ms_ssim_decimate_neon`). Coefficients are bit-exact across
   the SIMD paths — the rebase-sensitive invariant in
   [`ms_ssim_decimate.c`](../../core/src/feature/ms_ssim_decimate.c)
   pins them to `g_lpf_h` / `g_lpf_v` in
   [`ms_ssim.c`](../../core/src/feature/ms_ssim.c).
3. **Per scale (5×)**: run SSIM with the 11-tap Gaussian
   window, but emit three separate aggregates — `l_mean`,
   `c_mean`, `s_mean` — instead of just the combined SSIM mean.
4. **Wang combine** (host-side):
   `MS-SSIM = ∏_{i=0..4} l[i]^α[i] · c[i]^β[i] · s[i]^γ[i]`
   with the upstream-Netflix coefficients:
   - `α = {0.0000, 0.0000, 0.0000, 0.0000, 0.1333}`
   - `β = {0.0448, 0.2856, 0.3001, 0.2363, 0.1333}`
   - `γ = {0.0448, 0.2856, 0.3001, 0.2363, 0.1333}`
   Note: α[0..3] = 0, so `l[i]` contributes only at scale 4.

Each scale's `l, c, s` formulas (from
[`ssim_tools.c`](../../core/src/feature/iqa/ssim_tools.c)
`ssim_accumulate_default_scalar`):

```text
l = (2·μxμy + C1) / (μx² + μy² + C1)
c = (2·sqrt(σx²·σy²) + C2) / (σx² + σy² + C2)
s = (clamped(σxy) + C3) / (sqrt(σx²·σy²) + C3)
```

with `C1 = (0.01·255)²`, `C2 = (0.03·255)²`, `C3 = C2/2`. The
`clamped(σxy)` step handles the `ref == cmp` flat-region edge
case where `σxy` can drift slightly negative via float rounding
while `sqrt(σx²·σy²)` is zero.

## Decision

Ship `float_ms_ssim_vulkan` as **two new GLSL shaders + a
~700-LOC host orchestrator**:

### Shaders

1. **`ms_ssim_decimate.comp`** — 9-tap 9/7 biorthogonal LPF +
   2× downsample. Single 2D dispatch reading a 9×9 source
   neighbourhood per output pixel. Mirror boundary handling
   matches `ms_ssim_decimate_scalar`'s `ms_ssim_decimate_mirror`
   helper (period-2n reflection that handles sub-kernel-radius
   inputs the iqa single-reflect form leaves out of bounds —
   per ADR-0125).

   Output dimensions: `w_out = (w/2) + (w&1)`,
   `h_out = (h/2) + (h&1)`. Bit-exact constants:
   `{0.026727, -0.016828, -0.078201, 0.266846, 0.602914,
     0.266846, -0.078201, -0.016828, 0.026727}`.

2. **`ms_ssim.comp`** — same shape as `ssim.comp` (PR #139),
   but the vertical pass emits **three** per-WG partials
   instead of one:
   - `l_partials[wg_idx]` — sum of per-pixel `l`
   - `c_partials[wg_idx]` — sum of per-pixel `c`
   - `s_partials[wg_idx]` — sum of per-pixel `s`

   Horizontal pass is a verbatim copy of `ssim.comp`'s
   pass 0; the only divergence is the vertical-pass
   per-pixel formula and the 3-output reduction. Per-pixel
   math matches `ssim_accumulate_default_scalar` line-for-line
   including the σxy clamp.

### Host orchestrator: `ms_ssim_vulkan.c`

```text
init:
    picture_copy()-style float buffer for ref + cmp at full res
    allocate pyramid: 5 ref + 5 cmp float buffers, sized by halving
    allocate intermediate buffers for the SSIM compute (5 × float
        × max_pyramid_W × max_pyramid_H)
    allocate 3 × partials buffers for max_wg_count

extract per frame:
    upload ref + cmp at level 0
    for scale 0..4:
        if scale > 0:
            run decimate (level scale-1 → scale)
        run ssim horizontal (level `scale`)
        run ssim vertical + l/c/s combine (level `scale`)
        readback l/c/s partials, sum on host in `double`
        l[scale] = total_l / pixel_count
        c[scale] = total_c / pixel_count
        s[scale] = total_s / pixel_count
    msssim = product over scales of pow(l, α) · pow(c, β) · pow(s, γ)
    emit float_ms_ssim
```

### Precision contract

Target `places=4` on the `576×324` cross-backend gate fixture.
The 5-level pyramid only goes 4 deep at this resolution
(`min_dim = 11 << 4 = 176`; 576 / 16 = 36 ≥ 11), so the input
satisfies the [ADR-0153](0153-float-ms-ssim-min-dim-netflix-1414.md)
floor.

Each scale's `l, c, s` reduction follows the same per-WG-float

- host-double pattern as ssim_vulkan. The Wang product combine
runs entirely on the host in `double`, so the per-pixel
`pow(x, α)` calls don't accumulate float ULPs across scales.

If empirical exceeds `places=4`, relax to `places=3` per the
ciede / ssim precedent.

### v1 `enable_lcs` deferred

The CPU's `enable_lcs` option emits 15 additional metrics
(`float_ms_ssim_l_scale0..4`, `_c_scale0..4`, `_s_scale0..4`).
v1 does not implement this — the GPU pipeline still computes
the underlying l/c/s values per scale, so adding a flag-toggled
`vmaf_feature_collector_append` per metric is mechanical. Defer
to a focused follow-up.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Reuse ssim_vulkan's `ssim.comp` and add a 3-output combine pass that reads the same intermediates** | Maximum code reuse | Requires a 4-pipeline / 4-pass design (horiz + ssim_combine + l_combine + c_combine + s_combine) — more memory bandwidth, more pipelines | A single 3-output combine pass is cleaner and the duplicated horizontal pass is ~50 LOC of GLSL |
| **Compute MS-SSIM on the host by calling SSIM-on-GPU 5×** | Zero new GPU code beyond ssim_vulkan | Per-scale needs l/c/s separately, not just combined SSIM. Would require either modifying ssim_vulkan's kernel (breaking its `places=4` ssim_vulkan contract) or computing l/c/s analytically from SSIM (impossible — irreducible decomposition) | Not viable |
| **Single 1-dispatch fused kernel** (decimate + horiz + vert + combine) | Lowest dispatch count | One mega-kernel with bookkeeping for 5 scales; tile-based shared memory across scales is impractical (per-scale workgroup sizes differ) | 2 shaders × 5 scales = 11 dispatches per frame is fine at 576×324 / 1080p (compute-bound); fusion is profile-led |
| **Defer to ms_ssim CUDA + SYCL bundle, skip Vulkan first** | Smaller scope | Breaks the [ADR-0188](0188-gpu-long-tail-batch-2.md) ordering precedent | Vulkan first scaffolds the CUDA + SYCL ports; same as batch 1 + ssim |

## Consequences

- **Positive**: matches CPU `float_ms_ssim` at `places=4` on
  the gate fixture (subject to empirical measurement). Reuses
  the per-WG-float + host-double precision pattern from ssim.
  No new public C-API surface — the extractor registers as
  `float_ms_ssim_vulkan` under `HAVE_VULKAN`.
- **Negative**: 11 dispatches per frame (5 decimates + 5 horiz
  5 vert+combine, rounded down because scale 0 skips
  decimate). Higher submission overhead than ssim's 2
  dispatches; not noticeable at 1080p but worth a v2 fusion
  pass if a profiling pass shows submission overhead > 10% of
  per-frame time.
- **Neutral / follow-ups**:
  1. **Batch 2 part 2b — DONE.** `float_ms_ssim_cuda` shipped
     in the batch 2 parts 2b + 2c bundle (sibling PR). Three
     CUDA kernels (decimate, horiz, vert_lcs) mirror the GLSL
     shaders byte-for-byte modulo language differences.
     picture_copy normalisation runs host-side via a 2D D2H
     of the pitched device plane (`cuMemcpy2DAsync` honouring
     `srcPitch = stride[0]`) — surfaced a bring-up bug where
     the naïve `cuMemcpyDtoHAsync` of `width·height·bpc` bytes
     mis-copied row N≥1 because `cuMemAllocPitch` returns
     `stride[0] ≥ width·bpc`. Per-block float partials reduced
     on host in `double`. Empirical: 48 frames at 576×324 on
     RTX 4090 → `max_abs = 1.0e-6`, `0/48 places=4 mismatches`.
  2. **Batch 2 part 2c — DONE.** `float_ms_ssim_sycl` shipped
     alongside part 2b. Self-contained submit/collect (mirrors
     `ssim_sycl`). Host-pinned USM staging holds the
     picture_copy-normalised float planes; `nd_range<2>`
     vert+lcs kernel uses `sycl::reduce_over_group` × 3 for
     per-WG float partials. fp64-free (Arc A380). Empirical:
     48 frames at 576×324 on Arc A380 → `max_abs = 1.0e-6`,
     `0/48 places=4 mismatches`.
  3. **`enable_lcs` mode** (deferred) — 15 extra metrics.
     Trivial follow-up once the gate is green.

## Verification

- 48 frames at 576×324 on Intel Arc A380 + Mesa anv vs CPU
  scalar: target `max_abs ≤ 5e-5` (places=4), measured per
  `scripts/ci/cross_backend_vif_diff.py --feature
  float_ms_ssim --backend vulkan --places 4`.
- New CI step `float_ms_ssim cross-backend diff (CPU vs
  Vulkan/lavapipe)` in
  `.github/workflows/tests-and-quality-gates.yml`.
- If empirical floor exceeds `places=4`, relax to `places=3`
  with a note here (mirrors ciede / ssim precedents).

## References

- Parent: [ADR-0188](0188-gpu-long-tail-batch-2.md) — batch 2
  scope.
- Sibling: [ADR-0189](0189-ssim-vulkan.md) — ssim Vulkan.
- Pyramid math: [ADR-0125](0125-ms-ssim-decimate-simd.md) —
  9/7 biorthogonal LPF coefficients + mirror semantics.
- Min-dim guard:
  [ADR-0153](0153-float-ms-ssim-min-dim-netflix-1414.md) —
  176×176 minimum input for the 5-level pyramid + 11-tap
  Gaussian.
- CPU references:
  [`float_ms_ssim.c`](../../core/src/feature/float_ms_ssim.c),
  [`ms_ssim.c`](../../core/src/feature/ms_ssim.c),
  [`ms_ssim_decimate.c`](../../core/src/feature/ms_ssim_decimate.c),
  [`iqa/ssim_tools.c`](../../core/src/feature/iqa/ssim_tools.c).
