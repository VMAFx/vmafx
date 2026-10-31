<!-- markdownlint-disable MD013 MD060 -->
# ADR-3386: Saliency ROI channels follow measured encoder options

- **Status**: Accepted
- **Date**: 2026-10-28
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `saliency`, `codec-adapter`, `roi`, `fork-local`
- **Supersedes**: the SVT-AV1 and libvvenc parts of [ADR-0414](0414-saliency-roi-x265-svtav1-vvenc.md).

## Context

Measurements on 2026-10-28 in the development container used FFmpeg `n9.0.2-20-g60e3642` with the VMAFx patch series. VMAFx is the fork of Netflix's Video Multi-Method Assessment Fusion (VMAF) project. With `-c:v libsvtav1 -qpfile qp.txt`, patch `0007-libvmaf-tune-qpfile-unified.patch` loaded an x264-style 16x16 macroblock file, reported an 8x4 qpfile grid becoming a 2x1 superblock ROI grid, and enabled its ROI bridge. In contrast, `-svtav1-params qp-file=...` produced an option-parsing error while FFmpeg exited zero and silently dropped the map. Stock FFmpeg 9.0.2 with SVT-AV1 4.2.0 behaved the same way: the library's `enc_settings.c` has no `qp-file` key.

With `-c:v libvvenc -vvenc-params ROIFile=roi.csv`, the wrapper rejected the unknown `ROIFile` option, encoder opening failed, and FFmpeg returned 234. Neither FFmpeg nor the current patch series carries a saliency ROI map to libvvenc. Passing an unverified key therefore either loses ROI silently or aborts encoding. This conflicts with explicit error handling and observable behaviour in [the engineering principles](../principles.md).

## Decision

Use the existing patched `-qpfile` channel for libsvtav1 in both Go and Python: emit the x264-style frame headers and 16x16 macroblock offsets with suffix `.svtav1-qpfile.txt`; patch 0007 reduces them to 64x64 superblocks and quantises them to at most eight segments. Stock FFmpeg refuses `-qpfile`, making a missing bridge visible as an encode failure. Refuse libvvenc saliency through the existing [ADR-0546](0546-audit-bundle-vulkan-saliency-modelcard.md) unsupported-encoder path unless the caller explicitly accepts a plain encode. Retain the exported legacy writers, argv helpers and constants for append-only API stability (HISS-14), mark them unused by encode paths, and stop counting their legacy keys as applied ROI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `-svtav1-params qp-file=...` | No code changes | Measured parsing failure with zero FFmpeg exit status loses the map | Cannot establish that ROI was applied |
| Pass an SVT superblock map through a new patch | Could retain the old sidecar layout | Adds a second format and bridge requiring new validation | Existing patch 0007 already consumes the shared qpfile format |
| Add a libvvenc ROI bridge in a new patch hunk | Could enable VVC saliency | No measured map channel exists; needs an encoder API design and end-to-end validation | Follow-up work, outside this repair |
| Refuse libvvenc saliency | Makes the missing capability explicit and reuses ADR-0546 | Users must choose another encoder or explicitly accept a plain encode | Chosen until a measured channel exists |

## Consequences

- **Positive**: Go and Python deliver SVT-AV1 maps through the measured bridge instead of an ignored library key. VVenC no longer advertises an unavailable ROI channel.
- **Negative**: SVT-AV1 saliency requires patched FFmpeg and its ROI-capable SVT-AV1 ABI. Spatial detail below 64x64 is averaged and offsets use at most eight segments. VVenC saliency is refused by default.
- **Neutral / follow-ups**: Legacy APIs remain callable for compatibility but no encode path uses them. Tests cover qpfile argv, body, macroblock granularity, cleanup, libvvenc refusal and applied-ROI reporting. Any future VVenC bridge needs its own measured validation and documentation. P4-7 and P4-12 remain open.

## References

- Issue [#2477](https://github.com/VMAFx/vmafx/issues/2477), row P4-13.
- Development-container measurements, 2026-10-28: FFmpeg `n9.0.2-20-g60e3642` with the VMAFx patch series; stock comparison FFmpeg 9.0.2 with SVT-AV1 4.2.0. Observations and return codes are recorded in Context above.
- [Patch 0007](../../ffmpeg-patches/0007-libvmaf-tune-qpfile-unified.patch): shared qpfile parser and SVT-AV1 ROI bridge.
- [ADR-0414](0414-saliency-roi-x265-svtav1-vvenc.md): earlier saliency channels, superseded here for SVT-AV1 and libvvenc.
- Source: `req` — repair the Go and Python saliency channels to match the measured encoder options while retaining exported APIs.
