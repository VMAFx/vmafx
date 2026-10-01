<!-- markdownlint-disable MD013 MD060 -->
# ADR-1398: CLI accepts odd dimensions for chroma-subsampled raw YUV inputs

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris, user
- **Tags**: `cli`, `validation`, `correctness`, `odd-dimensions`

## Context

`validate_chroma_alignment()` (added in [ADR-0461](0461-cli-validate-dimensions-chroma.md))
rejected odd widths for 4:2:0 / 4:2:2 and odd heights for 4:2:0 under the assumption
that chroma-subsampled formats must require even dimensions to avoid fractional
chroma pixels.

However, the `.y4m` reader (`y4m_input.c`) pads `frame_w` and `frame_h` to a multiple of
16, meaning odd-sized `.y4m` streams bypassed `validate_chroma_alignment()` and were
read and scored correctly. libvmaf picture allocation and feature extractors derive
chroma dimensions using ceiling division (`vmaf_chroma_extent()`, `picture_geometry.h`,
Research-0094, PR #1643, PR #1664), ensuring full coverage of boundary samples.

Consequently, the CLI exhibited an arbitrary disparity: an odd-sized clip (e.g. 19×19
or 1921×1081) was accepted in a `.y4m` container but rejected with
`odd width %d not allowed for chroma-subsampled format` when provided as raw `.yuv`.

## Decision

Per user decision (popup 2026-10-01, "Accept both (Recommended)"), raw `.yuv` input
with odd width or height in 4:2:0 (and odd width in 4:2:2) is accepted and read with
ceil chroma, matching `.y4m`.

1. `validate_chroma_alignment()` in `core/tools/vmaf.cpp` no longer rejects odd
   dimensions. It remains as a non-failing validation helper to preserve the
   validation call-site structure established in ADR-0461.
2. Raw YUV plane geometry in `yuv_input.c` already derives plane buffer sizes using
   ceiling division `((dim + 1) / 2)` and verifies that the file size matches an exact
   multiple of frame bytes (`yuv_check_file_size()`), exiting cleanly with code 2 on
   file size mismatches.
3. This ADR supersedes the odd-dimension rejection policy of ADR-0461. The positive
   dimensions requirement (`validate_video_info()`, rejecting non-positive dimensions)
   from ADR-0461 remains in effect.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reject odd dimensions for both raw and `.y4m` | Strict enforcement of even dimensions | Breaks valid odd-sized video workflows; regresses working `.y4m` capabilities | Rejected — odd-sized streams are valid media |
| Leave disparity as-is (reject raw, accept `.y4m`) | Zero code changes | Arbitrary inconsistency across container types; confuses users | Rejected |
| Accept odd dimensions for both using ceiling chroma | Bit-exact parity between raw YUV and `.y4m`; handles arbitrary media resolutions | Requires testing across boundary sizes | **Accepted (Recommended)** — user decision 2026-10-01 |

## Consequences

- **Positive**: Raw `.yuv` and `.y4m` streams with odd dimensions (e.g. 19×19,
  1921×1081, 1×1) are accepted and score bit-identically across all features.
- **Negative**: None. Files with incorrect byte counts fail cleanly with exit 2 via
  `yuv_check_file_size()`.
- **Neutral / follow-ups**: Supersedes ADR-0461 on odd chroma dimension restrictions.

## References

- [ADR-0461](0461-cli-validate-dimensions-chroma.md) (CLI validates positive dimensions and chroma-alignment)
- Research-0094 (ceiling chroma plane geometry)
- PR #1643 (speed_temporal and speed_chroma buffer sizing for odd dimensions)
- PR #1664 (odd-dimension readback test in `core/test/test_video_input_odd_dims.c`)
- User decision popup 2026-10-01: "Accept both (Recommended)"
