<!-- markdownlint-disable MD013 MD060 -->
# ADR-1349: Build the vmafx-node image per architecture on native runners

- **Status**: Accepted
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: release, ci, container

## Context

`docker-publish-operator-node.yml` built `ghcr.io/vmafx/vmafx-node` for `linux/amd64` and `linux/arm64` in one job on an amd64 runner, with the arm64 half under QEMU emulation. For arm64, `docker/Dockerfile.node` compiles FFmpeg, libvmaf and the CGO node binary. Under emulation that took longer than the job allowed.

The image was never published for v1.0.0-rc.1:

- **First attempts:** cancelled or failed.
- **Recovery run of 2026-09-27 16:37 UTC:** still inside "Build and push" when it was cancelled at 82 minutes.
- **Next recovery run:** reached the 120-minute limit in the same step.

A job that times out never exports its BuildKit cache, so every retry starts from nothing. The other two images in the workflow are Go-only and finished within their limits (operator 29–35 minutes, server 46 minutes uncached).

## Decision

`build-node` is a matrix job with one leg per architecture on a native runner: amd64 on `ubuntu-latest`, arm64 on `ubuntu-26.04-arm` (the label `libvmaf-build-matrix.yml` already uses). Each leg:

- builds `node-cpu` for its platform only;
- pushes it by digest (`push-by-digest=true`), with its own GitHub Actions cache scope;
- uploads the digest as an artifact.

A new `publish-node` job (display name "Publish vmafx-node CPU", unchanged) does the rest:

- requires exactly two digests;
- creates the tagged multi-arch index with `docker buildx imagetools create`;
- signs the index, attests its build provenance, and generates and attests its SBOM, as the single job did before.

The smoke test and the summary job read the index digest from `publish-node`. Both jobs keep the `release-publish` environment, and `build-node` keeps the tag checkout, the ADR-1347 recipe overlay and the source-revision label.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Native runner per architecture, merge by digest (chosen) | Each half builds at native speed; per-arch cache scopes survive; the published index is still one signed, attested artifact | Two more jobs; the index is assembled from digests rather than in one build | The maintainer chose this (popup, 2026-09-27); the merge was checked locally with the exact script against a registry |
| Raise `timeout-minutes` toward GitHub's 360-minute cap | One-line change | Build time under QEMU is unknown; a second timeout again loses the cache; hours of runner time per release | Does not remove the cause |
| Publish vmafx-node for amd64 only in rc.1 | Fast | rc.1 ships without an arm64 node image the docs promise | Drops a deliverable |

## Consequences

- **Positive**: the arm64 node image builds natively; a failed leg can be re-run alone; each architecture keeps its own cache.
- **Negative**: the run approves three `release-publish` deployments for the node image instead of one (two build legs and the merge).
- **Neutral / follow-ups**: `test-docker-publish-source-binding.sh` requires the native runners, no QEMU in `build-node`, and a two-digest merge that signs the index; `test-publication-environment-binding.sh` verifies the node signature at `publish-node`'s digest. The server and operator images still build under QEMU in one job; move them the same way if they approach their limits.

## References

- popup, 2026-09-27: "Native runner per arch (Recommended)" (maintainer answer to how the vmafx-node image should be built for rc.1).
- [ADR-1347](1347-image-recovery-from-default-branch.md) — recovering a release's images from the default branch.
- Runs: 36333909882 (node build cancelled at 82 minutes), 36340218397 (node build at the 120-minute limit).
