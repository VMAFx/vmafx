<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1673: A push to master never cancels the runs of an earlier master commit; the concurrency group carries the SHA there

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: lusoris
- **Tags**: `ci`, `workflows`, `merge-train`, `rc3`, `fork-local`

## Context

A local merge train lands master commits every few minutes. Most workflows used
`group: <name>-${{ github.ref }}` with `cancel-in-progress: true`, so every landing
cancelled the previous master commit's push runs. Measured on 2026-10-05, no
master commit had a complete verdict: on `a60b7a966` the Tests, Rust, Lint,
FFmpeg, Docker Image Build, Dev Container, CI, Builds and Build all runs ended
`cancelled`. The train now keeps one sentinel master commit's runs alive and
cancels the other superseded master runs itself through the API, so GitHub's
own per-ref cancellation on master is no longer wanted. Pull request refs are
unaffected: a newer push to a PR should still supersede the older run.

## Decision

For workflows triggered by a push to master, the concurrency group becomes
`<name>-${{ github.ref == 'refs/heads/master' && github.sha || github.ref }}`
and `cancel-in-progress` becomes `${{ github.ref != 'refs/heads/master' }}`.
Blocks that must serialise stay as they are and are listed, each with its
reason, in `SERIALISED` of `scripts/ci/tests/test_master_concurrency_contract.py`:
`dev-container-publish.yml`, `release-please.yml`, `scorecard.yml` and the
`deploy` job of `docs.yml`. `docker-publish-tester.yml` and
`windows-tester-bundle.yml` already put the SHA in the group on a push and need
no change. The Required Checks Aggregator takes the same form: with the SHA in
its group, re-running an older master head no longer cancels the aggregator of a
newer one. On PR refs it is unchanged, so an older PR head's re-run still
cancels the current one. A contract test evaluates every concurrency block of
every push-to-master workflow for two master SHAs and two PR pushes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `cancel-in-progress: false` with the ref group | One-line change | Group still holds one running and one pending run; a newer push evicts the pending one, so the commit in between is still `cancelled` | Does not give every commit a verdict (the ADR-1294 pending-slot eviction) |
| SHA in the group for master pushes, cancel kept for PRs (chosen) | Every master run is independent; PR behaviour unchanged; the train decides what to cancel | More concurrent runs on a busy queue unless the train cancels | Chosen |
| Drop `concurrency:` from push runs, split PR and push workflows | Explicit | Doubles 18 workflows; a second configuration format | Duplicates workflows for a one-line expression |
| Keep the aggregator per ref | No change there | A re-run of an old master head cancels the newest aggregator | SHA group removes the hazard on master at no cost |

## Consequences

- **Positive**: every master commit can finish and report; the train's sentinel and API cancellation are the only things that stop a master run.
- **Negative**: without the train's cancellation, a burst of master pushes queues one full run set each.
- **Neutral / follow-ups**: a new push-to-master workflow either takes the SHA form or is listed with a reason; `docs/development/ci.md` "Master push runs" documents the policy.

## References

- Paraphrased: the user requested that master push runs stop being cancelled by concurrency, with the train owning cancellation of superseded master runs (task brief, 2026-10-05).
