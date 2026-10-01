<!-- markdownlint-disable MD013 MD060 -->
# Research-1397: Making a GPU psnr_hvs twin return the CPU's bits — what differs, what it costs, and how to tune it

- **Status**: Active
- **Workstream**: [ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md), [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)
- **Last updated**: 2026-10-01

## Question

`psnr_hvs_cuda` failed the ADR-1361 tolerance at 3840x2160 although it is the
more accurate of the two implementations
(`T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30`). The maintainer decided that the
twins copy the CPU's accumulation. Three questions followed:

1. What exactly does the CPU accumulate, and what does a twin have to
   reproduce besides the order of the sum?
2. Is bit-identity reachable on a device, and at which frame sizes and depths
   does it hold?
3. What does it cost, where does the time go, and what would recover it?

## Sources

- `core/src/feature/third_party/xiph/psnr_hvs.c` (`calc_psnrhvs()`, `extract()`),
  `core/src/feature/x86/psnr_hvs_avx2.c` (the path this host runs; its float
  accumulations are scalar and uncontracted, bit-identical to the scalar
  reference), `core/src/meson.build` (strict floating-point arguments).
- `core/src/feature/cuda/integer_psnr_hvs_cuda.c` and
  `integer_psnr_hvs/psnr_hvs_score.cu` at master `c7f28317f` (before) and on
  `fix/psnr-hvs-twins-cpu-float-sum` (after).
- Host `zeus`: RTX 4090 (sm_89, driver 615.71.09), CUDA 13.4 (`nvcc` V13.4.92),
  Ryzen 9 9950X3D, gcc 16.2.1, glibc 2.44, release build, `-Db_lto=false`.
  2026-10-01. Other sessions shared the CPU (load average 5 to 11); the GPU was
  held under the device lock.
- Fixtures: the Netflix 576x324 pair at 8, 10 and 12 bits and in 4:2:2 10-bit
  (`python/test/resource/yuv/`), the two 1920x1080 checkerboard pairs (8-bit,
  and converted to 10-bit with ffmpeg), 24 frames of BBB 3840x2160
  (`testdata/bbb`, 8-bit; a 10-bit conversion whose distorted side carries
  ffmpeg `noise=alls=6:allf=t`), and their bicubic 1920x1080 downscales.

## Findings

### 1. The CPU sum is one float per plane, and four things feed it

`calc_psnrhvs()` runs `ret += (err * csf) * (err * csf)` once per coefficient
with `ret` a `float`, over the blocks in raster order and the 64 coefficients
of a block row-major, then `ret /= pixels` and `ret /= samplemax * samplemax`,
both in `float`. `extract()` combines the plane values in `double`
(`0.8 Y + 0.1 (Cb + Cr)`) and converts each to dB.

The previous CUDA twin differed from that in four ways:

| Difference | CPU | Previous twin |
|---|---|---|
| Accumulation | one running `float` over all terms of a plane | 64 terms per block on the device, then the blocks on the host |
| Masking table | `(csf * 0.3885746225901003) * (csf * 0.3885746225901003)` in `double`, stored as `float` | the same product in `float` |
| Masking threshold | `sqrt((double)s_mask * s_gvar) / 32.f`, stored as `float` | `sqrtf(s_mask * s_gvar) / 32.f` |
| Contraction | none (`-ffp-contract=off`) | nvcc's default fuses a multiply with the following add |

The contrast-sensitivity constants are not a fifth: the CPU initialises
`float` tables from double literals and the kernel writes the same digits with
an `f` suffix, and for all 192 of them the two conversions give the same
`float`.

### 2. Bit-identity holds on every fixture

`--precision max`, `psnr_hvs` on `--backend cpu` against `psnr_hvs_cuda`;
largest absolute difference over the frames and the number of frames that
differ in at least one bit.

