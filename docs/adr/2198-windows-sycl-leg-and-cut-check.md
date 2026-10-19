<!-- markdownlint-disable MD013 MD060 -->
# ADR-2198: A tester leg builds where its inputs change, and no release is cut on a leg nobody saw green

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: `ci`, `release`, `supply-chain`, `testing`

## Context

The x64 SYCL Windows tester zip failed `check-windows-bundle-imports.py` on every
master push run that built it (runs 37281677119 and 37292850622 on 2026-10-05) and
again on the rc.3 publish dispatch (37594655522, 2026-10-07): the Level Zero loader
imports `cfgmgr32.dll`, a System32 DLL the checker did not list. No required check
noticed, and no rc.3 Windows zip was published. Measured over the last 91 master and
dispatch runs of `windows-tester-bundle.yml`, the SYCL leg ran four times and was red
every time; the 2026-10-05 run 37271658228 was a different, transient failure (the 2.5 GB
oneAPI installer file still held by the extractor when the step deleted it; the step now
retries and warns).

Three routing rules let this happen:

- ADR-1595 builds only the x64 zip on a pull request, so the SYCL, CUDA and arm64 legs
  run on a master push only; a red master push run is not a required check.
- ADR-1687 and ADR-1700 route the zip workflow by selector, and a master push skips
  every leg whose inputs did not change, so after the first red push nothing ran the leg
  again until the cut.
- ADR-2169 (CI tiers) makes the light tier skip the whole lane, `Windows Tester Zip`
  included, so a pull request that changed the zip's inputs would build nothing.

The macOS bundle (`macos-tester-bundle.yml`: weekly schedule and dispatch only) and the
tester image's arm64 leg (`docker-publish-tester.yml`: push and dispatch only) have the
same shape: their legs are not part of any pull request.

## Decision

1. **The x64 SYCL zip builds on a pull request when something it reads changes.**
   Selector `windows_tester_zip_sycl` of `.github/ci-impact.json` (`own_paths_only`) lists
   the x64 zip's inputs plus what only the SYCL leg reads: `sycl-rows.json`,
   `prepare_build.py`, `build-config.env` (the oneAPI and Level Zero pins) and
   `core/src/sycl/scratch_ratchet.txt`. A pull request builds the x64 zip, the SYCL zip, or
   both, as the two selectors say; the arm64 and CUDA zips stay with the push run.
2. **The lane runs in the light tier too.** `.github/ci-tier.json` gains
   `own_input_lanes`: a full-only context whose lane still plans and gates on a light-tier
   pull request (the tier job's `light` output, not `full`). The planner decides what
   builds; a light pull request that changed no input builds nothing. The release pull
   request without the cut label (`release-light`) still runs none of it. The routing
   contract test (`test_ci_routing_contract.py`) asserts the lane's planner and gate run
   for an own pull request and plants the old full-tier gate as a defect it must catch.
3. **A release is cut only on tester legs seen green.**
   `scripts/release/candidate-legs.json` lists every leg of the Windows zips (all four),
   the macOS bundle and the tester images; `scripts/release/check-candidate-legs.py`
   requires each to have concluded `success` in a completed run of the exact commit. A
   run counts when it is a push or schedule run on that commit, or a dispatch whose run
   title carries the commit's full SHA (the three workflows now set `run-name` so a
   dispatch names its source). Skipped, absent and red are all not green. The
   `Release Script Contract` job runs the check on a release pull request that carries the
   `autorelease: cut` label, against the pull request's base commit (the cut commits touch
   only the changelog and manifest), and the cut procedure in `docs/development/release.md`
   dispatches the legs at that commit first.

This partly supersedes ADR-1595 (the PR-time build is no longer the x64 zip alone) and
amends ADR-2169 (a light-tier pull request may run a declared own-input lane).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Build all four zips on every pull request | Simplest; no selector | Four Windows builds of up to 150 minutes each per pull request | The measured cost ADR-1595 and ADR-1687 weighed; the selectors give the same coverage when it matters |
| Build the SYCL zip on a pull request only for SYCL-only paths | Cheapest | The shared build script and the import checker, which broke it, would not select it | The defect lived in a shared script; the selector holds every input the leg reads |
| Make the master push run a required check | Reds block merges | A push run exists only after the merge; cannot block | Detects after the fact, as before |
| Cut check inside `rollover-changelog-fragments.sh` | One script | The rollover is offline and hermetic by contract (clean tree, receipts) | A network check belongs next to the other forge-reading cut gates |
| Cut check as a documented manual step only | No code | A check never enforced is not a check | Kept as the procedure, enforced by the contract job |

## Consequences

- **Positive**: a change to the SYCL leg's inputs is built before it merges; a red leg
  fails `Windows Tester Zip`, which the aggregator holds even in the light tier; a cut
  cannot merge while any Windows, macOS or tester-image leg is unseen or red.
- **Negative**: a shared change (the build script, the checker, `build-config.env`) now
  costs two Windows builds per pull request; a cut needs the legs dispatched at the base
  commit first, and a new master commit after that requires dispatching again.
- **Neutral / follow-ups**: the CUDA and arm64 Windows zips and the tester image's arm64
  leg still build on pushes only; the cut check covers them. Adding a leg means adding its
  job name to `candidate-legs.json` (a test pins the names to the workflows).

## References

- `Q-085` (maintainer decision, 2026-10-07): close the gate hole in a separate pull
  request: the SYCL leg builds on a pull request when its inputs change, in the light
  tier too; every Windows leg green on the exact candidate head before a cut.
- `Q-097` (maintainer decision, 2026-10-07): the x64 zip and the x64 SYCL zip each build on a
  pull request when their own inputs change.
- `Q-098` (maintainer decision, 2026-10-07): the candidate head of the cut check is the
  release pull request's base commit.
- [ADR-1595](1595-pr-time-verify-push-only-workflows.md),
  [ADR-1687](1687-required-release-dry-run-legs.md),
  [ADR-1700](1700-tester-selectors-own-paths-only.md),
  [ADR-1566](1566-windows-sycl-tester-zip.md),
  [ADR-2169](2169-ci-fewer-runs.md).
