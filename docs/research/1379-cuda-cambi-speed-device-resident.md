<!-- markdownlint-disable MD013 MD060 -->
# Research-1379: Device-resident CAMBI and SpEED on CUDA — the CUDA rounding contract, the CPU's libm, and verification without an NVIDIA device

- **Status**: Active
- **Workstream**: [ADR-1379](../adr/1379-cuda-cambi-device-resident-pipeline.md), [ADR-1380](../adr/1380-cuda-speed-device-resident-pipeline.md), [ADR-1357](../adr/1357-sycl-cambi-device-resident.md), [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md)
- **Last updated**: 2026-09-30

## Question

`cambi_cuda`, `speed_chroma_cuda` and `speed_temporal_cuda` round-tripped
through the host every frame (`T-CUDA-CAMBI-HOST-RESIDUAL-2026-09-29`,
`T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29`). The SYCL twins already run the whole
chain on the device and match the CPU extractor. Three questions followed for
the CUDA port:

1. What does CUDA need to reproduce the CPU's fp32 rounding, given that the
   SYCL port had to correct division and square root by hand?
2. Is "bit-identical to `--backend cpu`" a property of the twin alone, or also
   of how the CPU extractor was built?
3. How can the port be checked on a machine without an NVIDIA GPU?

## Sources

- `core/src/feature/speed.c`, `speed_internal.c`, `vif_tools.c`, `cambi.c`
  (CPU references); `core/src/feature/sycl/speed_sycl_pipeline.cpp` and
  `integer_cambi_sycl.cpp` (the ports); [Research-1358](1358-sycl-speed-device-resident.md)
  and [Research-2122](2122-sycl-cambi-device-resident.md).
- CUDA 13.4 (`nvcc` V13.4.92) in the `vmaf-dev-mcp` image, building the fatbin
  for `sm_80`, `sm_86`, `sm_89`, `sm_90`, `sm_100` and `sm_120`.
- glibc 2.43 (Ubuntu 26.04) and Intel's libimf from oneAPI 2026 on an
  i9-12900K; no NVIDIA device.

## Findings

### 1. CUDA spells the CPU's rounding directly

`__fadd_rn`, `__fsub_rn` and `__fmul_rn` round once to nearest and nvcc never
fuses them into an FMA, whatever `--fmad` says. `__fdiv_rn` and `__fsqrt_rn`
are correctly rounded; SYCL's `div_rn()` / `sqrt_rn()` residual refinement
exists only because icpx's division and square root are not. The CUDA kernels
therefore write every rounding-relevant operation with those intrinsics and
also build `speed_score.cu` with `--fmad=false`, so a plain operator added
later cannot fuse either. libdevice `log2f` is not correctly rounded (the CUDA
Programming Guide lists a 1 ulp bound), so `speed_log2()` keeps the SYCL
port's fp32-pair evaluation, rounded once. The top-K fixed-point conversion in
`cambi_score.cu` is `__float2ull_rz(__fmul_rn(value, 2^24))`, exact for every
c-value.

### 2. The CPU extractor's SpEED score depends on the libm's `log2f`

`speed.c` calls `log2f` about 25 times per block and channel. Over every float
in [1, 1024), glibc 2.43's `log2f` differs from the correctly rounded result on
115 329 of 83 886 080 inputs (0.137 %), and on 0.563 % of [0.25, 4); Intel's
libimf differs on 113 (0.000135 %) and 0.00113 % of the same ranges (checked
against `(float)log2l(x)`, which agrees with `(float)log2(x)` on every input).

On the Netflix 576x324 pair (48 frames), `speed_chroma` from a gcc build of
the CPU extractor and from the icx build of the same source are identical on
42 of 48 `speed_chroma_u` frames and 45 of 48 `speed_chroma_v` frames, at most
4.8e-7 apart. With `log2f` replaced by a correctly rounded one
(`LD_PRELOAD` of `(float)log2((double)x)`), the gcc build equals the icx build
on all 48 frames, and both equal the CUDA twin. The device twins, which round
`log2` correctly, match the CPU exactly where the CPU's `log2f` rounds the
arguments it meets correctly, as an icx build's libimf does on these inputs.
ADR-1358's SYCL measurements were taken with `scripts/dev/speed_gpu_parity.py`
against an icx build of the tree, which is why they were identical.

The CPU reference must not contract FMAs either. The `vmaf` the
`vmaf-dev-mcp` image installs (`dev/Containerfile`: icx with
`-Dc_args=-march=native`, so FMA instructions are available and icx fuses
multiply-adds in `speed.c`) differs from a plain `CC=icx meson setup` build of
the same source by up to 3.3e-5 on the Netflix pair and 7.9e-4 on a 1080p
clip (`speed_chroma_u`). The device reproduces the uncontracted build.
Tracked as `T-DEV-IMAGE-ICX-NATIVE-FMA-DRIFT-2026-09-30`.

### 3. The CUDA CAMBI top-K sum is exact

On a synthetic, heavily banded 3840x2160 clip (4 frames), the CPU and the CUDA
twin differ on every frame, by at most 3.0e-13. A diagnostic CPU build whose
`average_topk_elements()` sums in `long double` (64-bit mantissa, exact for
any CAMBI sum) equals the CUDA twin on all four frames: the difference is the
rounding of the CPU's own sequential `double` sum, as ADR-1357 derived. Every
other fixture, including 4K with `cambi_high_res_speedup=2160`, is identical.

### 4. `lanczos4` prescale is outside the ADR-0214 tolerance on both twins

