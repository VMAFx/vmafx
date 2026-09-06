<!-- markdownlint-disable MD060 -->
# Tiny AI — training

Everything happens through `vmaf-train`, the typer CLI in
[`ai/`](../../ai/). Five subcommands: `extract-features`, `fit`, `export`,
`eval`, `register`.

## Install

```bash
pip install -e ai
# optional extras
pip install -e 'ai[tune,viz]'
```

This pulls `torch>=2.12.0,<3.0` + `pytorch-lightning>=2.6.5,<3.0` (the
`lightning` PyPI package was renamed to `pytorch-lightning` on 2026-04-30).
If you have a GPU-capable PyTorch wheel installed separately, the extras will
not reinstall it.

## Dataset acquisition

Datasets are NOT committed. `ai/src/vmaf_train/data/datasets.py` knows
five canonical sources and caches them under
`${VMAF_DATA_ROOT:-~/.cache/vmaf-train}/datasets/<name>/`. Each dataset
ships a `manifests/<name>.yaml` SHA-256 manifest so downloads are
verifiable.

| Dataset | Use | License | Purpose |
| --- | --- | --- | --- |
| Netflix Public (NFLX) | C1, C2 | Netflix research | Same source as upstream `vmaf_v0.6.1` |
| KoNViD-1k | C2 | CC BY 4.0 | NR-friendly UGC clips with MOS |
| LIVE-VQC | C2 | Academic | NR validation |
| YouTube-UGC | C2 | CC BY 3.0 | Large-scale NR |
| BVI-DVC | C3 | Academic | Encoder distortion pairs for learned filters |

You are responsible for complying with each dataset's license. The
manifests only record hashes, not bytes.

## C1 — FR regressor walkthrough

```bash
# 1. Extract feature vectors from the NFLX pairs using the existing
#    libvmaf CPU backend.
vmaf-train extract-features \
    --dataset nflx \
    --vmaf-binary core/build-cpu/tools/vmaf \
    --output ai/data/nflx_features.parquet

# 2. Train a 2-layer MLP on the extracted features.
vmaf-train fit \
    --config ai/configs/fr_tiny_v1.yaml \
    --features ai/data/nflx_features.parquet \
    --output runs/fr_tiny_v1/

# 3. Export the trained weights to ONNX and validate roundtrip
#    (torch eval vs onnxruntime within atol=1e-5).
vmaf-train export \
    --checkpoint runs/fr_tiny_v1/last.ckpt \
    --output model/tiny/vmaf_tiny_fr_v1.onnx \
    --opset 17

# 4. Hold-out evaluation.
vmaf-train eval \
    --model model/tiny/vmaf_tiny_fr_v1.onnx \
    --features ai/data/nflx_features.parquet \
    --split test
# → PLCC, SROCC, RMSE vs MOS.

# 5. Write a sidecar and register into model/tiny/.
vmaf-train register \
    --model model/tiny/vmaf_tiny_fr_v1.onnx \
    --kind fr \
    --dataset nflx \
    --license CDLA-Permissive-2.0 \
    --train-commit "$(git rev-parse HEAD)"
```

The sidecar `model/tiny/vmaf_tiny_fr_v1.json` pins:

```json
{
  "schema_version": 1,
  "name": "vmaf_tiny_fr_v1",
  "kind": "fr",
  "onnx_opset": 17,
  "input_name": "features",
  "output_name": "score",
  "input_normalization": { "mean": [...], "std": [...] },
  "expected_output_range": [0.0, 100.0],
  "dataset": "nflx",
  "train_commit": "…",
  "train_config_hash": "sha256:…",
  "license": "CDLA-Permissive-2.0"
}
```

## Run provenance sidecars

Training, evaluation, and validation scripts that emit durable JSON reports
should emit a `run_provenance` block. The shared helper is
`aiutils.run_manifest.build_run_provenance()` when the script embeds
provenance into an existing stable report schema. New standalone sidecars
should use `aiutils.run_manifest.write_run_manifest()` so the repeated
`schema` + adapter counters/config + `run_provenance` envelope is not copied
between scripts. The Claude workflow for adding or auditing those sidecars is
`.claude/skills/ai-run-manifest/SKILL.md`.

Operator-facing AI scripts should also use the small CLI helper layer in
`aiutils.cli_helpers` when they fit the shared shape. `make_argument_parser()`
keeps parser formatting consistent, `collect_cli_argv()` preserves the raw
argument vector for provenance, and `add_batch_manifest_arguments()` owns the
standard batch-runner flags:

