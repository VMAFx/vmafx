- **Integer ADM gives one result for a non-integer `adm_enhn_gain_limit`,
  whichever code path computes it.** The scalar code bounds a restored sample
  with the double product of the sample and the limit, truncated toward zero.
  The AVX2 and AVX-512 decouple kernels rounded that product to nearest, as
  upstream Netflix/vmaf does, and the SYCL twin formed it in Q31 fixed point,
  so with a limit such as 1.2 each was one off in a share of the samples:
  `integer_adm_scale0` differed from the scalar path by up to 1.2e-6 per frame
  on the Netflix 576x324 pair, 6.2e-6 on a 352x288 pair and 3.6e-5 on blurred
  blocks. The vector kernels now truncate, and the SYCL twin forms the
  truncated double product exactly from 64-bit integer arithmetic. The scalar
  path, AVX2, AVX-512 and `adm_sycl` (Arc A380) give identical output at
  `--precision max` for limits of 1, 1.2, 1.5 and 100 on 20 test pairs and on
  Big Buck Bunny at 3840x2160. Nothing changes at the limits the shipped models
  use (1 and 100), and the Netflix golden gate is unchanged (271 passed, 12
  skipped before and after). `adm_cuda` and `adm_hip` already truncated. The
  Metal twin multiplies in single precision and was not changed, because no
  Apple device was available (ADR-1413,
  `T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`,
  `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`,
  `T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`;
  [features](docs/metrics/features.md)).
