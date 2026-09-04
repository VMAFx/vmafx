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
2. The container-only gate detector `scripts/ci/check-container-build.sh` was implemented to assert containerness via `/etc/vmafx-dev-container`, but could not be wired into `supply-chain.yml` because no published dev container image was available on GitHub Container Registry (GHCR) for the release workflow to pull.
3. As a result, release binaries (`libvmaf.so` SONAME chain, `vmaf` CLI binary, `models.tar.gz`) were built in an unpinned runner environment with zero provenance verifying container origin.

## Decision

We resolve `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` by establishing an automated container publication pipeline and enforcing container-built provenance across the entire release path:

1. **Automated GHCR Publication**:
   Create `.github/workflows/dev-container-publish.yml` triggered on pushes to `master` affecting `dev/Containerfile` or `dev/scripts/**`, as well as manual `workflow_dispatch`. The workflow builds the `libvmaf-build` target of `dev/Containerfile`, pushes tags `ghcr.io/vmafx/vmafx-dev-mcp:sha-<commit>` and `:master`, and signs the pushed image using Cosign keyless OIDC via Sigstore.

2. **Containerised Release Compilation**:
   Update `build-artifacts` in `.github/workflows/supply-chain.yml` to run inside `container: ghcr.io/vmafx/vmafx-dev-mcp@<digest>`. Drop host-level package installation (`apt-get`/`pip3`), keeping the Meson configure line unchanged.

3. **Container Provenance Stamping & Verification**:
   - In `build-artifacts`, invoke `scripts/ci/check-container-build.sh --stamp artifacts` after staging, writing `artifacts/container-build-provenance.txt`.
   - In `verify-native-artifacts`, invoke `scripts/ci/check-container-build.sh --verify artifacts` on the downloaded artifact bundle.
   - In `scripts/release/verify-native-release-artifacts.sh`, fail closed if `container-build-provenance.txt` is missing, empty, or a symlink.
   - In `attach-to-release`, require `dist/release-artifacts/container-build-provenance.txt` in `required=()`, ensuring it is cosign-signed and attached as an official release asset.

4. **Dependency Management**:
   Add a package rule to `renovate.json` for `ghcr.io/vmafx/vmafx-dev-mcp` with `pinDigests: true` and `automerge: false` to ensure updates to the dev container image digest are pinned and require maintainer review.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(a) Pre-published GHCR dev container with digest pin (Chosen)** | Clean separation of concerns; fast release runs (2-4 min download, ~45s build); cryptographically signed image; verifiable container provenance. | Runner downloads ~10-11 GB compressed image; requires GHCR storage and publish workflow. | Selected approach. Aligns release workflows with the single canonical development environment mandated by ADR-0496 and ADR-1102. |
| **(b) On-the-fly container build in `supply-chain.yml`** | Self-contained without external registry dependencies; no need to publish dev images to GHCR. | Adds 35-45 minutes to every release job; duplicates image build work; prone to network timeouts on SDK downloads during release. | Unacceptable release latency and failure risk. |
| **(c) Separate minimal release container image** | Smaller image download size (~1 GB vs ~10 GB); faster runner download time. | Creates a second container definition that will inevitably drift from `dev/Containerfile`; fragments toolchain maintenance. | Violates the core principle of ADR-1102 that canonical artifacts originate from the single dev container. |

## Consequences

- **Positive**:
  - Eliminates toolchain drift between local container development and production release binaries.
  - Releases are built using pinned `gcc-15`, `meson`, and SIMD/AVX-512 flags inside the reproducible container environment.
  - Published release bundles include `container-build-provenance.txt` signed with Cosign, providing auditable proof of container origin.
  - Completely satisfies Phase 4b.9 policy ([ADR-1102](1102-phase4b9-container-only-publishing.md)) and closes `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03`.

- **Negative**:
  - GitHub runner pulls ~10-11 GB compressed layer data, unpacking to ~45 GB uncompressed virtual space.
  - `dev-container-publish.yml` takes up to 45-60 minutes to complete on cold runners when container definitions change.

- **Neutral / follow-ups**:
  - Renovate will submit PRs to update the image digest in `supply-chain.yml` when new images are published to GHCR.
  - Runner disk space usage must be monitored if additional large GPU SDKs are introduced.

## References

- [ADR-1102: Container-only canonical artifact publishing (Phase 4b.9)](1102-phase4b9-container-only-publishing.md)
- [ADR-0496: Prefer dev-mcp container rule](0496-prefer-dev-mcp-container-rule.md)
- [docs/development/publishing.md](../development/publishing.md)
- [docs/research/1178-dev-container-image-publish.md](../research/1178-dev-container-image-publish.md)
- Source: Backlog item `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03`
