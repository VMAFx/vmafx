<!-- markdownlint-disable MD013 MD060 -->
# ADR-2166: Redesign FFmpeg patch series from 0001, drop Vulkan shims, and retire legacy libvmaf filter modifications

- **Status**: Accepted
- **Date**: 2026-10-31
- **Deciders**: maintainer (Issue #2435, Research-2166)
- **Supersedes**: [ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md)
- **Tags**: ffmpeg, patches, vmafx, vulkan, breaking-change

## Context

[ADR-0726](0726-drop-vulkan-backend.md) removed the Vulkan compute backend from the
VMAFx fork. However, [ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md) temporarily
re-included patches `0004` (`vulkan_device` selector) and `0006` (`libvmaf_vulkan` filter)
in `ffmpeg-patches/series.txt` as no-op shims to preserve context-line continuity across
downstream patches (`0005`, `0008`–`0015`).

Over time, this legacy series accrued significant maintenance overhead:

1. `vf_libvmaf.c` grew to over 2400 lines through repeated fork-specific option and backend additions.
2. Context lines referenced dead Vulkan shims and configure probes for non-existent headers.
3. [ADR-2125](2125-vmafx-ffmpeg-gstreamer-filters.md) introduced the unified `vmafx`, `vmafx_tune`,
   and `vmafx_pre` filters on the modern VMAFx C API (`--enable-libvmafx`), rendering all modifications
   to `vf_libvmaf.c` obsolete.
4. Upstream FFmpeg unpatched builds already maintain their own stock `libvmaf` filter linking against
   the `libvmaf.so.3` backwards-compatibility library.

Issue #2435 mandates eliminating all fork modifications to `vf_libvmaf.c`, removing every `vmaf`-named
filter and option added by the fork, and restructuring the patch stack into a clean, consecutive series
beginning at `0001`.

## Decision

1. **Drop Vulkan shims and retire all `vf_libvmaf.c` modifications**:
   Permanently remove `0004` and `0006` (the Vulkan shims) and all 17 other obsolete patches that modified
   `vf_libvmaf.c` or added legacy `vmaf`-named filters (`libvmaf_sycl`, `libvmaf_vulkan`, `libvmaf_metal`,
   `libvmaf_tune`, `vmaf_pre`, `-vmaf-profile`). ADR-2166 supersedes ADR-0860.

2. **Renumber and rebuild the series from `0001` grouped by purpose with no numbering gaps**:
   The active patch series in `ffmpeg-patches/series.txt` consists solely of:
   - `0001-codec-qpfile-unified.patch` (formerly `0007`): Generic per-frame `-qpfile` option across libx264, libsvtav1, and libaomenc. Renamed to remove `libvmaf-tune` prefix.
   - `0002-pass-autotune-cli-glue.patch` (formerly `0009`): Generic `-pass-autotune` CLI glue.
   - `0003-add-vmafx-profile-cli-glue.patch` (formerly `0025`): `-vmafx-profile` CLI helper.
   - `0004-add-vmafx-filter.patch` (formerly `0023`): `vmafx` scoring filter and `vmafx_tune` CRF recommendation filter on `libvmafx`.
   - `0005-add-vmafx-pre-filter.patch` (formerly `0024`): `vmafx_pre` learned filter on `libvmafx`.
   - `0006-fftools-loopback-decoder-hwaccel.patch` (formerly `0026`): Hardware-accelerated decoding preceding `-dec` loopback decoders (retained in fork per ADR-2125 decision Q-048).

3. **Port colorimetry passing to `vf_vmafx.c`**:
   The AVFrame colorimetry mapping capability from legacy patch `0022` (ADR-2093) is ported directly to
   `ffmpeg-patches/src/vf_vmafx.c`, declaring color range, primaries, transfer characteristics, and matrix
   to the VMAFx engine on both inputs.

4. **Fork FFmpeg builds configure solely `--enable-libvmafx`**:
   All fork container images, dev containers, CI workflows, and documentation configure `--enable-libvmafx`.
   All backend-specific configure flags (`--enable-libvmaf-*`) are removed.

5. **Upstream compatibility preserved**:
   Unpatched upstream FFmpeg retains its own stock `libvmaf` filter (`--enable-libvmaf`) linking against the
   `libvmaf.so.3` backwards-compatibility library.

## Consequences

- **Positive**:
  - Zero dead code or no-op shims; 19 obsolete patches removed.
  - Zero modifications to `vf_libvmaf.c`, eliminating upstream merge conflicts in the core FFmpeg filter.
  - Strict consecutive numbering `0001` to `0006` with no gaps.
  - Single `--enable-libvmafx` configure flag replaces multi-flag backend configurations.
- **Negative / Breaking change**:
  - Legacy filters (`libvmaf_sycl`, `libvmaf_cuda`, `libvmaf_metal`, `libvmaf_tune`, `vmaf_pre`) and CLI options
    (`-vmaf-profile`, `--enable-libvmaf-*`) are removed from fork FFmpeg builds.
  - Users migrate to `vmafx`, `vmafx_tune`, `vmafx_pre`, and `-vmafx-profile` following the migration guide in
    `docs/usage/ffmpeg.md`.

## References

- ADR-0726 (drop Vulkan backend)
- ADR-0860 (superseded: no-op Vulkan shims)
- ADR-1852 (VMAFx API redesign)
- ADR-2093 (AVFrame colorimetry forwarding)
- ADR-2125 (VMAFx FFmpeg filters)
- Research-2166 (FFmpeg patch series measured audit)
- Issue #2435 (FFmpeg patch series redesign)
- `docs/usage/ffmpeg.md` (Migration guide)
