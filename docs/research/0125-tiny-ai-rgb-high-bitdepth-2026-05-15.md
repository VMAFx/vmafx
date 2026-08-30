<!-- markdownlint-disable MD060 -->
# Research 0125: Tiny-AI RGB Extractors High-Bit-Depth Input

Date: 2026-05-15

## Question

LPIPS and DISTS-Sq both consume ImageNet-normalised RGB tensors, but their
host extractors rejected raw YUV inputs above 8 bpc. The question was whether
10/12/16-bit planar YUV could be accepted without changing either ONNX graph
ABI.

## Findings

- The shipped LPIPS and DISTS-Sq graphs already consume RGB tensors, not raw
  YUV. Bit depth is a host-side decode / normalisation concern.
- The existing BT.709 limited-range RGB conversion expects 8-bit nominal
  values. High-bit-depth raw YUV can be rounded into that same domain before
  the existing conversion, preserving model inputs and score key names.
- DISTS-Sq intentionally remains a smoke checkpoint. This change only removes
  the host input-format limitation; it does not promote DISTS-Sq to production
  DISTS weights.

## Alternatives Considered

| Option | Result | Rationale |
|--------|--------|-----------|
| Keep LPIPS / DISTS-Sq 8-bit only | Rejected | HDR / high-bit-depth corpus work would need pre-conversion outside libvmaf even though the model ABI can remain unchanged. |
| Add new high-bit-depth ONNX inputs | Rejected | It would fork the model ABI and require new model cards / exporters without improving the existing RGB checkpoint semantics. |
| Normalise high-bit-depth YUV into RGB8 before ImageNet normalisation | Accepted | It removes the input limitation while preserving graph ABI, output keys, and existing 8-bit behaviour. |

## Decision

Add `vmaf_tiny_ai_yuv_to_rgb8_planes()` beside the existing 8-bit helper.
The new helper preserves the 8-bit path and handles 10/12/16-bit planar YUV
as little-endian 16-bit sample containers rounded into the 8-bit domain. Wire
LPIPS and DISTS-Sq through the new helper and keep both ONNX tensor contracts
unchanged.

## Validation

`test_dists` now checks that a small 10-bit YUV420P fixture normalises to the
same RGB planes as an equivalent 8-bit fixture. Existing LPIPS / DISTS
registration tests continue to cover model-path and feature registration
contracts.
