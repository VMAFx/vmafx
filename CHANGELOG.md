# Change Log

> The Unreleased section tracks VMAFx changes. Release-please turns these
> entries and Conventional Commits into ordinary SemVer releases.

## [Unreleased]
### Added

- **`scripts/dev/hip_dispatch_drop_probe.hip` checks whether an AMD GPU runs
  every command of a HIP stream.** Built with `hipcc`, it runs frames of one
  memset, several small kernels and a readback on one stream and reports the
  frames with wrong results and the kernel launches that never ran. On the
  maintainers' gfx1036 iGPU (ROCm 7.2.4) about one frame in 10^4 loses a run
  of its commands, which makes a HIP twin report a wrong score for that frame
  on master as well; the probe tells whether a driver update fixed it. See
  [the HIP backend guide](docs/backends/hip/overview.md#known-issue-the-gfx1036-loses-stream-commands)
  (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`).


- **`vmaf_feature_backend_twin()` and `vmaf_registered_feature_extractor()`**
  in `libvmaf.h`. The first tells a caller which device twin model dispatch
  would use for a CPU extractor on the context's backend, and whether that twin
  can honour the given options and picture size. The second lists the
  registered extractors and the backend each one runs on. Both are additive;
  `vmaf_use_feature()` still selects by exact name. See
  [the C API reference](docs/api/index.md#device-twins-and-the-extractors-that-ran).
- **`feature_backends` in the CLI's JSON output**: one
  `{"extractor": ..., "backend": ...}` entry per registered extractor, next to
  `backend_used`, so a run that mixes device twins and CPU extractors says so.


- **RC3 home GPU retest kit** (`scripts/dev/rc3-home-gpu-retest.sh`): runs the
  verify-and-time commands that the `docs/state.md` rows carry for the RTX 4090
  (CUDA), the Arc A380 (SYCL) and the gfx1036 iGPU (HIP), one entry per row,
  with every device run under that device's lock. It writes a log and the JSON
  of every run per row plus a summary table, and `--baseline DIR` compares a
  pull request's run with an earlier run on `master`. See
  [the retest guide](docs/development/rc3-home-gpu-retest.md) (ADR-1386).


### Changed

- Migrated the Windows MSYS2 MinGW build matrix leg in
  `.github/workflows/libvmaf-build-matrix.yml` from the deprecated `MINGW64`
  environment linking legacy `msvcrt.dll` to `UCRT64` linking the Universal C
  Runtime (`ucrtbase.dll`), using `mingw-w64-ucrt-x86_64-*` packages. Updated the
  required status check name in `.github/workflows/required-aggregator.yml` to
  `Windows UCRT64` (ADR-1387, #1609).


- **CUDA twins take the CPU extractor's options and arithmetic (ADR-1373).** `psnr_cuda`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the same `core/src/feature/psnr_score.h` helpers as the CPU
  extractor, `apsnr_*` aggregates included; `integer_ssim_cuda` accepts
  `enable_db` / `clip_db`; `float_ssim_cuda` accepts `enable_lcs` (a device
  kernel reduces the L, C and S terms) / `enable_db` / `clip_db`; and
  `float_motion_cuda` accepts `motion_max_val` and weights its debug `motion`
  score by `motion_fps_weight` like the CPU. Before, a model or a
  `--backend cuda --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. `float_ssim_cuda` now computes the
  CPU's per-pixel `l * c * s` with the CPU's rounding and rounds the frame
  mean to fp32, so with `enable_db` identical frames report the CPU's value
  (identical flat frames: 72.247 dB, where the twin reported `+inf`);
  `integer_ssim_cuda` computes each pixel's term as the CPU does;
  `motion_v2_cuda` publishes the CPU's fps-weighted, capped SAD score and
  emits `motion2_v2` / `motion3_v2` for a one-frame input; `psnr_cuda` sums
  every frame into `apsnr_*` under `--subsample`, and its chroma accumulators
  can no longer be cleared while the chroma kernels run. `float_ssim_cuda`
  still accepts `enable_chroma`, which the CPU `float_ssim` does not have, and
  warns that it is ignored. Measured on an RTX 4090: the PSNR, `motion_v2`
  and `float_motion` options give the CPU's scores exactly, `ssim` stays
  within 7.3e-13 dB and `float_ssim` within 6.9e-6 dB of the CPU; see
  [the CUDA backend guide](docs/backends/cuda/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins).


- **Four HIP twins take the CPU extractor's options (ADR-1382).** `psnr_hip`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the CPU's own `core/src/feature/psnr_score.h`, `apsnr_*` aggregates
  included; `integer_ssim_hip` accepts `enable_db` / `clip_db`,
  `float_ssim_hip` `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s`
  computed on the device), and `float_motion_hip` `motion_max_val`, with its
  debug `motion` score now weighted by `motion_fps_weight` like the CPU's.
  Before, a model that set one of these options computed the feature on the
  CPU, and naming the twin with the option failed with `unknown option`.
  `float_ssim_hip` now scores each pixel as the CPU does (`l * c * s` from the
  CPU's luminance, contrast and structure terms) and rounds the frame mean to
  fp32 like the CPU, so with `enable_db` identical frames report what the CPU
  reports (72.247 dB on a flat frame, where the CPU's fp32 arithmetic leaves
  1 - 2^-24) instead of a forced `+inf`; `integer_ssim_hip` scores identical
  frames from 3x3 up exactly 1, as the CPU does. `motion_v2_hip` now stores
  its SAD weighted by `motion_fps_weight` and capped at `motion_max_val` like
  the CPU (it weighted at fold time and never capped) and scores one-frame
  runs; `psnr_hip` now sees every frame under `--subsample`, so `apsnr_*`
  covers the whole clip; `motion_hip` defaults `debug` to false and emits
  `VMAF_integer_feature_motion_sad_score`, as the CPU `motion` does. The
  parity gate (`scripts/ci/cross_backend_parity_gate.py`) takes `--backends
  hip` and a `float_ssim_lcs` cell. On a gfx1036 `psnr_hip` with all four
  options matches the CPU exactly, `apsnr_*` included, and the parity gate
  passes every HIP cell; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).


- **`vmaf` reads its two inputs ahead of scoring, on one thread each
  (ADR-1366).** The CLI used to read the reference frame, then the distorted
  frame, then score the pair, all on one thread; at 3840x2160 the two reads
  cost about 7 ms per frame whatever the backend did. Each input now has a
  reader thread that stays up to two frames ahead, so reading overlaps scoring
  and the two files are read at the same time. At 3840x2160 8-bit 4:2:0,
  `--feature psnr` drops from about 7-8 to about 3.5-4 ms per frame on the CPU,
  serial or with `--threads 16`, and `psnr`, `motion` and `adm` on an Arc B580
  from about 8 to about 4; runs limited by extraction keep their speed.
  Scores, frame order, `--frame_cnt`, `--frame_skip_*`, the progress line and
  the exit codes are unchanged, and the JSON is identical at
  `--precision max`. The picture pool holds four more pictures (about 50 MB at
  4K 8-bit). Inputs that may share a read position are still read on the main
  thread: on Linux and macOS the same file or pipe on both sides and
  `--no-reference`, on Windows anything but two regular files. See
  [Input read-ahead](docs/usage/cli.md#input-read-ahead).


- **`cambi_cuda` runs entirely on the device (ADR-1379).** The CUDA CAMBI twin
  no longer downloads the distorted picture, preprocesses it on the host or
  reads the image and mask back at each of the five scales for host c-values
  and pooling: every stage runs on the GPU, the twin reads the plane the CUDA
  engine already uploaded, and each frame reads back 88 bytes and waits once,
  in `collect()`. Scores equal `--backend cpu` to the last bit whenever the
  CPU's own double top-K sum is exact, and otherwise differ only by that sum's
  rounding. Like `cambi.c`, the twin now rejects an adjusted window above
  65 x 65 ("cambi: window_size N too large for reciprocal LUT") instead of
  reading past the reciprocal table. On an RTX 4090 every frame of the Netflix
  576x324 pair and of BBB 3840x2160 equals `--backend cpu`, and a 4K frame
  takes 6.01 ms instead of 64.71 ms. The old twin also crashed on wide, short
  frames such as 1920x128, where the new one matches the fixed CPU extractor.
  See [CAMBI](docs/metrics/cambi.md#cuda).


- **CUDA `psnr_hvs` reads the device pictures directly**
  (`T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29`, the CUDA port of ADR-1369).
  `psnr_hvs_cuda` no longer copies each frame to the host, converts it there and
  uploads float planes: the kernel reads the raw 8- to 12-bit samples, two threads
  per 8x8 block, one launch for every plane. Output is bit-identical to the previous
  twin at 8, 10 and 12 bits; 9- and 11-bit input, which it scored as -1.57 dB and NaN
  (`T-CUDA-PSNR-HVS-ODD-BPC-2026-09-30`), now matches the CPU. On an RTX 4090 a
  3840x2160 frame takes 3.20 ms instead of 18.28 ms (1920x1080: 0.43 instead of
  4.40). See
  [the CUDA backend guide](docs/backends/cuda/overview.md) and
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#gpu-twins), which also explains why
  the CPU extractor differs from every GPU twin by up to 1.1e-2 dB at 3840x2160.


- **`psnr_cuda`, `float_moment_cuda` and the CUDA motion SAD kernel spend
  far less GPU time per frame (ADR-1392).** The kernels added one atomic per
  warp to their 64-bit accumulators, which serialised them in the L2; they
  now add one per block and accumulator. PSNR and moment threads sum eight
  coalesced pixels each, PSNR selects its plane without copying both
  pictures to every thread's stack, and the motion SAD kernel (shared by
  `motion_cuda` and `motion_v2_cuda`) computes its vertical filter pass once
  per block. On an RTX 4090 with 3840x2160 8-bit frames the PSNR kernel drops
  from 1,750.5 to 17.7 us per frame, the motion SAD kernel from 136.5 to
  59.6 us and the moment kernel from 460.0 to 15.3 us (CUPTI, median of three
  traces); scores are unchanged. The whole-frame time barely moves, because
  copying each 4K frame to the device takes about 2.1 ms on that host's PCIe
  link, far longer than any of these kernels now runs.


- **The CUDA SpEED twins run entirely on the device (ADR-1380).**
  `speed_chroma_cuda` and `speed_temporal_cuda` no longer copy their planes to
  the host to filter them or solve the 25x25 eigenvalue problem and QR system
  there: the whole chain runs on the GPU, fed by device-to-device copies of the
  planes the engine already uploaded, with one 40-byte readback and one wait
  per frame. Every rounding the CPU performs is spelled with a round-to-nearest
  intrinsic, so the scores move from within 1e-4 of `--backend cpu` to equal to
  it, against a CPU build that rounds `log2f` correctly and does not fuse
  multiply-adds (an icx build without `-march=native`; a gcc build on glibc
  differs in the last bits on a few frames). On an RTX 4090 every frame of the
  Netflix 576x324 pair and of BBB 3840x2160 is identical, a 4K
  `speed_chroma_cuda` frame takes 6.89 ms instead of 24.90 ms, and
  `speed_temporal_cuda`, which failed at 1920x1080 and above with
  `CUDA_ERROR_INVALID_VALUE`, now runs 4K at 5.88 ms per frame. The `lanczos4`
  prescale of the SYCL and CUDA twins is not exact: up to 5.7e-4 relative from
  the CPU on a smooth 1080p gradient on an RTX 4090. See
  [SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device).


- **`ssimulacra2_cuda` runs entirely on the device (ADR-1391).** The CUDA
  ssimulacra2 twin no longer copies the pictures to the host, converts colour,
  computes XYB, combines the SSIM and edge-difference maps or downsamples on the
  host, and no longer copies five buffers back and waits at every scale: each
  frame is one chain of kernels on the picture stream and one 864-byte readback.
  On an RTX 4090 a 3840x2160 frame takes about 7 ms instead of about 720 ms, a
  1920x1080 frame about 2 ms instead of 239, and a 576x324 frame under 1 ms
  instead of about 17 (the CPU extractor on 16 threads: about 170, 33 and
  2 ms).
  Its score is within about 1e-12 of `--backend cpu` (1.5e-12 at worst on the
  tested content, the same on every run) where it used to match bit for bit:
  the per-pixel terms are the CPU's double-precision expressions, added in a
  fixed tree instead of one after another. 4:0:0 input and frames below 8x8 now
  go to the CPU extractor. See [SSIMULACRA 2](docs/metrics/ssimulacra2.md).


- **`cambi_hip` runs every CAMBI stage on the device (ADR-1378).** The HIP
  twin no longer preprocesses the picture on the host or copies the image and
  mask back at every scale to compute the c-values and the top-K pooling
  there: each frame is one staged upload of the luma, the whole
  ADR-1357 pipeline on the extractor's stream and one 88-byte read of the
  exact per-scale sums, with `collect()` the only wait. Scores are expected
  to equal `--backend cpu` bit for bit wherever the CPU's own top-K sum is
  exact. `cambi_hip` now also refuses, as the CPU extractor does, a window
  whose adjusted size exceeds 65 x 65 ("cambi: window_size N too large for
  reciprocal LUT"). Not yet run on an AMD device: the verify and timing
  commands are in `docs/state.md` (`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`)
  and [CAMBI](docs/metrics/cambi.md#hip).


- **The HIP SpEED twins run entirely on the device in the CPU's fp32
  arithmetic (ADR-1384).** `speed_chroma_hip` and `speed_temporal_hip` no
  longer copy and filter planes on the host or read the 25x25 covariance back
  for the eigenvalues and the QR solve: each frame is one staged upload, eight
  kernels (the ADR-1358 chain) and one result read, with `collect()` the only
  wait. The kernels are built without FMA contraction and with correctly
  rounded division and square root, so every per-frame score equals the CPU
  extractor's when the CPU's `log2f` is correctly rounded; against a glibc
  build, whose `log2f` misrounds about 0.4 % of arguments, a few chroma frames
  differ in the last float bits. The init-time setup is now one routine,
  `speed_internal_gpu_configure()`, shared with the SYCL twins. Request the
  twins by name (`--feature speed_chroma_hip`). Not yet run on an AMD device:
  see `docs/state.md` (`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`) and
  [SpEED](docs/metrics/speed_qa.md#hip-device-resident-cpu-fp32-arithmetic).


- **The SYCL integer ADM twin computes AIM on the device, so the default model's
  ADM no longer falls back to the CPU under `--backend sycl` (ADR-1362).**
  `adm_sycl` now emits `VMAF_integer_feature_aim_score` and
  `VMAF_integer_feature_adm3_score`, and every ADM output (adm2 and the four
  scales included) is now bit for bit equal to `--backend cpu` on
  8-bit and 10-bit input, odd and 17x17 frames, with the default and the
  default model's options. With the default model on an Arc B580, a 3840x2160
  frame drops from 48.4 to 9.2 ms at the default `--threads 0` and from 24.3 to
  9.7 ms at `--threads 16` (CPU backend with 16 threads: 27-32 ms). On a UHD 770
  the same run gets slower (4K: 75.7 to 87.5 ms, and 40.5 to 79.4 ms at
  `--threads 16`), because the iGPU now does the ADM work the CPU used to do
  beside it. The twin also accepts
  `adm_skip_aim`. Before, adm2 and the scales were up to 2.9e-7 from the CPU
  (double host finalisation) and, on 4K content, `integer_adm_scale2` up to
  1.40e-6 (a decouple ratio that wrapped at scales 1-3).
  See [SYCL backend](docs/backends/sycl/overview.md).


- `cambi_sycl` now runs every stage on the GPU — preprocessing, the spatial
  mask, the per-scale decimation and mode filter, the sliding-histogram
  c-values and the top-K pooling — and reads back one 88-byte block per
  frame instead of downloading the image and mask and computing the c-values
  on the host at every scale (ADR-1357). It reads the distorted plane from
  the shared SYCL frame upload and rides the combined command graph with the
  other SYCL extractors. At 3840x2160 (Big Buck Bunny, `--feature cambi_sycl`,
  ms/frame from `t(22) - t(2)`) the Arc B580 goes from 140 to 9.3 and the
  UHD 770 from 944 to 42; the default model on the UHD 770 goes from 976 to
  103 at 4K and from 138 to 20 at 576x324. Scores are bit-identical to `--backend cpu` whenever the CPU's
  own top-K double sum is exact (every 576x324 and 1080p fixture tested, 47
  of 50 Big Buck Bunny 4K frames); on the other frames the device returns the
  exactly rounded pooled mean and the CPU differs by its summation rounding
  (at most 2.2e-15 on Big Buck Bunny). `cambi.c` exports its reciprocal table
  as `vmaf_cambi_reciprocal_lut()` for GPU twins. The CUDA, HIP and Metal
  twins keep the host residual (RC3 rows in `docs/state.md`).


- **`ciede_sycl` is about twice as fast at 4K on Intel Arc.** The SYCL
  ciede2000 extractor no longer upscales U and V to luma resolution on the host
  before every frame. It uploads the planes at their native size and the kernel
  reads chroma at the subsampled position, as the CUDA and HIP extractors do.
  Measured at 3840x2160 8-bit 4:2:0, `--feature ciede_sycl` drops from 17.2 to
  8.4 ms per frame on an Arc B580 and from 52.9 to 46.0 ms on a UHD 770. Scores
  are unchanged, bit for bit.
- The CLI guide corrects the `--threads` default, which is serial (`0`), not
  the host's core count.


- **`float_ssim` runs on SYCL at 1080p and 4K (ADR-1370).** The SYCL twin
  implemented only scale 1, so `--backend sycl --feature float_ssim` and every
  model on a picture with a short side of 384 px or more computed the feature
  on the CPU and printed `float_ssim_sycl cannot run 3840x2160 8-bit pictures
  with these options`. `float_ssim_sycl` now applies `float_ssim`'s automatic
  (or explicit, `scale=2..10`) decimation on the device, with planes identical
  to the CPU's bit for bit, and uploads raw samples instead of converting both
  planes to fp32 on the host. At 3840x2160 8-bit on an Arc B580 it takes 6.7 ms
  per frame, against 11.0 ms for the CPU extractor on 16 threads and 29.3 ms
  for the old fallback at the default thread count; scale 1 at 4K drops from
  10.6 to 7.8 ms. `enable_lcs`, `enable_db` and `clip_db` work at every scale.
  Only a plane that decimates below 11x11 still falls back. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#float_ssim-decimation-on-the-device-2026-09-29).


- **SYCL `psnr_hvs`, `psnr` and `motion_v2` read the frame the device already
  holds (ADR-1369).** A SYCL run uploads each plane of a frame once, and these
  twins now read it there: `psnr_hvs_sycl` no longer converts all three planes
  to float on the host and uploads them a second time, `motion_v2_sycl` no
  longer re-uploads the reference luma, and chroma goes up once per frame for
  every twin that reads it, from a pinned staging buffer. The psnr_hvs kernel
  runs two work-items per 8x8 block in one dispatch for all planes, and psnr
  adds one atomic per work-group instead of one per pixel. At 3840x2160,
  `psnr_hvs` drops from 17.1 to 7.6 ms per frame on an Arc B580 (16 CPU
  threads: 6.6) and from 124 to 60 on a UHD 770, where `psnr` drops from 25.3
  to 12.3; `motion_v2` loses about 1.6 ms of host copy and upload per frame.
  Every score is bit-identical to the previous twins. On the zero-copy VA import path, which imports luma only,
  `psnr_sycl` and `psnr_hvs_sycl` with chroma now fail the frame with an error
  instead of crashing. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#psnr-psnr_hvs-and-motion_v2-share-the-uploaded-frame-adr-1369-2026-09-29).


- **The SYCL SpEED twins run entirely on the device and match the CPU bit for
  bit (ADR-1358).** `speed_chroma_sycl` and `speed_temporal_sycl` no longer
  filter, factorise the 25x25 covariance or wait on the queue on the host
  between device passes: each frame is one upload, one replayed SYCL graph and
  one result read. On an Arc B580, `speed_chroma` at 3840x2160 drops from 23.3
  to 7.5 ms per frame and `speed_temporal` from 60.4 to 7.6 (CPU with 16
  threads: 7.2 and 37.3); at 576x324 both run in under a millisecond. Every
  per-frame `speed_chroma_u/v/uv` and `speed_temporal` score now equals
  `--backend cpu` exactly, where previously most frames differed by up to
  4.2e-5. Request the twins by name (`--feature speed_chroma_sycl`):
  `--feature speed_chroma --backend sycl` runs the CPU extractor.
  `scripts/dev/speed_gpu_parity.py` checks and times any GPU twin against the
  CPU. See [SpEED](docs/metrics/speed_qa.md).


- **`ssimulacra2_sycl` runs entirely on the device, and `float_ms_ssim_sycl`
  waits once per frame (ADR-1363).** The SYCL ssimulacra2 twin no longer
  converts colour, computes XYB, downsamples or combines the SSIM and
  edge-difference maps on the host between device passes, and no longer copies
  five full-size buffers back per scale: each frame is one upload of the raw
  planes and one 864-byte readback. On an Arc B580 a 3840x2160 frame takes
  33 ms instead of 963 (the CPU extractor on 16 threads takes 167); on a UHD
  770, 445 instead of 1025. Its score is within about 1e-11 of `--backend cpu`
  (6.7e-12 at worst on the tested content, identical on every device) where it
  used to match bit for bit: the device has no fp64 and sums the per-pixel
  terms in a fixed tree of exact fp32 pairs. `float_ms_ssim_sycl` now enqueues
  every scale in `submit()` and waits once in `collect()` instead of once per
  scale; its output is unchanged. 4:0:0 input is rejected by `ssimulacra2_sycl`
  at init. `scripts/dev/speed_gpu_parity.py` takes `--feature` and
  `--max-abs-diff` to check and time any GPU twin. See
  [SSIMULACRA 2](docs/metrics/ssimulacra2.md).


- **Cross-backend gate: the `psnr_hvs` tolerance grows with the frame size.**
  The CPU `psnr_hvs` adds every coefficient error of a plane into one `float`,
  so its rounding error grows with the number of 8x8 blocks, and a correct GPU
  twin landed 8.4e-4 dB away at 3840x2160 against a fixed 5e-4 tolerance.
  `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py` now multiply
  the `psnr_hvs` tolerance by √(N / N₅₇₆ₓ₃₂₄) above 576x324 (3.34e-3 at 4K);
  576x324 and smaller frames keep 5e-4. Both gates also stop crashing when a
  score is non-finite on both backends (JSON `null`, for example `psnr_hvs_cb`
  on identical chroma) (ADR-1361).


- The oneAPI container image is published as `ghcr.io/vmafx/vmafx:<tag>-oneapi2026`,
  named for the oneAPI release it now carries. The same image is also tagged
  `<tag>-oneapi2025`, so existing scripts keep working, and the Dockerfile
  stage `final-oneapi2025` still builds it. Prefer `-oneapi2026` and
  `final-oneapi2026` in new scripts (ADR-1368).


- `scripts/dev/resolve-state-md-conflict.py` now resolves a conflicted
  `docs/state.md` by a three-way merge of the merge base and both sides, keyed
  by bug id, instead of letting master's side win. A branch that closes, edits
  or deletes a row keeps that change, and a later commit that rewrites a row an
  earlier commit added keeps the rewrite. Disposition rows merge their id lists
  as sets, and repeated rows with one label are folded into one. When both sides
  changed the same row differently the tool writes nothing and names it; rerun
  with `--take NAME=ours|theirs`. It runs `scripts/ci/check-state-md-rows.sh` on
  its result and always writes LF line endings. Its test suite now runs real
  `git rebase` conflicts in CI (ADR-1383).


- **Four SYCL twins take the CPU extractor's options and match it
  (ADR-1365).** `psnr_sycl` now accepts `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak` and `min_sse` and matches `--backend cpu` bit for bit,
  `apsnr_*` aggregates included; the PSNR option math now lives in
  `core/src/feature/psnr_score.h`, shared by the CPU extractor and the twin.
  `integer_ssim_sycl` accepts `enable_db` / `clip_db`, `float_ssim_sycl`
  `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s` within 8.3e-7 of
  the CPU), and `float_motion_sycl` `motion_max_val`. Before, a model or a
  `--backend sycl --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. Both SSIM twins now score an
  identical window exactly 1, so with `enable_db` identical frames report the
  CPU's `+inf` or `clip_db` ceiling; their default linear scores moved by at
  most 1.1e-8. Measured on an Arc B580 and a UHD 770; see
  [the SYCL backend guide](docs/backends/sycl/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins-2026-09-29).


### Fixed

- **`cambi` no longer reads and writes outside its buffers on wide, short
  frames, and scores tall, narrow frames the same on every path.** When the
  coarsest of CAMBI's five scales had no more rows than half the window,
  rounded down (`pad_size`; with the default window every height up to 176 at
  1920 wide, 240 at 2560 wide and 352 at 3840 wide), the c-values pass ran past
  both ends of the frame: AddressSanitizer reported heap-buffer-overflows and
  release builds could crash. The scalar, AVX2, AVX-512 and NEON paths now
  clip the window to the rows that exist, as the upstream fix does
  ([Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628),
  [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629)). Frames with
  fewer than `pad_size` rows at that scale (up to 160, 224 and 336 rows at
  those widths) score differently where they completed before; for example a
  3840x128 horizontal ramp moves from 19.544347 to 19.512269 on the C path.
  Going beyond upstream Netflix/vmaf#1629, which bounds only the rows, the
  columns are bounded too (`MIN(pad_size, width)` in the four left-edge loops):
  scores change for frames narrower than `pad_size` at some scale (measured:
  64x1920 vertical ramp master 14.975700714938673 vs branch 14.964394451743877
  on the C path; the SIMD paths already agreed). On frames with fewer than
  `pad_size` columns at that scale (up to 80 wide at 1080 high, 160 at 1920
  high) the scalar walk read columns past the frame. Those frames now score on
  the C path (`--cpumask 63`, builds without SIMD) and on the CUDA, HIP and
  Metal twins, which ran that walk on the host (the CUDA and HIP twins have
  since moved it to the device), what the default dispatch
  already gave (64x1920 vertical ramp moves from 14.975700714938673 to
  14.964394451743877; another vertical ramp variant moves from 16.141046 to
  16.131541). All other frame sizes score as before
  ([CAMBI frame sizes](docs/metrics/cambi.md#frame-sizes)).


- **`--feature <name>` now runs on the GPU that `--backend` names.**
  `vmaf --backend sycl --feature ciede` used to initialise the SYCL device and
  compute `ciede2000` with the CPU extractor, one frame at a time, while the
  JSON reported `"backend_used": "sycl"`. With an explicit `--backend cuda`,
  `sycl`, `hip` or `metal`, a CPU extractor name now runs on that backend's
  twin (`ciede_sycl` here), chosen the way a model's features are
  ([ADR-1359](docs/adr/1359-cli-feature-backend-twin.md)). When the backend has
  no twin, or the twin cannot honour an option or the input size, the CPU
  extractor runs and one `vmaf: warning: --feature <name>: ...` line says why.
  Twin names such as `--feature ciede_sycl`, `--backend cpu`, `--backend auto`
  and runs without `--backend` behave as before.
- **`backend_used` reports the backend that computed the features.** It used to
  name the backend that was initialised, even when nothing ran on it. It now
  names the device when at least one extractor ran there and `cpu` otherwise;
  the new `feature_backends` array says where each extractor ran.


- **`vmaf` no longer leaks its option dictionaries when a run stops early.**
  The dictionaries built from `--feature name=opt=val` and from a `--model`
  feature overload (`--model version=...:vif.vif_enhn_gain_limit=1.0`) were
  released only by the libvmaf calls that take them. A run that stopped
  before those calls (an input that cannot be opened, an odd height with
  4:2:0, a model or feature that does not fit the frame, a feature after a
  failing one, an unknown extractor name, a feature pinned to a backend the
  run did not start) exited with them allocated, and LeakSanitizer builds
  failed with a 158 to 329 byte leak. `cli_free()` now
  releases every dictionary the settings still own, and each hand-off to
  libvmaf clears the settings' copy first. Exit codes and output are
  unchanged.


- **`float_motion_cuda` emits `motion3`, like the CPU `float_motion`.** The
  CUDA twin wrote `motion` and `motion2` only, so `--backend cuda --feature
  float_motion` lost `VMAF_feature_motion3_score` without a warning. It now
  publishes the CPU's `motion3` (the fps-weighted `motion2`, blended by
  `motion_blend_factor` / `motion_blend_offset` and capped at
  `motion_max_val`; frame 0 from the first SAD, `0` for a one-frame input)
  and accepts both blend options (aliases `mbf` / `mbo`). On an RTX 4090 it
  stays within 2.8e-6 of the CPU on the Netflix pair, like `motion2`. The
  SYCL, HIP and Metal twins still write no `motion3`
  (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`).


- **`motion_cuda` computes the CPU `motion` arithmetic.** The CUDA twin blurred
  each frame and differenced the blurred frames, while the CPU (since the
  upstream pipelined-motion port) blurs the frame difference and rounds after
  each filter pass; the two orders round differently (the SYCL twin with the
  same order was up to 2.0e-4 off on 17x17 frames and 1.3e-5 on the Netflix
  576x324 pair). `motion_cuda` now runs the kernel `motion_v2_cuda` already
  used, whose arithmetic is the CPU's, and its debug `integer_motion` score
  is the CPU's (weighted by `motion_fps_weight`, capped at `motion_max_val`).
  Each frame is ordered against the previous one on the device instead of by
  the engine's context barrier, and the eight-frame batch readback waits once
  instead of twice. `motion_v2_cuda`'s SAD is unchanged. On an RTX 4090
  `integer_motion2` / `integer_motion3` now equal the CPU's on the Netflix
  pair and on 50 frames of a 3840x2160 clip, where they were 1.26e-5 and
  6.9e-5 off (ADR-1372; `docs/state.md`,
  `T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`;
  [CUDA backend](docs/backends/cuda/overview.md#cpu-parity-motion-options-and-tiny-frames-2026-09-30)).
- **CUDA integer ADM and VIF guard tiny frames like their SYCL twins.** The
  integer ADM DWT kernels take their row and tap arithmetic from a header a
  device-free test replays for every plane height: from the 17-row ADM minimum
  up no load leaves the plane, and the scale-0 load is clamped into the plane
  below it. `vif_cuda` needs 16 pixels in each dimension; model dispatch and
  `--backend cuda --feature vif` compute smaller frames with the CPU `vif`, and
  `--feature vif_cuda` on such a frame fails `init()` instead of returning
  scores from clamped taps (ADR-1374).


- **CUDA: `test_cuda_runtime_unwind` pins allocating state on host-pinned pictures.**
  Host-pinned pictures (`vmaf_cuda_picture_alloc_pinned`) record the allocating
  state on `priv->cuda.state`, preventing a NULL dereference of `state->f` during
  unref (upstream Netflix/vmaf#1573 hunk a). A device-free test
  `test_pinned_picture_release_uses_the_allocating_state` exercises the allocation
  and unref through the fake driver table, ensuring the allocating state is pinned
  across platforms.


- **The Gitleaks check scans only the commit it checked out.** It ran
  `git log --all` over a full-history checkout, so a finding on any branch in
  the repository, including a commit a force-push had already replaced,
  failed every other open pull request. Each run now scans the history of
  its own `HEAD`: on a pull request, master plus the PR's commits.


- **`motion_force_zero` no longer crashes `motion_cuda` and
  `float_motion_cuda`.** With the option set, the twins' `init()` switches
  them from the asynchronous `submit()` / `collect()` pair to a synchronous
  `extract()` that publishes zeros, but the engine had already chosen the
  asynchronous path and called the cleared `submit()` on the first frame:
  `--backend cuda --feature motion_cuda=motion_force_zero=true` (or
  `float_motion_cuda=...`) died with SIGSEGV. The engine now initialises such
  an extractor before it picks the path, so both twins publish zeros for
  every frame, as the CPU extractors do. The Metal motion twin makes the
  same switch and takes the same engine path; the HIP motion twins keep an
  asynchronous pair that writes the zeros (see the `motion_hip` entry)
  (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`).


- **HIP twins stay inside their buffers on small frames, and `vif_hip` hands
  frames below 16 pixels to the CPU (ADR-1381).** The HIP motion kernel's tile
  loads and the integer ADM scale-0 vertical DWT reflect an index once, which
  leaves the plane for the padding threads of a plane smaller than the tile
  (the defect that faulted the SYCL twins); both now clamp the reflected row
  into the plane, which changes no score of any accepted frame.
  `float_motion_hip` had the same single reflection and read before its input
  plane on 3x3 to 9x9 and 17x17 frames; its tile loads clamp the same way.
  `vif_hip` scored frames below 16 pixels from other samples than the CPU (its filters
  need 16 pixels at every scale); model dispatch now computes those frames
  with the CPU `vif`, and `--feature vif_hip` below 16x16 fails at init. The
  device tests pass on a gfx1036 with no GPU memory fault; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).


- **`motion_hip` now computes the CPU `motion` arithmetic (ADR-1377).** The
  HIP twin blurred each frame and differenced the blurred frames, while the
  CPU (since the upstream pipelined-motion port) blurs the frame difference
  and rounds after each filter pass; the two orders round differently, so
  `motion2` / `motion3` were up to 1.26e-5 off on the Netflix 576x324 pair on
  a gfx1036. `motion_hip` and `motion_v2_hip` now run one diff-first kernel,
  the one `motion_v2_hip` already used, and are expected to match
  `--backend cpu` bit for bit; the debug `motion` score now carries
  `motion_fps_weight` and `motion_max_val` like the CPU's, and a one-frame
  run reports `motion3 = 0`. Both motion twins copy the reference luma into
  pinned memory and upload it without a host wait in `submit()`. Measured on
  a gfx1036: `motion2` / `motion3` identical to `--backend cpu` on every
  frame (1.26e-5 apart before); at 4K `motion_hip` takes 12.95 ms per frame
  (14.25 before) and `motion_v2_hip` 13.24 (10.17 before), the staged upload
  costing more than the wait it removes on that iGPU; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).


- **`motion_hip` and `float_motion_hip` no longer crash with
  `motion_force_zero=true`.** Both HIP twins switched to their synchronous
  zero path inside `init()` and cleared `submit()` / `collect()`, but libvmaf
  had already chosen the asynchronous path for them, so the first frame
  called a NULL `submit()` and the process died with SIGSEGV. The twins now
  keep the asynchronous interface and write the CPU's zeros from
  `collect()`. Measured on a gfx1036: `--feature motion_hip=motion_force_zero=true`
  exits 0 with every `integer_motion*_force_0` score 0, as on the CPU
  (`T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30` in `docs/state.md`).


- The oneAPI container image no longer crashes on Arc B580 (Battlemage)
  GPUs. Through v1.0.0-rc.2 it shipped the Intel GPU compute runtime of
  Intel's `oneapi-runtime:2025.3.1` image (version 25.18), and every
  `vmaf --backend sycl` run on a B580 ended with a segmentation fault (exit
  code 139) right after device selection. Arc A380 and UHD 770 GPUs were not
  affected. Replacing only that runtime with compute-runtime 26.35 stopped the
  crash; replacing only the Level Zero loader did not. The image now builds and
  runs on Debian 13, like the CPU image, with Intel's oneAPI 2026.1 compiler
  and SYCL runtime from Intel's apt repository at one exact build, and with the
  compute runtime (26.35.39758.10) and Level Zero loader (1.34.0) that the
  development container uses, all pinned in `build-config.env` (ADR-1368).


- `scripts/ci/release-pr-exempt.sh` and pre-push hooks now exempt PAT-mode
  `release-please` pull requests (`RELEASE_BOT_TOKEN`, author `lusoris`, `type: User`)
  from authoring-discipline CI gates (ADR-1388, closes #1608). The exemption is
  fail-closed: it requires the designated PAT author and verifies that 100% of the
  files in the PR diff belong strictly to the approved release file set
  (`.release-please-manifest.json`, `release-please-config.json`, `CHANGELOG.md`,
  `changelog.d/*`, `docs/changelog-archive/*`, and coordinated version markers).


- Release provenance for the native Linux files and the `vmaf-mcp` wheel and
  sdist is a GitHub build-provenance attestation (SLSA v1 provenance
  predicate, signed through Sigstore) instead of `slsa-github-generator`
  output. The generator calls its own actions by tag, which the organisation's
  SHA-pinning policy rejects, so the v1.0.0-rc.2 publication failed until the
  policy was relaxed by hand. Releases now attach
  `vmafx-build-provenance.sigstore.json` and `vmaf-mcp-provenance.sigstore.json`
  in place of the `.intoto.jsonl` files; verify with
  `gh attestation verify FILE --repo VMAFx/vmafx`, or offline with `--bundle`
  (ADR-1356). PyPI's PEP 740 attestations are unchanged.


- **`speed_chroma` no longer overruns its frame buffers on odd-sized pictures
  in subsampled formats.** In YUV 4:2:0 and 4:2:2, picture allocators produce
  one extra chroma sample row or column to cover the odd luma extent
  (`vmaf_chroma_extent()`). `speed_chroma` (CPU, CUDA, and HIP) previously
  derived chroma dimensions with integer floor division, under-allocating its
  buffers by one row or column and causing `picture_copy()` to write past the
  buffer under AddressSanitizer. All three extractors now derive chroma extents
  via `speed_chroma_dimensions()`.


- **`speed_temporal` no longer overruns its frame buffers when `speed_prescale`
  is above 1.** The CPU extractor sized its four frame buffers with the source
  height, but resamples each frame in place at the prescaled height, so
  `--feature speed_temporal=speed_prescale=1.5` read and wrote past the end of
  the allocation: an AddressSanitizer build reports a heap-buffer-overflow and a
  release build aborts with `free(): invalid size` or corrupts the heap. The
  buffers now hold the upscaled plane, as `speed_chroma`'s already did. Scores
  at `speed_prescale` 1 and below are unchanged, and the CUDA, SYCL and HIP
  twins were not affected. Reported upstream as
  [Netflix/vmaf#1626](https://github.com/Netflix/vmaf/issues/1626)
  ([features](docs/metrics/features.md#options-shared)).


- **The CPU `ssimulacra2` extractor no longer crashes on 4:0:0 input.** Its
  init ignored the pixel format, so a luma-only picture passed to
  `vmaf_read_pictures()` through the C API reached the colour conversion, which
  read the missing U plane through a NULL pointer (a segmentation fault in
  `ssimulacra2_picture_to_linear_rgb_avx512` on an AVX-512 host). Init now
  refuses 4:0:0 with `-EINVAL` and an error message, as `ssimulacra2_sycl`
  does. The `vmaf` CLI was not affected: it rejects `-p 400` and converts Y4M
  `mono` input to 4:2:0. See [SSIMULACRA 2](docs/metrics/ssimulacra2.md).


- `scripts/ci/check-state-md-rows.sh` works with the `mawk` of Debian 12. That
  version reads regex intervals such as `{0,2}` literally, so the gate matched no
  bug row there and passed every `docs/state.md`, duplicates included. The gate
  now avoids intervals; CI's Ubuntu runner was not affected.


- **SYCL integer ADM row reduction runs spill-free on DG2 and restores Arc A380 parity under `xe`.**
  In `integer_adm_sycl.cpp`, `launch_csf_den_cm` kept nine 64-bit accumulators live across the
  column loop, which IGC compiled at SIMD16 with an 864 B/thread register spill on DG2 (Arc A380).
  Under the Linux `xe` driver, scratch memory accesses return corrupted values, causing all ADM
  accumulators to evaluate to zero and failing `test_sycl_adm_parity` (`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`
  reported CPU 0.5 vs SYCL 1.0). The kernel is restructured into two sequential column reduction
  phases (CSF denominator 3 sums, then DLM and AIM contrast measures 6 sums) with sub-group partials
  staged in local memory before folding, completely eliminating private and spill memory (`privateMemSize: 0`,
  `spillMemSize: 0` on Arc A380). All 7 parity cases in `test_sycl_adm_parity` and all 6 in
  `test_sycl_adm_tiny_frames` pass, and `speed_gpu_parity.py --backend sycl --feature adm` is bit-exact
  (0.000e+00 delta) against CPU on both 576x324 (48/48 frames) and 3840x2160 (50/50 frames).
  Throughput at 4K on BBB 3840x2160 8-bit is 10.92 ms/frame vs 10.70 ms/frame before
  (ADR-1395, `T-SYCL-ADM-CM-SCRATCH-2026-09-30`).


- **A SYCL build with a single AOT target no longer fails the image check.**
  Configuring `-Dsycl_icpx_aot_targets=dg2-g11`, the single-target example in
  the SYCL overview, stopped at `sycl_aot_image_check` with "holds no ocloc fat
  binary" although the build was correct: with exactly one target `ocloc`
  writes each image as a bare native binary instead of a fat binary, and the
  check only knew the fat binary. It now accepts both forms and reads the GPU
  IP version of a bare binary from its product-config note, the value
  `ocloc ids` prints for the target. It still fails the build when an image
  lacks a listed target, is built for a GPU IP version that no listed target
  uses, or is neither form. Builds with two or more targets are checked as
  before.


- SYCL builds now contain the native Intel GPU code that `sycl_icpx_aot_targets`
  asks for. Since ADR-0568 the images were compiled into the objects and then
  dropped at the link, so every `libvmaf.so` was SPIR-V only and compiled its
  kernels on each cold start: the default model's first frame took 524 ms on an
  Arc B580 with a cold compiler cache and now takes 201 ms (UHD 770: 609 to
  245 ms); scores are bit-identical. SYCL sources are compiled with
  `-fno-sycl-rdc --offload-compress`, so the images are built at compile time
  and survive every link; `libvmaf.so` grows from 4.6 to 7.7 MB. An AOT build
  now needs Intel's `ocloc` on `PATH` (`scripts/ci/install-intel-ocloc.sh`
  installs the release pinned as `INTEL_NEO_VERSION` in `build-config.env`),
  and configure stops with an explanation without it; configure with
  `-Dsycl_icpx_aot_targets=` for a SPIR-V-only build. On Linux the build fails
  if `libvmaf.so` lacks an image for any requested target (ADR-1360).


- **SYCL: `psnr_hvs_sycl` no longer crashes on Intel Arc B580 (Xe2).** The Intel
  GPU compiler (IGC 2.41.5) crashed the host process with SIGSEGV while
  compiling the kernel at SIMD32 for Xe2, triggered by the 8x8 DCT running in
  one work-item's private memory. The DCT now runs in local memory, split
  across work-items. Scores are bit-identical on devices where the old kernel
  ran, and a 4K frame takes 130 ms instead of 208 ms on a UHD 770
  (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`).
- **SYCL: small frames no longer lose the device.** `adm_sycl`, `vif_sycl`,
  `motion_sycl`, `motion_v2_sycl`, `float_motion_sycl` and `float_vif_sycl`
  loaded their local-memory tiles, padding lanes included, with a single edge
  reflection. On small planes that read outside the buffer and raised
  `UR_RESULT_ERROR_DEVICE_LOST` on an Arc B580 and a UHD 770: `adm_sycl` for
  frames 64 rows high or less, `vif_sycl` up to at least 96x64 and
  `motion_sycl` up to 33x33 on the B580. The loads now stay inside the plane;
  scores for larger frames are unchanged
  (`T-SYCL-TILE-HALO-OOB-READ-2026-09-29`).
- **SYCL: `vif_sycl` hands frames below 16 pixels to the CPU.** Its filters
  reach more than one reflection outside planes smaller than 16 pixels in
  either dimension, which lost the device at 8x8. Model dispatch now computes
  such frames with the CPU `vif` extractor, bit-identical to a CPU run; a
  direct `--feature vif_sycl` request fails with an error naming `vif`
  (`T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29`).
- **SYCL: `vif_sycl` scores are correct for odd widths.** When the width at
  any scale was odd (for example 854x480, 1366x768 or 853x480), scales 1-3 read
  the downsampled plane at the wrong row stride and drifted from the CPU by up
  to 1.2e-3 at those sizes and 3.3e-2 on tiny frames, which also moved the
  VMAF score. They now match the CPU within 1e-6 at those sizes
  (`T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29`).
- **SYCL: a device fault now fails the frame.** After a failed graph wait the
  `adm_sycl`, `vif_sycl`, `motion_sycl`, `psnr_sycl` and `float_moment_sycl`
  extractors emitted scores for that frame from stale buffers (about 1.0 for
  every ADM scale) and `vmaf_read_pictures()` returned 0; only a later frame's
  upload failed. They now return `-EIO` for the faulted frame
  (`T-SYCL-GRAPH-WAIT-ERROR-DROPPED-2026-09-29`).


- **`float_motion_sycl` honours `motion_force_zero` and weights its debug
  score.** The twin declared `motion_force_zero` but ignored it and emitted
  real motion scores; it now emits zeros, like the CPU `float_motion`. Its
  debug `VMAF_feature_motion_score` now carries `motion_fps_weight`, as on the
  CPU, instead of the unweighted SAD. The SSIM page no longer documents an
  `enable_chroma` option and `_cb` / `_cr` outputs that the `ssim` extractor
  does not have; it lists `enable_db` and `clip_db`
  ([SSIM](docs/metrics/ssim.md#options)).


- **`float_ssim_sycl` with `enable_db` no longer reports tens of dB below the
  CPU on near-identical frames.** The CPU rounds each frame's SSIM mean to fp32
  before converting it to dB, so a frame within half an fp32 step of 1 scores
  exactly 1 and reports `+inf` or the `clip_db` ceiling; the twin kept the
  double mean and reported a finite value (93.6 dB against the CPU's 121 dB on
  the first frames of a 4K pair). The twin now rounds the `float_ssim` and
  `float_ssim_l/c/s` means the same way; linear scores move by less than 6e-8.


- **`motion_sycl` matches the CPU `motion` exactly.** The SYCL twin blurred
  each frame and differenced the blurred frames, while the CPU (since the
  upstream pipelined-motion port) blurs the frame difference and rounds after
  each filter pass. The two orders round differently, so `motion2` was up to
  2.0e-4 off on 17x17 frames, 1.3e-5 on the Netflix 576x324 pair and 5.6e-6
  at 4K. `motion_sycl` and `motion_v2_sycl` now run one kernel with the CPU's
  arithmetic and agree with the CPU bit for bit at every size and bit depth
  tested, on an Arc B580 and a UHD 770 (ADR-1371). The 4K motion step costs
  about 11% more device time on both GPUs. With `motion_add_uv=true`,
  `motion_sycl` no longer waits on the device inside `submit()`: the U and V
  planes are staged in pinned memory and uploaded on the compute queue, which
  cuts host time per 4K frame from 5.4 to 0.6 ms on a UHD 770
  ([SYCL backend](docs/backends/sycl/overview.md#motion_sycl-matches-the-cpu-motion-exactly-2026-09-29)).


- **SYCL: `psnr_hvs_sycl` scores 9- and 11-bit input like the CPU.** The twin
  multiplied 9- and 11-bit samples by 16 before scoring them, so through the C
  API a 9-bit clip that the CPU scored at 22.47 dB came out at -1.57 dB
  (11 bits: 33.97 against 10.43). It now reads the raw sample at every bit
  depth, as `psnr_hvs` does; 8-, 10- and 12-bit scores are unchanged. The CLI
  accepts only 8, 10, 12 and 16 bits (`T-SYCL-PSNR-HVS-ODD-BPC-SCALE-2026-09-29`).


- **SYCL: `psnr_hvs_sycl` kernel is scratch-free on Intel Arc under xe (ADR-1395).**
  On the Linux `xe` driver, private memory causes corrupted reads and writes.
  The kernel had 2432 B/thread of private memory at SIMD16 on DG2 (Arc A380)
  due to dynamically indexed `means[4]` and `variances[4]` arrays in
  `hvs_variance_ratio()` and `args.plane[plane]` dynamic indexing in `hvs_locate()`,
  causing ~20 dB divergence at 4K (BBB frame 0 `psnr_hvs_y` 13.13 dB vs CPU
  33.17 dB). Replacing dynamic struct indexing with explicit member branches
  and restructuring quadrant accumulators into scalar members (`HvsQuadrants`)
  eliminates all private memory and register spills (`private_size: 0`,
  `spill: 0`). On Arc A380 under `xe`, scores match CPU reference across 576x324
  (max diff 8.37e-5 dB vs 5e-4 gate), 1080p (max diff 1.71e-3 dB), and 4K BBB
  (frame 0 33.161817 dB vs CPU 33.171624 dB, delta 0.0098 dB; max diff across
  22 frames 1.099e-2 dB). Throughput on 4K BBB improves from 12.55 ms/frame
  (corrupted) to 10.90 ms/frame (correct) (`T-SYCL-PSNR-HVS-XE-SCRATCH-2026-09-30`).


### Fixed
- **SYCL**: Fixed identical/flat-frame handling in `float_ssim_sycl` and `integer_ssim_sycl` by implementing the CPU's exact arithmetic without identical-window shortcuts, grouping integer terms as `((w*a)*b)/den`, and preserving ADR-1370 fp32 frame-mean rounding.
- **SYCL**: Fixed a bug where `psnr_sycl` produced incorrectly scaled scores under `--subsample` by adding the missing `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag.
- **SYCL**: Fixed a bug where `motion_v2_sycl` diverged from the CPU by applying `motion_fps_weight` and the `motion_max_val` cap in `collect()` and emitting scores for one-frame inputs in `flush()`.


- **Every SYCL feature kernel now does fp32 arithmetic the way the CPU
  reference does (ADR-1367).** The SYCL guides said the kernels ran in IEEE-754
  strict mode under `-fp-model=precise`; in fact icpx still fused
  `a * b + c` into one FMA and computed `/` and `sqrt` approximately (29% and
  8% of random fp32 operands differed from the host). Every SYCL feature
  translation unit now compiles with
  `-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`,
  and the link carries the precision flags for the SPIR-V image that devices
  outside `sycl_icpx_aot_targets` compile at first launch. Nine twins' scores
  move, each still inside its cross-backend tolerance: `float_adm_sycl` is ten
  times closer to `--backend cpu` (2.5e-5 -> 2.5e-6 on the Netflix pair),
  `float_ssim_sycl` and `integer_ssim_sycl` about twice as close, and
  `ciede_sycl` halves its worst 3840x2160 difference (9.7e-5 -> 4.5e-5);
  `float_vif_sycl`, `float_ms_ssim_sycl`,
  `float_motion_sycl` and `vif_sycl` move within their existing spread, and
  `psnr_hvs_sycl` by at most 1.3e-6 dB. The
  twins that were bit-identical to the CPU stay so, and no twin's 4K cost on
  an Arc B580 changed by more than the run-to-run spread. AdaptiveCpp builds
  keep contraction-off only. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#what-the-sycl-compile-line-guarantees).


- **`vmaf_init()` accepts an uninitialised handle again, as upstream libvmaf
  does.** Since ADR-1032 it returned `-EINVAL` whenever `*vmaf` was not NULL.
  Callers written against upstream, whose own CLI and tests declare
  `VmafContext *vmaf;` without an initialiser, failed at random depending on
  what the stack held. Upstream's `test_context.c` failed 3 of 3 runs.
  `vmaf_init()` no longer reads `*vmaf`: it sets it to NULL on entry and to
  the new context on success (ADR-1396). A handle that still holds an open
  context is now overwritten instead of rejected; close it first.


- **SYCL: the native Windows build runs its kernels.** A Windows MSVC build
  linked `vmaf.exe` and the tests with `link.exe`, which ignored `-fsycl` and
  never registered the SYCL device images, so every SYCL kernel submit failed
  with `No kernel named ... was found` and 47 of the 50 SYCL tests failed on an
  Arc B580. The build now runs one `icpx -fsycl -fsycl-link` step over the SYCL
  objects and links its registration object into every program that uses the
  SYCL backend, including static consumers of `vmaf.lib`. On an Arc B580 and a
  UHD 770 all SYCL tests pass and all 19 SYCL extractors agree with the CPU
  within the parity gate. The `Windows MSVC+SYCL` CI leg now checks that the
  kernels are registered, and [SYCL on Windows](docs/backends/sycl/windows.md)
  documents the native build (ADR-1364, `T-SYCL-WINDOWS-MSVC-KERNELS-UNREGISTERED-2026-09-29`).
- **CI: Windows test lanes gate their tests again.** `scripts/ci/run_meson_test.py`
  replaced itself with `meson test` through `os.execvp`, which on Windows starts
  Meson and ends the runner with status 0 at once. The Windows MinGW64 and ARM64
  MSVC lanes reported success after 17 to 19 tests, over failing ones. The runner
  now waits for Meson on Windows and returns its status; four Windows test-harness
  failures it had hidden are fixed (`T-CI-WINDOWS-MESON-TEST-RUNNER-EXIT-0-2026-09-29`,
  `T-TEST-WINDOWS-HARNESS-MASKED-FAILURES-2026-09-29`).
- **Parity gate: the `cambi` cell compares scores.** `cross_backend_parity_gate.py`
  and `cross_backend_vif_diff.py` looked the score up as `Cambi_feature_cambi_score`,
  but `vmaf --json` writes it as `cambi`, so the cell stopped with `KeyError`
  (`T-CI-PARITY-GATE-CAMBI-KEY-2026-09-29`).

## [1.0.0-rc.2] - 2026-09-28
### Changed

- The native Linux release bundle (`vmaf` and `libvmaf.so*`) now runs on
  Ubuntu 24.04, Debian 13 and newer distributions: it needs glibc 2.38 and the
  libstdc++ of GCC 12 instead of glibc 2.43. It is compiled on the fork's
  Debian 13 release track, the base of the published container images, and
  each release checks it on Ubuntu 24.04 and in the distroless `cc-debian13`
  runtime image. Ubuntu 22.04 and Debian 12 remain unsupported (ADR-1354).


- **The first-release candidate plan moved back by one candidate.**
  `v1.0.0-rc.2` is a stabilisation candidate: it ships the dependency updates
  and fixes merged since rc.1, and testers use the same report kit
  (`tools/rc1-tester/`) and exit bar as for rc.1. Benchmarking, profiling and
  tuning move to `v1.0.0-rc.3`, and the one-shot model retrain moves to
  `v1.0.0-rc.4`, so each phase number now matches its tag. The release guide,
  roadmap, tester guide, retrain runbook and `vmaf-rc1-report list-tools`
  inventory show the new mapping (ADR-1352).


### Fixed

- `--model` and `--feature` values keep their backslashes, so Windows paths work
  as typed: `path=..\..\models\m.json`, `path=\\server\share\m.json` and
  `path=C:\models\.cache\m.json` used to lose a backslash each (`\.` and `\\`
  were escapes in values too). `\:` and `\=` still escape a delimiter, and a
  backslash run directly before `:` or `=`, or at the end of a value, is read in
  pairs so a backslash in front of a delimiter stays writable. Keys and
  overload names keep the full escape set (ADR-1355). If you wrote a UNC path
  as `\\\\server\share` per the earlier advice, write `\\server\share` now.


- Container publication finishes for the large images. The GPU image jobs free
  runner disk before `syft` scans the pushed image (the 1.0.0-rc.1 ROCm SBOM
  failed with "no space left on device" after the image was pushed and
  signed), and the two-platform `vmafx-operator` build gets 60 minutes instead
  of 30. The CPU, MCP server, node and controller images no longer compile
  libvmaf's unit-test suite, which they never shipped: the arm64 builds were
  cancelled at 60 minutes while still linking tests under emulation.


- **Nightly Kubernetes E2E scores again** — the kind + kuttl scoring smoke sent
  a 64x64 clip to `/v1/score`. Since `vmaf_v1.0.16_3d0h` became the default
  model (ADR-1169), libvmaf refuses input that small (`cambi` needs one side of
  at least 216 pixels, `speed_chroma` needs 4:2:0 luma of at least 160x160), so
  the server answered HTTP 500 and every scheduled E2E run from 2026-09-24 on
  failed. The fixtures are now 216x160, the smallest size the default model
  accepts, and the test ConfigMap is created with server-side apply because the
  larger pair exceeds the 256 KiB annotation that client-side apply writes. The
  always-on E2E contract test now checks the fixture size against the
  thresholds in `core/src/feature/` on every pull request. On failure the score
  script now prints the `/v1/score` error body and the server Pods' logs; it
  previously discarded the body and, through `deployment/vmafx`, printed the
  operator's logs instead.


- The FFmpeg patched by `ffmpeg-patches/0019` builds without warnings on
  aarch64. GCC 14.2 flagged two `-Wstringop-overflow` false positives in
  `libavcodec/a64multienc.c`, which the node image's warning gate rejects, so
  the arm64 `vmafx-node` image could not build. The index tables are now
  filled per palette interval, with identical results. An image recovery run
  also takes `ffmpeg-patches/` from the dispatching commit (ADR-1350).


- **ffmpeg calls work with FFmpeg 9 again**: FFmpeg 9 removed the `-vsync`
  option, so the Python harness's decode step and the `describe_worst_frames`
  MCP tool (Python and Go servers) failed with "Unrecognized option 'vsync'"
  on the FFmpeg release this project pins. They now pass
  `-fps_mode passthrough`, the same mode as the old `-vsync 0`, which makes
  FFmpeg 5.1 the oldest release they work with. The harness change ports
  Netflix/vmaf `aeaf2877d`.


- The `vmafx-operator` and `vmafx-server` release images build each
  architecture on its own native runner, like `vmafx-node` (ADR-1349). Their
  arm64 halves were emulated with QEMU and took 30 to 46 minutes of a 60-minute
  limit; each still publishes one signed, attested multi-arch image.


- **Helm: the server Deployment no longer selects the operator and node Pods.**
  The chart's server Deployment and StatefulSet selected only the release
  labels, so they also matched the operator, node and `helm test` Pods, and
  `kubectl logs deployment/vmafx` could print the operator's log. Both now
  also select `app.kubernetes.io/component: server` (ADR-1353). Scoring
  traffic was not affected: the Services already selected the server Pods
  only. **Upgrade note for v1.0.0-rc.1 installs:** a workload's selector
  cannot be changed in place, so `helm upgrade` fails with
  `spec.selector: ... field is immutable`. Delete the server workload first;
  `--cascade=orphan` keeps its Pods serving until the upgrade replaces them:
  `kubectl delete deployment,statefulset -n <namespace> --cascade=orphan -l app.kubernetes.io/instance=<release>,app.kubernetes.io/component=server`,
  then run `helm upgrade` as usual. Uninstalling and installing again also
  works. See "Upgrading from 1.0.0-rc.1" in
  `docs/development/k8s-deployment.md`.


- A published release's container images can be recovered after a build
  recipe fix (ADR-1347). A `workflow_dispatch` of the image publish workflows
  on the default branch builds the release tag's source with that commit's
  `docker/` recipe and labels each image with `io.vmafx.build-recipe`; the
  dispatch path also reads the prerelease flag from the release itself, so it
  works for release candidates.
  The release guide now covers the `release-publish` environment step a
  recovery run needs (the environment admits only `v*` tags, so `master` is
  allowed for the recovery and removed afterwards) and how to make a new GHCR
  package public, which the REST API cannot do.
  The post-push smoke tests verify each image's signature against the
  identity of the run that signed it; they required the tag identity, which a
  recovery run cannot produce, so the first v1.0.0-rc.1 recovery failed its
  CPU smoke test after pushing and signing the image. The release guide shows
  how to verify a recovered image (`@refs/heads/master` identity and its
  `io.vmafx.build-recipe` label).


- `test_meson_secret_env_sanitization` passes when Meson is installed with
  `pip install --user`, which `scripts/setup/ubuntu.sh` and the nightly
  ThreadSanitizer job both do. Its probes replaced `HOME` with a temporary
  directory, which also moved Python's per-user package directory, so every
  probe stopped at `No module named 'mesonbuild'` before Meson ran. The probes
  now keep `PYTHONUSERBASE` pointed at the real user base while `HOME` stays
  synthetic.


- The `vmafx-node` image bundles rclone at `/usr/local/bin/rclone` again, as the
  storage guide states. The node resolves `s3://`, `gs://`, `rclone://` and
  `remote:path` inputs by running rclone (ADR-0719), but `docker/Dockerfile.node`
  never installed it, so every remote input failed. The static binary comes
  from the official rclone image, pinned by digest in `build-config.env`, and
  the release smoke test now runs it.


- The `vmafx-node` release image builds again. Its arm64 half compiled FFmpeg,
  libvmaf and the node binary under QEMU emulation and never finished within
  the job's two-hour limit, so no node image was published for v1.0.0-rc.1.
  Each architecture now builds on its own native runner, and the release
  publishes one merged multi-arch image, signed, attested and with an SBOM
  (ADR-1349).


- Release candidates after `1.0.0-rc.1` are numbered `1.0.0-rc.2`,
  `1.0.0-rc.3`, and so on (ADR-1348). release-please used its default
  versioning, which turned the first fix after `1.0.0-rc.1` into a proposed
  `1.0.1-rc.1` (release PR #1575); it now uses `prerelease` versioning, and the
  final cut becomes `1.0.0` once `prerelease` is switched off. The release guide
  also no longer claims that every new GHCR package starts private: with the
  organization's public-package setting on, a package first pushed from this
  repository is created public.
- The container quick start works for release candidates: it names the
  release tag instead of `latest` (release candidates are never tagged
  `latest`) and passes `--pixel_format 420` instead of the rejected `yuv420p`.
  The image docs list the `-rocm10` variant and the exact signing identities
  for recovered images, and a recovered image's
  `org.opencontainers.image.revision` label names the tag's source commit
  rather than the recipe commit it was built with.
- The container images carry the built-in models again: the CPU, MCP-server,
  CUDA and oneAPI builders lacked `xxd`, so libvmaf silently embedded no model
  and scoring without `--model` failed. The oneAPI image now installs the
  Unified Memory Framework runtime its SYCL adapters need; without it the image
  found no SYCL device. The publish smoke tests now score with the default model
  and check the oneAPI adapters, and the GPU image docs give working device and
  group flags, forced-backend scoring examples and measured parity figures.


- The native Linux `vmaf` CLI attached to a release runs next to the
  downloaded `libvmaf.so*` files without `LD_LIBRARY_PATH`. The
  `v1.0.0-rc.1` CLI kept Meson's build-tree RUNPATH `$ORIGIN/../src`, so it
  found `libvmaf.so.3` only when `LD_LIBRARY_PATH` pointed at the download
  directory. The release build now sets the staged CLI's RUNPATH to exactly
  `$ORIGIN`, and the release gate runs the CLI without `LD_LIBRARY_PATH` and
  rejects any other RUNPATH.


- libvmaf builds against libc++ 23 again. The vendored libsvm
  (`core/src/svm.cpp`) defined its own global `swap` template, and libc++ 23's
  `std::vector` internals now call `swap` unqualified, so both it and
  `std::swap` matched and the file failed with "call to 'swap' is ambiguous".
  libsvm now uses `std::swap`; scores are unchanged.


- **Whole-tree clang-tidy ratchet ignores generated build products**: `tidy-ratchet.py`
  now skips every translation unit, diagnostic and header under `--build-dir`, so a
  build directory inside the repository measures the same checked-in sources as one
  outside it. The nightly `Full clang-tidy scan` builds in `build/` and had been failing
  on the 18 `xxd`-generated model embeds (`build/src/*.json.c`,
  `build/src/brisque_live.model.c`, two `misc-use-internal-linkage` warnings each)
  that the cpu baseline no longer lists. `make tidy-ratchet` / `tidy-ratchet-write` with
  the default in-tree `core/build` no longer measure or record them either. The arm64
  baseline, recorded from an in-tree `build-arm64`, was re-measured on its own toolchain:
  764 to 615 warnings (36 generated-file warnings, 25 already-ignored Pelorus-mirror
  entries and 88 warnings cleaned since 2026-09-23; no count rose) (ADR-1142).

## [1.0.0-rc.1] - 2026-09-27

This release collects 2790 changelog entries.
They are recorded in full, unedited, in
[`docs/changelog-archive/1.0.0-rc.1.md`](docs/changelog-archive/1.0.0-rc.1.md) — too long to read inline here.

| Section | Entries |
| --- | --- |
| Changed | 625 |
| Added | 510 |
| Removed | 14 |
| Fixed | 1585 |
| Security | 56 |
