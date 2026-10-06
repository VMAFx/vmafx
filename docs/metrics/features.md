<!-- markdownlint-disable MD060 -->
# Features

A **feature extractor** is a per-frame computation that libvmaf runs as part of
scoring. Each extractor publishes one or more named metrics into the result
report, and VMAF models fuse some of them into the final VMAF score. You can
also request an extractor on its own, with no model and no fusion, when all you
want is PSNR, SSIM, CIEDE2000 or another single metric. Pass its registered
name to `--feature`.

This page is the index of every extractor. Use it to find the registered name,
the backends that accelerate it, whether a device twin is bit-identical to the
CPU, and the page that documents it. The option reference of the extractors
without a page of their own follows the table.

Per [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) every
user-discoverable extractor ships what / range / invocation / input formats /
limitations in the same PR as the code.

## Select an extractor

Name the extractor after `--feature`. Options go after the name: the first `=`
ends the name, `:` separates options, and each option is `key=value`.

```bash
# Single extractor, no model
vmaf --reference ref.yuv --distorted dis.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --no_prediction --feature psnr \
     --output score.json --json

# Several extractors
vmaf ... --feature psnr --feature ssim --feature ciede --feature cambi ...

# Per-extractor options
vmaf ... --feature "psnr=enable_mse=true:enable_apsnr=true" ...
vmaf ... --feature "adm=adm_enhn_gain_limit=1.0" ...
```

- **CLI aliases** — `integer_motion` selects `motion`, `integer_motion2`
  selects `motion_v2`, `integer_ssim` selects `ssim`, `integer_ms_ssim` selects
  `float_ms_ssim` and `integer_psnr` selects `psnr`. No other `integer_*` name
  is an alias: the fixed-point VIF and ADM are `vif` and `adm`.
- **ffmpeg** — `libvmaf=feature=name=vif` (set through `av_opt_set`); options
  follow the name with `:`, for example `libvmaf=feature=name=float_vif` or
  `libvmaf=feature=name=motion_v2`.
- **C API** — `vmaf_use_feature(ctx, "psnr", opts)` or
  `vmaf_use_features_from_model()`:

```c
VmafFeatureDictionary *opts = NULL;
vmaf_feature_dictionary_set(&opts, "enable_mse", "true");
vmaf_use_feature(ctx, "psnr", opts);
```

