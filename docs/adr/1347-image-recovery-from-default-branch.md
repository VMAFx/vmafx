<!-- markdownlint-disable MD013 MD060 -->
# ADR-1347: Recover a release's container images with the default branch's build recipe

- **Status**: Accepted
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: release, ci, container, supply-chain

## Context

`docker-publish-production.yml` and `docker-publish-operator-node.yml` publish container images for a published release. Their `validate-release` job required the run to execute at the release tag with the tag's own workflow file (`GITHUB_REF == refs/tags/<tag>`, workflow source SHA == tag commit). The documented `workflow_dispatch` recovery therefore could only re-run the same workflow and Dockerfiles that had already failed.

For v1.0.0-rc.1 the tag's recipe had defects: the ROCm SBOM ran out of runner disk, the operator job's 30-minute limit was too short, and the CPU and MCP images compiled libvmaf's whole unit-test suite under QEMU until they hit their 60-minute limit. #1574 fixed all of them on master, but none of the fixes could reach the tag, so four images could not be published for rc.1 at all. The dispatch path also read the prerelease flag from the release event, which a dispatch does not carry, so it rejected every release candidate.

## Decision

`validate-release` accepts one additional mode. A `workflow_dispatch` on the default branch (`refs/heads/<default_branch>`) for a published tag is a recovery run. It still checks out the tag, runs `verify-release-version.sh` on it, and requires a published release whose `prerelease` flag (read from the API) matches the tag. It exports `recovery=true` and `recipe-sha` (the dispatching commit). Each image job then overlays only the build recipe, `docker/` and `Dockerfile.go-server`, from `recipe-sha` onto the tag's source, and every image carries the label `io.vmafx.build-recipe=<recipe-sha>`. A run at the tag keeps the previous rule: the workflow source must be the tag's commit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Recovery dispatch builds the tag's source with the default branch's recipe (chosen) | Every rc.1 image ships under v1.0.0-rc.1; the tag, PyPI package and signed native assets stay untouched; the recipe commit is recorded on each image | Workflow and Dockerfile for a recovered image come from a later commit than the tag | Only the build recipe moves; for rc.1 the recipe diff is three `-Denable_tests=false` lines, so the compiled code is exactly the tag's |
| Cut 1.0.0-rc.2 for the missing images | No rule change | rc.1 stays incomplete | The maintainer wants every image on rc.1 |
| Move the v1.0.0-rc.1 tag to the fixed commit | Plain tag build | Breaks the immutable-candidate rule; the published PyPI 1.0.0rc1 and signed native assets would no longer match the tag | Unsafe |
| Re-run the tag's own workflow | No rule change | Repeats the same disk and timeout failures | Cannot succeed |

## Consequences

- **Positive**: a release's images can be recovered after a recipe fix without a new tag; the dispatch path works for release candidates.
- **Negative**: a recovered image's recipe is not the tag's; the `io.vmafx.build-recipe` label and the run's provenance name the recipe commit.
- **Neutral / follow-ups**: `scripts/release/tests/test-docker-publish-source-binding.sh` pins the recovery gate, the tag-mode source check and the recipe-only overlay.

## References

- req, 2026-09-27: "i want the images, all of them and properly on rc1".
- req, 2026-09-27: "I want everything on rc1".
- [ADR-1201](1201-release-candidates-before-1-0-0.md) — release candidates.
- [ADR-1346](1346-hosted-slim-container-release-build.md) — hosted release build.
