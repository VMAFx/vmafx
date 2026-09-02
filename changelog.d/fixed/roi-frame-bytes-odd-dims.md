Fix `frame_bytes()` chroma ceiling-division for odd-dimension YUV inputs in `vmaf-roi`.

`core/tools/vmaf_roi.c` computed the per-frame byte count for 4:2:0 and 4:2:2 inputs
using integer-truncating arithmetic (`y/2` and `y+y`). For odd width or height the
correct chroma plane size requires ceiling division. The truncation shortfall caused
`fseeko()` to land at the wrong file offset for any frame index > 0, resulting in
luma data read from inside the previous frame's chroma region and incorrect saliency
maps. The 4:4:4 path was unaffected.

Fix: use `cw = (w+1)/2`, `ch = (h+1)/2` ceiling-division in all three pixel-format
branches. Four regression tests added to `test_vmaf_roi` (even dims, odd 4:2:0, odd
4:2:2, 4:4:4 baseline).
