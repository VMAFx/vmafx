# ChatGPT vision + Claude-bias audit — VMAFx/vmafx fork

Date: 2026-05-08. Repo state: branch `chore/ensemble-kit-gdrive-quickstart`,
master tip `4c170667`.

## Part 1 — ChatGPT vision vs project state

### 1. VMAF Prediction (TinyAI predicts VMAF directly)

**Already shipped.** This is the dominant fork-local theme.

- Direct VMAF regressors live under `model/tiny/`:
  `vmaf_tiny_v1..v4`, `fr_regressor_v1..v3`, plus the 5-seed
  `fr_regressor_v2_ensemble_v1_seed{0..4}`.
  See `/home/kilian/dev/vmaf/model/tiny/registry.json` and the model cards.
- Codec-aware FR regressor: `docs/adr/0235-codec-aware-fr-regressor.md`,
  `0272-fr-regressor-v2-codec-aware-scaffold.md`,
  `0291-fr-regressor-v2-prod-ship.md`,
  `0303-fr-regressor-v2-ensemble-prod-flip.md`,
  `0321-fr-regressor-v2-ensemble-full-prod-flip.md`,
  `0323-fr-regressor-v3-train-and-register.md`.
- Predict-then-verify per-shot harness in `vmaf-tune`:
  `tools/vmaf-tune/src/vmaftune/predictor.py:181` (`predict_vmaf`),
  `predictor_validate.py`, `per_shot.py`, plus the inversion
  `pick_crf` at `predictor.py:258`.
- Training corpus pipeline:
  `ai/scripts/train_fr_regressor_v2_ensemble_loso.py`,
  `ai/scripts/extract_full_features.py`, BVI-DVC ingest under
  `ai/scripts/bvi_dvc_to_corpus_jsonl.py`
  (ADR-0310 — accepted 2026-05-05).

### 2. Alternative metrics & perceptual analysis

**Mostly already shipped, partially in flight.**

- DISTS extractor: `docs/adr/0043-dists-extractor-design.md`,
  `0236-dists-extractor.md`. Source under `core/src/feature/`
  (paired with research `docs/research/0043-dists-extractor-design.md`).
- LPIPS (squeezenet variant): `model/tiny/lpips_sq.onnx`,
  `core/src/feature/feature_lpips.c`, ADR-0041 / model-card
  `docs/ai/models/lpips_sq.md`.
- SSIMULACRA2: scalar + SIMD ports
  (`docs/adr/0003`/`0007`/`0015..0018`).
- SpEED-QA: scalar implementation present (`core/src/feature/speed.c`),
  ADR-0010 / ADR-0051 / ADR-0253 covering the upstream-direction
  question.
- NR (no-reference) metric: `model/tiny/nr_metric_v1.onnx` +
  ADR-0248-nr-metric-v1-ptq.md.
- Saliency / ROI: `model/tiny/saliency_student_v1.onnx` (ADR-0286,
  trained on DUTS-TR), `model/tiny/mobilesal.onnx` (ADR-0218),
  `tools/vmaf-roi-score/`, ADR-0247 / ADR-0296.
- "Source-aware tuning, encoder-aware decision making, lower file
  sizes": this is exactly the `vmaf-tune` harness — ADR-0237 /
  ADR-0276 / ADR-0297, predictor under
  `tools/vmaf-tune/src/vmaftune/`.

### 3. Local sidecar training during encodes

**Planned but partial.** No on-host continual training during
encode is wired up. The closest in-tree pieces:

- `tools/ensemble-training-kit/` (ADR-0324, Proposed,
  2026-05-06) — portable retrain bundle the user can ship to a
  collaborator and run offline. Not on-host-during-encode; offline
  retrain.
- The harness already records per-encode artifacts (corpus rows
  via `ai/scripts/extract_full_features.py`,
  `tools/vmaf-tune/src/vmaftune/cache.py`) so the data substrate
  exists; the *adaptive loop* (post-encode incremental fit, drift
  monitoring, on-host fine-tune) does not.
- No ADR for online / continual / sidecar fine-tune.
  Closest tangentially-relevant: ADR-0207 (QAT design) and
  ADR-0287 (corpus expansion).

### 4. Community learning / opt-in upload

