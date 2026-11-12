---
paths:
  - core/src/sycl/import_*.c
  - core/src/sycl/vmafx_sycl*
  - core/src/sycl/detile.h
  - core/src/sycl/queue_props.h
invariant: VMAFx SYCL frame import, immediate library queue, join() fence ordering, shared detile.h, and semaphore dup.
---
# VMAFx device frames, queue ordering, and semaphore import

## VMAFx device frames on SYCL (RC4 WP3, ADR-2091)

- 4:2:2 / 4:4:4 layouts (ADR-2133): packed (Y210, Y212, YUYV422, Y410 / XV30,
  XV36, VUYX) and MSB planar words = one `vmafx_sycl_rt_frame_gather()` per
  output plane (`launch_gather()` in `vmafx_sycl_rt.cpp`: plan of
  `vmafx_import_plane_read()` / `vmafx_import_read_plane()`,
  `core/src/vmafx/import_convert.h`; change both together; scratch-free,
  `test_sycl_kernel_scratch`). Semi-planar NV16 .. P416 use existing
  de-interleave and shift kernels (they take chroma plane's own size).
  Packed / MSB in GL texture or Intel-tiled dma-buf: refused naming
  `desc.memory` / plane's `modifier`. `test_vmafx_import_sycl_formats`
  (`VfOps` of `core/test/vmafx_format_cells.h`) holds all of it on A380.
- Files: `vmafx_sycl.h` (what `core/src/vmafx/` calls),
  `vmafx_sycl_internal.h`, `import_device.c` (Level Zero GPUs, devices,
  engine attach), `import_frame.c` (checks, plan, conversions, picture
  attach), `import_dmabuf.c` (Linux dma-buf planes, implicit fences),
  `import_gl.c` (GL textures via shared `vmafx/egl_export.c` in keep mode:
  tiled exports stay tiled for device de-tile; HISS-19), `import_fence.c`,
  `import_pool.c`; C++ half = `vmafx_sycl_rt.{h,cpp}` (C ABI, catches every
  exception). C files build into library target; `.cpp` in `sycl_sources`.
- One in-order library queue per device with `immediate_command_list`
  (ADR-3359, `queue_props.h`; `vmafx_sycl_rt_test_omit_immediate()`
  and `VMAFX_TEST_SYCL_OMIT_IMMEDIATE=1` exist for re-evaluating it). Engine
  states of contexts on device live in its SYCL context
  (`vmaf_sycl_state_init_queue()`); `sycl_state_create()` = only place
  building `VmafSyclState`.
- Frame = SYCL device picture (`VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE`,
  `priv->sycl.frame` = `VmafxSyclFrameRt`). Every engine reader copies plane
  with `vmaf_sycl_picture_read_plane()` (kernel on reader's queue after
  frame's ready event; read's event recorded on frame). Never
  `ext_oneapi_memcpy2d()` (behind host task it lost A380 on DPC++ 2026.0 and
  2026.1.1, Research-2160 finding 1), never host read.
- Ordering: `join()` (empty kernel with `depends_on()`) for acquire and
  release, never `ext_oneapi_submit_barrier()`: barrier on event of command
  behind host task returns only after that host task ran (finding 2).
  Release = join over ready + every recorded read; behind it HOST fence's
  host task, SYCL_EVENT slot record, deferred frees
  (`vmafx_sycl_rt_collect()`). Never `ext_oneapi_get_last_event()` in hot
  path: it waits for queue (finding 3).
- SYCL_EVENT release fences = slots of table in `vmafx_sycl_rt.cpp` (4096,
  refcounted): pending until release records join's event; release callback
  runs after record. `vmafx_fence_destroy()` of caller's own event =
  `VMAFX_E_INVALID` (caller's stays caller's).
- De-tile address math lives once in `detile.h`, shared with VA-surface
  import of `dmabuf_import.cpp` (`detile_tiled()`): change to Intel Y-tiled /
  Tile4 handling changes it there, both paths follow. P010 shift fused
  (`shift = 16 - bpc` when bpc > 8). Kernels byte-wise integer, scratch-free
  (ADR-1395): `test_sycl_kernel_scratch` audits every kernel of its program;
  `vmafx_sycl_rt.o` linked into it through `vmaf_sycl_picture_read_plane()`
  (common.cpp): keep that reference, else lane's kernels drop out of audit.
- `WIN32_SHARED` refused (`VMAFX_E_NOTSUP` naming `desc.memory`); SYNC_FILE
  release fences refused. Adding either changes `sycl_info()` in
  `import_device.c`, docs table of `docs/api/vmafx/index.md` and refusal rows
  of `test_vmafx_import_sycl.c` together.
- Vulkan timeline semaphores (ADR-3352, Linux): `acquire` + up to 3
  `acquire_more` imported per frame (`vmafx_sycl_rt_semaphore_import()`,
  DPC++ `external_semaphore_descriptor<resource_fd>`) and waited on library
  queue before first read (`wait_semaphores()` in `import_frame.c`);
  `signal_on_release` timelines (max 3) signalled by `signal_semaphores()`
  behind release join, then freed. Runtime closes fd it imports: always
  `dup()` first (caller keeps its fd, as CUDA). Planted switches
  `VMAFX_TEST_SKIP_ACQUIRE_WAIT` / `VMAFX_TEST_EARLY_RELEASE` must score
  frames wrong in `test_vmafx_import_vulkan_sycl_fence`. OPTIMAL tiling and
  non-semaphore further acquires stay refused by name.
- Guards: `test_vmafx_import_sycl`, `test_vmafx_import_sycl_bitexact`
  (`VMAFX_TEST_ONLY=netflix|bbb`), `test_vmafx_import_sycl_fence`,
  `test_vmafx_import_sycl_gl` (suite `gpu`/`sycl`, Linux, not AdaptiveCpp),
  `test_vmafx_import_sycl_cells_contract.py` (fast: cell table equals
  `scripts/ci/exact_twins.d/*.sycl`).
- Tests never wait on host for producer queue while one of its host tasks
  completes: DPC++ runtime crashed in its host-task cleanup that way (2026.0
  and 2026.1.1, Research-2160 finding 8). Fence test zeroes every buffer
  before its first host task.