| Helper | Use |
|---|---|
| `make_argument_parser()` | Standard parser construction for AI scripts. |
| `collect_cli_argv()` | Canonical raw-argv capture before parsing. |
| `add_batch_manifest_arguments()` | Shared `--manifest`, `--base-dir`, report-output, fail-fast, and optional row-failure flags for batch materializers. |

Table-specific defaults, row schemas, and materializer options stay in the
individual scripts; the helper only covers boilerplate that should not drift.

Directly executable `ai/scripts/*.py` files should use
`ai/scripts/_script_bootstrap.py` before importing shared repo-local modules.
`bootstrap_ai_script(__file__)` resolves the script path, repository root,
`ai/src`, `ai/scripts`, and the optional `tools/vmaf-tune/src` root without
copying ad hoc `sys.path.insert(...)` blocks into every script. Enable only the
roots the script needs:

| Bootstrap option | Use |
|---|---|
| default | Import `aiutils` from `ai/src`. |
| `include_repo_root=True` | Import repo-root packages such as `ai.data`. |
| `include_ai_scripts=True` | Import sibling materializers or feature extractors from `ai/scripts`. |
| `include_vmaf_tune_src=True` | Import `vmaftune` helpers for table materializers. |

`run_provenance` is intentionally compact:

| Field | Meaning |
|---|---|
| `schema` | Provenance schema name, currently `ai-run-provenance-v1`. |
| `entrypoint` | User-facing script path plus SHA-256 when the script file exists. |
| `argv` | Original command-line arguments after wrapper normalization. |
| `args` | Parsed arguments sorted into deterministic JSON values. |
| `inputs` | Named corpus, feature, metadata, or profile paths with existence and file hashes. |
| `outputs` | Named model, card, manifest, metrics, or report paths. Future outputs may be marked `missing` before they are written. |
| `shared_trainer` | Optional implementation script when a wrapper delegates to a shared trainer. |

KonViD MOS runs record `ai/scripts/train_konvid_mos_head.py` as the
entrypoint. CHUG HDR MOS runs record
`ai/scripts/train_chug_hdr_mos_head.py` as the entrypoint and
`ai/scripts/train_konvid_mos_head.py` as `shared_trainer`, because the
CHUG command is the operator-facing contract while the training loop is
shared. FR regressor runs (`train_fr_regressor.py`,
`train_fr_regressor_v2.py`, and `train_fr_regressor_v3.py`) record the
same block in their model sidecars; v1/v2 also include it in the metrics
JSON so failed gates still preserve the exact table path, arguments, and
output targets used for the run. The `vmaf_tiny_v2`, `vmaf_tiny_v3`,
`vmaf_tiny_v4`, C2/C3 KoNViD baselines, FastDVDnet pre-filter, TransNet V2,
and `fr_regressor_v2_ensemble_v1_seed*` exporters record the same block in
their sidecar JSON so an exported ONNX can be traced back to the checkpoint,
upstream weights, corpus, export command, gate verdict, and output paths used
to create it. The U2NetP mirror exporter writes a separate
`u2netp-mirror-export-manifest-v1` sidecar with the same `run_provenance`
block plus upstream checkout, checkpoint, license, NOTICE, output hash, and
ONNX metadata status. The direct `train_fr_regressor_v2_ensemble.py`
smoke/production
trainer also records `run_provenance` in `fr_regressor_v2_ensemble_v1.json`,
covering the corpus input, member ONNX outputs, registry target, and manifest
path.

The tiny-VMAF evaluation reports also carry the same block:
`eval_loso_vmaf_tiny_v3.py`, `eval_loso_vmaf_tiny_v4.py`,
`eval_loso_vmaf_tiny_v5.py`, and `eval_multiseed_v3_v4.py` write
`run_provenance` into their report JSONs. Evaluation provenance records the
feature parquet input, parsed evaluation hyperparameters, original argv, and
the report target path. Use that block when comparing refreshed LOSO or
multi-seed numbers instead of relying on shell history.
The tiny-VMAF smoke validators (`validate_vmaf_tiny_v2.py`,
`validate_vmaf_tiny_v3.py`, and `validate_vmaf_tiny_v4.py`) accept
`--out-json` for the same reason: the report records the validated ONNX,
feature parquet, PLCC/RMSE gate result, optional comparison model, argv, and
JSON target path.

