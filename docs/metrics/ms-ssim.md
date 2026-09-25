<!-- markdownlint-disable MD013 MD060 -->
# MS-SSIM

MS-SSIM (Multi-Scale Structural Similarity Index Measure) extends SSIM to a
multi-resolution pyramid, providing a perceptual similarity metric that is
robust to viewing distance and display resolution variation. Each scale
captures structural information at a different spatial frequency.

## Variant

The fork ships one CPU MS-SSIM extractor:

| Extractor name | Algorithm | Options |
|---|---|---|
| `float_ms_ssim` | Floating-point IQA library, 5-scale Gaussian pyramid | `enable_lcs`, `enable_db`, `clip_db`, `enable_chroma` |

GPU twins do **not** all expose the same options, and the difference is
user-visible. Read from the twins' own `options[]` tables:

| Twin | `enable_lcs` | `enable_db` | `clip_db` | `enable_chroma` |
|---|---|---|---|---|
| `float_ms_ssim_cuda` | yes | yes | yes | **no** |
| `float_ms_ssim_sycl` | yes | yes | yes | yes — computed on the GPU (3 planes) |
| `integer_ms_ssim_hip` | yes | yes | yes | yes — **accepted but a no-op** |
| `float_ms_ssim_metal` | yes | yes | yes | yes — computed on the GPU (3 planes) |

What that means when a model requests `enable_chroma`:

- **Metal** computes it. Each plane gets its own geometry and pyramid/partials
  storage, dispatched through the Metal compute pipeline. The twin advertises
  `float_ms_ssim_cb` and `float_ms_ssim_cr` in `provided_features` and the
  `g_metal_features` dispatch table, serving chroma on the GPU. It also exposes
  `enable_db` and `clip_db`, extending the ADR-1221 `max_db` rule under
  [ADR-1334](../adr/1334-metal-ms-ssim-option-parity.md).
- **SYCL** computes it. Each plane gets its own geometry, staging buffer and
  pyramid, and the planes run sequentially through the shared reduction
  workspace the way the five scales already do. The twin advertises
  `float_ms_ssim_cb` and `float_ms_ssim_cr` in `provided_features`, so those
  features are served by the GPU rather than falling back to the CPU twin by
  name. Verified on an Intel Arc A380 against the CPU twin with a
  textured-chroma 4:2:0 fixture: identical to all six emitted digits across
  three frames, with GPU time rising about 21% as the extra planes dispatch
  (ADR-1299).

  Until 2026-09-23 this option was accepted and silently ignored here —
  `n_planes` was computed and never read — and the chroma numbers a model saw
  came from the CPU twin under the ADR-0530 name fallback.
- **HIP** accepts the option and clamps to luma only; its own option help says
  so. It does not advertise `float_ms_ssim_cb` / `_cr` in `provided_features`,
  so those two features route to the CPU twin by name (the ADR-0530 fallback).
  The score is correct; the chroma planes simply are not GPU-accelerated.
- **CUDA** does not accept the option at all. `vmaf_fex_ctx_parse_options()`
  rejects it with `feature extractor 'float_ms_ssim_cuda': unknown option
  'enable_chroma'` and returns `-EINVAL`, so the run fails loudly rather than
  silently dropping chroma.

### Minimum resolution with chroma

The 5-level 11-tap pyramid needs every scored plane to be at least 176x176, and
with `enable_chroma` that includes the subsampled ones. Plane allocation uses
ceil subsampling, so for 4:2:0 the exact luma minimum is 351x351 (352x352 is
the next even-sized input), not 176x176. The CPU, SYCL, and Metal twins refuse
an input whose scored plane is smaller at init and name the chroma size they
measured; before ADR-1299 the CPU
twin checked luma only and a 4:2:0 input in between died mid-run with
`error: scale below 1x1!` on stdout and no output file — including on this
repository's own 576x324 Netflix fixture, whose chroma is 288x162.

Note that `--feature float_ms_ssim=enable_chroma=true` does **not** exercise any
of this: `--feature` selects the CPU extractor, so all three backends return
identical CPU numbers for that command line. The twins are reached only through
a model that names the feature. (The Vulkan backend was removed in ADR-0726.)

Tracked as `T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06` in
[docs/state.md](../state.md).

## `float_ms_ssim` extractor

The extractor uses the IQA library's Gaussian-window floating-point
implementation with a 5-scale Laplacian pyramid (Wang et al. 2004). It is the
extractor invoked when VMAF model JSON files reference `"float_ms_ssim"`.

The minimum supported input resolution is 176x176. Smaller inputs cause the
5-level pyramid to fall below the 11-tap Gaussian kernel footprint and are
rejected with an error at init time (Netflix#1414 / ADR-0153).

### Output features

| Feature name | Description | Condition |
|---|---|---|
| `float_ms_ssim` | MS-SSIM on the luma (Y) plane | Always |
| `float_ms_ssim_cb` | MS-SSIM on the Cb (U) chroma plane | `enable_chroma=true` only |
| `float_ms_ssim_cr` | MS-SSIM on the Cr (V) chroma plane | `enable_chroma=true` only |
| `float_ms_ssim_l_scale0-4` | Per-scale luminance component | `enable_lcs=true`, luma only |
| `float_ms_ssim_c_scale0-4` | Per-scale contrast component | `enable_lcs=true`, luma only |
| `float_ms_ssim_s_scale0-4` | Per-scale structure component | `enable_lcs=true`, luma only |

## Options

- `enable_chroma` (bool, default `false`): emit per-plane `_cb` and `_cr` scores in addition to luma. YUV400P sources are always luma-only.
- `enable_lcs` (bool, default `false`): emit per-scale luminance, contrast, and structure intermediate components for the luma plane.
- `enable_db` (bool, default `false`): report the luma MS-SSIM score as dB (`-10 * log10(1 - score)`).
- `clip_db` (bool, default `false`): cap the dB score at a **ceiling derived
  from the frame geometry** — `max_db = ceil(10 * log10(peak² / mse))` with
  `mse = 0.5 / (w * h)`. It is a ceiling on the dB *output*, not a clamp on the
  linear score, and it also defines what a perfect match reports: `score >= 1.0`
  returns `max_db` rather than `+Inf`. Only meaningful when `enable_db=true`.

  > **GPU scores before this release were wrong.** Up to and including v3.2.1 the
  > CUDA, SYCL and HIP twins read `clip_db` as a clamp on the *linear* score —
  > `[0, 1]`, then `-10 * log10(1 - score)` with no ceiling — and carried no
  > `max_db` at all. Scoring an identical reference/distorted pair returned
  > `+Inf`, and every high-similarity pair returned an uncapped dB value, so
  > `clip_db` did not clip. Fixed per
  > [ADR-1221](../adr/1221-gpu-ms-ssim-db-ceiling.md). **Re-measure any GPU
  > MS-SSIM dB score taken with `clip_db` set.** The Metal twin has been brought
  > to full parity by [ADR-1334](../adr/1334-metal-ms-ssim-option-parity.md),
  > implementing `enable_db`, `clip_db` with `max_db` ceiling, and
  > `enable_chroma`.

### How to run

```bash
# Luma-only MS-SSIM (default)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature float_ms_ssim --output /dev/stdout

# Per-channel MS-SSIM (luma + Cb + Cr)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature 'float_ms_ssim:enable_chroma=true' --output /dev/stdout
```

## See also

- [SSIM](ssim.md) - single-scale structural similarity
- [SSIMULACRA2](ssimulacra2.md) - perceptually tuned alternative
