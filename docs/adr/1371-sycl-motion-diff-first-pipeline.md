<!-- markdownlint-disable MD013 MD060 -->
# ADR-1371: SYCL motion differences the frames before the blur, in one shared kernel

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, gpu-parity, numerics, motion, performance, fork-local

## Context

Since the port of Netflix `a4a1492d` (PR #532) the CPU `motion` extractor computes its SAD as `sum |H(V(prev - cur))|`: the frames are differenced first, then the 5-tap Gaussian runs vertically (rounded, `>> bpc`) and horizontally (rounded, `>> 16`). `motion_sycl` kept the algorithm from before the port: it blurred each frame, stored the blurred frame, and summed `|H(V(cur)) - H(V(prev))|`. The two are the same sum only without rounding. With rounding each pixel can be one unit off in each pass, and the mean error over a frame falls roughly with the square root of its pixel count, so `T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29` saw `motion2` up to 2.0e-4 off at 17x17 and 1.3e-5 on the Netflix 576x324 pair, identically on an Arc B580 and a UHD 770. The row suspected edge handling; the reflection and the filter taps were already the CPU's. `motion_v2_sycl` already implemented the CPU's order and was bit-exact.

`motion_add_uv` had a second problem. ADR-1034 fixed a race by uploading U and V on the primary queue and then waiting on the host inside `submit()`, a host round trip in the middle of every frame (`T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29`, found by the ADR-1363 audit in PR #1627).

Constraints: integer motion must be bit-exact with the CPU; SYCL kernels stay fp64-free ([ADR-0220](0220-sycl-fp64-fallback.md)); one behaviour, one implementation (HISS-19); anonymous kernel lambdas in two translation units can receive the same generated name ([Research-2090](../research/2090-sycl-silent-revert-residuals-2026-09-24.md)); no mid-frame host wait.

## Decision

1. **One diff-first kernel for both motion twins.** `motion_sycl` and `motion_v2_sycl` call `motion_sycl_pipeline::enqueue_sad()` (`core/src/feature/sycl/integer_motion_pipeline_sycl.{h,cpp}`). A work-group stages `prev - cur` with reflect-101 borders in local memory, filters it vertically and horizontally with the CPU's rounding, and adds `|h|` into one int64 accumulator. The vertical sum is int32 up to 15 bits per sample and int64 at 16 bits; the host picks the instance, so the kernel has no per-pixel width test. The kernel exists only in that translation unit.
2. **`motion_sycl` keeps raw frames, not blurred ones.** The previous frame's luma lives in a device ping-pong `d_raw_y[2]` (1 or 2 bytes per pixel instead of 4), filled by a device copy from the shared frame buffer after the kernel. The first frame only copies.
3. **`motion_add_uv` without a host wait.** `submit()` packs the reference U and V planes into pinned host staging; the combined queue's `pre_fn` copies them to the device ahead of the kernels. This supersedes ADR-1034's primary-queue wait (its "Bug 2" decision); its `rd_stride` decision is unaffected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Diff-first kernel shared by both twins (chosen) | Bit-exact with the CPU at every size; one implementation; the old int32 blurred planes (4 bytes per pixel written and read per frame) are gone | Reads two raw planes per tile; at 4K the SAD step (kernel plus copy) is about 11% slower on a B580 and on a UHD 770 | — |
| Keep per-frame blurring and widen the motion gate for small frames | No code change | The integer twin stays inexact; a tolerance wide enough for 17x17 hides real regressions; the CPU is the reference | The discrepancy is arithmetic, not precision; there is nothing to tolerate |
| Store each frame's unrounded vertical sums and difference those | Bit-exact (the vertical pass is linear before rounding); reads one raw plane | Writes and reads 4 bytes per pixel per frame again, plus a five-column gather of the previous sums; more code | More memory traffic than two raw planes |
| Copy `cur` with a store inside the kernel instead of a device copy | One dispatch per plane | One more memory message per pixel; measured no faster (UHD 770 6.88 ms vs 6.65 + 0.17 ms, B580 0.517 vs 0.525 + 0.013 ms at 4K) | Simpler code for the same cost |
| A copy of the kernel in each extractor TU | No new file | Two implementations drift (HISS-19); identical generated kernel names across TUs (Research-2090) | HISS-19 |
| `motion_add_uv`: keep the primary-queue upload and order the graph after it with an event (ADR-1034's listed alternative) | Keeps the upload where it was | Needs a new public event hand-off between queues; the upload from pageable picture memory still needs the picture to outlive the copy | The combined queue's `pre_fn` already orders work ahead of the kernels; `psnr_sycl` stages its chroma the same way |

## Consequences

- **Positive**: `motion_sycl` equals the CPU `motion` bit for bit on every frame tested: 3x3 to 1283x723, the Netflix pair, 24 BBB 4K frames, 8, 10 and 16 bits, on an Arc B580 and a UHD 770, in direct and graph dispatch. The default-model VMAF score at 4K moves by at most 4e-6. `motion_v2_sycl` output is unchanged. With `motion_add_uv`, `submit()` no longer blocks: host time per 4K frame drops from 0.89 to 0.55 ms on the B580 and from 5.4 to 0.6 ms on the UHD 770.
- **Negative**: the motion kernel reads two planes. At 4K the kernel and the copy take 0.538 instead of 0.484 ms on the B580 and 6.82 instead of 6.15 ms on the UHD 770 (micro-benchmark); a motion-only UHD 770 run costs 18.9 instead of 16.6 ms of device time per frame. Default-model 4K throughput is unchanged within run-to-run noise on both GPUs.
- **Neutral / follow-ups**: the CUDA, HIP and Metal `motion` twins still blur each frame (`T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`, `T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29`, `T-METAL-MOTION-BLUR-THEN-DIFF-2026-09-29`); their `motion_v2` twins already difference first. Guarded by `test_sycl_motion_tiny_frames` (`==` against the scalar CPU), the diff-first oracle of `test_sycl_motion_add_uv_parity`, and the motion cases of `test_sycl_kernel_source_contract.py`.

## References

- req: task brief (2026-09-29): "Fix so outputs match CPU exactly (integer motion should be bit-exact) at 17x17, 33x33, 64x64, 257x145, 576x324, BBB 4K, 8- and 10-bit, on both B580 and UHD 770."
- req: coordinator (2026-09-29): "Remove it (enqueue the chroma upload/blur on the queue, single wait at collect), keep output bit-identical".
- [Research-1371](../research/1371-sycl-motion-diff-first-pipeline.md) — root cause, per-size before/after, timings.
- [ADR-1034](1034-sycl-vif-rd-stride-motion-uv-sync.md) (Bug 2 superseded), [ADR-0989](0989-sycl-motion-add-uv.md), [ADR-0220](0220-sycl-fp64-fallback.md), [ADR-0219](0219-motion3-gpu-coverage.md).
