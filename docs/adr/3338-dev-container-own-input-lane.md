<!-- markdownlint-disable MD013 MD060 -->

# ADR-3338: Dev Container Build runs on light-tier pull requests that change the image's own inputs

- **Status**: Accepted
- **Date**: 2026-10-17
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `docker`

## Context

Pull request #2821 added a ClangBuildAnalyzer build to the `build-deps` stage of
`dev/Containerfile` and merged with every required check green. The first master push
after it, `2c0db0b99` (run 39006126490), was red: `Dev Container Build` stopped in that
step because CMake's default generator needs a make program and the stage installs only
Ninja (fixed by #2824).

The pull request's own checks could not show it. `Dev Container Build` was a full-tier
context ([ADR-2169](2169-ci-fewer-runs.md)): its planner and its gate ran only when
`needs.tier.outputs.full` was true, so a light-tier pull request never built the image,
not even one whose whole diff was `dev/Containerfile`. The broad `dev_container`
selector of `.github/ci-impact.json` inherits the core, Python, AI, Go and shell
selectors, because the image copies all of those sources; running it on every light
pull request would build the image (20 to 45 minutes on a hosted runner) for most C
changes.

## Decision

`Dev Container Build` becomes the third own-input lane of `.github/ci-tier.json`
([ADR-2198](2198-windows-sycl-leg-and-cut-check.md), [ADR-3279](3279-msvc-sycl-leg-on-light-tier.md)):

- Its `impact` planner and its gate run on the light tier
  (`needs.tier.outputs.light == 'true'`). A release pull request without the cut label
  has no light tier, so the lane stays idle there.
- A new step `pick` of the planner chooses the selector by tier, in shell because a
  GitHub expression treats the string `'false'` as true: on the full tier (master push,
  `ci: full`, dispatch) the broad `dev_container` selector, on a light-tier pull request
  the narrow `dev_container_inputs` selector. Any value other than `true` or `false`
  fails the step, and the gate then fails because the planner did not succeed.
- `dev_container_inputs` is `own_paths_only` and lists the image's own inputs:
  `dev/**`, `.dockerignore`, `build-config.env`, the workflow itself and the scripts its
  work job calls (`build-dev-container-stage.sh`, `check-compose-dri-writable.sh`,
  `check-container-build.sh`, `check-dev-container-build-secret.py`,
  `docker-hub-mirror.sh`) plus `install-cuda-toolkit.sh`, which `dev/Containerfile`
  copies into the image. The workflow and scripts go beyond the three paths of the
  decision; they are derived from it, since a change to them changes what the build
  does.
- The gate writes the planner's reason as a notice when the work is skipped, so a
  docs-only pull request shows why the image was not built.

The narrow selector leaves these inputs to the full tier and the master push, where the
broad selector still runs as the second barrier: C and C++ sources and Meson files
under `core/`, the Python, AI, MCP and Go sources and their requirement locks the image
copies, the shell inherits of `dev_container`, `supply-chain.yml` and
`scripts/release/build-native-release-artifacts.sh`. A pull request that needs the
image built for one of those adds the label `ci: full`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Narrow own-input lane, broad selector on full (chosen) | Catches the class #2821 hit (a change to `dev/` or to what the build reads) before it lands; no image build for ordinary C changes; master keeps the broad build | Inputs the image copies from the rest of the tree can still break it after landing | Chosen by decision `vmafx-devcontainer-light-tier-gap` |
| Broad selector on the light tier | Every input that can break the image is built before landing | A 20 to 45 minute image build on most C pull requests; doubles the hosted-runner load the light tier exists to cut (ADR-2169) | Cost out of proportion to the defect class seen |
| Keep the context full-tier | No added runner time | A pull request that changes only `dev/Containerfile` lands without the image ever being built, which is how master went red at `2c0db0b99` | Leaves the measured gap open |
| Require the label `ci: full` on every `dev/` change | No workflow change | A rule that relies on memory; nothing enforces it | Not a working barrier |

## Consequences

- **Positive**: a light-tier pull request that changes `dev/`, `.dockerignore`,
  `build-config.env` or a build script builds the image and runs its smoke tests and
  the release rehearsal before it lands.
- **Negative**: those pull requests take the image build's time; changes to copied
  sources outside the narrow list are still caught only on the full tier or the master
  push.
- **Neutral / follow-ups**: `scripts/ci/tests/test_ci_impact.py` holds the selector's
  positive, negative and boundary cases (unknown change set, empty diff, a planted
  `docs/*` pattern); `scripts/ci/tests/test_ci_routing_contract.py` runs the `pick`
  step and plants three defects (the narrow selector read on the full tier, the gate
  and the planner moved back to `outputs.full`).

## References

- Decision `vmafx-devcontainer-light-tier-gap`, 2026-10-17 (paraphrased): an own-input
  lane with a narrow selector over `dev/**`, `.dockerignore` and `build-config.env`; the
  broad selector stays on master as the second barrier; the record names what the
  narrow selector leaves to master; check with a planted `dev/Containerfile`-only light
  pull request that runs the lane and a docs-only one that skips it with a reason.
- Pull request #2821 (the change that merged green), master push `2c0db0b99` (run
  39006126490), pull request #2824 (the Ninja fix).
