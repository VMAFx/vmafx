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

## Inputs

- **Pixel formats:** YUV 4:2:0, 4:2:2, and 4:4:4. The colour conversion needs
  both chroma planes, so 4:0:0 (luma-only) input is refused at init with
  `ssimulacra2: needs a YUV 4:2:0, 4:2:2 or 4:4:4 input, not 4:0:0` and
  `-EINVAL` from `vmaf_read_pictures()`. The `vmaf` CLI never produces it: it
  rejects `-p 400` and converts Y4M `mono` input to 4:2:0.
- **Bit depths:** 8, 10, and 12 bpc.
- **CPU SIMD:** AVX2, AVX-512, NEON, and SVE2 when the host advertises it. The
  CPU scalar/SIMD path is bit-exact across the fork's host matrix.

## GPU twins

The CUDA, SYCL and HIP twins run the whole frame on the device and return the
CPU extractor's score bit for bit. The Metal twin offloads the pyramid blur
and per-pixel multiply stages while keeping the colour conversion, XYB,
downsample and final combine on the host. (The Vulkan backend was removed in
ADR-0726.)

| Twin | Invocation | Exact vs CPU | ADR | Evidence (fragment) | Measured on |
| --- | --- | --- | --- | --- | --- |
| `ssimulacra2_cuda` | `--backend cuda` | Yes | ADR-1391, [ADR-1433](../adr/1433-cuda-ssimulacra2-cpu-sum-order.md) | `scripts/ci/exact_twins.d/ssimulacra2.cuda` | RTX 4090: 113 of 113 frames |
| `ssimulacra2_sycl` | `--backend sycl` | Yes | ADR-1363, [ADR-1446](../adr/1446-sycl-ssimulacra2-cpu-bits.md) | `scripts/ci/exact_twins.d/ssimulacra2.sycl` | Arc A380 (xe): 266 of 266 frames |
| `ssimulacra2_hip` | `--backend hip` | Yes | ADR-1390, [ADR-1445](../adr/1445-hip-ssimulacra2-cpu-sum-order.md) | `scripts/ci/exact_twins.d/ssimulacra2.hip` | gfx1036 (ROCm 7.2.4): 178 of 178 frames |
| `ssimulacra2_metal` | `--backend metal` | No (`FEATURE_TOLERANCE`, 5e-3) | none | none | not recorded |

The measurements are at `--precision max` against `--backend cpu`, and with
every `yuv_matrix`:

- CUDA: Netflix 576x324 at 8, 10, 12 and 16 bits, both 1080p checkerboard
  pairs, BBB 3840x2160.
- SYCL: Netflix 576x324 at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, both
  1080p checkerboard pairs, 200 frames of BBB 3840x2160.
- HIP: Netflix 576x324 at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, both
  1080p checkerboard pairs, Sparks 480x270, full-range noise at four bit
  depths, a bright 16-bit 1080p pair, 48 frames of BBB 3840x2160.

### Speed and memory

| Twin | Frame | Time now | Time before exact sums | CPU extractor | Memory |
| --- | --- | --- | --- | --- | --- |
| CUDA, RTX 4090 | 3840x2160 | 15.6 ms | 7.8 ms (ADR-1433) | 126 ms on sixteen threads | About 1.4 GB device (14.5 full-size three-plane float buffers: the linear-RGB pyramid, XYB, and the two passes of the five blurs); the sums add 3.8 MB |
| SYCL, Arc A380 | 3840x2160 | 195 ms | 84 ms (ADR-1446) | About 125 ms on sixteen threads | not recorded |
| SYCL, Arc A380 | 576x324 | 13.5 ms | 5.3 ms | 1.4 ms on sixteen threads | not recorded |
| HIP, gfx1036 | 3840x2160 | 662 ms | 234 ms (ADR-1445) | 124 ms on sixteen threads | not recorded |
| HIP, gfx1036 | 1920x1080 | 167 ms | 58 ms | not recorded | not recorded |

