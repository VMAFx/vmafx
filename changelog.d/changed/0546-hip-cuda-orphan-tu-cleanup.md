### chore(feature): delete orphan HIP/CUDA TUs (ADR-0546)

Removed 6 dead/orphan translation units found by a deep audit of
`core/src/feature/hip/` and `core/src/feature/cuda/`:

- `feature/hip/integer_ciede_hip.c` — duplicate of compiled `ciede_hip.c`;
  both defined `vmaf_fex_ciede_hip`.
- `feature/hip/integer_moment_hip.c` — duplicate of compiled
  `float_moment_hip.c`; both defined `vmaf_fex_float_moment_hip`.
- `feature/cuda/float_ssim_cuda.c` — duplicate of compiled
  `integer_ssim_cuda.c`; both defined `vmaf_fex_float_ssim_cuda`.
  The compiled file is the newer, more complete version.
- `feature/hip/adm_hip.c`, `motion_hip.c`, `vif_hip.c` — plumbing stubs
  compiled into the HIP archive but with zero callers in the repo and no
  `VmafFeatureExtractor` registration. The init=0/run=-ENOSYS posture was
  misleading: `init` signalled success while `run` always failed.
- `feature/hip/feature_hip.h` — forward-declared only the above three
  stub triplets; removed with the last of its consumers.

Net removal: ~1444 LOC. No user-visible behaviour change.
