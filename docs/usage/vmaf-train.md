# `vmaf-train` — tiny-AI training harness CLI

`vmaf-train` is the Python entry point for the fork's tiny-AI training
infrastructure. It is the complement of `vmaf-tune` (encode automation)
and `vmaf` (scoring CLI): `vmaf-train` produces the ONNX models the
other two consume. Defined in `ai/src/vmaf_train/cli.py`; entry point
registered in `ai/pyproject.toml` `[project.scripts]`.

This page covers all 14 subcommands. For background on what the models
do, see [`docs/ai/overview.md`](../ai/overview.md). For the specific
training-corpus pipeline, see
[`docs/ai/training.md`](../ai/training.md).

## Install

```bash
pip install -e ai
vmaf-train --help
```

## Subcommands

### `extract-features`

Pre-compute libvmaf features over a corpus of (ref, dis) pairs and
write them to a parquet cache.

| Flag | Purpose |
| --- | --- |
| `--dataset PATH` | JSONL corpus (one row per pair) |
| `--output PATH` | Parquet output path |
| `--vmaf-binary PATH` | Override the libvmaf CLI binary |

```bash
vmaf-train extract-features \
  --dataset .workingdir2/netflix/netflix.jsonl \
  --output .workingdir2/netflix/full_features.parquet
```

### `fit`

Train an MLP model from a feature cache.

| Flag | Purpose |
| --- | --- |
| `--config PATH` | Training-config TOML |
| `--cache PATH` | Parquet feature cache (output of `extract-features`) |
| `--output PATH` | Model checkpoint output |
| `--epochs N` | Override max epochs |
| `--seed N` | Deterministic random seed |

### `tune`

Optuna hyper-parameter sweep around `fit`. Produces a study DB
selectable for resumption.

| Flag | Purpose |
| --- | --- |
| `--config PATH` | Training-config TOML |
| `--param NAME=lo,hi` | Repeatable parameter range |
| `--trials N` | Number of Optuna trials |
| `--study-name STR` | Study name (resumable) |
| `--storage URL` | Study storage URL (`sqlite:///path` etc) |
| `--cache PATH` | Parquet feature cache |
| `--output PATH` | Best-checkpoint output |

### `export`

Export a trained checkpoint to ONNX with the fork's allowlist-conformant
op set.

| Flag | Purpose |
| --- | --- |
| `--checkpoint PATH` | Lightning checkpoint input |
| `--output PATH` | ONNX output |
| `--model fr_regressor\|nr_metric\|learned_filter` | Architecture tag |
| `--opset N` | ONNX opset version |
| `--atol FLOAT` | PyTorch↔ONNX tolerance for the round-trip check |

### `eval`

Evaluate a trained ONNX model on a deterministic split.

| Flag | Purpose |
| --- | --- |
| `--model PATH` | ONNX model input |
| `--features PATH` | Parquet feature cache |
| `--split train\|val\|test` | Which split to score |
| `--input-name NAME` | ONNX input name (default `features`) |

Reports PLCC / SROCC / RMSE.

### `manifest-scan`

Walk a corpus directory and produce a JSONL manifest enumerating
(ref, dis, MOS) rows.

| Flag | Purpose |
| --- | --- |
| `--dataset PATH` | Output JSONL |
| `--root PATH` | Corpus root |
| `--mos-csv PATH` | Optional MOS CSV (joined by content_name) |

### `validate-norm`

Sanity-check a model's normalisation — verifies that the input
mean/std encoded in the model matches the corpus statistics it was
trained on. Surfaces silently-broken normalisation that would cause
inference to under-predict by 5–20 PLCC points.

| Flag | Purpose |
| --- | --- |
| `--model PATH` | ONNX model |
| `--features PATH` | Parquet feature cache |
| `--fail-on-warning` | Exit non-zero on any warning |
| `--json` | Emit JSON report instead of text |

### `profile`

Per-EP latency + memory profile for an ONNX model. Useful for picking
the right EP for a given target.

| Flag | Purpose |
| --- | --- |
| `--model PATH` | ONNX model |
| `--shape NAME=N,N,...` | Repeatable input-shape override |
| `--provider NAME` | Repeatable EP name (`CPUExecutionProvider`, `CUDAExecutionProvider`, ...) |
| `--warmup N` | Warmup iterations |
| `--iters N` | Measurement iterations |
| `--json` | JSON output |

### `audit-compat`

Walk every ONNX model in `model/` and verify each conforms to the
fork's op-allowlist (`core/src/dnn/op_allowlist.c`).

| Flag | Purpose |
| --- | --- |
| `--model-dir PATH` | Directory to walk |
| `--fail-on-warning` | Exit non-zero on any allowlist violation |

### `check-ops`

Single-model variant of `audit-compat`.

| Flag | Purpose |
| --- | --- |
| `--model PATH` | ONNX model to check |

### `audit-learned-filter`

Specialised auditor for `learned_filter_v1`-class models — verifies the
output stays close enough to the input to be a "filter" (rather than a
generative transform).

| Flag | Purpose |
| --- | --- |
| `--model PATH` | Filter ONNX |
| `--frames N` | Number of frames to audit |
| `--peak FLOAT` | Peak luminance for normalisation |
| `--input-name NAME` | ONNX input name |
| `--ssim-min FLOAT` | Minimum SSIM(input, output) gate |
| `--mean-shift-max FLOAT` | Maximum mean shift gate |
| `--std-ratio-max FLOAT` | Maximum std ratio gate |
| `--clip-fraction-max FLOAT` | Maximum clipped-pixel fraction gate |
| `--json` | JSON output |
| `--fail-on-warning` | Exit non-zero on any warning |

