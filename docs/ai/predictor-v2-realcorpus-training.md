<!-- markdownlint-disable MD013 MD060 -->
# Predictor v2 — real-corpus LOSO training (Phase 2)

[`docs/ai/predictor.md`](predictor.md) (introduced by PR #450) covers
the runtime predict-then-verify loop and the synthetic-stub training
pipeline. This page covers **Phase 2**: how to promote those stubs
into production-flippable models trained on real corpora and gated
by the [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md)
production-flip threshold.

The Phase-2 trainer ships in this repo; the **trained-model artefacts
do not** — operators run the trainer locally against their corpora
and commit the resulting ONNX + model-card diff in a follow-up PR.

## When to run this

Run Phase 2 after either:

- A real corpus has been generated under `~/.workingdir2/netflix/`
  (canonical-6 schema; 9 Netflix Public Dataset sources × NVENC /
  QSV / SW codecs), `~/.workingdir2/konvid-150k/` (KoNViD-1k UGC,
  when ingested), or `~/.workingdir2/bvi-dvc-raw/` (BVI-DVC raw
  YUVs, when ingested). Or
- An operator wants to validate that the shipped synthetic stubs
  remain the right ship for a codec by running the gate against
  whatever corpus is locally available.

The trainer **never** auto-overwrites a stub ONNX without first
clearing the gate; failing codecs keep the stub and the model card
gains an explicit `Status: Proposed (gate-failed: REASON)` block.

## What the gate enforces

The Phase-2 gate is the same two-part threshold as
ADR-0303 §Decision applied per codec rather than per ensemble seed:

| Sub-gate | Threshold | Failure consequence |
|---|---|---|
| Mean fold PLCC | `>= 0.95` | Codec marked `fail`; ONNX stub kept. |
| Spread (`max - min` fold PLCC) | `<= 0.005` | Codec marked `fail`; ONNX stub kept. |
| Per-fold floor | `>= 0.95` | Codec marked `fail`; ONNX stub kept. |
| LOSO fold count | `5` | Corpora with `< 5` distinct sources -> `insufficient-sources`. |

These constants live in
`ai/scripts/train_predictor_v2_realcorpus.py` as
`SHIP_GATE_MEAN_PLCC`, `SHIP_GATE_PLCC_SPREAD_MAX`,
`SHIP_GATE_PER_FOLD_MIN`, `LOSO_FOLD_COUNT`. **Do not lower them**
to make a codec pass — per CLAUDE.md §13 / `feedback_no_test_weakening`,
the gate is load-bearing. If a codec genuinely requires a different
threshold, supersede ADR-0303 with a new ADR and update both call
sites (predictor trainer + `scripts/ci/ensemble_prod_gate.py`)
together.

## How to run the trainer

The orchestration shell is the canonical entry point. It auto-
discovers corpora, runs the trainer, retrains every passing codec on
the full corpus, and patches the model cards:

```bash
bash ai/scripts/run_predictor_v2_training.sh
```

Common overrides:

```bash
# Point at a specific corpus.
CORPUS=/path/to/canonical6.jsonl \
  bash ai/scripts/run_predictor_v2_training.sh

# Override the discovery roots.
CORPUS_ROOTS="$HOME/data/corpus_a $HOME/data/corpus_b" \
  bash ai/scripts/run_predictor_v2_training.sh

# Diagnostic run when no corpora are on disk yet.
ALLOW_EMPTY=1 bash ai/scripts/run_predictor_v2_training.sh
```

The script writes:

- `runs/predictor_v2_realcorpus/report.json` — machine-readable
  per-codec verdict + per-fold metrics. Schema:
  - `gate.{mean_plcc_threshold, plcc_spread_max, per_fold_min, loso_fold_count, adr}`.
  - `codecs[].{codec, status, mean_plcc, plcc_spread, mean_srocc, mean_rmse, n_rows_total, n_distinct_sources, failure_reasons[], folds[]}`.
  - `summary.{n_pass, n_fail, n_insufficient, n_missing_rows}`.
  - `run_provenance.{schema, entrypoint, argv, args, inputs, outputs}` records
    the exact trainer script, command line, corpus roots/files, and report
    target used for the run.
- `runs/predictor_v2_realcorpus/train_<UTC>.log` — full trainer log.
- `model/predictor_<codec>.onnx` — overwritten **only for codecs that
  PASS the gate**. Synthetic stubs are kept for failing codecs.
- `model/predictor_<codec>_card.md` — every codec's card gains a
  `Status: Production (ADR-0303 gate cleared)` or
  `Status: Proposed (gate-failed: REASON)` block. Idempotent across
  re-runs (the prior `Status:` block is replaced).

## Direct trainer invocation

The shell wraps `ai/scripts/train_predictor_v2_realcorpus.py`. For
tighter control:

```bash
# Restrict to a single codec for debugging.
python ai/scripts/train_predictor_v2_realcorpus.py --codec libx264

# Synthetic-smoke run (no real corpus required; never produces PASS).
python ai/scripts/train_predictor_v2_realcorpus.py --synthetic-smoke

# Custom report path + fewer epochs for a quick iteration.
python ai/scripts/train_predictor_v2_realcorpus.py \
  --epochs 50 --report-out /tmp/p2-fast.json
```

The trainer's exit code is `0` iff every codec passes the gate; this
is the CI hook a future workflow can consume once the corpus is
hosted somewhere CI can reach.

## Reading a fail report

Example honest-fail output (codec genuinely under-fits the corpus):

```text
libvvenc       FAIL                   0.8520  0.0420    320     8
  - mean PLCC 0.8520 < 0.9500 (ADR-0303 part 1)
  - PLCC spread 0.0420 > 0.0050 (ADR-0303 part 2)
```

Two recovery paths:

1. **Ship more training data.** `libvvenc` may need additional
   sources to clear the spread bound; add corpora under one of the
   discovery roots and re-run.
2. **Supersede ADR-0303.** If after exhaustive corpus expansion the
   gate still fails for a structural reason (e.g. encoder is
   inherently noisier than the deterministic v2 baseline), open a
   superseding ADR. **Do NOT silently lower the threshold in code.**

The `Status: Proposed (gate-failed: REASON)` block on the model card
makes the fail visible to anyone reading the card — there is no
silent-pass path.

## Test coverage

`ai/tests/test_train_predictor_v2_realcorpus.py` (23 cases) pins:

- **Gate enforcement is honest** — synthetic FoldResults at
  PLCC = 0.85 land in the `n_fail` bucket, never silently in
  `n_pass`. Constants match ADR-0303 §Decision.
- **LOSO partitioning is by source** — held-out fold sources never
  appear in the training fold, even when the same source contributes
  many rows.
- **Corpus discovery skips missing roots** — operators with only one
  of the three configured corpora do not crash the batch.
- **Report schema is stable** — `gate`, `codecs[]`, `summary` keys
  are pinned so the orchestration shell can rely on the layout, and diagnostic
  reports carry ADR-0661 `run_provenance`.

The fold-level training body itself (the per-fold MLP fit) is
exercised by the existing `tools/vmaf-tune/tests/test_predictor_train.py`
suite from PR #450 — Phase 2 uses the same trainer module so the
ONNX export remains byte-stable across the synthetic-stub and real-
corpus paths.

## Cross-references

- [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) —
  authoritative gate definition. Phase 2 mirrors the constants per
  codec; Phase-1 / ensemble path uses them per seed.
- [`docs/ai/predictor.md`](predictor.md) — runtime predict-then-
  verify loop, synthetic-stub policy.
- [`docs/research/0075-fr-regressor-v2-ensemble-prod-flip.md`](../research/0075-fr-regressor-v2-ensemble-prod-flip.md)
  — gate methodology + LOSO protocol.
- [`docs/research/0081-fr-regressor-v2-ensemble-real-corpus-methodology.md`](../research/0081-fr-regressor-v2-ensemble-real-corpus-methodology.md)
  — real-corpus retrain protocol the predictor trainer mirrors.
- [`scripts/ci/ensemble_prod_gate.py`](../../scripts/ci/ensemble_prod_gate.py)
  — companion CI gate for the deep-ensemble flip path.
