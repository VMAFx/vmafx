<!-- markdownlint-disable MD013 MD060 -->
# ADR-2796: Every clang-tidy ratchet lane is a hosted, path-routed required check

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: `ci`, `lint`, `docker`, `cuda`, `sycl`, `hip`

## Context

[ADR-1142](1142-whole-codebase-standards.md) bounds clang-tidy findings per
file with a baseline per lane. Only the `cpu` lane (`Tidy Ratchet`) was a
hosted, required check. The `cuda`, `hip`, `sycl`, `arm64` and `clang` lanes
were measured by hand in the dev container
([ADR-1471](1471-tidy-lanes-in-dev-container.md)), and `Tidy Metal` ran
weekly on a macOS runner without being required. A defect in a lane's files
reached master unseen: the `cuda` lane reported an unused out-parameter in
`mcp_server.c` and a baseline entry for a deleted file, and the `clang` lane
could not build its fuzz targets, each found only when someone measured the lane.

The maintainer decided (Q-315, paraphrased) that hosted CI runs every tidy lane,
scoped by path, with a nightly sweep of all lanes on master that opens or updates
one issue on drift.

## Decision

We will make every lane a required check that always reports and measures only
when a file of that lane changes:

- Six selectors `tidy_cuda`, `tidy_hip`, `tidy_sycl`, `tidy_arm64`, `tidy_clang`
  and `tidy_metal` in `.github/ci-impact.json`, each `own_paths_only`
  ([ADR-1700](1700-tester-selectors-own-paths-only.md)): the lane's own source
  directories and tests, the ratchet, the lane's baseline, the lane scripts and
  the workflow that hosts it. A CI-authority change that touches none of those
  does not start a lane. A test pins that every file a lane measures and the
  `cpu` lane does not matches its selector.
- The device and cross lanes are the legs of one matrix job, `Tidy Lane (<lane>)`,
  in `lint-and-format.yml`. Each leg always starts; when its selector is false it
  prints that the lane was not measured (HISS-18), otherwise it runs
  `scripts/dev/tidy-lane.sh` against the dev container pinned by digest in
  `.github/actions/tidy-lane/action.yml`, the toolchain the baselines are
  written with. `Tidy Metal` becomes a gate job over the macOS work, which runs
  only when `tidy_metal` is selected.
- All six names join the `required` and `strictMustReport` lists of the
  aggregator.
- `nightly.yml` sweeps every lane on master without routing and
  `tidy-metal.yml` runs nightly; a failure opens or updates the issue "Tidy
  ratchet drift on master" (`scripts/ci/tidy-drift-issue.sh`).
- A lane was made green on master before it was made required: the code was fixed,
  never a baseline by hand.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the lanes manual and nightly only | No hosted cost | A lane defect lands and waits for the next measurement; the three findings above | The maintainer decided against it |
| Run every lane on every pull request | Simple; no routing | A 30 GB image pull and 10 to 40 minutes per lane per pull request; macOS billing | Cost with no information on a change the lane does not read |
| Route by the planner's `c_core` selector | Reuses an existing selector | `c_core` is true for any file under `core/`, which is every lane | No scoping |
| One job with a container and a job-level `if` | One definition | A skipped job reports `skipped`, which a strict required context rejects; the container is pulled at job start | The matrix leg always starts and skips its steps |
| Metal in the same matrix | One definition | The lane needs Apple's SDK on a macOS runner | Kept as the existing workflow, gated the same way |

## Consequences

- **Positive**: a change to a device, cross or Metal source is checked by its lane
  before merge; a drift the routing cannot see shows up within a day as one issue.
- **Negative**: a lane leg that is selected pulls the dev image (tens of
  gigabytes) and runs 10 to 40 minutes; a change to a shared file read by a lane
  (for example `core/src/libvmaf.c`) selects `cpu` only, and the other lanes find
  it in the nightly sweep. A `Makefile` or `.clang-tidy` edit selects every lane.
- **Neutral / follow-ups**: the digest in the action moves with the dev
  container; a bump re-measures the baselines in the same pull request. The
  package must stay readable by the repository's `GITHUB_TOKEN`.

## References

- `req` (maintainer decision Q-315, 2026-10-08): hosted CI runs every tidy lane, path-scoped,
  with a nightly sweep of all lanes on master that opens or updates one issue on drift.
- [ADR-1142](1142-whole-codebase-standards.md), [ADR-1471](1471-tidy-lanes-in-dev-container.md),
  [ADR-1700](1700-tester-selectors-own-paths-only.md), [ADR-1140](1140-ci-impact-planner.md).
