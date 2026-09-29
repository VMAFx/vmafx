<!-- markdownlint-disable MD013 MD060 -->
# Research-1371: `motion_sycl` tiny-frame parity — blur order, not edges

- **Status**: Active
- **Workstream**: RC3 SYCL, `T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29` and `T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29`; decision in [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md)
- **Last updated**: 2026-09-29

## Question

`motion_sycl` differed from the CPU `motion` by more than places=4 below about 64x64, and by less on larger frames. The state row suspected edge handling. What is the cause, do `motion_v2_sycl` and `float_motion_sycl` share it, and which other GPU twins do?

## Sources

- CPU reference: `core/src/feature/integer_motion.c` (`motion_score_pipeline_8` / `_16`, the AVX2 / AVX-512 / NEON pipelines), `integer_motion_v2.c`, `float_motion.c`; git history of `integer_motion.c` (PR #532, port of Netflix `a4a1492d`, "replace integer_motion with pipelined v2 variant").
- Twins: `core/src/feature/sycl/integer_motion_sycl.cpp`, `integer_motion_v2_sycl.cpp`, `float_motion_sycl.cpp`; CUDA `cuda/integer_motion/motion_score.cu`, HIP `hip/integer_motion/motion_score.hip`, Metal `metal/integer_motion.metal` (read only).
- Host: i9-12900K, Windows 11 + WSL2 `/dev/dxg`, `vmaf-dev-mcp:ocloc`, oneAPI 2026.1 icpx, AOT `bmg-g21,adl-s`; `ONEAPI_DEVICE_SELECTOR=level_zero:0` Arc B580, `level_zero:1` UHD 770.
- Fixtures: synthetic ramp-plus-noise 4:4:4 sequences (6 frames, 8 and 10 bits) at 17x17, 19x23, 33x33, 64x64, 257x145 and 576x324; the Netflix `src01_hrc00/01_576x324` pair (48 frames); BBB 3840x2160 (first 24 frames). CPU reference runs used `--cpumask 4294967295` (scalar); the SIMD pipelines gave identical scores on every fixture.

## Findings

### The cause: the SYCL twin blurred each frame

The CPU sums `|H(V(prev - cur))|`, where `V` and `H` are the 5-tap filter with rounding `(sum + 2^(bpc-1)) >> bpc` and `(sum + 2^15) >> 16`. Upstream moved to this order when it replaced `integer_motion.c` with the pipelined variant (fork PR #532). `motion_sycl` still computed the pre-port form `|H(V(cur)) - H(V(prev))|`, storing each blurred frame as int32. Without rounding the two are equal by linearity; with it, each pass can leave one unit per pixel, in either direction. Summed over N pixels and divided by 256 N, the error behaves like a random walk, about `1 / (256 sqrt(N))`: 2.3e-4 predicted at 17x17 against 2.0e-4 measured, 6.1e-5 against 4.9e-5 at 64x64. Perimeter over area falls like `1 / sqrt(N)` too for square frames, which is why the row read it as an edge effect; the mirror (`2 * size - idx - 2`), the taps and the normalisation were the CPU's. Differencing first makes the twin compute exactly what the CPU computes; replacing only the order (same tiling, same reflection) takes every difference to zero.

### Before and after

Worst per-frame |SYCL - scalar CPU| of `integer_motion2` (`integer_motion3` is the same or smaller). The Arc B580 and the UHD 770 gave identical numbers.

| Fixture | 8-bit before | 10-bit before | After (both depths, both GPUs) |
|---|---|---|---|
| 17x17 | 2.03e-4 | 1.76e-4 | 0 |
| 19x23 | 1.70e-4 | 1.97e-4 | 0 |
| 33x33 | 1.33e-4 | 8.61e-5 | 0 |
| 64x64 | 4.86e-5 | 3.15e-5 | 0 |
| 257x145 | 1.52e-5 | 3.75e-5 | 0 |
| 576x324 synthetic | 6.89e-6 | 3.83e-6 | 0 |
| Netflix 576x324, 48 frames | 1.26e-5 | — | 0 |
| BBB 3840x2160, 24 frames | 5.56e-6 | — | 0 |

After the change the same holds with the combined graph forced on (`VMAF_SYCL_USE_GRAPH=1`) and off (`VMAF_SYCL_NO_GRAPH=1`). `test_sycl_motion_tiny_frames` adds 3x3, 4x5, 31x9 and 1283x723 (graph replay) at 8, 10 and 16 bits with independent full-range noise per frame, which drives the 16-bit vertical sum past int32; it compares with `==` and fails against the old twin (3x3: `motion3` 19.374131944444443 against 19.373697916666668). The default-model VMAF at 4K moves by at most 4e-6.

### The other twins

- `motion_v2_sycl` already differenced first and matched the CPU `motion_v2` exactly at every fixture before and after; it now runs the same kernel.
- `float_motion_sycl` blurs each frame, as the CPU `float_motion` does, so the order is the same. Its differences are float rounding that grows with the frame (5.6e-7 at 17x17, 2.7e-5 at 4K), unchanged by this work.
- The CUDA (`motion_score.cu`), HIP (`motion_score.hip`) and Metal (`integer_motion.metal`) `motion` twins blur each frame into a uint16 buffer and difference the blurred frames: the pre-port order, so the same gap. Their `motion_v2` twins difference first.

### Cost

Two raw planes per tile instead of one, but no int32 blurred planes. Kernel time at 4K, profiling events, 50 runs (scratch micro-benchmark):

| Device | Old per-frame blur | Diff-first | Device copy of `cur` |
|---|---|---|---|
| Arc B580 | 0.484 ms | 0.525 ms | 0.013 ms |
| UHD 770 | 6.15 ms | 6.65 ms | 0.17 ms |

Storing `cur` from inside the kernel instead of the copy measured 0.517 ms and 6.88 ms, no better, so the code uses the copy. Per-frame device time of a motion-only 4K run (`VMAF_SYCL_TIMING`, 39 frames from memory, median of 4): B580 7.7 → 7.7 ms, UHD 770 16.6 → 18.9 ms. Default model at 4K (60 frames, 3 runs): B580 52.8 / 53.4 / 52.8 → 53.2 / 52.5 / 57.3 ms per frame, UHD 770 90.5 / 84.6 / 78.6 → 92.2 / 81.8 / 81.9 ms, within run-to-run noise.

### `motion_add_uv` host wait

`motion_upload_chroma` copied U and V from the picture with `vmaf_sycl_memcpy_h2d_async` on the primary queue and then called `vmaf_sycl_queue_wait()`, because the kernels run on the combined queue (ADR-1034). Staging the planes into pinned memory in `submit()` and copying them in the combined queue's `pre_fn` orders them ahead of the kernels with no host wait: the graph for a frame is enqueued by the last extractor to submit, after staging, and the extractor's `collect()` of the previous frame has drained the copy that last read the staging. Output is bit-identical to the fixed-point oracle of `test_sycl_motion_add_uv_parity` (256x144 and 960x540). 4K, 39 frames from memory, median of 4 (`motion_sycl=motion_add_uv=true`):

| Device | Host time per frame, before → after | Frame time, before (old kernel) / with the new kernel and the wait / after |
|---|---|---|
| Arc B580 | 0.89 → 0.55 ms | 8.58 / 8.71 / 7.95 ms |
| UHD 770 | 5.4 → 0.6 ms | 23.0 / 25.0 / 21.8 ms |

### A state shared across frame sizes

Running every geometry of the new test on one `VmafSyclState` scored 4x5 wrong after 3x3: `vmaf_sycl_shared_frame_init()` returns early once the shared buffers exist, so a second context importing the same state with a larger frame gets buffers sized for the first one. The test now opens a state per case; the library gap is `T-SYCL-SHARED-FRAME-GEOMETRY-REUSE-2026-09-29`.

### `motion_add_uv` chroma geometry

Reading `motion_configure_chroma()` for the staging change: the chroma size is `(w + 1) / 2` by `(h + 1) / 2` whatever the pixel format, so 4:2:2 and 4:4:4 input stage only part of each chroma plane. Not run; recorded as `T-SYCL-MOTION-ADD-UV-CHROMA-GEOMETRY-2026-09-29`.

## Conclusion

The parity gap was the order of differencing and blurring, a leftover of the upstream motion rewrite that the GPU twins never followed. Sharing one diff-first kernel between `motion_sycl` and `motion_v2_sycl` makes both bit-exact at every size and depth measured, for a 4K SAD step about 11% slower; the `motion_add_uv` staging change removes the per-frame host wait. The CUDA, HIP and Metal `motion` twins need the same arithmetic.
