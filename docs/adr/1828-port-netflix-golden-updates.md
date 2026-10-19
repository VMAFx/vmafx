<!-- markdownlint-disable MD013 MD060 -->
# ADR-1828: Netflix's own golden-assertion updates are ported verbatim from upstream

- **Status**: Accepted (status update 2026-10-06 below)
- **Date**: 2026-10-05
- **Deciders**: lusoris (maintainer popup answer, 2026-10-05)
- **Tags**: tests, golden-data, upstream-port, agents, fork-local

## Context

The rule in `AGENTS.md` (§ Global project rules 1, §8) and
`docs/development/agent-hard-rules.md` forbids any change to Netflix's golden
`assertAlmostEqual` values in `python/test/`. Its purpose is that the fork
never edits a reference value to make its own code pass. Netflix itself,
however, tightens and re-records those values upstream (commits `5c7770080`,
`005988ead`, `4679db83c`, `d93495f5c`, `e3827e4dd`). The fork held the older
values at looser places, so the gate drifted from upstream's, and the 2026-10-05
upstream sync stopped on 164 changed assertions because the rule read as a
ban on any edit.

The sync measured them first (`/home/kilian/.cache/vmafx-rc3-handoff/prompts/golden-stop-measurement.md`):
the fork's CPU build at `origin/master` produces upstream's new value at
upstream's places in 156 of 164 rows and fails none; the other 8 are not
exercised (a skipped test and an assertion inside `assertRaises`). Adopting
them is therefore a tightening of the gate, not a loosening of a result.

## Decision

Porting Netflix's own updated golden assertion verbatim from upstream is
allowed; every other edit stays forbidden. A port copies the expected value and
the `places` exactly as upstream has them, with no value or tolerance the fork
chose itself, and is preceded by a measurement of the upstream value against the
fork's CPU build (`make test-netflix-golden` on the PR head must pass). An
upstream update the fork's build does not reproduce is not ported; it is
reported as a code question (fix code, not assertions). The rule text in
`AGENTS.md`, `docs/development/agent-hard-rules.md` and the PR template says so.

This PR applies it to the five commits above (162 assertions in
`quality_runner_test.py`, `result_test.py`, `routine_test.py`,
`local_explainer_test.py`, `vmafexec_test.py`). Where upstream's tip differs from
the commit's own value (a later upstream commit), the tip is taken.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the fork's older, looser values | No edit to any golden assertion | The gate stays looser than Netflix's and drifts with every upstream re-record; every future sync stops on it | Maintainer chose to adopt the measured values |
| Port only the values the fork already matches at its old places | Smaller diff | The fork's places stay looser than upstream's for the same value | Same drift for no gain |
| Let the fork re-record values from its own build | Always green | The fork's output becomes its own reference: a regression moves the "golden" value with it | Defeats the rule's purpose |

## Consequences

- **Positive**: the gate matches Netflix's; future syncs have a defined path
  (measure, then port verbatim) instead of a stop.
- **Negative**: the fork now carries upstream's chosen places, including looser
  ones where Netflix loosened them (the `akiyo_multiply` scores in
  `vmafexec_test.py` go from places 4 to 3, and upstream dropped the macOS
  per-platform values, so a macOS lane that differs from Linux by more than
  `5e-4` on those scores would fail until upstream or a maintainer decides
  otherwise).
- **Neutral / follow-ups**: `test_run_vmaf_runner_v1_model` is skipped in the
  fork (ADR-0865) so its upstream rows have no assertion to port; the
  `test_run_vmaf_runner_rdh540` assertions sit inside `assertRaises` and are
  ported unrun, as upstream has them.

## References

- Maintainer popup answer, 2026-10-05: `Q: "Adopt the 156 (Recommended)"`.
- Measurement: `/home/kilian/.cache/vmafx-rc3-handoff/prompts/golden-stop-measurement.md`
  (summary in the PR body).
- [ADR-0024](0024-netflix-golden-preserved.md), [ADR-1487](1487-upstream-parity-policy-and-guard.md).

## Status update 2026-10-06: Accepted

The decision was applied while this record still said Proposed. Checked on
`origin/master` `fd8b8c93b`: #2141 (`2faa124a9`, 2026-10-05) ported Netflix's
own golden-assertion re-records verbatim, and the rule text carrying the
exception is in `AGENTS.md` section 8, `docs/development/agent-hard-rules.md`
and the pull request template.

The body above is unchanged.
