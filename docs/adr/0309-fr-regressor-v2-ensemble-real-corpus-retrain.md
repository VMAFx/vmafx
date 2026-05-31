<!-- markdownlint-disable MD060 -->
# ADR-0309: `fr_regressor_v2` ensemble — real-corpus retrain harness + flip workflow

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris, Claude (Anthropic)
- **Companion research digest**: [Research-0081](../research/0081-fr-regressor-v2-ensemble-real-corpus-methodology.md)
- **Tags**: ai, fr-regressor, ensemble, loso, runbook, fork-local
- **Related**: [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md)
  (gate definition + LOSO trainer scaffold),
  [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md)
  (parent codec-aware design),
  [ADR-0291](0291-fr-regressor-v2-prod-ship.md)
  (deterministic v2 prod flip — defines the 0.95 LOSO PLCC ship gate),
  [ADR-0237](0237-quality-aware-encode-automation.md)
  (`vmaf-tune --quality-confidence` consumer).

## Context

PR #399 (ADR-0303) merged the **ensemble production-flip scaffold**:
`ai/scripts/train_fr_regressor_v2_ensemble_loso.py` (5-seed × 9-fold
LOSO trainer) and `scripts/ci/ensemble_prod_gate.py`
(`mean(PLCC) ≥ 0.95` AND `max-min ≤ 0.005`). The five
`fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
`model/tiny/registry.json` are still `smoke: true` because the
trainer needs a real LOSO run to clear the gate.

The Netflix Public Dataset is locally available at
`.workingdir2/netflix/` (9 reference + 70 distorted YUVs, ~37 GB,
provided by lawrence on 2026-04-27, gitignored). What's missing is
the **operational harness** that lets a maintainer fire-and-forget
the retrain: a wrapper that loops over the seeds, a validator that
applies the gate and emits a verdict file, and a runbook that
explains how to interpret the verdict and how to roll back if
something goes wrong.

A real LOSO run is hours of GPU work; doing it inside this PR would
be impractical and would create a CI artefact dependency. The harness
ships now and the retrain runs out-of-band.

## Decision

This ADR ships the **harness only**, deferring both the actual
training run and the registry flip to follow-up commits.

Specifically:

1. **`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`** — Bash
   wrapper that validates `.workingdir2/netflix/`, loops `seed ∈
   {0,1,2,3,4}` over the existing trainer, tees per-seed timestamped
   logs under `runs/ensemble_v2_real/logs/`, and emits a one-line
   summary on completion.
2. **`ai/scripts/validate_ensemble_seeds.py`** — Python validator
   that reads `runs/ensemble_v2_real/loso_seed{0..4}.json`, calls the
   ADR-0303 gate, snapshots the corpus YUV file list (sha256 over
   sorted `relpath\tsize` — not YUV bytes), and writes
   `PROMOTE.json` on gate-pass (recommendation: flip rows in
   `model/tiny/registry.json`) or `HOLD.json` on gate-fail
   (recommendation: keep `smoke: true`; investigate diversity).
3. **`ai/tests/test_validate_ensemble_seeds.py`** — synthetic
   fixtures for both gate-pass and gate-fail (mean-failure +
   spread-failure) cases, plus exit-code coverage for `main()`.
4. **`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`** — the
   runbook a maintainer follows to drive the retrain end-to-end,
   including rollback if a flip happens prematurely.

The actual `smoke: true → false` registry flip lands in a
**separate follow-up PR**, gated on a `PROMOTE.json` produced by
this harness. Splitting the harness from the flip keeps the
review surface small (the harness is reviewable without GPU
access) and means the flip PR can be a single-row diff against
the registry — easy to revert if anything regresses downstream.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Harness now, flip later (chosen)** | Reviewable without GPU access. Flip PR is a 5-line registry diff, trivially revertable. Verdict files (PROMOTE/HOLD) are auditable artefacts that pin the corpus sha256 to the gate result. Honours the [no-skip-shortcuts rule](../../CLAUDE.md#12-hard-rules-for-every-session) — a real-corpus run gates the flip. | Two PRs instead of one. Maintainer has to remember to open the follow-up. | Selected — the cost of two PRs is a 30-second mechanical step; the benefit is a clean rollback surface and a reviewable harness that doesn't depend on hours of GPU output. |
| Bundle harness + retrain + flip in one PR | One PR end-to-end. | The PR can't be reviewed until the 6–12 h retrain finishes. CI artefact dependencies (LOSO JSONs as inputs to the flip step) are fragile. The flip diff hides inside a multi-thousand-LOC PR alongside the harness. Reverting the flip means reverting the harness too. | Rejected — review latency and revert hygiene both lose. |
| Harness only, no verdict file (just print) | Smaller LOC. | Loses the corpus-snapshot audit trail. The flip PR has no machine-checkable artefact to cite. Re-running the validator to "check what it said last time" requires re-running the gate against the JSONs, which is fine until the JSONs are themselves regenerated and the corpus drifts silently. | Rejected — the PROMOTE/HOLD verdict is the load-bearing audit artefact; deleting it weakens the flip-PR audit trail. |
| Auto-flip on PROMOTE inside the validator | Fully automatic. | Violates the [registry-flip-is-a-separate-PR invariant](../../ai/AGENTS.md) being established in this very ADR; any rebase that touches the validator could silently flip the registry. Catastrophic during a `/sync-upstream`. | Rejected — automatic registry mutation from a script run is exactly the rebase-time foot-gun the AGENTS.md invariant exists to prevent. |

## Consequences

- **Positive**: a maintainer can drive the retrain end-to-end with
  two commands (`bash ai/scripts/run_ensemble_v2_real_corpus_loso.sh`
  then `python ai/scripts/validate_ensemble_seeds.py
  runs/ensemble_v2_real/`). The verdict file is a reproducible audit
  artefact that pins the corpus sha256 to the gate outcome.
- **Positive**: the rollback path is documented and the registry flip
  is a separate PR, so reverting the flip is a clean `git revert`
  on a tiny diff.
- **Positive**: the validator is fully unit-tested with synthetic
  fixtures — no real LOSO output is needed to exercise the gate
  logic. CI runs the tests on every PR.
- **Negative**: two PRs instead of one. Mitigated by the runbook
  that walks through the follow-up flow.
- **Neutral / follow-up**: the registry flip PR will land once a
  real LOSO run produces a `PROMOTE.json`. If the first run produces
  a `HOLD.json`, this ADR's harness is still useful for re-runs;
  the follow-up flip stays unopened.

## References

- req (2026-05-05, user direction): the user requested a follow-up
  to PR #399 that ships the operational harness for the real-corpus
  LOSO retrain — wrapper script, validator with PROMOTE/HOLD verdict
  files, tests, runbook, and an ADR. Paraphrased: "ship the harness
  now so the maintainer can fire-and-forget the retrain; do not run
  the LOSO inside this PR; do not flip the registry inside this PR."
- [Research-0081](../research/0081-fr-regressor-v2-ensemble-real-corpus-methodology.md)
  — corpus-size sufficiency, LOSO fold sizing, seed-diversity
  hyperparameters.
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — gate
  definition (mean ≥ 0.95 AND spread ≤ 0.005); LOSO trainer +
  CI gate scaffold.
- [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) — parent
  codec-aware FR regressor v2 design.
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — deterministic v2
  prod flip + 0.95 LOSO PLCC ship gate inherited per-seed.
- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune
  --quality-confidence` consumer that needs the ensemble's
  predictive distribution.
- PR #399 — ensemble production-flip trainer + CI gate scaffold
  (the prerequisite this ADR builds on).
