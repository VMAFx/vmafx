<!-- markdownlint-disable MD013 MD060 -->
# ADR-1350: Include the FFmpeg patch series in the image recovery recipe

- **Status**: Accepted
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: release, ci, container, ffmpeg

## Context

[ADR-1347](1347-image-recovery-from-default-branch.md) lets a `workflow_dispatch` on the default branch recover a published release's images. The run builds the tag's source and takes only the build recipe from the dispatching commit: `docker/` and `Dockerfile.go-server`.

The v1.0.0-rc.1 `vmafx-node` image then failed differently. Once [ADR-1349](1349-native-arch-node-image-build.md) moved its arm64 half to a native runner, FFmpeg compiled in about four minutes. It then stopped at `docker/Dockerfile.node`'s warning gate, which fails the build when any file the fork patches emits a compiler warning.

aarch64 GCC 14.2 (Debian 13) reported `writing 16 bytes into a region of size 15 [-Wstringop-overflow=]` at `libavcodec/a64multienc.c:136-137`. That file is modified by `ffmpeg-patches/0019`.

- **Why it is a false positive:** the two lines store into `uint8_t[256]` tables in a loop bounded by the table size, so no overflow is possible. The report comes from the vectoriser's 16-byte stores.
- **Reproduction:** cross-compiling FFmpeg n9.0.2 with the full series gives the same two warnings. Native amd64 GCC does not warn.

The fix belongs in patch 0019. But `ffmpeg-patches/` is identical on the tag and on master and is not part of the recovery recipe, so a recovery of rc.1 would still build the tag's patch and fail again.

## Decision

The recovery recipe also includes `ffmpeg-patches/`. Every image job of a recovery run now checks out `docker/ Dockerfile.go-server ffmpeg-patches/` from the dispatching commit onto the tag's source. `ffmpeg-patches/` holds the patch series applied to the third-party FFmpeg that the node image bundles; it is not VMAFx or libvmaf code. All of the tag's own code is still built unchanged, and the `io.vmafx.build-recipe` label records the recipe commit.

Patch 0019 now fills `index1` and `index2` once per palette interval with `memset`, instead of one byte per luma value inside the dither loop. It asserts that the palette values are increasing and within the tables. On the encoder's two real palettes and on 200,000 random increasing palettes, the new code produces tables byte-identical to the old. It is warning-free with GCC 14.2 on aarch64 and amd64. `scripts/ci/ffmpeg_patch_stack.py --refresh` and `--check` replay all 20 patches onto n9.0.2.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `ffmpeg-patches/` to the recovery recipe (chosen) | rc.1 gets an arm64 node image built from the warning-free patch; the gate stays strict | The recovered node image's bundled FFmpeg differs from the tag's patch series by a behaviour-preserving table fill | Maintainer's choice (popup, 2026-09-27); the change is proven equivalent and is recorded by the recipe label |
| Publish rc.1's node image for amd64 only | No recipe change | rc.1 ships without the arm64 node image | Drops a deliverable |
| Exempt this warning from the gate for the rc.1 recovery | No patch change for rc.1 | Weakens the gate that exists to keep patched files warning-free | Rejected in favour of fixing the code |

## Consequences

- **Positive**: the gate stays strict. The fix reaches rc.1 and every later build.
- **Negative**: a recovered image's bundled FFmpeg can differ from the tag's patch series. The recipe label and this ADR record that.
- **Neutral / follow-ups**: `test-docker-publish-source-binding.sh` requires the overlay to be exactly `docker/ Dockerfile.go-server ffmpeg-patches/`.

## References

- popup, 2026-09-27: "Add ffmpeg-patches/ to overlay (Recommended)" (maintainer answer to how rc.1's node image gets the patch fix).
- [ADR-1347](1347-image-recovery-from-default-branch.md), [ADR-1349](1349-native-arch-node-image-build.md).
- Run 36348882318, job "Build vmafx-node (arm64)": `FATAL: a file this fork patches emitted compiler warnings`.
