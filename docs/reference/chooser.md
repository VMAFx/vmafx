<!-- markdownlint-disable MD013 MD046 MD060 -->
# Metric and model chooser

One row per question: which metric and model answer it, and which pitfall to
watch. The [feature index](../metrics/features.md) is the full per-extractor
reference; the [concept guide](concepts.md) explains why each pitfall bends
the number; the [glossary](glossary.md) defines every term.

Per-backend cost numbers are deliberately not listed here yet — the RC8
measurements are still landing; when they do, this table gains a cost column
per backend rather than guesses.

| Question | Metric | Model | Watch out for |
|---|---|---|---|
| How does my encode look to a living-room viewer? | `vmaf` | `vmaf_v1.0.16_3d0h` (the fork default) | Upstream compares with `vmaf_v0.6.1` — pin the model or scores are not comparable ([default model](glossary.md#default-model)) |
| Does my number match a Netflix/AOM published run? | `vmaf` | `vmaf_v0.6.1` or `--aom_ctc` / `--nflx_ctc` | The CTC presets pin the whole feature set, not just the model ([CTC](glossary.md#ctc)) |
| Did my encoder add sharpening or other fake gains? | `vmaf` | `vmaf_v0.6.1neg` | NEG exists on the v0.6.1 family only; compare NEG against NEG ([NEG](glossary.md#neg)) |
| Phone screen quality | `vmaf` | `vmaf_v1.0.16_5d0h`, or `vmaf_v0.6.1:enable_transform=true` | The transform changes the output curve, not the features ([phone model](glossary.md#phone-model)) |
| 4K content | `vmaf` | `vmaf_v1.0.16_1d5h_2160` or `vmaf_4k_v0.6.1` | The 4K v1 3H variant reports on a [0, 110] range — not comparable with [0, 100] models |
| High-frame-rate content | `vmaf` | the `_hfr` variants | Trained on high-frame-rate subjective data; regular models misjudge high temporal activity |
| HDR content | `pu21_psnr`, `pu21_ssim` | (adapters, no model) | Classic models are SDR-trained; PQ/HLG luma breaks their assumptions ([HDR](glossary.md#hdr)) |
| HDR colour errors | `delta_e_itp`, `ciede` | (features) | `delta_e_itp` defaults to PQ, BT.2020 NCL, limited range — state the real one ([range](glossary.md#range)) |
| Banding / contouring | `cambi` | (feature) | CAMBI measures at 10-bit internally; 8-bit input is converted first |
| Chroma damage | `ciede2000`, `delta_e_itp` | (features) | Luma-only metrics (`psnr`, `ssim`, `vmaf`) do not see chroma subsampling damage ([chroma subsampling](glossary.md#chroma-subsampling)) |
| Structural similarity, tuned for humans | `ssim`, `ms_ssim` | (features) | The bundled SSIM includes the empirical downsampling step; FFmpeg's filter does not — numbers differ by design |
| Detail retention | `adm` (or `float_adm`) | (feature) | `adm` is the fixed-point feature the shipped models read |
| Fidelity / information preserved | `vif` (or `float_vif`) | (feature) | Scores per pyramid scale; models read the four scale outputs |
| No reference available | `brisque`, `niqe` | (features) | Opinion-aware (BRISQUE) vs opinion-unaware (NIQE); both SDR-luma-trained ([SDR](glossary.md#sdr)) |
| Still image / single frame | `vmaf` | `vmaf_v0.6.1still` | Motion is forced to 0 — use it only for genuinely still content ([motion](glossary.md#motion)) |
| How sure is the number? | `vmaf` + interval | `vmaf_b_v0.6.3` | Bootstrap model carries a 95% interval ([confidence interval](glossary.md#confidence-interval)) |
| Which encode hurts the worst moments? | `vmaf` pooled by `min` or `perc5` | any | Percentile pooling is a C API / `--pool` / FFmpeg `pool` option, not a CLI report flag ([pooling](glossary.md#pooling)) |
| Did temporal activity change? | `motion`, `tad` | (features) | `motion` reads the **reference** stream, not the distorted one |
| Temporal artifacts (flicker, pumping) | `float_moment`, `y_funque_plus_*` | (features) | Per-frame luma statistics; see each extractor page |
| Learned / AI-assisted quality signals | tiny-AI extractors (`lpips`, `ssimulacra2`, …) | (features) | ONNX Runtime based; some are smoke-status, check each page first |

## Reading the answer

- **One question, one metric.** Stacking metrics answers nothing extra; pick
  the row that matches the decision you are making.
- **Pin the model.** Two runs are comparable only when the model, its
  generation and the viewing condition match — this is the most common
  wrong-number source ([concepts](concepts.md)).
- **Report the pooling method.** `mean` and `harmonic_mean` on the same pair
  differ; the report carries both, say which one you quoted.
