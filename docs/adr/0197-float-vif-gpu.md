<!-- markdownlint-disable MD060 -->
# ADR-0197: float_vif GPU kernels — 4-scale pyramid with mirror-asymmetry fix

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, cuda, sycl, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) lists `float_vif` as the third
**Group B** float twin. CPU reference:
[`float_vif.c`](../../core/src/feature/float_vif.c) +
[`vif.c::compute_vif`](../../core/src/feature/vif.c) +
[`vif_tools.c`](../../core/src/feature/vif_tools.c).

The algorithm: 4-scale Gaussian pyramid with separable filters (widths
`{17, 9, 5, 3}` for the production default `vif_kernelscale = 1.0`), 2x
decimation between scales (`vif_dec2_s`), per-pixel
`vif_stat_one_pixel` computing `(num, den)` from
`(mu1, mu2, ref²_filt, dis²_filt, ref·dis_filt)`. Final score is
`num / den` per scale.

`vif_options.h` defines `VIF_OPT_HANDLE_BORDERS`, so per-scale dims =
`prev / 2` (no border crop). Decimation samples mu1 at `(2i, 2j)` —
mirror padding handles taps near the edge.

## Decision

Ship `float_vif_{vulkan,cuda,sycl}` as **single-pass-per-stage**
kernels driven by 7 dispatches per frame: 4 compute (one per scale) +
3 decimate (scales 1, 2, 3). v1 restricts `vif_kernelscale` to the
production default of 1.0 (the CPU supports 11 alternative scales but
they require a much larger filter table; rejected at init for the GPU
extractor).

### The mirror asymmetry — both H mirrors exist in CPU

CPU `vif_tools.c::vif_mirror_tap_h` returns `2 * extent - idx - 1`
for the right border. `convolution_internal.h::convolution_edge_s`
(horizontal branch) returns `2 * width - idx - 2` for the same
condition. **They differ by one.**

The mirrors collide in the CPU's hot path:

- `vif_filter1d_s` / `vif_filter1d_sq_s` / `vif_filter1d_xy_s` (the
  scalar fallback) call `vif_hpass_row_s` for **every** column,
  border or not — using `vif_mirror_tap_h` (-1 form).
- The AVX2 fast path (`convolution_f32_avx_s`) calls
  `convolution_edge_s` (-2 form) for the small border region and a
  vectorized scanline for the interior.

For `vif_sigma_nsq == 2.0` (the default) the AVX2 fast path is taken,
so the border H mirror that runs in production is the `-2` form. The
GPU's first attempt followed `vif_mirror_tap_h` (the scalar shape) —
which produced an empirical `5.46e-4` relative drift at scale 1
versus AVX2 production. Switching the GPU's H mirror to the `-2`
formula brought the drift to `1.40e-5` at scale 1, well under the
`places=4` threshold (`5e-5`).

### Mirror padding on GPU (final)

| axis | formula |
|---|---|
| vertical (V), idx >= sup | `2 * sup - idx - 2` (matches both CPU paths) |
| horizontal (H), idx >= sup | **`2 * sup - idx - 2` (matches AVX2 production path, NOT scalar)** |

A short comment block in each backend's mirror routine cites
ADR-0197 so future maintainers don't "fix" the asymmetry by re-
matching scalar's mirror.

### Per-frame dispatch flow

```text
scale 0 compute: read raw ref/dis            → (num0, den0)
scale 1 decimate: read raw  → buf_A           (filter @ HFW=4, sample @ 2x)
scale 1 compute:  read buf_A                  → (num1, den1)
scale 2 decimate: read buf_A → buf_B
scale 2 compute:  read buf_B                  → (num2, den2)
scale 3 decimate: read buf_B → buf_A
scale 3 compute:  read buf_A                  → (num3, den3)
```

Float ping-pong `(ref_buf[0], ref_buf[1])` + `(dis_buf[0], dis_buf[1])`.
Per-scale `(num, den)` partials at `wg_count[scale]` × 4 × 2 floats
(8 buffers total).