!!! note "The CPU is faster on integrated GPUs and on the Arc A380"
    On the A380 the CPU is the faster path for this metric at both sizes
    (before ADR-1446 the twin was faster at 3840x2160). On the integrated
    gfx1036 the CPU is the faster path too. Most of the SYCL increase is the
    integer arithmetic of the terms; the pass over the chunks runs on one lane
    per sum and is what small frames pay
    (`T-SYCL-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02`). On HIP most of the
    increase is the double-precision terms, evaluated twice per scale, and the
    integer steps (`T-HIP-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02`).

### How the exact sums work

The CPU evaluates six terms per pixel and channel in double precision and adds
each into one double, pixel after pixel, and every such add rounds. The twins
return the result of that loop without running it on one thread.

- While the running sum stays between two powers of two, adding a term moves
  it by a whole number of steps. The device adds those whole numbers per chunk
  of pixels in parallel (1024 pixels on CUDA and HIP, 512 on SYCL), and one
  pass over the chunks puts them together.
- The few chunks in which the sum passes a power of two are added term by
  term.
- CUDA and HIP devices have double precision, so their per-pixel terms are
  the CPU's own double expressions. A SYCL device has no double precision, so
  the SYCL twin computes each term's double in 64-bit integers (a sign, a
  53-bit significand and an exponent), the CPU's operations one for one.

### CUDA: device-resident, one readback per frame

`ssimulacra2_cuda` (ADR-1391) reads the Y/U/V planes that the CUDA picture
pipeline uploads once for every CUDA extractor, so the twin makes no copy of
its own. Its only transfer is one 864-byte block of per-scale sums per frame.
Name it, or pass `--backend cuda --feature ssimulacra2`, which maps to the
twin for inputs it can run and to the CPU extractor otherwise (4:0:0 input and
frames below 8x8):

```shell
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend cuda --no_prediction --feature ssimulacra2_cuda -o out.json --json
```

