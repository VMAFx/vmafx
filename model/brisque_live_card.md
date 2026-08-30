# Model card — BRISQUE LIVE model (`other_models/brisque_live.model`)

Trained EPSILON_SVR model backing the fork's BRISQUE no-reference
image-quality extractor (feature name `brisque`,
[`core/src/feature/brisque.c`](../core/src/feature/brisque.c)). Redistributed
under a documented research-use attribution exception — see
[ADR-1115](../docs/adr/1115-brisque-nr-metric.md) and
[`other_models/NOTICE-brisque`](other_models/NOTICE-brisque).

## What it is

- **Algorithm**: BRISQUE — Blind/Referenceless Image Spatial Quality Evaluator
  (Mittal, Moorthy, Bovik, IEEE TIP 2012). Opinion-aware no-reference IQA over a
  36-dimensional natural-scene-statistics (NSS) feature vector (18 features ×
  2 scales).
- **Predictor**: libsvm EPSILON_SVR, RBF kernel.
  `gamma 0.05`, `nr_class 2`, `total_sv 770`, `rho -155.845`, `probA 6.34795`.
- **Output**: a perceptual quality score; **lower = better quality**
  (≈ 0 for pristine natural images, rising toward ~100 for heavy distortion).
  Mild negative values on very clean content are expected and intentional — the
  reference prediction path does **not** clamp the output (the fork matches that;
  the OpenCV variant clamps to [0, 100] and is a *different* model).

## Provenance and integrity

- **Source**: `C++/allmodel` from
  <https://github.com/krshrimali/No-Reference-Image-Quality-Assessment-using-BRISQUE-Model>,
  a verbatim mirror of the original LIVE-lab model also shipped by the MATLAB
  pipeline `gregfreeman/image_quality_toolbox` (`+brisque/allmodel`) that trained it.
- **Format**: native libsvm text. Embedded into the libvmaf binary at build
  time by an `xxd -i` Meson `custom_target` (the same path libvmaf's JSON
  models take), exposing the `src_brisque_live_model[]` / `_len` symbols
  declared in
  [`brisque_model.h`](../core/src/feature/brisque_model.h); loaded once at
  `init()` via the vendored `svm_parse_model_from_buffer`. A caller may
  override it with an on-disk model through the `model_path` feature option
  (`svm_load_model`).
- `sha256 = 19526fb799c4c7992ccc109fcfecddb25976ba024b194cd3ee275d27e8909c8d`
- `bytes  = 351414`

There is no committed C array to regenerate. After replacing the model file,
just rebuild — Meson re-runs `xxd`. Update this card's `sha256` / `bytes` (e.g.
`sha256sum model/other_models/brisque_live.model`) and the NOTICE to match.

## Feature range normalization

The 36 features are scaled to [-1, 1] before predict via
`xs[i] = -1 + 2*(f[i]-min_[i])/(max_[i]-min_[i])`. The `min_[36]` / `max_[36]`
arrays are baked into `brisque.c` and come from the reference **inline** arrays
in `computescore.cpp` (the array the trained model expects). They are **not** the
separate `C++/allrange` file, which is a different, prediction-inconsistent set
the reference code never reads — substituting it would corrupt every score
(ADR-1115).

## Scope and limitations

- **SDR luma only.** BRISQUE is a spatial-luminance NSS model trained on 8-bit
  SDR LIVE images. The extractor scales any bit depth's luma to the [0, 255]
  double working range used at training. PQ / HLG **HDR is out of scope** — no
  HDR-trained BRISQUE model exists; the extractor emits a one-time warning and
  scores HDR transfer characteristics as if SDR (the score is not meaningful).
- **Single-frame, no-reference.** Scores the distorted picture's luma plane only;
  reference / 90°-rotated inputs are ignored (CAMBI/NIQE NR posture).

## Citation (required)

> A. Mittal, A. K. Moorthy and A. C. Bovik, "No-Reference Image Quality
> Assessment in the Spatial Domain," IEEE TIP 21(12):4695-4708, 2012.
> doi:10.1109/TIP.2012.2214050

See [`docs/metrics/brisque.md`](../docs/metrics/brisque.md) for the full
algorithm description and the end-to-end score snapshot.
