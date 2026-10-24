<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3368: Every artifact step has a Gitea twin or a named skip

- **Status**: Accepted
- **Date**: 2026-10-24
- **Deciders**: Lusoris (operator order vmafx-gitea-workflow-defects-20261024)
- **Tags**: ci, github-actions, gitea

## Context

The workflows also run on a Gitea mirror through Gitea's act runner. Every job
with an `actions/upload-artifact` or `actions/download-artifact` step failed
there at that step with `GHESNotSupportedError` (run 5569 on the home runner):
from v4 on, `@actions/artifact` refuses every server whose host is not
github.com, `*.ghe.com` or `*.localhost`. Gitea 1.27 serves the v3 artifact
protocol, and upstream names v3 as the version for servers that are not
GitHub. github.com no longer serves v3, so neither pin works on both hosts.
The act runner sets `GITEA_ACTIONS=true` in every job.

## Decision

We will keep the v4+ pins and guard each artifact step with
`env.GITEA_ACTIONS != 'true'`, followed by a Gitea twin with the same condition
and `== 'true'`. The twin uses the v3 pin with the same `with:` inputs. A
download that needs an input v3 lacks (`pattern`, `merge-multiple`) gets a
`run:` twin that prints a `::warning::` naming the missing inputs, so no step is
skipped without a message. `scripts/ci/check_gitea_artifact_steps.py` refuses an
unguarded step, a missing or mismatched twin, a skip where v3 would do and a v3
pin outside a twin; `--write` adds guards and twins. It runs in
`make lint-actions` and as a pre-commit hook. A Renovate rule keeps the v3 pins
below v4.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Twin per step, checked (chosen) | Works on both hosts; artifacts kept on Gitea; a new step without a twin fails lint | Two steps per artifact operation; +880 lines in workflows | — |
| Skip every artifact step on Gitea | Smaller diff | Gitea runs lose logs and reports; downloads that feed later jobs break | Loses evidence the runs exist for |
| Pin v3 everywhere | One step | github.com refuses v3 artifacts | Breaks every GitHub run |
| Spoof the host (`ACTIONS_RESULTS_URL` on a `*.localhost` name) | No workflow edits | Depends on the toolkit's host test, breaks silently on its next change; Gitea does not serve the v4 protocol anyway | Not a supported path |
| Composite wrapper action | One call site per step | Composite `uses:` cannot take a version chosen at run time; still two inner steps; hides the pin from Renovate | No gain over the twin |

## Consequences

- **Positive**: Gitea mirror jobs run their artifact steps or log why not; the
  pairing is held by a check with a planted-defect test.
- **Negative**: every new artifact step needs its twin (`--write` adds it);
  workflows grow.
- **Neutral / follow-ups**: once Gitea serves the v4 protocol and the toolkit
  accepts its host, the twins and the Renovate hold are removed together.

## References

- Operator order vmafx-gitea-workflow-defects-20261024 (paraphrased): fix the
  artifact-step failure on the home Gitea runner with a path Gitea supports, or
  skip with a named reason; no silent skip.
- Evidence: home runner run 5569, `GHESNotSupportedError` at the upload step.
- `@actions/artifact` host check (`isGhes()`), actions/toolkit.
