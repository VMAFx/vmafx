# ADR-0845: CUDA motion — multi-frame SAD batching to reduce per-launch overhead

- **Status**: Proposed
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `performance`, `motion`, `fork-local`

## Context

The CUDA `calculate_motion_score_kernel_8bpc` is dispatch-bottlenecked at all
resolutions below 4K (Research-0760, 2026-05-29). ncu profiling on an RTX 4090
shows the kernel takes 7 µs at 576p but the total per-frame wall time is ~12.7 ms,
meaning GPU busy fraction is under 0.1%. The CUDA backend runs at 0.22× CPU
throughput at 576p (79 fps vs 353 fps CPU) purely because of driver round-trip
overhead. At 1080p the situation is similarly dominated by dispatch overhead.

The root cause is that the legacy submit/collect pattern issues one
`cuLaunchKernel`, one `cuEventRecord` × 2, one `cuStreamWaitEvent`, one
`cuMemcpyDtoHAsync`, and one `cuStreamSynchronize` per frame. Each driver API
call costs 1–3 ms of CPU-side overhead; the kernel itself is irrelevant to
wall-clock performance at small resolutions.

This optimization is tracked under PR #75 and PR #77 findings, with the baseline
measurement provided by Research-0760.

## Decision

We will batch MOTION_BATCH_DEPTH (8) kernel launches before issuing a single
`cuStreamSynchronize` to drain all device-to-host copies. Each frame's SAD
accumulator uses a dedicated slot in a ring of 8 `VmafCudaBuffer *sad[]` device
buffers and a single pinned host array of 8 `uint64_t` values. The DtoH
readback is deferred from submit() to a batch-boundary collect() call, reducing
the number of `cuStreamSynchronize` calls from N (one per frame) to
ceil(N / MOTION_BATCH_DEPTH) (one per 8 frames).

Score emission is also deferred to batch boundaries: non-boundary collect()
calls increment frame_index and return 0 without emitting; the batch-boundary
collect() emits scores for all MOTION_BATCH_DEPTH frames at once using the
`emit_batch_scores()` helper. flush() handles the final partial batch.

The motion3_postprocess_cuda moving-average guard uses a per-frame
frame_index saved/restored inside emit_batch_scores() to preserve
numerical correctness (ADR-0219 / ADR-0214 places=4 gate).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Multi-frame kernel (process N frames in one launch) | Maximally reduces launch count | Requires new kernel variant; significant device-code change; hard to keep bit-exact with SYCL/Vulkan twins | Too high risk and scope; correctness gate harder to maintain |
| Separable horizontal + vertical filter passes (Candidate 2, Research-0760) | Reduces compute latency at 4K by ~40% | Two kernel launches per frame (doubles dispatch overhead at sub-4K); only helps at 4K where kernel compute dominates | Worsens sub-4K where the bottleneck is not compute; orthogonal follow-on |
| `cp.async` tile prefetch (Candidate 3, Research-0760) | ~10–15% occupancy improvement | Adds assembly-level PTX inline; sm_80+ only; kernel-level improvement irrelevant at 576p (dispatch-bottlenecked) | Small gain; should be bundled with Candidate 2 as a follow-on |
| `__launch_bounds__` / block-size tuning only | Zero code risk | Does not address dispatch overhead; kernel improvements are <5% at dispatch-bottlenecked resolutions | Does not address the bottleneck |
| BATCH_DEPTH = 2 (shallow pipeline) | Simpler logic | ~50% overhead reduction vs ~87.5% at BATCH=8 | BATCH=8 achieves 87.5% sync reduction with the same structural complexity |

## Consequences

- **Positive**: CUDA motion expected to achieve ≥ 2× CPU throughput at 576p and
  1080p (from 0.22× baseline). Per Research-0760 Candidate 1 estimate: ~79 fps →
  ~800 fps at 576p with BATCH=8. 4K performance is unchanged (dispatch is already
  amortised there).
- **Negative**: Score emission is deferred by up to MOTION_BATCH_DEPTH frames.
  This is transparent to callers because `vmaf_feature_collector_append` accepts
  any index. Memory overhead increases from 1 device SAD buffer + 1 pinned uint64
  to 8 device SAD buffers (8 × 8 bytes = 64 bytes) + 1 pinned array of 8 × 8
  bytes = 64 bytes. Negligible.
- **Neutral / follow-ups**: (1) The drain_batch engine-scope fence optimization
  (ADR-0242) is no longer relevant for the motion extractor — it has been removed
  from the submit path. The extractor is no longer registered with the drain batch.
  (2) The `finished` event handle (previously used for drain_batch) has been
  removed from MotionStateCuda. (3) The MOTION_BATCH_DEPTH constant is compile-time
  only; no runtime option is exposed. (4) A/B measurement against the baseline in
  Research-0760 is required before promoting to DRAFT or marking PASS.
  (5) The SYCL / Vulkan motion twins do NOT implement batching — their dispatch
  overhead profiles differ and this optimization does not apply to them.
  (6) A correctness check at places=4 (ADR-0214) against CPU is required.

## References

- Research-0760 (2026-05-29): CUDA motion kernel ncu multi-resolution profile
- Research-0735 (2026-05-28): CUDA motion kernel hotpath analysis at 576p
- ADR-0219: motion3 GPU contract
- ADR-0214: GPU-parity CI gate (places=4)
- ADR-0242: engine-scope fence batching (drain_batch — no longer consumed
  by motion after this ADR)
- ADR-0760: CUDA motion ncu multi-resolution profiling methodology
- `req`: per-agent task brief 2026-05-29 — "reduce per-launch overhead...
  Target: motion CUDA wins or ties CPU at 576p AND 1080p"
- PR #75, PR #77: benchmark findings that motivated this work
