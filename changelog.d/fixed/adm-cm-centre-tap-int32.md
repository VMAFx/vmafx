- **Integer ADM no longer scores isolated impairments above 1.** The scale-0
  contrast-masking threshold narrowed its centre tap to 16 bits, as upstream
  Netflix/vmaf does. A coefficient of 15360 or more wrapped the tap negative,
  and where such a coefficient stood alone the threshold added contrast instead
  of masking it: a flat grey 64x64 reference against the same picture with
  isolated 4x2 patches scored `integer_adm_scale0` 1.0829 and `integer_adm2`
  1.0355 where `float_adm` gives exactly 1. The tap is now 32 bits wide and the
  excess over the threshold is clamped in 64 bits, in the scalar, AVX2, AVX-512,
  CUDA, HIP, SYCL and Metal code (the second revision of Netflix/vmaf PR #1602,
  which upstream has not merged). Both patch pictures now score 1. The Netflix
  golden gate is unchanged (271 passed, 12 skipped before and after) and its 13
  fixture pairs give identical output at `--precision max` on the CPU and on
  the CUDA, HIP and SYCL twins. Scores change only where a scale-0 coefficient
  reaches 15360: independent full-range noise at 576x324 moves `integer_adm2`
  from 0.389548 to 0.389503 and `integer_adm_scale0` from 0.454972 to 0.454801.
  Until upstream merges #1602, integer ADM differs from upstream master on such
  content. The scalar tails of the AVX2 and AVX-512 kernels also no longer
  left-shift a negative threshold, which a UBSan build stopped on for a 24x24
  picture. The Metal change is source only; no Apple device was available
  (ADR-1402, `T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01`,
  `T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01`;
  [features](docs/metrics/features.md)).
