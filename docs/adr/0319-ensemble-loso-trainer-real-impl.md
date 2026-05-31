<!-- markdownlint-disable MD060 -->
# ADR-0319: `fr_regressor_v2` ensemble LOSO trainer — real loader + per-fold training

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, fr-regressor, ensemble, loso, fork-local
- **Related**: [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md)
  (gate definition + LOSO trainer scaffold),
  [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  (real-corpus retrain harness),
  [ADR-0291](0291-fr-regressor-v2-prod-ship.md)
  (deterministic v2 prod flip — defines the 0.95 LOSO PLCC ship gate),
  [Research-0075](../research/0075-fr-regressor-v2-ensemble-prod-flip.md)
  (LOSO protocol + JSON schema)

## Context

ADR-0303 (PR #399) merged the **ensemble production-flip scaffold** —
`ai/scripts/train_fr_regressor_v2_ensemble_loso.py` — but
intentionally left two functions stubbed because the Phase A canonical-6
corpus did not exist on master at the time:

- `_load_corpus(corpus_path)` raised `NotImplementedError`.
- `_train_one_seed(seed, corpus, args)` raised `NotImplementedError`.

ADR-0309 (PR #405) merged the **real-corpus retrain harness** — the
wrapper script `ai/scripts/run_ensemble_v2_real_corpus_loso.sh` and
the validator `ai/scripts/validate_ensemble_seeds.py`. The wrapper
forwarded `--corpus-root` / `--output` argv that did not match the
trainer's `--corpus` / `--out-dir` interface, but the mismatch was
masked by the trainer's smoke-mode no-op exit when the corpus was
absent.

The Phase A canonical-6 corpus
(`runs/phase_a/full_grid/per_frame_canonical6.jsonl`, ~5,640 NVENC
per-frame rows × 9 Netflix sources × 4 CQs) is now generated locally
via `scripts/dev/hw_encoder_corpus.py`. This ADR closes both deferrals
and the wrapper-mismatch in one PR.

## Decision

**Implement the real loader + per-fold trainer body, fix the wrapper
argv, and ship the runbook update — all in the same PR. No registry
flip; that stays a separate follow-up per ADR-0309's invariant.**

Concretely:

1. `_load_corpus` reads the JSONL via `pandas.read_json(..., lines=True)`,
   validates the canonical-6 columns + `vmaf` / `src` / `encoder` /
   `cq` / `frame_index`, fits a corpus-wide StandardScaler (ADR-0291
   recipe), and pre-computes the 14-D codec block:
   12-slot ENCODER_VOCAB v2 one-hot + `preset_norm` (constant 0.5 —
   `hw_encoder_corpus.py` does not record preset) + `crf_norm` (cq
   normalised over the corpus's observed cq range).
2. `_train_one_seed` runs 9-fold LOSO over the unique `src` values:
   per fold, fit a fold-local StandardScaler on the training rows
   only (mirrors `eval_loso_vmaf_tiny_v3.py`), train an
   `FRRegressor(in_features=6, num_codecs=14)` for `args.epochs`
   (default 200) with Adam(`lr=5e-4`, `weight_decay=1e-5`), evaluate
   PLCC / SROCC / RMSE on the held-out source. The returned dict
   carries `mean_plcc` (the field `scripts/ci/ensemble_prod_gate.py`
   parses) plus the per-fold list, `min_plcc`, `max_plcc`, `std_plcc`,
   `wall_time_s`, and full hyperparameters — matches
   Research-0075 §JSON schema.
3. The wrapper script passes `--corpus "$CORPUS_JSONL"` (default the
   canonical Phase A path) and `--out-dir "$out_dir"`; drops the
   obsolete `--corpus-root` / `--output` argv. Adds a prereq check
   that the JSONL exists and has ≥100 rows.
4. The runbook gains a "Step 0: Generate Phase A canonical-6 corpus"
   section before the verification step, documenting the
   `hw_encoder_corpus.py` invocation pattern and the QSV-optional
   note.
5. `ai/AGENTS.md`'s "Ensemble registry invariant" section gains a
   note pinning the canonical-6 schema as load-bearing — schema
   changes require an ENCODER_VOCAB version bump.

The `num_codecs=14` choice (full codec block width, including
`preset_norm` + `crf_norm`) matches `train_fr_regressor_v2.py`'s
`FRRegressor(num_codecs=num_codec_dims)` pattern. The
`fr_regressor_v2_ensemble_v1` smoke ONNX shipped under
`model/tiny/` was trained with `num_codecs=NUM_CODECS=6` against
`vmaf_train.codec.CODEC_VOCAB` (the v1 codec vocab); the registry
flip PR will need to retrain or accept the architecture mismatch.
The trainer here optimises for the v2 deterministic baseline that
already cleared 0.9681 mean LOSO PLCC (ADR-0291).

## Alternatives considered

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| **pandas + torch (chosen)** | Mirrors `eval_loso_vmaf_tiny_v3.py` loader pattern; FRRegressor reuse keeps the v2 ONNX-export path open; pandas is already a training-time dep | Adds ~30 MB pandas footprint to the trainer venv (already required by other LOSO scripts) | Selected — minimum-deviation from existing v2 + LOSO patterns |
| Pure numpy / json loader | No pandas dep; smaller install footprint | Re-implements the source-grouping + column projection that `pandas.groupby` does in two lines; loses parity with `eval_loso_vmaf_tiny_v3.py` reproducer | Rejected — trainer is dev-time, not runtime; pandas is already on the path for the other LOSO scripts |
| PyTorch Lightning Trainer | Matches `FRRegressor` Lightning class; gets logging / checkpointing for free | 200-epoch loops over 9 folds × 5 seeds inside Lightning add per-fold setup cost; not necessary for a fixed-budget LOSO pass | Rejected — manual `Adam` + `MSELoss` loop is ~50 LOC, Lightning's overhead would dominate on this corpus size |

## Consequences

- **Visible behaviour change**: `train_fr_regressor_v2_ensemble_loso.py`
  now produces real `loso_seed{N}.json` artefacts when given a corpus.
  The CI gate `scripts/ci/ensemble_prod_gate.py` can apply its
  two-part check (`mean PLCC ≥ 0.95` AND `spread ≤ 0.005`) against
  real numbers.
- **Wrapper now end-to-end**: `bash ai/scripts/run_ensemble_v2_real_corpus_loso.sh`
  runs without argv errors against the canonical Phase A corpus.
- **Registry untouched**: per ADR-0309's invariant, registry-flip
  stays a separate PR. This ADR ships the trainer + wrapper + runbook
  only.
- **Wall-time**: ~5 min per seed on RTX 4090 (verified end-to-end);
  ~25 min for the full 5-seed × 9-fold run. Slower CPUs scale
  linearly.
- **Preset-conditioning is a no-op for now**: the canonical-6 corpus
  doesn't record preset, so `preset_norm` is the constant 0.5. A
  future corpus regen with explicit preset metadata would activate
  the column without retraining (the constant-column model just
  ignores it).

## References

- req (2026-05-06, operator): "Operator on the fork's primary GPU
  host has already generated the canonical-6 corpus locally; this PR
  plugs in the real implementations + fixes the wrapper."
  (paraphrased from the dispatcher prompt requesting closure of the
  ADR-0303 / ADR-0309 deferrals.)
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — gate
  definition (mean ≥ 0.95 AND spread ≤ 0.005).
- [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) —
  retrain harness; this PR closes the deferred trainer-body + wrapper
  argv-mismatch tracked there.
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — deterministic v2
  prod flip; source of the 0.95 LOSO PLCC ship gate that the
  ensemble inherits.
- [Research-0075](../research/0075-fr-regressor-v2-ensemble-prod-flip.md)
  §JSON schema — emitted-JSON contract.
- [`scripts/ci/ensemble_prod_gate.py`](../../scripts/ci/ensemble_prod_gate.py)
  — single source of truth for the threshold constants.
- [`ai/scripts/eval_loso_vmaf_tiny_v3.py`](../../ai/scripts/eval_loso_vmaf_tiny_v3.py)
  — pandas-based LOSO loader pattern this trainer mirrors.
