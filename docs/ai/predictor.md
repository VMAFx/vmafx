<!-- markdownlint-disable MD060 -->
# Per-shot VMAF predictor

The per-shot VMAF predictor turns "encode every shot, score every
shot" into "predict every shot, encode every shot, score a sampled
subset". The predict-then-verify loop saves wall time on long titles
without giving up the per-shot quality contract:

1. Probe-encode each shot once at the codec's `probe_quality`
   (e.g. `libx264 --preset ultrafast --crf 28`).
2. Read cheap signals from the probe — bitrate, per-frame-type
   sizes, optional saliency / signalstats.
3. Feed those signals to the per-codec ONNX predictor; it returns a
   predicted VMAF for any candidate CRF.
4. Binary-search the codec's CRF range for the largest CRF whose
   predicted VMAF still meets the operator's target.
5. Encode at that CRF.
6. Validate by re-scoring a stratified sample of shots; if the
   residuals stay within tolerance the predictions hold (`GOSPEL`),
   else recalibrate or fall back.

This document covers the user-facing contract per the five-point
tiny-AI bar in
[ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md).

## 1. Purpose

Concretely, the predictor lets `tools/vmaf-tune` skip step 5's costly
real-VMAF measurement on every shot. With 14 codec adapters
(`libx264`, `libx265`, `libsvtav1`, `libaom-av1`, `libvvenc` plus the
NVENC, AMF, and QSV families across H.264, HEVC, AV1) the harness
loads `model/predictor_<codec>.onnx` at startup and routes every
`pick_crf(...)` through it.

The runtime predictor surface is:

```python
from vmaftune.predictor import Predictor, ShotFeatures

p = Predictor(model_path=Path("model/predictor_libx264.onnx"))
crf = p.pick_crf(features, target_vmaf=92.0, codec="libx264")
```

Without `model_path`, the predictor falls back to a per-codec
analytical curve. Tests and dev hosts without ONNX Runtime hit that
path automatically; production deployments load the ONNX file.

## 2. Training data

The trainer is `tools/vmaf-tune/src/vmaftune/predictor_train.py`. It
consumes the same vmaf-tune Phase A JSONL corpus
([ADR-0237](../adr/0237-quality-aware-encode-automation.md)) that the
recommend / per-shot tools already produce — one row per
`(source, preset, crf)` cell with `bitrate_kbps` and the measured
`vmaf_score`.

The hardware-encoder models for `h264_nvenc`, `hevc_nvenc`,
`av1_nvenc`, `h264_qsv`, `hevc_qsv`, and `av1_qsv` are trained on the
real Phase-A hardware sweep at
`runs/phase_a/full_grid/comprehensive.jsonl` (local training corpus;
not committed). Their model cards carry `corpus.kind: real-N=<rows>`
and honest held-out metrics.

The software and AMF models remain **synthetic-stub** models per
[ADR-0325](../adr/0325-predictor-stub-models-policy.md): each such codec
gets a deterministic 100-row synthetic corpus seeded by the codec name.
The synthetic target is the predictor's own analytical-fallback curve,
so the resulting ONNX model is a smooth re-encoding of the analytical
formula. **Stub models are not authoritative for production CRF picks.**
Every per-codec model card flags this prominently.

To train real models on a real corpus:

```bash
# 1. Generate the corpus (Phase A, may take hours).
python -m vmaftune.cli corpus --encoder libx264 \
    --source ref.yuv --output corpus.jsonl ...

# 2. Re-train the predictor for one or more codecs against it.
python -m vmaftune.predictor_train \
    --corpus corpus.jsonl \
    --output-dir model \
    --codec libx264 --codec libx265 \
    --epochs 200
```

The trainer writes one ONNX + one model card per requested codec.
Codecs not present in the corpus fall back to the synthetic-stub path
in the same run; mixed runs are explicit in each card via the
`corpus.kind` line.

### Corpus row → predictor input projection

| Predictor input               | Source                                                   |
|-------------------------------|----------------------------------------------------------|
| `crf`                         | row `crf` (`cq` / `q` aliases accepted for hardware sweeps) |
| `probe_bitrate_kbps`          | row `bitrate_kbps` (`actual_kbps` alias accepted)        |
| `probe_*_avg_bytes`           | row values when present; otherwise derived from `bitrate_kbps` + `framerate` |
| `saliency_*` / signalstats    | row values when present; otherwise zero                   |
| `shot_length_frames`          | `framerate × duration_s`                                  |
| `fps`, `width`, `height`      | row metadata                                              |

The runtime extractor in `predictor_features.py` populates the
saliency / signalstats inputs from a real probe run. `vmaf-tune predict
--use-saliency` decodes the current shot to temporary `yuv420p` and runs
the configured `saliency_student` ONNX model over sampled frames before
feeding `saliency_mean` and `saliency_var` into `ShotFeatures`; this is a
predictor input, not the ROI encode path from `recommend-saliency`.

