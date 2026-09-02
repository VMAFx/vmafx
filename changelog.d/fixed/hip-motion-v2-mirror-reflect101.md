- **HIP `integer_motion_v2` mirror padding corrected to reflect-101
  (`2*size-idx-2`).** The HIP kernel used `2*size-idx-1` at the high
  boundary while the CPU reference and the CUDA/SYCL twins all use
  `2*size-idx-2` — a one-pixel cross-backend divergence (same class as the
  HIP VIF fix, ADR-1103). Fixes the kernel and the comment/ADR-0377 claim
  that wrongly asserted the `-1` form matched CPU/CUDA. HIP-only; no CPU
  Netflix-golden impact. (ADR-1106)
