# ADR-0819: PR-time CI gate for dev/Containerfile

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `workspace`

## Context

The fork's primary development environment is the `vmaf-dev-mcp` container
defined in `dev/Containerfile`. Prior to this decision there was no CI gate
on that file: regressions were only discovered at user runtime. The concrete
trigger was a session where source-path references in `dev/Containerfile` were
not updated after the `libvmaf/ → core/` rename (ADR-0700), causing the
container build to fail silently — a problem that was only surfaced hours later
during a local `docker compose build` run.

The existing `docker-image.yml` workflow targets the root-level `Dockerfile`
(the upstream-inherited image), not `dev/Containerfile`, and is marked
`continue-on-error: true` (advisory-only) with no smoke test. It does not
cover the dev container at all.

## Decision

We will add `.github/workflows/dev-container-build.yml` that triggers on
every PR that touches `dev/Containerfile`, `dev/scripts/`, `dev/docker-compose.yml`,
or the workflow file itself. The job builds the container up to the
`libvmaf-build` stage (covering all GPU SDK layers and the libvmaf source
build), then runs two smoke tests inside the resulting image: `vmaf --version`
and a CPU-backend scoring pass against the bundled 48-frame fixture YUVs.
No image is pushed. The job is blocking (not `continue-on-error`).

Targeting only `libvmaf-build` (not the full `dev-mcp` stage) keeps the gate
under 45 min on a free-tier runner while still covering the main regression
surface (GPU SDK package pinning, meson configure flags, ONNX Runtime install).
The FFmpeg stage and final assembly are exercised by the release-time
`docker-publish-production` workflow.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Full multi-stage build (all stages) | Validates the entire image end-to-end | 80–120 min on free-tier; exhausts runner minutes on every Containerfile PR | Too slow for a PR gate; covered at release time |
| Advisory-only gate (`continue-on-error: true`) | Never blocks a PR | Defeats the purpose; current docker-image.yml already proves advisory gates go unread | Not chosen — we need a hard failure signal |
| No gate (status quo) | Zero CI cost | Regressions discovered only at user runtime; exact failure mode that motivated this ADR | Not chosen |

## Consequences

- **Positive**: Any `dev/Containerfile` commit that breaks the build or makes
  the `vmaf` binary non-functional will fail the PR gate before it reaches
  `master`.
- **Positive**: GPU SDK package-pin regressions (the most common failure class
  from the path-rename incident) are caught at PR time, not at runtime.
- **Negative**: PRs that touch `dev/Containerfile` now incur a 30–45 min CI
  job. This is acceptable given how infrequently the file changes and how
  costly the alternative (user runtime failures) is.
- **Neutral / follow-ups**: The `required-aggregator.yml` does not list this
  check as a hard required gate (to avoid blocking unrelated PRs that touch
  only `dev/`). The check is required by path filter on the PR itself.

## References

- ADR-0700: `libvmaf/ → core/` rename (root cause of the path-stale incident).
- ADR-0317: path-filter rationale for docker-image.yml.
- `docker-image.yml`: existing advisory-only Dockerfile gate.
- Per user direction: the dev container build must be verified on every PR that
  touches it; the existing docker-image.yml did not cover dev/Containerfile.
