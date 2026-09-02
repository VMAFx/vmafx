# DISTS-Sq Smoke Checkpoint

`dists_sq_placeholder_v0` is a tiny-AI smoke checkpoint for the
`dists_sq` full-reference extractor. It locks the DISTS host ABI and lets
libvmaf test the two-input ONNX path while the real Ding et al. weights are
tracked separately as `T7-DISTS-followup`.

## Model

| Field | Value |
| --- | --- |
| Registry id | `dists_sq_placeholder_v0` |
| File | `model/tiny/dists_sq.onnx` |
| Sidecar | `model/tiny/dists_sq.json` |
| SHA-256 | `ec8433e8c7c6a33ef3032a6e4538833e0bbb59de9f088054bbcb3be0e371ee55` |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Generator | `scripts/gen_dists_sq_placeholder_onnx.py` |

## Contract

Inputs:

| Name | Type | Shape | Meaning |
| --- | --- | --- | --- |
| `ref` | float32 | `[1, 3, H, W]` | ImageNet-normalised RGB reference frame |
| `dist` | float32 | `[1, 3, H, W]` | ImageNet-normalised RGB distorted frame |

Output:

| Name | Type | Shape | Meaning |
| --- | --- | --- | --- |
| `score` | float32 | scalar | Mean squared distance between `ref` and `dist` tensors |

The extractor publishes the scalar as the per-frame `dists_sq` feature.
The host side accepts planar YUV 4:2:0 / 4:2:2 / 4:4:4 at 8, 10, 12, or
16 bpc and normalises high-bit-depth samples into the same RGB8 tensor
contract before ImageNet normalisation.

## Intended Use

Use this checkpoint for build, packaging, registry, and smoke-test coverage of
the `dists_sq` extractor. It proves that model lookup, named two-input
binding, dynamic image dimensions, and scalar output collection work through
the tiny-AI runtime.

Do not use this checkpoint for perceptual-quality decisions. It is not a
trained DISTS model and intentionally sets `"smoke": true` in
`model/tiny/registry.json`.

## Regeneration

```bash
.venv/bin/python scripts/gen_dists_sq_placeholder_onnx.py
```

The generator writes the ONNX file, sidecar JSON, and registry entry in one
pass. Re-run registry validation after regeneration:

```bash
.venv/bin/python ai/scripts/validate_model_registry.py model/tiny/registry.json
```

## Limitations

The graph is `Sub -> Mul -> ReduceMean`; it contains no learned feature
backbone and no DISTS texture/structure statistics. The production follow-up
must replace the placeholder with upstream-derived DISTS-compatible weights,
pin a new SHA-256, update this model card, and verify representative scores
against an independent reference.
