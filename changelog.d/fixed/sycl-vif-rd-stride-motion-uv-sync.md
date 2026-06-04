### Fixed

- **SYCL integer_vif rd_stride OOB on odd widths** (HIGH): Both the
  scalar (SIMD-32, `launch_vif_hori_impl`) and SIMD-16 (`launch_vif_fused_impl`)
  downsampling paths used truncating `e_w / 2` as the row stride for the
  `rd_ref`/`rd_dis` downsampled buffers. For odd frame widths the last even column
  thread wrote one element past the row boundary, corrupting adjacent device memory
  and producing wrong VIF scores at subsequent scales. Fix: ceiling division
  `(e_w + 1U) / 2U` for stride; allocation changed from `(w/2) * (h/2)` to
  `((w+1)/2) * ((h+1)/2)` to match. (ADR-1034)

- **SYCL integer_motion UV queue sync gap** (HIGH): When `motion_add_uv=true`,
  UV H2D copies via `vmaf_sycl_memcpy_h2d_async()` were submitted to the primary
  queue (`state->queue`), but `vmaf_sycl_graph_submit()` only barriers the compute
  queue on `last_upload_event` from the DMA copy queue. UV data could be in-flight
  on the primary queue when compute kernels launched, producing wrong motion scores
  for UV planes. Fix: `vmaf_sycl_queue_wait(state)` flushes the primary queue
  after UV copies and before graph submission. (ADR-1034)
