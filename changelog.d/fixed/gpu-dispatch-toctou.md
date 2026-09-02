- Fix `cu_state` resource leak in `core/tools/vmaf.c`: when
  `vmaf_cuda_import_state` fails after a successful
  `vmaf_cuda_state_init`, the allocated `cu_state` is now freed before
  returning -1 (CWE-401). ADR-0840.
- Fix lock-free TOCTOU in `core/src/gpu_dispatch_env.c`: paired
  `atomic_thread_fence(memory_order_release)` / `memory_order_acquire`
  fences ensure `row->value` is visible to readers on weakly-ordered
  architectures (ARM64, POWER) before `row->var_name` is published.
  ADR-0840.
