<!-- markdownlint-disable MD013 -->
# ADR-3559: Offline pre-push checks for upstream contribution branches

- **Status**: Accepted
- **Date**: 2026-11-17
- **Tags**: upstream, process, tooling, testing

## Context

The upstream review audit identified recurring classes across our own contribution pull requests. The class counts and questions are recorded in `scripts/dev/upstream_review_classes.json`. Q-303 required an attribution rewrite of 27 branches; preventing the same class before another push needs an executable check alongside review evidence.

Contribution branches have different conventions from this fork: subsystem commit subjects, small ranges, no fork-only files, and verified signatures. The upstream contribution process does not require DCO sign-off. The existing sweep verifies ownership, signatures, leases, and builds, but previously had no common gate for message shape, leaked artifacts, test omissions, stale draft commit IDs, or missing self-review evidence.

## Decision

Add `scripts/dev/upstream_push_gate.py` and a schema-1 class catalogue. The catalogue is the single machine-readable class list for the gate, lane briefs, and self-review. The gate refuses missing implementations or implemented IDs absent from that list. Each implemented class has a named result and a planted regression in `scripts/dev/tests/test_upstream_push_gate.py`.

The first round checks local refs only. C01 (exact-head CI) and C10 (overlapping open contributions) remain listed as online checks with explicit SKIP results until a later round. C02 is catalogued as non-actionable. Every other listed class has either a mechanical check or a required self-review answer; the [operator guide](../development/upstream-pr-sweep.md#pre-push-gate-for-upstream-branches) documents the checks and answer format.

Reuse the sweep's allow-listed, timed Git runner and signature counter. Extend the counter with an optional head argument so verification covers the immutable commit being pushed even when symbolic HEAD changes during a build. Reuse only the attribution functions from `scripts/ci/check-dco.py`, loaded with `importlib.util.spec_from_file_location`; do not invoke its sign-off policy.

The sweep calls the gate directly before pushing `base_sha..new_sha`, with `--mechanical-only "rebase-only push"`. Any FAIL refuses the push and supplies its first failing result as the row detail. A person publishing a substantive change supplies the full self-review file. Rebase mode still enforces C04 and C07; a test omission or multi-commit branch therefore needs manual review before publication.

Pass `--self-review <file>` to the sweep to supply that review. The UTF-8 file
uses `Cxx: <answer>` lines, including a C04 or C07 reason where applicable.

## Alternatives considered

| Alternative | Benefit | Reason not selected |
| --- | --- | --- |
| Install a hook only in the contribution clone | Automatic local enforcement | A clone-local installation is easy to omit and does not protect the sweep's explicit push path. |
| Put a checklist only in the pull request body | Minimal tooling | It cannot refuse a bad push and does not exercise recurring mechanical defects. |
| Reuse the entire DCO checker | One existing entry point | It would add an upstream sign-off requirement; only its shared attribution policy applies here. |
| Perform online checks in the first round | Broader coverage | Local correctness can be tested without remote availability or authentication; online classes remain explicit deferred work. |

## Consequences

- Upstream-shaped signed patches with tests and review evidence can be checked before publication using the same implementation as the sweep.
- Local upstream refs must be current: a clean local merge does not demonstrate fresh remote CI or absence of overlapping contributions.
- Self-review validates evidence presence and applicability, rather than proving the measurements or review conclusions.
- Missing inputs, failed Git operations, and oversized inputs refuse verification. Explicit skips always state their reason.
- The pre-commit contract and affected-suite discovery select the new regressions; fixtures use disposable signed repositories and no remote service.

## References

- `scripts/dev/upstream_review_classes.json`: recurring review classes, counts, modes, and questions.
- `scripts/dev/upstream_push_gate.py`: check registry and bounded offline implementation.
- `scripts/dev/tests/test_upstream_push_gate.py`: positive, planted negative, boundary, and sweep integration cases.
- [Upstream sweep guide](../development/upstream-pr-sweep.md): publication workflow and self-review example.
