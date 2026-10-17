<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3315: Local images and local packages are installed pinned

- **Status**: Accepted
- **Date**: 2026-10-14
- **Deciders**: Lusoris (operator decision vmafx-red-main-20261013-scorecard-plan)
- **Tags**: ci, scorecard, security, docker, python

## Context

OpenSSF Scorecard evaluation on `master` reported two remaining unpinned dependency findings under the Pinned-Dependencies check, resulting in a check score of 9 and an aggregate repository score of 8.404762, which is below the project's 8.5 quality gate threshold:

1. **F1**: `Dockerfile.ffmpeg:3` specified `FROM vmaf:latest` (an unpinned tag). Although `vmaf` is built locally from the root `Dockerfile`, Scorecard inspects all `FROM` declarations statically. Under Scorecard v5.6.0 rules (`checks/raw/pinned_dependencies.go`), a `FROM` reference is considered pinned when it matches `.*@sha256:([a-f\d]{64}|\${.*})`, and `ARG` declarations without defaults are accepted. Docker BuildKit permits building against a local base image by digest via `--build-arg VMAF_IMAGE_DIGEST=$(docker image inspect --format '{{.Id}}' vmaf | cut -d: -f2)`.
2. **F2**: In `.github/workflows/tests-and-quality-gates.yml`, the RC1 Tester Report job executed `python -m pip install --no-deps --no-build-isolation ./tools/rc1-tester`. Under Scorecard v5.6.0 rules, `pip install` commands are recognized as pinned only when they pass `--require-hashes`, or use `-e` with `--no-deps` for local/VCS targets, or when every non-flag target argument ends with `.whl`. Installing a plain local directory is flagged as unpinned.

## Decision

The repository establishes that local image extensions and local package installations must satisfy Scorecard pinning rules without sacrificing reproducibility or hermeticity:

1. **Digest-pinned local container base**: `Dockerfile.ffmpeg` replaces `FROM vmaf:latest` with an un-defaulted `ARG VMAF_IMAGE_DIGEST` followed by `FROM vmaf@sha256:${VMAF_IMAGE_DIGEST}`. Callers supply the local image ID digest via `--build-arg VMAF_IMAGE_DIGEST=$(docker image inspect --format '{{.Id}}' vmaf | cut -d: -f2)`. The base-image validation gate (`scripts/ci/check-container-image-references.py`) updates its `LOCAL_FROM` table to accept `("Dockerfile.ffmpeg", "vmaf@sha256:${VMAF_IMAGE_DIGEST}")`.
2. **Wheel build and wheel install for non-editable local tools**: In `.github/workflows/tests-and-quality-gates.yml`, the RC1 Tester Report install step is replaced by building a wheel into a local staging directory (`python -m pip wheel --no-deps --no-build-isolation --wheel-dir dist/rc1-wheel ./tools/rc1-tester`) followed by installing the resulting wheel (`python -m pip install --no-deps dist/rc1-wheel/*.whl`).
3. **Fail-closed dependency lock checker**: In `scripts/ci/check_python_dependency_locks.py`, `_is_secure_install` drops the non-editable plain-directory branch. Non-editable local installations are permitted only when every package token ends in `.whl`. Plain directory and sdist installations without `-e` are refused. The finding diagnostic is reworded to reflect that plain directory trees require editable mode (`-e`) with both `--no-deps` and `--no-build-isolation`.
4. **Fixture and consumer contract tests**: Unit tests in `scripts/ci/tests/test_python_dependency_locks.py` and `scripts/ci/tests/test_base_image_single_source.py` are updated with positive and negative assertions covering the digest-bound base image and wheel installation rules. Documentation in `docs/usage/docker.md` and `docs/development/base-images.md` is updated to describe the digest build argument.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Do nothing | No code or workflow changes | Scorecard Pinned-Dependencies stays at 9; total score 8.404762 remains below 8.5 gate | Fails required security gate on `master` |
| Pin with `--require-hashes` on local source directory | Uses standard pip flag | Pip hashes distribution archives (wheels/sdists), not source directories; unsupported by pip | Technologically infeasible without building archives first |
| Use `-e` editable install for rc1-tester | Scorecard accepts `-e` + `--no-deps` for local paths | Links to workspace source checkout; obscures packaging defects and drifts from true installed-package testing | The report tester needs to test the packaged entry points rather than editable source links |
| Build local wheel with `--no-deps --no-build-isolation` and install `.whl` with `--no-deps`; digest-pin `Dockerfile.ffmpeg` (chosen) | Meets Scorecard 10/10 pinned dependencies; guarantees exact base image; preserves hermetic testing | Requires two-step wheel build in CI step and `--build-arg` for local Dockerfile | Satisfies all supply-chain and gate invariants cleanly |

## Consequences

- **Positive**: OpenSSF Scorecard Pinned-Dependencies score advances from 9 to 10; repository overall score exceeds the 8.5 threshold; base image reference in `Dockerfile.ffmpeg` is bound to the exact local image build ID; local package installation in CI runs against a built wheel.
- **Negative**: Manual builds of `Dockerfile.ffmpeg` require passing `--build-arg VMAF_IMAGE_DIGEST`; omitting the argument fails the build.
- **Neutral / follow-ups**: Documentation in `docs/usage/docker.md` and `docs/development/base-images.md` reflects the `--build-arg` pattern; `scripts/ci/AGENTS.d/base-images.md` and `python-locks.md` record the invariants.

## References

- Operator decision `vmafx-red-main-20261013-scorecard-plan` (paraphrased): Pin remaining Scorecard unpinned dependencies by digest-pinning `Dockerfile.ffmpeg` base and installing local packages from wheels.
- OpenSSF Scorecard v5.6.0 source: `checks/raw/pinned_dependencies.go`.
- [ADR-1231](1231-base-image-single-source.md), [ADR-1305](1305-hash-locked-python-installs.md).
