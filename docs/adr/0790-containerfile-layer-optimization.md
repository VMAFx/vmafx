<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0790: Containerfile layer optimization — merge apt layer, strip build artifacts, no-cache-dir pip

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `build`, `docker`, `containerfile`, `fork-local`

## Context

The `dev/Containerfile` multi-stage build produces a ~50 GB final image. Because
all four stages form a linear chain (`build-deps → gpu-sdks → libvmaf-build →
dev-mcp`), every layer in every stage contributes to the final image size.
An audit identified four categories of avoidable bulk:

1. **Redundant apt layer for `clinfo`** (Stage 2, line ~230): a standalone
   `apt-get update && apt-get install clinfo` layer was followed immediately by
   the NEO `.deb` install layer, which also runs `apt-get update`. Two separate
   `apt-get update` calls create two distinct layers with overlapping metadata;
   merging them into one removes a layer and one full apt-list download.

2. **pip wheel cache not suppressed** (Stage 4): the five `pip install` calls
   omitted `--no-cache-dir`, leaving downloaded wheel files in the pip cache
   directory (typically `~/.cache/pip` inside the layer). The cache is never
   used again at runtime and only bloats the image.

3. **FFmpeg source and build objects retained** (Stage 3.5 → dev-mcp): after
   `make install`, the entire `/build/ffmpeg` tree — comprising cloned source,
   configure output, and object files compiled with `-j$(nproc)` for a codec-
   heavy FFmpeg — remained in the image. FFmpeg source plus compiled objects
   for all enabled codecs (x264, x265, libvpx, SVT-AV1, VVenC, nvenc,
   QSV, AMF) is several gigabytes.

4. **libvmaf meson build directory retained** (Stage 3): after `ninja install`,
   the `core/build` directory containing CUDA PTX/SASS intermediates, SYCL
   AOT fat binaries, HIP `.co` objects, and Meson-generated C object files
   remained in the layer. The installed artifacts live under `/usr/local/`; the
   build directory is not needed at runtime.

Per user direction, the fix scope is limited to safe wins only — no changes to
the installed package set, no architecture changes.

## Decision

Apply four targeted changes to `dev/Containerfile`:

1. **Merge the `clinfo` apt layer** into the NEO `.deb` install layer: install
   `clinfo` as part of the first `apt-get` call in that block, then proceed to
   download and install the NEO `.deb` packages. Both calls are in the same
   BuildKit `RUN` block.

2. **Add `--no-cache-dir`** to all five `pip install` invocations in Stage 4
   (`/opt/vmaf-venv/bin/pip install ...`). This prevents pip from writing
   downloaded wheels to `~/.cache/pip` inside the layer.

3. **Remove `/build/ffmpeg`** in the same `RUN` step as `make install`, after
   the install completes and the version probe succeeds. The FFmpeg binaries
   (`/usr/local/bin/ffmpeg`, `/usr/local/bin/ffprobe`) and libraries
   (`/usr/local/lib/libav*.so`) are retained; only the source tree is removed.

4. **Remove `core/build`** (at `/build/vmaf/core/build`) in the same `RUN`
   step as `ninja install`, after the install completes. The installed shared
   library and headers under `/usr/local/` are retained; only the meson build
   directory is removed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| True multi-stage copy — final image copies only `/usr/local/` from a builder stage | Eliminates all build toolchains from final image; largest possible size reduction | Breaks the dev-container contract: `clangd`, `icpx`, `hipcc`, `nvcc` must be available inside the container for interactive dev sessions and MCP tool calls | Contradicts the dev-container purpose |
| Replace `intel-basekit` with component packages | Removes unneeded Intel SDK components; potentially saves 5-15 GB | Fragile — Intel's component package names change across versions; `intel-basekit` is the supported install path; fragmentation would cause breakage on next `apt-get update` | Risk outweighs benefit for this audit pass |
| Strip installed binaries (`strip --strip-unneeded`) | Removes debug symbols from installed binaries | Destroys debug info needed for profiling and crash analysis in the dev container; not appropriate for a dev image | Contradicts dev-container purpose |

## Consequences

- **Positive**: Removes several GB from the final image (FFmpeg build objects
  are typically 2-5 GB; `core/build` with CUDA+SYCL+HIP intermediates adds
  another 1-3 GB; pip cache is typically 200-600 MB depending on transitive
  deps). Merging the apt layer eliminates one redundant metadata download during
  rebuild. No runtime behaviour changes.
- **Negative**: Incremental rebuilds of only Stage 3.5 (FFmpeg) will be slower
  because the source tree is gone; a full re-clone is required. This is
  acceptable because the FFMPEG_TAG build-arg is pinned and rarely changes.
- **Neutral**: The `sycl-ls`/`rocminfo` probe in Stage 2 still runs after the
  merged layer, as before. Layer cache keys are unchanged for the NEO ARG pins.

## References

- User direction: per user request (2026-05-29) to audit for safe size-reduction
  wins without breaking the image.
- `dev/Containerfile` — the patched file.
- ADR-0541: FFmpeg encoder matrix and codec dep rationale.
- ADR-0603: Ubuntu 26.04 / glibc 2.43 base-image policy.
