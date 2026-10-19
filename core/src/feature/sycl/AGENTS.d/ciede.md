---
paths:
  - core/src/feature/sycl/integer_ciede_sycl.cpp
  - core/src/feature/sycl/sycl_ciede_math.h
  - core/test/test_sycl_ciede_parity.c
invariant: integer_ciede_sycl.cpp stages Y/U/V at native size; statements on fp32 pairs match ciede.c.
---
<!-- markdownlint-disable MD013 MD060 -->
# CIEDE2000 extractor and kernels

- **`integer_ciede_sycl.cpp` stages Y/U/V at native size; kernel
  subsamples chroma** ([Research-2120](../../../../../docs/research/2120-sycl-ciede-throughput.md)).
  `stage_plane()` packs each plane into host USM (`plane_w[p]` x
  `plane_h[p]`, chroma by `picture.c`'s ceil rule `(w + ss) >> ss`),
  one DMA per plane. `ciede_pixel()` reads chroma at
  `(x >> ss_hor, y >> ss_ver)` = nearest-neighbour upsample of
  `ciede.c::scale_chroma_planes`: horizontal from `ss_hor`, vertical
  from `ss_ver` (fork's fixed flags, not upstream's transposed pair).
  Same indexing as CUDA / HIP twins. **On rebase**: do not restore
  host `upscale_plane` (9.5 of 15 ms per 4K frame on Arc B580) and do
  not floor chroma dims. `test_sycl_ciede_parity` pins odd 4:2:0,
  4:2:2 10-bit, 4:4:4 against CPU (cases in
  `core/test/ciede_twin_parity.h`).
- **`integer_ciede_sycl.cpp` = `ciede.c`'s statements on fp32 pairs
  ([ADR-1436](../../../../../docs/adr/1436-sycl-ciede-cpu-arithmetic.md),
  after ADR-1426 for CUDA).** Arithmetic = `../ciede_ff_math.h` + pair
  functions `../ff_math.h`, shared with `ciede_hip` (ADR-1448):
  backend-neutral, no `sycl::` in them. `sycl_ff_math.h` /
  `sycl_ciede_math.h` = SYCL primitives only (`vmaf_ffm_base` =
  `vmaf_sycl_exact`, `VMAF_FF_*` macros -> `sycl::fabs` / `rint` / `sqrt` /
  `cbrt` / `pow(x, 0.2f)` / `ldexp`, `VMAF_FF_INLINE` =
  `VMAF_SYCL_ALWAYS_INLINE`) + namespace aliases `vmaf_sycl_ffm` /
  `vmaf_sycl_ciede`; no function definitions there. change to shared
  header changes both twins: A380 AND gfx1036 parity before merge (move
  measured bit-identical on 178 A380 frames). `ciede_ff_math.h` mirrors
  `../cuda/integer_ciede/ciede_device.h` function for function: fp64 of
  reference = `Ff` pair (48 bits), fp64 libm call = pair function of
  `sycl_ff_math.h` (`sqrt`, `cbrt`, `pow_2_4`, `pow_7`, `exp`, `sin_cos`,
  `atan2`; 2^-44 or better), `float` of reference = float, rounded
  from pair at reference's statement. `powf(x, 7)` = correctly
  rounded (glibc's is not); float square = product, as `ciede.c`
  (ADR-1467). reference's two float products (ADR-1476) are float
  here: `sqrt(from_float(c_prime_1 * c_prime_2))` and
  `add_f(squares, rotation * chroma * hue)`; `two_prod()` there = exact
  product = fork's form after PR #552, not upstream's. Constants: `make_constants(bpc)`
  on host from reference's own expressions, by value into
  kernel. Tables (`kAtanTable`, `kSinCosTable`): generated
  (`scripts/dev/gen_sycl_ff_math.py --write`), copied to device memory at
  first submit, read through pointer; NEVER index constant array
  with run-time value in kernel (scratch, ADR-1395). `ciede_pixel()`
  carries `__attribute__((flatten, always_inline))` and headers'
  functions `VMAF_SYCL_ALWAYS_INLINE`: without, some stay calls,
  frames are scratch (3.4 KiB) and A380 scores 27 dB off. Kernel
  stores one float per pixel at its raster position; host
  `ciede_frame_sum()` (`../ciede_frame_sum.h`, shared with CUDA and HIP
  hosts) = `extract()`'s double sum, score
  `45. - 20. * log10(sum / (w * h))`. Never: device fp32 `pow` / `cbrt` /
  `atan2` / `sin` / `cos` / `exp` for result, `7.787 t + 16 / 116`
  (that line alone was 1.12e-5), device reduction. NOT exact: A380 vs
  GCC CPU 47 of 48 Netflix frames identical, BBB 4K within 1.4e-11 (18 of
  8.3 M pixels per frame, all glibc `powf`); gate `LIBM_TWINS` `sycl` =
  1e-9. Pair quotient / root = `vmaf_sycl_ffm::div()` / `sqrt()` on
  device's own `/` and `sycl::sqrt` (second partial result comes from
  exact residual; `ff_div()` + `sqrt_rn()` there = 82 ms, same values).
  reference's FLOAT divisions stay `div_rn()`. Kernel shape pinned:
  `CiedeKernel`, SIMD-16, default register file (SIMD-32 = 38 ms but 16 KiB
  spills = scratch; grf 256 slower). Cost 50.3 ms per 4K frame (16.2
  before): `T-SYCL-CIEDE-EXACT-THROUGHPUT-2026-10-01`. Upstream change to
  `get_lab_color()` / `ciede2000()` / `get_r_sub_t()` / `extract()`'s sum
  -> this header + CUDA header same PR. Guards: `test_sycl_ciede_math`
  (pair functions vs extended-precision libm; pixel vs fp64 statements;
  host + device), `test_sycl_ciede_parity` (1e-8),
  `test_sycl_ciede_exact_contract.py` (10 planted regressions + generator
  check).

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_ciede_sycl.cpp` | `ciede.c` | `test_sycl_ciede_parity.c` (1e-8, 8 to 16 bit, 4:2:0 / 4:2:2 / 4:4:4), `test_sycl_ciede_math.c` | ADR-0884 (round 2), ADR-1436 |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_ciede_sycl.cpp` | `test_sycl_ciede_parity.c` | ADR-0884 |
