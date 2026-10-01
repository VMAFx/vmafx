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
| `psnr_hvs_y` | HVS-weighted PSNR on the luma (Y) plane | Always |
| `psnr_hvs_cb` | HVS-weighted PSNR on the Cb (U) plane | `enable_chroma=true` (the default), not for 4:0:0 |
| `psnr_hvs_cr` | HVS-weighted PSNR on the Cr (V) plane | `enable_chroma=true` (the default), not for 4:0:0 |
| `psnr_hvs` | Combined score: 0.8 Y + 0.1 (Cb + Cr) of the linear plane values, in dB; the luma value when chroma is off | Always |

## Options

- `enable_chroma` (bool, default `true`): score the Cb and Cr planes and weight them into `psnr_hvs`. With `false`, or for YUV400P sources, only luma is scored and `psnr_hvs` equals `psnr_hvs_y`.

### How to run

```bash
# Per-plane scores and the combined psnr_hvs (default)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr_hvs --output /dev/stdout

# Luma only
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature 'psnr_hvs=enable_chroma=false' --output /dev/stdout
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

The CUDA (`psnr_hvs_cuda`), HIP (`psnr_hvs_hip`), and SYCL (`psnr_hvs_sycl`,
[ADR-1369](../adr/1369-sycl-shared-planes-light-twins.md)) twins read the raw
integer samples of the device pictures at every supported depth, 8 to 12 bits
(every backend rejects deeper input, like the CPU extractor), eliminating host
float conversions and redundant pinned host staging allocations. Two threads share
each 8x8 block, one per image; the DCT runs in shared (local) memory, and one
launch covers every plane. 4:0:0 input is scored on luma only, as on the CPU.

`psnr_hvs_hip` returns the same values, bit for bit, as the host-conversion twin
it replaced, on the Netflix 576x324 pair, on 1920x1080 and on 3840x2160 content,
except at 9 and 11 bits, where the previous twin was wrong (-1.57 dB and NaN on a
64x48 test picture whose CPU scores are 22.47 and 33.97 dB).

### Agreement with the CPU extractor

The CPU extractor (`third_party/xiph/psnr_hvs.c`, `calc_psnrhvs()`) adds every
masked coefficient error of a plane to one running `float`: 64 terms per block,
about 10.8 million for a 3840x2160 luma plane. The rounding of each addition
depends on the sum so far, so the score depends on the order of the additions.
The CPU's value is the reference; it is not the most accurate one (it is up to
1.1e-2 dB above the same sum taken in `double` on 3840x2160 content), and it
stays as it is because the Netflix golden values of
`python/test/third_party/xiph/vmafexec_feature_extractor_test.py` pin it.

| Twin | How it sums | Agreement with `--backend cpu` |
|---|---|---|
| `psnr_hvs_cuda` | The kernel stores the 64 terms of every block, computed in the CPU's arithmetic; the host adds them in the CPU's order ([ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md)) | Bit-identical on every output, at every frame size and depth |
| `psnr_hvs_sycl`, `psnr_hvs_hip` | 64 terms per block on the device, then the blocks on the host | Held to a tolerance: 5e-4 dB at 576x324, growing with the frame size ([ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)) |

`psnr_hvs_cuda` was measured against the CPU at `--precision max` on the
Netflix 576x324 pair (8, 10 and 12 bits, 4:2:0 and 4:2:2), the 1920x1080
checkerboard pairs and Big Buck Bunny at 1920x1080 and 3840x2160 (8 and 10
bits): every frame of `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr`
has the same bits. Check it on your device with:

```bash
for b in cpu cuda; do
  build/tools/vmaf -r ref.yuv -d dist.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend $b --no_prediction --feature psnr_hvs --precision max --json -o $b.json
done
python3 - <<'PY'
import json
a, b = (json.load(open(f"{n}.json"))["frames"] for n in ("cpu", "cuda"))
print(all(x["metrics"] == y["metrics"] for x, y in zip(a, b)))
PY
```

The exact sum has a cost. `psnr_hvs_cuda` reads 256 bytes per block back to the
host (65 MB for a 3840x2160 4:2:0 frame, 259 MB at 7680x4320, held on the
device and in pinned host memory) and adds the terms on one host thread. On an
RTX 4090 a frame takes 0.29 ms at 576x324, 3.1 ms at 1920x1080 and 12.2 ms at
3840x2160, where sixteen CPU threads take 0.14, 1.7 and 6.5 ms; use
`--backend cpu` when throughput matters more than keeping the frame on the
device. [Research-1397](../research/1397-psnr-hvs-twins-cpu-float-sum.md) has
the measurements; `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` in
[docs/state.md](../state.md) tracks the tuning, and
`T-HIP-PSNR-HVS-EXACT-SUM-2026-10-01` / `T-SYCL-PSNR-HVS-EXACT-SUM-2026-10-01`
the same change for the other twins.

## See also

- [Features](features.md) - full feature extractor reference
