# `konvid_mos_head_v1` — KonViD subjective-MOS head v1 (ADR-0325 Phase 3)

`konvid_mos_head_v1` is the fork's first head trained directly against
**subjective MOS ratings** (not the libvmaf VMAF teacher score). It
maps the canonical-6 libvmaf features + saliency mean/var + 3 TransNet
shot-metadata columns + a single-slot ENCODER_VOCAB v4 one-hot to a
scalar MOS prediction in `[1.0, 5.0]`.

> **Status: Proposed — synthetic-corpus checkpoint (Phase 3 placeholder).**
> The shipped ONNX is trained on a deterministic-seeded synthetic
> corpus per ADR-0325 §Phase 3 because the real KonViD-150k JSONL
> drop is in flight under [PR #447](https://github.com/VMAFx/vmafx/pull/447).
> The head **production-flip is blocked on the real-corpus retrain** —
> see §Production-flip gate below for the threshold and §Honest gate
> verdict for the synthetic-corpus run that produced the shipped
> checkpoint. Per the user direction (memory
> `feedback_no_test_weakening`) and ADR-0325 §Production-flip gate,
> the threshold is **not lowered** when the real-corpus retrain
> misses — the head ships ``Status: Proposed`` instead.

## Why this exists

The fork's `nr_metric_v1` (no-reference metric, [ADR-0168])
and `fr_regressor_v2_ensemble` (codec-aware FR regressor,
[ADR-0303]) both target *VMAF*, not raw subjective MOS. The
ChatGPT-vision audit ([Research-0086]) flagged this gap: open-weight
competitors (DOVER-Mobile @ PLCC 0.853 KoNViD, Q-Align) ship MOS
predictors, and the fork could not honestly claim "we predict human
MOS" without a head trained against subjective ratings.

KonViD-1k / KonViD-150k provide ≥5 crowdworker MOS ratings per clip
on real-world UGC content — exactly the subjective ground truth
this head needs. Phase 3 of ADR-0325 trains the head; Phases 1/2
ingest the corpora.

## Inputs

Two named tensors, dynamic batch axis (matches the existing
`vmaf_dnn_session_run` two-input contract from [ADR-0040]):

- **`features`**, shape `(N, 11)` — float32. The MLP carries a
  `LayerNorm` at its input layer so the per-feature mean/std bake-in
  is informational; the manifest still records the corpus-level
  `feature_mean` / `feature_std` for downstream replication.

  | Index | Feature              | Source                                    |
  |-------|----------------------|-------------------------------------------|
  | 0     | `adm2`               | libvmaf canonical-6                       |
  | 1     | `vif_scale0`         | libvmaf canonical-6                       |
  | 2     | `vif_scale1`         | libvmaf canonical-6                       |
  | 3     | `vif_scale2`         | libvmaf canonical-6                       |
  | 4     | `vif_scale3`         | libvmaf canonical-6                       |
  | 5     | `motion2`            | libvmaf canonical-6                       |
  | 6     | `saliency_mean`      | `saliency_student_v1` ([ADR-0286])        |
  | 7     | `saliency_var`       | `saliency_student_v1` ([ADR-0286])        |
  | 8     | `shot_count_norm`    | TransNet v2 ([ADR-0223]) — `log10(1+N)/3` |
  | 9     | `shot_mean_len_norm` | TransNet v2 ([ADR-0223]) — seconds / 30   |
  | 10    | `shot_cut_density`   | TransNet v2 ([ADR-0223]) — cuts / frame   |

- **`encoder_onehot`**, shape `(N, 1)` — float32. Always `[1.0]`;
  the single slot is `"ugc-mixed"` per ENCODER_VOCAB v4
  ([ADR-0325 §Phase 2]). The 1-D shape is forward-compatible with
  multi-slot expansion (e.g. when the fork ingests LSVQ +
  YouTube-UGC).

## Output

`mos`, shape `(N,)` — float32 MOS prediction in `[1.0, 5.0]`. The
range clamp is built into the graph as `1.0 + 4.0 * sigmoid(raw)`,
so the head cannot emit out-of-range MOS even on adversarial input.

## Architecture

Small MLP — ONNX-allowlist conformant ([ADR-0258] / [ADR-0169]):

```text
LayerNorm(12)
  → Linear(12, 64) → ReLU → Dropout(0.1)
  → Linear(64, 64) → ReLU → Dropout(0.1)
  → Linear(64, 1)
  → Sigmoid + affine to [1, 5]
```

- **Total parameters**: 5,081.
- **Opset**: 17.
- **Ops emitted**: `LayerNormalization`, `Concat`, `Gemm`, `Relu`,
  `Sigmoid`, `Add`, `Mul`, `Squeeze` — all on the fork's ONNX
  op-allowlist (`core/src/dnn/op_allowlist.c`).

The parameter count is below the ~30K–100K range from the task
brief. The brief allowed a wider range to absorb future feature
expansion; the actual architecture lands smaller because the
canonical-6 + saliency + shot-metadata feature shape is already
well-summarised and a deeper MLP overfits the synthetic corpus.

## Training corpus

When `--smoke` is passed (or no real corpus is on disk), the trainer
synthesises a deterministic-seeded 600-row corpus per
`_synthesize_corpus()` in
[`ai/scripts/train_konvid_mos_head.py`](../ai/scripts/train_konvid_mos_head.py).
The synthetic generator picks plausible canonical-6 ranges and
derives a smooth nonlinear MOS target as a sum of monotone terms in
`adm2`, `motion2`, `saliency_mean`, `vif_scale3`, plus rater-noise
σ = 0.10 — that is the corpus the shipped ONNX was trained against
(seed `20260508`, 3 epochs, 5 folds × 5 seeds).

When the real KonViD-1k / KonViD-150k JSONL drops land
([PR #440] / [PR #447]) the trainer prefers them automatically:

```bash
python ai/scripts/train_konvid_mos_head.py \
    --konvid-1k     ~/.workingdir2/konvid-1k/konvid_1k.jsonl   \
    --konvid-150k   ~/.workingdir2/konvid-150k/konvid_150k.jsonl
```

The Phase 1/2 corpus rows do not yet carry the canonical-6 / saliency
/ shot-metadata columns; the trainer accepts whichever subset of those
columns the row carries and content-independent-zero-fills the rest.
Each follow-up PR that lands a column collapses to a no-op retrain
under the same seed.

## Production-flip gate

Per ADR-0325 Phase 3 (mirrors [ADR-0303] shape):

| Metric                 | Threshold        |
|------------------------|------------------|
| Mean PLCC across folds | **≥ 0.85**       |
| Mean SROCC             | **≥ 0.82**       |
| Mean RMSE              | **≤ 0.45 MOS**   |
| Max-min PLCC spread    | **≤ 0.005**      |

Threshold values come from ADR-0325; PLCC `≥ 0.85` is calibrated
against DOVER-Mobile's published 0.853 PLCC on KoNViD as the
external benchmark. The gate is **not lowered on real-corpus
failures** — see Status note above.

The synthetic-corpus surrogate gate (placeholder for the gate-test
harness when no real data is on disk) is mean PLCC `≥ 0.75`.

## Honest gate verdict — synthetic corpus, this PR

The ONNX shipped in this PR was produced by a deterministic
synthetic-corpus run; the per-fold metrics are reproduced verbatim
from the trainer's stdout:

| Fold     | PLCC       | SROCC      | RMSE       | n_val |
|----------|------------|------------|------------|-------|
| 0        | 0.8677     | 0.8831     | 0.2565     | 120   |
| 1        | 0.8854     | 0.9356     | 0.2138     | 120   |
| 2        | 0.8017     | 0.8453     | 0.3079     | 120   |
| 3        | 0.8839     | 0.9442     | 0.2263     | 120   |
| 4        | 0.8596     | 0.8938     | 0.2291     | 120   |
| **Mean** | **0.8597** | **0.9004** | **0.2467** | —     |

PLCC spread (max − min) = `0.0836`. The synthetic-corpus PLCC
clears the surrogate threshold (`≥ 0.75`) but **not** the real-
corpus threshold (`≥ 0.85` mean PLCC + `≤ 0.005` spread). That is
expected: synthetic noise σ = 0.10 is honestly noisier than real
KonViD inter-rater consistency, and the synthetic gate is set
loose deliberately so the smoke run does not flake on per-fold
sampling. **Production flip is blocked on the real-corpus retrain.**

The exact per-fold numbers are reproducible bit-for-bit:

```bash
python ai/scripts/train_konvid_mos_head.py --smoke
```

## Predictor integration

`tools/vmaf-tune/src/vmaftune/predictor.py` exposes
`Predictor.predict_mos(features, codec)` ([ADR-0325 Phase 3]):

- When `model/konvid_mos_head_v1.onnx` is present **and**
  `onnxruntime` is importable, the call loads the ONNX once and
  returns the head's prediction.
- When either is missing, the call falls back to a documented
  linear approximation: `mos = (predicted_vmaf − 30) / 14`,
  clamped to `[1, 5]`. The fallback is deliberately approximate
  (VMAF 30 → MOS 0; VMAF 100 → MOS 5) so callers without the head
  still get a plausible 5-point estimate; the model card flags it
  as approximate, not authoritative.

The fallback path keeps `vmaf-tune` usable on hosts without the
ML stack and on dev branches that haven't pulled the head yet.

## License + redistribution

The training corpus (KonViD-1k / KonViD-150k) is **not**
redistributed — it stays local under `~/.workingdir2/konvid-{1k,150k}/`
per [ADR-0325] §Constraint 1. Only the *derived* ONNX +
manifest sidecar redistribute, under the fork's BSD-3-Clause-Plus-
Patent licence.

## See also

- [ADR-0336] — KonViD MOS head v1 decision record (this PR).
- [ADR-0325] — KonViD-150k corpus ingestion + MOS-head plan
  (parent ADR; defines the production-flip gate this card cites).
- [ADR-0303] — fr_regressor_v2_ensemble production-flip protocol
  (this gate inherits the shape).
- [PR #440] — KonViD-1k Phase 1 ingestion.
- [PR #447] — KonViD-150k Phase 2 ingestion (in flight).
- [Research-0086] — KonViD-150k corpus feasibility audit.

[ADR-0040]: ../docs/adr/0040-tinyai-loader.md
[ADR-0168]: ../docs/adr/0168-tinyai-konvid-baselines.md
[ADR-0169]: ../docs/adr/0169-onnx-allowlist-loop-if.md
[ADR-0223]: ../docs/adr/0223-transnet-v2-shot-detector.md
[ADR-0258]: ../docs/adr/0258-onnx-allowlist-resize.md
[ADR-0286]: ../docs/adr/0286-saliency-student-fork-trained-on-duts.md
[ADR-0303]: ../docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md
[ADR-0325]: ../docs/adr/0325-konvid-150k-corpus-ingestion.md
[ADR-0336]: ../docs/adr/0336-konvid-mos-head-v1.md
[ADR-0325 §Phase 2]: ../docs/adr/0325-konvid-150k-corpus-ingestion.md
[ADR-0325 Phase 3]: ../docs/adr/0336-konvid-mos-head-v1.md
[Research-0086]: ../docs/research/0086-konvid-150k-corpus-feasibility.md
[PR #440]: https://github.com/VMAFx/vmafx/pull/440
[PR #447]: https://github.com/VMAFx/vmafx/pull/447