**Not in project.** No file under `docs/`, `docs/adr/`, or
`docs/research/` mentions community uploads, federated learning,
crowdsourced datasets, anonymisation, or telemetry. The fork's
training corpora are explicitly local-only (Netflix Public, BVI-DVC,
KonVid-1k) and ADR-0310 §2 keeps BVI-DVC archives gitignored under
`.workingdir2/`. This is a clean greenfield gap — not contradicted,
just unaddressed.

## Part 2 — Claude-bias audit

### 1. Fabricated APIs

Spot-checked: `onnxruntime.InferenceSession` (real),
`onnxruntime` `CPUExecutionProvider` (real), numpy `np.asarray`
(real), `torch.onnx.export` references in `ai/scripts/export_*.py`
(real). FFmpeg flag `-x264-params` referenced at
`tools/vmaf-tune/src/vmaftune/codec_adapters/x264.py:44` (real,
documented in FFmpeg). **No fabricated APIs found.**

### 2. Stale or wrong version numbers

- FFmpeg patch base `n8.1` (`ffmpeg-patches/series.txt:1`,
  `CLAUDE.md:277`). FFmpeg 8.1 was tagged 2025; reasonable.
- ONNX Runtime version is unpinned in docs — flagged elsewhere as
  "ONNX Runtime C API" without version. Not invented but
  imprecise. **No false-version claims found.**

### 3. Wrong attribution

- ADR-0310 cites "Ma, Zhang, Bull 2021" for BVI-DVC without DOI.
  The paper exists (BVI-DVC, IEEE TIP 2021, Ma/Zhang/Bull) — name
  matches but no link.
- ADR-0286 cites "DUTS-TR" — real saliency dataset (Wang et al.
  CVPR 2017). No URL.
- These citations are real; missing URLs are a docs polish issue,
  not a fabrication.

### 4. Self-confidently wrong patterns

**One real bug found.** Duplicate field declaration in
`tools/vmaf-tune/src/vmaftune/codec_adapters/x264.py:30` and
`x264.py:36`:

```python
quality_range: tuple[int, int] = (15, 40)   # line 30
...
quality_range: tuple[int, int] = (0, 51)    # line 36 — same field, redeclared
```

Both lines name the same dataclass field. Python keeps the second
binding, so the effective range is `(0, 51)`. Either the
restriction window comment at line 28 is dead, or the override at
line 36 is wrong. Either way the "we kept both ranges in case the
search loop wants the wider one" intent is silently broken — a
reader sees two values, only one survives. **Fix needed.**

VMAF range used as `[0, 100]` everywhere
(`predictor.py:216`, `:256`) — correct (VMAF is 0..100, not 0..1).
CRF orientation (higher CRF = lower quality, `invert_quality:
True` in adapters, `vmaf = a - b·delta` in
`predictor.py:215`) — correct sign. Codec quality ranges
(libaom 0..63 at `libaom.py:66`, svtav1 20..50 narrow window at
`svtav1.py:91`, vvenc 17..50 at `vvenc.py:92`, videotoolbox
0..100 at `_videotoolbox_common.py:70`, qsv 1..51 at
`_qsv_common.py:40`, NVENC 15..40 narrow at
`_nvenc_common.py:26`) — all match documented codec semantics.
Narrow windows are intentional Phase A "perceptually-informative"
restrictions, not bugs.

### 5. TODO/FIXME/XXX hotspots

`grep -rn "TODO|FIXME|XXX" tools/vmaf-tune/src/ ai/ core/src/dnn/`
returns **zero hits** — no Claude-punted markers in the
fork-local Python or the DNN runtime. (The repo at large has them
in upstream-mirror C, out of scope here.)

## Summary

ChatGPT's vision: items 1 + 2 are already the fork's centre of
gravity (TinyAI VMAF prediction, alt-metrics, codec-aware
predictor, predict-then-verify harness). Item 3 (local sidecar /
continual learning during encodes) has the data substrate but no
adaptive loop and no ADR. Item 4 (community-uploaded datasets) is
a clean greenfield gap.

Claude-bias: one real bug — the duplicate `quality_range` in
`tools/vmaf-tune/src/vmaftune/codec_adapters/x264.py:30,36`. No
fabricated APIs, no stale versions, no invented citations, no TODO
debt in fork-local code.
