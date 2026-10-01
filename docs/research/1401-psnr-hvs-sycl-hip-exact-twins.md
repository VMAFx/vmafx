<!-- markdownlint-disable MD013 MD060 -->
# Research-1401: Bit-exact psnr_hvs on SYCL and HIP — the CPU's masking threshold without fp64, measurements on an Arc A380 and a gfx1036, and the cost

- **Status**: Active
- **Workstream**: [ADR-1401](../adr/1401-psnr-hvs-sycl-hip-exact-twins.md), [ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md)
- **Last updated**: 2026-10-01

## Question

[ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md) made `psnr_hvs_cuda`
return the CPU extractor's scores bit for bit and left the SYCL and HIP twins
for a follow-up ([Research-1397](1397-psnr-hvs-twins-cpu-float-sum.md) §7).
Three questions:

1. The CPU's masking threshold is a `double` product and square root. How does
   a SYCL kernel without fp64 return the same `float`?
2. Do the two twins reach bit-identity on their devices, at which frame sizes
   and depths, and does the SYCL kernel stay free of scratch memory?
3. What does it cost on an Arc A380 and on a gfx1036?

## Sources

- `core/src/feature/third_party/xiph/psnr_hvs.c` (`calc_psnrhvs()`),
  `core/src/feature/psnr_hvs_score.c` (the shared host sum of ADR-1397).
- `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp` and
  `core/src/feature/sycl/sycl_exact_fp.h`;
  `core/src/feature/hip/integer_psnr_hvs_hip.c` and
  `integer_psnr_hvs/psnr_hvs_score.hip`; before (`origin/master` `c66d28b2f`
  with ADR-1397 applied) and after.
