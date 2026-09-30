<!-- markdownlint-disable MD013 MD060 -->
# Research-1379: Device-resident CAMBI and SpEED on CUDA — the CUDA rounding contract, the CPU's libm, and verification by emulation and on an RTX 4090

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
3. How can the port be checked on a machine without an NVIDIA GPU, and what
   does it do on one?

## Sources

- `core/src/feature/speed.c`, `speed_internal.c`, `vif_tools.c`, `cambi.c`
  (CPU references); `core/src/feature/sycl/speed_sycl_pipeline.cpp` and
  `integer_cambi_sycl.cpp` (the ports); [Research-1358](1358-sycl-speed-device-resident.md)
  and [Research-2122](2122-sycl-cambi-device-resident.md).
- CUDA 13.4 (`nvcc` V13.4.92) in the `vmaf-dev-mcp` image, building the fatbin
  for `sm_80`, `sm_86`, `sm_89`, `sm_90`, `sm_100` and `sm_120`.
- glibc 2.43 (Ubuntu 26.04) and Intel's libimf from oneAPI 2026 on an
  i9-12900K, without an NVIDIA device (findings 1 to 6).
- `ryzen-4090-arc`: an RTX 4090 (sm_89, driver 615.71.09, CUDA 13.4) and an
  Arc A380 (Level Zero 1.17.39758) with a Ryzen 9 9950X3D, glibc 2.44 and icx
  2026.0.0 (findings 7 and 8), 2026-09-30.

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

### 7. The fp32-pair `log2` and its 48 hard cases

`speed_log2()` carries about 2^-45 of relative error before its final
rounding. The pair algorithm was replayed on every positive finite float
(0x00000001 to 0x7f7fffff, 2 139 095 039 inputs) against a correctly rounded
reference: `(float)log2((double)x)`, except for the 49 987 inputs whose
double result lies within 2^-40 (relative) of a binary32 rounding boundary,
which MPFR decides at 24 bits, round to nearest. 48 inputs round the wrong
way, all in two mantissa families: 0x1.aa932c at exponents -32 to -17 and 16
to 31, and 0x1.ff800c at -16 to -9 and 8 to 15. No other input misrounds.
Both twins now compare the fraction field against those two mantissas and
look the input up in `core/src/feature/speed_log2_hard_cases.h`, which holds
the correctly rounded outputs; the common path costs one compare.
`core/test/test_cuda_device_resident_contract.py` recomputes every table
entry in quad precision.

The same replay against the device code, on the device, run on
`ryzen-4090-arc` against this branch's sources:

| Device | Inputs | `speed_log2()` != correctly rounded | Special values (0, -0, -1, +-inf, NaN, -denormal) |
|---|---:|---:|---|
| RTX 4090 (`speed_score.cu`, `nvcc --fmad=false -arch=sm_89`) | 2 139 095 039 | 0 | all as `log2f` |
| Arc A380 (`speed_sycl_pipeline.cpp`, icpx `-fsycl -fp-model=precise -ffp-contract=off`, JIT) | 2 139 095 039 | 0 | all as `log2f` |

### 8. On an RTX 4090

Built as the state rows prescribe, from the repository root:
`CC=icx CXX=icpx meson setup build-cuda-icx core -Denable_cuda=true
-Denable_nvcc=true -Denable_sycl=false -Denable_hip=false --buildtype=release
-Db_lto=false`, then `ninja -C build-cuda-icx`. "Before" is `origin/master`
at `10f27efe2`, built the same way. Each device run held the RTX 4090 alone;
the host ran other jobs (load average 13 to 41).