Legacy evaluation reports use the same schema where they still produce durable
operator artifacts: `eval_loso_mlp_small.py` and `eval_loso_3arch.py` record
their Netflix corpus root, fold-checkpoint directory, baseline ONNX inputs, and
JSON/Markdown report targets; `eval_probabilistic_proxy.py` records its
ensemble manifest, optional held-out parquet, and metrics output; and
`eval_saliency_per_mb.py` records the predicted / ground-truth mask directories
plus the JSON report target. This keeps old model-card evidence comparable with
the refreshed v3/v4/v5 reports.

The predictor-v2 real-corpus trainer also writes `run_provenance` into
`runs/predictor_v2_realcorpus/report.json`. That block records the selected
codec list, corpus roots, resolved JSONL files, gate arguments, and report
target so a gate failure can be reproduced without reconstructing shell history.

The `vmaf_tiny_v2`, `vmaf_tiny_v3`, `vmaf_tiny_v4`, and `vmaf_tiny_v5`
training scripts record the same block in their `--out-stats` JSON files.
Those stats files feed the ONNX exporters, so they now identify the training
parquet input(s), checkpoint target, stats target, argv, and hyperparameters
used before an export sidecar is produced.

Saliency-student training metrics use the same schema:
`train_saliency_student.py --metrics-out` and
`train_saliency_student_v2.py --metrics-out` record the DUTS-TR root, ONNX
output, metrics output, parsed training arguments, and original argv. Use this
block when comparing v1/v2 saliency refreshes or replaying a DUTS-rooted
training run from a model card.

Table materializers and audits use the same schema for durable audit JSON:
`materialize_mos_labels.py --audit-json`,
`materialize_second_opinion_features.py --audit-json`,
`materialize_saliency_features.py --audit-json`, and
`signal_mix_audit.py --out-json` all record `run_provenance`. Those blocks
identify source tables, label/score inputs, saliency model inputs where used,
output table targets, and report targets so refreshed feature-table evidence
can be replayed from the report alone. Derived FULL_FEATURES table builders
use the same contract: `extract_k150k_features.py`,
`combine_full_feature_parquets.py`, and
`enrich_k150k_parquet_metadata.py` write `<out>.manifest.json` by default.
Those manifests record the source parquet/video/metadata paths, feature
schema, backend split, filled feature columns, row counts, and original argv
so refreshed Netflix/K150K/CHUG tables are not anonymous local artifacts and
replay does not depend on shell history.
The corpus JSONL boundary scripts also write manifests:
`aggregate_corpora.py` records MOS scale-conversion metadata, source-shard
inputs, dedup counters, and corpus-source overrides; `merge_corpora.py`
records required vmaf-tune corpus keys, the natural dedup key, input shards,
and merge counters. Both default to `<output>.manifest.json` and accept
`--manifest-out`.
Legacy corpus/extraction entrypoints now follow the same sidecar pattern:
`extract_full_features.py`, `konvid_to_vmaf_pairs.py`, and
`bvi_dvc_to_corpus_jsonl.py` all default to `<out>.manifest.json` /
`<output>.manifest.json`. Their manifests record corpus/cache roots, VMAF/model
inputs, feature lists or row schema versions, row/frame/clip counters, failed
KoNViD clip IDs where applicable, adapter labels, and ADR-0661
`run_provenance`. The BVI-DVC adapter also emits the current vmaf-tune v3
additive columns with explicit unavailable defaults so cached BVI rows remain
schema-compatible.
MOS corpus adapters now emit the same replay evidence at the source boundary:
`chug_to_corpus_jsonl.py`, `konvid_1k_to_corpus_jsonl.py`,
`konvid_150k_to_corpus_jsonl.py`, `youtube_ugc_to_corpus_jsonl.py`,
`lsvq_to_corpus_jsonl.py`, `live_vqc_to_corpus_jsonl.py`, and
`waterloo_ivc_to_corpus_jsonl.py` all default to
`<output>.manifest.json` and accept `--manifest-out`. These manifests record
the corpus label, row counters, download/probe attrition, effective row caps,
local corpus roots, manifest CSV inputs, and ADR-0661 `run_provenance`.
Dataset fetchers now cover the step before those adapters run:
`fetch_konvid_1k.py` writes `<root>/fetch_manifest.json`, while
`fetch_youtube_ugc_subset.py` preserves its existing stem content manifest and
writes `<manifest>.run-manifest.json`. These sidecars record archive/source
URLs, selection policy, output bundle paths, and ADR-0661 `run_provenance` so a
later JSONL/parquet manifest can be traced back to the original local download
instead of only to an anonymous corpus directory.

