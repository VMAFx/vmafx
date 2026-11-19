<!-- markdownlint-disable MD013 MD060 -->
# Verifying the release and tester workflows before they publish

Six workflows publish something and used to start only after a merge or a
release: the tester image, the Windows zip, the macOS bundle, the production
images, the operator / server / node images and the supply-chain run. A change
to their inputs was first built by the merge or the release itself. Each of them
now has a pull-request or scheduled verify that builds and runs the same
targets without publishing. The choice per workflow is recorded in
[ADR-1595](../adr/1595-pr-time-verify-push-only-workflows.md). Three of those
pull-request runs block a merge: `Tester Image`, `Windows Tester Zip` and
`Release Dry Run` are required contexts of the Required Checks Aggregator
([ADR-1687](../adr/1687-required-release-dry-run-legs.md)).

## When you need this page

A pull request shows a red `Tester Image`, `Windows Tester Zip` or
`Release Dry Run` check (or a red job of the workflows `Publish Tester Image`,
`Publish Windows Tester Bundle` and `Release Dry Run`), or you change a Dockerfile, a toolkit pin
(`build-config.env`), the licence inputs (`tools/rc1-tester/image/`) or the
`vmaf-mcp` package and want to know which build will exercise it.

## What runs where

| Workflow | Pull request | Weekly | Left to the merge or the release |
| --- | --- | --- | --- |
| `docker-publish-tester.yml` | `Build and test (amd64)` and the x86_64 reference scores, when the planner selects `tester_image`; required context `Tester Image` | none | arm64 image; Intel, NVIDIA and AMD GPU images; publishing |
| `windows-tester-bundle.yml` | the x64 zip: build, run, verify, SBOM, when the planner selects `windows_tester_zip`; required context `Windows Tester Zip` | none | arm64, x64 CUDA and x64 SYCL zips; publishing |
| `macos-tester-bundle.yml` | none (macOS minutes cost ten times a Linux minute) | Monday 04:23 UTC: build and verify master's head | publishing |
| `docker-publish-production.yml` | `Release Dry Run`: the CPU CLI image and the MCP server image (linux/amd64, `--version`, a score with the built-in model); the CUDA 13, ROCm 10 and oneAPI 2026 images built, not run | Wednesday 03:41 UTC: all of it | arm64; HTTP startup; signing; attestation; publishing |
| `docker-publish-operator-node.yml` | `Release Dry Run`: the operator, vmafx-server and vmafx-node images (linux/amd64, `--version` equals the stamped tag, a score, the node's ffmpeg and rclone) | Wednesday 03:41 UTC: all of it | arm64; HTTP startup; the multi-arch manifest; signing; publishing |
| `supply-chain.yml` | `Release Dry Run`: the `vmaf-mcp` wheel and sdist named for the release version, the SBOMs of the installed package and the check of those SBOMs against the files | Wednesday 03:41 UTC | native artifacts (rehearsed by `dev-container-build.yml`, [ADR-0819](../adr/0819-dev-container-ci-gate.md)); signing; provenance; the PyPI publication |

Nothing in a pull-request or scheduled run logs in to a registry, pushes, signs,
attests or creates a release; `scripts/ci/tests/test_pr_time_verify_workflows.py`
fails if `release-dry-run.yml` gains any of those.

## Which dry run a pull request gets

`Release Dry Run` always starts and its `Plan release dry run` job decides, from
the changed paths, which of three groups to run. [`scripts/ci/release-dry-run-plan.sh`](../../scripts/ci/release-dry-run-plan.sh)
holds the lists:

| Group | Runs when the diff touches |
| --- | --- |
| images | `docker/Dockerfile.production`, `docker/Dockerfile.operator`, `docker/Dockerfile.node`, `Dockerfile.go-server`, `ffmpeg-patches/`, `build-config.env`, the licence inputs (`tools/rc1-tester/image/`, `REUSE.toml`, `LICENSES/`), the install scripts the images copy, the two publishing workflows, the dry run itself |
| gpu | `docker/Dockerfile.production-gpu`, the CUDA, ROCm and oneAPI install scripts, `build-config.env`, the licence inputs, the production workflow, the dry run itself |
| mcp | `mcp-server/vmaf-mcp/`, `requirements/locks/package-build.txt`, `scripts/release/verify-mcp-sbom.sh`, `scripts/release/pep440-version.sh`, `supply-chain.yml`, the dry run itself |

A group that is off is not exercised by that pull request; the step summary of
the plan job says so. The gate job `Release Dry Run` reports on every pull
request: it passes when the plan succeeded and every group is either selected
and green or unselected and skipped.

## When the tester image and the Windows zip build

Both workflows start on every pull request and every push to `master`. Their
first job (`Plan tester image impact`, `Plan Windows zip impact`) runs the CI
impact planner, [`scripts/ci/plan-ci-impact.py`](../../scripts/ci/plan-ci-impact.py),
and the build runs only when its selector in
[`.github/ci-impact.json`](../../.github/ci-impact.json) is true:

| Selector | Inputs |
| --- | --- |
| `tester_image` | `docker/Dockerfile.tester`, `tools/rc1-tester/`, the oneAPI ocloc, CUDA and ROCm install scripts, `build-config.env`, `dev/scripts/fetch-intel-neo.py`, `dev/scripts/intel-neo-lock.json`, `python/requirements-test-lock.txt`, `REUSE.toml`, `LICENSES/`, `docs/hardware-reports/report.schema.json`, the workflow |
| `windows_tester_zip` | `scripts/ci/build-windows-tester-bundle.py`, `scripts/ci/check-windows-bundle-imports.py`, `requirements/locks/windows-tester-zip.txt`, `tools/rc1-tester/image/windows/`, the unit, CUDA and SYCL test lists and `sycl-runtime-windows.json` under `tools/rc1-tester/image/`, the workflow |

These are the path lists the two workflows' triggers carried before ADR-1687,
and only these select a build. Both selectors are `own_paths_only`
([ADR-1700](../adr/1700-tester-selectors-own-paths-only.md)): when the
planner cannot bound a change (`mode=full`: a change under `scripts/ci/`, to a
workflow that hosts a required context, to `.standards-baseline.json` or
`Makefile`, a delete or rename, an unknown top-level path) it sets every other
selector, but these two only when one of the changed paths is their own. A
dispatch and the nightly schedule have no diff, so they always build. The plan
job's log names the mode and the reason. To see what your branch gets:

```bash
python3 scripts/ci/plan-ci-impact.py --event pull_request \
  --base "$(git merge-base origin/master HEAD)" --head HEAD --print
```

The gate (`Tester Image`, `Windows Tester Zip`) passes an unselected run without
work and a selected run only when every pull-request job of the chain passed:
the image gate waits for `Build and test`, which needs the reference scores and
the source validation; the zip gate waits for `Verify the zip and write its SBOM`,
which fails when its build left no zip. On a push to `master` the same gates
cover every leg the push run builds (both image architectures, all four zips),
and the master aggregator waits for them.

## The nightly tester image build

`docker-publish-tester.yml` also runs every night at 00:29 UTC on the head of
`master` ([ADR-1701](../adr/1701-nightly-tester-image-build.md)): the amd64 image is built, run with the documented command and its
report validated. Nothing is pushed, signed or attested, and no GPU image is
built. The image copies `core/`, `model/`, `python/` and `compat/`, which are not
pull-request inputs of the tester build; this run builds a change there within a
day. It ends, at its job timeouts, before `nightly.yml` and the weekly Release Dry
Run start.

A red nightly is a failed run of `Publish Tester Image` with the event
`schedule` on `master` (Actions tab, filter `event:schedule`). GitHub mails it to
whoever last changed the cron line of the workflow, and `praetorctl audit` lists
the workflow as failing on `master`. Reproduce it on the head of `master` with
`docker build --file docker/Dockerfile.tester .` and the documented
`docker run` line of [the tester image guide](../usage/tester-image.md).

## Reproducing a failure locally

The image dry run is `docker build` of the same file and target, then the smoke
script:

```bash
docker build --file docker/Dockerfile.production --target cli \
  --build-arg VMAFX_VERSION=dev --tag release-dry-run:cli .
bash scripts/ci/release-image-smoke.sh release-dry-run:cli vmaf dev
```

The kinds are `vmaf` (the CLI image), `mcp`, `wrapped` (vmafx-server), `node` and
`binary` (the operator); the header of the script says what each runs. The
`vmaf-mcp` SBOM check is `bash scripts/release/verify-mcp-sbom.sh <version> <sbom-dir> <dist-dir>`,
the script `supply-chain.yml` runs on a release. Its fixtures:
`bash scripts/release/tests/test-verify-mcp-sbom.sh`.

## Changing a release workflow

The dry run builds what the release builds. A new image target, a new
`vmaf-mcp` build command or a new Syft pin goes into both
`release-dry-run.yml` and the release workflow, or
`scripts/ci/tests/test_pr_time_verify_workflows.py` fails.

## Tester prereleases do not start release workflows

The tester workflows publish prereleases tagged `tester-*`, and a published
release fires the `release` event. `docker-publish-production.yml`,
`docker-publish-operator-node.yml` and `supply-chain.yml` therefore skip every
job unless the tag starts with `v` (the guard on `validate-release`; the other
jobs depend on it). A skipped workflow is neutral for the checks. Version
validation of a `v*` tag is unchanged. `scripts/ci/tests/test_release_workflows_version_tag_guard.py`
fails when an `on: release` workflow lacks the guard.
