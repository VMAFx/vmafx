<!-- markdownlint-disable MD013 MD060 -->
# PSNR-HVS

PSNR-HVS (Peak Signal-to-Noise Ratio - Human Visual System) extends
traditional PSNR with contrast sensitivity function (CSF) weighting applied in
the DCT domain, making it more sensitive to perceptually significant
distortions. The score is in dB and higher is better.

The `psnr_hvs` extractor computes PSNR in the 8x8 DCT domain with
per-coefficient weighting derived from a human visual system contrast
sensitivity model (Ponomarenko et al.). It is the extractor invoked when VMAF
model JSON files reference `"psnr_hvs"`. The scalar source is
`core/src/feature/third_party/xiph/psnr_hvs.c`.

## How to run

```bash
# Per-plane scores and the combined psnr_hvs (default)
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr_hvs --output /dev/stdout

# Luma only
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature 'psnr_hvs=enable_chroma=false' --output /dev/stdout
```

Input is limited to 12 bits per component; deeper input is rejected on every
backend.

## Output features

| Feature name | Description | Condition |
|---|---|---|
| `psnr_hvs_y` | HVS-weighted PSNR on the luma (Y) plane | Always |
| `psnr_hvs_cb` | HVS-weighted PSNR on the Cb (U) plane | `enable_chroma=true` (the default), not for 4:0:0 |
| `psnr_hvs_cr` | HVS-weighted PSNR on the Cr (V) plane | `enable_chroma=true` (the default), not for 4:0:0 |
| `psnr_hvs` | Combined score: 0.8 Y + 0.1 (Cb + Cr) of the linear plane values, in dB; the luma value when chroma is off | Always |

## Options

| Option | Type | Default | Effect |
|---|---|---|---|
| `enable_chroma` | bool | `true` | Score the Cb and Cr planes and weight them into `psnr_hvs`. With `false`, or for YUV400P sources, only luma is scored and `psnr_hvs` equals `psnr_hvs_y`. |

## Chroma plane dimensions

For chroma planes (`enable_chroma=true`) on subsampled formats (YUV420 /
YUV422) whose luma width or height is **odd**, each chroma plane dimension is
the **ceiling** of the half-resolution, not the floor.

- **Example.** A 1921-wide 4:2:0 frame has a 961-sample-wide chroma plane
  (`(1921 + 1) / 2`), not 960.
- **Why.** All backends (CPU, CUDA, HIP, SYCL) compute the ceiling so that
  `psnr_hvs_cb` / `psnr_hvs_cr` agree across them on odd-dimension frames.
- **Earlier behaviour.** The SYCL path previously floored the chroma
  dimensions, dropping the last chroma column or row on odd inputs and
  diverging from the other backends.

## CPU instruction sets

The CPU extractor binds one of three functions per plane. All three return the
same bits.

| Function | Where |
|---|---|
| `calc_psnrhvs()` | Scalar, every host |
| `calc_psnrhvs_avx2()` | x86 hosts with AVX2 |
| `calc_psnrhvs_neon()` | aarch64 |

The SIMD functions vectorise the integer DCT, which has no rounding, and keep
every float operation in the scalar's order: the means and variances, the
masking threshold $\sqrt{\mathrm{mask} \cdot \mathrm{variance\_ratio}} / 32$ with its `float` product,
and the running `float` sum of the masked errors.

`test_psnr_hvs_dispatch_invariance` holds the extractor to that through the
public API: 165 picture pairs (blocks recorded from the Netflix pair,
generated texture, random samples, the fixtures of the GPU twin comparison and
edge inputs, at 8 to 12 bits and in every chroma layout), each scored with the
host's instruction set and with every flag masked, all four outputs compared
bit for bit. Check a build on your own content with:

```bash
# x86: --cpumask 63 masks every flag; aarch64: --cpumask 3
for m in 0 63; do
  vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --cpumask $m \
       --no_prediction --feature psnr_hvs --precision max --json -o mask$m.json
done
diff <(grep -v '"fps"' mask0.json) <(grep -v '"fps"' mask63.json)
```