The ensemble production validator `ai/scripts/validate_ensemble_seeds.py`
records `run_provenance` in its `PROMOTE.json` / `HOLD.json` verdicts. That
block identifies the LOSO artifact directory, corpus root snapshot input,
thresholds, seed list, and verdict output path.
The remaining validation helpers use the same schema when asked for reports:
`validate_model_registry.py --out-json` records the registry/schema inputs and
cross-file validation result, while `validate_saliency_student.py --out-json`
records the ONNX input and allowlist / parity / registry check verdicts.
The LOSO trainer itself (`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`)
also records the same block in each `loso_seed{N}.json` report, identifying
the corpus JSONL, parsed training arguments, original argv, and per-seed report
target before the validator condenses those reports into a promotion verdict.

The `vmaf-train` CLI uses the same schema for durable report commands that
accept `--json`: `validate-norm`, `profile`, `audit-learned-filter`,
`quantize-int8`, `cross-backend`, and `bisect-model-quality`. Those reports
identify the CLI entrypoint, parsed thresholds/options, model and feature
inputs, and JSON/model output targets, so promotion evidence can be attached to
model cards without losing the command context.

Feature-analysis reports use the same convention where they produce durable
JSON. `ai/scripts/feature_correlation.py --out` records the source parquet,
target column, redundancy threshold, top-K setting, and report path alongside
the Pearson / MI / LASSO / random-forest outputs, so signal-mix audits can be
replayed from the report alone.
`ai/scripts/phase3_subset_sweep.py --out` also records `run_provenance` next to
the subset result keys, including the source parquet, subset list, seed policy,
standardization flag, and report path used for Phase-3 model-selection sweeps.
`ai/scripts/calibrate_phase_f_recipes.py --out` records the same block in the
calibrated `vmaf-tune auto` recipe JSON, including the source corpus JSONL,
optional row cap, argv, and recipe output path.
`ai/scripts/calibrate_nr_threshold.py --output` records `run_provenance` in the
updated `nr_metric_v1.json` calibration sidecar, including the requested and
actual corpus directories, `nr_metric_v1.onnx`, CRF grid, argv, and Markdown
calibration report path. The same script quality-gates sidecar writes with
`--min-calibration-samples` and `--min-plcc`; weak fits still leave a Markdown
report for diagnosis, but do not update the tune-facing JSON unless
`--allow-weak-calibration` is set.

Quantisation scripts also use the same schema when asked for durable reports:
`ptq_dynamic.py --report-out`, `ptq_static.py --report-out`,
`qat_train.py --report-out`, and `measure_quant_drop.py --out-json` record the
fp32/int8 model paths, calibration/config inputs where applicable, size/gate
statistics, argv, and report output path. Prefer those reports for model-card
evidence instead of terminal logs.

Legacy extractor/cache utilities now use the standalone sidecar helper as
well: `build_bisect_cache.py --manifest-out` records cache mode, check status,
target-column candidates, default feature columns, and generated artifact
counts; `collect_gpu_calibration_data.py` defaults to
`<output>.manifest.json` and records selected features/backends/devices;
`extract_ugc_features.py` defaults to `<out-parquet>.manifest.json` and
records manifest/pair/fail/source counts; and `extract_konvid_frames.py`
defaults to `ai/data/konvid_frames_manifest.json` and records frame-pair
materialization counts.

## MOS label materialization

Real MOS-head training expects feature tables to already carry `mos` or
`mos_raw_0_100`. If an extraction pass produced only metric columns, join the
subjective labels before training:

```bash
.venv/bin/python ai/scripts/materialize_mos_labels.py \
    --features runs/full_features_konvid_refresh_20260520_with_folds.parquet \
    --labels .corpus/konvid-150k/konvid_150k.jsonl \
    --feature-key-column key \
    --label-key-column src \
    --feature-key-regex '([0-9]{6,})' \
    --label-key-regex '([0-9]{6,})' \
    --out runs/full_features_konvid_refresh_20260520_with_mos.parquet
```

The KonViD MOS trainer rejects real-path inputs that yield zero labelled rows
and writes no checkpoint. Use `--smoke` when the desired input is synthetic.

## C1 (Netflix corpus) — runnable training prep

