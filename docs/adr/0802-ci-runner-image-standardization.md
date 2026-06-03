# ADR-0802: CI Runner Image Standardization — Pin ubuntu-latest to ubuntu-24.04

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `build`

## Context

GitHub Actions' `ubuntu-latest` label is a floating alias. When GitHub
promotes the alias from `ubuntu-24.04` to `ubuntu-26.04` (expected H2 2026),
every workflow job that uses `ubuntu-latest` will silently switch to a new
system image — potentially picking up a different glibc, Python version, apt
package set, and default compiler. For a project that gates on numerical
correctness (Netflix golden assertions, CUDA/SYCL parity), silent host-image
churn is a risk: a toolchain version bump can shift floating-point rounding,
change default compile flags, or break package availability without any diff
in the repo.

An audit of all `.github/workflows/` files found:

- 13 workflow files used `runs-on: ubuntu-latest` (excluding Docker-build jobs
  where the host OS version is irrelevant).
- 2 ARC-runner conditional expressions fell back to `ubuntu-latest`.
- Matrix `os:` entries in `build.yml`, `libvmaf-build-matrix.yml`, and
  `ffmpeg-integration.yml` used `ubuntu-latest`.
- All CUDA pins were already consistent at `13.2.0` (ADR-0603) — no action
  needed.
- Windows runners were consistently `windows-2025` — no action needed.
- macOS matrix entries intentionally use `macos-latest` to track Apple
  Silicon availability; left unchanged.
- Container images: only one (`semgrep/semgrep`) is already SHA-pinned.
- `upstream-watcher.yml` used `ubuntu-latest` while all sibling watchers used
  `ubuntu-24.04`; corrected.

## Decision

Pin all non-Docker workflow jobs to `ubuntu-24.04` explicitly. Docker build
jobs (`docker-image.yml`, `docker-publish-production.yml`) are exempt because
they invoke `docker build` and the host OS version does not affect the produced
image. macOS matrix entries remain `macos-latest` by design.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `ubuntu-latest` everywhere | No churn today | Silent breakage when GitHub moves the alias to 26.04 | Risk outweighs convenience |
| Bump directly to `ubuntu-26.04` | Forward-compatible | 26.04 not yet GA on GitHub-hosted runners as of 2026-05-29 | Premature; revisit when GA |
| Pin only build/test workflows, leave infra workflows floating | Less churn in infra jobs | Inconsistency is confusing | Uniform policy is simpler |

## Consequences

- **Positive**: Deterministic host image; no surprise toolchain or glibc
  upgrades at GitHub's discretion. The fork controls when to bump to 26.04.
- **Negative**: Requires a deliberate PR to bump to `ubuntu-26.04` when the
  time comes (likely H2 2026).
- **Neutral / follow-ups**: When GitHub GA's `ubuntu-26.04`, open a follow-up
  chore PR to bump and update this ADR.

## References

- [GitHub Actions runner image changelog](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md)
- ADR-0603 — CUDA 13.2.0 pin (established consistent CUDA versioning)
- ADR-0664 — Windows 2025 runner pin
- Per user direction: standardize CI runner images for reproducibility.
