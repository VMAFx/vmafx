<!-- markdownlint-disable MD013 MD060 -->
# ADR-1178: Dev container image publication and release artifact container enforcement

- **Status**: Accepted
- **Date**: 2026-09-04
- **Deciders**: lusoris
- **Tags**: ci, release, supply-chain, container, dev-container, adr-1102, fork-local

## Context

[ADR-1102](1102-phase4b9-container-only-publishing.md) codified the project's Phase 4b.9 policy: all canonical build artifacts (release binaries, published images, and downstream CI artifacts) must be built inside the canonical `vmaf-dev-mcp` container (`dev/Containerfile`). The dev container pins compilers, GPU toolchains, Python libraries, and system dependencies, guaranteeing reproducible outputs.

However, as documented in backlog issue `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` and `docs/development/publishing.md`, the native release path remained uncontainerised:

1. `.github/workflows/supply-chain.yml`'s `build-artifacts` job ran directly on bare `ubuntu-latest`, installing toolchain dependencies dynamically via `apt-get` and `pip3 install meson`.
2. The container-only gate detector `scripts/ci/check-container-build.sh` was implemented to assert containerness via `/etc/vmafx-dev-container`, but could not be wired into `supply-chain.yml` on hosted runners: physical measurement on the workstation showed the `libvmaf-build` stage sums to ~29.5 GB of uncompressed layers (`dev-mcp` sums to ~37.6 GB), whereas GitHub-hosted `ubuntu-latest` runners for public repositories provide only 14 GB SSD. A job-level `container:` pull executes before any step runs, making pre-step disk cleaning ineffective and creating an insurmountable disk-exhaustion blocker on hosted runners.
3. Concurrently, [ADR-1177](1177-sycl-arc-self-hosted-runner.md) (PR #1304) provisioned a containerised self-hosted runner for Intel Arc A380 SYCL CI (`vmaf-sycl-arc-runner:local`, built `FROM vmaf-dev-mcp:local`), running on the maintainer's workstation with runner labels `[self-hosted, linux, x64, sycl-arc]` and operator runbook [`docs/development/ci-self-hosted-sycl.md`](../development/ci-self-hosted-sycl.md).

## Decision

We resolve `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` by building native release artifacts on the Arc A380 self-hosted canonical runner using the local dev container environment, while retaining container publication for optional provenance:

1. **Self-Hosted Canonical Runner Release Compilation**:
   Update `build-artifacts` in `.github/workflows/supply-chain.yml` to run on `runs-on: [self-hosted, linux, x64, sycl-arc]`, without a `container:` pull key.
   - **Local Environment**: The runner container itself (`vmaf-sycl-arc-runner:local`, derived `FROM vmaf-dev-mcp:local`) IS the build environment. It already contains the complete canonical toolchain (GCC-13, Clang-19, Meson, Ninja, Python 3.14, oneAPI, CUDA 13.3, ROCm 7.2.4) and the marker `/etc/vmafx-dev-container`. No Docker socket is needed or mounted.
   - **Fork-PR Guard**: Protected with `if: github.repository == 'VMAFx/vmafx' && startsWith(github.ref, 'refs/tags/v')` so that only official tag releases on the canonical repository execute on the self-hosted runner.
   - **Concurrency & Limits**: Pinned to `concurrency: group: release-artifacts-build, cancel-in-progress: false` to ensure exactly one release build executes at a time on the single runner, with a generous `timeout-minutes: 90`.
   - **Permissions**: Kept at least-privilege `permissions: contents: read`. Build artifacts (`libvmaf.so` SONAME chain, `vmaf` CLI binary, `models.tar.gz`, `u2netp_mirror` if present) are uploaded as workflow artifacts and attached to the GitHub release by the existing protected, environment-gated `attach-to-release` job on `ubuntu-latest`.

2. **Container Provenance Stamping & Verification**:
   - In `build-artifacts`, invoke `scripts/ci/check-container-build.sh --stamp artifacts` after staging, writing `artifacts/container-build-provenance.txt`.
   - `scripts/ci/check-container-build.sh` accepts both `vmaf-dev-mcp` (local dev container) and `vmaf-sycl-arc-runner` (self-hosted canonical runner) as valid canonical environments, while continuing to strictly reject bare `ubuntu-latest` (where `/etc/vmafx-dev-container` is absent) and foreign image titles.
   - In `verify-native-artifacts` (running on `ubuntu-latest`), invoke `scripts/ci/check-container-build.sh --verify artifacts` on the downloaded artifact bundle.
   - In `scripts/release/verify-native-release-artifacts.sh`, fail closed if `container-build-provenance.txt` is missing, empty, or a symlink.
   - In `attach-to-release`, require `dist/release-artifacts/container-build-provenance.txt` in `required=()`, ensuring it is cosign-signed and attached as an official release asset.

3. **Optional GHCR Provenance Publication**:
   Retain `.github/workflows/dev-container-publish.yml` triggered on master pushes affecting `dev/Containerfile` or `dev/scripts/**` (and manual `workflow_dispatch`). The workflow builds the `libvmaf-build` stage, pushes tags to `ghcr.io/vmafx/vmafx-dev-mcp`, and signs the image with Cosign keyless OIDC. This provides public supply-chain transparency and container availability for remote contributors, but is completely decoupled from the critical-path release build. No Renovate digest pinning is required in `supply-chain.yml`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(a) Self-hosted Arc A380 canonical runner with local image (Chosen)** | Zero network pull latency during release; builds in identical canonical toolchain; no registry dependency or size blockers; enforces container-build provenance end-to-end. | Release compilation depends on the workstation runner being online; requires operator pause/resume runbook. | **Selected approach.** Directly overcomes the 29.5 GB layer size blocker while upholding ADR-1102 and ADR-0496 without compromising toolchain parity. |
| **(b) GHCR dev container pull on hosted `ubuntu-latest` (`container:`)** | Fully hosted in GitHub Actions; independent of workstation availability. | **Fatal design blocker:** `libvmaf-build` sums to ~29.5 GB uncompressed layers (`docker history`), exceeding the 14 GB SSD limit of standard public `ubuntu-latest` runners. Pre-step disk cleanup cannot help because `container:` is initialized before any step runs. | Impractical on standard GitHub-hosted infrastructure. |
| **(c) Dedicated slim release-only container stage** | Smaller image download (~1-2 GB without GPU SDKs); could pull on hosted runners. | Violates the core principle of ADR-1102 that canonical artifacts originate from the single dev container; creates a second container definition that will drift; fragments toolchain maintenance. | Rejected to preserve a single source of truth for the project toolchain. |
| **(d) On-the-fly container build in `supply-chain.yml`** | Self-contained without pre-published registry dependency. | Adds 35-45 minutes to every release job; duplicates image build work; prone to network timeouts on multi-gigabyte SDK downloads and runner disk exhaustion during BuildKit caching. | Unacceptable release latency and failure risk. |

## Consequences

- **Positive**:
  - Eliminates toolchain drift between local container development and production release binaries.
  - Releases are built using pinned `gcc-13`/`gcc-15`, `meson`, and SIMD/AVX-512 flags inside the reproducible container environment.
  - Published release bundles include `container-build-provenance.txt` signed with Cosign, providing auditable proof of container origin.
  - Completely satisfies Phase 4b.9 policy ([ADR-1102](1102-phase4b9-container-only-publishing.md)) and closes `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03`.
  - Zero registry pull traffic or runner disk bloat during the native release build.

- **Negative / Operational**:
  - Release availability depends on the workstation runner (`vmaf-sycl-arc-runner`) being online.
  - If the runner is stopped or paused, the `build-artifacts` job queues until the maintainer starts the runner container via `docker compose -f dev/docker-compose.runner.yml up -d` (see [`docs/development/ci-self-hosted-sycl.md`](../development/ci-self-hosted-sycl.md)).

- **Neutral / follow-ups**:
  - `dev-container-publish.yml` remains available as optional provenance on GHCR for transparency and remote developer convenience.
  - No Renovate digest pinning is required in `supply-chain.yml`.

## References

- [ADR-1102: Container-only canonical artifact publishing (Phase 4b.9)](1102-phase4b9-container-only-publishing.md)
- [ADR-1177: Containerised self-hosted GitHub Actions runner for Intel Arc A380 SYCL CI](1177-sycl-arc-self-hosted-runner.md)
- [ADR-0496: Prefer dev-mcp container rule](0496-prefer-dev-mcp-container-rule.md)
- [docs/development/publishing.md](../development/publishing.md)
- [docs/development/ci-self-hosted-sycl.md](../development/ci-self-hosted-sycl.md)
- [docs/research/1178-dev-container-image-publish.md](../research/1178-dev-container-image-publish.md)
- Source: Backlog item `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03`