Once the local Netflix corpus exists at `.workingdir2/netflix/` (see
[training-data.md](training-data.md) for the layout and ADR-0242 for
scope), the prep stack under [`ai/data/`](../../ai/data/) and
[`ai/train/`](../../ai/train/) replaces the parquet-driven flow above
with a runnable end-to-end pipeline. ADR-0203 records the
implementation decisions (distillation source, val-split policy,
architecture roster, cache layout).

### One-command training

```bash
# Builds libvmaf if you haven't yet:
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build

# Defaults: arch=mlp_small, val-source=Tennis, epochs=10.
# The first invocation pre-warms the per-clip cache at
# $VMAF_TINY_AI_CACHE (default ~/.cache/vmaf-tiny-ai); subsequent runs
# only re-train.
python ai/train/train.py \
    --data-root .workingdir2/netflix \
    --model-arch mlp_small \
    --epochs 30 \
    --batch-size 256 \
    --lr 1e-3 \
    --out-dir runs/tiny_nflx
```

Or, equivalently, via the wrapper script:

```bash
bash ai/scripts/run_training.sh
```

### CLI flags

| Flag | Default | Notes |
|---|---|---|
| `--data-root` | `.workingdir2/netflix` | Directory with `ref/` and `dis/`. |
| `--model-arch` | `mlp_small` | One of `linear`, `mlp_small`, `mlp_medium`. |
| `--epochs` | 10 | `0` runs the smoke-export path and exits. |
| `--batch-size` | 256 | SGD batch size. |
| `--lr` | 1e-3 | Adam learning rate. |
| `--out-dir` | `runs/tiny_nflx` | ONNX checkpoints land at `<out-dir>/<arch>_epoch<n>.onnx` and `<arch>_final.onnx`. |
| `--val-source` | `Tennis` | Source name held out for validation. |
| `--max-pairs` | unset | Cap on (ref, dis) pairs (smoke / debugging). |
| `--no-export-onnx` | unset | Skip per-epoch ONNX dump (final still written). |
| `--assume-dims WxH` | unset | For tests / mock corpora with non-1080p YUVs. |

### Architectures

| Arch | Layers | Params (feature_dim=6) |
|---|---|---|
| `linear` | `Linear(6, 1)` | 7 |
| `mlp_small` | `Linear(6,16) -> ReLU -> Linear(16,8) -> ReLU -> Linear(8,1)` | 257 |
| `mlp_medium` | `Linear(6,64) -> ReLU -> Linear(64,32) -> ReLU -> Linear(32,1)` | 2 561 |

### Expected runtime + GPU requirements

| Phase | CPU-only (8-core) | CUDA (RTX 3060) |
|---|---|---|
| Cache warm (full corpus, 70 pairs) | 30–60 min (libvmaf-bound) | 5–8 min (libvmaf CUDA backend) |
| Train 30 epochs `mlp_small` | 1–2 min | <30 s |
| Train 30 epochs `mlp_medium` | 2–4 min | <60 s |
| ONNX export | <1 s | <1 s |

The cache is the bottleneck on first run; subsequent training runs
re-use the JSON cache and skip libvmaf entirely. To force a re-extract,
delete `$VMAF_TINY_AI_CACHE`.

### Evaluation

```bash
python -c "
from pathlib import Path
import numpy as np
from ai.train.dataset import NetflixFrameDataset
from ai.train.eval import evaluate

val = NetflixFrameDataset(Path('.workingdir2/netflix'), split='val')
X, y = val.numpy_arrays()
report = evaluate(
    features=X,
    targets=y,
    onnx_path=Path('runs/tiny_nflx/mlp_small_final.onnx'),
    out_path=Path('runs/tiny_nflx/eval_report.json'),
)
print(report)
"
```

The JSON report contains `n_samples`, `plcc`, `srocc`, `krocc`, `rmse`,
`latency_ms_p50_per_clip`, `latency_ms_p95_per_clip`, `model`,
`feature_dim`. Latency is measured against a synthetic 240-frame clip
on the CPU EP — the whole point of the tiny model is being meaningfully
faster than the SVR.

### Smoke command

CI runs only the `--epochs 0` smoke test (the real corpus and a
real training run are not GitHub-runner-friendly). The smoke command
lives in `ai/tests/test_train_smoke.py` and the equivalent shell
invocation is:

```bash
python ai/train/train.py \
    --epochs 0 \
    --data-root /tmp/mock_corpus \
    --assume-dims 16x16 \
    --val-source BetaSrc \
    --out-dir /tmp/tiny_smoke
```

