<!-- markdownlint-disable MD013 -->
# `fr_regressor_v1` — full-reference VMAF score regressor (C1 baseline)

`fr_regressor_v1` is the Wave-1 **C1** baseline (full-reference scoring): a
tiny MLP that maps libvmaf's classical 6-feature vector
(`adm2`, `vif_scale0..3`, `motion2`) to a per-frame VMAF score. It is the
neural-network sibling of the production `vmaf_v0.6.1` SVR — same input,
same target — packaged as a 67-op-allowlisted ONNX so it can run inside
`libvmaf`'s tiny-AI inference path on every supported execution provider
(CPU / CUDA / OpenVINO / ROCm).

> **Status — refreshed 2026-05-20.** Originally shipped 2026-04-29
> (ADR-0249); refreshed after the May feature-extraction/default-path
> bugfix wave using `runs/full_features_netflix_refresh_20260520.parquet`.
> See [ADR-0647](../../adr/0647-ai-fr-regressor-v1-refresh-20260520.md).

## What the output means

The model emits a single scalar feature `score`, one value per frame
pair. Range matches `vmaf_v0.6.1`: `[0, 100]` typical, with > 100
clipped at the libvmaf level.

| Value | Interpretation |
| --- | --- |
| **0** | Catastrophic distortion |
| **~30** | Heavily compressed, blocky |
| **~60** | Visible artefacts but watchable |
| **~80** | Near-transparent on consumer displays |
| **100** | Indistinguishable from the reference |

PLCC against `vmaf_v0.6.1` per-frame on the Netflix Public 9-fold LOSO
hold-out is reported in `model/tiny/fr_regressor_v1.json` (sidecar
field `training.loso_mean_plcc`). The ship gate is **mean LOSO
PLCC ≥ 0.95**; the trainer refuses to overwrite the registry below
that threshold.

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model name | `fr_regressor_v1` |
| Location | `model/tiny/fr_regressor_v1.onnx` |
| Sidecar | `model/tiny/fr_regressor_v1.json` |
| Architecture | `FRRegressor` (2-layer GELU MLP, hidden=64, dropout=0.1) |
| Input | `features` — `[N, 6]` float32, standardised (mean / std in sidecar) |
| Output | `score` — `[N]` float32, VMAF-scale |
| Feature order | `adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2` |
| ONNX opset | 17 |
| Training corpus | Netflix Public Dataset refresh table `runs/full_features_netflix_refresh_20260520.parquet` (11190 rows, 30 cols; source YUVs remain local-only) |
| Teacher | `vmaf_v0.6.1` per-frame score |
| Held-out PLCC | `0.9982 ± 0.0014` mean 9-fold LOSO |
| License | BSD-3-Clause-Plus-Patent (fork-local; checkpoint is non-redistributable Netflix data derivative — see *Provenance* below) |
| Exporter | `ai/scripts/train_fr_regressor.py` |

The sidecar JSON pins the training-time per-feature mean / std vector
under `feature_mean` / `feature_std`. **Callers must standardise their
input feature vector with the same statistics before invoking the
graph** — the standardisation is *not* baked into the ONNX so that
downstream consumers can substitute a different feature pool without
re-exporting.

The sidecar also carries `run_provenance` (`ai-run-provenance-v1`):
the trainer entrypoint, parsed arguments, input parquet path/hash, and
output model/sidecar/registry/metrics targets. The metrics JSON written
by `--metrics-out` carries the same block, including failed ship-gate
runs where no ONNX is exported.

### 2026-05-20 refresh metrics

| Source | PLCC | SROCC | RMSE |
| --- | ---: | ---: | ---: |
| BigBuckBunny | 0.9962 | 0.6277 | 4.466 |
| BirdsInCage | 0.9993 | 0.9997 | 1.854 |
| CrowdRun | 0.9997 | 0.9998 | 1.005 |
| ElFuente1 | 0.9992 | 0.9963 | 1.635 |
| ElFuente2 | 0.9964 | 0.9969 | 3.169 |
| FoxBird | 0.9967 | 0.9954 | 2.352 |
| OldTownCross | 0.9992 | 0.9998 | 1.931 |
| Seeking | 0.9989 | 0.9962 | 1.982 |
| Tennis | 0.9982 | 0.9982 | 1.356 |

Summary: mean PLCC `0.9982 ± 0.0014`, mean SROCC
`0.9567 ± 0.1234`, mean RMSE `2.194 ± 1.049`. BigBuckBunny has weak
rank ordering but high linear agreement; the C1 ship gate remains the
ADR-0249 PLCC gate.

## Provenance

