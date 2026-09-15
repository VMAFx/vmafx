# ADR-1240: Maintain the FFmpeg patch stack against stable releases

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: Lusoris
- **Tags**: build, ci, ffmpeg

## Context

Patch 0018 duplicated the percentile mappings already inserted by patch 0005.
It reached master because the FFmpeg build was advisory, the local replay
hook ran only at pre-push, and the documented 000* glob omitted later patches.
The old checker also returned success after network preparation failed.

The user requires a current upstream release and automatic patch maintenance,
while asking that upstream discovery not run independently on every PR.

## Decision

Keep the FFmpeg remote and release tag in the shared root build-config.env.
Only nMAJOR.MINOR and nMAJOR.MINOR.PATCH tags qualify as stable releases;
development, RC, snapshot and arbitrary commit refs are excluded.

Local hooks regenerate the complete series when relevant inputs change.
A required FFmpeg Patch Stack job checks the same replay and canonical output.
Both fetch the configured stable tag into disposable storage, apply entries
from series.txt in order, and fail on missing files, conflicts or fetch errors.
They never reset, clean or reuse a contributor's FFmpeg checkout.

Scheduled release discovery selects the highest stable tag numerically.
It owns FFmpeg discovery; remove the redundant Renovate manager that proposed
pin-only updates without rebasing the series.
The updater applies the old series first, rebases those commits onto the new
release, and formats every patch reproducibly. It changes tracked files only
after the whole series succeeds. Conflicts retain diagnostics for review;
no generated resolution guesses at integration semantics. The proposed diff
and exact upstream commit receipt are retained by CI for review.

Container ARG defaults mirror the shared tag and a drift check covers them.
Release builds use a reviewed tag; no development-master dependency is added.
Changes to the public API still need an integration implementation when
necessary: patch regeneration cannot invent the semantics of a new C API.

## Alternatives considered

- Keep the historical tag and optional build: fails to detect release drift
  and reproduced the patch-0018 regression.
- Follow individual master commits: rejected by the user's released-tag rule.
- Discover latest upstream on every PR: makes unrelated pushes depend on a
  moving upstream target; discovery belongs to the scheduled refresh.
- Resolve conflicts by choosing one side: can drop integration behavior;
  preserve the failed candidate for a deliberate source-level fix.

## Consequences

Network failure makes replay unverified and fails the gate. Relevant local
commits require a working upstream connection. Stable release updates produce
reviewable patch and configuration changes; they never publish a release.

## References

- req: "we actually can automate that (if its needed)"
- req: "please not on single commits/pr's"
- req: "well so the latest master release (not dev)"
- [ADR-0118](0118-ffmpeg-patch-series-application.md)
- [FFmpeg downloads](https://ffmpeg.org/download.html)