| Fixture (frames) | Before: `psnr_hvs_y` | Before: `psnr_hvs` | Before: frames differing | After |
|---|---|---|---|---|
| Netflix 576x324 8-bit (48) | 8.37e-5 | 8.03e-5 | 48 | identical |
| Netflix 576x324 10-bit (3) | 4.90e-5 | 4.76e-5 | 3 | identical |
| Netflix 576x324 12-bit (3) | 3.48e-5 | 3.33e-5 | 3 | identical |
| Netflix 576x324 4:2:2 10-bit (48) | 6.69e-5 | 6.51e-5 | 48 | identical |
| Checkerboard 1 px 1920x1080 8-bit (3) | 1.71e-3 | 1.71e-3 | 3 | identical |
| Checkerboard 10 px 1920x1080 8-bit (3) | 7.41e-4 | 7.41e-4 | 3 | identical |
| Checkerboard 1 px 1920x1080 10-bit (3) | 2.95e-4 | 2.95e-4 | 3 | identical |
| Checkerboard 10 px 1920x1080 10-bit (3) | 4.50e-4 | 4.50e-4 | 3 | identical |
| BBB 1920x1080 8-bit (24) | 1.38e-3 | 1.31e-3 | 24 | identical |
| BBB 1920x1080 10-bit (24) | 1.73e-3 | 1.45e-3 | 24 | identical |
| BBB 3840x2160 8-bit (24) | 1.10e-2 | 1.05e-2 | 24 | identical |
| BBB 3840x2160 10-bit (24) | 1.66e-2 | 1.45e-2 | 24 | identical |

