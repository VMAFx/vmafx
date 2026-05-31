<!-- markdownlint-disable MD013 MD060 -->
# `konvid_mos_head_v1` — KonViD subjective MOS head v1

`konvid_mos_head_v1` is the fork's first model trained directly against
**subjective Mean Opinion Score ratings** rather than a libvmaf VMAF teacher
score. It maps 11 input features to a scalar MOS prediction in [1.0, 5.0].

> **Status: Proposed — synthetic-corpus placeholder checkpoint.**
> The ONNX shipped with this model was trained on a deterministic-seeded
> synthetic corpus (PR #491). A real-corpus retrain against the KonViD-1k
> and KonViD-150k JSONL drops is required before the head can be
> promoted to production. The production-flip gate is documented in
> §Production-flip gate below.

- **ADR**: [ADR-0336](../../adr/0336-konvid-mos-head-v1.md) (decision record) and
  [ADR-0325](../../adr/0325-konvid-150k-corpus-ingestion.md) (parent KonViD ingestion plan)
- **Introduced in**: PR #491 (ADR-0325 Phase 3)
- **ONNX file**: `model/konvid_mos_head_v1.onnx`
- **Manifest sidecar**: `model/konvid_mos_head_v1.json`
- **Model card (canonical source)**: `model/konvid_mos_head_v1_card.md`
- **Opset**: 17
- **Parameters**: 5 081
- **Corpus**: KonViD-1k (1 200 clips) + KonViD-150k (~150 000 clips)

## Why this model exists

The fork's `fr_regressor_v2_ensemble` and `nr_metric_v1` both predict VMAF,
not raw subjective MOS. Competitors (DOVER-Mobile, Q-Align) publish MOS
predictors; without a head trained against crowdworker ratings the fork
cannot honestly compare against them on subjective benchmarks. KonViD ships
at least 5 crowdworker ratings per clip on a 1–5 ACR Likert scale — exactly
the subjective ground truth this head needs.

## Inputs

Two named tensors (dynamic batch axis, matches the `vmaf_dnn_session_run`
two-input contract from ADR-0040):

**`features`** — shape `(N, 11)`, float32

| Index | Feature              | Source                          |
|-------|----------------------|---------------------------------|
| 0     | `adm2`               | libvmaf canonical-6             |
| 1     | `vif_scale0`         | libvmaf canonical-6             |
| 2     | `vif_scale1`         | libvmaf canonical-6             |
| 3     | `vif_scale2`         | libvmaf canonical-6             |
| 4     | `vif_scale3`         | libvmaf canonical-6             |
| 5     | `motion2`            | libvmaf canonical-6             |
| 6     | `saliency_mean`      | `saliency_student_v1` (ADR-0286)|
| 7     | `saliency_var`       | `saliency_student_v1` (ADR-0286)|
| 8     | `shot_count_norm`    | TransNet v2 (ADR-0223): `log10(1+N)/3` |
| 9     | `shot_mean_len_norm` | TransNet v2 (ADR-0223): seconds / 30 |
| 10    | `shot_cut_density`   | TransNet v2 (ADR-0223): cuts / frame |

The model includes a `LayerNorm` at its input, so raw (unnormalised)
feature values are acceptable. The manifest sidecar records corpus-level
`feature_mean` / `feature_std` for downstream replication.

**`encoder_onehot`** — shape `(N, 1)`, float32. Always `[1.0]`; the single
slot encodes `"ugc-mixed"` (ENCODER_VOCAB v4, ADR-0325 Phase 2). The 1-D
shape is forward-compatible with multi-slot expansion.

## Output

**`mos`** — shape `(N,)`, float32. Predicted MOS in [1.0, 5.0]. The range
clamp is built into the graph as `1.0 + 4.0 * sigmoid(raw)` so the model
cannot emit out-of-range values.

## Architecture

Small MLP on the fork's ONNX op-allowlist (ADR-0258 / ADR-0169):

```text
LayerNorm(12)
  → Linear(12, 64) → ReLU → Dropout(0.1)
  → Linear(64, 64) → ReLU → Dropout(0.1)
  → Linear(64, 1)
  → Sigmoid + affine to [1.0, 5.0]
```

Ops emitted: `LayerNormalization`, `Concat`, `Gemm`, `Relu`, `Sigmoid`,
`Add`, `Mul`, `Squeeze` — all on the allowlist in
`core/src/dnn/op_allowlist.c`.

## Training

Train or retrain using `ai/scripts/train_konvid_mos_head.py`:

```bash
# Smoke — no real corpus needed (~30 s, deterministic seed):
python ai/scripts/train_konvid_mos_head.py --smoke

# Production — real KonViD JSONL drops on disk:
python ai/scripts/train_konvid_mos_head.py \
    --konvid-1k   .workingdir2/konvid-1k/konvid_1k.jsonl \
    --konvid-150k .workingdir2/konvid-150k/konvid_150k.jsonl \
    --out-onnx    model/konvid_mos_head_v1.onnx \
    --out-manifest model/konvid_mos_head_v1.json

# Production — refreshed feature parquet already carrying MOS labels:
python ai/scripts/train_konvid_mos_head.py \
    --konvid-1k /tmp/no-konvid-1k.jsonl \
    --konvid-150k /tmp/no-konvid-150k.jsonl \
    --feature-parquet runs/full_features_konvid_refresh_20260520_with_mos.parquet \
    --out-onnx model/konvid_mos_head_v1.onnx \
    --out-manifest model/konvid_mos_head_v1.json

# Key flags:
#   --epochs N          training epochs (default 30)
#   --k-folds N         cross-validation folds (default 5)
#   --seed N            RNG seed (default 20260508)
#   --no-export         skip ONNX write (dev / dry-run)
```

The real path requires labelled rows. A parquet produced by feature extraction
must contain `mos` or `mos_raw_0_100`; otherwise the trainer exits with code
`2` and writes no checkpoint. Use
[`materialize_mos_labels.py`](../mos-label-materializer.md) to join labels,
and reserve `--smoke` for synthetic CI/load-path checks.

CHUG HDR subjective-MOS training uses the CHUG-specific wrapper
`ai/scripts/train_chug_hdr_mos_head.py`; do not pass CHUG shards through
the KonViD-named flags. That wrapper defaults to its own
`chug-hdr-wide-v1` schema and writes local-only `chug_hdr_mos_head_v1`
manifests under `.workingdir2/chug/`; it does not change this committed
11-feature KonViD model contract.

Both KonViD and CHUG MOS-head manifests include `run_provenance`. The
block records the user-facing entrypoint script, CLI arguments, named
input/output paths, and file hashes where the files exist. CHUG wrapper
runs keep `train_chug_hdr_mos_head.py` as `entrypoint` and record the
shared `train_konvid_mos_head.py` implementation as `shared_trainer`.

For target-panel HDR experiments, add
`--display-profile-json <profile.json>`. When no explicit
`--feature-schema` is passed, that flag selects `chug-hdr-display-v1`
and appends normalized display context to the CHUG-local feature order:
peak luminance, black level, log contrast ratio, ambient lux,
BT.2020/P3 coverage, OLED/QLED/LCD panel flags, local dimming, and
dynamic tone-mapping. The generated manifest records the profile values
and sha256 so the checkpoint can be traced back to the viewing context.

For corpus acquisition instructions see
[mos-corpora.md](../mos-corpora.md). The Phase 1 (KonViD-1k) and Phase 2
(KonViD-150k) adapters must have produced their JSONL drops before running
the trainer in production mode.

## Production-flip gate

Mirrors the ADR-0303 shape for `fr_regressor_v2_ensemble`:

| Metric               | Threshold  |
|----------------------|-----------|
| Mean PLCC across folds | **≥ 0.85** |
| Mean SROCC           | **≥ 0.82** |
| Mean RMSE            | **≤ 0.45 MOS units** |
| Max-min PLCC spread  | **≤ 0.005** |

The PLCC threshold of 0.85 is calibrated against DOVER-Mobile's published
0.853 PLCC on KoNViD-1k. A real-corpus run that misses any threshold ships
the model with `Status: Proposed`; thresholds are not lowered.

### Synthetic-corpus result (shipped checkpoint)

The ONNX at `model/konvid_mos_head_v1.onnx` was produced from 600
deterministic synthetic rows (seed `20260508`). The per-fold metrics
are reproduced verbatim from the trainer's stdout:

| Fold     | PLCC   | SROCC  | RMSE   | n_val |
|----------|--------|--------|--------|-------|
| 0        | 0.8677 | 0.8831 | 0.2565 | 120   |
| 1        | 0.8854 | 0.9356 | 0.2138 | 120   |
| 2        | 0.8017 | 0.8453 | 0.3079 | 120   |
| 3        | 0.8839 | 0.9442 | 0.2263 | 120   |
| 4        | 0.8596 | 0.8938 | 0.2291 | 120   |
| **Mean** | **0.8597** | **0.9004** | **0.2467** | — |

PLCC spread (max − min) = 0.0836. The synthetic surrogate gate (≥ 0.75
mean PLCC) is cleared; the real-corpus gate (≥ 0.85 mean PLCC, spread
≤ 0.005) is **not** — as expected from synthetic noise σ = 0.10.
Production flip is blocked on the real-corpus retrain.

Reproduce this exact run:

```bash
python ai/scripts/train_konvid_mos_head.py --smoke
```

## Predictor integration

`tools/vmaf-tune/src/vmaftune/predictor.py` exposes
`Predictor.predict_mos(features, codec)`:

- When `model/konvid_mos_head_v1.onnx` is present and `onnxruntime` is
  importable the call loads the ONNX once and returns the head's prediction.
- When either is absent the call falls back to a documented linear
  approximation: `mos = (predicted_vmaf − 30) / 14`, clamped to [1, 5].
  This fallback is approximate and is not authoritative; the model card
  flags it as such.

## License and redistribution

The training corpus (KonViD-1k / KonViD-150k) is **not** redistributed —
it remains local under `.workingdir2/` per ADR-0325 §Constraint 1.
The derived ONNX weights and manifest sidecar redistribute under the
fork's BSD-3-Clause-Plus-Patent licence.

## Feature coverage gap — speed_chroma / speed_temporal (ADR-0559)

`konvid_mos_head_v1` was trained on 11 features (canonical-6 + 5 saliency /
scene-transition signals). It does **not** consume `speed_chroma` or
`speed_temporal`. These CPU-only extractors (Netflix `speed_ported` branch,
ported to the fork's libvmaf as `core/src/feature/speed.c`) are expected
inputs to a future Netflix HDR VMAF model.

A `konvid_mos_head_v2` that includes speed features in its input vector will
require:

1. Re-extraction of the KoNViD-1k and KoNViD-150k corpora with speed features
   (scripts updated in ADR-0559 — re-extraction is tracked by the corpus agent).
2. Retraining with a wider input dimension (11 → 15).
3. A passing production-flip gate on real-corpus cross-validation.

Until then, callers that receive NaN for speed columns (pre-extraction corpora)
can drop those columns and run inference with the existing 11-feature model.

## See also

- [mos-corpora.md](../mos-corpora.md) — MOS-corpus ingestion family overview
- [konvid-1k-ingestion.md](../konvid-1k-ingestion.md) — Phase 1 corpus acquisition
- [konvid-150k-ingestion.md](../konvid-150k-ingestion.md) — Phase 2 corpus acquisition
- [ADR-0336](../../adr/0336-konvid-mos-head-v1.md) — decision record
- [ADR-0325](../../adr/0325-konvid-150k-corpus-ingestion.md) — KonViD ingestion plan and production-flip protocol
- [ADR-0303](../../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) — gate shape this model inherits
- [ADR-0559](../../adr/0559-feature-coverage-audit.md) — feature coverage audit
  that identified the speed-feature gap
