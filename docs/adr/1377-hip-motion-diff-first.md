<!-- markdownlint-disable MD013 MD060 -->
# ADR-1377: HIP motion differences the frames before the blur, in one shared kernel, and waits only in collect

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: hip, gpu-parity, numerics, motion, performance, fork-local

## Context

Since the port of Netflix `a4a1492d` (PR #532) the CPU `motion` extractor computes its SAD as `sum |H(V(prev - cur))|`: the frames are differenced first, then the 5-tap Gaussian runs vertically (rounded, `>> bpc`) and horizontally (rounded, `>> 16`). `motion_hip` kept the algorithm from before the port: `integer_motion/motion_score.hip` blurred each frame into a uint16 ping-pong and summed `|blur(cur) - blur(prev)|`. The two are the same sum only without rounding. The SYCL twin had the same order and was 2.0e-4 off at 17x17 and 1.3e-5 on the Netflix 576x324 pair until [ADR-1371](1371-sycl-motion-diff-first-pipeline.md); the HIP page records the same 1.26e-5 on `motion2` / `motion3` against the CPU on a gfx1036 (`T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29`). `motion_v2_hip` already ran a diff-first kernel, `integer_motion_v2/motion_v2_score.hip`, and CPU `motion` and `motion_v2` compute the same SAD frame for frame (Research-1377).

Two more gaps sat in the same extractor. `motion_hip` emitted its debug `motion` score without `motion_fps_weight` and `motion_max_val`, where the CPU emits the weighted, capped score, and a one-frame run never wrote `motion3[0]`, which the CPU reports as 0. And every submit uploaded the reference luma with `vmaf_hip_picture_upload()`, which waits on the host until the copy has read the pageable picture (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18); on the single-queue gfx1036 that wait sits behind the other extractors' kernels (`T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19`).

Constraints: integer motion must be bit-exact with the CPU; no GPU-CPU round trip and no mid-frame host wait, one wait per frame at collect (maintainer, RC3 port brief); one behaviour, one implementation (HISS-19); the HIP twin mirrors the CUDA twin's call graph, which moves to the same design in parallel (ADR-1372 on `fix/cuda-rc3-parity`).

## Decision

1. **One diff-first SAD kernel for both HIP motion twins.** `motion_hip` and `motion_v2_hip` call `vmaf_hip_motion_sad_submit()` (`core/src/feature/hip/integer_motion_sad_hip.{h,c}`), the only host code that loads and launches `motion_v2_score.hip`. `integer_motion/motion_score.hip`, its HSACO target and `integer_motion_hip.h` are deleted. The kernel stages `prev - cur` with reflect-101 borders in shared memory, rounds after the vertical pass (int32 at 8 bits, int64 above) and after the horizontal pass, and adds `|h|` into one 64-bit accumulator.
2. **Raw frames, not blurred ones.** `motion_hip` keeps the previous frame's raw luma in a device ping-pong `pix[2]` (1 or 2 bytes per pixel instead of the uint16 blurred planes); the first frame only uploads.
3. **Staged uploads, one wait per frame.** A frame's luma is copied on the host into an extractor-owned pinned buffer (`vmaf_hip_picture_upload_staged()`, `core/src/hip/picture_hip.{h,c}`) and uploaded from there without a host wait; the SAD and its 8-byte read-back follow on the private stream, and `collect()` is the only wait. The upload is bounded by the buffer's allocated size, which the owner passes (not the frame geometry), and the geometry stays the one `init()` sized the buffers for. Only on an error after a copy was enqueued (a later copy or the SAD launch failing) does the call wait for the stream before returning, so no copy still reads the buffer once the error is reported. The upload is bounded by the buffer's allocated size, which the owner passes (not the frame geometry), and the geometry stays the one `init()` sized the buffers for. Only on an error after a copy was enqueued (a later copy or the SAD launch failing) does the call wait for the stream before returning, so no copy still reads the buffer once the error is reported. The pinned buffer is reused next frame, which is safe because libvmaf collects frame N - 1 before it submits frame N and `collect()` drains the stream. Both motion twins use it; other HIP extractors keep `vmaf_hip_picture_upload()` until they adopt it.
4. **CPU score semantics.** Every score `motion_hip` emits goes through the CPU's clip (`motion_fps_weight`, then `motion_max_val`), the debug `motion` score included, and a one-frame run reports `motion3[0] = 0`.
5. **Tile loads stay in bounds.** The kernel's tile loads pass the reflected index through `vmaf_hip_tile_index()` (`hip_tile_index.h`), the identity for every sample an output consumes; see [ADR-1381](1381-hip-integer-tiny-frame-guards.md).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Shared diff-first kernel, raw ping-pong, staged upload (chosen) | Bit-exact with the CPU at every size by construction; one kernel and one launcher for both twins; half the per-pixel ping-pong bytes at 8 bits; no host wait in `submit()` | Reads two raw planes per frame; one host `memcpy` of the luma per frame | — |
| Keep blurring each frame and widen the gate for small frames | No code change | Integer twin stays inexact; the CPU is the reference; a tolerance wide enough for 17x17 hides real regressions | The difference is arithmetic, not precision |
| Store each frame's unrounded vertical sums and difference those | Bit-exact (the vertical pass is linear before rounding); one raw plane read | 4 bytes per pixel written and read per frame, plus a gather of five columns; more code | More traffic than two raw planes (the SYCL analysis of ADR-1371 applies unchanged) |
| A diff-first kernel of its own in `motion_score.hip` | No change to `motion_v2_hip` | Two copies of the same arithmetic drift (HISS-19) | HISS-19 |
| Keep `vmaf_hip_picture_upload()` (host wait) | Existing, race-free helper | Blocks `submit()` until the copy runs, behind other extractors' kernels on one hardware queue; violates the one-wait-per-frame rule | The staged form is race-free too and never waits |
| Upload straight from the pageable picture without a wait | No host copy | Scores frames against the next frame's samples (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18) | Incorrect |

## Consequences

- **Positive**: `motion_hip` computes the CPU `motion` arithmetic: integer SAD, same host scoring, so `motion2` / `motion3` / debug `motion` are expected to equal `--backend cpu` bit for bit (1.26e-5 today on the Netflix pair). `submit()` of both motion twins no longer waits on the host. The debug score and one-frame runs follow the CPU.
- **Negative**: not measured on AMD hardware in this change; the home verification commands live in the `T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29` row of `docs/state.md`. The kernel reads two planes per frame where the blurred pipeline read one blurred plane plus the raw frame.
- **Neutral / follow-ups**: other HIP extractors can adopt `vmaf_hip_picture_upload_staged()` one by one (`T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19`). Guarded by `test_hip_motion_tiny_frames` (`==` against the scalar CPU, 3x3 to 1283x723, 8/10/16 bits, skips without a device), `test_hip_adm_dwt2_rows` (tile index replay) and the motion cases of `test_hip_kernel_source_contract.py`.

## References

- req: RC3 port brief (2026-09-30): "there shouldnt be any gpu cpu rountrips"; "Implementation matching the CPU reference (integer paths bit-exact ...), no host round trips and no mid-frame host waits (one wait per frame, at collect)".
- [ADR-1371](1371-sycl-motion-diff-first-pipeline.md) — the SYCL design this ports; [Research-1371](../research/1371-sycl-motion-diff-first-pipeline.md).
- [Research-1377](../research/1377-hip-rc3-cpu-parity.md) — CPU `motion` / `motion_v2` SAD equivalence, tile and ADM row replays, SSIM identical-window analysis.
- [ADR-0219](0219-motion3-gpu-coverage.md), [ADR-1216](1216-gpu-motion3-fps-weight-applied-once.md), [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md).
