`vmaf-roi` now accepts 10/12/16-bit little-endian planar YUV inputs and
normalises luma to the existing 8-bit saliency-model contract before emitting
x265 / SVT-AV1 ROI sidecars.
