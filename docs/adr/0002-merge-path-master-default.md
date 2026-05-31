<!-- markdownlint-disable MD013 MD060 -->
# ADR-0002: Merge path gpu-opt to sycl to master, master is fork default

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: git, release, workspace

## Context

The fork carried multiple long-lived branches (`gpu-opt`, `sycl`, `master`) with `sycl` serving as the de-facto integration branch. Upstream Netflix/vmaf uses `master` as its default. The fork needed a predictable merge path and a default branch that matched the golusoris model ("main = HEAD, branches merge back").

## Decision

We will integrate via `gpu-opt → sycl → master`, make `master` the fork default on `origin` (`gh api -X PATCH repos/VMAFx/vmafx -f default_branch=master`), and keep `upstream/master` fetchable via `git fetch upstream` for periodic syncs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rename `sycl` to `main` | Matches modern "main" convention | Loses historical meaning of `master` vs `sycl`; forces retagging everywhere; diverges from upstream naming | Too costly for a rename; the chosen option matches the golusoris model directly |
| Keep `sycl` as default | No rename churn | Confuses external consumers; breaks upstream sync expectations | `master` is what every Netflix/vmaf doc, CI config, and release tool expects |

Rejected based on the rationale note: the user phrased this as "main = HEAD, branches merge back like golusoris". Merging into `master` and setting it as default matches the golusoris model directly and keeps `upstream/master` usable for future Netflix syncs.

## Consequences

- **Positive**: `master` stays usable for upstream sync PRs without translation; branch protection (ADR-0037) applies to the branch external consumers land on.
- **Negative**: downstream users who had bookmarked `sycl` must update.
- **Neutral / follow-ups**: auto-push policy in ADR-0004 complements this.

## References

- Source: `Q1.2`, `Q3.2`
- Related ADRs: ADR-0004, ADR-0037
