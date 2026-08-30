# BRISQUE Extractor

BRISQUE (Blind/Referenceless Image Spatial Quality Evaluator) is a
**no-reference**, opinion-aware blind image-quality metric (Mittal, Moorthy &
Bovik, *IEEE Transactions on Image Processing* 21(12):4695-4708, 2012). It
scores a single picture from its spatial natural-scene statistics (NSS) and a
trained support-vector regressor (SVR), without a reference frame. The fork
ships it as a libvmaf feature extractor named `brisque`.

Because it is a no-reference metric, BRISQUE scores **only the distorted
picture**; the reference frame and the 90°-rotated inputs are ignored, mirroring
the CAMBI / NIQE no-reference posture. Lower score means better perceptual
quality.

## Output

| Field | Value |
| --- | --- |
| Feature name | `brisque` |
| Output metric | `brisque` |
| Direction | **Lower is better** (≈ 0 pristine, rising toward ~100 for heavy distortion) |
| Range | unbounded; **no output clamp** — mild negative values on very clean content are expected (matches the reference; OpenCV's variant clamps to `[0, 100]` and is a different model) |
| Reference frame | Ignored (no-reference metric) |
| Model | `model/other_models/brisque_live.model` (libsvm EPSILON_SVR; **embedded into the binary at build time** via an `xxd -i` Meson `custom_target`, the same mechanism libvmaf's JSON models use — see [Model loading](#model-loading)) |
| Snapshot | `testdata/scores_cpu_brisque.json` (fork-added, not Netflix golden) |

## Usage

```bash
vmaf \
    --reference dist.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature brisque \
    --output score.json
```

BRISQUE ignores the reference, so you may pass the same file for `--reference`
and `--distorted` (or any reference of matching dimensions). The per-frame JSON
metric key is `brisque`; pooled values appear under the same key in
`pooled_metrics`.

### Options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `model_path` | string | *(unset)* | Path to an on-disk libsvm EPSILON_SVR model that overrides the build-time-embedded LIVE allmodel. **Required** when libvmaf was built with `built_in_models` disabled (otherwise `init()` fails with `-EINVAL` and a log line). |

Pass it like any feature option (`=` separates the feature name from its
options, `:` separates options):

```bash
vmaf … --feature brisque=model_path=/path/to/brisque_live.model …
```

## Model loading

The bundled LIVE-lab `allmodel` is **embedded into the libvmaf binary at build
time**. `core/src/meson.build` runs an `xxd -i` `custom_target` over
`model/other_models/brisque_live.model` (the ~343 KB native libsvm text model)
to generate `brisque_live.model.c` in the build directory; that translation
unit defines the `src_brisque_live_model[]` / `src_brisque_live_model_len`
symbols declared in `core/src/feature/brisque_model.h`. This is the identical
pattern libvmaf uses to bake in its own SVR JSON models, so BRISQUE carries no
runtime model-file dependency and nothing needs to be installed alongside the
binary.

At `init()` the extractor loads the model in this order:

1. If the `model_path` option is set, `svm_load_model(model_path)` reads that
   on-disk file (used to override the bundled model, or as the only loader when
   `built_in_models` is disabled).
2. Otherwise the embedded buffer is parsed with
   `svm_parse_model_from_buffer(src_brisque_live_model, src_brisque_live_model_len)`.

A missing/invalid path, or a build with neither the embed nor a `model_path`,
fails `init()` with `-EINVAL` and a descriptive log line.

The model bytes are **not** committed as a C source file: the byte array
expands to ~2.1 MB, which exceeds the repository's 1 MB large-file gate. Only
the ~343 KB binary model and the tiny declaration header live in the tree; the
big array exists solely in the build directory.

## Algorithm

Per frame, on the distorted luma plane only (all double precision), replicating
the gregfreeman MATLAB pipeline that **trained the model** (`brisque_feature.m`):

1. Read the luma into a `[0, 255]` double working buffer (8-bit copied directly;
   higher bit depths scaled `value * 255 / (2^bpc - 1)`).
2. For each of two scales (full resolution, then ½-resolution):
   1. **MSCN** — `mu = filter2(gauss7x7, img)`, `sigma = sqrt(|filter2(gauss7x7,
      img²) − mu²|)`, `mscn = (img − mu) / (sigma + 1)`. The 7×7 Gaussian uses
      `sigma = 7/6` (separable, unit-volume, zero boundary — MATLAB
      `filter2('same')`).
   2. **GGD fit** of the MSCN field → push `{alpha, sigma²}` (2 features).
   3. **AGGD fit** of each of the 4 paired products (shifts
      `(0,1),(1,0),(1,1),(-1,1)`, wraparound roll) → push
      `{alpha, eta, leftstd², rightstd²}` (16 features).
   4. Downsample `img` by ½ (MATLAB antialiased bicubic, Catmull-Rom a=-0.5).
3. The 36-D vector is `[scale1 f1..f18, scale2 f1..f18]`.
4. **Range-scale** each feature to `[-1, 1]` via `xs = -1 + 2·(f-min)/(max-min)`
   (no clamp).
5. **Predict**: `score = svm_predict(allmodel, x)` over the embedded EPSILON_SVR
   RBF model (`gamma 0.05`, `total_sv 770`, `rho -155.845`).

The gamma-ratio tables for the GGD/AGGD shape fits are precomputed once at
`init()` over the grid `gam = 0.2 : 0.001 : 10` (9801 entries):
`r_ggd(g) = Γ(1/g)Γ(3/g)/Γ(2/g)²`, `r_aggd(g) = Γ(2/g)²/(Γ(1/g)Γ(3/g))`.

### Load-bearing fidelity choices

The fork follows the **MATLAB pipeline that trained the model**, NOT the
widely-copied krshrimali C++ port, which diverges on three points the model was
never trained with:

- **GGD (not AGGD) for the MSCN field** (features f1, f2). krshrimali fits the
  MSCN field with AGGD — a bug versus both the paper (Table I) and the trained
  model. The model expects GGD.
- **Gaussian `sigma = 7/6`** (not the truncated `1.166` in the C++ port).
- **MATLAB antialiased bicubic** ½-downscale (not OpenCV `INTER_CUBIC`).

### Range arrays

The `min_[36]` / `max_[36]` range-normalization arrays are baked into
`core/src/feature/brisque.c` and come from the **inline** arrays in the
reference `computescore.cpp` (the array the trained model expects, and the only
one the reference prediction path reads). They are **not** the separate
`allrange` file in the same upstream repo, which is a different,
prediction-inconsistent set the code never reads — substituting it would corrupt
every score (an RBF `gamma=0.05` perturbation over a 0.71 scaled-space shift is
score-destroying, not a 1e-4 nudge). See ADR-1115.

## Inputs and backends

- **Bit depth**: 8 / 10 / 12 / 16, scaled to the `[0, 255]` SDR working range.
- **Pixel formats**: all (luma-only; chroma is ignored, so 400P is fine).
- **Backends**: CPU scalar only. GPU twins (CUDA/SYCL/HIP) are out of scope; a
  future twin must keep the gamma argmin and RBF kernel sum in fp64 and is not
  expected to be bit-exact (per the fork's GPU-parity posture).

## Limitations

- **SDR luma only.** BRISQUE is trained on 8-bit SDR LIVE images. PQ / HLG HDR
  is **out of scope** — no HDR-trained BRISQUE model exists. On >8-bpc input the
  extractor emits a one-time warning and scores the content as SDR; the score is
  not meaningful for HDR transfer characteristics.
- **Near-zero AGGD sign sensitivity.** The AGGD paired-product fit buckets
  samples by a strict sign cutoff (`x<0` / `x>0`, zeros excluded). On
  heavily-compressed near-flat content a large fraction of paired products sit
  within ~1e-11 of zero, so the bucket assignment — and hence `eta` and the
  score — is sensitive to floating-point summation order (~0.1 score units).
  This is inherent to BRISQUE. Correctness is validated on stable natural
  content (cameraman: C extractor `-13.70844` vs an independent MATLAB-faithful
  oracle `-13.70840`, ~5e-5); the in-tree fixture snapshot
  (`testdata/scores_cpu_brisque.json`) is the extractor's own deterministic
  output rather than a places=4 cross-assert.
- Minimum frame size: width and height each ≥ 7 (the 7×7 window must fit).

## Updating the model and regenerating the score snapshot

There is no header to regenerate: the model is embedded straight from the
vendored binary at build time. To swap in a different/retrained model, replace
`model/other_models/brisque_live.model` (keep the libsvm text format) and
rebuild — Meson re-runs `xxd` automatically. Update the model card
(`model/brisque_live_card.md`), the NOTICE (`model/other_models/NOTICE-brisque`),
and the score snapshot below to match.

To refresh the end-to-end snapshot after an intentional kernel change, run the
extractor on frame 12 of the fixture and record the score:

```bash
core/build-cpu/tools/vmaf \
    --reference testdata/dis_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --feature brisque --json --output /tmp/brisque.json
# update testdata/scores_cpu_brisque.json "score" and core/test/test_brisque.c
```

## Correctness test

`core/test/test_brisque.c` (meson `test_brisque`, suite `fast`) asserts:

- gamma-table anchors `GGD(2)=1.5707963`, `AGGD(1)=0.5`, `AGGD(2)=2/π`;
- the 7×7 Gaussian window (unit sum, symmetric);
- GGD / AGGD fits of `[-2,-1,0,1,2,3]` (with zeros excluded from the AGGD
  buckets — `right² = 14/3`, not the NIQE zero-bucketing);
- a symmetric AGGD giving `eta == 0` exactly and an all-zero flat-patch NaN
  guard;
- the MATLAB-imresize bicubic coefficients (normalized to 1, odd-dimension
  safe);
- the range-scale anchors (min→-1, max→+1, mid→0);
- an **end-to-end** score snapshot through the public extractor-context API;
- an **odd-dimension** regression (577×325) asserting a finite score and no
  bicubic-table overflow.

## See also

- [ADR-1115](../adr/1115-brisque-nr-metric.md) — design + the model
  redistribution exception.
- [docs/research/1101-brisque-nr-metric.md](../research/1101-brisque-nr-metric.md)
  — research digest (constants, oracle, instability analysis).
- [model/brisque_live_card.md](../../model/brisque_live_card.md) — model card +
  required citation.
- [NIQE](niqe.md) — the sibling no-reference, opinion-unaware metric.
