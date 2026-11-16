<!-- markdownlint-disable MD013 MD060 -->

# ADR-3554: Tester Image runs on light-tier pull requests that change the image's own inputs

- **Status**: Accepted
- **Date**: 2026-11-16
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `docker`, `release`, `supply-chain`, `testing`

## Context

Renovate pull request #3476 (`e36e898c5`) updated the `python:3.14-slim` digest
(`RELEASE_PYTHON_BASE`) in `build-config.env` and every Dockerfile that mirrors it,
including `docker/Dockerfile.tester` and `Dockerfile.go-server`. The new digest ships
CPython 3.14.10, whereas `tools/rc1-tester/image/licensing.json` recorded
`cpython_license_rst` entries only for 3.13.16, 3.14.7 and 3.14.8, and pinned
`python.version` `3.14.8` on `artifacts.image` and `artifacts.production-server-image`.
Every master push after #3476 failed in `Publish Tester Image` (`x86_64 reference scores`
and `Tester Image`) at the `licence-texts` stage (`docker/Dockerfile.tester` line 139,
`tools/rc1-tester/image/licensing.py` `fetch_texts`):
`licensing: no cpython_license_rst entry for Python 3.14.10`.

Pull request #3476 passed all required checks and automerged because Renovate pull
requests run the light CI tier ([ADR-2169](2169-ci-fewer-runs.md)):

- `Tester Image` was a `full_only` context of `.github/ci-tier.json` with no
  `own_input_lanes` entry, and `.github/workflows/docker-publish-tester.yml` gated both
  `impact` and `tester-image` on `needs.tier.outputs.full == 'true'`, so no pull-request
  build of `docker/Dockerfile.tester` ran even though selector `tester_image` of
  `.github/ci-impact.json` already owned `build-config.env` and
  `docker/Dockerfile.tester`.
- In `.github/workflows/required-aggregator.yml`, `delayedMissing` filtered `missing`
  (which already excluded `notOwed`, derived from `CI_TIER_FULL_ONLY`). Because
  `own_input_lanes` contexts remain in `CI_TIER_FULL_ONLY` and GitHub Actions creates a
  dependent gate's check run only after its `needs` work jobs finish, a light-tier pull
  request whose fast checks finish while `x86_64 reference scores` or
  `Build and test (amd64)` is still running would exit the poll loop before `Tester Image`
  registered and pass the aggregator.

## Decision

1. **Record Python 3.14.10 in `tools/rc1-tester/image/licensing.json`.** Add `"3.14.10"`
   to `cpython_license_rst` (`https://raw.githubusercontent.com/python/cpython/v3.14.10/Doc/license.rst`,
   SHA-256 `8a7329b25131f64b2bea284155f0824876b1af9f4a2f970e6335751d97dfa6dd`), keep the
   3.13.16, 3.14.7 and 3.14.8 entries, and bump `python.version` to `"3.14.10"` on
   `artifacts.image` and `artifacts.production-server-image`. Leave
   `published-rc-server-image` at `"3.14.7"`, which records an already-published image.
2. **`Tester Image` becomes the fourth own-input lane** of `.github/ci-tier.json`
   ([ADR-2198](2198-windows-sycl-leg-and-cut-check.md),
   [ADR-3279](3279-msvc-sycl-leg-on-light-tier.md),
   [ADR-3338](3338-dev-container-own-input-lane.md)), bound to the existing
   `own_paths_only` selector `tester_image` of `.github/ci-impact.json`:
   - In `.github/workflows/docker-publish-tester.yml`, `impact` and the `tester-image`
     gate run on `needs.tier.outputs.light == 'true'`. Because `tester_image` is already
     `own_paths_only` and serves both tiers, `impact` exports
     `steps.impact.outputs.tester_image` directly as `windows-tester-bundle.yml` does;
     an unselected run emits a skip notice on `tester-image` and reports `success`.
   - `validate`, `refs-x86` and `build` (narrowed to `amd64` on pull requests) stay gated
     on `needs.impact.outputs.selected == 'true'`. `build-gpu`, `publish` and
     `publish-gpu` stay off pull requests.
   - A release pull request without `autorelease: cut` (`release-light`) keeps
     `outputs.light == 'false'`, so the lane stays idle there.
