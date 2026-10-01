- **Reuse caller-supplied `tmpbuf` in scalar VIF fallback filters (`vif_filter1d_s`, `_sq_s`, `_xy_s`) (ADR-0463 / BUG-048 B4).**
  The scalar VIF fallback paths in `core/src/feature/vif_tools.c` now reuse the
  scratch buffer already allocated by `compute_vif` instead of performing
  per-invocation `aligned_malloc` and `aligned_free` calls. This eliminates
  up to 12 dynamic heap allocations per frame on architectures without AVX2
  float convolution (such as ARM64 and fallback CPU paths) while keeping scores
  100% bit-identical.
