- GPU SpEED (`speed_chroma` / `speed_temporal` on CUDA / HIP / SYCL) now
  produces scores that match the CPU reference. Two algorithm bugs were
  corrected: the means / covariance kernels computed **per-tile block-local**
  statistics instead of the CPU's single **global** covariance over the
  5×5-phase-shifted full-plane submatrix (`means[25]`, not
  `means[25*num_blocks]`) — giving ~7× low scores; and the distorted path
  reused the **reference** covariance + eigenvalue basis via a save/restore,
  so distorted entropy was wrong whenever `ref ≠ dis` (chroma ~2× high). The
  ref and distorted paths now keep **separate** covariance + eigenvalue bases.
  Verified bit-parity (≤ 1e-4) on an RTX 4090 via
  `test_cuda_speed_chroma_parity` + `test_cuda_speed_temporal_parity`.
- Fixed several SpEED safety bugs found in the same audit: a host read of a
  CUDA device pointer in `picture_copy()` that SEGV'd every frame
  (`speed_chroma_cuda` / `speed_temporal_cuda` now download the plane via
  `cuMemcpyDtoH` first); an eigenvalue-scratch heap overflow (`n*n + 3*n` →
  `n*n + 4*n`) in all six GPU SpEED extractors that corrupted the heap during
  GPU extraction; a CPU `speed.c` heap-buffer-overflow from an unchecked
  `speed_init_dimensions()` return underflowing `submatrix_height`; a SYCL
  solve-kernel divergent-barrier deadlock (DEVICE_LOST on Intel Arc); and
  init-OOM resource leaks on the CUDA / HIP error paths.