3. **Hold the aggregator while an own-input lane's work jobs run.** In
   `.github/workflows/required-aggregator.yml`, `delayedMissing` checks every non-exempt
   required context whose `delayedStrictDependencies` jobs are active or within
   `registrationPropagationMs` (ignoring `'skipped'` dependencies on `notOwed` contexts
   so genuinely full-only skipped lanes add no wait). When `Tester Image` (or
   `Dev Container Build` or `Windows Tester Zip`) builds on a light-tier pull request,
   the aggregator waits until the gate check run registers and fails if it does not
   succeed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `Tester Image` as an own-input lane on `tester_image` (chosen) | Exercises `docker/Dockerfile.tester` and its `licensing.py` stages on light-tier pull requests that touch `build-config.env`, `Dockerfile.tester` or `tools/rc1-tester/**`; zero build on unrelated light pull requests; Renovate automerge settings stay unchanged | Adds an amd64 tester image build to light-tier pull requests that touch `tester_image` inputs; arm64, GPU images and copied `core/`/`python/` sources remain covered by master pushes and the nightly schedule ([ADR-1701](1701-nightly-tester-image-build.md)) | Matches the measured failure class of #3476 at bounded runner cost |
| Run the full tier on Renovate Docker digest pull requests | Exercises every container and platform lane before merge | Runs the 30+ leg C/C++, MSVC, macOS, GPU, sanitizer and FFmpeg build suite on container-only digest bumps, restoring the queue load [ADR-2169](2169-ci-fewer-runs.md) removed | Disproportionate runner cost for a container-input failure class |
| Static check comparing `build-config.env` with `licensing.json` | Instant, offline, zero container build | `RELEASE_PYTHON_BASE` pins the floating minor tag `python:3.14-slim@sha256:...`; neither `build-config.env` nor the Dockerfile text contains the patch version (`3.14.10`) that the digest resolves to inside the image | Cannot observe `platform.python_version()` of a digest-pinned floating tag without pulling and running the image |
| Add `Release Dry Run` images as a second own-input lane | Also builds `docker/Dockerfile.production`, `Dockerfile.operator`, `Dockerfile.go-server` and `Dockerfile.node` on light-tier digest PRs | `Tester Image` already builds `RELEASE_PYTHON_BASE` against the same `cpython_license_rst` table in `licensing.json` when `build-config.env` changes, while `Release Dry Run` fans out across five CPU/server/operator/node images, three GPU toolkit images, wheels and MCP builds | Duplicate coverage of the Python base-image licence check at several times the runner cost; kept on the full tier and weekly schedule |

## Consequences

- **Positive**: any light-tier pull request (Renovate included) that updates
  `build-config.env`, `docker/Dockerfile.tester`, `tools/rc1-tester/**` or another
  `tester_image` input builds and runs the amd64 tester image (including `licence-texts`,
  `notices` and `licence-check`) before merging, and the Required Checks Aggregator waits
  for `Tester Image` to register and blocks on failure.
- **Negative**: light-tier pull requests touching `build-config.env` or other
  `tester_image` inputs pay the amd64 tester image build time; `Release Dry Run`, arm64
  and GPU tester images still run only on the full tier, master push or schedule.
- **Neutral / follow-ups**: `scripts/ci/tests/test_ci_routing_contract.py` pins the
  `RENOVATE` light-tier run of `impact` and `Tester Image` and plants moving either job
  back to `outputs.full`; `scripts/ci/tests/test_ci_impact.py` pins PR #3476's
  `RELEASE_PYTHON_BASE` diff shape on `tester_image`;
  `scripts/ci/tests/test_ci_aggregator_tier.py` pins the light-tier `delayedMissing` wait
  and plants the `missing.filter` regression; `tools/rc1-tester/tests/test_licensing.py`
  pins Python 3.14.10 in `licensing.json`.

## References

- Order `vmafx-master-red-python-31410`, 2026-11-16, on tester image Python 3.14.10
  licensing and light-tier own-input lane routing (paraphrased): record CPython 3.14.10
  in `licensing.json` (`image` and `production-server-image`, keeping
  `published-rc-server-image` at 3.14.7), add `Tester Image` to `own_input_lanes` on
  `tester_image` without changing Renovate automerge settings, verify aggregator behavior
  on light-tier pull requests, and leave `Release Dry Run` out with its reason recorded.
- Pull request #3476 (`e36e898c5`), [ADR-1503](1503-tester-artifact-licensing.md),
  [ADR-1513](1513-production-artifact-licensing.md),
  [ADR-1687](1687-required-release-dry-run-legs.md),
  [ADR-1700](1700-tester-selectors-own-paths-only.md),
  [ADR-1701](1701-nightly-tester-image-build.md),
  [ADR-2169](2169-ci-fewer-runs.md),
  [ADR-2198](2198-windows-sycl-leg-and-cut-check.md),
  [ADR-3279](3279-msvc-sycl-leg-on-light-tier.md),
  [ADR-3338](3338-dev-container-own-input-lane.md).
