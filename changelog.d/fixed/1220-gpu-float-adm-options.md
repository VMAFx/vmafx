- `adm_p_norm` now reaches the CUDA, SYCL, HIP and Metal `float_adm`
  kernels. All four hardcoded the cube sum in the kernel and the `1/3`
  root in the host pooling, applying the option to the AIM exponent
  alone, so a non-default `apn` produced a hybrid quantity: a sum of
  cubes raised to `1/p`, with `adm2` and every `adm_scaleN` sub-score
  left at `p = 3`. Measured at `apn=2.0` against the `1e-4` gate:
  `cpu = 0.43097075` vs `cuda = 0.45416959` on `adm2_apn_2`, a
  `2.32e-02` drift. The kernels mirror the CPU's own `p == 3` fast path,
  so the default path is unchanged.
- `adm_bypass_cm` now reaches the CUDA and Metal `float_adm` kernels.
  Both declared the option and stored it in their state struct, and
  neither read it anywhere — the 3x3 contrast-masking threshold was
  always subtracted, so `bcm=1` was silently ignored. SYCL and HIP do
  not declare the option and reject it, which is unchanged.
- Metal's `adm_skip_scale0` now excludes scale 0 from the pooled `adm2`
  and `aim` scores as `adm.c` does (`num_scale = 0`,
  `den_scale = 1e-10`). It used to zero only the reported
  `adm_scale0` sub-score while still folding the full scale-0
  numerator and denominator into the pooled score.
- Each backend's `float_adm` parity test now carries a variant per
  option it declares, reading the derived `adm2_apn_2` / `adm2_bcm_1`
  keys. The previous tests ran with `NULL` options, where `p = 3` *is*
  the hardcoded exponent and `bypass = 0` *is* the hardcoded behaviour.
  See ADR-1220.
