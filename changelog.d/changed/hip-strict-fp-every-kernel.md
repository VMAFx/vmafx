- **Every HIP kernel is built with contraction off (ADR-1407).** hipcc fuses
  `a * b + c` into one multiply-add for device code by default, and all but
  three HIP kernels were built that way, so they rounded differently from the
  CPU extractors they mirror. One flag list, `hip_strict_fp_args`
  (`-ffp-contract=off`, `-fhip-fp32-correctly-rounded-divide-sqrt`), now
  applies to every kernel, and the per-kernel flag table is gone. On a gfx1036
  `float_adm_hip` moves from 2.5e-5 to 2.5e-6 from the CPU on the Netflix
  576x324 pair and `float_ssim_hip` from 1.8e-7 to 1.2e-7; `float_vif_hip`'s
  worst frame moves from 2.7e-5 to 3.8e-5 with the same mean; twelve twins
  produce bit-identical output; every twin stays inside its parity tolerance.
  `float_vif_hip` is 4% slower at 3840x2160 and no other twin changes
  measurably. `test_hip_fp_arith_contract` checks the arithmetic on the
  device and `test_hip_strict_fp_policy.py` the build files.
