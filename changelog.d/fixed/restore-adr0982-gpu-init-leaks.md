- **GPU partial-init leak fixes restored (ADR-0982 / BUG-048 Sec A3)**:
  Restored error-path unwinds across CUDA and SYCL runtimes that were silently
  reverted by PR #504 after landing in #503:
  - `core/src/cuda/common.c`: `vmaf_cuda_release` now releases the dlopen'd `CudaFunctions`
    table on failure paths (`fail_release_funcs`).
  - `core/src/cuda/drain_batch.c`: `drain_stream_ensure` destroys `drain_str` if
    `cuCtxPopCurrent` fails after stream creation (`fail_after_stream`).
  - `core/src/cuda/picture_cuda.c`: `vmaf_cuda_picture_alloc` zeroes `priv` upon
    allocation and safely unwinds previously-allocated device planes (`DEV_PIC_UNWIND_DATA`)
    with null sentinels.
  - `core/src/sycl/common.cpp`: `vmaf_sycl_graph_register` creates the combined compute
    queue before pushing the extractor entry.
  - Companion unit test `core/test/test_cuda_runtime_unwind.c` (8/8 cases pass) exercises
    these error-path unwinds deterministically using a mock driver table.