## 3. Op allowlist compliance

The trainer validates every exported ONNX against the libvmaf C-side
allowlist (`core/src/dnn/op_allowlist.c`) via
`ai/src/vmaf_train/op_allowlist.py`. Failure aborts the export.

The shipped MLP graph uses only allowlisted ops:

| Op           | Used in                                  |
|--------------|------------------------------------------|
| `Sub`, `Div` | per-feature input normalisation          |
| `Gemm`       | three fully-connected layers             |
| `Relu`       | hidden-layer activation                  |
| `Sigmoid`    | output range gating                      |
| `Mul`        | output × 100 to land in `[0, 100]`       |
| `Constant`   | normalisation buffers + bias terms       |

Op-allowlist status appears in every per-codec model card under
section 3.

## 4. Validation metrics

Each model card carries PLCC, SROCC, and RMSE on the held-out
20 % split (seeded shuffle). Stub-model numbers are artificially high
because the regression target *is* the analytical fallback — the
network smooths itself. Real-corpus runs produce honest numbers; the
production gate is "PLCC ≥ 0.95 on the held-out split per codec",
matching the existing `fr_regressor_v2` gate
([ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md)).

The trainer also pins the runtime contract via
`tools/vmaf-tune/tests/test_predictor_train.py`:

- Every shipped `model/predictor_<codec>.onnx` loads under ONNX
  Runtime CPU.
- Output is finite and clamped to `[0, 100]`.
- Output is non-strictly monotone-decreasing in CRF.
- `Predictor(model_path=...)` routes through the ONNX session, not
  the analytical fallback.

## 5. Signing

Production-grade tiny-AI weights ship with a Sigstore-keyless OIDC
signature attached at the release-please tag step (per the existing
`model/tiny/*.onnx` pattern; see
[`docs/development/release.md`](../development/release.md)). Stub
models ship **unsigned** because their numerical content is not
authoritative; their cards carry a `Sigstore signature: PLACEHOLDER`
line. Real-corpus model files are still unsigned while in-tree on a
branch; release automation attaches the Sigstore-keyless OIDC bundles
for published tags, following the same release workflow as the
`model/tiny/*.onnx` artefacts.

## File layout

```text
model/
  predictor_libx264.onnx         + predictor_libx264_card.md
  predictor_libx265.onnx         + predictor_libx265_card.md
  predictor_libsvtav1.onnx       + predictor_libsvtav1_card.md
  predictor_libaom-av1.onnx      + predictor_libaom-av1_card.md
  predictor_libvvenc.onnx        + predictor_libvvenc_card.md
  predictor_h264_nvenc.onnx      + predictor_h264_nvenc_card.md
  predictor_hevc_nvenc.onnx      + predictor_hevc_nvenc_card.md
  predictor_av1_nvenc.onnx       + predictor_av1_nvenc_card.md
  predictor_h264_amf.onnx        + predictor_h264_amf_card.md
  predictor_hevc_amf.onnx        + predictor_hevc_amf_card.md
  predictor_av1_amf.onnx         + predictor_av1_amf_card.md
  predictor_h264_qsv.onnx        + predictor_h264_qsv_card.md
  predictor_hevc_qsv.onnx        + predictor_hevc_qsv_card.md
  predictor_av1_qsv.onnx         + predictor_av1_qsv_card.md

tools/vmaf-tune/src/vmaftune/
  predictor.py                   # runtime: Predictor, pick_crf, pick_keyint
  predictor_features.py          # probe-encode + signalstats extractor
  predictor_train.py             # trainer (this PR)
  predictor_validate.py          # GOSPEL / RECALIBRATE / FALL_BACK loop

tools/vmaf-tune/tests/
  test_predictor.py              # runtime + analytical fallback pins
  test_predictor_train.py        # trainer + shipped-model pins (this PR)
```

## Reproduction

```bash
# Re-train every shipped stub (~30s on CPU).
python -m vmaftune.predictor_train --output-dir model --epochs 120

# Verify every shipped model.
pytest tools/vmaf-tune/tests/test_predictor_train.py -v
```

## See also

- [ADR-0237 — quality-aware encode automation](../adr/0237-quality-aware-encode-automation.md)
- [ADR-0392 — vmaf-tune Phase D per-shot tuning](../adr/0392-vmaf-tune-phase-d-per-shot.md)
- [ADR-0325 — predictor stub-models policy](../adr/0325-predictor-stub-models-policy.md)
- [ADR-0042 — tiny-AI docs bar](../adr/0042-tinyai-docs-required-per-pr.md)
