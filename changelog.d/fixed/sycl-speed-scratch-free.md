- **SpEED SYCL kernels are now scratch-free on Intel Arc GPUs (ADR-1395).**
  On the Intel Arc A380 under the Linux `xe` driver, kernel execution that used
  scratch memory (private memory arrays or register spills) caused silent data
  corruption, resulting in singular covariance matrices and zeroed SpEED scores.
  By replacing dynamically-indexed captured plane arrays in `RawPlanes` with
  scalar plane members resolved once per work-item (`RawBound` / `FloatBound`)
  and unrolling bicubic/lanczos weighting loops, all eight SpEED `launch_scale`
  and `launch_decimate` kernels now compile with zero private memory and zero
  register spill (`private_mem_size == 0`, `spill_memory_size == 0`).
  All SpEED parity tests pass, and `speed_gpu_parity.py` achieves bit-identical
  scores (`0.000e+00` max absolute difference) against the CPU reference on
  both 576x324 and 3840x2160 fixtures.
