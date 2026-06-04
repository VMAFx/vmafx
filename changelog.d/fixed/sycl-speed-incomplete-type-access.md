- **sycl:** Fix SYCL build failure in `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp`
  where `VmafSyclState::queue` was accessed directly through an incomplete type.
  Replace all 8 direct `s->sycl_state->queue` member accesses with the public API
  `vmaf_sycl_get_queue_ptr(s->sycl_state)` from `sycl/common.h`, which was already
  included but unused for queue access. Restores the Linux GCC SYCL build.
