<!-- markdownlint-disable MD013 MD060 -->
# ADR-1372: CUDA motion differences the frames before the blur, in the kernel motion_v2 already had

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: cuda, gpu-parity, numerics, motion, fork-local

## Context

Since the port of Netflix `a4a1492d` (PR #532) the CPU `motion` extractor computes its SAD as `sum |H(V(prev - cur))|`: the frames are differenced first, then the 5-tap Gaussian runs vertically (rounded, `>> bpc`) and horizontally (rounded, `>> 16`). `motion_cuda` kept the algorithm from before the port: `integer_motion/motion_score.cu` blurred each frame into a uint16 ping-pong and summed `|H(V(cur)) - H(V(prev))|`. The two are the same sum only without rounding. The SYCL twin with the same order was 2.0e-4 off at 17x17 and 1.3e-5 on the Netflix 576x324 pair until [ADR-1371](1371-sycl-motion-diff-first-pipeline.md) (`T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`). `motion_v2_cuda` already ran the CPU's order: its kernel differences first, and the CPU `motion_v2` uses the same arithmetic as the CPU `motion`.

Two smaller gaps sat in the same extractor. Its debug `VMAF_integer_feature_motion_score` was the raw normalised SAD, where the CPU emits its SAD score, weighted by `motion_fps_weight` and capped at `motion_max_val`. And each batch readback ([ADR-0845](0845-cuda-motion-launch-overhead.md)) synchronised the readback stream twice, once before queueing the copies and once after, although the copies already queue behind every frame's event.

Constraints: integer motion must be bit-exact with the CPU; one behaviour, one implementation (HISS-19); no GPU-CPU round trip and no host wait in the middle of a frame (req); each extractor must be correct on its own, without leaning on the engine's once-per-frame context barrier ([ADR-1199](1199-cuda-picture-handover-barrier.md)), which exists for pictures an external producer fills.

## Decision

1. **One diff-first kernel for both CUDA motion twins.** `motion_cuda` and `motion_v2_cuda` call `vmaf_cuda_motion_sad_submit()` (`core/src/feature/cuda/integer_motion_sad_cuda.{h,c}`), which loads and launches the kernel of `integer_motion_v2/motion_v2_score.cu`. A block stages `prev - cur` with reflect-101 borders in shared memory, filters it vertically and horizontally with the CPU's rounding, and adds `|h|` into one uint64 accumulator. The vertical sum is int32 for 8-bit input and int64 above, picked by the host. `integer_motion/motion_score.cu` and its module are deleted.
2. **`motion_cuda` keeps raw frames, not blurred ones.** A device ping-pong `raw[2]` holds the packed luma of the current and previous frame (1 or 2 bytes per pixel instead of the old uint16 blurred planes), filled by a device-to-device copy before the kernel. The first frame only copies.
3. **Ordering on the device, not the host.** Each frame's stream waits on the previous frame's event before it overwrites one ping-pong slot and reads the other. `motion_cuda`'s batch readback queues its copies behind the chained events and synchronises once. `submit()` never waits on the host.
4. **The debug score is the CPU's.** `motion_cuda` emits `MIN(sad * motion_fps_weight, motion_max_val)` for `VMAF_integer_feature_motion_score`, as `integer_motion.c::extract` does.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reuse the motion_v2 kernel through one host helper (chosen) | Bit-exact at every size; the kernel already existed and matched the CPU `motion_v2`; one implementation for both twins; no uint16 blurred planes | `motion_cuda` now reads two raw planes per tile | — |
| Fix the order inside `motion_score.cu` | Smaller diff in the motion TU | Two kernels with the same arithmetic drift apart (HISS-19); SYCL chose one shared kernel for the same reason (ADR-1371) | HISS-19 |
| Keep the per-frame blur and widen the motion gate | No code change | The integer twin stays inexact; the discrepancy is arithmetic, not precision | Nothing to tolerate |
| Rely on the engine's per-frame `cuCtxSynchronize` for the ping-pong hazards | No event wait | The extractor would race the moment the ADR-1199 barrier is narrowed or removed | Device-side ordering costs nothing |
| Drop ADR-0845's eight-frame readback batch for a per-frame readback | Simpler collect path | Changes a measured throughput decision outside this fix | Out of scope; the batch keeps one wait per eight frames |

## Consequences

- **Positive**: `motion_cuda` computes the CPU `motion` SAD exactly, so `motion2` / `motion3` and the debug score equal the CPU's; `motion_v2_cuda`'s SAD is unchanged (same kernel, same arithmetic); its option handling is aligned with the CPU by [ADR-1373](1373-cuda-twin-cpu-option-parity.md). One module per motion twin pair instead of two. The batch readback waits once instead of twice. Tile loads of padding threads are clamped into the plane (`cuda_tile_index.h`), so planes smaller than the 20x20 tile can no longer read before a buffer.
- **Negative**: the `motion_cuda` kernel reads two planes per tile. On an RTX 4090 (2026-09-30, shared host) the 4K wall time per frame stayed within the noise: 5.1 against 4.8 ms for a `master` build at (t(200) - t(2)) / 198, median of 5, and 2.6 against 3.1 at (t(22) - t(2)) / 20, median of 3; reading and uploading the frames dominate. The SAD kernels use 28 (8-bit) and 40 (16-bit) registers with no local memory, 6 blocks of 256 threads per SM.
- **Neutral / follow-ups**: verified on an RTX 4090 on 2026-09-30: `integer_motion2` / `integer_motion3` equal the CPU (0.0) on the Netflix pair and on 50 frames of a 3840x2160 clip, where a `master` build was 1.26e-5 and 6.93e-5 off (`T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`, closed). The same run found that `motion_force_zero` crashed `motion_cuda` on its first frame, an engine defect fixed alongside (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`). Guarded by `test_cuda_motion_tiny_frames` (`==` against the scalar CPU, 3x3 to 1283x723, 8, 10 and 16 bits) and the motion cases of `test_cuda_kernel_source_contract.py`. The HIP and Metal twins are `T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29` and `T-METAL-MOTION-BLUR-THEN-DIFF-2026-09-29`.

## References

- req: "there shouldnt be any gpu cpu rountrips" (user, relayed in the RC3 CUDA port brief, 2026-09-30).
- req: RC3 CUDA port brief (2026-09-30): "Implementation matching the CPU reference (integer paths bit-exact ...), no host round trips and no mid-frame host waits (one wait per frame, at collect)".
- [Research-1372](../research/1372-cuda-rc3-parity-port.md) — root cause, launch-grid traces, what was and was not verifiable without a device.
- [ADR-1371](1371-sycl-motion-diff-first-pipeline.md) (the SYCL decision this ports), [ADR-0845](0845-cuda-motion-launch-overhead.md), [ADR-0358](0358-cuda-motion-race-and-precision-fixes.md), [ADR-0219](0219-motion3-gpu-coverage.md), [ADR-1199](1199-cuda-picture-handover-barrier.md).
