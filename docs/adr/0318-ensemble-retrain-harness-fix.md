<!-- markdownlint-disable MD013 -->
# ADR-0318: `fr_regressor_v2` ensemble retrain harness — wrapper-trainer interface fix + Phase A pre-step doc

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `ai/scripts/run_ensemble_v2_real_corpus_loso.sh` present on master;
  Phase A runbook + wrapper landed.
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `fr-regressor`, `ensemble`, `loso`, `runbook`, `fork-local`
- **Related**: [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  (real-corpus retrain harness — this ADR fixes the harness it shipped),
  [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md)
  (gate definition + LOSO trainer scaffold),
  [ADR-0291](0291-fr-regressor-v2-prod-ship.md)
  (deterministic v2 prod flip).

## Context

PR #405 (ADR-0309, merged 2026-05-06) shipped the
`fr_regressor_v2` real-corpus LOSO retrain harness — wrapper script
`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`, validator
`ai/scripts/validate_ensemble_seeds.py`, and the runbook at
`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`. The PR was
not end-to-end tested; the moment an operator runs the wrapper, two
real bugs surface:

1. **Wrapper-trainer argv mismatch.** The wrapper invokes the
   trainer with `--corpus-root "$corpus_root"` and
   `--output "$out_dir/loso_seed${seed}.json"`. The trainer
   (`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`)
   actually accepts `--corpus` (a JSONL path, not a directory) and
   `--out-dir` (the trainer derives `loso_seed{N}.json` from
   `--seeds` itself). The trainer rejects the wrapper's invocation
   with `unrecognized arguments: --corpus-root --output`.
2. **Missing Phase A canonical-6 corpus pre-step.** The trainer's
   default `--corpus` path is
   `runs/phase_a/full_grid/per_frame_canonical6.jsonl`
   (Research-0075 line 87 — "33,840 per-frame canonical-6 rows × 9
   sources × NVENC + QSV"). The runbook only verifies the **raw**
   YUV corpus exists; it does not document the producer
   (`scripts/dev/hw_encoder_corpus.py`, PR #392) or the loop the
   operator must run to materialise the JSONL. Operators hit the
   "corpus not present" branch immediately after the wrapper's
   argv parse error.

The trainer's argparse block is the authoritative interface — it
matches what
`ai/scripts/train_fr_regressor_v2_ensemble.py` and
`scripts/ci/ensemble_prod_gate.py` already produce / consume, and
the JSONL path is what the v3 follow-up flip PR will plug a real
loader into. The wrapper drifted from it. Fixing the wrapper
preserves the trainer's interface across the whole codepath; fixing
the trainer to accept the wrapper's accidental argv shape would
ripple into every other caller.

## Decision

Fix the harness shape in two places, in one PR:

1. **Wrapper** (`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`):
   pass `--corpus "$CORPUS_JSONL"` (defaulting to the canonical
   `runs/phase_a/full_grid/per_frame_canonical6.jsonl`) and
   `--out-dir "$out_dir"`. Drop `--output`. Replace the YUV
   directory hard-fail with a JSONL-existence hard-fail; the YUV
   directory check becomes informational because the trainer never
   reads YUVs directly.
2. **Runbook**
   (`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`):
   prepend a step **0. Generate the Phase A canonical-6 corpus**
   that walks the operator through the
   `scripts/dev/hw_encoder_corpus.py` loop (9 sources × 2 encoders
   × 4 CQs → ~33,840 rows). Update the prereqs table to add the
   JSONL row and the Phase A wall-time estimate.

The trainer is **not** modified. Its CLI is the authoritative
interface; freezing it is the cheapest way to keep
`train_fr_regressor_v2_ensemble_loso.py`,
`train_fr_regressor_v2_ensemble.py`, and the production-flip CI
gate aligned.

## Alternatives considered

1. **Fix the wrapper + document the pre-step (chosen).** Smallest
   diff, no trainer surface change. The trainer's CLI stays the
   single source of truth for "what does the LOSO trainer
   accept?". Cost: a doc-heavy step 0; the operator still has to
   run the Phase A loop themselves (~3–5 h). Win: zero ripple into
   other consumers, idempotent (re-running step 0 is cheap if the
   JSONL already exists), aligns with the post-#392 / post-#399
   data-flow assumed by Research-0075.
2. **Rewrite the wrapper as Python (e.g. a click CLI under
   `ai/scripts/`).** A Python wrapper could import the trainer's
   `build_argparser()` and avoid drift by construction. Cost: net
   +200 LOC, plus a new dependency surface (`click` or argparse
   plumbing) for what is fundamentally a 5-iteration `for` loop;
   the wrapper would still have to shell out to the producer in
   step 0. The wrapper script doesn't have a pattern of drifting —
   it drifted once because the original PR was not end-to-end
   tested. Adding a Python layer to fix a one-off bug would over-
   engineer the harness. Rejected.
3. **Patch the trainer to accept `--corpus-root` and `--output`
   as aliases.** Would silently make the wrapper "work" without
   any other code changes. Cost: the trainer would carry two
   parallel CLI shapes, both documented as authoritative,
   guaranteed to drift again the next time someone wires a new
   caller. The companion scripts
   (`train_fr_regressor_v2_ensemble.py`,
   `eval_loso_vmaf_tiny_v3.py`,
   `scripts/ci/ensemble_prod_gate.py`) already use the trainer's
   current CLI shape; admitting the wrong shape on the trainer
   side would invite divergence across all of them. Rejected.

## Consequences

- The wrapper's failure mode for "Phase A JSONL not produced"
  becomes a clean exit-2 with a pointer to runbook step 0 instead
  of `unrecognized arguments` followed by the trainer's
  `NotImplementedError`.
- Operators must run the Phase A loop (step 0) once per machine.
  The JSONL is gitignored under `runs/`, so it is not shared via
  source control — `feedback_netflix_training_corpus_local`
  remains the canonical pointer to the YUV corpus and now extends
  to its derived JSONL.
- The runbook's wall-time estimate grows from "6–12 h" to
  "~3–5 h Phase A + 6–12 h LOSO" on an 8 GB GPU class. Subsequent
  retrains (re-seeded, hyperparameter sweeps) skip step 0 and pay
  only the LOSO cost.
- Validator (`validate_ensemble_seeds.py`) and CI gate
  (`scripts/ci/ensemble_prod_gate.py`) are unaffected — they
  consume `loso_seed{N}.json`, which the trainer's `--out-dir`
  already produces.

## References

- ADR-0309 — the real-corpus retrain harness this ADR patches.
- ADR-0303 — gate definition + the trainer the wrapper drives.
- PR #392 — `scripts/dev/hw_encoder_corpus.py` (Phase A producer).
- Research-0075 — "33,840 per-frame canonical-6 rows × 9 sources ×
  NVENC + QSV" sizing target consumed in step 0 of the runbook.
- `req`: user direction 2026-05-06 — "Fix the wrapper +
  doc the pre-step. Don't rewrite the wrapper as Python; don't
  patch the trainer to accept the wrapper's accidental argv shape;
  the trainer's CLI is the authoritative interface."
