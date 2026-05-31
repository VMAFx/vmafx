<!-- markdownlint-disable MD013 -->
# `fr_regressor_v2` ensemble — real-corpus retrain runbook

This runbook walks through the **real-corpus LOSO retrain** of the
five `fr_regressor_v2_ensemble_v1_seed{0..4}` members of the deep
ensemble, including how to interpret the verdict and how to roll
back if the registry was flipped prematurely.

- **Parent design**: [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md)
  (codec-aware scaffold).
- **Gate definition**: [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md)
  (ensemble production-flip trainer + CI gate — `mean(PLCC) ≥ 0.95`
  AND `max(PLCC) - min(PLCC) ≤ 0.005`).
- **Harness scope**: [ADR-0309](../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  (this runbook's PR — wrapper script + validator + verdict files;
  registry flip is a separate follow-up PR).

---

## Prerequisites

| Requirement       | Expected value                                                                                  |
|-------------------|-------------------------------------------------------------------------------------------------|
| YUV corpus        | `.workingdir2/netflix/` (gitignored; consumed by the Phase A pre-step, not the trainer)         |
| YUV contents      | 9 reference + 70 distorted YUVs (full Netflix Public Dataset)                                   |
| Phase A JSONL     | `runs/phase_a/full_grid/per_frame_canonical6.jsonl` (overridable via `$CORPUS_JSONL`)           |
| Phase A row count | ~33,840 per-frame rows (9 sources × {h264_nvenc, h264_qsv} × 4 CQs; per Research-0075)          |
| GPU memory        | ≥ 8 GB VRAM (NVIDIA / SYCL — the trainer is single-GPU per seed)                                |
| Free disk         | ≥ 5 GB under `runs/ensemble_v2_real/` (logs + per-seed JSON, no model weights)                  |
| Wall time         | ~3–5 h Phase A (RTX 4090) **plus** 6–12 h LOSO retrain (8 GB GPU, 5 seeds × 9 folds sequential) |
| Python deps       | `ai/pyproject.toml` editable install (`pip install -e ai/`)                                     |

The YUV corpus is gitignored — it was provided locally by lawrence on
2026-04-27 (memory-pinned in
`feedback_netflix_training_corpus_local`). Do **not** check it in.
The Phase A JSONL is also gitignored (it lives under `runs/`).

---

## Step-by-step run

### 0. Generate the Phase A canonical-6 corpus

The trainer (`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`)
consumes a **per-frame canonical-6 JSONL**, not raw YUVs. The
producer is `scripts/dev/hw_encoder_corpus.py` (PR #392), which
emits one source × one encoder × N CQs per call. Loop over the 9
Netflix sources × {`h264_nvenc`, `h264_qsv`} × CQs ∈ {19, 25, 31,
37}, then concatenate the per-call JSONLs into the canonical
location (~33,840 rows total per Research-0075).

If the JSONL already exists from a previous run, skip this step
and jump to step 1. The JSONL is gitignored and lives under
`runs/`, so it won't be picked up by other clones; either re-run
this step on a fresh machine or copy the file across.

```bash
# Layout assumed: .workingdir2/netflix/<src>/<src>.yuv at WxH@FPS.
# Adjust SOURCES if your local corpus uses a flat / different
# naming convention; the producer is per-source per-encoder.

set -euo pipefail
out_root="runs/phase_a/full_grid"
mkdir -p "$out_root/parts"

# 9 Netflix Public Dataset sources (matches NETFLIX_SOURCES in the
# trainer — see ai/scripts/train_fr_regressor_v2_ensemble_loso.py).
sources=(BigBuckBunny_25fps BirdsInCage_30fps CrowdRun_25fps \
         ElFuente1_30fps ElFuente2_30fps FoxBird_25fps \
         OldTownCross_25fps Seeking_25fps Tennis_24fps)
encoders=(h264_nvenc h264_qsv)
cqs=(19 25 31 37)

for src in "${sources[@]}"; do
  src_yuv=".workingdir2/netflix/${src}/${src}.yuv"  # adjust to local layout
  for enc in "${encoders[@]}"; do
    out="$out_root/parts/${src}__${enc}.jsonl"
    python3 scripts/dev/hw_encoder_corpus.py \
      --vmaf-bin core/build-cpu/tools/vmaf \
      --source "$src_yuv" \
      --width 1920 --height 1080 --pix-fmt yuv420p --framerate 25 \
      --encoder "$enc" \
      $(printf -- '--cq %s ' "${cqs[@]}") \
      --out "$out"
  done
done

# Concatenate per-call JSONLs into the canonical path.
cat "$out_root"/parts/*.jsonl > "$out_root/per_frame_canonical6.jsonl"
wc -l "$out_root/per_frame_canonical6.jsonl"   # expect ~33,840
```

Pre-step wall time: ~3–5 h on an RTX 4090 (NVENC + QSV are
hardware-accelerated; the dominant cost is ffmpeg encode + libvmaf
score per (source, encoder, CQ) cell). The pre-step is a one-shot
— once `per_frame_canonical6.jsonl` exists, subsequent retrains
(re-seeded, re-tuned hyperparameters, etc.) skip directly to
step 1.

### 1. Verify the corpus

```bash
test -f runs/phase_a/full_grid/per_frame_canonical6.jsonl  # required
wc -l runs/phase_a/full_grid/per_frame_canonical6.jsonl    # expect ~33,840

# Informational — the YUV directory is consumed by step 0, not the trainer.
ls -d .workingdir2/netflix/                      # should exist for step 0
find .workingdir2/netflix/ -name '*.yuv' | wc -l # should be > 0 for step 0
```

### 2. Kick off the wrapper

```bash
bash ai/scripts/run_ensemble_v2_real_corpus_loso.sh
```

The wrapper:

1. Validates `$CORPUS_JSONL` (default
   `runs/phase_a/full_grid/per_frame_canonical6.jsonl`) exists. If
   not, errors with a pointer to step 0.
2. Loops `seed ∈ {0,1,2,3,4}`, calling
   `train_fr_regressor_v2_ensemble_loso.py --seeds N
   --corpus $CORPUS_JSONL --out-dir runs/ensemble_v2_real/` per
   seed. The trainer writes `loso_seed{N}.json` to `--out-dir`
   automatically; the wrapper does not pass `--output`. Each
   `loso_seed{N}.json` includes ADR-0661 `run_provenance` with the
   trainer entrypoint, argv, parsed hyperparameters, corpus JSONL input,
   and per-seed report target.
3. Tees a timestamped log per seed under
   `runs/ensemble_v2_real/logs/seed{N}_<UTC>.log`.
4. Prints a one-line summary on completion (elapsed seconds + next
   command to run).

The wrapper is **not** a daemon — it blocks until all 5 seeds
finish. Use `tmux` / `nohup` if you want to detach.

### 3. Apply the production-flip gate

```bash
python ai/scripts/validate_ensemble_seeds.py runs/ensemble_v2_real/
```

The validator:

- Loads `loso_seed{0..4}.json`.
- Calls the gate from `scripts/ci/ensemble_prod_gate.py`
  (single source of truth for the threshold constants).
- Snapshots the corpus YUV file list (sha256 over sorted `relpath\tsize`,
  not YUV bytes — that would dominate validate runtime on a 37 GB corpus).
- Writes `runs/ensemble_v2_real/PROMOTE.json` *or* `HOLD.json`, including a
  `run_provenance` block with the validator entrypoint, argv, seed list,
  threshold arguments, LOSO directory input, corpus root input, and verdict
  output path.
- Exits 0 on PROMOTE, 1 on HOLD, 2 on input error.

---

## Interpreting the verdict

### `PROMOTE.json` — gate cleared

```jsonc
{
  "verdict": "PROMOTE",
  "gate": {
    "passed": true,
    "mean_plcc": 0.96,
    "plcc_spread": 0.003,
    "per_seed_plccs": {"0": 0.961, "1": 0.962, ...}
  },
  "corpus": {"sha256": "...", "yuv_count": 79, ...},
  "run_provenance": {"schema": "ai-run-provenance-v1", "...": "..."},
  "recommendation": "flip seeds smoke->false in model/tiny/registry.json ..."
}
```

Action: open a **separate follow-up PR** that flips the five
`fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
`model/tiny/registry.json` from `smoke: true` to `smoke: false`.
Cite the `PROMOTE.json` corpus sha256 in the PR description so the
flip is traceable to the exact retrain that earned it.

### `HOLD.json` — gate failed

```jsonc
{
  "verdict": "HOLD",
  "gate": {
    "passed": false,
    "mean_plcc": 0.951,
    "plcc_spread": 0.012,
    "plcc_spread_pass": false,
    "failing_seeds": [3]
  },
  "recommendation": "keep smoke=true; investigate diversity / hyperparameters. Failing aspects: plcc_spread 0.0120 > 0.0050"
}
```

Action: do **not** flip the registry. Common remediations:

- **Spread too wide (`max - min > 0.005`)**: the seeds disagree on
  which Netflix sources they generalise to. Try
  re-seeding (cheap), increase epochs, or widen ensemble diversity
  via different weight-init scales — see ADR-0303 §Decision and
  Research-0081 §Seed-diversity hyperparameters.
- **Mean PLCC too low**: corpus / loader regression. Re-derive the
  Phase A canonical-6 features and confirm against ADR-0291's
  deterministic v2 baseline first.
- **One seed below 0.95 with healthy spread**: drop / replace that
  seed before re-running. The flip is per-seed (a seed flips only
  after it individually clears the gate).

---

## Rollback procedure

If the registry was flipped prematurely (i.e. before a passing
`PROMOTE.json` existed, or against an outdated corpus snapshot):

1. **Revert the flip commit** — `git revert <flip-sha>` from `master`
   via a fresh PR. Direct push to `master` is host-blocked
   (CLAUDE §12 r2 + ADR-0037).
2. **Verify the registry** — `python ai/scripts/validate_model_registry.py`
   should pass; the five rows must read `"smoke": true` again.
3. **Verify the C-side ORT loader** — re-run
   `python ai/tests/test_registry.py` to confirm the smoke graphs
   still load (they always have, since the ONNX bytes never change
   on a smoke→smoke revert).
4. **Audit the consumers** — anything that read the now-reverted
   non-smoke ensemble (e.g. `vmaf-tune --quality-confidence`)
   silently degrades to ADR-0291's deterministic point estimate
   when `smoke: true` is back in force; surface that in the revert
   PR description.
5. **Open a HOLD-tracking issue** — link the original `PROMOTE.json`
   that was used for the bad flip (if any) and the new `HOLD.json`
   that justifies the revert. The issue stays open until a clean
   real-corpus retrain produces a fresh `PROMOTE.json`.

---

## See also

- [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) —
  gate definition (mean ≥ 0.95 AND spread ≤ 0.005).
- [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md) —
  parent design for the codec-aware FR regressor v2.
- [Research-0081](../research/0081-fr-regressor-v2-ensemble-real-corpus-methodology.md)
  — corpus-size sufficiency, LOSO fold sizing, seed-diversity
  hyperparameters.
- [`scripts/ci/ensemble_prod_gate.py`](../../scripts/ci/ensemble_prod_gate.py) —
  single source of truth for the threshold constants.
- [`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`](../../ai/scripts/run_ensemble_v2_real_corpus_loso.sh) —
  the wrapper this runbook drives.
- [`ai/scripts/validate_ensemble_seeds.py`](../../ai/scripts/validate_ensemble_seeds.py) —
  the validator that emits `PROMOTE.json` / `HOLD.json`.
