---
paths:
  - core/src/feature/cuda/integer_vif_cuda.c
  - core/src/feature/cuda/integer_vif_cuda.h
  - core/src/feature/cuda/integer_vif/filter1d.cu
invariant: vif_cuda: 16-px minimum, CPU log2 table, names before clearing enable_chroma, stream reset, per-picture pitch.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer VIF minimum size, log2 table, and stream reset

- **`vif_cuda` minimum = 16 pixels** via ADR-1324 gate (`context_check`,
  `context_fallback_name = "vif"`), derived from `vif_filter1d_width` in
  `vif_cuda_min_dim()`. `init()` refuses below it BEFORE reading
  `fex->cu_state` (device-free tests pass none).

## `vif_cuda` reads the CPU's log2 table (ADR-1462)

- CPU `vif` reads `log2_table[]` (host libm, `vif_log2_table_generate()`).
  `vif_cuda` reads SAME values: module global `vif_cuda_log2_table`
  (`integer_vif/vif_statistics.cuh`), `log2_lookup(v)` =
  `table[v & (VIF_LOG2_TABLE_SIZE - 1)]`. NO `log2f` / `roundf` in any vif
  kernel source: device `log2f` != host's (307 of 32768 arguments on CUDA
  13.4 vs glibc 2.44; 77 table entries on gfx1036, ADR-1435).
- Host: `init_fex_cuda()` -> `vmaf_cuda_vif_upload_log2_table()` right
  after module load, before buffers; stages table in device buffer, kernel
  `vif_cuda_log2_table_transfer` copies into global, waits on stream. Failure
  -> `vif_init_unwind()`.
- Transfer = kernel, not `cuModuleGetGlobal()`: ffnvcodec loader binds
  legacy symbol, current-API context answers `CUDA_ERROR_INVALID_CONTEXT`.
- Global, not kernel argument: `filter1d.cu` kernel signatures and
  arithmetic unchanged.
- Guards: `test_cuda_vif_log2_table` (device: table empty before upload,
  all 32768 entries == host after, wrong module refused),
  `test_cuda_vif_log2_contract.py` (eight planted regressions),
  `test_cuda_vif_parity`.

- **`integer_vif/filter1d.cu` 16-bit rd-filter upper-bound guard must use
  `(fwidth - fwidth_rd)`, not `(fwidth_rd - fwidth_rd)` (r6-cuda-kernel / 2026-06-04).**
  Correct guard = `fi < (fwidth - (fwidth - fwidth_rd) / 2)`, matching 8-bit
  form at line 183. Writing `(fwidth_rd - fwidth_rd)` (always zero) widens tap
  window to all `fwidth` taps, causes OOB reads into `vif_filt.filter[scale+1]`.
  On rebase: if vertical-pass loop in 16-bit path modified, verify
  upper-bound guard expression before pushing.

- `integer_vif_cuda.c`: reset on the picture stream (scale 0 kernels run there),
  scales 1-3 run on `s->str` after an event recorded behind reset + scale 0,
  DtoH on `s->str`. Reset on `s->str` raced scale 0 under GPU contention: late
  reset erased the first adds, vif scales wrong with >= 2 instances on one
  device (Netflix/vmaf#1305).

- **`integer_vif/filter1d.cu` kernels = short bodies over `__forceinline__`
  `vif_*` stages** (ADR-1142, HISS-04; the four baselined function-size rows
  are gone). Horizontal 8-bit and 16-bit = one template `vif_hori_kernel`
  (filter row and `__ldg()` as template arguments, rounding and shift as
  arguments); `filter1d_8_horizontal_kernel` / `filter1d_16_horizontal_kernel`
  only instantiate it. Upstream change
  to a kernel body -> port it into the matching stage, never restore a long
  body. Stages are integer-only; same operation sequence as before, so
  verify on device: `test_cuda_exact_twins`, `test_cuda_vif_parity`, `vif`
  gate cell (tolerance 0).

- **`vif_hori_flush_accums()` runs after the per-lane edge branch**
  (T-CUDA-WARP-REDUCE-UB-2026-10-05): `warp_reduce()` shuffles with the full
  mask, so every lane of the warp must reach it; a lane past the plane edge
  flushes its zeroed accumulators. The union of accumulators is declared before
  the branch. `core/test/test_cuda_warp_reduce_contract.py` reports a flush
  inside the branch.

- **Residual variance via `vif_sv_sq()` (ADR-1561).** Kernel defines
  `VMAF_IVIF_FUNC` as `static __device__ __forceinline__` and includes
  `feature/integer_vif_sv_sq.h`; `sv_sq` is `uint32_t`. No raw
  `double` -> `int32_t` conversion of `sigma2_sq - g * sigma12`
  (`test_integer_vif_sv_sq_contract.py`). Header listed in the backend's
  `depend_files` (`core/src/meson.build`).

## `vif_cuda` names its features before it clears `enable_chroma` (ADR-1836)

- `init_fex_cuda()` builds `feature_name_dict` from the options as the caller
  set them, then `vif_drop_vestigial_chroma_option()` clears the no-op
  `enable_chroma`: `enable_chroma=true` gives `integer_vif_scaleN_enable_chroma`
  (scores unchanged). Clearing first gave the default names; every other
  extractor names its features from its options first. A failure after the
  dictionary exists goes through `vif_init_unwind()`, which frees it.
- Guards: `test_gpu_twin_name_order_contract.py` (every CUDA, SYCL, HIP twin;
  no exception), `test_integer_vif_cpu_cuda_parity` (reads the suffixed names
  and refuses the default ones for `enable_chroma=true`),
  `test_cuda_vif_log2_contract.py` (init order ends in
  `return vif_setup_buffers(`).

## `vif_cuda` reads each picture with its own pitch (ADR-2023)

- Scale 0 reads `ref_pic->data[0]` and `dist_pic->data[0]` with
  `VifBufferCuda.stride` and `.dis_stride`, set per frame from the pictures'
  `stride[0]` in `vif_submit_scales()`; `vif_vert_load_tiles()` takes both.
  The init-time value (texture-aligned) is only the engine pool's pitch: an
  imported VMAFx frame has its producer's, and two inputs may differ
  (`T-CUDA-VIF-PICTURE-PITCH-2026-10-06`). Never go back to one pitch.
- The vertical pass loads 4 (8-bit) or 8 (16-bit) bytes per thread: a picture
  row must start 8-byte aligned with the pitch a multiple of 8, which the
  CUDA import requires of bound planes (`core/src/cuda/import_frame.c`).
- Guards: `test_vmafx_import_cuda` (one import, two contexts, luma pitch 368),
  `test_vmafx_import_cuda_bitexact` (`vif` cell, padded pitches).
