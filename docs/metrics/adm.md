<!-- markdownlint-disable MD060 -->
# ADM — Additive Detail Metric (DLM)

ADM scores how much of the reference's detail survives in the distorted
picture. Run `--feature adm` (fixed-point, the one the shipped models read) or
`--feature float_adm`; both are in [features](features.md), which lists every
extractor and backend.

ADM separately measures **detail loss** (the component that affects content
visibility) and **additive impairment** (which distracts attention) at four
wavelet sub-band scales. VMAF uses the detail-loss branch (`adm2`), and the
default model also reads `adm3_score`. Numerical edge cases (black frames, flat
areas) are handled specifically to avoid divide-by-zero.

## Run it

- CLI: `--feature adm` (fixed-point) or `--feature float_adm`. Options go after
  the name: `--feature adm=adm_enhn_gain_limit=1.0`; separate several options
  with `:`.
- ffmpeg: `libvmaf=feature=name=adm`.
- C API: `vmaf_use_feature(ctx, "adm", opts)`.
- Device twins: `--backend cuda|sycl|hip|metal --feature adm` runs the twin of
  that backend; see [Backends](#backends).

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc. Y plane
only. The smallest accepted frame is 17x17 ([Small frames](#small-frames)).

## Output metrics

- `adm2` — the fused final value, published as the VMAF-model input. Range
  `[0, 1]`.
- `adm_scale0..3` — per-wavelet-scale fidelity.
- `aim_score` — Additive Impairment Measure (AIM): the contrast-masked
  additive impairment (CM of `decouple_a` relative to the CSF of
  `decouple_r`, with `noise_weight = 0`) divided by the DLM denominator.
  0 means no additive impairment; **higher is worse**. The fixed-point
  `adm` extractor reports the ratio as it is, so its range is `[0, ∞)`;
  `float_adm` clips it, range `[0, 1]`. See [AIM above 1](#aim-above-1).
  Required by the Netflix HDR VMAF model and by the default model
  `vmaf_v1.0.16_3d0h`.
- `adm3_score` — ADM version 3: `max(w * adm2 + (1 - w) * (1 - aim),
  adm_min_val)` with `w = adm_dlm_weight`, or the harmonic mean of adm2
  and AIM (`adm_adm3_apply_hm=true`, float path), floored at
  `adm_min_val` in both cases. This is the ADM feature the default model
  consumes.
- With `debug=true`: `adm`, `adm_num`, `adm_den`, and per-scale
  numerator / denominator. `adm` is never suffixed with the options (the
  Netflix tests read it under every option set), so a run can hold only one
  `float_adm` instance with `debug=true`: a second is refused when it is
  registered, with a message naming the key
  ([ADR-2056](../adr/2056-float-adm-debug-key-refusal.md)). Put `debug=true`
  on one instance, or leave it off.

**Output range** — `adm2` and the scale scores lie in `[0, 1]` unless the
distorted picture has more of the reference's detail than the reference, which
a contrast enhancement produces under an `adm_enhn_gain_limit` above 1. Higher
is better. `aim_score` is the exception on both counts: higher is worse, and
the fixed-point one has no upper bound.

### Which extractor emits `aim_score` and `adm3_score`

| Path | Extractors that emit both | Notes |
| --- | --- | --- |
| Fixed-point `adm` | CPU `adm`, `adm_cuda` (ADR-0746), `adm_sycl` (ADR-1362, bit-identical to the CPU), `adm_hip` ([ADR-1525](../adr/1525-adm-hip-aim-device-pass.md), bit-identical to the CPU), `integer_adm_metal` | Each twin has a dedicated AIM contrast-measure device pass. |
| `float_adm` | CPU `float_adm`, `float_adm_cuda` (ADR-0574), `float_adm_sycl` ([ADR-1434](../adr/1434-sycl-float-adm-cpu-arithmetic.md)), `float_adm_hip`, `float_adm_metal` | `float_adm_sycl` returns the CPU's values for both. |

## Options

Every option except `debug`, `adm_skip_aim` and `adm_norm_view_dist_extra` is
a feature parameter, so a non-default value changes the published feature name
(the alias is appended, for example `adm_egl_1.2`).
"Declared by" lists the extractors whose option table has the entry; an
extractor that does not declare an option rejects it as unknown.

| Option | Alias | Type | Default | Range | Declared by | Effect |
| --- | --- | --- | --- | --- | --- | --- |
| `debug` | — | bool | `false` | — | all | Emit debug metrics. |
| `adm_enhn_gain_limit` | `egl` | double | `100.0` | `1.0–100.0` | all | How many times its reference value a restored coefficient may count for. `1.0` (`egl=1.0`) counts no enhancement as restored detail (NEG models and `vmaf_v1.0.16_*`); `100.0` is upstream's default. Note 1. |
| `adm_norm_view_dist` | `nvd` | double | `3.0` | `0.75–24.0` (integer `adm`: `nvd × rdh ≥ 3240`) | all | Normalised viewing distance (distance ÷ display height). Note 6. |
| `adm_norm_view_dist_extra` | `nvde` | double | `0.0` (none) | `0–24.0`; must pass the same floor and give names other than `adm_norm_view_dist`'s | `adm` (CPU, `adm_rust`, CUDA) | A second viewing distance evaluated on the same DWT and decouple; its scores carry that distance's `nvd` suffix. Note 7. |
| `adm_ref_display_height` | `rdh` (`adm`), `rdf` (`float_adm`) | int | `1080` | `1–4320` | all | Reference display height in pixels, for viewing-distance scaling. |
| `adm_csf_mode` | `csf` | int | `0` | `0–3` (`adm`), `0–9` (`float_adm`) | all | Contrast-sensitivity-function model. Note 2. |
| `adm_csf_scale` | `scf` | double | `1.0` | `0–50` | all | H/V-axis CSF sensitivity scale. Read only by `adm_csf_mode=1`. Note 3. |
| `adm_csf_diag_scale` | `scfd` | double | `1.0` | `0–50` | all | Diagonal-axis CSF sensitivity scale; same applicability. |
| `adm_noise_weight` | `nw` | double | `0.03125` | `0–1500` | all | Weight in the $(\mathrm{area} \times \mathrm{noise\_weight})^{1/3}$ noise-floor term of `adm_cm` / `adm_csf_den`; the default `1/32 ≈ 0.03125` is upstream's noise-floor divisor. |
| `adm_dlm_weight` | `dlmw` | double | `0.5` | `0.0–1.0` | all | Linear blend between DLM and AIM; `1.0` = DLM only, `0.0` = AIM only. |
| `adm_min_val` | `min` | double | `0.0` | `0.0–1.0` | all | Floor: fused ADM scores below it are raised to it. |
| `adm_p_norm` | `apn` | double | `3.0` | `1.0–20.0` | all | p-norm exponent of the contrast-measure finalisation ($x^{1/p}$ pooling in `adm_cm`). Note 4. |
| `adm_skip_scale0` | `ssz` | bool | `false` | — | `adm` (CPU, CUDA, SYCL, HIP, Metal); `float_adm` (CPU, SYCL, Metal) | Skip scale 0: its outputs are `0.0` and it leaves the fused score. Note 5. |
| `adm_skip_aim` | — | bool | `false` | — | `adm` (CPU, CUDA, SYCL, Metal; not HIP) | Skip the AIM sub-band calculation; forces the AIM contribution to zero. |
| `adm_bypass_cm` | `bcm` | int | `0` | `0–1` | `float_adm` (CPU, CUDA, SYCL, HIP, Metal) | Bypass contrast masking: drops the 3x3 masking threshold from the numerator, so `adm2` rises sharply. |
| `adm_adm3_apply_hm` | `aah` | bool | `false` | — | `float_adm` (CPU, CUDA, SYCL, HIP, Metal) | Combine DLM and AIM into `adm3_score` with the harmonic mean instead of the linear blend. |
| `adm_skip_aim_scale` | `sasc` | int | `-1` | `0–3` | `float_adm` (CPU, CUDA, SYCL, HIP, Metal) | Leave one scale out of the AIM sums; `-1` leaves none out. `adm2` and the scale scores are unaffected. |
| `adm_f1s0` … `adm_f1s3` | `f1s0` … `f1s3` | double | `-1.0` | `-1.0–10.0` | `float_adm` (CPU, SYCL) | Replace the CSF weight of the horizontal and vertical bands of scale 0 … 3; a negative value keeps the computed weight. |
| `adm_f2s0` … `adm_f2s3` | `f2s0` … `f2s3` | double | `-1.0` | `-1.0–10.0` | `float_adm` (CPU, SYCL) | The same for the diagonal band. |

### Notes on the option values

- **Note 1** — Non-integer values such as `1.2` are valid; see
  [Non-integer enhancement gain limits](#non-integer-enhancement-gain-limits).
- **Note 2** — Modes: `0` Watson97 (upstream-canonical), `1` Barten, `2`
  Barten/Watson
  blend, `3` Barten/Watson blend (MAE-fitted). Modes above 3 exist on
  `float_adm` only. The default model `vmaf_v1.0.16_3d0h` requests `2`.

  - Fixed-point `adm` implements modes 0–3 on CPU, CUDA, SYCL, HIP and Metal.
    It normalises finite over-range weights without changing the three bands'
    ratios, and an unsupported blend-table viewing geometry still returns
    `-EINVAL` ([Fixed-point CSF limits](#fixed-point-csf-limits)).
  - Every GPU `float_adm` twin implements mode `0` only. For model-driven
    scoring libvmaf routes a valid nonzero mode to the CPU reference before
    device initialisation and keeps unrelated features on the GPU. Naming such
    a twin explicitly keeps its direct `-EINVAL`
    ([ADR-1316](../adr/1316-gpu-option-value-capability-fallback.md),
    [ADR-1325](../adr/1325-integer-adm-barten-fixed-point-normalization.md)).
- **Note 3** — `1.0` is upstream-canonical. Modes 0, 2 and 3 ignore it on every
  backend, the CPU included. It is the `adm_csf_scale` argument of `barten_csf()`.
- **Note 6** — The integer `adm` pipeline enforces an angular-frequency floor
  of `adm_norm_view_dist × adm_ref_display_height ≥ 3240` (1080p viewed at 3H).
  Geometries below this floor are refused with a named error and `-EINVAL`;
  use `float_adm` for geometries below the floor ([Fixed-point CSF limits](#fixed-point-csf-limits)).
- **Note 7** — You rarely set it yourself: libvmaf sets it when two models
  need `adm` at two viewing distances
  ([Two viewing distances share one `adm`](#two-viewing-distances-share-one-adm)).
  Set by hand, `--feature adm=nvd=3:nvde=5` files the `nvd=5` scores under
  `integer_adm2_nvd_5`, `integer_aim_nvd_5`, `integer_adm3_nvd_5` and
  `integer_adm_scale0_nvd_5` … `integer_adm_scale3_nvd_5`, the names
  `--feature adm=nvd=5` gives them. With `debug=true` the numerators and
  denominators are the first distance's only. A second distance equal to the
  first, or one whose `%g` text names the same feature (`3.0000001` next to
  `3`), stops initialisation with `-EINVAL`.

### Notes on `adm_p_norm` and `adm_skip_scale0`

- **Note 4** — Honoured on every backend: CPU `adm` / `float_adm`, the x86 AVX2
  / AVX-512
  `adm` paths, the CUDA / SYCL / HIP / Metal `adm` twins and, since
  [ADR-1220](../adr/1220-gpu-float-adm-options-reach-kernels.md), the CUDA /
  SYCL / HIP / Metal `float_adm` twins (they previously hardcoded `p = 3` and
  applied the option to the AIM exponent alone). It applies to the numerator
  only: the CPU denominator (`adm_den_scale_finalise`) is a fixed cube root,
  and every twin mirrors that.
- **Note 5** — Up to v3.2.1 the Metal `float_adm` twin zeroed only the reported
  `adm_scale0` sub-score while still folding scale 0 into the fused score;
  fixed per ADR-1220. The SYCL `float_adm` twin takes the option since
  [ADR-1434](../adr/1434-sycl-float-adm-cpu-arithmetic.md); before,
  `--feature float_adm_sycl=adm_skip_scale0=true` was rejected as an unknown
  option.

`adm_bypass_cm` is declared and honoured by the CPU `float_adm` and by the
CUDA, Metal, SYCL and HIP `float_adm` twins per ADR-1220. The fixed-point `adm`
extractor does not declare it.

Setting `adm_csf_scale`, `adm_csf_diag_scale` and `adm_noise_weight` to their
defaults (`1.0`, `1.0`, `0.03125`) produces output bit-identical to upstream
Netflix ADM. The CUDA, SYCL and Metal `adm` twins mirror the CPU table
entry for entry (names, aliases, defaults, bounds, feature-param flags), so the
feature-name key a twin emits equals the CPU's for any options dict; the HIP
`adm_hip` twin has no `adm_skip_aim`. `adm_dlm_weight` and `adm_min_val` affect
`adm3_score` only, which the HIP twin does not emit, so there they are carried
for feature-name-key parity alone.

## Backends

| Extractor | CPU SIMD | CUDA | SYCL | HIP | Metal |
| --- | --- | --- | --- | --- | --- |
| `adm` | AVX2, AVX-512, NEON | `adm_cuda` | `adm_sycl` | `adm_hip` | `integer_adm_metal` |
| `float_adm` | AVX2, AVX-512, NEON | `float_adm_cuda` | `float_adm_sycl` | `float_adm_hip` | `float_adm_metal` |

All CUDA, SYCL and HIP twins of both extractors have an `exact_twins.d`
declaration (`adm.{cuda,sycl,hip}`, `float_adm.{cuda,sycl,hip}`), so the
parity gate compares them with tolerance 0
([generated table](../development/cross-backend-exact-twins.md)). The Metal
twins are not declared exact. The sections below give the measurements.

On the CPU, a build with Rust features also has `adm_rust`, a Rust port of
`adm` with the same scores ([Rust implementation](#rust-implementation-adm_rust)).

## Behaviour you can rely on

Each subsection states what a twin or path returns relative to the CPU
extractor, how it was measured and what it means for stored scores. Pick the
one for the extractor you run.

### Two viewing distances share one `adm`

Two models that ask for `adm` with the same options except
`adm_norm_view_dist`, such as `vmaf_v1.0.16_3d0h` and `vmaf_v1.0.16_5d0h`,
run one `adm` instead of two. The wavelet transform and the decouple stage do
not depend on the viewing distance and run once per scale; the
contrast-sensitivity, denominator and masking stages run once per distance.
The second distance's scores keep the names the second model reads
(Netflix/vmaf `cffd5b77d`, [ADR-2795](../adr/2795-adm-shared-viewing-distances.md)).

```sh
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 \
  --model version=vmaf_v1.0.16_3d0h --model version=vmaf_v1.0.16_5d0h
```

#### What this means when you use it

- **The scores do not change.** Every frame and pooled value of the two-model
  run equals the values of each model run alone, as text at `%.6f` and as
  doubles at `--precision max`: 1560 values on the 48 frames of the 576x324
  test pair, with `--threads 1` and `4`, for `adm` and for `adm_rust`
  (`VMAF_FEATURE_IMPL=rust`). `test_integer_adm_view_dist` checks the seven
  scores of both distances at 8 and 10 bits on the scalar and SIMD paths.
- **What is not shared.** A context keeps two distances at most; a third model
  at a third distance gets its own. A context whose `debug` is set, or whose
  `adm_skip_aim` differs, is not folded in, so its debug scores and its AIM
  stay its own. A model at a distance a context already evaluates second is
  absorbed, not run again.
- **Which backends.** The CPU extractor, `adm_rust` and `adm_cuda` share;
  `adm_cuda` runs the wavelet transform once and the other kernels per
  distance, and returns the CPU's bits for both (`test_cuda_adm_parity`). The
  SYCL, HIP and Metal `adm` twins still run one instance per distance until
  each takes the option.

### `float_adm` does not depend on the processor

`float_adm` gives the same scores on every processor and with every compiler
([ADR-1442](../adr/1442-float-adm-reference-divides.md)). One step of the
metric, the decouple, divides the distorted wavelet coefficient by the
reference one. Upstream Netflix forms that quotient on x86 from the
processor's reciprocal-estimate instruction (`RCPSS`) and one correction
step.

The instruction is specified by an error bound, not bit for bit: its
low bits differ between processor models, so the same pair of frames could
score differently on two x86 machines, and differently again on ARM and
under MSVC, which never used it. This fork divides.

#### What this means when you use it

- `float_adm` scores from different machines can be compared and mixed
  without a last-digit caveat.
- Against earlier releases of this fork, and against upstream Netflix on
  x86, `float_adm` scores move in the seventh decimal place. Measured on a
  Ryzen 9 9950X3D: 147 of 791 scores changed, by at most 1.3e-7 (Netflix
  576x324 at 8, 10, 12 and 16 bits, both 1920x1080 checkerboard pairs, 50
  frames of 3840x2160). The `vmaf_float_v0.6.1`, `vmaf_float_v0.6.1neg` and
  `vmaf_float_4k_v0.6.1` models, which take `float_adm` as an input, move by
  up to 1.2e-5 on a frame and 2.7e-6 on a clip's mean. Builds for ARM and
  MSVC builds are unchanged: they divided already.
- The default models (`vmaf_v0.6.1` and the other fixed-point ones) do not
  use `float_adm` and do not change.
- It is not slower. The extractor takes the same time or a little less at
  576x324, 1920x1080 and 3840x2160 with the scalar, AVX2 and AVX-512 paths.

### `float_adm` uses Netflix's contrast-sensitivity weights

The weights `float_adm` applies to the wavelet bands (the Watson model by
default, the Barten model with `adm_csf_mode=1`) are computed with Netflix's
arithmetic
([ADR-1489](../adr/1489-float-adm-barten-upstream-float.md)). Earlier
releases of this fork kept a few intermediates of those formulas in `double`
where Netflix keeps them in `float`, which changed every weight in its last
digits.

#### What this means when you use it

- The division described above is the only arithmetic difference between this
  fork's `float_adm` and upstream Netflix's on x86. Measured against a
  Netflix build that divides: every `float_adm` output with `debug=true` under
  36 option sets, and the score of every model that reads `float_adm`, is
  identical on 658 frames (Netflix 576x324 at 8, 10, 12 and 16 bits,
  1920x1080 and 3840x2160 clips, 4:2:2, 4:4:4 and 4:0:0 input, small frames
  down to 17x17), with the scalar, AVX2 and AVX-512 paths.
- Against earlier releases of this fork, `float_adm` scores move in the
  seventh decimal place: `adm2` by at most 1.1e-7, a per-scale score by at
  most 2.7e-7. The `vmaf_float_v0.6.1`, `vmaf_float_v0.6.1neg`,
  `vmaf_float_4k_v0.6.1` and `vmaf_v0.6.0` models move by up to 2.7e-5 on a
  frame and 1.5e-5 on a clip's mean.
- Fixed-point `adm` with `adm_csf_mode=1` reads the same Barten weights and
  moves by at most 1.6e-7 (`integer_adm2`). In its default mode it does not
  change, and neither do the default models (`vmaf_v0.6.1` and the other
  fixed-point ones).
- The GPU twins of `float_adm` and `adm` take the weights from the same
  routines and return the CPU's scores as before.

### `float_adm` uses AVX2 and AVX-512, with the same scores

On x86 `float_adm` runs its wavelet and its contrast-sensitivity stage through
AVX2 or AVX-512 kernels when the processor has them
([ADR-1473](../adr/1473-float-adm-x86-simd-exact-and-dispatched.md)); on
64-bit ARM the wavelet runs through NEON. Every kernel returns the bits of the
scalar code, so the instruction set does not change a score.

#### What this means when you use it

- You do not select anything. `--cpumask` restricts the instruction sets as
  for every other extractor: `--cpumask 63` forces the scalar code, `48`
  allows up to AVX2, `0` (the default) allows everything.
- Scores are the same on all three settings and the same as before the kernels
  were enabled: every `float_adm` output with `debug=true` under 13 option
  sets, and the `vmaf_float_v0.6.1` score, on the Netflix 576x324 pair at 8,
  10, 12 and 16 bits, both 1920x1080 checkerboard pairs, 3840x2160 and 14
  small frame sizes down to 17x17.
- It is faster. One thread, whole run of `vmaf --feature float_adm`, on a
  Ryzen 9 9950X3D:

  | Frame | scalar | AVX2 | AVX-512 |
  | --- | --- | --- | --- |
  | 576x324 | 1.45 ms | 1.17 ms | 1.11 ms |
  | 1920x1080 | 17.9 ms | 15.2 ms | 15.1 ms |
  | 3840x2160 | 76.2 ms | 62.4 ms | 62.9 ms |

- The rest of the extractor (the decouple, the denominator and the contrast
  masking) is scalar on every processor. Those stages add `float` values in a
  fixed order that the scores depend on.
- A frame that contains NaN is refused on every setting, as before.

### `float_adm` on CUDA returns the CPU's values

`float_adm_cuda` gives the same number as `--backend cpu --feature float_adm`
for every output of every frame, down to the last bit of the `--precision max`
output ([ADR-1420](../adr/1420-cuda-float-adm-cpu-arithmetic.md)). Measured on
an RTX 4090 on the Netflix 576x324 pair at 8, 10, 12 and 16 bits, both
1920x1080 checkerboard pairs and 3840x2160, with `debug=true` as well.

#### What this means when you use it

- You can mix CPU and CUDA `float_adm` results in one data set. Before
  ADR-1420 the CUDA twin was up to 1.3e-5 from the CPU (the fifth decimal
  place of `adm_scale0` could differ); `float_adm_cuda` outputs stored before
  it differ from new ones by that much.
- For content with almost no reference detail, scored with
  `adm_noise_weight=0`, the twin used to report `adm2 = 1` where the CPU
  reports 0. It now reports the CPU's value.
- "The CPU" is any CPU. See
  [`float_adm` does not depend on the processor](#float_adm-does-not-depend-on-the-processor)
  below: the twin divides as the CPU extractor does, and no longer measures
  anything of the host when it starts.
- One option is not identical: with `adm_p_norm` other than 1 or 3 the twin
  is within 1.1e-7 of the CPU, because the two sides raise each term with
  different `powf` implementations.
- Frames smaller than 17x17 are refused by both; see the next section.

### `float_adm` on SYCL returns the CPU's values

`float_adm_sycl` gives the same number as `--backend cpu --feature float_adm`
for every output of every frame, down to the last bit of the `--precision max`
output ([ADR-1434](../adr/1434-sycl-float-adm-cpu-arithmetic.md)). Measured on
an Arc A380 on the Netflix 576x324 pair at 8, 10, 12 and 16 bits, both
1920x1080 checkerboard pairs and 200 frames of 3840x2160, with `debug=true`
as well. A 3840x2160 frame takes 12.3 ms, 15.1 ms before.

#### What this means when you use it

- You can mix CPU and SYCL `float_adm` results in one data set. Before
  ADR-1434 the SYCL twin was up to 1.7e-5 from the CPU (the fifth decimal
  place of a scale score could differ); `float_adm_sycl` outputs stored
  before it differ from new ones by that much.
- For content with almost no reference detail, scored with
  `adm_noise_weight=0`, the twin used to report `adm2 = 1` where the CPU
  reports 0. It now reports the CPU's value.
- The twin takes four options it rejected before: `adm_skip_scale0`,
  `adm_skip_aim_scale`, `adm_f1s0` … `adm_f1s3` and `adm_f2s0` … `adm_f2s3`
  (see the table above). With each of them it returns the CPU's values.
- "The CPU" is the CPU extractor of the same build. The final roots are the
  math library's `powf`: a libvmaf built with GCC and one built with the
  Intel compiler used to differ in the last digit of `aim` and `adm3` on a
  few frames (1.6e-9 on 2 of 200 frames measured, 7.5e-8 with a CSF weight
  override), CPU extractor against CPU extractor. Since
  [ADR-1495](../adr/1495-icx-system-libm.md) an Intel compiler build calls
  glibc's `powf` and the two builds agree. The processor does not matter; see
  [`float_adm` does not depend on the processor](#float_adm-does-not-depend-on-the-processor).
- One option is not identical: with `adm_p_norm` other than 1 or 3 the twin
  is within 1.8e-7 of the CPU, because the two sides raise each term with
  different `powf` implementations.
- With `debug=true` the ratio is filed under the same unsuffixed key `adm` on
  the CPU and on every twin, whatever other option is set; every other output
  carries the option suffix on both (for example `adm_num_egl_1.2`).
- Frames smaller than 17x17 are refused by both; see the next section.
- The twin uses 48 MB more device memory at 3840x2160.

### `float_adm` on HIP returns the CPU's values

`float_adm_hip` returns the CPU extractor's scores bit for bit since
[ADR-1458](../adr/1458-hip-float-adm-cpu-arithmetic.md). It runs the CUDA
twin's arithmetic from the same source file. Measured on a gfx1036
(ROCm 7.2.4) at `--precision max` against `--backend cpu`: 1246 of 1246
scores identical on 178 frames (576x324 at 8, 10, 12 and 16 bits and as
10-bit 4:2:2, 1920x1080, 3840x2160, full-range noise), 3204 of 3204 outputs
with `debug=true`, and every score again with `adm_enhn_gain_limit=1.2`,
`adm_bypass_cm=1`, `adm_skip_aim_scale=1`, `adm_norm_view_dist=1.5` and
`adm_p_norm=1`.

#### What this means for you

- You can mix CPU and HIP `float_adm` results in one data set. Before
  ADR-1458 the HIP twin was up to 1.3e-5 from the CPU; `float_adm_hip`
  outputs stored before it differ from new ones by that much.
- The twin takes `adm_skip_aim_scale`, which it rejected before. It does not
  take `adm_skip_scale0` or the per-scale weight overrides `adm_f1s0` …
  `adm_f2s3`; with one of those set, use the CPU extractor.
- One option is not identical: with `adm_p_norm` other than 1 or 3 the twin
  is within 1.5e-7 of the CPU, because the two sides raise each term with
  different `powf` implementations.
- Frames smaller than 17x17 are refused by both; see the next section.
- The twin uses 48 MB more device memory at 3840x2160. A frame takes no
  longer than before: 13 ms at 1920x1080 and 68 ms at 3840x2160 on a
  gfx1036.

The Metal `float_adm` twin agrees with the CPU to four decimal places.

### Small frames

The fixed-point `adm` extractor needs at least 17x17 pixels, on every
backend. Each of its four DWT levels halves the band, and below 17 pixels the
coarsest level has a single sample. Smaller input fails with `-EINVAL` when
the extractor starts. The CPU, CUDA, HIP and SYCL extractors also log an error
that names them:

```text
libvmaf ERROR integer_adm requires width >= 17 and height >= 17 (got 16x16)
libvmaf ERROR adm_cuda requires width >= 17 and height >= 17 (got 16x16)
```

The CUDA, HIP and SYCL twins (`adm_cuda`, `adm_hip`, `adm_sycl`) used to
accept such frames; they now refuse them like the CPU and Metal extractors.

`float_adm`, `float_adm_cuda`, `float_adm_sycl` and `float_adm_hip` refuse the
same frames, with the same kind of message:

```text
libvmaf ERROR float_adm requires width >= 17 and height >= 17 (got 16x16)
libvmaf ERROR float_adm_cuda requires width >= 17 and height >= 17 (got 16x16)
libvmaf ERROR float_adm_sycl requires width >= 17 and height >= 17 (got 16x16)
libvmaf ERROR float_adm_hip requires width >= 17 and height >= 17 (got 16x16)
```

They used to accept them. Below 17x17 the coarsest level has a single sample
and the CPU extractor read outside it: before the start of a buffer at 8
pixels or fewer, the wrong sample from 9 to 16 (a random 8x8 pair scored
`adm_scale3 = 1.05`). Scores of such frames from an older build are not
meaningful. `float_adm_hip` checks the size first in `init`, before it claims
any device
resource. Only the Metal `float_adm` twin has no check and still accepts these
frames; use at least 17x17 with it.

#### Frames of 17 to 32 pixels

Frames from 17 to 32 pixels wide or high are the smallest it accepts. These
were wrong at those sizes and are fixed:

- Scale 3 read outside its band, so `integer_adm_scale3` could change between
  two runs on the same input.
- For frames 17 to 32 pixels wide, the AVX-512 path scored scale 0 up to 0.01
  away from the scalar path.
- For the same widths, the CUDA and HIP twins scored scale 0 up to 0.2 away
  from the CPU, and 32x32 frames came out as NaN.

The scalar, AVX2, AVX-512 and NEON paths now give identical scores at every
width in that range. The CUDA, HIP and SYCL twins agree with them within the
1e-4 cross-backend tolerance, as they do on larger frames.

#### Large band coefficients on SYCL

The SYCL twin also used to drift on content with very large band
coefficients, such as independent random noise, where the CPU's 16-bit
intermediates wrap: `integer_adm_scale0` came out up to 2.1e-4 away from the
CPU. It now wraps at the same points and rounds its scale 1-3 terms the way
the CPU, CUDA and HIP do, which moves SYCL's ordinary-video scores by less
than 1e-6, toward the CPU. One of those intermediates, the centre term of the
masking threshold, no longer wraps on any backend; see the next section.

### AIM above 1

`adm` (fixed point) and `float_adm` report different AIM values on content
whose additive impairment is larger than the reference's own detail: the
fixed-point value goes above 1 and the float value stops at 1.

Both are what upstream Netflix/vmaf defines. AIM is the additive impairment
that survives contrast masking divided by the DLM denominator, which measures
the detail of the reference. Upstream's float extractor then clips the ratio
at 1 (`MIN(aim_num / aim_den, 1.0f)` in `adm.c`); its fixed-point extractor
does not (`aim_num / den` in `integer_adm.c`). The fork keeps both as they
are ([ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md)).

A reference without detail shows it most clearly, because its denominator is
the noise-floor term alone:

| Picture, flat grey reference | `integer_aim` | upstream master prints | `float_adm` `aim` |
|---|---|---|---|
| 64x64, 4x2 patches `255 0 0 0` every 16 pixels | 3.1755853876204676 | 3.175585 | 1 |
| 576x324, the same patches | 2.602848185401339 | 2.602848 | 1 |
| 576x324, uniform noise of ±24 | 1.2616298263467636 | 1.261630 | 1 |

Stage by stage the two extractors agree before the clip: on the 64x64 picture
the summed AIM numerators are 64.66445 (fixed point) and 64.66458 (float) over
the same denominator of 20.363002.

#### What this means in practice

- `adm3_score` follows. With the default model's `adm_dlm_weight=0.7` and
  `adm_min_val=0.5`, the 64x64 picture gives a fixed-point `adm3` of 0.5 (the
  floor) and a float `adm3` of 0.7.
- The shipped `vmaf_v1.0.16` models read the fixed-point feature, and the fork
  returns what upstream libvmaf returns for it: on the 576x324 patch picture
  both report `integer_adm3` 0.5 and a VMAF of 0 with `vmaf_v1.0.16_3d0h`.
- When you threshold or plot `integer_aim` yourself, do not assume it stays
  within `[0, 1]`. Clip it at 1 if you want the float extractor's convention.
- Ordinary encodes stay far below 1: the Netflix 576x324 pair has a mean
  `integer_aim` of 0.0266.

### Isolated impairments and very large coefficients

Since ADR-1402 the fixed-point `adm` extractor cannot score above 1 because
of its masking threshold, and it differs from upstream Netflix/vmaf master on
one kind of content.

Scale 0 masks every coefficient with a threshold built from its eight
neighbours and from 1/15 of the coefficient itself. Upstream stores that last
term in 16 bits, so it wraps negative once the coefficient reaches 15360
(8-bit content gets there with a one-pixel-wide full-contrast edge next to
flat picture). A negative threshold adds contrast instead of masking it. On a
flat grey reference against the same picture with a few isolated patches, the
extractor used to report more detail in the distorted picture than in the
reference:

| Picture (flat grey reference, patches `255 0 0 0`, two rows high) | Before | Now | `float_adm` |
|---|---|---|---|
| 64x64, a patch every 16 pixels: `integer_adm_scale0` | 1.0829225419556654 | 1 | 1 |
| the same: `integer_adm2` | 1.035481944668303 | 1 | 1 |
| 24x24, one patch at (3, 3): `integer_adm_scale0` | 1.0701309766616138 | 1 | 1 |
| the same: `integer_adm2` | 1.0301034698295983 | 1 | 1 |

The term is 32 bits wide now, on every backend: the scalar path, AVX2,
AVX-512, and the `adm_cuda`, `adm_hip`, `adm_sycl` and `integer_adm_metal`
twins. The change is the second revision of Netflix/vmaf pull request 1602,
which upstream has not merged.

#### What this means for scores

- Ordinary video is unaffected. The three Netflix reference pairs and the
  other ten fixture pairs under `python/test/resource/yuv/` give identical
  output
  at `--precision max`, on the CPU and on the CUDA, HIP and SYCL twins.
- Content with scale-0 coefficients of 15360 or more moves. Independent
  full-range noise at 576x324 (reference and distorted unrelated):
  `integer_adm2` 0.38954843646923215 to 0.38950305912838107,
  `integer_adm_scale0` 0.4549724278265123 to 0.45480076976959144. Noise
  against the same noise plus a small perturbation: `integer_aim`
  7.166752298663627e-05 to 0, `integer_adm3` 0.9771812019019719 to
  0.9772170356634652. The default model's own ADM features and its VMAF score
  did not change on either.
- On such content the fork's `adm` no longer equals upstream master's, until
  upstream merges the pull request. Results from the fork's own earlier
  releases differ by the same amounts.

!!! warning
    The Metal twin was changed in source only; it has not been run on a device.

#### Reproduce the old behaviour

To see the old behaviour, compare with a build of the fork before ADR-1402:

```bash
vmaf -r flat_64x64.yuv -d patches_64x64.yuv -w 64 -h 64 -p 420 -b 8 \
     --feature adm --feature float_adm --no_prediction --precision max --json
```

`core/test/test_integer_adm_cm_threshold.c` draws both pictures.

### Non-integer enhancement gain limits

`adm_enhn_gain_limit` accepts any value from 1 to 100. Since ADR-1413 the
fixed-point `adm` score for a non-integer limit such as `1.2` no longer depends
on which code path computes it: the scalar path, AVX2, AVX-512 and the SYCL
twin give the same output bit for bit.

Nothing changes at the limits the shipped models use (1 and 100), on any path.

The limited sample is the product of an integer coefficient and the limit,
formed in double precision and truncated toward zero. The AVX2 and AVX-512
kernels used to round that product to nearest, and the SYCL twin formed it in
fixed point; each was one off in a share of the limited samples. Worst frame
of `integer_adm_scale0` against the scalar path, before the change:

| Input | AVX2, limit 1.2 | AVX-512, limit 1.2 | SYCL (Arc A380), limit 1.2 |
|---|---|---|---|
| Netflix pair `src01` 576x324 | 1.23e-6 | 1.15e-6 | 1.23e-6 |
| `akiyo` 352x288 | 6.23e-6 | 6.23e-6 | 7.28e-6 |
| Blurred blocks 576x324 | 3.56e-5 | 3.44e-5 | 3.60e-5 |
| Big Buck Bunny 3840x2160 | 1.99e-6 | 1.99e-6 | 2.31e-6 |

On x86 the result of a run with a non-integer limit therefore moves by up to
these amounts against earlier builds of the fork and against upstream master,
whose vector kernels round in the same way.

The other device twins:

- `adm_cuda` and `adm_hip` always truncated and are unchanged.
- `integer_adm_metal` multiplies in single precision and can still differ from
  the CPU under a non-integer limit
  (`T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01` in [state](../state.md)).

To compare two paths yourself:

```bash
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --no_prediction \
     --feature 'adm=adm_enhn_gain_limit=1.2' --precision max --json -o simd.json
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --no_prediction \
     --feature 'adm=adm_enhn_gain_limit=1.2' --precision max --json -o scalar.json \
     --cpumask 4294967295
```

### `adm` on CUDA returns the CPU's values

`adm_cuda` gives the same number as `--backend cpu --feature adm` for every
output of every frame, down to the last bit of the `--precision max` output
([ADR-1416](../adr/1416-cuda-adm-cpu-row-rounding.md)). Its CSF weights, its
rounding shifts and the conversion of the integer sums into scores are the
CPU extractor's own routines, and its denominator is rounded once per row as
on the CPU. Measured on an RTX 4090 on the Netflix 576x324 pair at 8, 10, 12
and 16 bits, both 1920x1080 checkerboard pairs and 3840x2160, with
`debug=true` and with every option the extractor has.

#### What this means when you use it

- You can mix CPU and CUDA `adm` results in one data set. Before ADR-1416
  the CUDA twin was up to 2.1e-7 from the CPU; `adm_cuda` outputs stored
  before it differ from new ones by that much.
- On a few unusual frame sizes the old twin was wrong, not merely imprecise:
  where the scale-0 region inside the ADM border has an area just above a
  power of two (81 areas up to $2^{26}$, for example 962x13542), it reported
  `integer_adm_scale0` up to 0.12 too low (0.860 instead of 0.979) and
  `adm2` 0.014 too low. Re-score such material.
- With `adm_skip_scale0=true` the debug output `integer_adm_den_scale0` is
  now the CPU's `1.00000001335e-10` and it is part of `integer_adm_den`.
- The SYCL twin is bit-identical as well (ADR-1362), and so is the HIP twin
  (ADR-1423). The Metal twin has not been measured on a device.

Check it on your own device:

```shell
python3 scripts/dev/speed_gpu_parity.py --backend cuda \
     --vmaf "$PWD/build/tools/vmaf" --feature adm
```

It prints, per output, how many frames are bit-identical and the largest
difference, and exits 0 only when every frame is.

### Fixed-point CSF limits

The fixed-point `adm` extractor stores each scale's CSF weight as an integer:
`uint16_t` at scale 0 (horizontal/vertical bands scaled by $2^{21}$, the diagonal
band by $2^{23}$) and `uint32_t` at scales 1-3 (scaled by $2^{32}$). Those budgets
were sized for Watson97 weights near `1e-2`. Full-scale Barten weights are
about 1.21 at scale 0 and 26.98 at scale 3, so direct narrowing used to wrap
and publish NaN or near-zero scores. ADR-1191 first made that failure explicit;
[ADR-1325](../adr/1325-integer-adm-barten-fixed-point-normalization.md) makes
finite weights computable instead.

#### Normalisation

For each scale, `adm` chooses the smallest non-negative power-of-two exponent
`k` that puts all three fixed-point bands inside their arithmetic budget:

- scale 0: the horizontal and vertical weights stay below 43900, the
  diagonal weight below $2^{16}$;
- scales 1, 2 and 3: every weight stays below 279958309, 539893111 and
  546406567.

#### Why the limits are not storage limits

The limits are not storage limits. Contrast masking squares the weighted
wavelet coefficient and keeps the square in 32 bits, so a weight is admitted
only when the largest coefficient the wavelet can produce at that scale still
has a square that fits. The largest coefficient follows from the filter taps
and holds for every picture and bit depth
([ADR-1472](../adr/1472-integer-adm-cm-weight-budget.md)).

At scale 0 one more stage binds the horizontal and vertical weight first
([ADR-1917](../adr/1917-integer-adm-scale0-weight-limit-csf-magnitude.md)).
The CSF stage keeps the 1/30 magnitude of the weighted band in 16 bits, and
for the largest band the integer wavelet produces (22930) that holds only
below a weight of 43900; the square would allow 46603.4. Between the two, the
magnitude wrapped negative in the scalar code and on the GPUs and saturated
in AVX2, and a 31-32 pixel wide masking row could pass $2^{64}$. A weight in that
range now takes one more halving. Watson97 at every viewing geometry, the
default Barten configuration and both blend modes stay below 43900 and keep
their bits; a Barten configuration that lands in the range (for example
`adm_csf_scale=1.16:adm_csf_diag_scale=0.3`) moves by about 1e-6.

With the earlier limit of $2^{30}$ a high-contrast picture could wrap that
square in
Barten mode:
`adm=adm_csf_mode=1` failed every frame of the 10 px checkerboard with a NaN
numerator and returned `integer_adm2` 0.587 instead of 0.784 on the 1 px
checkerboard. Barten-mode scores that were already right move in the seventh
decimal place (the weights lose one or two bits); Watson97 and both blend
modes are unchanged.

#### Effect on the three bands

All three bands use the same `k`, so their horizontal, vertical, and diagonal
CSF ratios do not change. Contrast masking cubes the normalized values; its
host finalizer restores `3k`, while the denominator continues to use the
original floating-point factors. Configurations with `k=0` retain their old
fixed-point values and SIMD path. A normalized CPU configuration keeps SIMD
DWT, decoupling, and denominator work but uses scalar weighted-CSF and
contrast-masking stages until the later performance phase.

#### Errors and viewing geometry

Negative or non-finite factors remain errors. In particular, modes 2 and 3
use lookup tables for specific viewing geometries; an unsupported pair such
as `adm_ref_display_height=1200` at the default viewing distance returns a
negative sentinel and is rejected with `-EINVAL` rather than converted to an
unsigned weight.

Independently of the selected CSF mode, the fixed-point pipeline requires
`adm_norm_view_dist × adm_ref_display_height >= 3240` (the default 1080p
display viewed at 3H). Lower angular-frequency geometry is rejected with
`-EINVAL` before score computation; GPU backends reject it before normalization
or device allocation. For example, mode 1 rejects 1080p at 0.75H, and mode 2
rejects its otherwise-tabulated 720p-at-3H geometry. CPU, CUDA, SYCL, HIP, and
Metal enforce the same floor.

The CPU, CUDA, SYCL, HIP, and Metal integer extractors share this conversion
contract. The float extractor has no fixed-point storage and remains the
numerical reference. On the canonical 576x324 pair, full-scale mode 1 emits
finite `adm2`, `aim`, `adm3`, and all four scale scores; the largest pooled
absolute difference from `float_adm` is `2.7e-5`.

## SIMD paths and portability

The `adm` SIMD paths (AVX2, AVX-512, NEON) produce the same scores as the
scalar path bit for bit, on any content. Earlier releases of the AVX2 and
AVX-512 paths differed from scalar by up to 7e-4 on content with very large
band coefficients, such as full-range noise; ordinary video, including the
Netflix reference clips, was already identical.

On aarch64 the NEON path covers the 8-bit DWT (frame widths divisible by 8)
and the scale-zero decouple; the other stages run the scalar kernels. The
decouple uses vectors for integral `adm_enhn_gain_limit` values and the scalar
kernel for fractional ones, and returns the scalar's bits for both.

**32-bit x86** — the fork is 64-bit only (ADR-1258). The ADM x86
sources still extract 64-bit lanes through 32-bit-safe helpers
(`extract_epi64()`, `extract_epi64_128()`), ported from upstream Netflix
commits [`8a289703`](https://github.com/Netflix/vmaf/commit/8a289703) and
[`1b6c3886`](https://github.com/Netflix/vmaf/commit/1b6c3886) and completed
in ADR-1258, so they compile for 32-bit x86. Nothing builds or tests 32-bit in
CI, so that is portability hygiene, not a supported configuration.

## Rust implementation (`adm_rust`)

A build configured with `-Denable_rust_features=true` also contains `adm_rust`,
a Rust port of the fixed-point `adm` extractor
(`core/src/rust/feature/adm/`). It reads the options of `adm` from the C
option table, so every option above works, with the same defaults, aliases and
ranges. It emits the same features under the same names (`adm2`, `aim`,
`adm3`, the four scale scores, and with `debug=true` the numerators and
denominators) and returns the C extractor's values bit for bit.

Run it:

- `VMAF_FEATURE_IMPL=rust` replaces `adm` by `adm_rust` wherever `adm` is
  requested, by `--feature adm` or by a model such as `vmaf_v1.0.16`. The log
  line `feature extractor adm: Rust implementation adm_rust` and the
  `feature_backends` entry of the JSON output name what ran.
- `--feature adm_rust` runs the Rust extractor directly, with or without the
  variable.
- Without the variable, or with `VMAF_FEATURE_IMPL=c`, the C extractor runs.

The build option, the selection rules and the fallback are in the
[Rust extractor framework](../development/rust-extractor-framework.md#build-and-run-the-rust-path)
guide.

The Rust extractor refuses what `adm` refuses, at the same point: frames below
17x17 at initialisation, a viewing geometry below 1080p at 3H, and a blended
CSF (`adm_csf_mode` 2 or 3) at a geometry it has no table for, on every frame.
It reads 8-, 10-, 12- and 16-bit input, Y plane only.

**What "bit for bit" was measured on.** `scripts/ci/rust_twin_diff.py
--feature adm` runs the same `vmaf` binary once with each implementation at
`--precision max` and compares every metric of every frame as IEEE doubles.
Zero differences on the Netflix 576x324 pair (48 frames), both 1080p
checkerboard pairs, the 10-bit 480x270 `sparks` pair and the 3840x2160 `bbb`
pair (200 frames), for the default options and for the four option sets of the
`vmaf_v1.0.16` models (`adm_csf_mode=2` at 1.5H and 3H for 2160 lines, 3H and
5H for 1080 lines). The options the Netflix golden tests set (Barten CSF,
`adm_skip_aim`, `adm_skip_scale0`, `adm_p_norm`, `debug`, fractional
`adm_enhn_gain_limit`) were compared on the 576x324 pair with the same result.
The Netflix golden tests (the test list of `make test-netflix-golden`) pass
unchanged with `VMAF_FEATURE_IMPL=rust` set.

**Speed.** The Rust extractor is scalar and runs as fast as the C extractor's
scalar path; the C extractor is faster where it can use its AVX2 or AVX-512
kernels. Processor time of a single-threaded `vmaf --feature adm
--no_prediction` run per frame, default options, median of three runs, on an
AMD Ryzen 9 9950X3D (AVX-512) under load:

| Input | `adm`, AVX-512 | `adm`, scalar (`--cpumask 4294967295`) | `adm_rust` |
| --- | --- | --- | --- |
| 576x324, 48 frames | 0.6 ms | 3.4 ms | 3.2 ms |
| 3840x2160, 50 frames | 30.7 ms | 128.4 ms | 133.7 ms |

The same runs with `--feature null` take 0.1 ms and 4.2 ms per frame, the cost
of reading the input.

## Reference

Li S., Zhang F., Ma L., Ngan K., "Image Quality Assessment by Separately
Evaluating Detail Losses and Additive Impairments," IEEE Transactions on
Multimedia 13(5):935–949, 2011.
