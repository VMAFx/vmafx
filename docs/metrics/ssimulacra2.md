# SSIMULACRA 2 Extractor

SSIMULACRA 2 is a full-reference perceptual similarity metric from the JPEG XL
ecosystem. It compares reference and distorted frames in an XYB-inspired colour
space, combines multi-scale SSIM-style structural terms, and applies an
asymmetric penalty for lost texture energy. The fork ships it as a normal
libvmaf feature extractor named `ssimulacra2`.

## Output

| Field | Value |
| --- | --- |
| Feature name | `ssimulacra2` |
| Output metric | `ssimulacra2` |
| Direction | Higher is better |
| Range | `[0, 100]`; identical frames return `100` |
| Snapshot gate | `python/test/ssimulacra2_test.py` |

Practical score bands:

| Score | Meaning |
| --- | --- |
| `90-100` | Visually lossless |
| `70-90` | High quality |
| `50-70` | Medium quality, clearly lossy |
| `30-50` | Low quality |
| `0-30` | Very low quality |

## Usage

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature ssimulacra2 \
    --output score.json
```

The per-frame JSON metric key is `ssimulacra2`; pooled values appear under the
same key in `pooled_metrics`.

## Options

| Option | Type | Default | Values | Effect |
| --- | --- | --- | --- | --- |
| `yuv_matrix` | int | `0` | `0..3` | `0`: BT.709 limited, `1`: BT.601 limited, `2`: BT.709 full, `3`: BT.601 full |

Example:

```bash
vmaf ... --feature ssimulacra2=yuv_matrix=2
```

## Inputs And Backends

- Pixel formats: YUV 4:2:0, 4:2:2, and 4:4:4. The colour conversion needs
  both chroma planes, so 4:0:0 (luma-only) input is refused at init with
  `ssimulacra2: needs a YUV 4:2:0, 4:2:2 or 4:4:4 input, not 4:0:0` and
  `-EINVAL` from `vmaf_read_pictures()`. (The `vmaf` CLI never produces it: it
  rejects `-p 400` and converts Y4M `mono` input to 4:2:0.)
- Bit depths: 8, 10, and 12 bpc.
- CPU SIMD: AVX2, AVX-512, NEON, and SVE2 when the host advertises it.
- GPU twins: `ssimulacra2_cuda`, `ssimulacra2_sycl`, and
  `ssimulacra2_hip`. (The Vulkan backend was removed in ADR-0726.)

The CPU scalar/SIMD path is bit-exact across the fork's host matrix. The CUDA,
HIP and Metal twins offload the pyramid blur and per-pixel multiply stages while
keeping the colour conversion, XYB, downsample and final combine on the host.

### SYCL: device-resident, one readback per frame

`ssimulacra2_sycl` runs the whole frame on the device (ADR-1363): it uploads the
raw Y/U/V planes once, converts to linear RGB and XYB, blurs, forms the SSIM and
edge-difference sums and downsamples on the device, and reads back one 864-byte
block of per-scale sums. Name it, or pass `--backend sycl --feature ssimulacra2`,
which maps to the twin for inputs it can run and to the CPU extractor otherwise
(ADR-1359; `ssimulacra2_sycl` needs chroma planes and at least 8x8):

```shell
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend sycl --no_prediction --feature ssimulacra2_sycl -o out.json --json
```

Colour conversion, XYB, blurs and downsample match the CPU bit for bit. The CPU
evaluates the per-pixel SSIM and edge terms in double precision and adds them
one by one; the device has no double precision, so it evaluates them in pairs of
floats (about 13 significant digits) and adds them in a fixed tree. The
per-frame score is within about 1e-11 of `--backend cpu` (6.7e-12 at worst at
3840x2160) and is the same on every device and every run. Before ADR-1363 the
twin matched the CPU exactly but copied about 4 GB per 4K frame between device
and host. On an Arc B580 a 3840x2160 frame now takes about 33 ms, against
963 ms before and about 167 ms for the CPU extractor on 16 threads. Check and
time it with:

```shell
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --feature ssimulacra2 --max-abs-diff 1e-9 \
    --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
```

`ssimulacra2_sycl` rejects 4:0:0 (luma-only) input at init.

### Cross-compiler bit-exactness (FMA unification)

`picture_to_linear_rgb` performs the YCbCr→linear-RGB conversion using a
fused-multiply-add (FMA) chain on every code path: scalar uses `fmaf()` and the
AVX2 / AVX-512 / NEON main loops use `_mm256_fmadd_ps` / `_mm512_fmadd_ps` /
`vfmaq_f32`. Earlier revisions used explicit `mul + add` pairs, but icx with
`-mfma` auto-fused them while gcc kept them separately-rounded, producing
sub-ULP divergence between compilers. Unifying on FMA preserves the
left-to-right associativity of `G = Yn + cb_g*Un + cr_g*Vn` (two chained FMAs)
while delivering a single, identically-rounded result on every supported
compiler. See ADR-0891 for the analysis and alternatives.

ADR-0891 originally reached the four SIMD kernels and the SIMD test's own
scalar reference, but not the five shipped copies that are not SIMD: the
scalar fallback in `core/src/feature/ssimulacra2.c` and the host-side
conversions in the CUDA, HIP, Metal and SYCL twins. Those kept a plain
`mul + add`, so until ADR-1205 a host **without** AVX2 scored `ssimulacra2`
differently from one with it, and every GPU backend disagreed with the CPU.

The size of that disagreement is worth knowing when reading any
`ssimulacra2` number: the pipeline is ill-conditioned downstream. The
edge-diff term computes `|img - blur(img)|`, a catastrophic cancellation, and
pooling takes a 4-norm, which is dominated by the few largest survivors. A
~1 ULP difference in linear RGB therefore grew to a **2.62e-03** difference in
the final score — not the ~1e-7 a single rounding step would suggest. After
ADR-1205 every shipped path uses the same FMA chain and CPU-vs-CUDA agreement
is ~2.8e-09.

If you add another `ssimulacra2` code path, the conversion must be
`fmaf()`-based and in the ADR-0891 order. Note that
`core/test/test_ssimulacra2_simd.c` compares the SIMD kernels against a
*private* scalar reference rather than the shipped function, so it will not
catch a shipped copy that drifts.

## Limitations

- Chroma is nearest-neighbour upsampled to luma resolution before colour
  conversion.
- The recursive Gaussian coefficient derivation is pinned to the shipped
  sigma used by the libjxl reference path; arbitrary sigma values are not a
  user option.
- Scores are useful as a perceptual ranking signal, not as an MOS-calibrated
  VMAF replacement.

## See Also

- [Feature extractor matrix](features.md#ssimulacra-2-perceptual-similarity-in-xyb-space)
- [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
- [ADR-0164](../adr/0164-ssimulacra2-snapshot-gate.md)
- [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md)
