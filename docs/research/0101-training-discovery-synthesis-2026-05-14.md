# Training Discovery Synthesis — 2026-05-14

## Scope

This note answers the operator question: "we already have trained a lot,
I wonder if we already can make discoveries by what we learned so far?"

The answer is yes, but only for claims backed by committed model
sidecars or model cards. This synthesis intentionally excludes
gitignored local run directories and uncommitted corpora so the
evidence can be reproduced from a clean checkout.

Reproducer:

```bash
python3 scripts/dev/training_discovery_report.py
```

## Actionable findings

### 1. Canonical-6 FR prediction is saturated for the current corpus

`fr_regressor_v3` uses the canonical-6 libvmaf feature block plus an
18-D codec block and clears the LOSO gate by a wide margin:

| Model | Rows | PLCC | SROCC | RMSE | Evidence |
| --- | --- | --- | --- | --- | --- |
| fr_regressor_v2 | 216 | 0.9794 | 0.9640 | 3.0143 | in-sample |
| fr_regressor_v3 | 5640 | 0.9975 | 0.9691 | 1.0883 | LOSO |
| fr_regressor_v2_ensemble_v1 | - | 0.9973 | - | - | LOSO ensemble, spread=0.000951 |

Action: stop spending effort on deeper MLPs over exactly the same
canonical-6 feature space. The v3 / v4 history already shows the next
gains need a regime change: richer feature columns, more diverse
corpora, or uncertainty/ensemble use, not a larger fully-connected
network over the same six inputs.

What to do next:

- Keep `fr_regressor_v3` as the strong baseline for future retrain
  comparisons.
- Prioritise the `v3plus`/richer-feature path and corpus expansion
  over architecture-only experiments.
- Use the weaker folds from the v3 sidecar (`FoxBird`, `ElFuente2`,
  `Tennis`) as the first content set for residual analysis.

### 2. QSV is easier to predict than NVENC in the current hardware corpus

The real hardware predictor cards show QSV ahead of NVENC for every
shared codec family in PLCC and RMSE. The AV1 gap is large enough to
be operationally interesting rather than measurement noise.

| Codec family | NVENC PLCC | QSV PLCC | Delta | NVENC RMSE | QSV RMSE |
| --- | --- | --- | --- | --- | --- |
| h264 | 0.7908 | 0.7945 | +0.0037 | 13.7288 | 12.9497 |
| hevc | 0.7439 | 0.8302 | +0.0863 | 12.0813 | 9.7754 |
| av1 | 0.6561 | 0.8777 | +0.2216 | 12.4922 | 8.5336 |

Action: treat NVENC predictor quality as the next hardware-model
debugging target. The current 14-D predictor feature vector is not
capturing enough of NVENC's rate-control behaviour, especially for
AV1.

What to do next:

- Add per-card/per-driver/device metadata to the training sidecar
  audit, but keep it out of static hardware-capability priors until
  it comes from measured corpus rows.
- Run permutation/residual analysis on the real NVENC rows first,
  with slices by codec, source, resolution, CQ, and bitrate bucket.
- Test whether adding first-pass encode statistics or GOP-shape
  features closes the AV1 NVENC residual before collecting a larger
  corpus.

### 3. Resize+Conv is a real saliency-student improvement

The saliency-student v2 ablation changed only the decoder upsampler
shape and improved validation IoU over v1:

| Model | Best val IoU | Params | Decoder |
| --- | --- | --- | --- |
| saliency_student_v1 | 0.6558 | 112841 | ConvTranspose decoder |
| saliency_student_v2 | 0.7105 | 123721 | Resize+Conv decoder |

Action: v2 is good enough to justify an ROI encode validation pass,
but not a production flip by itself. The next gate is whether the
better saliency map improves bitrate allocation in real encodes.

What to do next:

- Run matched ROI encodes using v1 vs v2 on the existing saliency-aware
  `vmaf-tune` surfaces.
- Compare bitrate at fixed VMAF, saliency-weighted VMAF, and visible
  artifacts in high-saliency regions.
- Promote v2 only if encode-level validation agrees with the DUTS
  IoU improvement.

### 4. CHUG is the immediate HDR subjective-corpus target

CHUG ("Crowdsourced User-Generated HDR Video Quality Dataset") is now
the highest-leverage HDR corpus to add before drawing HDR-specific
training conclusions. The repository describes 5,992 UGC-HDR videos
from 856 HDR references, 211,848 AMT ratings, bitrate-ladder encodes,
portrait/landscape coverage, and a CSV manifest with `Video`, `mos_j`,
`sos_j`, `ref`, `bitladder`, `resolution`, `bitrate`, `orientation`,
`framerate`, `height`, and `width` columns.

Action: use the CHUG manifest adapter before further HDR discovery
claims. It is a metadata/manifest loader first, with downloads kept in
`.workingdir2` and no video redistribution.

What to do next:

- Run `ai/scripts/chug_to_corpus_jsonl.py`: parse `chug.csv`, expose
  video IDs, MOS/SOS, reference flag, resolution, bitrate ladder label,
  orientation, FPS, height, and width.
- Use the script's `--max-rows` smoke path to validate CSV parsing and
  S3 URL construction without downloading the whole dataset.
- Materialise FR feature rows by pairing each distorted ladder row with
  its `chug_content_name` reference row, scaling the distorted side to
  the reference geometry before libvmaf extraction. This is recorded in
  ADR-0427.