Check and time it with (`--vmaf` takes an absolute path; `--bbb-dir` is
explained [below](#checking-a-twin)):

```shell
python3 scripts/dev/speed_gpu_parity.py --backend cuda --feature ssimulacra2 \
    --vmaf "$PWD/build/tools/vmaf" \
    --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
```

### SYCL: device-resident, one readback per frame

`ssimulacra2_sycl` (ADR-1363) uploads the raw Y/U/V planes once, converts to
linear RGB and XYB, blurs, forms the SSIM and edge-difference sums and
downsamples on the device, and reads back one 864-byte block of per-scale
sums. Name it, or pass `--backend sycl --feature ssimulacra2`, which maps to
the twin for inputs it can run and to the CPU extractor otherwise
([ADR-1359](../adr/1359-cli-feature-backend-twin.md); the twin needs chroma
planes and at least 8x8, and rejects 4:0:0 input at init):

```shell
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend sycl --no_prediction --feature ssimulacra2_sycl -o out.json --json
```

Check and time it with:

```shell
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --feature ssimulacra2 --vmaf "$PWD/build/tools/vmaf" \
    --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
```

### HIP: device-resident, tiled row pass

`ssimulacra2_hip` (ADR-1390) uploads the raw Y/U/V planes once into pinned
staging in `submit()`, converts to linear RGB and XYB, executes IIR blurs with
a tiled shared-memory row pass (`SS2H_ROW_TILE` rows per wavefront,
single-wave blocks, two-slot ring, register prefetch), evaluates the per-pixel
SSIM and edge terms in double precision, adds them with the result of the
CPU's loops, and downsamples on the device. A single 864-byte readback of
per-scale sums occurs in `collect()`. Name it, or pass
`--backend hip --feature ssimulacra2` (ADR-1359). The twin rejects 4:0:0 input
at init:

```shell
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend hip --no_prediction --feature ssimulacra2_hip -o out.json --json
```

Check and time it with:

```shell
python3 scripts/dev/speed_gpu_parity.py --backend hip --feature ssimulacra2 \
    --vmaf "$PWD/build-hip/tools/vmaf" \
    --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
```

### Checking a twin

`scripts/dev/speed_gpu_parity.py` compares the twin with the CPU extractor
frame by frame. Its default bound is 0: every frame must be bit-identical.

The script always runs two fixtures: the Netflix 576x324 pair from
`--netflix-dir`, and a Big Buck Bunny 3840x2160 8-bit 4:2:0 pair that you
supply as `ref_3840x2160_200f.yuv` and `dis_3840x2160_200f.yuv` in
`--bbb-dir`. The default `testdata/bbb` is not tracked in the repository, so
pass `--bbb-dir` explicitly.

## Implementation notes: cross-compiler bit-exactness (FMA unification)

`picture_to_linear_rgb` performs the YCbCr to linear-RGB conversion using a
fused-multiply-add (FMA) chain on every code path: scalar uses `fmaf()` and the
AVX2 / AVX-512 / NEON main loops use `_mm256_fmadd_ps` / `_mm512_fmadd_ps` /
`vfmaq_f32`. This preserves the left-to-right associativity of
`G = Yn + cb_g*Un + cr_g*Vn` (two chained FMAs) while delivering a single,
identically-rounded result on every supported compiler. See ADR-0891 for the
analysis and alternatives.

The pipeline is ill-conditioned downstream, which matters when reading any
`ssimulacra2` number:

- The edge-diff term computes `|img - blur(img)|`, a catastrophic
  cancellation.
- Pooling takes a 4-norm, which is dominated by the few largest survivors.
- A difference of about 1 ULP in linear RGB therefore grew to a **2.62e-03**
  difference in the final score, not the about 1e-7 a single rounding step
  would suggest.

After ADR-1205 every shipped path uses the same FMA chain and CPU-vs-CUDA
agreement is about 2.8e-09.

If you add another `ssimulacra2` code path, the conversion must be
`fmaf()`-based and in the ADR-0891 order. `core/test/test_ssimulacra2_simd.c`
compares the SIMD kernels against a *private* scalar reference rather than the
shipped function, so it will not catch a shipped copy that drifts.

## Limitations

- Chroma is nearest-neighbour upsampled to luma resolution before colour
  conversion.
- The recursive Gaussian coefficient derivation is pinned to the shipped
  sigma used by the libjxl reference path; arbitrary sigma values are not a
  user option.
- Scores are useful as a perceptual ranking signal, not as an MOS-calibrated
  VMAF replacement.

## History

- **ADR-1445 (HIP), ADR-1446 (SYCL), ADR-1433 (CUDA).** Before them the terms
  were added in a fixed tree (pairs of floats on SYCL and HIP) and no SYCL or
  HIP frame was identical. The score was up to 7.6e-11 from the CPU's on SYCL
  and HIP and up to 7.3e-11 on CUDA; `ssimulacra2_sycl`, `ssimulacra2_hip` and
  `ssimulacra2_cuda` scores stored before differ from new ones by that much.
- **ADR-1391, ADR-1363, ADR-1390.** The CUDA, SYCL and HIP twins became
  device-resident. Before ADR-1391 the CUDA twin copied every scale between
  device and host; before ADR-1363 the SYCL twin copied about 4 GB per 4K
  frame between device and host.
- **ADR-1205.** `picture_to_linear_rgb`'s FMA chain reached the five shipped
  copies that ADR-0891 had not: the scalar fallback in
  `core/src/feature/ssimulacra2.c` and the host-side conversions in the CUDA,
  HIP, Metal and SYCL twins. Those kept a plain `mul + add`, so until
  ADR-1205 a host without AVX2 scored `ssimulacra2` differently from one with
  it, and every GPU backend disagreed with the CPU.
- **ADR-0891.** Earlier revisions used explicit `mul + add` pairs, but icx
  with `-mfma` auto-fused them while gcc kept them separately-rounded,
  producing sub-ULP divergence between compilers. It reached the four SIMD
  kernels and the SIMD test's own scalar reference.

## See Also

- [Feature extractor
  matrix](features.md#ssimulacra-2-perceptual-similarity-in-xyb-space)
- [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
- [ADR-0164](../adr/0164-ssimulacra2-snapshot-gate.md)
- [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md)
