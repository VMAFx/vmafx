<!-- markdownlint-disable MD013 MD060 -->
# PSNR-HVS

PSNR-HVS (Peak Signal-to-Noise Ratio - Human Visual System) extends traditional
PSNR with contrast sensitivity function (CSF) weighting applied in the DCT
domain, making it more sensitive to perceptually significant distortions.

## Variants

| Extractor name | Algorithm | Options |
|---|---|---|
| `psnr_hvs` | CSF-weighted DCT-domain PSNR | `enable_chroma` |

## `psnr_hvs` extractor

The extractor computes PSNR in the 8x8 DCT domain with per-coefficient
weighting derived from a human visual system contrast sensitivity model
(Ponomarenko et al.). It is the extractor invoked when VMAF model JSON files
reference `"psnr_hvs"`.

### Output features

| Feature name | Description | Condition |
|---|---|---|
| `psnr_hvs` | HVS-weighted PSNR on the luma (Y) plane | Always |
| `psnr_hvs_cb` | HVS-weighted PSNR on the Cb (U) plane | `enable_chroma=true` only |
| `psnr_hvs_cr` | HVS-weighted PSNR on the Cr (V) plane | `enable_chroma=true` only |

## Options

- `enable_chroma` (bool, default `false`): emit per-plane `_cb` and `_cr` scores in addition to luma. YUV400P sources are always luma-only.

### How to run

```bash
# Luma-only PSNR-HVS (default)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr_hvs --output /dev/stdout

# Per-channel PSNR-HVS (luma + Cb + Cr)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature 'psnr_hvs:enable_chroma=true' --output /dev/stdout
```

## Backend parity (chroma plane dimensions)

`psnr_hvs` runs on CPU, CUDA, HIP, and SYCL with runtime backend selection.
For chroma planes (`enable_chroma=true`) on subsampled formats (YUV420 /
YUV422) whose luma width or height is **odd**, each chroma plane dimension is
the **ceiling** of the half-resolution, not the floor — e.g. a 1921-wide 4:2:0
frame has a 961-sample-wide chroma plane (`(1921 + 1) / 2`), not 960. All
backends compute the ceiling so that `psnr_hvs_cb` / `psnr_hvs_cr` agree across
CPU / CUDA / HIP / SYCL on odd-dimension frames. (The SYCL path previously
floored the chroma dimensions, dropping the last chroma column/row on odd
inputs and diverging from the other backends.)

## GPU twins

### Sample conversion

The CUDA (`psnr_hvs_cuda`) and SYCL (`psnr_hvs_sycl`,
[ADR-1369](../adr/1369-sycl-shared-planes-light-twins.md)) twins read the raw
integer samples of the device pictures at every supported depth, 8 to 12 bits
(every backend rejects deeper input, like the CPU extractor). Two threads share
each 8x8 block, one per image; the DCT runs in shared (local) memory, and one
launch covers every plane. Each block's float sum goes to a partials buffer, and
the host adds each plane's partials in block order. 4:0:0 input is scored on luma
only, as on the CPU.

`psnr_hvs_cuda` returns the same values, bit for bit, as the host-conversion twin
it replaced, on the Netflix 576x324 pair, on 1920x1080 and on 3840x2160 content,
except at 9 and 11 bits, where that twin was wrong (-1.57 dB and NaN on a
64x48 test picture whose CPU scores are 22.47 and 33.97 dB).

### Difference to the CPU extractor at large frame sizes

The CPU extractor (`third_party/xiph/psnr_hvs.c`, `calc_psnrhvs()`) adds every
masked coefficient error of a plane to one running `float`, about 10.8 million
terms for a 3840x2160 luma plane. Its rounding error grows with the frame: on 22
frames of the 3840x2160 Big Buck Bunny fixture that
[docs/state.md](../state.md) uses, the CPU values are up to 1.1e-2 dB from the
same sum taken in `double`, while `psnr_hvs_cuda` is within 5.1e-5 dB of it. The
difference between a GPU twin and the CPU at that size is therefore mostly the
CPU's. It exceeds the 3.34e-3 dB that
[ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md) allows at
3840x2160 on this content; `T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30` tracks it. The
CPU extractor keeps its `float` sum because the Netflix golden values of
`python/test/third_party/xiph/vmafexec_feature_extractor_test.py` pin it.

## See also

- [Features](features.md) - full feature extractor reference