- Gate all committed CHUG-derived weights as non-commercial research
  artefacts unless the license ambiguity below is resolved in a more
  permissive direction.

## Blockers for the remaining claims

### Synthetic predictor cards are not evidence

The AMF, libx264, libx265, libaom-av1, libsvtav1, and libvvenc
predictor cards are still synthetic-stub cards. Their metrics are
expected to look high because the regression target is the analytical
fallback, not a held-out measured corpus.

Blocker: real corpora do not exist yet for these adapters in committed
artefacts. Until they do, these cards can validate the load path only.

### MOS-head discoveries need committed gate metrics

`konvid_mos_head_v1` is structurally present and the invariants are
documented, but the sidecar does not expose the same compact metric
block that the FR and saliency sidecars expose.

Blocker: the MOS-head model card / sidecar needs a committed summary
of PLCC, SROCC, RMSE, spread, corpus split, and gate verdict before we
can cite it in discovery claims.

### We do not yet know whether NVENC needs features or just rows

The real hardware cards identify NVENC as the weak family, but they do
not explain the cause. The plausible causes are separable:

- corpus imbalance across content / CQ / resolution;
- insufficient probe features for NVENC rate control;
- device / driver behaviour hidden behind a single encoder label;
- train/test split leakage or mismatch from the seeded 80/20 split.

Blocker: residual analysis over the underlying real rows is needed
before changing the predictor architecture.

### HDR conclusions are blocked on the external model

The current FR and predictor evidence is SDR / existing-model
evidence. Netflix's future HDR model can change score distributions
and the feature-response profile. CHUG closes part of the data gap for
subjective UGC-HDR/MOS learning, but it does not replace a committed
HDR-FR teacher model.

Blockers:

- no HDR-FR teacher model artefact is in-tree yet;
- no CHUG feature-extraction pass has completed yet;
- CHUG's README badge says CC BY-NC 4.0, while `license.txt` contains
  Creative Commons Attribution-NonCommercial-ShareAlike 4.0 text. Treat
  the stricter non-commercial/share-alike terms as the working license
  until clarified;
- CHUG videos are externally hosted on S3 and must remain out of git.

HDR-specific discoveries should stay out of the production report until
the model and/or CHUG adapter lands and a fresh corpus pass runs.

## Generated sidecar report

The following table is generated from committed sidecars/cards by
`scripts/dev/training_discovery_report.py`.

```text
# Training Discovery Report
#
# Generated from committed model sidecars and model cards.
#
# Tiny FR Regressors
#
# | Model | Rows | PLCC | SROCC | RMSE | Evidence |
# | --- | --- | --- | --- | --- | --- |
# | fr_regressor_v2 | 216 | 0.9794 | 0.9640 | 3.0143 | in-sample |
# | fr_regressor_v3 | 5640 | 0.9975 | 0.9691 | 1.0883 | LOSO |
# | fr_regressor_v2_ensemble_v1 | - | 0.9973 | - | - | LOSO ensemble, spread=0.000951 |
#
# Saliency Students
#
# | Model | Best val IoU | Params | Decoder |
# | --- | --- | --- | --- |
# | saliency_student_v1 | 0.6558 | 112841 | ConvTranspose decoder |
# | saliency_student_v2 | 0.7105 | 123721 | F.interpolate(scale_factor=2.0, mode='bilinear', align_corners=False) + nn.Conv2d(kernel=3, padding=1, no bias) |
#
# Real Hardware Predictor Cards
#
# | Codec | Corpus | PLCC | SROCC | RMSE | Card |
# | --- | --- | --- | --- | --- | --- |
# | av1_nvenc | real-N=2592 | 0.6561 | 0.6154 | 12.4922 | model/predictor_av1_nvenc_card.md |
# | h264_nvenc | real-N=2592 | 0.7908 | 0.7837 | 13.7288 | model/predictor_h264_nvenc_card.md |
# | hevc_nvenc | real-N=2592 | 0.7439 | 0.7374 | 12.0813 | model/predictor_hevc_nvenc_card.md |
# | av1_qsv | real-N=1620 | 0.8777 | 0.8424 | 8.5336 | model/predictor_av1_qsv_card.md |
# | h264_qsv | real-N=1620 | 0.7945 | 0.8555 | 12.9497 | model/predictor_h264_qsv_card.md |
# | hevc_qsv | real-N=1620 | 0.8302 | 0.8322 | 9.7754 | model/predictor_hevc_qsv_card.md |
#
# QSV vs NVENC Predictor Delta
#
# | Codec family | NVENC PLCC | QSV PLCC | Delta | NVENC RMSE | QSV RMSE |
# | --- | --- | --- | --- | --- | --- |
# | h264 | 0.7908 | 0.7945 | +0.0037 | 13.7288 | 12.9497 |
# | hevc | 0.7439 | 0.8302 | +0.0863 | 12.0813 | 9.7754 |
# | av1 | 0.6561 | 0.8777 | +0.2216 | 12.4922 | 8.5336 |
# ```

## External corpus reference — CHUG

- Repository: <https://github.com/shreshthsaini/CHUG>
- Paper DOI: <https://doi.org/10.1109/ICIP55913.2025.11084488>
- CSV manifest: `https://raw.githubusercontent.com/shreshthsaini/CHUG/master/chug.csv`
- Video ID list: `https://raw.githubusercontent.com/shreshthsaini/CHUG/master/chug-video.txt`
- Video URL pattern:
  `https://ugchdrmturk.s3.us-east-2.amazonaws.com/videos/VIDEO.mp4`
