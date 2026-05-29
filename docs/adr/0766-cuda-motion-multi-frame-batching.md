# ADR-0766: N-Slot SAD Ring Buffer for CUDA Motion Extractor

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: `cuda`, `perf`, `build`

## Context

The CUDA motion extractor (`integer_motion_cuda.c`) used a single
`uint64_t` device SAD accumulator shared across all frames.  Each frame
followed the pattern:

1. `cuMemsetD8Async(sad->data, 0, ...)` on `pic_stream`
2. `cuLaunchKernel` on `pic_stream`
3. `cuEventRecord(event, pic_stream)`
4. `cuStreamWaitEvent(str, event)`
5. `cuMemcpyDtoHAsync(sad_host, sad->data, 8, str)` — 8-byte DtoH
6. `cuEventRecord(finished, str)` + `drain_batch_register_event`

The `cuMemsetD8Async` in step 1 needed to be on `pic_stream` (ADR-0358)
because the kernel launches on the same stream.  With a **single** sad
buffer, this memset is ordered after the previous frame's DtoH
(because the previous frame's DtoH enqueues on `s->str`, and the memset
is on `pic_stream`, but the cross-stream dependency is established by
`cuStreamWaitEvent(pic_stream, event)`... only for the ready event of the
_dist_ picture, not for the previous frame's DtoH).

Actually the more significant issue: the single sad buffer means the memset
for frame N+1 CANNOT start until frame N's DtoH has completed AND the
host has validated the data, because they share the same device pointer.
With different slots, frame N+1's memset runs independently on pic_stream
while frame N's DtoH is still in flight on `s->str`.

At 576p the per-frame kernel execution time is ~7 µs and the whole
dispatch overhead (cuStreamSynchronize round-trip via drain_batch) is
~12.7 ms per frame (~79 fps).  Full N× sync amortisation (submitting N
frames before syncing) requires the engine to not call `collect()` between
the N submits.  The current engine uses a strict 1-frame-lag pattern
(`collect(i-1)` then `submit(i)` per frame), so the ring provides the
correct device and host layout for a future engine-level N-frame dispatch
optimisation.

## Decision

Replace the single `VmafCudaBuffer *sad` with an N-slot ring:

```c
VmafCudaBuffer *sad_ring;   // N × uint64_t on device
uint64_t       *sad_host;   // N × uint64_t pinned on host
```

Slot assignment: `slot = (1-based index - 1) % N` for `index >= 1`.
Frame 0 uses slot 0 as a throwaway.

Each `submit()` zeros its own slot (no cross-frame dependency),
launches the kernel targeting that slot, and issues an 8-byte DtoH
for its slot only.  The `collect()` path and drain_batch integration
are unchanged.

N defaults to 16, overridable via `-Dmotion_batch_n=N` (integer 1–64).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| N=1 (status quo, pre-ADR-0766) | Zero change | Single-buffer false dependency; no ring for future batching | Strictly worse than N=16 ring |
| N=16 (chosen) | Eliminates false dependency; ring is positioned for future engine batching; tiny memory overhead (16×8=128 bytes device + 128 bytes pinned host) | No sync amortisation under 1-frame-lag engine today | Best balance of correctness, infrastructure, and future-readiness |
| N=∞ (full-stream batching) | Maximum amortisation if engine supports it | Breaks streaming use case; requires all frames in memory simultaneously | Rejected — streaming is a hard requirement |
| Engine N-frame dispatch (libvmaf.c) | Achieves N× sync amortisation; full speedup | Requires framework changes; broad impact on all CUDA extractors | Deferred to follow-up ADR; this ADR provides the extractor-side prerequisite |

## Consequences

- **Positive**: Eliminates false dependency between consecutive frame memsets;
  provides the correct device/host layout for future engine-level N-frame
  batching; N-slot pinned host allocation improves reuse locality.
- **Negative**: Slightly more device memory (N×8 bytes, negligible) and
  host pinned memory (N×8 bytes).  No measured speedup under the current
  1-frame-lag engine.
- **Neutral / follow-ups**: A follow-up patch to `libvmaf.c`'s
  `read_pictures_extractor_loop_cuda` to batch N submits before the
  `drain_batch_flush` would unlock the projected 79 → ≥800 fps speedup
  at 576p.  See `docs/research/` for the dispatch analysis.

## References

- req: "Implement multi-frame SAD batching for `calculate_motion_score_kernel_8bpc`
  per PR #98's top recommendation."
- PR #98: dispatch overhead 12.7ms/frame vs 7µs/frame GPU work
- [ADR-0358](0358-cuda-motion-race-and-precision-fixes.md): memset-on-pic_stream ordering invariant
- [ADR-0242](0242-cuda-drain-batch.md): engine-scope fence batching
- [ADR-0219](0219-motion3-gpu-contract.md): motion3 GPU contract
