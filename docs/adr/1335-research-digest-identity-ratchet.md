<!-- markdownlint-disable MD013 MD060 -->
# ADR-1335: Bind research-digest debt to the trusted merge base

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: VMAFx maintainers
- **Tags**: docs, ci, testing, correctness, ratchet, fork-local

## Context

The research tree predates the one-numeric-ID-per-digest convention. A narrow
repair found 53 collision sets and 210 non-canonical H1 headings on the trusted
base. Keeping an exact baseline makes gradual cleanup practical, but a baseline
stored only on the proposed branch is not authority: a change can add debt,
rewrite the JSON to match, and make both the tree scan and generated-form check
green. Deleting the file and running an unconstrained writer has the same
laundering effect.

The first ratchet candidate demonstrated both failure modes. It also revealed
new train-era debt—a three-way Research-2080 collision and two malformed H1s—
that its self-authored 52/211 baseline had accepted. The gate needs an immutable
comparison point while still permitting reviewed debt reduction and a bounded
first adoption when the trusted branch predates the ratchet itself.

## Decision

Every audit will resolve the merge base between `HEAD` and an explicit trusted
base ref, then compare the proposed baseline with debt at that immutable
revision. If the trusted revision contains a canonical baseline, the proposal
may only remove entries from it. During first adoption only, when the trusted
revision contains neither the checker nor a baseline, the gate derives
authority from that revision's `docs/research/` tree and still rejects growth.
The checked-in baseline must exactly equal the current tree in both cases.

Ordinary `--write` requires a canonical baseline at the trusted merge base and
fails when it is absent. The separate `--bootstrap-from-ref` path accepts only
a full 40-character ancestor commit, requires that it predate both checker and
baseline, refuses to overwrite a current baseline, and rejects debt not present
in that tree. CI and pre-commit never invoke bootstrap; required CI passes the
pull request's exact `base.sha` as trusted authority.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Trust the branch-owned generated baseline | Small implementation; deterministic JSON | Adding debt and regenerating the file makes the gate green | It checks self-consistency, not regression |
| Hard-code one baseline hash in the checker | Simple immutable comparison for the first snapshot | Every legitimate cleanup couples data and code hashes; both remain branch-owned on first adoption | Merge-base debt comparison expresses the real monotonic policy |
| Normalize all inherited debt before adding a gate | Ends with no baseline | Rewrites hundreds of links across unrelated active workstreams in one high-conflict change | A trusted monotonic ratchet permits safe reviewed batches |
| Trusted merge-base authority with bounded first adoption | Rejects deletion, rewrites, and debt growth while permitting cleanup | Requires full Git history and a small Git-tree reader | Chosen; required CI already checks out full history |

## Consequences

- **Positive**: a branch cannot bless a new collision or malformed H1 by
  changing or recreating its own baseline.
- **Positive**: fixing inherited debt remains additive to policy; the baseline
  can shrink without a policy bypass.
- **Negative**: the checker needs reachable Git history and fails closed when
  the trusted ref, merge base, baseline, or tree cannot be read.
- **Neutral / follow-ups**: inherited debt remains tracked for reviewed batch
  cleanup; bootstrap is a one-time operator action and never a CI/hook mode.

## References

- [Research-2114](../research/2114-research-digest-ratchet-authority.md)
- [ADR-0108](0108-deep-dive-deliverables-rule.md)
- Source: `req` — “oh of course all bugs.md's in this local repo should of course be fully fixed”
