<!-- markdownlint-disable MD060 -->
# Research 0123: `vmaf-roi` High-Bit-Depth Input

Date: 2026-05-15

## Question

`docs/usage/vmaf-roi.md` still documented `--bitdepth` as 8-bit only, while
the adjacent ROI-score path already accepts high-bit-depth planar YUV. The
question was whether `vmaf-roi` could safely accept 10/12/16-bit raw inputs
without changing the saliency model ABI or encoder sidecar schemas.

## Findings

- `vmaf-roi` only consumes the Y plane for saliency; chroma must still be
  counted while seeking to a requested frame.
- The existing DNN helper expects luma8 input, so high-bit-depth support can
  be an input-normalisation step rather than a new model ABI.
- The fork's raw-YUV CLI conventions use little-endian 16-bit containers for
  10/12/16-bit planar inputs, matching the `vmaf` CLI and ROI-score materialiser.
- The x265 and SVT-AV1 sidecar formats are independent of source bit depth;
  they remain per-CTU signed QP offsets.

## Alternatives Considered

| Option | Result | Rationale |
|--------|--------|-----------|
| Keep `vmaf-roi` 8-bit only | Rejected | It leaves HDR / CHUG-style raw inputs blocked at the encoder-side ROI tool even though the score-side ROI materialiser already accepts high-bit-depth inputs. |
| Add a high-bit-depth saliency model ABI | Rejected | No current saliency model consumes luma10/12/16 directly; changing the ABI would create a model-card and runtime migration for no immediate quality gain. |
| Normalise high-bit-depth luma to luma8 before DNN | Accepted | It removes the input-format limitation while preserving the existing DNN and encoder contracts. |

## Decision

Accept `--bitdepth 8|10|12|16` in `vmaf-roi`. Treat 10/12/16-bit planar YUV
as little-endian 16-bit samples, count full YUV frame bytes for seeking, read
Y only, clamp to the declared bit-depth range, and round-shift luma to 8-bit
before the existing saliency model path.

## Validation

The regression test builds a two-frame `yuv420p10le` fixture, verifies that
frame 1 can be read, verifies that frame 2 short-reads with high-bit-depth
frame accounting, and rejects unsupported `--bitdepth 9`.
