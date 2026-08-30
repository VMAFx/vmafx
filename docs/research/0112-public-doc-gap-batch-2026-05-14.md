# Research-0112 — Public Doc Gap Batch

| Field | Value |
| --- | --- |
| Date | 2026-05-14 |
| Status | Implementation digest |
| Tags | docs, saliency, ssimulacra2, vmaf-tune |

## Scan

The targeted scan was:

```bash
rg -n -i '\(stub\)|\bstub\b|\blimitations\b|\bdeferred\b' \
  docs/usage docs/metrics docs/ai docs/backends docs/mcp docs/development --glob '*.md'
```

Open PR heads were checked first to avoid duplicating in-flight MCP, DISTS,
CUDA, and vmaf-tune work. Two independent but small public-doc gaps were
batchable:

- `docs/metrics/ssimulacra2.md` still said "stub" despite the extractor,
  SIMD paths, GPU twins, and snapshot gate being shipped.
- `docs/usage/vmaf-tune.md` still listed luma-replicated saliency input as a
  deferred limitation.

## Implementation

The saliency path now reads Y, U, and V from yuv420p, nearest-neighbour
upsamples chroma, applies BT.709 limited-range YUV-to-RGB conversion, clips to
`[0, 1]`, and applies the existing ImageNet normalisation before ONNX
inference. A unit test feeds non-neutral chroma and asserts that the ONNX input
channels diverge, proving the path is chroma-aware.

The SSIMULACRA2 page is now a direct operator reference: feature name, output
range, invocation, options, formats, CPU/GPU backends, limitations, and links
to the governing ADRs.

## References

- [ADR-0430](../adr/0430-saliency-rgb-ingest-and-ssimulacra2-docs.md)
- `req`: user suggested scanning human-facing docs for `stub`, `limitations`,
  and `deferred`, and batching related gaps.