- Host `zeus`, 2026-10-01: Ryzen 9 9950X3D, Linux 7.2.8-1-cachyos, gcc 16.2.1.
  Intel Arc A380 (dg2-g11) on the `xe` driver, compute runtime 26.35.39758.10,
  IGC 2.41.5, oneAPI DPC++ 2026.0.0, JIT build (`-Dsycl_icpx_aot_targets=`).
  AMD gfx1036 (the Ryzen's integrated GPU), ROCm 7.2.4, `hipcc` on AMD clang
  22.0.0git. Release builds, `-Db_lto=false`. Other sessions shared the host
  throughout (load average 12 to 30); each device was held under its lock.
- Fixtures: the Netflix 576x324 pair at 8, 10 and 12 bits and in 4:2:2 10-bit
  (`python/test/resource/yuv/`), the two 1920x1080 checkerboard pairs (8-bit,
  and converted to 10-bit with ffmpeg), 24 frames of BBB 3840x2160
  (`testdata/bbb`, 8-bit; a 10-bit conversion whose distorted side carries
  ffmpeg `noise=alls=6:allf=t`), and their bicubic 1920x1080 downscales (the
  10-bit one with the same noise).

## Findings

### 1. The threshold is the fp32 rounding of an exact square root

`calc_psnrhvs()` computes `s_mask = sqrt((double)s_mask * s_gvar) / 32.f` and
stores it as `float`. Three roundings could take part: of the product, of the
root to `double`, and of that to `float`.

- The product of two `float` values has at most 48 significant bits, so the
  `double` product is exact.
- Write the product as an integer `P` in [2^48, 2^50) times an even power of
  two. Its root lies in [2^24, 2^25), where `float` values are the even
  integers and the midpoints of two neighbours are the odd integers. `P` is
  even (it carries at least one doubling), an odd integer squared is odd, so
  `P` is never the square of a midpoint, and
  `|sqrt(P) - m| = |P - m^2| / (sqrt(P) + m)` exceeds 2^-26 for an odd `m`,
  against a `double` spacing of 2^-28 in that range. Rounding the root to
  `double` first therefore never moves it onto or across a midpoint, and the
  two-step rounding equals one rounding of the exact root to `float`.
- Dividing by 32 changes the exponent only. A block's nonzero product
  `energy * ratio` is above 2^-48 (the smallest nonzero masking energy is one
  squared coefficient times the smallest table entry, about 2^-9; the smallest
  nonzero variance ratio is about 2^-28), so the result is far from the
  subnormal range.

So the kernel needs `RN_float(sqrt(a * b))` with the product taken exactly.
`sqrt_prod_rn()` (`sycl_exact_fp.h`) multiplies the two significands as
`uint64_t`, shifts the product into [2^48, 2^50) with an even exponent, takes
`s = floor(sqrt(P))` with the digit-by-digit integer method (25 steps), and
returns `s` when it is even and `s + 1` when it is odd: an odd `s` is itself
the midpoint and the root lies above it. Operands that are not normal positive
numbers (zero, subnormal, negative, non-finite) take `sqrt_rn(a * b)`, which is
exact when the product is zero, infinite or NaN; a block never produces a
subnormal operand.

Checked against the host expression `(float)sqrt((double)a * (double)b)`:

| Operands | Pairs | `sqrt_prod_rn` differs | `sqrt(a * b)` in `float` differs |
|---|---|---|---|
| Random, all 254 normal exponents, host build of the function | 400 000 000 | 0 | 137 764 267 (34 %) |
| `k (k + 1)`, `k k`, `k (k + 2)` and scaled forms, every 24-bit `k`, host | 50 331 636 | 0 | 10 441 044 (21 %) |
| Random, exponents -20 to 20, on the A380 | 1 048 576 | 0 | not run |
| 13 boundary values cubed, on the A380 | 2 197 | 0 | not run |
| `k (k + 1)`, `k k`, `k (k + 2)`, `k (2 k + 2)`, 65 536 values of `k`, on the A380 | 262 144 | 0 | not run |

`k (k + 1)` is the product whose root is nearest a midpoint: 1 / (8 k) below
`k + 1/2`. The device rows are `test_sycl_fp_arith_contract`.

### 2. Bit-identity on both devices

`--precision max`, `psnr_hvs` on `--backend cpu` against the twin of the same
binary; largest absolute difference over the frames and the number of frames
that differ in at least one bit. Before: `origin/master` `c66d28b2f` with
ADR-1397 applied.

| Fixture (frames) | SYCL before: `psnr_hvs_y` | SYCL before: `psnr_hvs` | HIP before: `psnr_hvs_y` | HIP before: `psnr_hvs` | Before: frames differing | After, both twins |
|---|---|---|---|---|---|---|
| Netflix 576x324 8-bit (48) | 8.37e-5 | 8.03e-5 | 8.37e-5 | 8.03e-5 | 48 | identical |
| Netflix 576x324 10-bit (3) | 4.87e-5 | 4.73e-5 | 4.87e-5 | 4.73e-5 | 3 | identical |
| Netflix 576x324 12-bit (3) | 3.48e-5 | 3.33e-5 | 3.48e-5 | 3.33e-5 | 3 | identical |
| Netflix 576x324 4:2:2 10-bit (48) | 6.76e-5 | 6.57e-5 | 6.69e-5 | 6.51e-5 | 48 | identical |
| Checkerboard 1 px 1920x1080 8-bit (3) | 1.71e-3 | 1.71e-3 | 1.71e-3 | 1.71e-3 | 3 | identical |
| Checkerboard 10 px 1920x1080 8-bit (3) | 7.41e-4 | 7.41e-4 | 7.41e-4 | 7.41e-4 | 3 | identical |
| Checkerboard 1 px 1920x1080 10-bit (3) | 2.95e-4 | 2.95e-4 | 2.95e-4 | 2.95e-4 | 3 | identical |
| Checkerboard 10 px 1920x1080 10-bit (3) | 4.50e-4 | 4.50e-4 | 4.50e-4 | 4.50e-4 | 3 | identical |
| BBB 1920x1080 8-bit (24) | 1.38e-3 | 1.31e-3 | 1.38e-3 | 1.31e-3 | 24 | identical |
| BBB 1920x1080 10-bit (24) | 2.22e-3 | 1.90e-3 | 2.22e-3 | 1.90e-3 | 24 | identical |
| BBB 3840x2160 8-bit (24) | 1.10e-2 | 1.05e-2 | 1.10e-2 | 1.05e-2 | 24 | identical |
| BBB 3840x2160 10-bit (24) | 1.66e-2 | 1.46e-2 | 1.66e-2 | 1.46e-2 | 24 | identical |

"Identical" means every one of `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and
`psnr_hvs_cr` has the same bits on every frame. The parity gate, run on all 200
frames of BBB 3840x2160 and on the Netflix pair, reports a maximum difference
of 0 for `cpu` against `sycl` and for `cpu` against `hip`. For the HIP twin the
3840x2160 10-bit fixture was repeated 125 more times: all 3000 frames equal the
CPU's.

The SYCL kernel uses no scratch memory on the A380:
`kernel_device_specific::private_mem_size` and
`ext::intel::info::kernel_device_specific::spill_memory_size` are both 0 for
the one kernel of the twin, at the compiler's own SIMD16 and with
`IGC_ForceOCLSIMDWidth=32`. `test_sycl_kernel_scratch`
([ADR-1395](../adr/1395-sycl-kernels-no-scratch.md)) audits it with every
other kernel; the twin has no entry in `core/src/sycl/scratch_ratchet.txt`.

### 3. The comparison has to stay inside one binary

The SYCL build compiles the host code with `icx` and links Intel's `libimf`,
whose `log10` is not glibc's. `--backend cpu` of that build and of a gcc build
differ on 3 of the 48 Netflix frames, by one unit in the last place of the dB
value (3.6e-15 or 7.1e-15), and the SYCL twin differs from the gcc build's CPU
extractor on the same 3 frames and from its own build's on none. The plane
sums are equal; the difference is in `10 * -log10(score)`. The parity gate
runs both sides of a cell with one binary, so its equality is not affected.

### 4. Each arithmetic difference matters on its own

With everything else in place on the A380, restoring one difference at a time
(largest difference, frames that differ):

| Restored | Netflix 576x324 (48) | BBB 1920x1080 (24) | BBB 3840x2160 (24) |
|---|---|---|---|
| `sycl::sqrt(energy * ratio)` in `float` for the threshold | 4.4e-7, 3 | 5.0e-7, 1 | 3.6e-7, 1 |
| masking table from a `float` product | 7.4e-7, 9 | 5.4e-7, 3 | 4.6e-7, 3 |

These are the magnitudes Research-1397 §3 found for the CUDA twin. The strict
floating-point line (no contraction, correctly rounded division) was already
on every SYCL feature translation unit
([ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md)); the HIP module
gains `-ffp-contract=off -fhip-fp32-correctly-rounded-divide-sqrt`.

### 5. Cost

ms per frame, median of three runs of (t(N) − t(2)) / (N − 2) of the `vmaf`
CLI (`--backend <b> --no_prediction --feature psnr_hvs`), N = 960 at 576x324
(the 48-frame pair concatenated 20 times) and 102 at the larger sizes. The
before and after binaries ran back to back, four passes in all (three for the
10-bit row); each cell is the median of the passes' medians. The host was
shared (load average 12 to 29), which shows in the HIP figures: the integrated
GPU shares memory with the CPU, and the passes of its 3840x2160 row spread
from 17.6 to 28.7 ms before the change and from 30.6 to 44.9 ms after it. The
SYCL passes agree within 0.8 ms at 3840x2160.

| Frame size | SYCL (A380) before | SYCL after | HIP (gfx1036) before | HIP after | CPU, 16 threads |
|---|---|---|---|---|---|
| 576x324 | 0.41 | 0.61 | 0.39 | 0.65 | 0.16 |
| 1920x1080 | 5.4 | 9.2 | 3.9 | 8.7 | 1.7 |
| 3840x2160 | 22.4 | 35.9 | 18.3 | 37.9 | 6.7 |
| 3840x2160 10-bit | 17.6 | 25.2 | 28.8 | 46.0 | 9.4 |

Both twins were already slower than sixteen CPU threads before the change and
are more so now. The increase has the two parts Research-1397 §5 measured for
CUDA: the host sum (16.2 million additions in one dependency chain per
3840x2160 frame, about 6 ms) and the readback, 64.8 MB per frame instead of
1.0 MB. The tuning options of Research-1397 §6 apply unchanged, because the
sum is the shared helper; for SYCL the integer square root (25 steps per
work-item) is a further candidate, replaceable by a device `sqrt` plus an
integer correction once that is proven.

### 6. Found on the way

- **4:0:0.** `psnr_hvs_sycl` and `psnr_hvs_hip` refuse 4:0:0 input at `init()`;
  the CPU extractor and `psnr_hvs_cuda` score its luma plane.
  `docs/metrics/psnr-hvs.md` claimed luma-only scoring for all three twins and
  is corrected; `T-SYCL-HIP-PSNR-HVS-YUV400-REFUSED-2026-10-01` tracked the
  twins. Fixed on `fix/psnr-hvs-sycl-hip-yuv400`: both score the luma plane,
  `psnr_hvs_hip` takes `enable_chroma`, and the shared test compares 4:0:0
  and `enable_chroma=false` for every twin.
- **A GPU memory access fault on the gfx1036.** One run of the new HIP twin on
  24 frames of the 3840x2160 10-bit fixture, the first after a 3840x2160 8-bit
  run, was killed with `Memory access fault by GPU node-1 ... Page not present
  or supervisor privilege`. The kernel log attributes it to the copy engine
  (`Faulty UTCL2 client ID: SDMA0`), a read (`RW: 0x0`) of ten consecutive
  pages. 131 further runs of the same command did not repeat it, and 126 runs
  of the previous twin did not show it. The twin's copies stay inside their
  buffers (the frame's uploads, one readback of exactly the allocated size);
  the cause is not established, and
  `T-HIP-GFX1036-SDMA-READ-FAULT-2026-10-01` records the evidence next to the
  dropped-dispatch defect of the same device
  (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`).

## Reproducer

```bash
# Bit-identity on a device, and the device-free guards.
ninja -C build test/test_sycl_psnr_hvs_parity test/test_sycl_fp_arith_contract
./build/test/test_sycl_psnr_hvs_parity && ./build/test/test_sycl_fp_arith_contract
ninja -C build-hip test/test_hip_psnr_hvs_parity && ./build-hip/test/test_hip_psnr_hvs_parity
python3 core/test/test_psnr_hvs_twin_exact_sum_contract.py

# The gate's exact cell (tolerance 0, --precision max) at 3840x2160.
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build/tools/vmaf \
    --reference testdata/bbb/ref_3840x2160_200f.yuv \
    --distorted testdata/bbb/dis_3840x2160_200f.yuv \
    --width 3840 --height 2160 --features psnr_hvs --backends cpu sycl
```
