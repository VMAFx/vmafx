---
paths:
  - core/src/feature/metal/integer_motion_v2_metal.mm
  - core/src/feature/metal/integer_motion_v2.metal
invariant: mv2_mirror is reflect-101, identical across backends (ADR-1176).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# mv2_mirror cross-twin invariant (ADR-1176)

- **mv2_mirror is reflect-101, identical across backends**:
  `integer_motion_v2.metal::mv2_mirror` uses iterated reflect-101
  `idx = (idx < 0) ? -idx : 2 * (sup - 1) - idx`, bit-identical to CPU
  `integer_motion_v2.c::mirror`, CUDA `cuda_tile_index.h::vmaf_cuda_reflect_101`
  (then `vmaf_cuda_tile_index()` clamp, identity for consumed samples, ADR-1372),
  SYCL `integer_motion_pipeline_sycl.cpp::reflect_101`, and HIP
  `motion_v2_score.hip::mv2_mirror`. Never revert to single-bounce or
  `- 1` edge-replicating form.
