<!-- markdownlint-disable MD013 MD060 -->
# ADR-1406: Preallocate pinned host pictures for zero-copy 4K CLI CUDA upload

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris, user
- **Tags**: `cuda`, `perf`, `cli`, `picture-pool`

## Context

In the `vmaf` CLI, when executing CUDA-accelerated feature extractors or models at 4K resolution (3840×2160, 8-bit YUV 4:2:0 is ~12.4 MB/frame), reading uncompressed video frames into standard pageable host memory allocated via `posix_memalign` forces the NVIDIA CUDA driver (`cuMemcpy2DAsync`) to stage the frame data through an internal driver bounce buffer before PCIe transfer.

At 4K resolution, this synchronous host CPU bounce copy incurs ~2.1–2.6 ms per frame, dominating frame time across CUDA twins (e.g. `psnr_cuda`, `adm_cuda`, `vif_cuda`, and the default model `vmaf_v1.0.16_3d0h`).

While `vmaf_cuda_picture_alloc_pinned()` and asynchronous DMA upload existed in `core/src/cuda/picture_cuda.c`, the CLI picture pool did not utilize pinned host memory allocations or upload completion synchronization events, causing all CLI-driven CUDA workflows to pay pageable bounce staging overhead.

## Decision

1. Extend `VmafPicturePoolConfig` and `VmafPicturePool` in `core/src/picture_pool.h`, `core/src/picture_pool.c`, and `core/src/picture_pool.cpp` to support pinned host picture allocation callbacks:
   - `alloc_picture_callback`: Custom allocator invoking `vmaf_cuda_picture_alloc_pinned()`
   - `free_picture_callback`: Custom deallocator releasing pinned memory via `cuMemFreeHost()` and destroying synchronization events
   - `sync_picture_callback`: Slot synchronization callback waiting for pending DMA uploads via `cuEventSynchronize()` before buffer reuse
   - `cookie`: Backend state pointer (`VmafCudaState*`)
   - `buf_type`: Attached buffer type (`VMAF_PICTURE_BUFFER_PINNED`)

2. In `core/src/libvmaf.c`, configure `prepare_picture_pool()` to instantiate a pinned host picture pool when CUDA is active (`vmaf_cuda_state`), preallocating reusable pinned host picture buffers and initialising completion events via `cuEventCreate` with `CU_EVENT_DISABLE_TIMING`. If pinned host memory allocation fails (e.g. system `ulimit -l` or memory constraints), gracefully fall back to the standard pageable picture pool.

3. In `core/src/cuda/picture_cuda.c`, record `pic_priv->cuda.ready` on the CUDA stream in `vmaf_cuda_picture_upload_async()` when uploading from `VMAF_PICTURE_BUFFER_PINNED`. When the picture is recycled and fetched from the pool for the next frame (`vmaf_picture_pool_fetch`), `sync_picture_callback` synchronizes the ready event before the CLI writes new frame data into the pinned buffer.

4. Add unit test `test_cuda_cli_preallocate_pinned_pool` in `core/test/test_cuda_pic_preallocation.c` verifying pool creation, fetch, upload event recording, and cleanup.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Direct `cuMemHostRegister` on pageable buffers per-frame | No pool changes needed | Registration/unregistration per frame has severe kernel driver lock overhead (~1.5 ms) | Rejected — slower than bounce buffer |
| Require users to pass `--pinned-upload` CLI flag | Explicit opt-in | Suboptimal out-of-the-box performance for all CLI users | Rejected — should be automatic with graceful fallback |
| Dedicated pinned picture pool with stream event synchronization | Zero-copy direct PCIe DMA; completely transparent to CLI; zero score drift | Pinned memory footprint equal to pool capacity (~12.4 MB × pool_size) | **Accepted (Recommended)** |

## Consequences

- **Positive**: 4K CUDA upload latency reduced by eliminating pageable bounce staging.
  - Default model (`vmaf_v1.0.16_3d0h`): 5.42 ms/frame -> 4.87 ms/frame (10.1% faster)
  - `psnr_cuda`: 2.29 ms/frame -> 2.10 ms/frame (8.3% faster)
  - `adm_cuda`: 3.99 ms/frame -> 3.65 ms/frame (8.5% faster)
  - `vif_cuda`: 2.32 ms/frame -> 1.82 ms/frame (21.6% faster)
  - Bit-identical scores across all features (0 score drift).
- **Negative**: Pinned host memory consumed while CLI runs (~37–75 MB for 3–6 pictures at 4K).
- **Neutral / follow-ups**: Graceful fallback to pageable pool preserves functionality if pinned allocation fails.

## References

- Row `T-CUDA-PAGEABLE-UPLOAD-4K-2026-09-30`
- [ADR-0239](0239-gpu-picture-pool-dedup.md) (GPU picture pool deduplication)
- [ADR-1199](1199-cuda-picture-handover-barrier.md) (CUDA picture handover barrier)
- [ADR-1398](1398-cli-accept-odd-dimensions-chroma-subsampled.md) (CLI raw video input)
