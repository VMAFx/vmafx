<!-- markdownlint-disable MD013 MD060 -->
# ADR-1102: Container-only canonical artifact publishing (Phase 4b.9)

- **Status**: Accepted
- **Date**: 2026-06-08
- **Deciders**: lusoris
- **Tags**: container, build, release, publish, phase4b, docs-policy, fork-local

## Context

The fork now ships five backends (CUDA, SYCL, HIP, Metal scaffolds, CPU) and
a production image stack at `ghcr.io/vmafx/vmafx`. The `vmaf-dev-mcp`
container at `dev/Containerfile` pins every toolchain version, GPU SDK, Python
dependency, and model file needed to produce a reproducible build. Host-side
meson/ninja builds do not pin these dependencies and diverge as host OS
packages update (compiler versions, CUDA driver ABI, Python interpreter
version, numpy ABI, libsvm API).

Phase 4b.9 of the VMAFX modernization plan extended the container-first rule
(ADR-0496) to artifact publishing: the container is not merely the preferred
_development_ environment but the sole authorized source for any published
binary, container image, or downstream-consumed artifact. This decision was
reached verbally and was not previously documented.

Without explicit documentation, agents and contributors cannot reliably
distinguish a "canonical" build from a "diagnostic" build, which risks:

- Publishing host-built binaries with unreproducible toolchain state.
- CI gates using a different environment than local verification, making
  container-local passes unreliable predictors of CI outcomes.
- Contributors spending time chasing host toolchain drift when the container
  already has the pinned environment.

## Decision

We will treat the `vmaf-dev-mcp` container as the **exclusive source** for all
canonical artifacts (release binaries, published container images, CI
benchmark/snapshot artifacts used downstream). Host-side builds remain
available for IDE integration, debugger sessions, and sanitizer sweeps, but
must not produce published artifacts.

The policy is documented in `docs/development/publishing.md`, which defines:

- What qualifies as a canonical artifact.
- Container rebuild trigger conditions.
- Approved use cases for host-side builds.
- CI integration (release.yml and cross-backend.yml run inside the container
  image, making local container builds a reliable CI predictor).

CLAUDE.md §15 already encodes the "default to container" rule (ADR-0496);
this ADR extends it to cover the publishing surface and adds the `publishing.md`
reference document.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Status quo (no explicit publishing policy) | No new rule to maintain | Agents and contributors cannot distinguish canonical from diagnostic builds; host-side drift silently enters release artifacts | Risk of unpinned toolchain creeping into releases |
| Container-only for release builds, host-side allowed for CI artifacts | Reduces required container rebuilds in CI | CI artifacts produced from an unpinned environment become difficult to reproduce | The consistency argument applies equally to CI artifacts |
| Nix/Guix instead of Docker for reproducibility | Cryptographically reproducible builds | Adds a new toolchain (Nix) on top of the existing Docker stack; no existing GPU passthrough story for Nix on this repo | Strictly more moving pieces; Docker is already first-class on all CI runners |

## Consequences

- **Positive**: local container builds are reliable CI predictors — a pass in
  the container means CI will pass.
- **Positive**: release artifacts are traceable to the Containerfile layer set
  and therefore to the exact SDK versions used.
- **Positive**: contributors no longer need to diagnose host toolchain drift
  before filing a build-failure issue — the container is the reference.
- **Negative**: contributors who want to produce a "publishable" local build
  must have Docker and (for GPU artifacts) the NVIDIA Container Toolkit
  installed.
- **Neutral**: the Containerfile becomes a hard dependency for the release
  pipeline. Drift in the Containerfile that breaks a backend must be treated
  as a release blocker.

## References

- ADR-0496 — default-to-container project rule.
- ADR-0451 — initial dev-MCP container design.
- ADR-0698 — production Dockerfile design.
- `docs/development/publishing.md` — human-readable policy.
- `docs/development/dev-mcp.md` — container operator guide.
- Source: req (user direction: Phase 4b.9 — container-only canonical artifact policy decided verbally, needs documentation).