See [usage/cli.md](../usage/cli.md) for the full CLI grammar and
[api/index.md](../api/index.md#vmaffeaturedictionary) for the dictionary
ownership rules.

With `--backend cuda|sycl|hip|metal`, `--feature <name>` runs that backend's
twin of the extractor when one exists. You can also name a twin directly, for
example `--feature float_adm_cuda`. A backend that was not compiled in fails
instead of falling back to the CPU
([ADR-0498](../adr/0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)); see
[Backends](../backends/index.md) for the runtime dispatch rules.

## Extractor coverage

Names in the first column are the registered names in
`core/src/feature/feature_extractor.cpp`. Twin columns show the registered
twin name, or `—` when none is registered. Exactness comes from the
declaration files `scripts/ci/exact_twins.d/<feature>.<backend>`: `exact` means
the parity gate compares the twin with tolerance 0, `bound <value>` is a
documented tolerance from `scripts/ci/cross_backend_calibration.py`. The
generated list of declarations is
[cross-backend-exact-twins.md](../development/cross-backend-exact-twins.md).

| Registered name | Measures | CPU SIMD | CUDA twin | SYCL twin | HIP twin | Metal twin | Exactness of the CUDA / SYCL / HIP twins | Page |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `vif` | Visual information fidelity, four scales (core) | AVX2, AVX-512, NEON | `vif_cuda` | `vif_sycl` | `vif_hip` | `integer_vif_metal` | cuda `exact`, sycl `exact`, hip `exact` | [vif](vif.md) |
| `float_vif` | VIF in float (core) | AVX2 (convolution only) | `float_vif_cuda` | `float_vif_sycl` | `float_vif_hip` | `float_vif_metal` | cuda `exact`, sycl `exact`, hip `exact` | [vif](vif.md) |
| `motion` | Motion2, fixed-point (core) | AVX2, AVX-512, NEON | `motion_cuda` | `motion_sycl` | `motion_hip` | `integer_motion_metal` | cuda `exact`, sycl `exact`, hip `exact` | [motion](motion.md) |
| `motion_v2` | Pipelined Motion2 | AVX2, AVX-512, NEON | `motion_v2_cuda` | `motion_v2_sycl` | `motion_v2_hip` | `motion_v2_metal` | cuda `exact`, sycl `exact`, hip `exact` | [motion](motion.md) |
| `float_motion` | Motion2 in float (core) | AVX2, AVX-512, NEON | `float_motion_cuda` | `float_motion_sycl` | `float_motion_hip` | `float_motion_metal` | cuda `exact`, sycl `exact`, hip `exact` | [motion](motion.md) |
| `adm` | Additive detail metric, fixed-point (core) | AVX2, AVX-512, NEON | `adm_cuda` | `adm_sycl` | `adm_hip` | `integer_adm_metal` | cuda `exact`, sycl `exact`, hip `exact` | [adm](adm.md) |
| `float_adm` | ADM in float (core) | AVX2, AVX-512, NEON | `float_adm_cuda` | `float_adm_sycl` | `float_adm_hip` | `float_adm_metal` | cuda `exact`, sycl `exact`, hip `exact` | [adm](adm.md) |
| `cambi` | Banding index | AVX2, AVX-512, NEON | `cambi_cuda` | `cambi_sycl` | `cambi_hip` | `integer_cambi_metal` | cuda `exact`, sycl `exact`, hip `exact` | [cambi](cambi.md) |
| `ciede` | CIEDE2000 colour difference | AVX2, AVX-512, NEON | `ciede_cuda` | `ciede_sycl` | `ciede_hip` | `integer_ciede_metal` | cuda `bound 1e-9`, sycl `bound 1e-9`, hip `bound 1e-9` | [ciede](ciede.md) |
| `psnr` | PSNR, fixed-point (Y, Cb, Cr) | AVX2, AVX-512, NEON | `psnr_cuda` | `psnr_sycl` | `psnr_hip` | `integer_psnr_metal` | cuda `exact`, sycl `exact`, hip `exact` | [psnr](psnr.md) |
| `float_psnr` | PSNR in float (luma) | AVX2, AVX-512, NEON | `float_psnr_cuda` | `float_psnr_sycl` | `float_psnr_hip` | `float_psnr_metal` | cuda `exact`, sycl `exact`, hip `exact` | [psnr](psnr.md) |
| `psnr_hvs` | PSNR weighted by a contrast-sensitivity function | AVX2, NEON | `psnr_hvs_cuda` | `psnr_hvs_sycl` | `psnr_hvs_hip` | `integer_psnr_hvs_metal` | cuda `exact`, sycl `exact`, hip `exact` | [psnr-hvs](psnr-hvs.md) |
| `ssim` | SSIM, fixed-point | AVX2 | `integer_ssim_cuda` | `integer_ssim_sycl` | `integer_ssim_hip` | `integer_ssim_metal` | cuda `exact`, sycl `exact`, hip `exact` | [ssim](ssim.md) |
| `float_ssim` | SSIM in float | AVX2, AVX-512, NEON | `float_ssim_cuda` | `float_ssim_sycl` | `float_ssim_hip` | `float_ssim_metal` | cuda `exact`, sycl `exact`, hip `exact` | [ssim](ssim.md) |
| `float_ms_ssim` | Multi-scale SSIM | AVX2, AVX-512, NEON | `float_ms_ssim_cuda` | `float_ms_ssim_sycl` | `integer_ms_ssim_hip` | `float_ms_ssim_metal` | cuda `exact`, sycl `exact`, hip `exact` | [ms-ssim](ms-ssim.md) |
| `ssimulacra2` | SSIMULACRA 2 in XYB space | AVX2, AVX-512, NEON, SVE2 | `ssimulacra2_cuda` | `ssimulacra2_sycl` | `ssimulacra2_hip` | `ssimulacra2_metal` | cuda `exact`, sycl `exact`, hip `exact` | [ssimulacra2](ssimulacra2.md) |
| `float_moment` | First and second moments | AVX2, AVX-512, NEON, SVE2 | `float_moment_cuda` | `float_moment_sycl` | `float_moment_hip` | `float_moment_metal` | cuda `exact`, sycl `exact`, hip `exact` | [float-moment](float-moment.md) |
| `speed_chroma` | SpEED chroma (research) | AVX2, AVX-512, NEON | `speed_chroma_cuda` | `speed_chroma_sycl` | `speed_chroma_hip` | — | cuda `exact`, sycl `exact`, hip `exact` | [speed_qa](speed_qa.md) |
| `speed_temporal` | SpEED temporal (research) | AVX2, AVX-512, NEON | `speed_temporal_cuda` | `speed_temporal_sycl` | `speed_temporal_hip` | — | cuda `exact`, sycl `exact`, hip `exact` | [speed_qa](speed_qa.md) |
| `speed_qa` | SpEED-QA | — | — | — | — | — | — | [speed_qa](speed_qa.md) |
| `niqe` | No-reference naturalness (distorted frame only) | — | — | — | — | — | — | [niqe](niqe.md) |
| `y_funque_plus` | Y-FUNQUE+ atoms (fused SVR deferred) | — | — | — | — | — | — | [y-funque-plus](y-funque-plus.md) |
| `brisque` | No-reference BRISQUE | — | — | — | — | — | — | [brisque](brisque.md) |
| `delta_e_itp` | Delta E ITP colour difference | — | — | — | — | — | — | [delta_e_itp](delta_e_itp.md) |
| `pu21` | PU21 perceptually uniform PSNR / SSIM | — | — | — | — | — | — | [pu21](pu21.md) |
| `tad` | Temporal absolute difference (Rust pilot, `-Denable_rust_features=true`) | — | — | — | — | — | — | [tad](tad.md) |
| `lpips` | Learned perceptual image patch similarity (ONNX) | — | — | — | — | — | — | [tiny-ai-extractors](tiny-ai-extractors.md#lpips-learned-perceptual-image-patch-similarity) |
| `dists_sq` | DISTS-shaped perceptual distance (ONNX, placeholder weights) | — | — | — | — | — | — | [dists](dists.md) |
| `fastdvdnet_pre` | Temporal denoising pre-filter, residual only (ONNX) | — | — | — | — | — | — | [tiny-ai-extractors](tiny-ai-extractors.md#fastdvdnet-pre-temporal-denoising-pre-filter) |
| `transnet_v2` | Shot-boundary detector (ONNX) | — | — | — | — | — | — | [tiny-ai-extractors](tiny-ai-extractors.md#transnet_v2-transnet-v2-shot-boundary-detector) |
| `mobilesal` | Saliency mean (ONNX) | — | — | — | — | — | — | [tiny-ai-extractors](tiny-ai-extractors.md#mobilesal-mobilesal-saliency-map) |

### Published metrics

| Extractor | Metrics it publishes |
| --- | --- |
| `vif` | `vif_scale0`, `vif_scale1`, `vif_scale2`, `vif_scale3` |
| `float_vif` | `float_vif_scale0` … `float_vif_scale3` |
| `motion` | `motion2` (+ `motion` with `debug=true`) |
| `motion_v2` | `VMAF_integer_feature_motion_v2_sad_score`, `VMAF_integer_feature_motion2_v2_score`, `VMAF_integer_feature_motion3_v2_score` |
| `float_motion` | `float_motion2` (+ `float_motion` with `debug=true`, the default) |
| `adm` | `adm2`, `adm_scale0` … `adm_scale3`, `aim_score`, `adm3_score` |
| `float_adm` | `float_adm2`, `adm_scale0` … `adm_scale3`, `aim_score`, `adm3_score` |
| `cambi` | `cambi` |
| `ciede` | `ciede2000` |
| `psnr` | `psnr_y`, `psnr_cb`, `psnr_cr` (+ MSE and APSNR when enabled) |
| `float_psnr` | `float_psnr` (luma only; the CPU extractor emits a single luma score) |
| `psnr_hvs` | `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb`, `psnr_hvs_cr` |
| `ssim` | `ssim` |
| `float_ssim` | `float_ssim` (+ L/C/S when enabled) |
| `float_ms_ssim` | `float_ms_ssim` (+ per-scale L/C/S when enabled) |
| `ssimulacra2` | `ssimulacra2` |
| `niqe` | `niqe` (no-reference; scores the distorted frame only) |
| `y_funque_plus` | `y_funque_plus_ms_ssim`, `y_funque_plus_dlm`, `y_funque_plus_mad` (atoms only) |
| `float_moment` | `float_moment_ref1st`, `float_moment_dis1st`, `float_moment_ref2nd`, `float_moment_dis2nd` |
| `speed_chroma` (`--feature speed_chroma`) | `Speed_chroma_feature_speed_chroma_u_score`, `..._v_score`, `..._uv_score` |
| `speed_temporal` (`--feature speed_temporal`) | `Speed_temporal_feature_speed_temporal_score` |
| `speed_qa` | `speed_qa` |
| `brisque` | `brisque` (no-reference) |
| `delta_e_itp` | `delta_e_itp` |
| `pu21` | `pu21_psnr`, `pu21_ssim` |
| `tad` | `tad`, `tad_sad` |
| `lpips` | `lpips` |
| `dists_sq` | `dists_sq` |
| `fastdvdnet_pre` | `fastdvdnet_pre_l1_residual` |
| `transnet_v2` | `shot_boundary_probability`, `shot_boundary` |
| `mobilesal` | `saliency_mean` |
| `float_ansnr` | removed: `float_ansnr` and `float_anpsnr` are no longer emitted |

### Notes on the table

- **Core** extractors (`vif`, `float_vif`, `motion`, `float_motion`, `adm`,
  `float_adm`) are inputs of the shipped VMAF models (see
  [models/overview.md](../models/overview.md)); the others are standalone.
- **Metal** twins are registered when libvmaf is built with Metal. None is
  declared exact; Metal has no `speed_chroma`, `speed_temporal` or `speed_qa`
  twin (`GAP-METAL-MISSING-SPEED-TWINS`). `ssimulacra2_metal` runs a hybrid
  host/GPU pipeline. Metal parity runs on an Apple device from the macOS tester
  bundle, not on a hosted runner (ADR-1496).
- **`float_psnr`, `float_adm`, `float_vif`, `float_motion`, `float_moment`,
  `speed_chroma` and `speed_temporal`** (and their twins) are registered only
  when libvmaf is built with `-Denable_float=true` (`VMAF_FLOAT_FEATURES=1`),
  which is the default
  (`core/meson_options.txt`).
- **`psnr` GPU chroma** — the twins honour `enable_chroma` (default `true`) and
  emit `psnr_cb` / `psnr_cr` next to `psnr_y`. With `enable_chroma=false`, only
  `psnr_y` is emitted, as on the CPU. YUV 4:0:0 sources always produce luma
  only. GPU chroma parity for CUDA and SYCL came with ADR-0453.
- **`float_ssim` and `float_ms_ssim`** — the `ssim_accumulate_avx512` reduction
  is vectorised, bit-exact against scalar
  ([ADR-0139](../adr/0139-ssim-simd-bitexact-double.md), PR #342, about 7 to 11
  percent
  less wall-clock time on the SSIM and MS-SSIM hot path). The fixed-point `ssim`
  has an AVX2 path only.
- **Tiny-AI extractors** run their ONNX graph on the ORT execution provider
  selected with `--tiny-device` (CPU, CUDA, OpenVINO, ROCm); libvmaf has no
  SIMD or GPU path for their pre- and post-processing. See
  [`docs/ai/inference.md`](../ai/inference.md).
- **HIP** — 19 extractors are registered in `feature_extractor_list[]` and
  resolve through `vmaf_get_feature_extractor_by_name` (T7-10b). See
  [`backends/hip/overview.md`](../backends/hip/overview.md).
- **`float_vif_hip`** takes part in model-driven dispatch through the build
  option `enable_float_vif_hip_autodispatch`, on by default since
  [ADR-2092](../adr/2092-vmafx-hip-device-frames.md) ([vif](vif.md)).
- **`null`** is a registered no-op extractor used by tests.
- **`float_ansnr`** was removed (PR #38, ADR-0865): `--feature float_ansnr`
  fails with feature-not-found on a current build. See
  [ANSNR](ansnr.md).

Depending on your build configuration not every backend is available; see
[`backends/`](../backends/index.md).

## Non-finite result handling

A failed extractor computation never becomes a plausible metric. If VIF, ADM,
SSIM, MS-SSIM, SSIMULACRA2, TransNet V2 or the final VMAF piecewise mapping
produces `NaN` or an unexpected infinity, libvmaf fails that frame with
`-EINVAL`. It does not substitute a minimum, a maximum, a threshold result or
zero.

What this means for you:

1. **The frame has no score.** The CLI treats the negative return as a runtime
   error and prints no fabricated value; C API callers handle the existing
   negative-errno contract ([API error
   semantics](../api/index.md#error-semantics)).
   The log callback names the extractor, the frame and the offending value
   where known.
2. **Hidden values are checked too.** CPU, CUDA, HIP, SYCL and Metal host paths
   validate the complete enabled score set before their first collector write.
   That covers VIF debug numerators and denominators, ADM reductions before
   their precision floor, and every MS-SSIM L/C/S atom even when
   `enable_lcs=false`.
   - All four VIF ratios must be finite before any scale is published (scale 0
     stays unclamped, scales 1 to 3 then apply their minimum).
   - The same complete-set rule holds for float and integer VIF and for
     SSIMULACRA2 on scalar, SIMD, CUDA, HIP, SYCL and Metal.
3. **Finite results are unchanged**, including ADM's defined perfect aggregate
   flat-frame result, a finite ADM per-scale `0/0` (now an explicit `1.0`) and
   scores that legitimately reach a configured clamp.

One output is non-finite on purpose: with dB output enabled and
`clip_db=false`, a finite perfect SSIM or MS-SSIM raw score reports positive
infinity (ADR-1221). Set `clip_db=true` to cap it at `max_db`. NaN raw scores
and invalid dB ceilings still fail the frame.

!!! note
    Those substitutions used to look legitimate: an invalid SSIMULACRA2 result
    appeared as the perfect `100.0`, an invalid SSIM as `max_db`, and an
    undefined ADM AIM ratio as the perfect `1.0`.

## Per-feature GPU dispatch hints (T7-26 / ADR-0181)

Each feature carries a small `VmafFeatureCharacteristics` descriptor that
drives the per-backend dispatch decision: graph replay or direct submit on
SYCL, graph capture or streams on CUDA. The per-backend `dispatch_strategy`
modules consume it
([`core/src/{cuda,sycl}/dispatch_strategy.{c,h}`](../../core/src/sycl/dispatch_strategy.cpp)).
The defaults match the behaviour before T7-26 byte for byte: graph replay above
720p area, direct submit below.

Two environment variables override the default. Each takes a comma-separated
list of `feature:strategy` pairs and wins over the registry default for the
named features:

| Env var | Strategy values | Effect |
| --- | --- | --- |
| `VMAF_SYCL_DISPATCH` | `graph` / `direct` | Per-feature SYCL graph-replay override. |
| `VMAF_CUDA_DISPATCH` | `graph` / `direct` | Per-extractor CUDA graph-capture override, keyed by the CUDA extractor name (`vif_cuda:graph`) and read when the extractor initialises. Graph capture is not implemented: `graph` logs a warning and runs direct ([env-var reference](../usage/env-vars.md#cuda-dispatch)). |

Examples:

```bash
# Force ADM to direct submit on SYCL (default below 720p, override above):
VMAF_SYCL_DISPATCH=adm:direct vmaf [...] --feature adm_sycl --backend sycl

# Mix per-feature strategies:
VMAF_SYCL_DISPATCH=vif:graph,motion:direct,adm:graph vmaf [...]
```

The legacy global knobs `VMAF_SYCL_USE_GRAPH=1` and `VMAF_SYCL_NO_GRAPH=1`
remain as aliases that force every feature to graph or to direct; the
per-feature `VMAF_SYCL_DISPATCH` takes precedence.

## Option reference

Extractors with their own page link to it. The sections below give the options
of the rest. Each option is a feature parameter, so a non-default value
appends its alias to the published feature name.

### VIF — Visual Information Fidelity

VIF measures information-fidelity loss between reference and distorted at four
Gaussian-pyramid scales. In the original Sheikh/Bovik formulation the scales
are combined into a single score; in VMAF each scale is kept as a separate
feature so the model can learn per-scale weights. See [vif](vif.md) for the
twin measurements and the minimum frame size.

- **Run** — `--feature vif` (fixed-point, default) or `--feature float_vif`;
  ffmpeg `libvmaf=feature=name=vif`; C API `vmaf_use_feature(ctx, "vif", opts)`.
- **Output metrics** — `vif_scale0`, `vif_scale1`, `vif_scale2`, `vif_scale3`:
  per-scale fidelity ratios in `[0, 1]`. Higher is better (1 =
  reference-identical). With `debug=true` also `vif`, `vif_num`, `vif_den`, and
  per-scale `*_num` / `*_den`.
- **Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0, 8 / 10 / 12 / 16 bpc.
  Y plane only.
- **Backends** — `vif`: AVX2, AVX-512, NEON, CUDA, SYCL, HIP, Metal.
  `float_vif`: AVX2 convolution, CUDA, SYCL, HIP, Metal.

| Option | Alias | Declared by | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- | --- |
| `debug` | — | both | bool | `false` | — | Emit `vif`, `vif_num`, `vif_den` and the per-scale numerator and denominator. |
| `vif_enhn_gain_limit` | `egl` | both | double | `100.0` | `1.0–100.0` | Cap on the enhancement-gain ratio, so over-sharpened output cannot saturate. `1.0` disables the enhancement-gain path (matches pre-v1.3 behaviour). |
| `vif_skip_scale0` | `ssclz` | both | bool | `false` | — | Skip the finest scale: exclude it from the aggregate and report it as zero ([below](#vif_skip_scale0-and-what-it-publishes)). |
| `vif_kernelscale` | `ks` | `float_vif` | double | `1.0` | `0.1–4.0` | Scale the Gaussian kernel standard deviation. |
| `vif_prescale` | `ps` | `float_vif` | double | `1.0` | `0.1–4.0` | Resize factor applied to the frame before VIF. |
| `vif_prescale_method` | `pm` | `float_vif` | string | `nearest` | — | Resize method for the prescale. |
| `vif_scale1_min_val` … `vif_scale3_min_val` | `s1miv` … `s3miv` | `float_vif` | double | `0.0` | `0.0–1.0` | Minimum value of scale 1 … 3; smaller values are set to it. |
| `vif_sigma_nsq` | `snsq` | `float_vif` | double | `2.0` | `0.0–5.0` | Neural-noise variance. |

#### `vif_skip_scale0` and what it publishes

With `vif_skip_scale0=true` the finest VIF scale is dropped from the aggregate
`integer_vif` score **and** its own score is reported as exactly `0.0` rather
than as a computed ratio. The two are separate effects, and a backend has to do
both:

| Key | Value when skipping |
| --- | --- |
| `integer_vif` | sum over scales 1–3 only |
| `VMAF_integer_feature_vif_scale0_score` | `0.0` |
| `integer_vif_num_scale0` / `..._den_scale0` (debug only) | `0.0` / `-1.0` |

Because the option is a `FEATURE_PARAM`, setting it changes the published
feature names: the alias `ssclz` is appended, so the scale-0 score is filed
under `integer_vif_scale0_ssclz`. Read that key, not the default one.
Where a GPU twin declares one of these options, its alias must match the CPU
extractor so an equivalent configuration publishes the same key
([ADR-1312](../adr/1312-gpu-option-alias-parity.md)).

The CPU reference simply never computes scale 0. The GPU twins do compute all
four scales and apply the skip when they publish, so the zeroing lives at the
emission site rather than in the kernel. `integer_vif` honours this on CPU,
CUDA, SYCL, HIP and Metal.

**Reference** — Sheikh H. R., Bovik A. C., "Image information and visual
quality," IEEE TIP 15(2):430–444, 2006.

### Motion

Motion measures temporal activity: both frames are blurred with a fixed
low-pass filter and the mean absolute pixel difference between the current and
the previous reference luma is taken. `motion2` is the improved version with
proper padding and boundary handling; the unfixed `motion` is kept behind
`debug=true` for back-compat. See the [Motion page](motion.md) for output
ranges, the five-frame window and the per-variant backend matrix.

- **Run** — `--feature motion` (fixed-point), `--feature float_motion` or
  `--feature motion_v2`; ffmpeg `libvmaf=feature=name=motion` (or
  `name=motion_v2`); C API `vmaf_use_feature(ctx, "motion", opts)` (for
  `motion_v2`, `vmaf_use_feature(ctx, "motion_v2", NULL)`).
- **Output range** — `[0, ∞)` before the `motion_max_val` clamp (default
  10 000). Zero for a frozen reference; grows with motion content.
- **Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc. Y plane
  only.
- **Limitations** — temporal. The extractor carries state across frames (two
  previous blurred references) and has a flush callback that emits the final
  frame's score after the input stream ends. Single-frame scoring is not
  supported; Motion2 on frame 0 is defined as `0.0`.

#### Output metrics

| Extractor | Metrics |
| --- | --- |
| `motion` | `motion2` (the shipped feature), `motion3`, the SAD score (CPU, CUDA, SYCL and HIP; see [Motion](motion.md)), and the legacy `motion` with `debug=true` |
| `float_motion` | `float_motion2`, `float_motion3`, and `float_motion` (the legacy variant, emitted by default because `debug` defaults to `true`) |
| `motion_v2` | `VMAF_integer_feature_motion_v2_sad_score`, `VMAF_integer_feature_motion2_v2_score`, `VMAF_integer_feature_motion3_v2_score` |

#### Options of `motion` and `float_motion`

| Option | Alias | `motion` | `float_motion` | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `debug` | — | yes (default `false`) | yes (default `true`) | bool | see column | — | Emit the legacy `motion` alongside `motion2`. |
| `motion_force_zero` | `force_0` | yes | yes | bool | `false` | — | Override all scores to `0.0`, for deterministic test fixtures. |
| `motion_fps_weight` | `mfw` | yes | yes | double | `1.0` | `0.0–5.0` | Multiplicative FPS-aware correction applied before clamping. |
| `motion_blend_factor` | `mbf` | yes | yes | double | `1.0` | `0.0–1.0` | Blend factor of `motion3`. |
| `motion_blend_offset` | `mbo` | yes | yes | double | `40.0` | `0.0–1000.0` | Score offset at which blending of `motion3` begins. |
| `motion_max_val` | `mmxv` | yes | yes | double | `10000.0` | `0.0–10000.0` | Upper clamp of the emitted scores. |
| `motion_five_frame_window` | `mffw` | yes | — | bool | `false` | — | Take each SAD against the frame two back instead of the previous one. |
| `motion_moving_average` | `mma` | yes | — | bool | `false` | — | Two-frame moving average of `motion3`. |
| `motion_add_scale1` | `mdc` | — | yes | bool | `false` | — | Add a half-resolution bilinear-downsampled SAD term on top of the full-resolution SAD. |
| `motion_add_uv` | `mau` | — | yes | bool | `false` | — | Add the U and V plane SADs to the Y SAD. |
| `motion_filter_size` | `mfs` | — | yes | int | `5` | `0–9` | Blur kernel size; `5` is the original Motion2 filter, `3` a cheaper variant. |

`force_0` is the collector-key suffix on the CPU, CUDA, SYCL and HIP motion
twins; backend selection does not change the published key.

#### Options of `motion_v2`

`motion_v2` declares seven options with the same aliases, defaults and ranges
as the `motion` rows above: `motion_force_zero`, `motion_blend_factor`,
`motion_blend_offset`, `motion_fps_weight`, `motion_max_val`,
`motion_five_frame_window` and `motion_moving_average`. It has no `debug`
option.

#### Motion v2 — pipelined Motion2

`motion_v2` exploits the linearity of the blur kernel. Instead of storing the
blurred previous reference across frames, it folds the frame difference, blur
and absolute sum into one row-at-a-time pipeline that needs a single scratch
row. The score equals Motion2 apart from the SAD-against-sum semantics
described below. It is a separate extractor so callers can opt into the
pipelined arithmetic without touching the legacy `motion` registry entry.

- **Output metrics** — `VMAF_integer_feature_motion_v2_sad_score` is the
  per-frame sum of absolute blurred differences (frame 0 emits `0.0`).
  `VMAF_integer_feature_motion2_v2_score` is the Motion2-equivalent score
  (current frame plus the next frame's score, divided by 2, matching the legacy
  temporal smoothing). `VMAF_integer_feature_motion3_v2_score` is the
  perceptually blended and clipped score, optionally moving-averaged
  ([Motion](motion.md)).
- **Output range** — `[0, ∞)`, in the units of Motion2.
- **Limitations** — temporal. The extractor caches its own previous reference
  in a GPU-side ping-pong (the framework's `prev_ref` slot is unused on the GPU
  paths). Frame 0 is `0.0`, and the final frame's smoothed score is emitted by
  the flush callback, as for `motion`.

#### Motion backends

- **CPU** — AVX2, AVX-512 and NEON for `motion`, `float_motion` and
  `motion_v2`.
- **GPU, three-frame window** — all GPU backends emit `motion`, `motion2` and
  `motion3` (the latter since T3-15(c) /
  [ADR-0219](../adr/0219-motion3-gpu-coverage.md)).
- **GPU, five-frame window** (`motion_five_frame_window=true`,
  [motion.md](motion.md#five-frame-window)) — runs on the CPU and on the CUDA,
  SYCL and HIP twins of `motion` and `motion_v2`, bit-identical to the CPU
  ([ADR-1491](../adr/1491-gpu-motion-five-frame-window.md)). On Metal the CPU
  extractor computes it.
- **`motion_add_uv`, `motion_add_scale1`, `motion_filter_size`** — implemented
  on the CPU and by `float_motion_hip`
  ([ADR-1404](../adr/1404-hip-float-motion-motion3-and-options.md), see
  [motion.md](motion.md)); the CUDA, SYCL and Metal `float_motion` twins do not
  declare them. See [backends/cuda/overview.md §Known
  gaps](../backends/cuda/overview.md#known-gaps)
  and [backends/sycl/overview.md §Known
  gaps](../backends/sycl/overview.md#known-gaps).
- **`float_motion` twins** — `float_motion_cuda` and `float_motion_sycl` return
  the CPU scores bit for bit at `--precision max`
  ([ADR-1409](../adr/1409-float-motion-twins-cpu-float-sum.md),
  [ADR-1411](../adr/1411-sycl-float-motion-cpu-float-sum.md)), and so does
  `float_motion_hip`
  ([ADR-1419](../adr/1419-hip-float-motion-cpu-float-sum.md)). The Metal
  `float_motion` twin has not been measured on a device.
- **`motion_v2` twins** — bit-exact against the CPU scalar reference on 8-bit
  and 10-bit inputs (`max_abs_diff = 0.0` across the cross-backend gate
  fixture), for
  [`integer_motion_v2_cuda.c`](../../core/src/feature/cuda/integer_motion_v2_cuda.c),
  [`integer_motion_v2_sycl.cpp`](../../core/src/feature/sycl/integer_motion_v2_sycl.cpp)
  and
  [`integer_motion_v2_hip.c`](../../core/src/feature/hip/integer_motion_v2_hip.c).
  They share a design:
  - one dispatch over `(prev_ref - cur_ref)` exploits convolution linearity and
    skips the per-frame blurred-state buffer;
  - a raw-pixel ping-pong of two private device buffers caches the previous
    frame's Y plane;
  - per-workgroup `int64` SAD partials reduce on the host;
  - `motion2_v2_score = min(score[i], score[i+1])` is emitted in `flush()`.
- **`motion_v2` padding** — mirror padding **diverges** from the corresponding
  `motion_*` kernels by one pixel at the boundary. CPU `integer_motion_v2.c`
  uses reflect-101 mirror `2*size - idx - 2` (ADR-0662 corrected stale GPU-side
  prose that had documented `-1`).

### ADM — Additive Detail Metric

ADM measures detail loss and additive impairment at four wavelet sub-band
scales. Its options, outputs, backend measurements and edge-case behaviour are
on the [ADM page](adm.md): run it with `--feature adm` (fixed-point) or
`--feature float_adm`.

#### AIM above 1

`integer_aim` of the fixed-point `adm` can exceed 1, while `float_adm`'s `aim`
is clipped at 1; both match upstream Netflix/vmaf
([ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md)). The full
explanation and the numbers are in
[AIM above 1](adm.md#aim-above-1).

### CAMBI — Contrast-Aware Multiscale Banding Index

CAMBI has parameters enough for its own reference: see [cambi](cambi.md).

- **Run** — `--feature cambi`; options such as `full_ref` go after the name
  (`--feature cambi=full_ref=true`).
- **Output** — `cambi` in `[0, ∞)`; 0 = no banding, larger = more visible
  banding. Typical "bad" content sits in `1–10`.
- **Backends** — CPU (AVX2, AVX-512, NEON) and CUDA, SYCL, HIP and Metal twins.
  The SYCL, CUDA and HIP twins run every stage on the device
  ([ADR-1357](../adr/1357-sycl-cambi-device-resident.md),
  [ADR-1379](../adr/1379-cuda-cambi-device-resident-pipeline.md),
  [ADR-1378](../adr/1378-hip-cambi-device-resident.md)).

### CIEDE2000 — colour-difference metric

`--feature ciede` converts both frames to CIELAB and averages the CIEDE2000 ΔE
per pixel. Output `ciede2000` in `[0, ~100]` (smaller is better), no options,
no 4:0:0 input. See [ciede](ciede.md) for the twin measurements and the
agreement with Netflix's source.

### PSNR

Peak Signal-to-Noise Ratio on each colour plane. The fixed-point `psnr` path
is the default; the `float_psnr` path is kept for parity with upstream
consumers of the float pipeline. See [PSNR](psnr.md) for the full comparison.

- **Run** — `--feature psnr` or `--feature float_psnr`; ffmpeg
  `libvmaf=feature=name=psnr`.
- **Output metrics** (fixed) — `psnr_y`, `psnr_cb`, `psnr_cr`. With
  `enable_mse=true` also `mse_y/cb/cr`. With `enable_apsnr=true` also
  `apsnr_y/cb/cr` (aggregate across the whole clip, emitted at flush).
  `float_psnr` emits `float_psnr`, luma only.
- **Output range** — dB, saturated at `6 × bpc + 12`: 60 dB for 8 bpc, 72 dB
  for 10 bpc, 84 dB for 12 bpc, 108 dB for 16 bpc. That value is reported both
  when the two planes are identical (MSE=0, where the true PSNR is `+inf`) and,
  by default, when a computed score exceeds it. Set `uncapped=true` to report
  the true value while keeping the MSE=0 sentinel
  ([ADR-1193](../adr/1193-psnr-uncapped-option.md)), or `min_sse` to raise both
  at once.
- **Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0, 8 / 10 / 12 / 16 bpc.
- **Limitations** — temporal flag set only because of `apsnr` accumulation;
  per-frame PSNR itself is stateless.

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_chroma` | bool | `true` | Include `psnr_cb` / `psnr_cr`; set `false` for luma-only. |
| `enable_mse` | bool | `false` | Emit `mse_y/cb/cr` alongside PSNR. |
| `enable_apsnr` | bool | `false` | Emit clip-aggregate `apsnr_y/cb/cr` at flush. |
| `reduced_hbd_peak` | bool | `false` | Scale the high-bit-depth peak to match 8-bit content. |
| `min_sse` | double | `0.0` | Clamp the minimum MSE, so both the PSNR ceiling and the identical-frame sentinel. |
| `uncapped` | bool | `false` | Report the true PSNR instead of truncating at the ceiling; the MSE=0 sentinel is unaffected. |

`float_psnr` declares `uncapped` only.

How the twins treat the options:

- The GPU twins honour `enable_chroma` and emit `psnr_cb` / `psnr_cr`
  identically to the CPU path; pass `enable_chroma=false` for luma-only
  operation on any backend.
- `uncapped` is mirrored on every GPU twin under the same name and default.
- `psnr_sycl` and `psnr_cuda` also implement `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak` and `min_sse`, bit-exact with the CPU. `psnr_hip` does so
  through the CPU's own helpers and matched the CPU bit for bit on a gfx1036
  (ADR-1382).
- On Metal those four options keep `psnr` on the CPU (see
  [PSNR](psnr.md#options)).
- `float_psnr` adds CUDA, SYCL, HIP and Metal twins on the float pipeline and
  accepts `uncapped` on all of them.

### PSNR-HVS

PSNR weighted by a human-visual-system contrast-sensitivity function applied in
the DCT domain. It correlates better with subjective quality than plain PSNR on
blocking-style distortions. See [psnr-hvs](psnr-hvs.md).

- **Run** — `--feature psnr_hvs`.
- **Output metrics** — `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb`, `psnr_hvs_cr`.
- **Output range** — dB, typically `20–60`.
- **Input formats** — 8 to 12 bpc, YUV 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4. Deeper
  input is rejected at init.
- **Options** — `enable_chroma` (default `true`).
- **Backends** — scalar (Xiph reference), AVX2
  ([ADR-0159](../adr/0159-psnr-hvs-avx2-bitexact.md)) and NEON on aarch64
  ([ADR-0160](../adr/0160-psnr-hvs-neon-bitexact.md)). The 8×8 integer DCT block
  is vectorised eight rows in parallel (butterfly, transpose, butterfly,
  transpose); the float accumulators stay scalar to keep byte-identity with the
  reference. Verified bit-identical to scalar on the three Netflix golden
  pairs, with about 3.58x DCT microbenchmark speedup on AVX2. The GPU twins
  `psnr_hvs_cuda`, `psnr_hvs_sycl` and `psnr_hvs_hip` are bit-identical to the
  CPU ([ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-1401](../adr/1401-psnr-hvs-sycl-hip-exact-twins.md); see
  [agreement with the CPU extractor](psnr-hvs.md#gpu-twins)).

### SSIM / MS-SSIM

Structural Similarity Index on luma. MS-SSIM extends SSIM to five
Gaussian-pyramid scales and fuses them with the Wang 2003 weights. See
[ssim](ssim.md) and [ms-ssim](ms-ssim.md) for the precision of each twin.

- **Run** — `--feature ssim` (fixed-point), `--feature float_ssim` or
  `--feature float_ms_ssim`; ffmpeg `libvmaf=feature=name=ssim`.
- **Output metrics** — `ssim` (one scalar in `[0, 1]`); `float_ssim` (scalar in
  `[0, 1]`, with `enable_lcs=true` also `float_ssim_l`, `float_ssim_c`,
  `float_ssim_s`); `float_ms_ssim` (scalar in `[0, 1]`, with `enable_lcs=true`
  the per-scale triples `float_ms_ssim_{l,c,s}_scale{0..4}`, with
  `enable_chroma=true` also `float_ms_ssim_cb` and `float_ms_ssim_cr`).
- **Output range** — `[0, 1]`, higher is better. With `enable_db=true` the
  score is `-10 × log10(1 − score)`; `clip_db=true` caps the infinite value of
  identical frames.
- **Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc.

#### Options

| Option | `ssim` | `float_ssim` | `float_ms_ssim` | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `enable_lcs` | — | yes | yes | bool | `false` | — | Emit the L / C / S components (per scale for MS-SSIM). |
| `enable_db` | yes | yes | yes | bool | `false` | — | Report `-10·log10(1-score)` instead of the raw ratio. |
| `clip_db` | yes | yes | yes | bool | `false` | — | Cap dB values based on the minimum representable MSE. |
| `scale` | — | yes | — | int | `0` | `0–10` | Decimation factor of `float_ssim`: `0` = auto per Wang 2003, `1` = none, `2–10` explicit. |
| `enable_chroma` | — | — | yes | bool | `false` | — | Score the chroma planes too. |

#### Minimum dimensions

- `float_ms_ssim` needs at least **176×176** luma. The five Gaussian-pyramid
  scales force a `2⁴ = 16`× downsample on the smallest level; smaller inputs
  (for example QCIF) make the decimate kernel produce undefined output, so
  init rejects them with `-EINVAL` and a log message
  ([ADR-0153](../adr/0153-float-ms-ssim-min-dim-netflix-1414.md)).
- `ssim` and `float_ssim` have no such constraint, but `float_ssim` scores
  windows of 11×11 samples. A luma plane with fewer than 11 samples in a
  direction (after the `scale` decimation) has no window, and the score is `0`
  (the sum of no terms over `(w − 10) · (h − 10)`, as in Netflix's libvmaf) on
  the scalar and every SIMD path.
- With exactly 10 samples in a direction the divisor is 0, the mean is `0 / 0`
  and the frame fails with a non-finite-score error
  ([ADR-1302](../adr/1302-nonfinite-scores-fail-the-frame.md)).
- The GPU twins do not run such a plane: `--backend <gpu> --feature float_ssim`
  computes it on the CPU and says so, and `--feature float_ssim_cuda` (or
  `_sycl`, `_hip`) fails at init
  ([ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md)).

#### Twins and options

| Twin | Options it implements | Arithmetic against the CPU |
| --- | --- | --- |
| `integer_ssim_cuda` | `enable_db`, `clip_db` ([ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md)) | Bit for bit ([ADR-1424](../adr/1424-cuda-ssim-cpu-frame-sum.md)). |
| `integer_ssim_sycl` | `enable_db`, `clip_db` ([ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md)) | Bit for bit ([ADR-1443](../adr/1443-sycl-ssim-cpu-arithmetic.md)). |
| `integer_ssim_hip` | `enable_db`, `clip_db` ([ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)) | Declared exact (`exact_twins.d/ssim.hip`). |
| `integer_ssim_metal` | see [ssim](ssim.md) | Precision stated on the SSIM page. |
| `float_ssim_cuda` | `enable_lcs`, `enable_db`, `clip_db`, `scale`; accepts and ignores the `enable_chroma` the CPU `float_ssim` never had | Equal to the CPU on every measured frame ([ADR-1399](../adr/1399-cuda-float-ssim-device-decimation.md), [ADR-1464](../adr/1464-cuda-float-ssim-raster-order-sum.md)). |
| `float_ssim_sycl` | `enable_lcs`, `enable_db`, `clip_db`, `scale` | Bit for bit: decimation on the device ([ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md)), terms added in the CPU's order ([ADR-1463](../adr/1463-sycl-float-ssim-raster-sum.md)). |
| `float_ssim_hip` | `enable_db`, `clip_db`, `enable_lcs` | CPU's own `l * c * s` arithmetic ([ADR-1405](../adr/1405-hip-float-ssim-device-decimation.md), ADR-1382). |
| `float_ssim_metal` | scale 1 only; larger scales run on the CPU extractor | — |
| `float_ms_ssim_cuda` | all MS-SSIM options | Bit-identical at `--precision max`, per-scale `enable_lcs` outputs included ([ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-1465](../adr/1465-cuda-float-ms-ssim-raster-order-sum.md)). |
| `integer_ms_ssim_hip` (registered for `float_ms_ssim`) | all MS-SSIM options | Same arithmetic as the CUDA twin, bit-identical on a gfx1036 ([HIP backend](../backends/hip/twins.md#integer_ms_ssim_hip)). |
| `float_ms_ssim_sycl` | all MS-SSIM options | Bit-identical without fp64 on the device ([ADR-1414](../adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md)); see [MS-SSIM](ms-ssim.md#agreement-with-the-cpu). |
| `float_ms_ssim_metal` | all MS-SSIM options | Within the cross-backend tolerance of `5e-5`. |

Notes on the twins:

- The `enable_lcs` option ships across all MS-SSIM backends: CPU, CUDA, SYCL,
  HIP and Metal emit the same 15 `float_ms_ssim_{l,c,s}_scale{0..4}` metrics on
  top of the combined score (T7-35 / [ADR-0243](../adr/0243-enable-lcs-gpu.md)).
- On SYCL and CUDA, identical frames report the CPU's `+inf` or `clip_db`
  ceiling on the device too. `float_ssim_cuda` computes the CPU's per-pixel
  `l * c * s` and fp32 frame mean, so identical frames report the CPU's value
  (`+inf`, or 72.247 dB for flat frames). `float_ssim_hip` reports the same
  finite 72.247 dB the CPU gives some identical frames.
- `float_ssim_sycl`, `float_ssim_cuda` and `float_ssim_hip` decimate on the
  device at the automatic scale and at every explicit one, so 1080p and 4K
  `float_ssim` run on those backends.
- The `float_ssim_cuda` host sum costs about 1 ns per scored window and the
  `float_ms_ssim_cuda` one about 2.1 ns per scored window. The SYCL host sum
  costs time only where the scale is 1 ([SYCL
  backend](../backends/sycl/history.md#float_ssim_sycl-adds-its-frame-sums-in-the-cpus-order-2026-10-02)).

#### MS-SSIM decimate (fork-local)

The 9-tap 9/7 biorthogonal wavelet low-pass filter that produces scales 1–4
runs through `ms_ssim_decimate` in
[`core/src/feature/ms_ssim_decimate.c`](../../core/src/feature/ms_ssim_decimate.c).
SIMD variants live in
[`core/src/feature/x86/ms_ssim_decimate_avx2.c`](../../core/src/feature/x86/ms_ssim_decimate_avx2.c)
(8-wide),
[`core/src/feature/x86/ms_ssim_decimate_avx512.c`](../../core/src/feature/x86/ms_ssim_decimate_avx512.c)
(16-wide) and
[`core/src/feature/arm64/ms_ssim_decimate_neon.c`](../../core/src/feature/arm64/ms_ssim_decimate_neon.c)
(4-wide). Dispatch prefers AVX-512 over AVX2 over scalar on x86 and NEON over
scalar on aarch64 at runtime through `vmaf_get_cpu_flags()`. All four paths are
strictly **byte-identical** (per-lane `fmaf` / `_mm{256,512}_fmadd_ps` /
`vfmaq_n_f32` with broadcast coefficients and scalar-fallback borders);
`core/test/test_ms_ssim_decimate.c` verifies it across 1x1, 8x8, 9x9,
border-edge and 1920x1080 cases
([ADR-0125](../adr/0125-ms-ssim-decimate-simd.md)).

### ANSNR — Adjusted Noise SNR

!!! note
    **Removed.** The CPU implementation and all GPU twins (CUDA, SYCL, HIP,
    Metal) were removed in commit 70ed8b3ce3 (PR #38); the Vulkan backend and
    its kernel source were removed in ADR-0726. No `float_ansnr` source remains
    in the tree, and `--feature float_ansnr` returns a feature-not-found error
    on any current build. The historical GPU kernel design
    ([ADR-0194](../adr/0194-float-ansnr-gpu.md)) is kept for reference only.

ANSNR was SNR after a noise-shaping Wiener filter: historical VMAF input that
no shipped model consumed. See [ansnr](ansnr.md).

### SSIMULACRA 2 — perceptual similarity in XYB space

`--feature ssimulacra2` scores one scalar per frame in `[0, 100]` (higher is
better; identical frames return exactly `100`). It is a port of the libjxl
reference metric. See [ssimulacra2](ssimulacra2.md) for the score bands, the
options and the twin measurements.

- **Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 bpc. Chroma is
  nearest-neighbour upsampled to luma resolution. Minimum input 8×8.
- **Options** — `yuv_matrix` (int, default `0`, range `0–3`): `0` BT.709
  limited, `1` BT.601 limited, `2` BT.709 full, `3` BT.601 full.
- **CPU backends** — AVX2, AVX-512, NEON and SVE2, bit-identical to scalar.
- **Twins** — `ssimulacra2_cuda`, `ssimulacra2_sycl` and `ssimulacra2_hip` are
  device-resident and return the CPU's score bit for bit
  (`exact_twins.d/ssimulacra2.{cuda,sycl,hip}`;
  [ADR-1391](../adr/1391-cuda-ssimulacra2-device-resident.md),
  [ADR-1433](../adr/1433-cuda-ssimulacra2-cpu-sum-order.md),
  [ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md),
  [ADR-1446](../adr/1446-sycl-ssimulacra2-cpu-bits.md),
  [ADR-1445](../adr/1445-hip-ssimulacra2-cpu-sum-order.md)).
  `ssimulacra2_metal` runs a hybrid host/GPU pipeline. Name a twin with
  `--feature ssimulacra2_cuda` (or `_sycl`, `_hip`) and pair it with the
  matching `--backend` flag for exclusive GPU dispatch.
- **Limitations** — `create_recursive_gaussian` derives its coefficients with
  Cramer's rule in doubles, which yields the same `n2` / `d1` floats as libjxl's
  `Inv3x3Matrix` for σ=1.5 at 10-decimal precision but is not guaranteed
  bit-exact at every σ; the fork pins σ=1.5, libjxl's `kSigma`.

The SIMD and determinism history is under [History](#history).

### Float moment — first and second statistical moments

`--feature float_moment` emits `float_moment_ref1st`, `float_moment_dis1st`,
`float_moment_ref2nd` and `float_moment_dis2nd`, has no options and runs on all
four backends. See [float-moment](float-moment.md).

### Tiny-AI extractors

`lpips`, `dists_sq`, `fastdvdnet_pre`, `mobilesal` and `transnet_v2` run an
ONNX model through ONNX Runtime and take one option, `model_path`. All five are
on [tiny-ai-extractors](tiny-ai-extractors.md); the summaries below keep the
headings that other pages link to.

#### LPIPS — learned perceptual image patch similarity

`--feature lpips=model_path=/path/to/lpips.onnx` emits `lpips` (lower is more
similar). See
[tiny-ai-extractors](tiny-ai-extractors.md#lpips-learned-perceptual-image-patch-similarity).

#### DISTS-Sq — deep image structure and texture similarity

`--feature dists_sq=model_path=/path/to/dists_sq.onnx` emits `dists_sq`; the
shipped checkpoint is a smoke placeholder. See [dists](dists.md) and
[tiny-ai-extractors](tiny-ai-extractors.md#dists-sq-deep-image-structure-and-texture-similarity).

#### FastDVDnet pre — temporal denoising pre-filter

`--feature fastdvdnet_pre=model_path=...` emits the diagnostic
`fastdvdnet_pre_l1_residual`, not a quality score. See
[tiny-ai-extractors](tiny-ai-extractors.md#fastdvdnet-pre-temporal-denoising-pre-filter).

#### `mobilesal` — MobileSal saliency map (tiny-AI, NR / single-input)

`--feature mobilesal=model_path=...` emits `saliency_mean` for the distorted
frame, 8-bit input only. See
[tiny-ai-extractors](tiny-ai-extractors.md#mobilesal-mobilesal-saliency-map).

#### `transnet_v2` — TransNet V2 shot-boundary detector (tiny-AI, NR / single-input)

`--feature transnet_v2=model_path=...` emits `shot_boundary_probability` and
`shot_boundary` over a 100-frame window. See
[tiny-ai-extractors](tiny-ai-extractors.md#transnet_v2-transnet-v2-shot-boundary-detector).

### Speed (chroma + temporal) — Netflix research extractors

`speed_chroma` and `speed_temporal` are research-stage extractors ported from
Netflix upstream commit
[`d3647c73`](https://github.com/Netflix/vmaf/commit/d3647c73), with its
dependency `4ad6e0ea` for the `vif_tools` helpers. They share a spatial-pooling
backbone in the style of SpEED-QA and a per-frame neural-network-shaped
weighting. They register when libvmaf is built with `-Denable_float=true`, the
default. See also [speed_qa](speed_qa.md).

Their scores are Netflix's: every value compared with a build of Netflix
master is identical, and the CUDA, HIP and SYCL twins return the CPU's scores
bit for bit
([ADR-1477](../adr/1477-speed-upstream-double-math.md); the comparison, the
options where the fork differs on purpose and what changed on 2026-10-02 are on
the [SpEED page](speed_qa.md#the-scores-are-netflixs)).

| Extractor | Output metrics | Input formats | Backends |
| --- | --- | --- | --- |
| `speed_chroma` | `Speed_chroma_feature_speed_chroma_u_score`, `..._v_score`, `..._uv_score` (no `y` score) | YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc | CPU (scalar, AVX2, AVX-512, NEON); CUDA, SYCL, HIP; Metal missing (`GAP-METAL-MISSING-SPEED-TWINS`) |
| `speed_temporal` | `Speed_temporal_feature_speed_temporal_score` | same | same |

`speed_chroma` scores the U and V planes of the input pictures, lifted with
`picture_copy(..., channel)` from the same upstream commit, plus a combined
U+V score. `speed_temporal` scores a small cyclic frame buffer and captures
distortions that appear only across consecutive frames, such as flicker and
judder.

#### Speed options

| Option | Alias | Declared by | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- | --- |
| `speed_kernelscale` | `ks` | both | double | `1.0` | `0.1–4.0` | Scale of the Gaussian kernel (2.0 doubles the standard deviation and enlarges the kernel). |
| `speed_prescale` | `ps` | both | double | `1.0` | `0.1–4.0` | Resize factor of each plane before the SpEED filters; values above 1 upsample. |
| `speed_prescale_method` | `psm` | both | string | `nearest` | `nearest`, `bilinear`, `bicubic`, `lanczos4` | Resize method. |
| `speed_sigma_nn` | `snn` | both | double | `0.29` | `0.1–2.0` | Standard deviation of the neural noise. |
| `speed_nn_floor` | `nnf` | both | double | `0.0` | `0.0–1.0` | Neural-noise floor, as a share of sigma_nn. |
| `speed_max_val` | `mxv` | both | double | `1000.0` | `0.0–1000.0` | Upper clamp of the scores. |
| `speed_weight_var_mode` | `wvm` | `speed_chroma` | int | `0` | `0–6` | Approach to variance-based weighting. |
| `speed_use_ref_diff` | `urd` | `speed_temporal` | bool | `false` | — | Debug mode: additional output. |

Defaults match Netflix upstream; `core/src/feature/speed.c` carries the help
strings.

#### Speed performance and exactness notes

- The SIMD paths vectorise the 25x25 covariance matrix each plane needs. They
  return the scalar path's bits on every input: a kernel keeps one lane per
  covariance sum instead of splitting one sum over lanes, and multiplies and
  adds separately ([ADR-1459](../adr/1459-speed-cov-kernel-exact.md)).
  Scalar, AVX2, AVX-512 and NEON dispatch give the same scores
  (`core/test/test_speed_simd.c` compares the sums bit for bit).
- The kernels upstream Netflix ships for AVX2 and AVX-512 are faster on large
  planes and differ from scalar in the last bits of a sum. This fork does not
  use them, and `speed_chroma` + `speed_temporal` take up to 12 % more CPU time
  per frame for it (3840x2160, AVX-512).
- Each plane goes through a Gaussian anti-alias filter and is then decimated by
  16 in both directions. On x86 the whole plane is filtered (AVX2) and one
  sample in 256 is kept. On every other target the filter is evaluated only at
  the kept samples (`vif_filter1d_dec16_s()` in
  `core/src/feature/vif_tools.c`, Netflix/vmaf
  [`76ea5f03`](https://github.com/Netflix/vmaf/commit/76ea5f03)), which returns
  the same bits as filtering the plane with the scalar filter and decimating
  it; `core/test/test_speed_filter.c` compares the two with `memcmp`. Scores do
  not change on any target.
- In subsampled formats (4:2:0, 4:2:2) an odd luma width or height produces an
  extra chroma row or column to cover the last luma sample
  (`vmaf_chroma_extent()`); `speed_chroma` sizes its buffers from
  `speed_chroma_dimensions()`, matching `picture.c`.

**Stability** — research. The option grammar and score scale may shift in
future Netflix upstream commits; track upstream releases before pinning these
features into a downstream pipeline.

## Related

- [CAMBI](cambi.md) — banding-specific extractor.
- [Confidence Interval](confidence-interval.md) — bootstrapped uncertainty on
  the final VMAF score.
- [Bad cases](bad-cases.md) — how to report content where extractors disagree
  with subjective ratings.
- [Backends](../backends/index.md) — which SIMD / GPU paths get picked at
  runtime.
- [Models](../models/overview.md) — how the fixed-point core extractors feed
  into the shipped VMAF models.
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — the per-surface
  doc bar this page satisfies.

## Licensing of the extractors (ADR-1250)

Extractor sources carry their terms per file as an `SPDX-License-Identifier`.
An extractor the fork wrote is EUPL-1.2. One that carries Netflix's, libjxl's,
Xiph's or IQA's code (every SIMD and GPU kernel of an upstream metric, and the
scalar ports of third-party references) keeps that code's terms and its
copyright notice. `scripts/dev/relicense_fork_files.py --list` prints the
verdict and the reason for every file. See
[ADR-1250](../adr/1250-eupl-fork-relicense.md).

## History

Newest first.

### Dated changes, September and October 2026

- **2026-10-03** — [ADR-1500](../adr/1500-arm-float-moment-scalar-order.md)
  aligned the NEON and SVE2 `float_moment` sums with the scalar order
  ([float-moment](float-moment.md)).
- **2026-10-02** — SpEED scores follow Netflix's upstream double math
  ([ADR-1477](../adr/1477-speed-upstream-double-math.md); see the
  [SpEED page](speed_qa.md#the-scores-are-netflixs)).
- **2026-10-01** — `--backend <gpu> --feature float_moment` stopped computing
  on the CPU with a no-twin warning and runs the twin.
- **2026-09-30** — The CPU `speed_temporal` overran its frame buffers for any
  `speed_prescale` above 1, so the run crashed or corrupted memory; it now
  sizes them for the upscaled plane
  ([Netflix/vmaf#1626](https://github.com/Netflix/vmaf/issues/1626)). Output at
  `speed_prescale` 1 and below is unchanged, and so is `speed_chroma`, which
  prescale never affected. `core/test/test_speed_frame_buffers.c` extracts three
  frames at 1.0, 1.5, 2.0 and 4.0. Also on this date `speed_chroma` (CPU, CUDA
  and HIP) sized its buffers by floor division for odd subsampled frame sizes,
  so `picture_copy()` wrote past the allocation; it now sizes from
  `speed_chroma_dimensions()`.

### Dated changes, April and May 2026

- **2026-05-28** — The Vulkan backend and its column were removed
  ([ADR-0726](../adr/0726-drop-vulkan-backend.md)). Earlier ADRs that mention
  Vulkan or a Vulkan stub describe historical state.
- **2026-04-29** — Port of upstream Netflix/vmaf
  [`b949cebf`](https://github.com/Netflix/vmaf/commit/b949cebf): the
  `motion_add_scale1`, `motion_add_uv`, `motion_filter_size` and
  `motion_max_val` options. With the defaults, output is bit-identical to the
  pre-port baseline on the Y-plane SIMD fast path; non-default options route
  through the scalar `compute_motion()` path. The same port lets `float_motion`
  emit `motion3_score` (a perceptual blend of `motion2`, controlled by
  `motion_blend_factor` / `motion_blend_offset`) on the second frame. The
  trained VMAF models do not consume `motion3_score` and are unchanged.
- **2026-04-29** — SSIMULACRA 2: ARM64 SVE2 ports of the IIR blur and
  `picture_to_linear_rgb` ([ADR-0213](../adr/0213-ssimulacra2-sve2.md)), moving
  the SVE2 deferral notes in
  [Research-0016](../research/0016-ssimulacra2-iir-blur-simd.md) and
  [Research-0017](../research/0017-ssimulacra2-ptlr-simd.md) from "deferred" to
  "shipped". The SVE2 path runs alongside NEON on hosts that advertise the
  `sve2` HWCAP and falls back to NEON otherwise.
- **2026-04-25** — SSIMULACRA 2: three SIMD ports landed, the pointwise and
  reduction kernels ([ADR-0161](../adr/0161-ssimulacra2-simd-bitexact.md)), the
  IIR blur ([ADR-0162](../adr/0162-ssimulacra2-iir-blur-simd.md)) and
  `picture_to_linear_rgb` ([ADR-0163](../adr/0163-ssimulacra2-ptlr-simd.md)).
  All SIMD paths build with `-ffp-contract=off` in dedicated split static
  libraries to pin cross-host bit-exactness. The source-level `FP_CONTRACT OFF`
  pragmas are wrapped for GCC so warning-clean builds keep the same no-FMA
  contract; compilers that ignore the pragma rely on the split-library flag.

### SSIMULACRA 2 background

- **Origin** — a fork-added scalar port of the libjxl reference metric, with a
  bit-close C port of libjxl's `FastGaussian` 3-pole recursive IIR as the
  pyramid blur
  ([ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md),
  [Research-0007](../research/0007-ssimulacra2-scalar-port.md)).
- **SIMD parity** — scalar and SIMD outputs are byte-identical on the fork's
  host matrix (`core/test/test_ssimulacra2_simd.c`, 11 unit tests).
- **Cross-host determinism** — libm `cbrtf` and `powf(x, 2.4)` are replaced
  with deterministic polynomials: `vmaf_ss2_cbrtf` (bit-trick init plus two
  Newton-Raphson iterations, about 7e-7 accuracy) and a 1024-entry sRGB-EOTF
  LUT (about 5e-7 accuracy)
  ([ADR-0164](../adr/0164-ssimulacra2-snapshot-gate.md)).
- **CI snapshot gate** — `python/test/ssimulacra2_test.py` pins 48-frame mean,
  min, max, hmean, frame-0 and frame-47 values at `places=4`.
- **Warning hygiene** — the registration structs of the CPU, SIMD and tiny-AI
  extractors are kept warning-clean across the hosted CI matrix; that does not
  change feature names, option names, score formulas or backend selection.
- **GPU twins** — first reported as optional follow-up work (BACKLOG T3-8) and
  shipped in [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md). On a first GPU
  iteration GPU `cbrtf` differed from libm by up to 42 ULP and cascaded to a
  1.59e-2 pooled-score drift, which is why every path uses the shared
  `vmaf_ss2_cbrtf`.
- **Contraction** — the CUDA and SYCL IIR kernels are built without contraction
  (`--fmad=false` on CUDA, `-fp-model=precise` or contraction off on SYCL) so
  `n2*sum - d1*prev1 - prev2` keeps its CPU ordering.

### Overview table notes

- `float_ansnr` was removed per
  [ADR-0865](../adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md).
- The CAMBI Vulkan kernel (T7-36 / ADR-0210) was removed with the backend in
  ADR-0726.