"Identical" means every one of `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and
`psnr_hvs_cr` has the same bits on every frame. The parity gate, run on all 200
frames of BBB 3840x2160, reports a maximum difference of 0. Before, two
1920x1080 results (1.71e-3 and 1.73e-3) exceeded the ADR-1361 tolerance for
that size (1.67e-3), as both 3840x2160 ones exceeded theirs (3.34e-3).

### 3. Each arithmetic difference matters on its own

With the CPU-order sum in place, restoring one of the other three differences
at a time breaks identity on a few frames (largest difference, frames
differing in `psnr_hvs_y` / `psnr_hvs_cb` / `psnr_hvs_cr`):

| Restored | Netflix 576x324 (48) | BBB 1920x1080 (24) | BBB 3840x2160 (24) |
|---|---|---|---|
| nvcc contraction (`--fmad` default) | 4.9e-7, 2 / 4 / 1 | 5.0e-7, 1 / 0 / 1 | 8.5e-7, 3 / 0 / 0 |
| float square root in the threshold | 4.4e-7, 1 / 1 / 2 | 5.0e-7, 0 / 0 / 1 | 3.6e-7, 1 / 0 / 0 |
| float masking table | 7.4e-7, 4 / 1 / 5 | 5.4e-7, 1 / 2 / 0 | 4.6e-7, 2 / 1 / 0 |

They are two to four orders of magnitude smaller than the effect of the
summation order, and invisible at the default `%.6f` precision on most frames,
which is why the gate runs an exact cell at `--precision max`.

### 4. The CPU's error is a bias

A CPU build whose accumulator is `double` (the terms still `float`) gives, on
the same frames:

| Fixture | CPU float against CPU double, `psnr_hvs_y` | Previous twin against CPU double |
|---|---|---|
| Netflix 576x324 8-bit | 8.15e-5 | 9.0e-6 |
| BBB 1920x1080 8-bit | 1.40e-3 | 2.4e-5 |
| BBB 3840x2160 8-bit | 1.10e-2 | 5.1e-5 |
| BBB 3840x2160 10-bit | 1.66e-2 | 5.5e-5 |

The CPU's value is above the double one on every frame of every fixture. On a
3840x2160 luma plane 6.7 to 8.5 % of the terms are nonzero and still leave the
running sum unchanged when added (0.9 % at 576x324): once the sum is large,
its spacing exceeds twice a typical term and the term is rounded away. The
score in dB rises accordingly. ADR-1361 modelled the error as a random walk
(λ·u·√N); the measured error grows faster than that, which is how a twin within
5e-5 dB of the exact sum came to fail a 3.3e-3 dB tolerance. This is why the
exact twin reproduces the CPU's rounding rather than a better value.

### 5. Cost on an RTX 4090

ms per frame, median of three runs of (t(N) − t(2)) / (N − 2) of the `vmaf`
CLI, N = 960 at 576x324 (the 48-frame pair concatenated 20 times), 102 at
1920x1080 and 3840x2160. Values of the last of three passes; the passes agree
within 0.1 ms at the two smaller sizes and 0.5 ms at 3840x2160, except the
first 3840x2160 pass of the previous twin (4.86 ms, cold page cache).

| Frame size | Before | After | CPU, 16 threads |
|---|---|---|---|
| 576x324 | 0.09 | 0.29 | 0.14 |
| 1920x1080 | 0.60 | 3.06 | 1.7 to 2.9 |
| 3840x2160 | 2.36 | 12.16 | 6.5 |
| 3840x2160 10-bit (N = 24) | 6.4 | 11.8 | 6.9 |

The host sum alone, timed around `reduce_hvs_planes()`: 0.14 ms at 576x324,
1.61 ms at 1920x1080, 6.42 ms at 3840x2160 (16.2 million additions, 0.40 ns
each: the loop is one dependency chain, bounded by the latency of a float
add). The rest of the increase is the readback, 256 bytes per block instead of
4: 1.4 MB, 16.2 MB and 64.8 MB per frame at the three sizes. The twin is now
slower than sixteen CPU threads at every size measured.

### 6. Tuning options, none applied

- **Skip zeros.** Only 15 to 21 % of the luma terms and 18 to 32 % of the
  chroma terms are nonzero (masked coefficients are exactly 0), and
  `x + 0.0f == x` for every `x` the sum can hold. Compacting the nonzero terms
  preserves the result. On the host it shortens the chain by a factor of 3 to
  6; on the device (count per block, prefix sum, scatter) it shortens the
  readback by the same factor as well.
- **Order-preserving device reduction.** The terms are non-negative, so the
  sum only grows. While it stays in one binade its float value is an integer
  mantissa `m` times a fixed unit, and adding a term adds a fixed integer to
  `m`, except that an exact tie rounds to even and so depends on the parity of
  `m`. A run of terms is therefore a map from the incoming parity to an
  increment, and such maps compose associatively, which a parallel reduction
  needs. Binade changes (a few dozen per plane) have to be handled where the
  running value is known. This is a sketch: not implemented, not verified.
- **Three independent chains.** The planes' sums do not depend on each other;
  interleaving them in one loop costs the length of the luma chain instead of
  the total (two thirds of it for 4:2:0).

`T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` tracks this.

### 7. What the other twins need

`psnr_hvs_hip` and `psnr_hvs_sycl` have the same four differences on master.
Both are being rewritten (#1658, #1657), so they are left for a follow-up that
calls the same host helpers. The SYCL kernel has one extra constraint: Arc
A-series devices have no fp64 ([ADR-0220](../adr/0220-sycl-fp64-fallback.md)),
so the threshold cannot be written as a double product and square root there.
The product of two floats is exact in 48 bits, so an fp32 pair holds it; the
square root then has to be rounded to double precision first and to float
second, as the CPU does, which `sycl_exact_fp.h` does not provide yet.

## Reproducer

```bash
# Bit-identity on a device, and the device-free guards.
ninja -C build test/test_cuda_psnr_hvs_parity test/test_psnr_hvs_score
./build/test/test_cuda_psnr_hvs_parity && ./build/test/test_psnr_hvs_score
python3 core/test/test_psnr_hvs_twin_exact_sum_contract.py

# The gate's exact cell (tolerance 0, --precision max) at 3840x2160.
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build/tools/vmaf \
    --reference testdata/bbb/ref_3840x2160_200f.yuv \
    --distorted testdata/bbb/dis_3840x2160_200f.yuv \
    --width 3840 --height 2160 --features psnr_hvs --backends cpu cuda
```