`lanczos4_kernel()` evaluates `a * sin(pi x) * sin(pi x / a) / (pi^2 x^2)` in
fp64 and rounds once; the device evaluates it in fp32 with `sinpif`, as the
SYCL twin does with `sycl::sinpi`. With `speed_prescale=0.5` at 1920x1080 the
CUDA `speed_chroma_u` differs from the CPU by up to 4.4e-4 relative (1.1e-2
absolute), beyond the 1e-4 of ADR-0214, not merely "within tolerance" as
ADR-1358 states for SYCL. Nearest, bilinear and bicubic prescale are exact.
Tracked as `T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`.

### 5. The CPU `speed_temporal` crashes with `speed_prescale > 1`

`speed_temporal`'s `init()` allocates its four frame buffers as
`float_stride * h`, the source height, while `filter_and_downscale()` resamples
into them at `scaled_height`, which exceeds `h` when the prescale is above 1.
`--feature speed_temporal=speed_prescale=1.5` aborts with `free(): invalid
size` on the 576x324 pair. `speed_chroma` sizes the same buffers with
`alloc_height` and is unaffected; upstream Netflix `speed.c` has the same line.
The CUDA twin sizes its planes from the scaled geometry and runs. Tracked as
`T-SPEED-TEMPORAL-PRESCALE-UP-OVERFLOW-2026-09-30`.

### 6. Verification without an NVIDIA device

The fatbin compiles for every configured architecture, but compiling proves
nothing about the arithmetic. The kernels were therefore also compiled as
host C++ (`g++ -O2 -ffp-contract=off -fno-fast-math`) and run behind a
host-memory replacement of the CUDA driver library that `vmaf` loads as
`libcuda.so.1`. Every CUDA thread of a block runs as a cooperative fiber, so
`__syncthreads()`, warp shuffles and `__ballot_sync` are real rendezvous
points, and each intrinsic maps to the IEEE host operation it names. The
tooling is local verification, not part of the repository. What it checks: the
kernels' algorithms, indexing, barrier and warp-collective use, and the host
orchestration and transfer pattern. What it cannot check: nvcc's code
generation, races between blocks (blocks run one after another), memory-model
effects and speed.

Against the CPU extractor (gcc build; for SpEED with the correctly rounded
`log2f` of finding 2), every per-frame output is identical except where noted.
The fixtures are the 576x324 pair from `testdata/` and clips generated with
FFmpeg's `gradients` source (8-bit steps, which CAMBI scores as banding) and
`testsrc2` source, with noise added to the distorted side:

| Fixture | CAMBI | `speed_chroma` | `speed_temporal` |
|---|---|---|---|
| 576x324, 48 frames, 8-bit | 48/48 | 48/48 (u, v, uv) | 48/48 |
| 1920x1080 gradient, 6 frames, 8-bit | 6/6; `hrs=1080` 6/6; `enc` 1280x720 6/6; `enc` 1366x767 3/3 | — | — |
| 1920x1080 gradient, 4 frames, 10-bit | 4/4; `enc_bitdepth=8` 4/4; `eotf=pq` 4/4 | — | — |
| 1920x1080 `testsrc2`, 4 frames, 10-bit | — | 4/4 | 4/4 |
| 3840x2160 gradient, 4 frames | 0/4, max 3.0e-13 (finding 3); `hrs=2160` 4/4 | — | — |
| 3840x2160 `testsrc2`, 4 frames | 4/4 | 4/4 | 4/4 |
| 576x324, `window_size=31`, `topk=0.25`, `max_log_contrast=5`, `tvi_threshold=0.01`, `vlt=5` | 12/12 | — | — |
| 1920x1080 gradient, prescale 0.5 bilinear / 0.7 nearest / 1.5 bicubic | — | 6/6 each | 0.5 bicubic 6/6 |
| 576x324 and 1920x1080, `ks=2`, `snn=0.5`, `nnf=0.1`, `wvm` 3 / 5 / 6 | — | 12/12, 6/6, 6/6 | — |
| 576x324, `speed_use_ref_diff` | — | — | 24/24 |
| 1920x1080 gradient, prescale 0.5 `lanczos4` | — | 0/6 (finding 4) | — |

Every row has non-zero CPU scores on most frames; the `speed_temporal` first
frame is 0 by definition.

`test_cuda_cambi_parity` (bit-exact per frame, 256x256 and the 960x540
`_large` build) and the SpEED parity, singular and smoke tests pass under the
emulation and exit 77 without a device.

Per frame on the 576x324 pair, counted in the emulated driver over frames 3 to
12, the twins add no host-to-device transfer of their own:

| Twin | Kernel launches | Device-to-host | Other per-frame driver work | Waits |
|---|---|---|---|---|
| `cambi_cuda` | 65 (66 with validation) | one 88-byte `CambiCudaResults` | two `cuMemsetD8Async` | one stream synchronisation, in `collect()` |
| `speed_chroma_cuda` | 7 | one 40-byte `SpeedGpuFrameResult` | four device-to-device plane copies | one, in `collect()` |
| `speed_temporal_cuda` | 7 | one 40-byte `SpeedGpuFrameResult` | two device-to-device plane copies | one, in `collect()` |

The engine's own per-frame picture uploads and its ADR-1199 context
synchronisation are the same for every CUDA extractor and are not counted.

## Open questions

- Device parity and speed on an RTX 4090: the commands are in the two state
  rows, to be run on `ryzen-4090-arc`.
- Whether the parity script should accept a separate CPU binary, so a gcc
  CUDA build can be compared against the icx CPU build directly.