**Tests.** `python3 scripts/ci/run_meson_test.py -- -C build-cuda-icx
--no-rebuild test_cuda_cambi_parity test_cuda_cambi_parity_large
test_cuda_device_resident_contract test_cuda_speed_chroma_parity
test_cuda_speed_temporal_parity test_cuda_speed_singular_parity
test_cuda_speed_chroma_smoke test_cuda_speed_temporal_smoke`: 8 OK, none
skipped. `test_cuda_speed_temporal_parity_1080p`, the 1920x1080 build of the
temporal parity test added for the launch failure below, passes too; built
against `10f27efe2` it fails with that `CUDA_ERROR_INVALID_VALUE`. `compute-sanitizer --tool memcheck`, `--tool racecheck` and
`--tool synccheck` on `test_cuda_cambi_parity`, `test_cuda_cambi_parity_large`,
`test_cuda_speed_singular_parity`, `test_cuda_speed_temporal_parity` and
`test_cuda_speed_chroma_parity`: 0 errors and 0 hazards on all fifteen runs.

**Parity.** `python3 scripts/dev/speed_gpu_parity.py --backend cuda --vmaf
$PWD/build-cuda-icx/tools/vmaf --netflix-dir python/test/resource/yuv
--bbb-dir testdata/bbb` (and `--feature cambi`), `--precision max`, identical
frames per output:

| Output | 576x324 after | 3840x2160 after | 576x324 before | 3840x2160 before |
|---|---:|---:|---:|---:|
| `cambi` | 48/48 | 50/50 | 48/48 | 50/50 |
| `speed_chroma_u` | 48/48 | 50/50 | 5/48 | 9/50 |
| `speed_chroma_v` | 48/48 | 50/50 | 3/48 | 8/50 |
| `speed_chroma_uv` | 48/48 | 50/50 | 1/48 | 10/50 |
| `speed_temporal` | 48/48 | 50/50 | 8/48 | run fails |

Before, the largest difference was 4.0e-5 (`speed_chroma_u`, 576x324), and
`speed_temporal_cuda` failed at 1920x1080 and 3840x2160: its solve launch
asked for `((nb + 7) / 8) * 32` threads per block for `nb` 5x5 blocks (312 at
1080p, 1296 at 4K), past the 1024-thread limit, and `cuLaunchKernel` returned
`CUDA_ERROR_INVALID_VALUE` (`speed_temporal_cuda.c:425` at `10f27efe2`). A gcc
16.2.1 build of this branch against its own CPU extractor (glibc 2.44) exits
1: `speed_chroma_u` 48/48 and 49/50, `speed_chroma_v` 47/48 and 49/50,
`speed_chroma_uv` 47/48 and 48/50, at most 1.4e-6 apart; `speed_temporal`
48/48 and 50/50.

With `speed_prescale=0.5` on 6 frames of the BBB 3840x2160 pair, the
`speed_chroma_cuda` outputs equal the CPU on 6/6 frames for nearest, bilinear
and bicubic prescale. `lanczos4` matches on 1 of 18 outputs, at most 1.9e-5
(3.1e-6 relative) apart on this content, and at most 2.5e-5 on 8 frames of BBB
scaled to 1920x1080. Finding 4 holds on the device for smooth content: on an
ffmpeg `gradients=s=1920x1080:r=25:speed=0.02:seed=7` clip, 6 frames, with the
distorted side through `noise=alls=6:allf=t`, `speed_chroma_v` is up to 2.1e-2
(5.7e-4 relative) from the CPU and no frame matches, while bicubic matches on
6/6.

**Wide, short frames.** Banded 8-bit ramps (`floor(90 + 12 x / W + frame)`,
3 frames) at 1920x64, 1920x128, 1920x160 and 3840x128, the sizes of
`T-CAMBI-SHORT-FRAME-OOB-2026-09-30`: `cambi_cuda` under
`compute-sanitizer --tool memcheck` reports 0 errors and equals, on every
frame, the CPU `cambi` of the `fix/cambi-short-frame-oob` build (6.0056,
7.7220, 8.0805 and 6.0946 per size). The hybrid `cambi_cuda` of `10f27efe2`
scored frame 0 at 5.1739, 7.4273, 7.7122 and 5.2785, the later frames at 0,
and then died in `close_fex_cuda()` (SIGSEGV through a corrupted picture
reference, or SIGABRT) on all four sizes.

