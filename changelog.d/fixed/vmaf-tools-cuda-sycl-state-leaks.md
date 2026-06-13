- `core/tools/vmaf.c` + `core/tools/vmaf_bench.c`: plug four
  GPU-state lifetime defects surfaced by the 2026-05-30 state-leak
  audit.
  - **T1** (`vmaf.c` cleanup label): the SYCL cleanup block guarded
    on `sycl_active`, not the `sycl_state` pointer. A successful
    `vmaf_sycl_state_init` followed by a failing
    `vmaf_sycl_import_state` returns before `sycl_active` is set,
    leaking the just-allocated state. Now mirrors the HIP/Metal
    pointer-gated pattern.
  - **T2** (`vmaf.c` `init_gpu_backends`): `cu_state` was a function-
    local with the only `vmaf_cuda_state_free` call on the import-
    error path — every successful CUDA run leaked it. State now lives
    in `main()` and is freed at the cleanup label after `vmaf_close`,
    matching the SYCL/HIP/Metal lifetime model.
  - **T5** (`vmaf_bench.c` `run_feature_collect`): GPU state pointers
    lived inside `#ifdef` branches and were leaked on every early-
    return path (`vmaf_use_feature`, `yuv_pair_open`, per-frame
    failures) and on the normal-exit path. Hoisted to function scope
    and freed before every return.
  - **L7** (`vmaf.c` output write): the return value of
    `vmaf_write_output_with_format` was discarded, letting a silent
    writer failure leave CI consumers reading a stale/partial output
    file. Capture, log, and propagate the error.
