<!-- markdownlint-disable MD013 MD060 -->
# Research-1426: Why ciede_cuda was 1e-5 from the CPU — fp32 where the CPU is fp64, and what is left once that is fixed

- **Status**: Active
- **Workstream**: [ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md), [ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-10-01

## Question

`ciede_cuda` was up to 1.1e-5 from `--backend cpu` and its gate tolerance is
`5e-3`. Is that the transcendentals, as the kernel's header said, or the
kernel's own arithmetic? How close can the twin get, what exactly keeps it
from being identical, and what does the CPU's arithmetic cost on the device?

## Sources

- CPU: `core/src/feature/ciede.c` (`get_lab_color()`, `ciede2000()`,
  `get_r_sub_t()`, `extract()`); the AVX2 / AVX-512 preprocess only converts
  samples to float.
- CUDA: `core/src/feature/cuda/integer_ciede_cuda.c` and
  `integer_ciede/ciede_score.cu` at master `5c8b9e9c7` (before) and on
  `fix/cuda-ciede-cpu-arithmetic` (after).
- Reference workstation: RTX 4090, CUDA 13.4 (`nvcc` V13.4.92), gcc 16.2.1, glibc 2.44,
  Ryzen 9 9950X3D. `meson setup build-cuda core -Denable_cuda=true
  -Denable_sycl=false --buildtype=release -Db_lto=false`.
- Fixtures, 4:2:0, `--precision max`: the Netflix pair at 8 bits (48 frames)
  and at 10, 12 and 16 bits (3 frames each), both 1920x1080 checkerboard
  pairs (3 frames each), BBB 3840x2160 (50 frames; 200 through the gate).

## Findings

### 1. The reference is fp64 with float stores

`get_lab_color()` takes doubles and stays in fp64 through the YUV scaling,
the BT.709 matrix, `pow(x, 2.4)`, the XYZ matrix and `pow(c, 1.0 / 3.0)`;
`xyz_to_lab_map()` returns `float`, and the three L*a*b* components are
floats. `ciede2000()` declares nearly every intermediate `const float`, but
the expression that initialises it is fp64 wherever a double literal or a
libm call takes part: `sqrt(pow(a, 2) + pow(b, 2))`,
`1. + 0.045 * c_bar_prime`, `atan2(x, y)` with float arguments. Three things
are genuinely float: the differences and means of floats (`delta_l_prime`,
`l_bar`, `c_bar`), the three final quotients (`lightness`, `chroma`, `hue`),
and `get_r_sub_t()`'s `powf(c, 7) / (powf(c, 7) + powf(25., 7))` and
`powf(degrees, 2)`. `degrees_to_radians()` takes a `float`, so
`60.0 * exp(...)` is rounded on the way in. `extract()` adds the float of
every pixel into one double in raster order and reports
`45 - 20 * log10(sum / (w * h))`.

The old kernel was fp32 from the first operation, called `powf`, `cbrtf`,
`atan2f`, `sinf`, `cosf` and `expf`, kept the hue in degrees, used
`7.787 * t + 16 / 116` for the linear branch of the L*a*b* map, and added per
warp and per 16x16 block in fp32 with the blocks added in double on the host.

### 2. Before and after

| Fixture | Identical before | Largest before | Identical after | Largest after |
|---|---:|---:|---:|---:|
| Netflix 576x324, 8 bit | 0 / 48 | 1.1e-5 | 47 / 48 | 6.9e-13 |
| Checkerboard 1 px | 0 / 3 | 8.6e-7 | 3 / 3 | 0 |
| Checkerboard 10 px | 0 / 3 | 8.6e-7 | 3 / 3 | 0 |
| BBB 3840x2160 | 0 / 50 | 1.5e-6 | 0 / 50 | 1.4e-11 |
| Netflix 10, 12, 16 bit (each) | 0 / 3 | 9.4e-6 | 3 / 3 | 0 |
| Total | 0 / 113 | 1.1e-5 | 62 / 113 | 1.4e-11 |

Through the gate, on all 200 BBB frames: none identical, largest 1.4e-11.

The old reduction alone, applied to the new kernel's per-pixel values of BBB
frame 0 (float sums per warp and block, blocks added in double), moves the
score by 1.4e-9. The rest of the old distance was the fp32 arithmetic; it was
not broken down further, because the old kernel evaluated another form of
the formula and has no single-operation difference from the reference.

### 3. What is left, pixel by pixel

The device's per-pixel floats for BBB frame 0 were written to a file and
compared with a host replay of `ciede_device.h` (the same header, with
glibc), once with glibc's `powf` and once with the correctly rounded power
the device uses.

| Comparison over 8 294 400 pixels | Pixels that differ |
|---|---:|
| device against the replay with glibc's `powf` | 38 |
| of those, device equal to the replay with the correctly rounded `powf` | 38 |
| device against the replay with the correctly rounded `powf` | 1 |
| replay with glibc's `powf` against replay with the correctly rounded one | 39 |

The largest relative difference of a pixel is 1.2e-7, one float step. So 38
pixels differ because glibc's `powf` is not correctly rounded, and one
because an fp64 function of the device rounds the other way across a float
boundary.

glibc's `powf` against the correctly rounded value, over 20 million random
arguments in the ranges `get_r_sub_t()` uses: `powf(x, 7)` for `x` in
`[0.01, 150]` differs in 13 157 (0.066 %), `powf(x, 2)` for `x` in
`[-11, 3.4]` in 32 162 (0.16 %). `(float)pow((double)x, 7.0)` equals the
correctly rounded value in all of them. `pow((double)x, 2)` equals the exact
fp64 square of a float `x` in all of them, which is why the header writes the
squares as products.

One float step in one pixel moves the score by
`(20 / ln 10) * 2^-23 * (value / sum)`, at most
`8.7 * 1.2e-7 * (value / mean) / pixels`: 5.6e-12 times the pixel's weight at
576x324 and 1.2e-13 at 3840x2160. On BBB frame 0 the 39 pixels change the
sum by 3.9e-6 of 1.7e7 and the score by 1.9e-12; the largest difference over
200 frames is 1.4e-11.

### 4. Cost

Against master `5c8b9e9c7`, host load average 14 to 19, `(t(52) - t(2)) / 50`
on BBB and `(t(48) - t(2)) / 46` on the Netflix pair, seven alternating
pairs:

| Fixture | Before | After | Paired difference |
|---|---:|---:|---:|
| 3840x2160 | 2.79 ms | 32.69 ms | +29.96 (+28.81 to +32.51) |
| 576x324 | 0.34 ms | 0.74 ms | +0.28 (+0.22 to +0.86) |

The CPU extractor on the same host: 2 644 ms per 4K frame on one thread,
222 ms on sixteen.

Per pixel pair the kernel now calls `pow` fifteen times (six `x^2.4`, six
cube roots, one `x^7`, two for the float powers), `atan2` twice, `sin`
twice, `cos` four times and `exp` once, all in fp64, on a device whose fp64
throughput is a small fraction of its fp32 throughput. The host reads back
33 MB per 4K frame and adds 8.3 million values (3.2 ms for a plane of doubles
of that size, timed on its own). Where the 30 ms split between the kernel and
the host was not measured.

### 5. Not done

- **glibc's `powf` on the device.** glibc evaluates `powf` in fp64 from two
  small tables and rounds once. The same evaluation on the device would
  return glibc's float for all but about one argument in 10^8 (the fp64 value
  can differ in its last place when glibc is built with FMA; the float
  rounding hides that unless it straddles a boundary). It would remove 38 of
  the 39 pixels above. It would also make the twin a copy of one libm's
  algorithm.
- **Skipping identical pixels.** When the reference and the distorted pixel
  are the same triple, the two colours are equal and the reference's result
  is exactly 0; no math is needed. Content with unchanged regions would be
  faster; the scores would not change.

## Reproduce

```bash
ninja -C build-cuda
build-cuda/test/test_ciede_device_math          # host replay against the CPU extractor
build-cuda/test/test_cuda_ciede_parity          # needs a CUDA device
python3 core/test/test_cuda_ciede_exact_contract.py
python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build-cuda/tools/vmaf \
    --reference testdata/bbb/ref_3840x2160_200f.yuv \
    --distorted testdata/bbb/dis_3840x2160_200f.yuv \
    --width 3840 --height 2160 --features ciede --backends cpu cuda
```