**Driver calls per frame.** A CUPTI injection library
(`CUDA_INJECTION64_PATH`) counted driver-API calls by name, kernel launches
by symbol, and copies by direction and bytes; per frame is (count at 22
frames - count at 12 frames) / 10 on the 576x324 pair. The engine's own calls
(six 2D picture uploads of 559 872 B in all, one context synchronisation, two
event synchronisations) appear in every row and are left out:

| Twin | Launches | Device-to-host | Host-to-device | Stream syncs |
|---|---:|---|---:|---:|
| `cambi_cuda` after | 65 | one copy, 88 B | 0 | 1 |
| `cambi_cuda` before | 19 | 11 2D copies, 1 181 232 B (the picture, then the image and mask at five scales) | one 2D copy, 373 248 B (the preprocessed picture) | 7 |
| `speed_chroma_cuda` after | 7 | one copy, 40 B | 0 (four 2D device-to-device plane copies) | 1 |
| `speed_chroma_cuda` before | 18 | 20 copies, 344 368 B | 16 | 6 |
| `speed_temporal_cuda` after | 7 | one copy, 40 B | 0 (two 2D device-to-device plane copies) | 1 |
| `speed_temporal_cuda` before | 9 | 10 copies, 674 600 B | 8 | 6 |

The device counts equal the emulated ones of finding 6.

**Time.** Milliseconds per frame, `(t(N) - t(2)) / (N - 2)`, median of 3,
before and after runs alternating so both see the same host load; the CPU
column is `--backend cpu --threads 16` of the same build. With the rows' N =
22 the 576x324 differences were below the run-to-run noise on this host
(single samples from -2.9 to 9.5 ms), so the table uses N = 102 at 3840x2160
and N = 402 at 576x324 (the Netflix pair looped ten times):

| Twin | Size | Before | After | CPU before | CPU after |
|---|---|---:|---:|---:|---:|
| `cambi_cuda` | 3840x2160 | 64.71 | 6.01 | 21.49 | 19.65 |
| `speed_chroma_cuda` | 3840x2160 | 24.90 | 6.89 | 9.07 | 9.11 |
| `speed_temporal_cuda` | 3840x2160 | fails | 5.88 | 26.38 | 26.74 |
| `cambi_cuda` | 576x324 | 1.59 | 0.38 | 0.08 | 0.09 |
| `speed_chroma_cuda` | 576x324 | 1.51 | 0.44 | 0.15 | 0.12 |
| `speed_temporal_cuda` | 576x324 | 2.81 | 0.42 | 1.40 | 0.92 |

An earlier interleaved run with the rows' own N = 22 at 3840x2160 gave
68.39 -> 3.67 (`cambi_cuda`), 14.29 -> 5.81 (`speed_chroma_cuda`) and fails
-> 5.06 (`speed_temporal_cuda`). The absolute numbers move with the host's
load (the CLI reads about 25 MB per 4K frame pair); the relation to a trivial
twin does not. A later run with N = 102 at load average 13, `psnr_cuda` before
and after the three twins: `psnr_cuda` 2.56 and 2.41 ms per frame,
`cambi_cuda` 2.55, `speed_chroma_cuda` 2.75 and `speed_temporal_cuda` 2.73. At
4K the three twins now run at the per-frame cost of reading and uploading the
pictures, so faster kernels would not show in the CLI's time. The CPU columns show that the `cambi.c` and
`speed_internal.c` refactor did not change the CPU extractors' cost beyond
the noise.

## Open questions

- `lanczos4` prescale: computing the kernel weights on the host once per run,
  with `vif_tools.c`'s own fp64 code, would make it exact on both device twins
  (`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`).
- Whether the parity script should accept a separate CPU binary, so a gcc
  CUDA build can be compared against the icx CPU build directly.