## Agreement with Netflix's `psnr_hvs`

The score of this fork is Netflix's, bit for bit. Measured against Netflix
master (`9e48141b`, GCC 16.2.1, glibc 2.44) through the C API at `%.17g`, with
every instruction-set flag masked and with the host's dispatch, on 319 frames
from 8x8 to 3840x2160 at 8 to 12 bits in every chroma layout: `psnr_hvs`,
`psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr` are identical on every frame.

What still differs from Netflix's extractor is behaviour, not arithmetic:

- This fork refuses input above 12 bits (Netflix's returns no score and no
  error).
- This fork scores the luma plane of 4:0:0 input (Netflix's refuses the
  format).

### The mask product

The product of the two `float` factors of the masking threshold is a `float`
product: it is rounded to `float` before the square root widens it. That is
Netflix's statement, and all three functions write it
([ADR-1488](../adr/1488-psnr-hvs-upstream-mask-product.md)).

## GPU twins

The CUDA (`psnr_hvs_cuda`), HIP (`psnr_hvs_hip`) and SYCL (`psnr_hvs_sycl`,
[ADR-1369](../adr/1369-sycl-shared-planes-light-twins.md)) twins return the
CPU's scores bit for bit, on every output, at every frame size and depth. The
Metal twin (`integer_psnr_hvs_metal`) now has the same design, ported the way
[ADR-1498](../adr/1498-metal-twins-exact-designs.md) ports the other Metal
twins: it stores every term, the host adds them in the CPU's order, and the
masking table is the CPU's. Its arithmetic is checked against the CPU on the
host (`test_metal_psnr_hvs_math`); it counts as exact once a macOS tester
report shows `test_metal_integer_psnr_hvs_parity` passing on an Apple GPU
(`T-METAL-PSNR-HVS-PER-BLOCK-SUM-FP32-MASK-2026-10-05` in
[the state ledger](../state.md)). Before that change it summed each block on
the device and formed the masking table in `float`, and an Apple M4 Pro
measured it up to 1.7e-3 dB away from the CPU.

| Twin | Exact vs CPU | ADR | Evidence (fragment) | Masking threshold (CPU: `double` root of a `float` product) | Measured on |
|---|---|---|---|---|---|
| `psnr_hvs_cuda` | Yes | [ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md) | `scripts/ci/exact_twins.d/psnr_hvs.cuda` | `float` product, `double` root | RTX 4090 |
| `psnr_hvs_hip` | Yes | [ADR-1401](../adr/1401-psnr-hvs-sycl-hip-exact-twins.md) | `scripts/ci/exact_twins.d/psnr_hvs.hip` | `float` product, `double` root | gfx1036 (integrated) |
| `psnr_hvs_sycl` | Yes | ADR-1401 | `scripts/ci/exact_twins.d/psnr_hvs.sycl` | No fp64 in the kernel: `float` product and a correctly rounded `float` root, which is the `double` root rounded to `float` | Arc A380 |
| `integer_psnr_hvs_metal` | By design; waits for a device report | [ADR-1498](../adr/1498-metal-twins-exact-designs.md) | none yet | No `double` in the kernel: `float` product and Metal's `sqrt`, correctly rounded without fast math, as on SYCL; masking table formed in `double` on the host | not yet measured after the change |

