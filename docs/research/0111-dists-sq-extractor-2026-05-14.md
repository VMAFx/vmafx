# Research-0111 — DISTS-Sq Extractor Implementation Notes

| Field | Value |
| --- | --- |
| Date | 2026-05-14 |
| Status | Implementation digest |
| Tags | dnn, tiny-ai, fr, dists, onnx |

## Context

ADR-0236 designed `dists_sq` as the DISTS companion to the shipped LPIPS
full-reference extractor. The stale docs still marked the surface as a stub:
no C extractor, no model registry entry, no placeholder ONNX, and no smoke
test existed.

## Implementation Shape

The extractor follows the existing LPIPS host pattern instead of introducing a
second preprocessing path:

- Convert 8-bit YUV 4:2:0 / 4:2:2 / 4:4:4 frames to RGB.
- Convert RGB planes to ImageNet-normalised float32 NCHW tensors.
- Bind named ONNX inputs `ref` and `dist`.
- Read scalar output `score` and publish it as `dists_sq`.

The smoke checkpoint at `model/tiny/dists_sq.onnx` is intentionally tiny:
`Sub -> Mul -> ReduceMean`. It locks the ABI and exercises named two-input
binding without claiming production DISTS quality.

## Follow-Up Boundary

`T7-DISTS` is closed by the extractor surface. `T7-DISTS-followup` remains
open for real upstream-derived DISTS-compatible weights, representative-score
validation, and any threshold guidance needed once the output matches the
published metric rather than the smoke graph.

## Verification

```bash
.venv/bin/python scripts/gen_dists_sq_placeholder_onnx.py
meson test -C build-dists test_dists
```

## References

- [ADR-0236](../adr/0236-dists-extractor.md)
- [Research-0043](0043-dists-extractor-design.md)
- `req`: user pointed at the `DISTS extractor (stub)` docs page and asked to
  do that gap next.