This exports an initial-weights ONNX file without touching the real
corpus or invoking libvmaf, and is the documented reproducer in the PR
template.

## C1 (KoNViD-1k corpus) — synthetic-distortion FR pairs

The 9-source Netflix Public corpus is fully utilised by the LOSO
sweep — Research-0023 §5 documents how the FoxBird outlier reflects
content-distribution variance within those 9 clips. To reduce that
variance the natural unblocker is a *different / larger* training
corpus. KoNViD-1k (Konstanz natural video database, 1 200 user-
generated clips at 540p with crowd-sourced MOS) is the natural
starting point; it's already locally available at
`$VMAF_DATA_ROOT/konvid-1k/` or `$VMAF_KONVID_1K_DIR`
(downloaded via `ai/scripts/fetch_konvid_1k.py`).

KoNViD-1k ships as no-reference (clip + MOS), not as VMAF-style
(ref, dis) pairs. To turn it into the FR-pair format the LOSO
trainer expects, the fork adds an acquisition step that synthesises
a distorted variant per clip via libx264 CRF=35 round-trip — same
recipe used for the Netflix dis-pairs in the existing corpus —
and runs libvmaf to extract the 6 canonical features + per-
frame VMAF teacher score (resolved from ADR-1168 single source; ADR-1173) per
(ref, dis) pair.

### Acquisition

```bash
# smoke (5 clips, ~30 s wall):
python ai/scripts/konvid_to_vmaf_pairs.py --max-clips 5

# full run (1 200 clips, ~30 min wall on the ryzen-4090 profile):
python ai/scripts/konvid_to_vmaf_pairs.py
```

Output: `ai/data/konvid_vmaf_pairs.parquet` (gitignored). Schema
matches what `NetflixFrameDataset.numpy_arrays()` produces:
`(key, frame_index, vif_scale0..3, adm2, motion2, vmaf, teacher_model)` per row.
The command also writes `ai/data/konvid_vmaf_pairs.manifest.json` by default,
including CRF, feature names, clip/frame counts, failed clip IDs, the VMAF
binary/model inputs, and `run_provenance`. Use `--manifest-out PATH` when the
parquet is bundled under a different experiment directory.

Per-clip JSON caches under `$VMAF_TINY_AI_CACHE/konvid-1k/<key>.json`
so re-runs are idempotent — only newly-added clips re-extract.

For the current full-feature FR refresh, use the fork CPU `vmaf`
binary explicitly:

```bash
# smoke
python ai/scripts/konvid_to_full_features.py \
    --konvid-root "$VMAF_KONVID_1K_DIR" \
    --vmaf-bin core/build-cpu/tools/vmaf \
    --max-clips 5

# full run
python ai/scripts/konvid_to_full_features.py \
    --konvid-root "$VMAF_KONVID_1K_DIR" \
    --vmaf-bin core/build-cpu/tools/vmaf
```

This writes `runs/full_features_konvid.parquet` plus
`runs/full_features_konvid_with_folds.parquet`. The folded file adds
`source=fold0..fold4` using a deterministic balanced hash order over
clip keys so `eval_multiseed_v3_v4.py` can reproduce the KoNViD 5-fold
gate without relying on stale local parquet files.

The script also writes `runs/full_features_konvid.manifest.json` by
default. The manifest records the KoNViD root, resolved videos
directory, cache directory, vmaf binary, model path, CRF/codec recipe,
fold settings, selected/processed clip counts, row/column counts, and
ADR-0661 `run_provenance` block. Pass `--manifest-out PATH` when the
sidecar needs to live inside a dated experiment bundle instead of next
to the parquet.

### Combining Refreshed FULL_FEATURES Shards

After Netflix, KoNViD, BVI-DVC, and optional UGC refreshes finish,
rebuild aggregate training tables with the combiner instead of manual
`pandas.concat`:

```bash
python ai/scripts/combine_full_feature_parquets.py \
    --input netflix=runs/full_features_netflix_refresh_20260520.parquet \
    --input konvid=runs/full_features_konvid_refresh_20260520.parquet \
    --input bvi=runs/full_features_bvi_dvc_D_refresh_20260520.parquet \
    --out runs/full_features_4corpus_refresh_20260520.parquet

python ai/scripts/combine_full_feature_parquets.py \
    --input base=runs/full_features_4corpus_refresh_20260520.parquet \
    --input ugc=runs/full_features_ugc_refresh_20260520.parquet \
    --out runs/full_features_5corpus_refresh_20260520.parquet
```

