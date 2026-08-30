# NIQE Extractor

NIQE (Natural Image Quality Evaluator) is a **no-reference**, opinion-unaware
blind image-quality metric (Mittal, Soundararajan & Bovik, *IEEE Signal
Processing Letters* 20(3):209-212, 2013). It scores a single picture by
measuring how far its natural-scene statistics (NSS) deviate from the
statistics of a pristine image population — no reference frame and no
subjective opinion scores are required. The fork ships it as a normal libvmaf
feature extractor named `niqe`.

Because it is a no-reference metric, NIQE scores **only the distorted
picture**; the reference frame is ignored, mirroring the existing CAMBI
no-reference posture. Higher score means lower perceptual quality (further from
the natural-scene model).

## Output

| Field | Value |
| --- | --- |
| Feature name | `niqe` |
| Output metric | `niqe` |
| Direction | **Lower is better** (0 = closest to the pristine model) |
| Range | `[0, ∞)`; typical natural content scores roughly `2–30` |
| Reference frame | Ignored (no-reference metric) |
| Pristine model | `model/other_models/niqe_v0.1.pkl` (embedded at build time) |
| Snapshot | `testdata/scores_cpu_niqe.json` (fork-added, not Netflix golden) |

## Usage

```bash
vmaf \
    --reference dist.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature niqe \
    --output score.json
```

NIQE ignores the reference, so you may pass the same file for `--reference` and
`--distorted` (or any reference of matching dimensions). The per-frame JSON
metric key is `niqe`; pooled values appear under the same key in
`pooled_metrics`.

## Algorithm

Per frame, on the distorted luma plane only:

1. **MSCN (scale 1)** — compute the mean-subtracted contrast-normalized
   coefficients via a separable 7-tap Gaussian (sigma = 7/6, `lw = 3`) with a
   `nearest` (clamp-to-edge) boundary and an additive `C = 1` stabiliser:
   `mscn = (luma - mu) / (sigma + 1)`. The convolutions run in float64; the
   resulting map is rounded to float32 (harness parity).
2. **MSCN (scale 2)** — bicubic-downscale the integer luma by 2
   (PIL-compatible Catmull-Rom, `a = -0.5`, output rounded to float32) and
   repeat the MSCN transform.
3. **Patch features** — for each non-overlapping 96×96 patch (48×48 on the
   half-resolution map), fit an asymmetric generalized Gaussian (AGGD) to the
   MSCN patch and to its four paired products (V, H, D1, D2), producing an
   18-feature vector per scale, 36 features per patch.
4. **Pooling** — average the per-patch vectors to a sample mean and compute
   their unbiased (ddof = 1) covariance.
5. **Distance** — the score is the Mahalanobis distance
   `sqrt(Xᵀ · pinv((cov_pris + cov_sample)/2) · X)` where `X = mu_sample -
   mu_pris` and `pinv` is the symmetric pseudo-inverse (scipy default cutoff
   `rtol = 36 · ε`).

### Fork-specific divergences (load-bearing)

The fork's pristine model `niqe_v0.1.pkl` was trained against the fork's Python
harness, which differs from upstream NIQE (LIVE MATLAB, scikit-video) in two
ways the C port must replicate exactly:

- **AGGD mean parameter `N`** carries a trailing `*aggdratio` factor that
  upstream omits. Without it, `N` is ~0.245 instead of ~0.428 on the
  reference oracle.
- **float32 round-trip**: the MSCN maps *and* the PIL bicubic half-resolution
  output are quantized to float32 before patch features are extracted.
  Skipping this shifts each feature by ~1e-7, which — amplified by the
  ill-conditioned averaged covariance — moves the final score by up to ~1e-4.

**Consequence**: the fork's NIQE scores match the fork Python harness but
**do not** match reference LIVE / scikit-video NIQE. See
[ADR-1112](../adr/1112-niqe-nr-metric.md).

## Inputs and backends

| Property | Support |
| --- | --- |
| Backend | CPU (scalar) only in this release |
| Pixel formats | Any planar YUV (luma plane only; chroma ignored) |
| Bit depth | 8-bit fully supported; 10/12/16-bit feed raw luma (see Limitations) |
| Minimum frame size | ≥ 96 px in each dimension **and** ≥ 2 total 96×96 patches |

## Limitations

- **No-reference, SDR-trained**: NIQE has no transfer-function awareness and
  scores raw luma. For PQ/HLG HDR content the NSS assumptions break down; the
  metric still produces a number but it is not calibrated for HDR.
- **High bit depth**: the pristine model was trained on 8-bit luma. For
  >8-bpc input the extractor feeds raw high-bit-depth values; because the MSCN
  `C = 1` denominator is **not** scale-invariant, scores are not guaranteed to
  match the 8-bit-trained model. Convert to 8-bit (e.g. via the CLI's bit-depth
  handling) for calibrated results until an explicit scaling policy lands.
- **Upstream parity**: scores match the fork Python harness, not reference
  LIVE/skvideo NIQE (see fork-specific divergences above).
- **GPU twins**: no CUDA/SYCL/HIP implementation yet; a future twin will keep
  the covariance + pinv in fp64 and is not expected to be bit-exact to CPU
  (consistent with the fork's GPU-parity posture, places = 4).

## Regenerating the model header and score snapshot

`core/src/feature/niqe_model.h` is generated from the pristine `.pkl`. To
regenerate it (and the score snapshot) after an intentional model change:

```python
import pickle, numpy as np
with open("model/other_models/niqe_v0.1.pkl", "rb") as f:
    d = pickle.load(f)
m = d["model_dict"]["model"]
mu, cov = np.asarray(m["mu"], float), np.asarray(m["cov"], float)
# Reorder the alphabetically-stored feature vector into the per-block
# interleaved order the C extractor emits:
atom = ["alpha_m1","blbr1","alpha11","N11","lsq11","rsq11","alpha12","N12",
        "lsq12","rsq12","alpha13","N13","lsq13","rsq13","alpha14","N14",
        "lsq14","rsq14","alpha_m2","blbr2","alpha21","N21","lsq21","rsq21",
        "alpha22","N22","lsq22","rsq22","alpha23","N23","lsq23","rsq23",
        "alpha24","N24","lsq24","rsq24"]
stored = [s.replace("NIQE_noref_feature_","").replace("_scores","")
          for s in d["model_dict"]["feature_names"]]
perm = [stored.index(a) for a in atom]
mu_i, cov_i = mu[perm], cov[np.ix_(perm, perm)]   # emit as %.17g into niqe_model.h
```

Build-time checksums asserted against the `.pkl`: `mu.sum() = 21.0218816411`,
`trace(cov) = 2.6622550083`, `mu sha256[:16] = f244e4a7538d6837`,
`cov sha256[:16] = 75c0095bba8abc89`.

## Correctness test

`core/test/test_niqe.c` (run via `meson test -C core/build-cpu test_niqe`)
asserts the gauss window, AGGD oracles, the bicubic resampler, the symmetric
pseudo-inverse, and an end-to-end NIQE score against the fork Python-harness
reference on frame 0 of `testdata/ref_576x324_48f.yuv`
(`testdata/scores_cpu_niqe.json`) at places = 4.

## See also

- [ADR-1112: NIQE no-reference CPU feature extractor](../adr/1112-niqe-nr-metric.md)
- [CAMBI](cambi.md) — the other fork no-reference metric
- [Features overview](features.md)
