---
paths:
  - core/src/sycl/common.cpp
  - core/src/sycl/dmabuf_import.cpp
  - core/src/sycl/dispatch_strategy.cpp
  - core/src/libvmaf.c
invariant: Shared luma/chroma plane uploads, synchronous upload_plane, zero-copy luma-only gate, and P010 VA normalization.
---
# Shared planes, zero-copy gate, and P010 VA normalization

## Rebase-sensitive invariants per kernel

- **Shared planes: one upload per plane per frame for every twin
  ([ADR-1369](../../../../docs/adr/1369-sycl-shared-planes-light-twins.md)).**
  Luma: shared frame, uploaded by `read_pictures_sycl_prep` for every run.
  Chroma: opt-in planes in `SyclPlaneState` (`VmafSyclState::planes`, which
  also holds fence markers): `vmaf_sycl_shared_chroma_init` at extractor
  init, idempotent per geometry; `vmaf_sycl_shared_chroma_upload` in
  `submit()`, first chroma reader of frame packs into pinned `staging` and
  uploads into `cur_compute`, rest return 0 on `frame == frame_counter`;
  folded into `last_upload_event`. Never copy chroma straight from
  pageable picture (pitched 2-D copy cost 168 ms per 576x324 frame on
  UHD 770). Runs whose twins read no chroma from these planes (e.g.
  `vmaf_v0.6.1`; default model's `speed_chroma_sycl` stages own U/V)
  never allocate or upload chroma: do not move chroma upload into
  `vmaf_sycl_shared_frame_upload`. Readers: `psnr_sycl` (chroma in
  `post_fn`, after `graph_submit`'s upload barrier), `psnr_hvs_sycl` and
  `motion_v2_sycl` on primary queue behind
  `vmaf_sycl_queue_after_upload()` (= `sycl_apply_input_barriers`).
  `sycl_fence_slot_readers()` in `vmaf_sycl_shared_frame_upload`: upload
  into slot s waits on device for `ext_oneapi_get_last_event()` markers of
  primary + combined queues taken when s stopped being compute slot;
  copy barrier only if marker is still running. Covers readers next
  collect never waits for (`n_subsample` skips). **On rebase**: new
  compute queue that reads shared slots must add its marker there; keep
  fence before ref upload; zero-copy import path imports luma
  only, so chroma readers must fail (`-EINVAL`) on NULL pictures, never
  read stale chroma. Guards: `test_sycl_shared_planes`,
  `test_sycl_init_unwind` (wraps `vmaf_sycl_shared_chroma_init`).

- **`vmaf_sycl_upload_plane()` returns after its copy completed.**
  Copy queue memcpy from caller host memory; function calls
  `sycl_fence_slot_readers()` first, then `last_ev.wait_and_throw()`
  before return. Caller may free/unmap source right after (D3D11 import
  unmaps staging), and compute on other queues has no barrier on this
  copy. **On rebase**: never make it return with copy in flight; without
  wait 4K frame scored wrong pixels on A380
  (`T-SYCL-UPLOAD-PLANE-NO-COMPUTE-FENCE-2026-10-05`). Guard:
  `test_sycl_zero_copy_model_gate` case `test_upload_plane_orders_compute`.

- **Zero-copy admission gate (ADR-1688).** `vmaf_read_pictures_sycl()`
  (`core/src/libvmaf.c`) calls `sycl_zero_copy_admit()` before
  `pic_cnt++` / `vmaf_sycl_advance_frame()`: every registered extractor
  must answer `vmaf_feature_extractor_reads_shared_luma_only()` true, else
  one ERROR line names it and call returns `-ENOTSUP`. CPU extractor and
  SYCL twin without hook = refused. `vmaf_flush_sycl()` skips uninitialized
  extractors so refused context flushes 0. **On rebase**: keep gate first
  in read; never make zero-copy path skip, fall back or pass NULL pictures
  to refused twin. Guards: `test_sycl_zero_copy_admission` (device-free),
  `test_sycl_zero_copy_model_gate` (A380).

- **`dmabuf_import.cpp` normalizes P010/P012 luma MSB→LSB on every
  import path (ADR-1121).** VA-API delivers 10/12-bit samples
  MSB-aligned (`V_MSB = V_LSB << (16 − bpc)`); VMAF feature kernels
  expect LSB-aligned values. `>> (16 − bpc)` shift (guarded
  `if (bpc > 8)`, no-op for 8-bit) applied **two ways**:
  - fused into de-tile store for Tile4 / Y-tiled paths — each sample
    shifted as written, no extra kernel, no second pass; this is
    QSV/DG2 hot path;
  - via standalone `launch_p010_normalize()` kernel for rare LINEAR
    D2D and readback paths (no per-sample store to fuse into), whose
    event threads into `vmaf_sycl_set_detile_event()` (or gets
    subsumed by `q->wait_and_throw()` on readback).

  **On rebase**: shift must stay on *every* import path — dropping it
  on any one re-introduces 64× `integer_motion` / NaN bug for that
  tiling mode; and do **not** re-add standalone full-plane normalize
  pass on tiled paths (cost ~15% throughput at 4K — keep it fused).
  Chroma needs no normalization (shared frame pipeline luma-only,
  above). **Do not add `DMA_BUF_IOCTL_SYNC` coherency flush** — tried
  and removed: insufficient for contamination (real fix = separate
  -per-decoder-QSV-session contract, FIX-03), and its `SYNC_START`
  blocking fence-wait serialised decode→compute.

- **Zero-copy VA-import path defaults to DIRECT dispatch, not
  combined graph (ADR-1121).** `vmaf_sycl_select_strategy()` takes
  `va_import_path` flag (passed `state->has_imported` from
  `common.cpp`), returns DIRECT for it. Applied *after* env-override
  checks -> `VMAF_SYCL_USE_GRAPH=1` / `VMAF_SYCL_DISPATCH=…:graph`
  still force graph. Graph = net throughput loss on this path
  (byte-identical output, but per-frame de-tile import + graph
  compute-barrier serialise decode→compute, ~15–25% slower at 4K).
  **On rebase**: keep `va_import_path` threaded from `has_imported`;
  do not let area-threshold heuristic (tuned for host-upload) re-select
  graph for VA-import.