### Precision contract: places=4 across all 4 scales, 8-bit and 10-bit

| Backend / bit-depth | scale 0 | scale 1 | scale 2 | scale 3 |
|---|---:|---:|---:|---:|
| Vulkan (Mesa anv + Arc A380) — 8-bit  | `1e-6` | `1.4e-5` | `1.8e-5` | `3.7e-5` |
| Vulkan — 10-bit                       | `1e-6` | `1e-6`   | `7e-6`   | `2e-6`   |
| CUDA (RTX 4090) — 8-bit               | `1e-6` | `1.4e-5` | `1.8e-5` | `3.7e-5` |
| SYCL (Arc A380, oneAPI 2025.3) — 8-bit| `1e-6` | `1.4e-5` | `1.8e-5` | `3.7e-5` |

Identical numbers across backends. All under `places=4` threshold
(`5e-5`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Match scalar CPU's mirror (`-1`)** | Matches the documented `vif_mirror_tap_h` | Doesn't match AVX2 production; drift `5.46e-4` at scale 1 — fails `places=4` | Production runs AVX2; we match what production runs |
| **Use double-precision V→H accumulators on GPU** | Higher precision; might converge to "ground truth" | Doubles aren't natively supported on Intel Arc; emulated; large perf hit. AVX2 has its own deviation from ground truth (~`5e-4` vs scalar) so doubles wouldn't help close the AVX2 gap anyway | Mirror fix solves the actual deviation source for cheaper |
| **Loosen the gate to `places=3`** | Quick green | Weakens the test; doesn't expose real bugs | Per CLAUDE.md / project memory: never weaken correctness gates to make red turn green |
| **Support all 11 `vif_kernelscale` values in v1** | Full CPU feature parity | 11 × 4 = 44 distinct filter coef arrays; large compile-time table; the cross-backend gate only exercises kernelscale=1.0 | v1 ships the production default; alternative scales TBD as a focused follow-up |

## Consequences

- **Positive**: third Group B float twin shipped at the same
  `places=4` contract as `float_psnr` / `float_motion`. Mirror-fix
  documented for future maintainers (and for the eventual `float_adm`
  port — which uses the same `convolution_avx.c` infrastructure and
  likely has the same asymmetry).
- **Positive**: identical numerical output across all three GPU
  backends — strong correctness signal.
- **Negative**: the kernel restricts `vif_kernelscale` to 1.0 only.
  Callers that pass other values get `-EINVAL` at init. Documented in
  the extractor's option table; matches the `vif_kernelscale = 1.0`
  default that production uses.
- **Negative**: 7 dispatches per frame is heavier than batch 3's
  earlier kernels (1 dispatch each). Per-scale resolution decreases
  geometrically so the total compute is bounded; per-frame wall-clock
  on Arc A380 at 576×324 is roughly 4× a single-dispatch kernel.
- **Neutral / follow-ups**:
  1. CHANGELOG + features.md updates ship in the same PR per ADR-0100.
  2. Lavapipe lane gains a `float_vif` step at `places=4`.
  3. Next batch 3 metric: `float_adm` (most complex; closes Group B).

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch 3 scope.
- Sibling: [ADR-0193](0193-motion-v2-vulkan.md), [ADR-0194](0194-float-ansnr-gpu.md),
  [ADR-0195](0195-float-psnr-gpu.md), [ADR-0196](0196-float-motion-gpu.md).
- CPU reference:
  [`float_vif.c`](../../core/src/feature/float_vif.c),
  [`vif.c`](../../core/src/feature/vif.c),
  [`vif_tools.c`](../../core/src/feature/vif_tools.c).
- AVX2 fast path:
  [`convolution_avx.c`](../../core/src/feature/common/convolution_avx.c),
  [`convolution_internal.h`](../../core/src/feature/common/convolution_internal.h).
- Verification: cross-backend gate
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  with `--feature float_vif --places 4`. New step in the lavapipe
  lane of
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
- User direction: 2026-04-27 — "fix everything to fucking 4 places min".
