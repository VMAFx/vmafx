- HIP backend: integer ADM now runs end-to-end with **real GPU kernels**
  on AMD ROCm hosts.  ADR-0539 ports the four CUDA-twin kernels
  (`adm_dwt2`, `adm_csf`, `adm_csf_den`, `adm_cm`) to compile standalone
  via `hipcc --genco`, registers them in `hip_kernel_sources`, and
  removes the four ADR-0536 weak HSACO fallbacks that had been silently
  routing `--backend hip --feature adm` to the CPU implementation.
  Bit-exact vs CPU on the Netflix golden src01 pair (diff = 0.000000
  across `integer_adm`, `integer_adm2`, `integer_adm3`, and all four
  `integer_adm_scale[0-3]` features).  Per-thread `atomicAdd` on the
  64-bit unsigned accumulator replaces the CUDA twin's per-warp
  `__shfl_down_sync` reduction (the warpSize=32 mask is wrong on AMD
  wavefronts of 64); the swap is bit-exact since uint64 addition is
  associative and commutative.
