<!-- markdownlint-disable MD013 MD060 -->
# ADR-1274: Make HISS-21 claims replayable and platform-gated

- **Status**: Proposed
- **Date**: 2026-09-20
- **Deciders**: VMAFx maintainers
- **Tags**: ci, governance, agents, testing, docs

## Context

The repository advertised HISS-16 after Praetor's standard had advanced through
HISS-21. More importantly, `praetorctl hiss coverage --verify` failed because
`.config/hiss/coverage.yaml` did not exist. The ordinary standards workflow ran
the debt audit and context compiler, but never replayed the scanner behavior it
treated as enforcement. A scanner regression could therefore keep reporting a
green required check.

The README also repeated the project name below a banner that already contains
it. That visual duplication and the stale badge exposed the governance drift;
changing only the badge would hide the CI defect rather than fix it.

## Decision

Declare the measured Praetor scanner coverage for HISS-01, HISS-02, HISS-04,
HISS-07, HISS-08, and HISS-09 across C, Go, Python, and Rust. Replay 43 positive,
negative, and known-gap fixtures for 18 language claims. Keep the catalog
deliberately limited to the scanner behavior the replay command actually
executes; compiler, linter, release, and review claims remain separate gates
instead of being mislabeled as replayed evidence.

Run the replay in `make verify-all`, pre-commit, pre-push, the existing required
standards job, and a Linux/macOS/Windows matrix. Require the standards job and
all three platform contexts through the aggregator with strict-success
semantics: absent, skipped, neutral, cancelled, or failed is not accepted. Pin
that wiring with a removal-regression test in both local hooks and CI. Update
the canonical agent contract and its
compiled projections through HISS-21, relabel the README badge, and remove the
redundant visible heading beneath the banner.

## Alternatives considered

| Option | Benefit | Risk | Decision |
| --- | --- | --- | --- |
| Change only the badge and heading | Tiny visual diff | Claims HISS-21 while the replay command still fails | Rejected |
| Copy Praetor's complete catalog unchanged | Broad corpus immediately | Its external-tool claims describe Praetor, not VMAFx | Rejected |
| Declare every VMAFx gate as replayed | Looks comprehensive | The verifier only replays its own scanner and would overclaim delegated tools | Rejected |
| Record measured scanner claims and require cross-platform replay | Evidence matches the command's real capability | External gates retain their own evidence paths | Chosen |

## Consequences

- **Positive**: removing the catalog, losing a positive match, overmatching a
  negative fixture, or unexpectedly closing a recorded gap fails a required
  check.
- **Positive**: the same corpus must pass on Linux, macOS, and Windows.
- **Positive**: the aggregator cannot reinterpret a missing or skipped replay
  job as an acceptable path-filter skip.
- **Negative**: the standards workflow adds three small Go-backed matrix jobs.
- **Neutral / follow-ups**: external compiler, ABI, supply-chain, and coverage
  gates remain independently required; future catalog rows need fixtures that
  the replay command can actually execute.

## References

- [ADR-1249](1249-praetor-governance-adoption.md) introduced the Praetor ratchet.
- [Research-2074](../research/2074-hiss-21-replay-evidence.md) records the measured gap and fixture selection.
- `req` (2026-09-20): “well the badge says 16 as well”.
- `req` (2026-09-20): “well then do that asap wtf... thats a huge blindspot in ci as well then”.
