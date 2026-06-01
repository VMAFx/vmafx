Bundle of four core C bugfixes applied via PR #148, #307, #308, and #316:

- **#148 errno propagation** (`core/src/libvmaf.c`, `core/src/cuda/common.c`):
  capture `errno` immediately after `open(2)` / `fdopen(3)` failure and return
  `-errno` instead of hardcoded `-EINVAL`; map CUDA driver-missing to `-ENOSYS`
  and `cuInit` failure to `-ENODEV`; propagate `vmaf_thread_pool_wait` /
  `vmaf_framesync_destroy` return values from `vmaf_close`; return `-EBUSY` on
  double-call to `vmaf_cuda_preallocate_pictures`; return the actual sub-init
  error from `vmaf_init` (CERT ERR33-C / JPL Power-of-10 r7).

- **#307 unchecked returns in vmaf_bench** (`core/tools/vmaf_bench.c`):
  check `fseek` x2 in `yuv_pair_read_frame` and `vmaf_picture_alloc` x2 +
  `vmaf_read_pictures` flush in `run_sycl_gpu_profile` (JPL Power-of-10 r7).

- **#308 -ENOSYS stubs for HIP/Metal public API** (`core/src/hip/stubs.c`,
  `core/src/metal/stubs.c`, `core/src/meson.build`): emit `-ENOSYS` fallbacks
  for every `libvmaf_hip.h` / `libvmaf_metal.h` entry point when the backend
  is disabled at build time, mirroring the `dnn_api.c` VMAF_HAVE_DNN pattern.

- **#316 GPU dispatch token-boundary match** (`core/src/feature/cuda/integer_motion.c`
  and related dispatch sources): enforce token boundary on strategy-name matching
  to prevent partial-string false positives in backend selection.
