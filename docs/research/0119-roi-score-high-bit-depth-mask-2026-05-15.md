# Research 0119: ROI-Score High-Bit-Depth Mask Materialisation

## Summary

The user-facing `vmaf-roi-score` docs still described the saliency-mask
materialiser as 8-bit planar YUV only. That left CHUG/HDR-style 10-bit
inputs on the unweighted full-frame path even though the Python Option C
tool already owns mask materialisation outside libvmaf.

## Findings

- `tools/vmaf-roi-score/src/vmafroiscore/mask.py` rejected pix_fmts that
  contained `10`, `12`, or `16`.
- The mask inference API already consumes 8-bit RGB bytes, so high-bit
  support does not require a model-input schema change.
- The output YUV must remain native-depth: down-converting the masked
  file to 8-bit would make the subsequent `vmaf --bitdepth` invocation
  score malformed input.

## Decision Input

The smallest useful fix is to keep the existing Option C algorithm and
make the plane reader/writer bit-depth-aware:

1. Parse little-endian planar 8/10/12/16-bit pix_fmts.
2. Blend Y/U/V samples at native integer depth.
3. Down-convert only the reference frame used for saliency-model RGB
   inference.
4. Keep big-endian high-bit-depth YUV out of scope until a real caller
   needs it.

## Smoke

```bash
PYTHONPATH=tools/vmaf-roi-score/src .venv/bin/python -m pytest \
  tools/vmaf-roi-score/tests/test_combine.py -q
.venv/bin/ruff check \
  tools/vmaf-roi-score/src/vmafroiscore/mask.py \
  tools/vmaf-roi-score/src/vmafroiscore/score.py \
  tools/vmaf-roi-score/tests/test_combine.py
```