The combiner normalizes every input to
`corpus, source, frame_index, codec, teacher_model, <FULL_FEATURES>, vmaf`,
fills missing feature columns with `NaN`, and preserves the caller-provided
corpus label. It also writes `<out>.manifest.json` by default with
per-input row counts, missing-feature fill lists, output column order,
the aggregate corpus distribution, and `run_provenance`. Pass
`--manifest-out PATH` only when the manifest needs to live next to a
separate experiment bundle.

The standalone KoNViD and BVI-DVC full-feature builders follow the same
sidecar rule (`runs/full_features_konvid.manifest.json` and
`runs/full_features_bvi_dvc_<tier>.manifest.json` by default), so each
refreshed shard can be replayed before it is combined.

#### Teacher model provenance (ADR-1173)

Every producer distils from **one** teacher: the fork default model
(`vmaf_v1.0.16_3d0h`, single-sourced from
`core/include/libvmaf/model.h` / `vmaftune.defaultmodel.DEFAULT_MODEL`,
ADR-1168). Resolution order, implemented by
`ai.data.scores.resolve_teacher_model()`:

1. an explicit override — `--vmaf-model` (`extract_full_features.py`,
   `extract_k150k_features.py`) or `--model` (`bvi_dvc_to_full_features.py`,
   `extract_ugc_features.py`, `konvid_to_full_features.py`,
   `konvid_to_vmaf_pairs.py`, `bvi_dvc_to_corpus_jsonl.py`), accepting a
   version name (`vmaf_v0.6.1`), a `version=`/`path=` libvmaf model spec, or
   a model JSON path;
2. `$VMAF_MODEL_PATH` (a model JSON file);
3. the single-source default.

Every feature row and run manifest carries a `teacher_model` column naming
the teacher that produced its `vmaf` target. Consumers refuse to mix
teachers:

- `combine_full_feature_parquets.py`, `train_vmaf_tiny_v5.py` and
  `eval_loso_vmaf_tiny_v5.py` raise `ValueError` when a shard contains more
  than one `teacher_model`, when two inputs disagree, or when an input has
  no `teacher_model` column at all. Legacy tables (extracted before the
  column existed, i.e. with `vmaf_v0.6.1`) are only accepted with an
  explicit `--assume-teacher vmaf_v0.6.1`, which stamps that value on
  every row; the flag must match any stamp already present.
- the `NetflixFrameDataset` per-clip cache (`$VMAF_TINY_AI_CACHE`, default
  `~/.cache/vmaf-tiny-ai/`) is revalidated on read: an entry whose stamped
  teacher differs from the resolved teacher, or a legacy entry with no
  stamp, is treated as a miss and recomputed (logged at INFO), never
  relabelled. Expect one full re-extraction the first time a pre-ADR-1173
  cache is reused.

Mixed-teacher tables are never merged silently; there is no flag that
overrides a genuine conflict.

### Loader

[`ai/train/konvid_pair_dataset.py::KoNViDPairDataset`](../../ai/train/konvid_pair_dataset.py)
mirrors `NetflixFrameDataset`'s interface — same `feature_dim` (6),
same `numpy_arrays() → (X, y)` shape — so the existing
`_train_loop` consumes it without modification:

```python
from ai.train.konvid_pair_dataset import KoNViDPairDataset

# all 1 200 clips
ds = KoNViDPairDataset("ai/data/konvid_vmaf_pairs.parquet")

# LOSO-style holdout: 1 clip val, rest train
val_keys = {ds.unique_keys[0]}
train_keys = set(ds.unique_keys) - val_keys
val_ds = KoNViDPairDataset("ai/data/konvid_vmaf_pairs.parquet", keep_keys=val_keys)
train_ds = KoNViDPairDataset("ai/data/konvid_vmaf_pairs.parquet", keep_keys=train_keys)

X, y = train_ds.numpy_arrays()  # (n_train_frames, 6), (n_train_frames,)
```

### Combining KoNViD with the Netflix corpus

The combined trainer driver lives at
[`ai/train/train_combined.py`](../../ai/train/train_combined.py). It
concatenates the Netflix `NetflixFrameDataset` train slice with the
KoNViD `KoNViDPairDataset` train slice on the feature axis and feeds
the union to the same `_build_model` + `_train_loop` + `export_onnx`
pipeline that `ai/train/train.py` uses, so the model factory and ONNX
output layout stay identical.

