<!-- markdownlint-disable MD013 MD060 -->

# ADR-1186: Publish the dev container's build stage to GHCR so the release path can build inside it

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: ci, build, supply-chain, release

## Context

[ADR-1102](1102-phase4b9-container-only-publishing.md) and CLAUDE.md rule 15 require every
canonical artifact — release binaries, published images, CI artifacts consumed downstream
— to be produced inside the `vmaf-dev-mcp` container. The release path does the opposite:
`build-artifacts` in `.github/workflows/supply-chain.yml` runs `apt-get install
ninja-build nasm`, `pip3 install meson`, and then `meson setup build core` directly on a
bare `ubuntu-latest` runner.

[PR #1269](https://github.com/VMAFx/vmafx/pull/1269) added the enforcement mechanism
(`scripts/ci/check-container-build.sh`, which fails closed and can stamp an artifact tree
with a provenance receipt a host build cannot produce) but deliberately did not wire it
into `supply-chain.yml`, because doing so would fail every release. Its commit message
names the blocking reason exactly: `dev/Containerfile` is never pushed to a registry —
`dev-container-build.yml` builds it as a PR gate and pushes no image — so a release job
has no image to pull. The follow-up was filed as
`T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` in [state.md](../state.md).

The ordering is forced rather than merely convenient. `supply-chain.yml` triggers only on
`release: published` and on a `workflow_dispatch` naming an existing tag, so a change that
containerises `build-artifacts` cannot be exercised by any pull request: the first time it
runs is the first real release. Publishing the image is therefore a prerequisite, and it
has to land — and be observed working on `master` — before the release path can be pointed
at it.

## Decision

We will publish the `libvmaf-build` stage of `dev/Containerfile` to
`ghcr.io/vmafx/vmafx-dev-builder` from `dev-container-build.yml`, on pushes to `master`
only, tagged `:master` and `:sha-<short-sha>`. The package stays **private**: the only
consumer is a GitHub Actions job in this repository, which authenticates with the
workflow's own `GITHUB_TOKEN`, so no public distribution surface is created. Pull requests
keep the existing behaviour unchanged — they build the image, smoke-test it, and push
nothing.

We publish the `libvmaf-build` stage rather than the full `dev-mcp` image because that
stage is precisely the build toolchain (CUDA, oneAPI, ROCm, meson/ninja/nasm) that
`build-artifacts` needs, and it omits the FFmpeg and MCP-server stages the release build
never uses. The PR gate already builds exactly this target for cost reasons, so the
published image is the artifact CI already verifies on every container-touching PR rather
than a second, unverified build.

Pointing `supply-chain.yml`'s `build-artifacts` at the published image is a **separate,
subsequent change**, tracked by the same state.md row, which stays open until it lands.
This ADR covers only making the image exist.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Publish the `libvmaf-build` stage to GHCR on master pushes (**chosen**) | Unblocks the release path; reuses the image the PR gate already builds and smoke-tests; private package, so no public distribution surface; cheap (the stage is already built in CI) | Adds a push step and `packages: write` to one job; the registry becomes a release-path dependency | — |
| Publish the full `dev-mcp` image instead | One image for development and release; matches CLAUDE.md rule 15's wording most literally | The full multi-stage build (FFmpeg + MCP) is far slower than the 45-min ceiling the PR gate was scoped to, and the release build uses none of it | Cost with no benefit to the consumer; the extra stages are dead weight in a release job |
| Build the container inside `supply-chain.yml` itself, no registry | No registry dependency; nothing is published | Adds a 45+ min container build to the critical path of every release, and rebuilds an image CI already built on master — while providing no way to test the containerised release path before a real release | Makes releases slow and still untestable |
| Leave `build-artifacts` native and relax ADR-1102 | Zero work | Abandons the policy that release binaries are reproducible and container-built; the enforcement script from #1269 becomes decorative | The policy is the point; #1269 built the mechanism specifically to be used |
| Push from pull requests as well | The image would exist earlier | A PR from any contributor could overwrite the image a release consumes — a supply-chain hole | Unacceptable: publication must follow review, not precede it |

## Consequences

- **Positive**: the blocker named in #1269 is removed, so `build-artifacts` can be
  containerised in a follow-up and `check-container-build.sh` can finally gate the release
  path it was written for. The image consumed by a release is the one CI smoke-tested.
- **Negative**: `master` pushes that touch the container inputs get slower by the push
  step, and the release path gains a dependency on GHCR being reachable. A release cut
  before the first successful master publish would find no image — the follow-up change
  must therefore verify the tag exists rather than assuming it.
- **Neutral / follow-ups**: (1) point `build-artifacts` at
  `ghcr.io/vmafx/vmafx-dev-builder:master` and wire in `check-container-build.sh --stamp`;
  (2) `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` stays **open** until that
  lands, since this change alone does not stop a host-built release binary.

## Supply-chain impact

- **New dependencies**: none. No package is added to `dev/Containerfile`.
- **Build-time fetches**: unchanged. The published image is built from the same
  `dev/Containerfile` the PR gate already builds.
- **Sigstore-signable**: not yet. This image is build *infrastructure*, not a published
  artifact, and is not signed here. Published release bytes keep their existing trust
  anchor — cosign keyless signing plus SLSA provenance in `supply-chain.yml`. When
  `build-artifacts` moves into this image, the container-build receipt from
  `check-container-build.sh` becomes an additional, non-cryptographic accident gate, as
  [ADR-1102](1102-phase4b9-container-only-publishing.md) already states.
- **CVE surface delta**: neutral for shipped bytes — no new component enters the release.
  The registry package is private, so it adds no public attack surface.

## References

- req: the maintainer's standing direction to converge the fork on a 1.0.0 release; this
  is the blocker [PR #1269](https://github.com/VMAFx/vmafx/pull/1269) named as its own
  follow-up.
- [ADR-1102](1102-phase4b9-container-only-publishing.md) — container-canonical publishing.
- [docs/development/publishing.md](../development/publishing.md) — the enforcement section
  this change serves.
- `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` in [state.md](../state.md).
