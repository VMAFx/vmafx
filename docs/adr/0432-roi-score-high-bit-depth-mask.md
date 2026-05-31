<!-- markdownlint-disable MD060 -->
# ADR-0432: High-Bit-Depth ROI-Score Mask Materialisation

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Codex
- **Tags**: roi, tiny-ai, hdr, tooling, fork-local

## Context

`vmaf-roi-score` already materialises saliency-masked distorted YUV for
8-bit planar inputs, then runs the normal `vmaf` CLI over the masked
file. The public docs still directed higher-bit-depth callers back to
the full-frame VMAF path. That is a poor fit for the current CHUG / HDR
workstream, where 10-bit planar YUV clips are common and ROI scoring
needs the same mask-materialisation path as SDR smoke tests.

The saliency model still consumes 8-bit RGB tensors, but the masked YUV
output should preserve the input sample depth so the subsequent `vmaf`
run receives a valid high-bit-depth raw file.

## Decision

Support little-endian planar 8/10/12/16-bit YUV in the ROI-score mask
materialiser. The implementation keeps each Y/U/V plane in its native
integer sample width while blending, converts only the reference frame
to 8-bit RGB for saliency inference, and passes the matching bit depth
through the `vmaf` subprocess wrapper.

Big-endian high-bit-depth YUV remains unsupported because the rest of
the fork's local YUV/HDR workflows use FFmpeg's little-endian planar
formats (`yuv420p10le`, `yuv422p10le`, `yuv444p10le`, etc.).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep 8-bit-only materialisation | Smallest surface | Blocks ROI-score smoke/use on CHUG/HDR-style 10-bit planar sources | Rejected because the documented limitation is now an active local workflow blocker |
| Down-convert high-bit-depth YUV to 8-bit output | Simple blending math | The masked file no longer matches the caller's requested `--bitdepth`, and the VMAF invocation would score the wrong sample geometry | Rejected because it corrupts the scoring input contract |
| Support both little-endian and big-endian high-bit-depth YUV | Most complete pix_fmt coverage | Adds untested byte-order paths that are not used by the current FFmpeg/libvmaf workflows | Deferred until a real big-endian source appears |

## Consequences

- **Positive**: `vmaf-roi-score --saliency-model` and `--synthetic-mask`
  now work on common HDR planar pix_fmts such as `yuv420p10le`.
- **Positive**: high-bit-depth mask output preserves native sample
  values and remains compatible with the existing `vmaf` CLI bit-depth
  argument.
- **Negative**: big-endian high-bit-depth raw YUV is still rejected.
- **Neutral / follow-ups**: true Option A per-pixel feature pooling
  remains out of scope for this Python Option C tool.

## References

- [ADR-0296](0296-vmaf-roi-saliency-weighted.md)
- [ADR-0425](0425-vmaf-roi-score-saliency-materialiser.md)
- Source: `req` — "and chug?"
- Source: `req` — "and we need new backlogs/open/scaffolds/stubs/docs for pr's"