### `quantize-int8`

Dynamic / static post-training int8 quantisation per ADR-0173.

| Flag | Purpose |
| --- | --- |
| `--fp32 PATH` | fp32 ONNX input |
| `--output PATH` | int8 ONNX output |
| `--calibration PATH` | Calibration parquet (static PTQ) |
| `--input-name NAME` | ONNX input name |
| `--n-calibration N` | Calibration sample count |
| `--batch-size N` | Calibration batch size |
| `--rmse-gate FLOAT` | RMSE gate vs fp32 (per-sample) |
| `--json` | JSON output |

### `cross-backend`

Run the same model on multiple ORT EPs and report per-row delta —
catches EP-specific numerical regressions.

| Flag | Purpose |
| --- | --- |
| `--model PATH` | ONNX model |
| `--features PATH` | Parquet feature cache |
| `--provider NAME` | Repeatable EP name |
| `--shape NAME=N,N,...` | Optional input-shape override |
| `--n-rows N` | How many rows to score |
| `--atol FLOAT` | Per-row tolerance |
| `--json` | JSON output |
| `--fail-on-mismatch` | Exit non-zero if any row exceeds atol |

### `bisect-model-quality`

Walks an ordered list of model checkpoints and finds the first one
that violates a PLCC / SROCC / RMSE gate on a held-out feature cache.
Companion to `/bisect-model-quality` skill.

| Flag | Purpose |
| --- | --- |
| `models` | Positional list of ONNX checkpoint paths |
| `--features PATH` | Parquet feature cache |
| `--min-plcc FLOAT` | PLCC gate |
| `--min-srocc FLOAT` | SROCC gate |
| `--max-rmse FLOAT` | RMSE gate |
| `--input-name NAME` | ONNX input name |
| `--json` | JSON output |
| `--fail-on-first-bad` | Exit non-zero on the first bad model (default: walk full list and report) |

### `register`

Add a model to `model/tiny/registry.json` per ADR-0211.

| Flag | Purpose |
| --- | --- |
| `--model PATH` | ONNX model to register |
| `--kind fr\|nr\|filter` | Architecture tag |
| `--dataset NAME` | Training dataset identifier |
| `--license SPDX` | License SPDX identifier |
| `--train-commit SHA` | Training-commit SHA |
| `--train-config PATH` | Training-config path |
| `--manifest PATH` | Optional supplementary manifest |

## JSON report provenance

Every `vmaf-train` subcommand that writes a durable JSON report via `--json`
adds an ADR-0661 `run_provenance` block. This currently covers
`validate-norm`, `profile`, `audit-learned-filter`, `quantize-int8`,
`cross-backend`, and `bisect-model-quality`.

The block records:

- `entrypoint`: `ai/src/vmaf_train/cli.py` plus a SHA-256 of the CLI file.
- `argv` and `args`: the invoked command arguments and parsed option values.
- `inputs`: the model, feature table, calibration table, frame corpus, or model
  list used by the report.
- `outputs`: the JSON report path, plus generated model outputs where the
  command writes one, such as `quantize-int8 --output`.

Use that block when attaching reports to model cards, promotion PRs, or
regression investigations; it is the reproducibility pointer for the exact
files and thresholds behind the table.

## Common workflows

### From scratch: train + register a new fr_regressor

```bash
# 1. Pre-compute features
vmaf-train extract-features \
  --dataset .workingdir2/netflix/netflix.jsonl \
  --output .workingdir2/netflix/features.parquet

# 2. Tune hyper-parameters
vmaf-train tune \
  --config ai/configs/fr_regressor.toml \
  --cache .workingdir2/netflix/features.parquet \
  --output runs/fr_regressor_v1.ckpt \
  --trials 50

# 3. Export ONNX
vmaf-train export \
  --checkpoint runs/fr_regressor_v1.ckpt \
  --output model/tiny/fr_regressor_v1.onnx \
  --model fr_regressor

# 4. Audit op allowlist
vmaf-train check-ops --model model/tiny/fr_regressor_v1.onnx

# 5. Validate normalisation
vmaf-train validate-norm \
  --model model/tiny/fr_regressor_v1.onnx \
  --features .workingdir2/netflix/features.parquet \
  --fail-on-warning

# 6. Eval on test split
vmaf-train eval \
  --model model/tiny/fr_regressor_v1.onnx \
  --features .workingdir2/netflix/features.parquet \
  --split test

# 7. Register
vmaf-train register \
  --model model/tiny/fr_regressor_v1.onnx \
  --kind fr \
  --dataset netflix-public-drop \
  --license BSD-3-Clause-Plus-Patent \
  --train-commit "$(git rev-parse HEAD)" \
  --train-config ai/configs/fr_regressor.toml
```

### Quantise an existing fp32 model

```bash
vmaf-train quantize-int8 \
  --fp32 model/tiny/vmaf_tiny_v3.onnx \
  --output model/tiny/vmaf_tiny_v3.int8.onnx \
  --calibration .workingdir2/netflix/features.parquet \
  --rmse-gate 0.5 \
  --json
```

## Related

- [`vmaf-tune`](vmaf-tune.md) — encode automation that consumes the
  models produced here.
- [`vmaf`](cli.md) — scoring CLI (`--tiny-model` flag).
- [`docs/ai/training.md`](../ai/training.md) — training-corpus pipeline.
- [`docs/ai/quantization.md`](../ai/quantization.md) — post-training
  quantisation (ADR-0173).
