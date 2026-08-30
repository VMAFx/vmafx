# Tiny AI — benchmarks

How to produce comparable numbers for the tiny-AI models and how to read
them. The table below is a registry snapshot of shipped model-card
metrics; regenerate the per-model reports before using any row for a
release claim.

## Accuracy methodology

For FR (C1) and NR (C2) models, the three canonical regression metrics:

- **PLCC** — Pearson linear correlation with MOS.
- **SROCC** — Spearman rank-order correlation.
- **RMSE** — root mean square error against MOS (0–100 scale).

All three are computed by `vmaf-train eval` on the held-out **test split**
produced by `vmaf_train.data.splits.split_keys` with the fixed salt
`vmaf-train-splits-v1`. Splits are deterministic so baseline and
challenger see the same frames/keys.

```bash
vmaf-train eval \
    --model model/tiny/vmaf_tiny_fr_v1.onnx \
    --features ai/data/nflx_features.parquet \
    --split test
```

### Baseline: upstream `vmaf_v0.6.1` SVM

To compare a new tiny FR model against the upstream SVM, score the same
test pairs through both and run `ai/tests/test_eval_metrics.py` helper
functions. Keep the baseline's version in the committed report for
auditability.

## Runtime methodology

```bash
# Frames/second, end-to-end, single-thread CPU:
./testdata/bench_all.sh --tiny-model model/tiny/vmaf_tiny_fr_v1.onnx --backend=cpu

# GPU throughput:
./testdata/bench_all.sh --tiny-model model/tiny/vmaf_tiny_fr_v1.onnx --backend=cuda
```

The `testdata/bench_all.sh` harness logs into
`testdata/netflix_benchmark_results.json` (never committed — ad-hoc run
artefact). Collect multiple runs and report median + p99.

## Shipped-score snapshot

| Model | Target | Validation summary | Runtime note |
| --- | --- | --- | --- |
| `fr_regressor_v1` | FR VMAF-teacher score | Netflix Public Dataset 9-fold LOSO mean PLCC `0.9977 ± 0.0025` | Tiny MLP over canonical-6 features; standardisation lives in the sidecar. |
| `fr_regressor_v2` | FR codec-aware VMAF-teacher score | Phase-A corpus in-sample PLCC `0.9794`; promoted by ADR-0291's LOSO gate | Adds codec / preset / CRF conditioning. |
| `fr_regressor_v3` | FR codec-aware VMAF-teacher score | LOSO mean PLCC `0.9975`, gate `>= 0.95` | Current 16-slot encoder-vocab model. |
| `vmaf_tiny_v2` | FR VMAF-teacher score | Netflix LOSO PLCC `0.9978 ± 0.0021`; KoNViD 5-fold PLCC `0.9998` | Production tiny fusion default; StandardScaler baked into ONNX. |
| `vmaf_tiny_v3` | FR VMAF-teacher score | Netflix LOSO PLCC `0.9986 ± 0.0015`; train-set RMSE `0.112` | Higher-capacity opt-in model; int8 sidecar available. |
| `vmaf_tiny_v4` | FR VMAF-teacher score | Netflix LOSO PLCC `0.9987 ± 0.0015` | Largest shipped tiny fusion model; opt-in. |
| `dists_sq_placeholder_v0` | FR perceptual-distance smoke | No perceptual-quality score claimed; registry row is `smoke: true` | ABI / ORT two-input smoke checkpoint only. |
| `mobilesal_placeholder_v0` | NR saliency smoke | Superseded for production ROI by `saliency_student_v1`; registry row is `smoke: true` | Retained to preserve the historical MobileSal I/O contract. |

Runtime throughput depends on ORT execution provider, CPU ISA, and GPU
driver. Record measured CPU / CUDA / SYCL / OpenVINO numbers in the
individual model card or release note for the exact build under test
rather than maintaining a single stale global table here.

## Model-size targets

| Model class | Target size | Typical |
| --- | --- | --- |
| C1 (FR MLP) | ≤ 100 KB | ~50 KB |
| C2 (NR CNN) | ≤ 5 MB | ~2 MB |
| C3 (learned filter) | ≤ 2 MB | ~800 KB |

Models larger than `VMAF_DNN_DEFAULT_MAX_BYTES` (50 MB, compile-time
constant) are rejected at load time. The historical
`VMAF_MAX_MODEL_BYTES` env override was retired in T7-12 — tiny-AI is
tiny by definition, and if a candidate model balloons past the targets
the design is wrong, not the limit.

## Determinism in benchmarks

Same `--seed` + same `train_commit` + same dataset manifest SHA should
reproduce the reported scores within a tight allclose. CI includes a
float-rounding guard so drift ≥ 1e-3 on the primary FR metric trips a
regression failure.