Each exact twin was measured against the CPU at `--precision max` on the
Netflix 576x324 pair (8, 10 and 12 bits, 4:2:0 and 4:2:2), the 1920x1080
checkerboard pairs and Big Buck Bunny at 1920x1080 and 3840x2160 (8 and 10
bits): every frame of `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and
`psnr_hvs_cr` has the same bits.

### Sample conversion

The twins read the raw integer samples of the device pictures at every
supported depth, 8 to 12 bits, eliminating host float conversions and
redundant pinned host staging allocations.

- Two threads share each 8x8 block, one per image.
- The DCT runs in shared (local) memory, and one launch covers every plane.
- Samples at 9 and 11 bits are scored as they are, like every other depth. The
  twins that converted on the host scored them wrongly.
- All three twins take `enable_chroma`. With `enable_chroma=false`, or for
  4:0:0 input, they stage, dispatch and score the luma plane only and emit
  `psnr_hvs_y` and `psnr_hvs`, as the CPU does.

### How the sum stays exact

The CPU extractor adds every masked coefficient error of a plane to one running
`float`: 64 terms per block, about 10.8 million for a 3840x2160 luma plane. The
rounding of each addition depends on the sum so far, so the score depends on
the order of the additions.

The CPU's value is the reference, not the most accurate one: it is up to
1.1e-2 dB above the same sum taken in `double` on 3840x2160 content. It stays
as it is because the Netflix golden values of
`python/test/third_party/xiph/vmafexec_feature_extractor_test.py` pin it.

The kernel therefore stores the 64 terms of every block, computed in the CPU's
arithmetic, and the host adds them in the CPU's order.

### Check a twin on your device

Replace `cuda` by `sycl` or `hip`:

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

Run both sides with the same `vmaf` binary. The dB value goes through the
host's `log10`. A binary built with oneAPI `icx` before
[ADR-1495](../adr/1495-icx-system-libm.md) called Intel's `libimf` and
differed from a gcc build (glibc) by one unit in the last place on a few
frames, on the CPU extractor and on the twins alike; an icx build now calls
glibc's `log10` too.

### Cost of the exact sum

A twin reads 256 bytes per block back to the host (65 MB for a 3840x2160 4:2:0
frame, 259 MB at 7680x4320, held on the device and in host memory) and adds the
terms on one host thread. Milliseconds per frame, with
`--backend cpu --threads 16` for comparison:

| Frame size | `psnr_hvs_cuda`, RTX 4090 | `psnr_hvs_sycl`, Arc A380 | `psnr_hvs_hip`, gfx1036 | CPU, 16 threads |
|---|---|---|---|---|
| 576x324 | 0.29 | 0.61 | 0.65 | 0.16 |
| 1920x1080 | 3.1 | 9.2 | 8.7 | 1.7 |
| 3840x2160 | 12.2 | 35.9 | 37.9 | 6.7 |

The CUDA column is from
[Research-1397](../research/1397-psnr-hvs-twins-cpu-float-sum.md), the others
from [Research-1401](../research/1401-psnr-hvs-sycl-hip-exact-twins.md), on a
Ryzen 9 9950X3D that other work shared (the integrated gfx1036 varied between
31 and 45 ms at 3840x2160).

!!! note "Every twin is slower than the CPU threads here"
    Use `--backend cpu` when throughput matters more than keeping the frame on
    the device. `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` and
    `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` in
    [docs/state.md](../state.md) track the tuning.

## History

- **ADR-1488, the mask product.** Between May and October 2026 the scalar and
  the AVX2 function of this fork widened the first factor of the mask product
  to `double`, which keeps the product exact and puts the threshold one `float`
  step off on about one block in twenty of real content. Most of those
  differences vanish in the running sum, so a frame differed only now and then.
- **ADR-1488, effect on scores.** With the `double` product, 27 of the 319
  frames differed in `psnr_hvs` (9, 9 and 10 in the three plane scores) from
  Netflix's, by at most 9.4e-7 dB; a value printed with the default `%.6f`
  changed in its last digit on some of them.
- **ADR-1488, NEON.** Until 2026-10-02 the NEON function still had the `float`
  product, so an aarch64 build differed from an x86-64 build by the same
  amount.
- **ADR-1397 and ADR-1401.** The GPU twins gained the CPU's sum order and
  returned the CPU's scores bit for bit.
- **ADR-1495.** An icx build calls glibc's `log10` instead of Intel's
  `libimf`.

## See also

- [Features](features.md) - full feature extractor reference
