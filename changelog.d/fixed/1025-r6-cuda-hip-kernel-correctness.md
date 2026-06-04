- **CUDA VIF**: fix copy-paste typo in 16-bit vertical filter1d rd-filter
  upper bound — `fwidth_rd - fwidth_rd` (always 0) → `fwidth - fwidth_rd`;
  previously caused OOB reads into `vif_filt.filter[scale+1]` producing
  wrong VIF scores at scales 0-2 (ADR-1025, `r6-cuda-kernel`)
- **CUDA ADM CM**: fix operator-precedence bug — `+ add_shift_sq >> shift_sq`
  was parsed as `+ 0`, dropping the normalisation shift on `accum_thread²`;
  add explicit parentheses matching the CPU reference macro (ADR-1025,
  `r6-cuda-kernel`)
- **HIP ADM decouple**: restore missing `__device__ __forceinline__ uint16_t
  get_best15_from32(uint32_t, int*)` function signature dropped during the
  CUDA→HIP port — bare body block was invalid C++ (ADR-1025, `r6-hip-kernel`)
- **HIP VIF statistics**: fix `wavefront_reduce_i64` carry propagation — change
  bitwise OR reassembly to integer addition so `lo` overflow carries into the
  upper 32 bits correctly (ADR-1025, `r6-hip-kernel`)
