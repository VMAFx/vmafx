**fix(core):** C memory-safety bundle — three UAF / double-free / leak fixes
bundled from PRs #317, #133, and #187.

- `core/src/gpu_picture_pool.c`: `vmaf_gpu_picture_pool_init` now sets
  `*pool = NULL` on every failure path so callers cannot UAF via the
  long-lived `VmafContext.cuda.ring_buffer` handle (PR #317).
- `core/src/libvmaf.c`: `read_pictures_dispatch_one` synchronous path
  uses `vmaf_picture_ref` + `vmaf_picture_unref` instead of a bare struct
  copy for `fex->prev_ref`, closing a UAF window when the pool reuses a
  buffer while an extractor is still reading it (PR #133, ADR-0778 Fix-A).
- `core/src/picture_pool.c`: `pool_preallocate_pictures` restructured to
  a two-pass scheme (allocate all, then strip `priv`/`ref`); error-unwind
  now calls `vmaf_picture_unref` on intact pictures instead of leaking the
  data buffer (PR #133, ADR-0778 Fix-E).
- `core/test/test_vif_skip_scale0.c`, `core/test/test_integer_vif_cpu_cuda_parity.c`:
  `VmafFeatureDictionary` callers now let `vmaf_use_feature` / `vmaf_model_feature_overload`
  own the dictionary, eliminating double-free and leak (PR #187, ADR-0806).
