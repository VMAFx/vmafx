- HISS-21 burn-down, `core/src` SIMD/GPU slice: the SYCL runtime
  (`core/src/sycl/common.cpp`) no longer trips HISS-01 or HISS-04.
  `vmaf_sycl_shared_frame_init` drops its two `goto fail` jumps for a
  `sycl_shared_frame_release()` cleanup owner whose body is the former
  `fail:` block verbatim, so every error exit frees the same buffers in the
  same order (`ref[0]`, `dis[0]`, `ref[1]`, `dis[1]`, each null-guarded).
  `vmaf_sycl_state_init`, `vmaf_sycl_shared_frame_upload` and
  `vmaf_sycl_graph_submit` are split into same-translation-unit `static`
  helpers that preserve enqueue order on the in-order queues and keep every
  throwing call inside its original `try` block. No feature kernel, SIMD
  intrinsics path or scoring arithmetic was touched, so VMAF scores are
  unchanged; the oversized AVX2 / AVX-512 / NEON / SVE2 kernels remain
  deliberately unsplit under their ADR-0138 / ADR-0139 bit-exactness
  invariants.
