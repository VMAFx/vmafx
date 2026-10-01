<!-- markdownlint-disable MD013 MD060 -->
# ADR-1410: SYCL CLI picture pool allocates pinned host USM to bypass staging upload

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `sycl`, `performance`, `cli`, `zero-copy`, `usm`, `picture-pool`, `rc3`, `fork-local`

## Context

When running the `vmaf` CLI with `--backend sycl`, pictures were allocated via `vmaf_picture_alloc()` in standard pageable host heap memory (`picture_pool.c`, `libvmaf.c:prepare_picture_pool()`).

Because the host memory was pageable:

1. `vmaf_sycl_shared_frame_upload()` (`sycl_enqueue_plane_upload`) forced the driver to page-lock and copy memory through intermediate staging buffers on the calling thread. For pitched or pageable memory, uploading 4K luma (3840x2160, 8.3 MB) took 2.2 to 3.0 ms per frame.
2. In `vmaf_sycl_shared_chroma_upload()` ([ADR-1369](1369-sycl-shared-planes-light-twins.md)), chroma planes were packed via `std::memcpy` on the host into pinned staging buffers (`chroma.staging`) before dispatching DMA transfers, adding another 0.8 to 1.0 ms of host CPU staging overhead.

`T-SYCL-PAGEABLE-UPLOAD-HOST-STAGING-2026-09-29` recorded this staging latency floor. The objective was to eliminate the 2.2 to 3.0 ms host staging per 4K luma upload and bypass chroma staging by providing a pinned host USM picture pool directly to the CLI while preserving identical numerical scores.

## Decision

We introduce a pinned host USM picture pool for SYCL CLI runs:

1. **Custom Allocation Callbacks in `VmafPicturePoolConfig`**:
   `VmafPicturePoolConfig` gains optional callbacks (`alloc_picture_callback`, `free_picture_callback`, `sync_picture_callback`, `attach_picture_callback`, and `cookie`) and buffer type `VMAF_PICTURE_BUFFER_TYPE_SYCL_HOST_PINNED`.
2. **Pinned SYCL Host Picture Allocation**:
   `vmaf_sycl_picture_alloc_pinned()` (`core/src/sycl/picture_sycl.cpp`) allocates picture planes using `vmaf_sycl_malloc_host()` (`sycl::malloc_host`), ensuring 32-byte alignment per plane (`DATA_ALIGN_PINNED 32`) matching the contiguous row layout when `stride == row_bytes`.
3. **Picture Pool Integration in `libvmaf.c`**:
   When a SYCL state is present, `prepare_picture_pool()` wires `sycl_pinned_pic_alloc`, `sycl_pinned_pic_free`, `sycl_pinned_pic_sync`, and `sycl_pinned_pic_attach` into the pool configuration. When `vmaf_picture_pool_fetch()` dispenses a picture, `sync_picture_callback` waits on the picture's last upload event before reuse, preventing the host reader thread from overwriting a buffer actively being read by GPU DMA.
4. **Direct Asynchronous DMA in SYCL Upload**:
   `sycl_enqueue_chroma_plane()` and `vmaf_sycl_shared_frame_upload()` detect host USM memory via `sycl::get_pointer_type()`. For contiguous pinned host pictures, chroma staging copies are bypassed and direct `copy_queue.memcpy` transfers are enqueued directly from the picture planes. Upload completion events are recorded into `ref_priv->sycl.ready_event` and `dis_priv->sycl.ready_event`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pinned host USM pool in CLI with sync callbacks (chosen) | Zero staging copies on host thread; direct DMA transfer; asynchronous queue execution; scores bit-identical | Requires tracking upload completion events and synchronizing on pool fetch | — |
| Keep pageable pool and register memory (`zeDriverRegisterHostPointer`) | No change to picture pool allocation | Level Zero host pointer registration is driver/hardware dependent, has high registration overhead per frame, and is unsupported across older platforms | Unreliable across driver versions and adds overhead |
| Device-resident picture pool (device USM) | Direct device allocation | Host file readers cannot write directly to device memory without staging copies or mapping | Still requires staging from disk/pipe to device |

## Consequences

- **Positive**: At 3840x2160 on an Intel Arc A380 (Linux `xe` driver), luma + chroma upload time dropped from 2.2–3.0 ms down to 0.70 ms per frame (~3-4x speedup in upload latency). Scores across PSNR Y, Cb, Cr match bit-identically (0.0 ULP drift).
- **Negative**: Pinned host USM memory consumes locked physical RAM proportional to pool capacity (7 pictures at 4K = ~87 MB).
- **Neutral**: Transparent fallback to pageable allocation when SYCL is not enabled or allocation fails.

## References

- req: `T-SYCL-PAGEABLE-UPLOAD-HOST-STAGING-2026-09-29`
- [ADR-1369](1369-sycl-shared-planes-light-twins.md) — SYCL shared planes and light twins
- [ADR-1366](1366-cli-frame-readahead.md) — CLI asynchronous frame read-ahead
