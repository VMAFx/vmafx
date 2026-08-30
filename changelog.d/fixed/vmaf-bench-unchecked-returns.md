- `core/tools/vmaf_bench.c`: harden three classes of unchecked
  return values flagged by the 2026-05-30 S9 (JPL Power-of-10 r7)
  audit.
  - `yuv_pair_read_frame`: the two `fseek` calls were `(void)`-cast,
    silencing the lint but hiding a real semantic bug — a failed
    `fseek` leaves the FILE position undefined, after which `fread`
    silently feeds the wrong bytes into the benchmark. Now checks
    each `fseek` and returns `-EIO` with a `perror` diagnostic on
    failure.
  - `run_sycl_gpu_profile` (HAVE_SYCL): the per-frame
    `vmaf_picture_alloc` returns were discarded. On allocation
    failure the subsequent `yuv_pair_read_frame -> ref->data[0]`
    dereference would crash on a sentinel-zero `VmafPicture`. Now
    captures the return code, logs, unrefs any already-allocated
    sibling, and breaks the frame loop.
  - `run_sycl_gpu_profile` (HAVE_SYCL) end-of-stream block: the
    final `vmaf_read_pictures(vmaf, NULL, NULL, 0)` flush surfaced
    pooling / aggregation errors via its int return that the
    previous code discarded; now captured and propagated. `printf`
    / `vmaf_close` returns explicitly `(void)`-cast to match the
    surrounding file convention.
  - Independent of PR #304 (vmaf.c / vmaf_bench.c CUDA/SYCL
    state-leak fix); no overlapping hunks. No behavior change on
    success paths.