```bash
# Default: hold out the Netflix Tennis source for val; KoNViD is
# fully in training. Mirrors the canonical ADR-0203 split so the
# result is directly comparable to mlp_small / mlp_medium baselines.
python ai/train/train_combined.py \
    --netflix-root .workingdir2/netflix \
    --konvid-parquet ai/data/konvid_vmaf_pairs.parquet \
    --model-arch mlp_small \
    --epochs 30 \
    --out-dir runs/tiny_combined
```

`--val-mode` selects the validation split:

| Mode                                | Validation set                              |
| ----------------------------------- | ------------------------------------------- |
| `netflix-source` (default)          | Netflix `--val-source` (default `Tennis`)   |
| `konvid-holdout`                    | Deterministic 10 % of KoNViD clip keys      |
| `netflix-source-and-konvid-holdout` | Union of the two above                      |
| `netflix-only`                      | KoNViD slice is omitted entirely            |
| `konvid-only`                       | Netflix slice is omitted entirely           |

KoNViD train/val splits hold out *whole clips* (not random frames)
keyed off `--seed` + `--konvid-val-fraction`, so frames from the
same KoNViD clip cannot leak across the split. ONNX checkpoints
land at `<out-dir>/<arch>_combined_epoch<n>.onnx` and
`<arch>_combined_final.onnx`.

When the parquet is missing, the trainer prints a warning and falls
back to the Netflix-only path; when both corpora are missing it
exports an initial-weights ONNX and exits 0 so the smoke command
still produces a deterministic artefact.

## C2 — NR metric

Same flow, different config: [`ai/configs/nr_mobilenet_v1.yaml`](../../ai/configs/nr_mobilenet_v1.yaml).
`extract-features` is replaced by a direct frame loader
([`frame_loader.py`](../../ai/src/vmaf_train/data/frame_loader.py)) that
feeds ffmpeg-decoded tensors into training. The loader supports
single-channel `gray` frames as `HxW` arrays and packed colour formats
`rgb24`, `bgr24`, `rgba`, and `bgra` as `HxWxC` arrays. Other FFmpeg
pixel formats fail before spawning the decoder so training jobs do not
silently reinterpret planar or subsampled layouts as packed tensors.

## C3 — Learned filter

[`ai/configs/filter_residual_v1.yaml`](../../ai/configs/filter_residual_v1.yaml)
trains a residual CNN where the model is clamped to `x + residual` in
normalized space. Target is BVI-DVC encoder-distortion pairs.

## Determinism

`vmaf-train fit` seeds Python, NumPy, and PyTorch with the config's `seed`
field and sets Lightning's `deterministic=True`. Combined with
`train_commit + train_config_hash + dataset_manifest_sha + seed` the output
weights are reproducible to within float-rounding nondeterminism (which CI
will flag as a regression when it exceeds a tight allclose).

## Hyperparameter sweeps

The `ai[tune]` extra pulls in Optuna + Ray Tune. `vmaf-train tune`
wraps the existing Optuna sweep helper around a base YAML config and
searches `model_args` entries. Each trial writes to
`<output>/trial_NNN` and the objective minimises the best validation
loss (`val/mse` for regressors, `val/l1` for learned filters) recorded
by Lightning.

```bash
pip install -e 'ai[tune]'
vmaf-train tune \
  --config ai/configs/fr_tiny_v1.yaml \
  --output runs/fr_tiny_sweep \
  --trials 20 \
  --param hidden=choice:16,32,64 \
  --param lr=float:0.0001:0.01:log
```

`--param` is repeatable and accepts three forms:

| Form | Example | Trial API |
| --- | --- | --- |
| `name=float:LOW:HIGH[:log]` | `lr=float:0.0001:0.01:log` | `trial.suggest_float` |
| `name=int:LOW:HIGH` | `depth=int:1:4` | `trial.suggest_int` |
| `name=choice:A,B,...` | `hidden=choice:16,32,64` | `trial.suggest_categorical` |

Values from `choice` are coerced to `int`, `float`, or boolean when
possible; otherwise they stay strings. Use `--storage sqlite:///...`
to resume or share an Optuna study.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `extract-features` is slow | libvmaf CPU-only | rebuild with `-Denable_cuda=true` and rerun |
| `fit` OOM | batch size too big for GPU | edit `ai/configs/*.yaml` `batch_size`, or drop `precision` to `16-mixed` |
| Export roundtrip fails atol=1e-5 | op using `float16` with a value near `inf` | retrain in `float32` end-to-end, or tighten clamping |