The training corpus (`.workingdir2/netflix/`) is the Netflix Public
Dataset, distributed by Netflix under a license that forbids
redistribution. The shipped ONNX is a derivative: parameters were
fitted to per-frame `vmaf_v0.6.1` teacher scores computed locally on
that corpus. The fork ships the resulting ONNX (~few KB of
parameters) under BSD-3-Clause-Plus-Patent on the basis that the
parameter values are a derived statistical summary, not a redistribution
of the YUV bitstreams or the (separately access-gated) DMOS sidecar
CSV. If your jurisdiction reads "derivative work" more broadly, treat
the checkpoint as Netflix-license-encumbered and rebuild from your
own copy of the dataset.

## Usage — CLI

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --tiny-model fr_regressor_v1 \
    --tiny-device auto \
    --output score.json
```

`--tiny-device auto` resolves to the best available execution provider
(CUDA → OpenVINO → CPU). The CPU path is a hard requirement — every
shipped tiny model must run there as the variance-anchor (see
[ADR-0214](../../adr/0214-gpu-parity-ci-gate.md)).

## Usage — Python

```python
import json
import numpy as np
import onnxruntime as ort

# 1. Load the ONNX + the per-feature standardisation from the sidecar.
sidecar = json.loads(open("model/tiny/fr_regressor_v1.json").read())
mean = np.asarray(sidecar["feature_mean"], dtype=np.float32)
std = np.asarray(sidecar["feature_std"], dtype=np.float32)

sess = ort.InferenceSession("model/tiny/fr_regressor_v1.onnx",
                            providers=["CPUExecutionProvider"])

# 2. Compute the canonical-6 features per frame using libvmaf
#    (e.g. via ai.data.feature_extractor.extract_features).
feats = ...  # shape (n_frames, 6) in the order documented above

# 3. Standardise and run.
x = (feats - mean) / std
scores = sess.run(["score"], {"features": x.astype(np.float32)})[0]
```

## Re-training

```bash
# 1. Make sure a fresh Netflix feature table exists. Regenerate via:
python ai/scripts/extract_full_features.py \
    --data-root .workingdir2/netflix \
    --vmaf-bin core/build-cpu/tools/vmaf \
    --out runs/full_features_netflix_refresh_YYYYMMDD.parquet

# 2. Train + export (defaults match the shipped checkpoint).
PYTHONPATH=ai/src python ai/scripts/train_fr_regressor.py \
    --parquet runs/full_features_netflix_refresh_YYYYMMDD.parquet
```

The trainer:

1. Runs a 9-fold leave-one-source-out (LOSO) sweep over the parquet,
   reporting per-fold PLCC / SROCC / RMSE.
2. **Refuses to ship** if the mean LOSO PLCC < 0.95 (configurable
   via `--ship-threshold`; lowering the threshold is a soft-fail
   *of policy* — fix the model, not the gate).
3. Re-trains a final model on all 9 sources and exports it to
   `model/tiny/fr_regressor_v1.onnx`, updating the sidecar +
   registry sha256.

Idempotent: re-running with the same seed + parquet produces the
same ONNX bytes (modulo torch / onnx producer-string drift).

## Known limitations

- **Canonical-6 input only.** Larger feature pools (subsets A / B /
  full-21) live in `ai/scripts/phase3_subset_sweep.py`; ship-grade
  models from those subsets are tracked separately.
- **Netflix Public corpus only.** Generalisation to UGC / live-encode /
  HDR is not validated. C2 (`nr_metric_v1`) covers UGC; HDR is on the
  Wave-1 follow-up backlog.
- **vmaf_v0.6.1 as teacher.** Inherits its biases (banding insensitive,
  over-confident on heavily-compressed cartoon content, etc.). MOS
  alignment is transitive through the Netflix Public DMOS that
  `vmaf_v0.6.1` was originally calibrated against.
- **Static input shape.** Only the batch axis is dynamic
  (`[N, 6]`); the feature dimension is pinned at 6.
- **Rank correlation caveat on BigBuckBunny.** The 2026-05-20 refresh
  keeps high PLCC (`0.9962`) on BigBuckBunny but low SROCC
  (`0.6277`). Treat this model as a PLCC-aligned teacher-score
  regressor, not a per-content ranker.

## See also

- [overview.md](../overview.md) — where C1 fits in the C1–C4 capability map.
- [roadmap.md §2.1](../roadmap.md) — Wave-1 ship-baselines table.
- [training.md](../training.md) — `vmaf-train` CLI and dataset flow.
- [training-data.md](../training-data.md) — Netflix corpus layout +
  feature parquet schema.
- [benchmarks.md](../benchmarks.md) — PLCC/SROCC/RMSE methodology.
- [security.md](../security.md) — ONNX op-allowlist + registry sha256 pinning.
- [ADR-0168](../../adr/0168-tinyai-konvid-baselines.md) — C2 + C3
  baselines (sibling).
- [ADR-0249](../../adr/0249-fr-regressor-v1.md) — this model's decision record.
- [ADR-0647](../../adr/0647-ai-fr-regressor-v1-refresh-20260520.md) — 2026-05-20 refresh.
