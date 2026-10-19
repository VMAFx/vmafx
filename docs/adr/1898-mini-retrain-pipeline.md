<!-- markdownlint-disable MD013 MD060 -->
# ADR-1898: A resumable stage runner and a mini retrain that exercises the retrain tooling in CI

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: Lusoris
- **Tags**: ai, ci, training, reproducibility, fork-local

## Context

The one-shot retrain (RC9, issue #1246) runs for about 125 to 130 hours. The
maintainer asked that it not be the first time its tools run end to end, so
that the run does not stop on a tooling defect. Before this decision each
script was tested alone; no test ran extraction, combination, training, export,
validation, registry update and gating in one pass, and no stage could be
resumed after a kill except K150K extraction through its own `.done` files.

## Decision

We add `aiutils.pipeline`, a stage runner (one command, its inputs, its
outputs, one manifest per stage), and `ai/scripts/mini_retrain.py`, a driver that
runs the real retrain scripts with the runbook's flags on a generated corpus
of twelve clip pairs cut from the tracked 576x324 test pair. A stage skips only
when its manifest is complete, its key (argv, input digests, seed, environment
identity) is unchanged and its outputs still hash as recorded. Inputs are
checked before the first stage starts. The required Tiny AI job runs the
end-to-end tests for changes under `ai/`, and `mini-retrain.yml` runs them nightly. The `mini`
profile's thresholds prove plumbing on 144 rows; the `full` profile carries the
runbook's gates.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Shell script chaining the commands | trivial | no resume, no named failures, no manifests | the three properties asked for would be re-implemented per script |
| Extend each trainer with its own checkpoint/resume | finer-grained resume inside a stage | 40+ scripts, duplicated logic (#1249) | stage-level resume is what a killed 125 h run needs first; inner checkpoints stay per trainer |
| Download a small public corpus in CI | realistic data | network in CI, licence review, flaky | the repo already tracks a licence-cleared pair; generation is deterministic |
| No fixture: unit tests only | fast | never exercises the seams (the `motion` column was NaN in every row until this run) | seams are where the defects were |

## Consequences

- **Positive**: tooling defects surface in CI; per-stage resource use is measured for the resource plan; the same runner can drive the full run.
- **Negative**: one more CI job (about two minutes of tests after a CPU build).
- **Neutral / follow-ups**: model families not yet in the pipeline (codec-aware FR regressors, ensemble, NR head, QAT, predictor, quantisation, model cards) are added in later stacked PRs.

## References

- Maintainer direction 2026-10-05 (paraphrased): the 125-130 hour retrain must not be the first end-to-end run of its tooling.
- Issue #1246 (Training-tooling readiness), issue #1245.
