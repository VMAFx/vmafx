Fix four errno defects identified in PR #125 code review:

- `vmaf_write_output_with_format`: capture `errno` immediately after
  `open(2)` / `fdopen(3)` failure; return `-errno` instead of hardcoded
  `-EINVAL` so callers receive the OS-precise error code (e.g. `-EACCES`,
  `-ENOENT`).
- `vmaf_cuda_state_init`: map driver-library-missing to `-ENOSYS` and
  `cuInit(0)` failure to `-ENODEV`, mirroring the SYCL/HIP convention.
- `vmaf_close`: propagate return values from `vmaf_thread_pool_wait()` and
  `vmaf_framesync_destroy()` instead of silently discarding them (CERT
  ERR33-C / Power-of-10 rule 7). NULL-pool case returns 0 (single-threaded
  init path).
- `vmaf_cuda_preallocate_pictures`: return `-EBUSY` on double-call to
  prevent silent ring-buffer leak.
- `vmaf_init`: return the actual sub-init error code rather than a
  hardcoded `-ENOMEM` for every failure path.
