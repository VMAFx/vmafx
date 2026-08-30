# DISTS-Sq Extractor

`dists_sq` is the fork's DISTS-shaped full-reference deep perceptual
extractor. It mirrors the existing `lpips` tiny-AI surface: libvmaf converts
each reference/distorted frame pair to ImageNet-normalised RGB tensors, runs a
two-input ONNX model through the DNN runtime, and emits one scalar per frame.

The shipped checkpoint is a smoke placeholder, not Ding et al.'s production
DISTS weights yet. It exists to make the feature discoverable, test the host
pipeline, and lock the model ABI while the real weights are prepared under
`T7-DISTS-followup`.

## Output

| Field | Value |
| --- | --- |
| Feature name | `dists_sq` |
| Output metric | `dists_sq` |
| Direction | Lower is more similar |
| Current range | Unbounded non-negative smoke distance |
| Model path | `model/tiny/dists_sq.onnx` |

The placeholder ONNX computes mean squared distance between the two
normalised RGB tensors. Treat it as a pipeline smoke signal only; do not use
the placeholder values for codec tuning decisions or MOS-style claims.

## Usage

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 10 \
    --feature dists_sq \
    --feature_params dists_sq:model_path=model/tiny/dists_sq.onnx \
    --output score.json
```

The extractor can also resolve the model path from the environment:

```bash
export VMAF_DISTS_SQ_MODEL_PATH=model/tiny/dists_sq.onnx
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 10 \
     --feature dists_sq --output score.json
```

Resolution order is `model_path` option first, then
`VMAF_DISTS_SQ_MODEL_PATH`. If neither is set, extractor init fails with
`-EINVAL` so callers do not accidentally run an implicit model.

## C API

```c
VmafFeatureDictionary *opts = NULL;
vmaf_feature_dictionary_set(&opts, "model_path", "model/tiny/dists_sq.onnx");
vmaf_use_feature(vmaf, "dists_sq", opts);
```

After frames are processed, pooled scores are available under the
`dists_sq` feature key.

## Inputs And Runtime

- Pixel formats: YUV 4:2:0, 4:2:2, and 4:4:4.
- Bit depth: 8, 10, 12, or 16 bpc. High-bit-depth planar inputs are
  rounded into the same 8-bit RGB tensor contract used by LPIPS.
- Chroma: required; YUV400 is rejected because the extractor converts to RGB.
- Tensor contract: `ref` and `dist` float32 tensors shaped
  `[1, 3, H, W]`, ImageNet-normalised RGB, NCHW.
- ONNX output: scalar `score` float32.
- Runtime: ONNX Runtime through the tiny-AI DNN surface. ORT execution
  provider selection follows `--tiny-device`.

## Limitations

The current checkpoint is smoke-only. It does not implement DISTS' learned
texture/structure feature stack and should not be compared to published DISTS
numbers. Real upstream-derived weights remain the follow-up. The host
extractor uses the same BT.709 limited-range RGB conversion helper as the
LPIPS extractor.

## See Also

- [DISTS-Sq model card](../ai/models/dists_sq.md)
- [Feature extractor matrix](features.md)
- [ADR-0236](../adr/0236-dists-extractor.md)
