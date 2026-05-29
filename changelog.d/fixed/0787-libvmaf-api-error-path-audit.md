## libvmaf API error-path audit (ADR-0787)

Identified six error-path defects in the libvmaf public C API (Research-0787):
`vmaf_write_output_with_format()` returns `-EINVAL` instead of `-errno` on file-open failure;
`vmaf_cuda_state_init()` returns `-EINVAL` for driver-not-found (should be `-ENOSYS`) and
no-GPU (should be `-ENODEV`); `vmaf_close()` silently drops return values from
`vmaf_framesync_destroy()` and `vmaf_thread_pool_wait()`; `vmaf_cuda_preallocate_pictures()`
lacks the `-EBUSY` double-call guard present in SYCL/Vulkan; `vmaf_cuda_state_free()` has a
mismatched single-pointer/int-return signature vs. all other backend `state_free()` functions.
Fixes for items 1–3, 5–6 are planned for a follow-up implementation PR; item 4 (ABI break)
is deferred to the next major version bump.
