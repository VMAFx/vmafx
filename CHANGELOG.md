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


- **actionlint pre-commit hook and Makefile target**: Wired `actionlint`
  pinned to `v1.7.12` (HISS-11 hermetic supply chain pin) into
  `.pre-commit-config.yaml` to validate all 35 GitHub Actions workflow files
  under `.github/workflows/` against `.github/actionlint.yaml`. Added
  `make lint-actions` target and documented workflow linting in
  `docs/development/pre-commit-hooks.md`.


### Changed

- Migrated the Windows MSYS2 MinGW build matrix leg in
  `.github/workflows/libvmaf-build-matrix.yml` from the deprecated `MINGW64`
  environment linking legacy `msvcrt.dll` to `UCRT64` linking the Universal C
  Runtime (`ucrtbase.dll`), using `mingw-w64-ucrt-x86_64-*` packages. Updated the
  required status check name in `.github/workflows/required-aggregator.yml` to
  `Windows UCRT64` (ADR-1387, #1609).


- **The documented range of `integer_aim` is corrected: it is not bounded by
  1.** The fixed-point `adm` extractor reports the additive impairment divided
  by the reference's detail as it is, and `float_adm` clips the same ratio at
  1; both follow upstream Netflix/vmaf. On a reference without detail the two
  differ: a flat grey 64x64 reference against the same picture with isolated
  patches gives `integer_aim` 3.1756 (upstream master prints 3.175585) and a
  float `aim` of 1, and with the default model's weight and floor
  `integer_adm3` 0.5 against a float `adm3` of 0.7. No score changes. The
  metrics guide now states both ranges, the definition of `adm3_score` and
  what to expect on such content, and a test pins both behaviours so that
  neither changes unnoticed (ADR-1417,
  `T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01`;
  [features](docs/metrics/features.md)).


- Rewrote agent-facing documentation into caveman internal register across 6 subtree AGENTS.md files: `cmd/vmafx-controller/AGENTS.md`, `cmd/vmafx-mcp/AGENTS.md`, `compat/python-vmaf/AGENTS.md`, `deploy/helm/vmafx/AGENTS.md`, `mcp-server/AGENTS.md`, and `pkg/tune/AGENTS.md`. All load-bearing tokens and invariants preserved under determinism and context gates.


- Rewrote agent-facing documentation into caveman internal register across 10 subtree AGENTS.md files: `ai/AGENTS.md`, `dev/AGENTS.md`, `cmd/vmafx-node/AGENTS.md`, `pkg/libvmaf/AGENTS.md`, `ai/sidecar/AGENTS.md`, `docker/AGENTS.md`, `bindings/rust/vmafx-sys/AGENTS.md`, `requirements/AGENTS.md`, `tools/vmaf-roi-score/AGENTS.md`, and `tools/rc1-tester/AGENTS.md`. All load-bearing tokens and invariants preserved under determinism and context gates.


- Rewrote agent-facing documentation into caveman internal register across 6 subtree AGENTS.md files: `gen/go/AGENTS.md`, `docs/research/AGENTS.md`, `internal/app/scoringservice/AGENTS.md`, `.zed/AGENTS.md`, `pkg/model/AGENTS.md`, and `api/vmafx/v1/AGENTS.md`. All load-bearing tokens and invariants preserved under determinism and context gates.


- **Core and GitHub AGENTS.md files use the internal register.**
  `core/AGENTS.md`, `core/test/AGENTS.md`, `.github/AGENTS.md`,
  `core/src/AGENTS.md`, `core/tools/AGENTS.md`, `core/src/dnn/AGENTS.md`,
  `core/src/hip/AGENTS.md`, `core/src/cuda/AGENTS.md`, `scripts/AGENTS.md`,
  and `core/src/sycl/AGENTS.md` conform to the caveman register required by
  ADR-1249. Every code span, command, identifier, link, and invariant is
  preserved verbatim and verified against `praetorctl caveman check` and
  `caveman_keep_check.py`.


- **Remaining core and scripts AGENTS.md files use the internal register.**
  `core/src/metal/AGENTS.md`, `core/src/mcp/AGENTS.md`,
  `core/include/libvmaf/AGENTS.md`, `scripts/lib/AGENTS.md`,
  `scripts/dev/AGENTS.md`, and `.zed/AGENTS.md` conform to the caveman register
  required by ADR-1249. Every code span, command, identifier, link, and
  invariant is preserved verbatim and verified against `praetorctl caveman check`
  and `caveman_keep_check.py`.


- **A large subtree `AGENTS.md` is now a generated index over one page per
  topic.** `scripts/ci/AGENTS.md` (93,543 bytes) is the first: its text moved
  unchanged into 46 pages under `scripts/ci/AGENTS.d/`, and the file itself is
  a 14,133-byte index that tells an agent which pages to read for the paths
  it is about to touch. Measured on three tasks, an agent now loads 21% to 23%
  of what it loaded before. To record an invariant in such a directory, edit
  or add a page and run `make docs-fragments-write`;
  `make docs-fragments-check` fails on a stale index, on a page above 12,000
  bytes, on an index above 16,000 bytes and on a page whose path globs match
  no file. `scripts/docs/agents_migration_check.py` proves that a migration
  moved every paragraph and every identifier
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **The parity gate covers `speed_chroma` on CUDA, at `5e-6`.**
  `speed_chroma_cuda` reproduces the CPU extractor's arithmetic and rounds
  `log2` correctly; the CPU extractor calls the C library's `log2f`, and
  glibc's is the neighbouring `float` for up to 1 % of its arguments.
  Measured on an RTX 4090 at `--precision max` (Netflix 576x324 at 8, 10, 12
  and 16 bits, both 1080p checkerboard pairs, 200 frames of BBB 3840x2160):
  776 of 789 values are identical to the CPU, the other 13 differ by one to
  five steps of the 32-bit score (1.4e-6 at most), and all 789 are identical
  when the CPU run uses a correctly rounded `log2f`
  ([ADR-1430](docs/adr/1430-cuda-speed-chroma-log2f-bound.md)). The twin and
  its scores do not change. The gate had no `speed_chroma` cell before; the
  CUDA parity test compared one score of one frame at `1e-4` on a fixture
  that never reached the scoring path, and now compares all three scores of
  every frame to one part in a million on one that does.


- **Every CUDA kernel is built without FMA contraction, and
  `float_ms_ssim_cuda` is bit-identical to the CPU.** nvcc fuses `a * b + c`
  into one FMA by default; six of the 21 CUDA kernels were built with
  `--fmad=false` and fifteen were not, while the CPU build and the SYCL twins
  never fuse. All kernels now take one flag list
  (`cuda_device_strict_fp_args`), so a plain multiply-add rounds twice on the
  device as it does on the host, and a kernel whose CPU reference fuses on
  purpose writes the fused operation explicitly
  ([ADR-1403](docs/adr/1403-cuda-strict-fp-every-kernel.md)). Measured on an
  RTX 4090 against `--backend cpu` at `--precision max`: `adm`, `vif`,
  `motion`, `motion_v2`, `psnr`, `psnr_hvs`, `float_psnr`, `float_moment`,
  `cambi`, `float_adm`, `float_ssim`, `ssim`, `ssimulacra2`, `speed_chroma` and
  `speed_temporal` produce exactly the values they produced before. `float_ms_ssim_cuda`, whose
  kernels now follow the CPU extractor operation for operation, goes from up
  to 4.4e-6 away to bit-identical on every frame of the Netflix pair, the
  1080p checkerboard pairs and BBB 3840x2160, per-scale `enable_lcs` outputs
  included. `ciede_cuda`, `float_vif_cuda` and `float_motion_cuda` move in
  their last digits and stay inside the cross-backend tolerance where they
  were. No twin is measurably slower at 3840x2160. Re-run any stored CUDA
  output of those four twins.
  `-Denable_nvcc=false` (CUDA kernels through clang) configures again and
  agrees with the nvcc build on 18 of 19 twins
  ([CUDA backend](docs/backends/cuda/overview.md#floating-point-model-no-fma-contraction-adr-1403)).


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


- **`vif_cuda` is declared bit-identical to the CPU `vif` extractor, with a
  proof that covers every input.** The CPU reads its logarithms from a table
  of 32768 values built with the host math library; `vif_cuda` computes them
  on the device, which on an AMD GPU had moved 77 of the values (ADR-1435). A
  new test launches a probe kernel and compares the device's value with the
  CPU's table for all 32768 entries: all are equal on an RTX 4090 (CUDA 13.4,
  glibc 2.44), although the device's `log2f()` differs from the host's by one
  unit in the last place for 307 arguments. The parity gate now compares the
  CPU and CUDA `vif` cells with tolerance 0; 1392 of 1392 scores on 348
  frames are identical at `--precision max`. No scoring kernel, stored score
  or frame time changes. If the test fails on another host or CUDA release,
  the twin has to read the CPU's table as the HIP twin does
  ([ADR-1456](docs/adr/1456-cuda-vif-device-log2-pinned.md),
  [CUDA backend](docs/backends/cuda/overview.md#vif_cuda-returns-the-cpus-scores-bit-for-bit-2026-10-02)).


- The set of GPU twins the parity gate compares exactly is no longer a literal
  in `scripts/ci/cross_backend_calibration.py`: each (feature, backend) is one
  file under `scripts/ci/exact_twins.d/` (`adr:` and `evidence:`), the loader
  validates the directory, and the table in
  `docs/development/cross-backend-exact-twins.md` is generated from it by
  `make docs-fragments-write`. Declaring a twin exact edits no shared file
  (ADR-1428).


- **`float_adm` no longer depends on the processor; its scores move by about
  1e-7 on x86.** One step of float ADM divides two wavelet coefficients. On
  x86 the quotient was formed from the processor's reciprocal-estimate
  instruction (`RCPSS`) and one correction step, as upstream Netflix does.
  That instruction is specified by an error bound, not bit for bit, so the
  same frames could score differently on two x86 machines, and differently
  again on ARM and under MSVC, which never used it. `float_adm` now divides
  on every host and with every compiler
  ([ADR-1442](docs/adr/1442-float-adm-reference-divides.md)). Measured on a
  Ryzen 9 9950X3D against the previous build: 147 of 791 `float_adm` scores
  change, by at most 1.3e-7, and the `vmaf_float_v0.6.1`,
  `vmaf_float_v0.6.1neg` and `vmaf_float_4k_v0.6.1` models by at most 1.2e-5
  on a frame and 2.7e-6 on a clip's mean (Netflix 576x324 at 8, 10, 12 and 16
  bits, both 1080p checkerboard pairs, BBB 3840x2160). The fixed-point `adm`
  and the default models do not change, ARM and MSVC builds do not change,
  the Netflix golden tests pass unchanged, and the extractor is not slower.
  `float_adm_cuda` divides as well: it equals the CPU extractor of any
  machine (2034 of 2034 outputs on an RTX 4090) and no longer measures the
  host's instruction when it starts. Scores stored from an x86 build of an
  earlier release differ from new ones by the amounts above.


- **Five more HIP twins are held to the CPU's bits by the parity gate.**
  `motion_hip` (also with `debug=true`), `motion_v2_hip`, `psnr_hip`,
  `integer_ms_ssim_hip` (the HIP twin of `float_ms_ssim`, with and without
  `enable_lcs`) and `cambi_hip` return the CPU extractor's scores bit for bit:
  measured on a gfx1036 at `--precision max` on 178 frames from 480x270 to
  3840x2160 at 8 to 16 bits, and with their options. They are now listed as
  exact twins, so the gate compares them with tolerance 0 where it allowed
  5e-5, and `test_hip_exact_twins` asserts equality on a device. The same
  sweep found `float_psnr_hip` and `float_moment_hip` identical on real clips
  but not on all input (up to 7.6e-8 dB at 10 to 16 bits with large
  differences; second moments up to 1.0e-4 at 16 bits); they stay under their
  tolerance. The table of every HIP twin is in
  [the HIP backend page](docs/backends/hip/overview.md#which-hip-twins-return-the-cpus-bits-2026-10-01)
  ([ADR-1437](docs/adr/1437-hip-exact-twins-declared.md),
  [Research-1437](docs/research/1437-hip-twin-exactness-sweep.md)).


- **`float_motion_hip` emits `motion3` and takes every CPU `float_motion`
  option (ADR-1404).** The HIP twin wrote `motion` and `motion2` only and
  lacked `motion_blend_factor` (`mbf`), `motion_blend_offset` (`mbo`),
  `motion_filter_size` (`mfs`), `motion_add_scale1` (`mdc`) and
  `motion_add_uv` (`mau`), so `--backend hip --feature float_motion` dropped
  `motion3` from the output and a request with one of those options was
  computed on the CPU. It now emits `VMAF_feature_motion3_score` with the
  CPU's blend, selects the blur filter in the kernel, adds the half-size SAD
  with a second kernel and runs both on the U and V planes for
  `motion_add_uv`. On a gfx1036 every option is within 1e-5 of the CPU on the
  Netflix 576x324 pair and a 3840x2160 clip, and `motion` / `motion2` with the
  previous options are bit-identical to the previous build.


- **HIP extractors share one upload of each frame (ADR-1408).** Every HIP
  extractor used to copy the planes it reads to the device itself and wait
  for that copy, so a run with several extractors uploaded the same frame
  several times: 31 planes for a 4:2:0 frame pair with thirteen extractors.
  The `VmafContext` now uploads each plane once per frame and the extractors
  read that copy (`psnr`, `float_psnr`, `float_moment`, `ciede`, `ssim`,
  `float_ssim`, `vif`, `float_vif`, `adm`, `float_adm`, `motion`, `motion_v2`
  and `float_motion` on HIP). No score changes: every metric of every frame is
  bit-identical before and after. On a gfx1036
  `--backend hip --model version=vmaf_float_v0.6.1` goes from 57.3 to 46.9 ms
  per 1920x1080 frame (17.5 to 21.3 frames per second) and from 294 to 226 ms
  per 3840x2160 frame; `vmaf_v0.6.1` and runs whose time is all device
  kernels are unchanged. `--subsample` stays correct: a plane an extractor
  still reads is not overwritten.


- **The parity gate bounds `speed_chroma` between the CPU and the HIP twin at
  `5e-6` instead of `5e-5`** (ADR-1452). `speed_chroma_hip` rounds `log2`
  correctly and `speed.c` calls the C library's `log2f`; on a gfx1036 13 of
  990 values differ from a glibc CPU, by 1.4e-6 at most, and none with a
  correctly rounded `log2f` preloaded, the CUDA twin's figures (ADR-1430). No
  score changes. `test_hip_speed_chroma_parity` now compares all three scores
  of every frame on a fixture that reaches the scoring path.


- **Every HIP kernel is built with contraction off (ADR-1407).** hipcc fuses
  `a * b + c` into one multiply-add for device code by default, and all but
  three HIP kernels were built that way, so they rounded differently from the
  CPU extractors they mirror. One flag list, `hip_strict_fp_args`
  (`-ffp-contract=off`, `-fhip-fp32-correctly-rounded-divide-sqrt`), now
  applies to every kernel, and the per-kernel flag table is gone. On a gfx1036
  `float_adm_hip` moves from 2.5e-5 to 2.5e-6 from the CPU on the Netflix
  576x324 pair and `float_ssim_hip` from 1.8e-7 to 1.2e-7; `float_vif_hip`'s
  worst frame moves from 2.7e-5 to 3.8e-5 with the same mean; twelve twins
  produce bit-identical output; every twin stays inside its parity tolerance.
  `float_vif_hip` is 4% slower at 3840x2160 and no other twin changes
  measurably. `test_hip_fp_arith_contract` checks the arithmetic on the
  device and `test_hip_strict_fp_policy.py` the build files.


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


- **Restore `adm_sum_cube_s_p3`, `adm_csf_den_scale_s_p3`, and `adm_cm_s_p3` fast-path
  functions in `adm_tools.c` (ADR-0463 / BUG-048 B3).**
  The specialized `adm_p_norm == 3.0` fast-paths eliminate all per-pixel `powf()`
  calls and branch overhead on the hot path for default VMAF evaluation.
  Scores remain 100% bit-identical to the baseline generic path across all 48
  frames on the Netflix 576x324 reference pair at `--precision max`.
  Dispatched once per scale in `core/src/feature/adm.c`.


- `ai/scripts/extract_k150k_features.py`: auto-select `/dev/shm` as the
  YUV scratch directory when `/dev/shm` is writable and has at least 20 GiB
  free (Win 3 — Research-0135).  Eliminates NVMe I/O for the ~1.5 GiB
  per-clip raw YUV intermediate, saving an estimated 5–15 s per clip on
  NVMe-bound hosts.  Falls back to the OS temp directory when `/dev/shm` is
  absent, unwritable, or has insufficient free space.  Pass `--scratch-dir`
  to override auto-selection.


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


- **Preallocate pinned host pictures for zero-copy 4K CLI CUDA upload (ADR-1406).**
  The `vmaf` CLI now preallocates pinned host pictures via `VmafPicturePool` when
  running CUDA-accelerated feature extractors, eliminating the synchronous driver
  bounce buffer copy on pageable host memory. At 3840×2160 on RTX 4090, default
  model frame time drops from 5.42 ms to 4.87 ms/frame (10.1% faster), `psnr_cuda`
  from 2.29 ms to 2.10 ms/frame (8.3% faster), `adm_cuda` from 3.99 ms to 3.65 ms/frame
  (8.5% faster), and `vif_cuda` from 2.32 ms to 1.82 ms/frame (21.6% faster). Scores
  remain bit-identical with zero drift across all features. If pinned memory allocation
  fails, the pool transparently falls back to pageable memory.


- **`float_ssim` runs on the CUDA device at every scale and equals the CPU's
  score (ADR-1399).** `float_ssim_cuda` computed scale 1 only, so at
  1920x1080 and 3840x2160 `--backend cuda --feature float_ssim` and models
  computed the feature on the CPU and printed a fallback warning. The twin now
  reduces both pictures on the device the way the CPU does (the automatic
  scale and every `scale` from 1 to 10) and adds its two Gaussian passes in
  double precision like the CPU. On an RTX 4090 every measured frame equals
  `--backend cpu` at `--precision max` (576x324, 1920x1080 and 3840x2160; 8,
  10, 12 and 16 bits; `enable_lcs`, `enable_db` and `clip_db` included), where
  the twin used to be 1 to 3 units in the last fp32 place off on every frame.
  A 3840x2160 frame takes 3.0 ms through the CLI instead of 18.9 ms with the
  CPU fallback (11.0 ms for the CPU extractor on 16 threads). An explicit
  `scale=1` on a large picture is slower than before, 3.9 ms instead of 3.1 ms
  per 3840x2160 frame, because the double-precision sums then cover the full
  picture. `--feature float_ssim_cuda` no longer fails at sizes that decimate;
  it fails only when the reduced picture is smaller than 11x11. See the
  [CUDA guide](docs/backends/cuda/overview.md#float_ssim-runs-on-the-device-at-every-scale-adr-1399-2026-10-01).


- **Compact nonzero terms on device for `psnr_hvs_cuda`, reducing 4K frame time from 12.16 ms to 3.39 ms while preserving bit-exact CPU parity (ADR-1397).**
  The kernel compacts nonzero terms before device-to-host readback via block bitmasks and parallel prefix scan, dropping 4K readback size from 64.8 MB to ~11.0 MB (83% reduction) and reducing the host addition chain from 16.2M terms to ~2.7M terms. Because all kernel terms are non-negative squares and `x + 0.0f == x` in IEEE-754 single-precision float addition, omitting zero terms preserves the CPU's exact sequence and numerical sum bit-for-bit at `--precision max`. Closes `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`.


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


- **`float_ssim` runs on the HIP device at 1080p and 4K (ADR-1405).**
  `float_ssim_hip` implemented scale 1 only, so pictures with a short side of
  384 px or more were computed on the CPU, with the warning
  `float_ssim_hip cannot run 3840x2160 8-bit pictures with these options`.
  The twin now decimates on the device at the automatic scale and every
  explicit one, with the CPU's reduced planes bit for bit, and falls back only
  when the decimated plane is smaller than the 11x11 SSIM window. On a gfx1036
  `float_ssim` is within 1.8e-6 of the CPU at 3840x2160 and 4.3e-6 at
  1920x1080; a 4K frame takes 5.5 ms against 11.9 ms for the CPU extractor on
  16 threads and 19.7 ms for the previous fallback, a 1080p frame 2.0 ms
  against 2.7 ms and 6.6 ms. Scale-1 scores are unchanged.


- **HIP `psnr_hvs` uploads native samples and converts on device (ADR-1369 port).**
  `integer_psnr_hvs_hip` uploads raw native samples via `vmaf_hip_picture_upload()`
  (uint8_t for 8 bpc, uint16_t for 9–12 bpc), converts them to integers on the
  device in `psnr_hvs_score.hip`, and fuses all plane dispatches into a single
  kernel (`n_dispatches_per_frame = 1`). This eliminates host float conversion loops
  and removes 6 unused pinned host staging allocations (`h_uint_ref` and `h_uint_dist`).
  On AMD gfx1036, 4K frame time drops from 221.79 ms to 18.90 ms/frame (CPU 16t is 6.04 ms/frame).
  Output scores are within 8.37e-05 dB of CPU reference at 576x324 and within area-scaled
  tolerance at 4K. Also resolves a latent scaling defect on 9-bit and 11-bit depths,
  validated by `test_psnr_hvs_deep_parity`.


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


- **HIP `ssimulacra2` runs entirely on the device (ADR-1390).**
  `ssimulacra2_hip` no longer roundtrips through the host per scale; each frame
  is one upload of the raw Y/U/V planes in `submit()` and one 864-byte readback
  of per-scale totals in `collect()`. YUV-to-linear, XYB, IIR Gaussian blurs with
  a tiled shared-memory row pass (`SS2H_ROW_TILE` rows, single-wave blocks,
  two-slot ring, register prefetch), exact fp32-pair per-pixel SSIM and edge sums
  over a deterministic LDS reduction tree, and 2x2 downsample run on the device.
  On AMD gfx1036, a 3840x2160 frame takes 234.30 ms (CPU 16t takes 148.03 ms) and
  a 576x324 frame takes 5.68 ms (CPU 16t takes 2.89 ms). Scores match CPU
  reference within 1e-9 at `--precision max`: max abs diff is 1.123e-12 on
  576x324 and 5.826e-13 on 4K BBB. Rejects 4:0:0 input at init. See
  [SSIMULACRA 2](docs/metrics/ssimulacra2.md) and
  [ADR-1390](docs/adr/1390-hip-ssimulacra2-device-resident.md).


- **SpEED filters only the samples it keeps on non-x86 targets (port of
  Netflix/vmaf `76ea5f03`, [Netflix/vmaf#1653](https://github.com/Netflix/vmaf/pull/1653)).**
  `speed_chroma` and `speed_temporal` blur each plane with a Gaussian
  anti-alias filter and keep one sample in 256. On aarch64 and every other
  target without the AVX2 convolution the extractor now evaluates the filter
  at the kept samples only (`vif_filter1d_dec16_s()`): the vertical pass runs
  for one row in 16 and the horizontal pass for one column in 16 of it. The
  result has the bits of the filter-then-decimate path, so no score changes
  (`core/test/test_speed_filter.c`; before/after reports under `qemu-aarch64`
  are byte-identical). x86 is unchanged. The NEON covariance kernel of the same
  upstream pull request (`15297286`) is not taken: its partial sums are not
  bit-identical to the scalar kernel; `docs/rebase-notes.md` has the numbers.
  `docs/backends/arm/overview.md` showed `--cpumask 0` as the scalar-only
  switch; `--cpumask` takes the bits to mask out, so that is `--cpumask 3` on
  aarch64 (corrected).


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


- **SYCL CLI preallocates pinned host USM pictures, dropping 4K upload latency from ~2.5 ms to 0.70 ms (ADR-1410).**
  The CLI picture pool allocates pictures in SYCL host USM (`sycl::malloc_host`) when `--backend sycl` is active, avoiding pageable memory staging and host copies on upload. Chroma planes in contiguous pinned host memory bypass staging buffers for direct DMA transfers. Measured on an Intel Arc A380 (Linux `xe` driver) with BBB 3840x2160: upload time dropped from 2.2–3.0 ms per frame down to 0.70 ms steady-state. Scores are bit-identical. Closes `T-SYCL-PAGEABLE-UPLOAD-HOST-STAGING-2026-09-29`.


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


- **SYCL `float_vif` kernels eliminate scratch memory and restore parity on Intel Arc A380 under the Linux `xe` driver (ADR-1395).**
  On the Arc A380 (`dg2-g11`, PCI `56a5`) under the Linux `xe` driver, SYCL kernels that use scratch memory or register spills return corrupted values. `launch_compute<0>` (previously spilled 14080 B private memory) and `launch_decimate<1>` (previously 2432 B private memory) are completely scratch-free (`private_mem_size == 0`, `spill_memory_size == 0` for both JIT and `dg2-g11` AOT). Filter coefficients are now evaluated via compile-time template constants with `#pragma unroll`, and `launch_compute<0>` utilizes the 256-register file (`VmafSyclKernelShape<32, 256>`), while scales 1-3 maintain default 128 GRF occupancy. Parity is restored: `test_sycl_float_vif_parity` and `test_sycl_float_vif_parity_large` pass; max absolute diff vs CPU is < 4e-5 on Netflix 576x324 and < 8e-6 on BBB 4K (was up to 0.3540 on master). 4K runtime improved from 25.61 ms/frame to 19.98 ms/frame (22% speedup).


- **`psnr_hvs_sycl` and `psnr_hvs_hip` compact nonzero terms on the device before readback (ADR-1397).**
  Because $x + 0.0\text{f} == x$ for every float value in the running sum, zero error terms
  contribute nothing to the plane score. Both twins now compute a 64-bit mask of nonzero terms per 8×8 block,
  perform work-group parallel prefix scans, and compact nonzero terms directly on the device into contiguous
  buffers prior to host readback. Readback size drops from ~198.3 MB to ~11.0 MB at 3840×2160 (94.4% reduction),
  from ~49.4 MB to ~1.39 MB at 1920×1080 (97% reduction), and from ~4.35 MB to ~0.21 MB at 576×324 (95% reduction).
  Host summation overhead in `vmaf_psnr_hvs_plane_score_compacted()` shrinks accordingly.
  Throughput at 3840×2160 improves from 39.23 ms to 29.30 ms/frame on Intel Arc A380 (SYCL) and from
  41.18 ms to 35.53 ms/frame on AMD gfx1036 (HIP). Every score on every frame remains bit-identical to
  `--backend cpu` of the same binary at `--precision max`, and the SYCL kernels remain completely scratch-free
  (0 private memory, 0 spills).


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


- **Reuse caller-supplied `tmpbuf` in scalar VIF fallback filters (`vif_filter1d_s`, `_sq_s`, `_xy_s`) (ADR-0463 / BUG-048 B4).**
  The scalar VIF fallback paths in `core/src/feature/vif_tools.c` now reuse the
  scratch buffer already allocated by `compute_vif` instead of performing
  per-invocation `aligned_malloc` and `aligned_free` calls. This eliminates
  up to 12 dynamic heap allocations per frame on architectures without AVX2
  float convolution (such as ARM64 and fallback CPU paths) while keeping scores
  100% bit-identical.


- `tools/vmaf-tune`: batch `TuneCache` index writes via in-memory caching and a dirty flag, flushed once per sweep or on LRU eviction rather than rewriting `__index__.json` on every `get()` / `put()`.


- The praetor governance engine moves from `f41e74d` to `6c772713a133`
  (ADR-1351), the newest praetor commit whose own CI is green. Its HISS
  scanners now find 244 existing issues the old engine did not measure: 140
  Python functions over 60 lines, 61 process exits from library code in
  Python, Go and Rust, 36 recursive Python functions and 7 Go calls without a
  deadline. The debt baseline records them, so the ratchet starts from 378
  entries instead of 182. The move refreshes the compiled agent context, the
  README governance block, the Paperclip harness, the branch ruleset template
  (now for `master`), the devcontainer's vendored praetor source and the
  documentation gate's locked files; the devcontainer keeps its
  `vmafx-dev-mcp` base image. `make verify-all` now also runs praetor's
  Documentation Governance gate (`make docs-lint` and `make docs-figures`),
  which needs Node.js 24. `.standards.yaml` declares the text register for
  every surface (agent-only text is `internal`, the terse `caveman` form).
  The audit in the git hooks passes `--offline`, which saves about 40 seconds
  per commit and push. Every workstation's `praetorctl` has to move to the new
  pin when this merges; the two engines do not read each other's trees
  ([CI guide](docs/development/ci.md#moving-the-praetor-pin)).


- **Cross-backend gate: the `psnr_hvs` tolerance grows with the frame size.**
  The CPU `psnr_hvs` adds every coefficient error of a plane into one `float`,
  so its rounding error grows with the number of 8x8 blocks, and a correct GPU
  twin landed 8.4e-4 dB away at 3840x2160 against a fixed 5e-4 tolerance.
  `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py` now multiply
  the `psnr_hvs` tolerance by √(N / N₅₇₆ₓ₃₂₄) above 576x324 (3.34e-3 at 4K);
  576x324 and smaller frames keep 5e-4. Both gates also stop crashing when a
  score is non-finite on both backends (JSON `null`, for example `psnr_hvs_cb`
  on identical chroma) (ADR-1361).


- **The first-release candidate plan now has eight candidates.**
  `v1.0.0-rc.3` owns twin exactness (every GPU and SIMD twin returns the CPU
  extractor's scores bit for bit, or carries a measured tolerance), `rc.4` the
  first full Rust metric (the whole `vmaf_v1.0.16_3d0h` path), `rc.5`
  deduplication, `rc.6` a generated per-vendor GPU capability table with a
  static audit of every kernel for every target, `rc.7` benchmarks, profiling
  and tuning, and `rc.8` the one-shot model retrain. Benchmarks and the retrain
  move back so that they run on a tree that is no longer being corrected or
  restructured. The release guide, roadmap, retrain runbook, tester guide,
  model card and `vmaf-rc1-report list-tools` inventory show the new mapping
  (ADR-1421).


- **Refactor CUDA test files part 1 for clang-tidy and HISS standard compliance (ADR-1142).**
  Brings 11 CUDA test files (`test_cuda_pic_preallocation.c`, `test_cuda_float_adm_parity.c`,
  `test_cuda_motion3_parity.c`, `test_cuda_psnr_parity.c`, `test_cuda_float_ms_ssim_parity.c`,
  `test_cuda_float_moment_parity.c`, `test_cuda_float_psnr_parity.c`, `test_cuda_speed_chroma_parity.c`,
  `test_cuda_ciede_parity.c`, `test_cuda_motion_v2_parity.c`, `test_cuda_preallocation_leak.c`)
  to 0 warnings in the `cuda` lane, tightening the baseline by 250 warnings (from 1149 to 899).
  Applies file-level ADR-1138 `modernize-use-nullptr` brackets, isolates variable declarations,
  and extracts helpers to satisfy function size and branch complexity constraints.


- **Refactor CUDA test files part 2 for clang-tidy and HISS standard compliance (ADR-1142).**
  Brings 10 CUDA test files (`test_cuda_ssim_parity.c`, `test_cuda_speed_chroma_smoke.c`,
  `test_cuda_speed_singular_parity.c`, `test_cuda_speed_temporal_parity.c`,
  `test_cuda_speed_temporal_smoke.c`, `test_cuda_drain_batch.c`,
  `test_cuda_picture_pinned_overflow.c`, `test_cuda_buffer_alloc_oom.c`,
  `test_cuda_single_frame_flush.c`, `test_cuda_arch_floor.c`) to 0 warnings in the
  `cuda` lane, tightening the baseline by 104 warnings (from 1110 to 1006).
  Applies file-level ADR-1138 `modernize-use-nullptr` brackets, isolates variable declarations,
  and extracts helpers to satisfy function size and branch complexity constraints.


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


- **Ten core C and C++ test files conform to clang-tidy and HISS standards (part 1).**
  The first batch of CPU and core test sources (`test_barten_csf.c`,
  `test_psnr_hvs_simd.c`, `test_propagate_metadata.c`, `test_thread_pool.c`,
  `test_context.c`, `test_predict.c`, `test_ciede.c`,
  `test_pic_preallocation.c`, `test_locale_handling.c`, and `test_dict.cpp`)
  were refactored to zero clang-tidy findings in the CPU lane under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch, nesting, and statement thresholds were split into modular helpers
  satisfying HISS-04 / NASA JPL Rule 4, eliminating 2 recorded infractions from
  `.standards-baseline.json`. C23 `nullptr` diagnostics in C files are scoped under
  [ADR-1138](docs/adr/1138-c23-nullptr-msvc-compat.md), and `test_dict.cpp` uses
  anonymous namespaces and standard `nullptr`. All tests continue to pass.


- **Eleven core C and C++ test files conform to clang-tidy and HISS standards (part 2).**
  The second batch of CPU and core test sources (`test_feature_collector.c`,
  `test_luminance_tools.cpp`, `test_framesync.c`, `test_log.c`, `test_psnr.c`,
  `test_version.c`, `test_adm_csf.c`, `test_cpu.c`, `test_ref.c`,
  `test_moment_simd.c`, and `tiny_ai_test_template.h`) were brought to zero
  clang-tidy findings on the CPU lane (-17 recorded findings) under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into modular helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics in C files are scoped under
  [ADR-1138](docs/adr/1138-c23-nullptr-msvc-compat.md), and all files retain valid
  SPDX license identifiers. All tests continue to pass.


- **Core test files brought to lint and HISS standards (batch C).**
  The final batch of core C test files (`core/test/test.h`,
  `core/test/test_float_adm_dwt2_neon.c`, `core/test/test_float_adm_neon.c`,
  `core/test/test_float_motion_neon.c`, `core/test/test_motion_neon.c`,
  `core/test/test_psnr_neon.c`, `core/test/test_ssim_neon.c`,
  `core/test/test_gpu_picture_pool.c`, `core/test/test_integer_cambi_sycl.c`)
  was brought to full compliance under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md).
  `test.h` modernized C++ typedefs to type aliases (`using`) and eliminated redundant
  void parameter lists. Missing SPDX license identifiers were restored per
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md). C23 `nullptr` diagnostics are
  scoped under [ADR-1138](docs/adr/1138-c23-nullptr-windows-portability.md) to maintain
  MSVC cl.exe compatibility across all architecture blocks. `test_pelorus_interop.c`
  is tracked as an exact-sync vendored Pelorus ABI mirror per
  [ADR-1113](docs/adr/1113-vendor-pelorus-interop.md).


- **Twelve CPU feature extractor and support files conform to clang-tidy and HISS standards (batch 1).**
  The first batch of CPU feature extractors and support headers (`integer_motion.c`,
  `integer_motion.h`, `convolution_internal.h`, `barten_csf_tools.h`, `blur_array.c`,
  `float_vif.c`, `math_utils.h`, `ms_ssim.c`, `ssim.c`, `integer_motion_v2.c`,
  `feature_collector.h`, and `ssim_tools.c`) were refactored to zero clang-tidy findings
  in the CPU lane under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions
  exceeding NASA JPL Rule 4 complexity and line-count limits in `integer_motion_v2.c`
  were split into modular helpers, eliminating 2 recorded infractions from
  `.standards-baseline.json`. All 12 reference score configurations remain bit-identical
  at `--precision max` across scalar, AVX2, and AVX-512 cpumasks, and the Netflix CPU
  golden gate passes with unchanged counts.


- **Ten CPU feature extractor and support files conform to clang-tidy standards (batch 3).**
  The third batch of CPU feature extractors and support headers (`alias.c`,
  `feature_name.h`, `iqa/iqa.h`, `iqa/iqa_options.h`, `iqa/iqa_os.h`,
  `moment_options.h`, `motion_blend_tools.h`, `motion_options.h`, `motion_tools.h`,
  and `niqe_model.h`) were refactored to zero clang-tidy findings in the CPU lane
  under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). All 12 reference
  score configurations remain bit-identical at `--precision max` across scalar,
  AVX2, and AVX-512 cpumasks, and the Netflix CPU golden gate passes with unchanged
  counts.


- **Nine HIP test files conform to clang-tidy and HISS standards (part 1).**
  The first batch of HIP test sources (`test_hip_smoke.c`,
  `test_hip_float_adm_parity.c`, `test_hip_motion3_parity.c`,
  `test_hip_float_vif_parity.c`, `test_hip_float_moment_parity.c`,
  `test_hip_cambi_parity.c`, `test_hip_ssimulacra2_parity.c`,
  `test_hip_motion_v2_parity.c`, `test_hip_float_psnr_parity.c`) were brought to
  zero clang-tidy findings in the HIP lane (-245 baseline findings) under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into clean helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics are scoped under ADR-1138
  to maintain MSVC C portability, and `__HIP_PLATFORM_AMD__` is supplied by the
  build harness (ADR-1263). All tests pass on AMD gfx1036.


- **Ten HIP test files conform to clang-tidy and HISS standards (part 2).**
  The second batch of HIP test sources (`test_hip_wavefront_reduce.c`,
  `test_hip_psnr_hvs_parity.c`, `test_hip_speed_chroma_parity.c`,
  `test_hip_speed_temporal_parity.c`, `test_hip_speed_singular_parity.c`,
  `test_hip_ciede_parity.c`, `test_hip_motion_parity.c`,
  `test_hip_vif_parity.c`, `test_hip_psnr_parity.c`, `test_hip_adm_parity.c`)
  were brought to zero clang-tidy findings in the HIP lane (-156 baseline
  findings) and cross-lanes under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into clean sub-helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics are scoped under ADR-1138
  to maintain MSVC C portability, and `__HIP_PLATFORM_AMD__` is supplied by the
  build harness (ADR-1263). All tests pass on AMD gfx1036.


- Refactored ai/scripts exporter and validator utilities to satisfy HISS-04 complexity limits (ADR-1142).


- Refactored 12 `ai/scripts/` training and corpus tools to satisfy HISS-04 complexity/length limits, bounded loops, and proper entrypoint error reporting under ADR-1142.


- Refactored 11 `ai/scripts/` training, feature materialization, and model exporter tools to satisfy HISS-01, HISS-04, and HISS-07 standards (ADR-1142).


- Refactored `ai/src/` and `ai/lpips_export.py` functions to resolve HISS-02 while loops and HISS-04 size violations under ADR-1142.


- Refactored oversized test functions in 12 files under `ai/tests/` to satisfy HISS-04 modular size bounds under ADR-1142.


- Refactored Go packages (`cmd/` and `pkg/`) to resolve HISS-02 context timeouts and HISS-07 exit/panic violations under ADR-1142.


- Added missing SPDX-License-Identifier declarations across 387 clean source
  and header files in accordance with ADR-1250 and repository provenance,
  skipping 131 files with baselined debt, 5 vendored Pelorus mirror paths,
  and 2 files undergoing concurrent review. Extended `scripts/ci/check-copyright.sh` and the `check-copyright`
  pre-commit hook to enforce valid SPDX license identifiers on languages named
  in ADR-1250 (C, C++, CUDA, Go, Python), backed by positive, negative, and
  boundary unit test suite in `scripts/ci/tests/test_check_copyright.py`.


- `cambi_sycl`'s `launch_reset` kernel now uses an explicit 1D `nd_range` and a
  scalar select chain for its per-scale top-K rank initialization instead of an
  array captured by value and indexed at runtime in the kernel closure
  (ADR-1395). This eliminates 1280 B of private array scratch memory and the
  896 B `RoundedRangeKernel` wrapper on Intel GPUs under the Linux xe driver,
  leaving every kernel in `integer_cambi_sycl.cpp` completely scratch- and
  spill-free. Bit-identical parity against `--backend cpu` is preserved on all
  tested fixtures (48/48 frames on Netflix 576x324, 50/50 frames on BBB 4K, max
  abs diff 0.0), with 4K throughput measured at 17.05 ms/frame on Arc A380
  (down from 18.53 ms/frame).


- **Six more SYCL twins are held to the CPU's bits by the parity gate.**
  `adm_sycl`, `motion_sycl` (also with `debug=true`), `motion_v2_sycl`,
  `psnr_sycl`, `float_ssim_sycl` (with and without `enable_lcs`) and
  `cambi_sycl` return the CPU extractor's scores bit for bit: measured on an
  Arc A380 at `--precision max` on 333 frames from 576x324 to 3840x2160 at 8
  to 16 bits, full-range noise included. They are now listed as exact twins,
  so the gate compares them with tolerance 0 where it allowed 5e-5, and
  `test_sycl_exact_twins` asserts equality on a device. With the twins made
  exact earlier, 19 of the 21 gate features are exact on SYCL; `ciede` is
  within its 1.4e-11 bound and `speed_chroma` depends on the build's math
  library. With the default model the VMAF score of every measured frame
  equals `--backend cpu`
  ([ADR-1451](docs/adr/1451-sycl-exact-twins-declared.md),
  [SYCL backend](docs/backends/sycl/overview.md#exact-twins-declared-as-a-group-2026-10-02)).


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


- **Regression test for model-collection growth failure.** When the array of a
  model collection cannot grow, `libvmaf` returns `-ENOMEM` and keeps the
  collection and its models intact; upstream Netflix/vmaf loses them
  (Netflix/vmaf PR #1590). The behaviour is unchanged, and
  `test_model_collection_growth` now holds it in place
  ([C API](docs/api/index.md)).


- **Regression test for reading frames with an odd width or height.** The raw
  and y4m readers consume a whole frame when a dimension is odd, where upstream
  Netflix/vmaf loses framing and crashes (Netflix/vmaf PR #1604). The behaviour
  is unchanged, and `test_video_input_odd_dims` now holds it in place for both
  reader entry points ([CLI](docs/usage/cli.md)).


- **The fixed-point VIF log2 table has one definition for every backend.**
  `vif_log2_table_generate()` moved to `core/src/feature/vif_log2_table.h`,
  which `integer_vif.h` includes. The SYCL and Metal hosts of the `vif` twins
  built the same 32768 values with copies of the expression and now call it,
  as the CPU extractor and the HIP host do. No score changes: `vif_sycl`
  stays bit-identical to the CPU on an Arc A380 (48 of 48 and 50 of 50
  frames) and `vif_hip` on a gfx1036; the Metal change is not built or run on
  this host. The table "Which HIP twins return the CPU's bits" on the
  [HIP backend page](docs/backends/hip/overview.md) is re-measured.


### Fixed

- **Integer ADM no longer scores isolated impairments above 1.** The scale-0
  contrast-masking threshold narrowed its centre tap to 16 bits, as upstream
  Netflix/vmaf does. A coefficient of 15360 or more wrapped the tap negative,
  and where such a coefficient stood alone the threshold added contrast instead
  of masking it: a flat grey 64x64 reference against the same picture with
  isolated 4x2 patches scored `integer_adm_scale0` 1.0829 and `integer_adm2`
  1.0355 where `float_adm` gives exactly 1. The tap is now 32 bits wide and the
  excess over the threshold is clamped in 64 bits, in the scalar, AVX2, AVX-512,
  CUDA, HIP, SYCL and Metal code (the second revision of Netflix/vmaf PR #1602,
  which upstream has not merged). Both patch pictures now score 1. The Netflix
  golden gate is unchanged (271 passed, 12 skipped before and after) and its 13
  fixture pairs give identical output at `--precision max` on the CPU and on
  the CUDA, HIP and SYCL twins. Scores change only where a scale-0 coefficient
  reaches 15360: independent full-range noise at 576x324 moves `integer_adm2`
  from 0.389548 to 0.389503 and `integer_adm_scale0` from 0.454972 to 0.454801.
  Until upstream merges #1602, integer ADM differs from upstream master on such
  content. The scalar tails of the AVX2 and AVX-512 kernels also no longer
  left-shift a negative threshold, which a UBSan build stopped on for a 24x24
  picture. The Metal change is source only; no Apple device was available
  (ADR-1402, `T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01`,
  `T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01`;
  [features](docs/metrics/features.md)).


- **Integer ADM no longer shifts a negative masking threshold on the scalar
  path.** The scale-0 contrast-masking step computed
  `abs(x) - (threshold << shift)` in signed arithmetic, and the threshold is
  negative when one large coefficient stands among small ones. That shift is
  undefined in C: a sanitizer build of `vmaf --feature adm --cpumask 4294967295`
  stopped on full-range noise with `left shift of negative value`. The scalar
  path, which every aarch64 run uses, now computes the expression modulo 2^32,
  as the AVX2 and AVX-512 vector code already did. Scores are unchanged on
  every dispatch level ([features](docs/metrics/features.md)).


- **Integer ADM gives one result for a non-integer `adm_enhn_gain_limit`,
  whichever code path computes it.** The scalar code bounds a restored sample
  with the double product of the sample and the limit, truncated toward zero.
  The AVX2 and AVX-512 decouple kernels rounded that product to nearest, as
  upstream Netflix/vmaf does, and the SYCL twin formed it in Q31 fixed point,
  so with a limit such as 1.2 each was one off in a share of the samples:
  `integer_adm_scale0` differed from the scalar path by up to 1.2e-6 per frame
  on the Netflix 576x324 pair, 6.2e-6 on a 352x288 pair and 3.6e-5 on blurred
  blocks. The vector kernels now truncate, and the SYCL twin forms the
  truncated double product exactly from 64-bit integer arithmetic. The scalar
  path, AVX2, AVX-512 and `adm_sycl` (Arc A380) give identical output at
  `--precision max` for limits of 1, 1.2, 1.5 and 100 on 20 test pairs and on
  Big Buck Bunny at 3840x2160. Nothing changes at the limits the shipped models
  use (1 and 100), and the Netflix golden gate is unchanged (271 passed, 12
  skipped before and after). `adm_cuda` and `adm_hip` already truncated. The
  Metal twin multiplies in single precision and was not changed, because no
  Apple device was available (ADR-1413,
  `T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`,
  `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`,
  `T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`;
  [features](docs/metrics/features.md)).


- **`libvmaf.h` and the API guide say what an index gap and an early query do.**
  `vmaf_read_pictures()` has always rejected a repeated or earlier index with
  `-EINVAL`; it also accepts an index that skips values, and then the motion
  extractors write no `motion2` / `motion3` for the pictures that follow, so
  reading them returns `-EAGAIN` even after the flush. A score asked for
  before the flush returns the value or `-EAGAIN`, never a partial value. The
  Doxygen of `vmaf_read_pictures()`, `vmaf_score_at_index()`,
  `vmaf_feature_score_at_index()` and the two pooled calls now carry both
  rules, and [the API guide](docs/api/index.md#scoring-before-the-flush-and-index-gaps)
  has a section on them (ADR-1429, Netflix/vmaf#910, #755, #1180). No
  behaviour changes.


- **`--threads` no longer breaks GPU twins that are selected by name.**
  `vmaf --backend hip --feature adm_hip --threads N` (and `float_vif_hip`)
  exited with `problem flushing context` for every `N` and wrote no score:
  a twin without a backend flag was handed to the CPU worker pool, whose
  workers call `extract()`, which a `submit()` / `collect()` extractor does
  not have. Such an extractor now runs on the thread that calls
  `vmaf_read_pictures()` whatever its flags, and threaded and unthreaded runs
  give the same scores bit for bit.


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


- **`test_cuda_parity_gate_default_run` no longer times out on a build
  without CUDA while another job holds the CUDA device lock.** The test takes
  the per-device lock before it runs the gate, and it learned that the binary
  has no CUDA only from the gate's output afterwards. On a HIP-only or
  SYCL-only build directory it therefore waited for a device it cannot use and
  hit its 120 s timeout whenever the lock was busy. It now reads Meson's
  option record of the build directory first and skips at once when
  `enable_cuda` is off.


- **The CUDA parity-gate default run skips on a build without CUDA.**
  `test_cuda_parity_gate_default_run` is registered for every build, and on a
  libvmaf built without CUDA the `vmaf` CLI refuses `--backend cuda`, which
  the test reported as a failed parity gate: `--suite=gpu` on a HIP-only or
  SYCL-only build had one failing test. The refusal is now a skip, like a
  missing device, and `test_cuda_parity_gate_skip` pins the decision to the
  CLI's message without a device.


- **Cross-backend parity gate compares emitted default motion metrics.**
  `scripts/ci/cross_backend_parity_gate.py` and `cross_backend_vif_diff.py`
  read `integer_motion`, which the CLI only emits in debug mode, causing a
  `KeyError` in the motion cell during default runs. Updated the motion metric
  keys to `integer_motion2` and `integer_motion3` so the full default parity matrix
  completes without error across backends. Also updated
  `scripts/ci/test_cross_backend_feature_names.py` to test active backends instead
  of the removed Vulkan backend.


- **`motion_sycl` defaults `debug` to `false`, and the parity gate reports a
  missing metric as a cell error.** `motion_sycl` declared `debug` with default
  `true`, so a default run emitted `integer_motion` while the CPU, CUDA and HIP
  extractors did not; it now follows them (pass `debug=true` to get the score).
  `scripts/ci/cross_backend_parity_gate.py` and
  `scripts/ci/cross_backend_vif_diff.py` gain a `motion_debug` cell
  (`motion` with `debug=true`, comparing `integer_motion`, `integer_motion2`
  and `integer_motion3`). A metric that one backend does not emit now makes
  its cell `ERROR` and names the backend, instead of ending the whole matrix
  with `KeyError`. See
  [ADR-1418](docs/adr/1418-motion-parity-gate-metric-alignment.md)
  (`T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`).


- **`vmaf` CLI now accepts odd dimensions for raw YUV 4:2:0 and 4:2:2 inputs.**
  `validate_chroma_alignment()` (`core/tools/vmaf.cpp`, ADR-0461) previously
  refused odd widths for 4:2:0 and 4:2:2 inputs and odd heights for 4:2:0
  inputs. However, `.y4m` inputs with odd dimensions were already accepted and
  processed with ceiling chroma extent (`vmaf_chroma_extent()`,
  `core/src/picture_geometry.h`, PR #1643). Per user decision 2026-10-01
  ("Accept both (Recommended)"), the CLI accepts odd dimensions for raw YUV
  inputs with ceiling chroma, bit-identically matching `.y4m` scores for identical
  content (ADR-1398). Files with mismatched frame byte sizes continue to exit 2
  cleanly via `yuv_check_file_size()`.


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


- **The `vmaf` CLI's read-ahead checks its invariants with assertions again.**
  A lint cleanup had replaced the seven `assert()`s of the frame reader by
  early returns, so a broken invariant would have dropped a frame or ended a
  stream without a message instead of stopping a debug build. No release
  carried the change. The clang-tidy finding that prompted it is a false
  positive of clang-tidy 22 on glibc 2.44 hosts; the CI image does not report
  it.


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


- **CodeQL include-non-header alert #1309 resolved with internal test accessors and CI guard.**
  `core/test/test_feature_backend_twin.c` linked directly against `libvmaf` instead
  of unity-including `core/src/libvmaf.c`. Narrow internal accessors
  (`vmaf_backend_twin_verdict_for_test`, `vmaf_context_fake_backend_for_test`,
  `vmaf_context_set_gpumask_for_test`, `vmaf_context_append_registered_feature_extractor_for_test`,
  and `vmaf_context_resolve_context_fallbacks_for_test`) are declared in
  `core/src/libvmaf_priv.h` with static definitions in `core/src/libvmaf.c`. A new
  `scripts/ci/check-no-non-header-includes.sh` check runs in pre-commit and CI to
  prevent non-header source file inclusions under `core/test/`.
- **Scorecard SAST alert #6 resolved by running CodeQL Actions universally on every PR.**
  Scorecard's `sastToolInCheckRuns` evaluates PR head commits across the last 30 commits
  on master. Under [ADR-1389](docs/adr/1389-codeql-actions-universal-pr-sast.md),
  `CodeQL (Actions)` now runs unconditionally on all pull requests and pushes,
  providing 100% commit SAST coverage across docs-only and non-code PRs with
  negligible (~15–20s) overhead, and is enforced in the required checks aggregator.


- **Brought CPU lane back to clang-tidy baseline.** Resolved 13 clang-tidy
  regressions introduced by merges into `core/src/picture_pool.cpp`,
  `core/src/read_json_model.cpp`, `core/test/test_psnr_hvs_score.c`,
  `core/test/test_read_pictures_failure_ownership.c`, and `core/tools/vmaf.cpp`
  without baseline modifications or `NOLINT` waivers. Refactored callbacks,
  sign comparisons, test helpers, and assertion guards to preserve exact
  behavior and numerical equivalence across all CPU test suites.


- **CUDA: `adm_cm.fatbin` register pressure and spill stack eliminated.**
  Restructured `adm_cm_aim_line_kernel` into adaptive launch bounds
  (`adm_cm_aim_line_kernel_2` and `adm_cm_aim_line_kernel_4`, ADR-1226) and fused
  scales 1-3 (`i4_adm_cm_aim_line_kernel_fused`), eliminating the 255-register
  ceiling and 344-byte spill stack (`STACK:0`, `LOCAL:0` across all architectures,
  `REG <= 176` on sm_89, max 208 on sm_100/120) with bit-identical scores and
  19-31% whole-feature speedups (`T-CUDA-ADM-CM-REGISTER-PRESSURE-2026-09-07`).


- **`adm_cuda` returns the CPU's scores bit for bit.** The CUDA twin of the
  fixed-point ADM extractor carried its own copy of the CSF weight routine,
  which multiplied the exponent in `float` where the CPU multiplies in
  `double`, so its weights were 1 to 3 units in the last place off and
  `integer_adm_scale1..3`, `adm2` and `adm3` up to 2.1e-7 from the CPU. Its
  denominator kernels rounded each warp of a row where the CPU rounds the
  row, which shows on frames with little reference detail (6.6e-7), and
  derived the scale-0 rounding shift from an fp32 logarithm: on frames whose
  scale-0 border region has an area just above a power of two (81 areas up
  to 2^26, for example 962x13542) the denominator came out twice too large
  and `integer_adm_scale0` up to 0.12 too low. The twin now takes its CSF
  weights, rounding shifts and score conclusion from the CPU's own routines
  and folds the denominator once per row
  ([ADR-1416](docs/adr/1416-cuda-adm-cpu-row-rounding.md)). Measured on an
  RTX 4090 at `--precision max`: every output of every frame identical on
  the Netflix pair at 8, 10, 12 and 16 bits, both 1080p checkerboard pairs
  and BBB 3840x2160, with `debug=true` and with every option, including
  `adm_csf_mode` 1 to 3 and `adm_skip_scale0`. The parity gate compares this
  twin with tolerance 0. No change in time per frame (3.63 and 3.66 ms at
  3840x2160). Stored `adm_cuda` outputs change by up to 2.1e-7, and on the
  frame sizes above by the amounts given. The CPU extractor's scores do not
  change.


- **`ciede_cuda` computes the CPU's arithmetic and agrees with it to 1e-11.**
  The CUDA twin of `ciede` was up to 1.1e-5 from the CPU extractor and
  matched it on no frame. The CPU computes CIEDE2000 in double precision and
  stores intermediate values in `float`; the twin computed everything in
  `float`, with another form of the formula, and added per 16x16 block. The
  twin now evaluates the CPU's expressions in the CPU's types and adds the
  per-pixel values on the host in the CPU's order
  ([ADR-1426](docs/adr/1426-cuda-ciede-cpu-arithmetic.md)). Measured on an
  RTX 4090 at `--precision max`: 62 of 113 frames identical to the CPU and
  the rest within 1.4e-11 (Netflix 576x324 at 8, 10, 12 and 16 bits, both
  1080p checkerboard pairs, BBB 3840x2160). What is left is the math library:
  the CPU calls glibc, the device CUDA's functions, and a few pixels per
  million round to the neighbouring `float` (38 of 8.3 million on a 4K frame,
  because glibc's `powf` is not correctly rounded). The parity gate compares
  this twin at `1e-9` instead of `5e-3`. The price is time: a 3840x2160 frame
  takes 32.7 ms instead of 2.8 ms and a 576x324 frame 0.74 ms instead of
  0.34 ms, because the per-pixel math is now double precision; the CPU
  extractor takes 222 ms per 4K frame on sixteen threads. Stored `ciede_cuda`
  outputs change by up to 1.1e-5. The SYCL, HIP and Metal twins keep their
  `float` arithmetic and the `5e-3` tolerance.


- **A CPU extractor scores all three planes of device-resident input.** With
  the pictures in device memory (the FFmpeg `libvmaf_cuda` path, or the
  `DEVICE` picture preallocation) and an extractor that runs on the CPU,
  libvmaf downloaded the luma plane into the host picture only. The chroma
  planes stayed uninitialised: `psnr_cb` and `psnr_cr` came out as the 60 dB
  cap (the CPU gives 12.54 dB on the test pictures) and no error was reported.
  The download now takes every plane the picture has (Netflix/vmaf#1613).


- **`float_adm_cuda` returns the CPU's scores bit for bit.** The CUDA twin
  of `float_adm` was up to 1.3e-5 from the CPU extractor (`adm_scale0` at
  3840x2160) and matched it on 144 of 791 measured scores. Nine things
  differed: the association of the angle test's threshold (the 1.3e-5), the
  order of the sums, CSF weights from a copied formula that rounded
  differently, a true division where the CPU multiplies by a refined
  reciprocal estimate, the order of the masking threshold's terms, `float`
  constants and a `float` gain limit where the CPU uses `double`, and a
  floor of the frame sums at `1e-2` where the CPU's is `1e-10`. The last one
  could report `adm2 = 1` where the CPU reports 0, for content with almost no
  reference detail scored without the noise floor. The twin now runs the
  CPU's arithmetic in the CPU's types, adds each row on the device and the
  rows on the host in the CPU's order, and takes the weights, the reduced
  region, the pooling and the floor from the CPU's own routines
  ([ADR-1420](docs/adr/1420-cuda-float-adm-cpu-arithmetic.md)). The CPU's
  division is built on the processor's `RCPSS` estimate, so the twin probes
  that estimate when the extractor starts (about 10 ms) and evaluates it on
  the device. Measured on an RTX 4090 at `--precision max`: every output of
  every frame identical on the Netflix pair at 8, 10, 12 and 16 bits, both
  1080p checkerboard pairs and BBB 3840x2160, also with `debug=true` and
  with non-default `adm_enhn_gain_limit`, `adm_bypass_cm`,
  `adm_noise_weight`, `adm_skip_aim_scale` and viewing geometry. The parity
  gate compares this twin with tolerance 0. Not identical: `adm_p_norm`
  other than 1 or 3, where the twin is within 1.1e-7 of the CPU (the two
  `powf` implementations differ). A run of the twin alone takes 1.98 ms per
  3840x2160 frame instead of 1.87 ms; its kernels take 1.11 ms instead of
  0.76 ms. Stored `float_adm_cuda` outputs change in their low digits by at
  most 1.3e-5. The SYCL, HIP and Metal twins still agree with the CPU to
  four decimal places.


- **`float_moment_cuda` is bit-identical to the CPU `float_moment` extractor
  at 16 bits.** The CPU forms each sample's square in `float` before adding
  it, which at 16 bits is the square rounded to 24 bits; the CUDA twin added
  exact integer squares. Its second moments (`float_moment_ref2nd`,
  `float_moment_dis2nd`) were up to 1.0e-4 from the CPU's on 16-bit content
  with real low bits (0 of 77 such frames identical on an RTX 4090), while
  8-, 10- and 12-bit input and the first moments were identical. The 16-bit
  kernel now adds the CPU's float square, and all four outputs are identical
  on 262 of 262 measured frames at `--precision max`. The parity gate
  compares the CPU and CUDA `float_moment` cells with tolerance 0. One range
  stays within a derived bound instead: on a 16-bit frame of more than
  2 097 152 pixels whose sum of squares passes 2^53 the CPU's own sum rounds
  as it goes (2.7e-7 measured at 2560x1440). No measurable cost. Stored
  16-bit `float_moment_cuda` second moments change by up to 1.0e-4
  ([ADR-1453](docs/adr/1453-cuda-float-moment-cpu-float-squares.md),
  [CUDA backend](docs/backends/cuda/overview.md#float_moment_cuda-matches-the-cpu-float_moment-at-16-bits-2026-10-02)).


- **`float_motion_cuda` returns the CPU's scores bit for bit.** The CPU
  `float_motion` extractor adds the absolute differences of a row into one
  `float`, the row sums into another, and divides in `float`, so its score
  depends on that order. The CUDA twin summed each 16x16 block on the device
  and the blocks in `double` on the host, which left `motion`, `motion2` and
  `motion3` up to 1.36e-4 from the CPU on 1920x1080 checkerboards (above the
  5e-5 cross-backend tolerance), 2.4e-5 at 3840x2160 and 3.1e-6 on the
  Netflix 576x324 pair. It now adds each row on the device in the CPU's
  order and the rows on the host
  ([ADR-1409](docs/adr/1409-float-motion-twins-cpu-float-sum.md)). Measured
  on an RTX 4090 at `--precision max`: every frame identical on the Netflix
  pair, both 1080p checkerboard pairs and 200 frames of BBB 3840x2160, also
  at 10 bits and with the fps-weight, cap and blend options set; the time per
  3840x2160 frame did not change (3.00 and 2.98 ms). The parity gate compares
  this twin with tolerance 0. Stored `float_motion_cuda` outputs change in
  their low digits by at most those differences. The SYCL, HIP and Metal
  twins still agree with the CPU to four decimal places.


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


- **`float_psnr_cuda` is bit-identical to the CPU `float_psnr` extractor at
  every bit depth.** The CPU squares each sample difference in `float` and
  adds the squares in `double`, which does not round. The CUDA twin added
  each 16x16 block in single precision: exact at 8 bits, and at 10, 12 and
  16 bits only while the differences in a block are small. On an RTX 4090 it
  matched the CPU on every frame of real clips and was up to 1.2e-7 dB off
  on high-bit-depth input with large differences (2 of 92 such frames
  identical). The kernel now adds the squares as integers, and 268 of 268
  measured frames are identical at `--precision max`, with `uncapped=true`
  too. The parity gate compares the CPU and CUDA `float_psnr` cells with
  tolerance 0. No measurable cost. Stored `float_psnr_cuda` scores of such
  input change by up to 1.2e-7 dB
  ([ADR-1455](docs/adr/1455-cuda-float-psnr-exact-block-sums.md),
  [PSNR](docs/metrics/psnr.md#float_psnr)).


- **`float_vif_cuda` returns the CPU's scores bit for bit.** The CUDA twin
  filtered with a table of Gaussian taps that the CPU `float_vif` extractor
  stopped using when it began to compute its filters at start-up (26 of the
  34 taps differ in the last digits), called the device `log2f` where the
  CPU evaluates a polynomial, kept `vif_sigma_nsq` in `float` where the CPU
  keeps it in `double`, and summed per 16x16 block where the CPU adds row by
  row in `float`. That left `vif_scale0..3` up to 3.8e-5 from the CPU on the
  Netflix 576x324 pair and 7.0e-6 at 3840x2160, with no frame identical. The
  twin now takes the taps from the CPU's own routine, evaluates the CPU's
  per-pixel statistic in its types, and adds the terms of each row on the
  device and the rows on the host in the CPU's order
  ([ADR-1412](docs/adr/1412-cuda-float-vif-cpu-arithmetic.md)). Measured on
  an RTX 4090 at `--precision max`: every output of every frame identical on
  the Netflix pair at 8, 10, 12 and 16 bits, both 1080p checkerboard pairs
  and BBB 3840x2160, also with `debug=true` and with non-default
  `vif_enhn_gain_limit`, `vif_sigma_nsq` and `vif_skip_scale0`. The parity
  gate compares this twin with tolerance 0. `float_vif_cuda` also accepts
  the CPU's per-scale floors `vif_scale1_min_val`, `vif_scale2_min_val` and
  `vif_scale3_min_val`. A run of the twin alone takes the same time per
  3840x2160 frame (1.97 and 1.96 ms); its kernels take 1.00 ms instead of
  0.72 ms. Stored `float_vif_cuda` outputs change in their low digits by at
  most 3.8e-5. The SYCL, HIP and Metal twins still agree with the CPU to four
  decimal places.


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


- **`motion_cuda` emits `VMAF_integer_feature_motion_sad_score`, as the CPU
  `motion` extractor does.** The CPU writes the frame's SAD score on every
  frame (weighted by `motion_fps_weight`, capped at `motion_max_val`); the
  CUDA twin computed it and published it only as the debug `integer_motion`
  score, so the result of `--backend cuda --feature motion` lacked a key the
  CPU result has. It now writes it on every frame, bit-identical to the CPU
  on 348 of 348 measured frames on an RTX 4090, also with `debug`,
  `motion_force_zero`, `motion_moving_average` and weight, blend and cap
  options. `motion2` / `motion3` and the frame time are unchanged
  ([motion](docs/metrics/motion.md#output-features)).


- **CUDA: `test_cuda_runtime_unwind` pins allocating state on host-pinned pictures.**
  Host-pinned pictures (`vmaf_cuda_picture_alloc_pinned`) record the allocating
  state on `priv->cuda.state`, preventing a NULL dereference of `state->f` during
  unref (upstream Netflix/vmaf#1573 hunk a). A device-free test
  `test_pinned_picture_release_uses_the_allocating_state` exercises the allocation
  and unref through the fake driver table, ensuring the allocating state is pinned
  across platforms.


- **CUDA `psnr_hvs` returns the CPU extractor's scores bit for bit**
  (`T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30`,
  [ADR-1397](docs/adr/1397-psnr-hvs-twins-cpu-float-sum.md)). The CPU adds every
  masked coefficient error of a plane into one running `float`, so its score
  depends on the order of the additions; `psnr_hvs_cuda` summed each block first
  and was up to 1.7e-2 dB from `--backend cpu` at 3840x2160, beyond the parity
  tolerance. The kernel now stores the 64 terms of every block in the CPU's
  arithmetic and the host adds them in the CPU's order: `psnr_hvs`, `psnr_hvs_y`,
  `psnr_hvs_cb` and `psnr_hvs_cr` are identical to the CPU at `--precision max`
  from 576x324 to 3840x2160 and at 8 to 12 bits, and the parity gate compares
  this twin with tolerance 0. `psnr_hvs_cuda` scores therefore change in their
  last digits (by up to 1.7e-2 dB at 3840x2160). The twin is slower for it: on
  an RTX 4090 a 3840x2160 frame takes 12.2 ms instead of 2.4 ms, and the term
  buffer needs 65 MB per 3840x2160 frame; tuning is tracked as
  `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. The HIP and SYCL twins
  keep their per-block sums until their rewrites land. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#agreement-with-the-cpu-extractor).


- **`speed_chroma_cuda` and `speed_temporal_cuda` match the CPU with
  `speed_prescale_method=lanczos4`.** The CPU scaler evaluates each lanczos4
  weight in fp64 with `sin()` and rounds it once; the CUDA scale kernel
  evaluated the weights itself in fp32, a few ulp off on some of them, and
  SpEED amplifies that on smooth content. On an RTX 4090 the twins were up to
  8.8e-3 relative (0.27 absolute) from the CPU on a smooth synthetic field and
  1.0e-3 absolute on a 1920x1080 gradient with noise, beyond the cross-backend
  tolerance of 1e-4. The weights depend only on the output column and row, so
  the host now evaluates them once per run with the scaler's own routine and
  the kernel reads the table: nearest, bilinear, bicubic and lanczos4 prescale
  at 0.5 and 2.0 are all bit-identical to the CPU extractor. Scores with the
  other three methods, and without prescale, are unchanged. The SYCL and HIP
  twins still evaluate the weights on the device
  ([SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device)).


- **`integer_ssim_cuda` returns the CPU's `ssim` bit for bit.** The CUDA twin
  of the fixed-point `ssim` extractor computed the CPU's moments and the
  CPU's per-pixel term, but added the terms per 16x8 block where the CPU adds
  them one after the other across the whole frame. Its score matched the CPU
  on no measured frame and was up to 1.1e-11 away (3.6e-10 with
  `enable_db`); on very small identical frames it could report `+inf` where
  the CPU reports about 156 dB. The twin now reads the per-pixel terms back
  and adds them on the host in the CPU's order
  ([ADR-1424](docs/adr/1424-cuda-ssim-cpu-frame-sum.md)). Measured on an RTX
  4090 at `--precision max`: identical on every frame of the Netflix pair at
  8, 10, 12 and 16 bits, both 1080p checkerboard pairs and BBB 3840x2160,
  with and without `enable_db` / `clip_db`, and on frames from 1x1 up. The
  parity gate now knows the `ssim` feature and compares the CUDA twin with
  tolerance 0. The price is time: a 3840x2160 frame takes 9.7 ms instead of
  2.2 ms, because 8.3 million terms are read back and added sequentially; at
  576x324 the difference is not measurable. The twin also needs 66 MB more
  device memory and as much pinned host memory at 3840x2160. The SYCL, HIP
  (above 64x64) and Metal twins still agree with the CPU to four decimal
  places or better.


- **`ssimulacra2_cuda` returns the CPU extractor's score bit for bit.** The
  CUDA twin of `ssimulacra2` computed the CPU's per-pixel terms but added
  them in a tree, where the CPU adds them one after the other into one
  `double`; every add rounds, so the two ended a few units in the last place
  apart. Measured on an RTX 4090 at `--precision max`, 8 of 113 frames
  matched and the rest were up to 7.3e-11 away. The twin now forms the sums
  of the CPU's loops on the device
  ([ADR-1433](docs/adr/1433-cuda-ssimulacra2-cpu-sum-order.md)): while a
  running sum stays between two powers of two, adding a term moves it by a
  whole number of steps, so the device adds those whole numbers per
  1024-pixel chunk in parallel, one pass over the chunks puts them together,
  and the few chunks in which the sum passes a power of two are added term by
  term. All 113 frames are identical (Netflix 576x324 at 8, 10, 12 and 16
  bits, both 1080p checkerboard pairs, BBB 3840x2160), and the parity gate
  compares the cell at 0 instead of `5e-3`. The price is time: a 3840x2160
  frame takes 15.6 ms instead of 7.8 ms and a 576x324 frame 1.7 ms instead of
  0.4 ms; the CPU extractor takes 126 ms per 4K frame on sixteen threads.
  Stored `ssimulacra2_cuda` scores change by up to 7.3e-11. The SYCL and HIP
  twins keep their tree sums and the `5e-3` tolerance.


- **Several CUDA instances on one device no longer get wrong `vif` scores.**
  `integer_vif_cuda` cleared its accumulators on its private stream while the
  scale 0 kernels that add into them ran on the picture stream, with nothing
  ordering the two. One instance never lost the race; with four instances on
  one `CUcontext` a late clear erased the first adds, and every run returned
  wrong `vif` scales (and, through the model, wrong VMAF) for tens of frames.
  The clear now runs on the picture stream. Measured on an RTX 4090 with four
  instances on one context, 48 frames of the Netflix 576x324 pair: 28 to 85
  wrong frames of 192 per feature in every run before, none in 105 runs after
  (Netflix/vmaf#1305).


- **Omit `-march=native` from the reference binary build in `vmaf-dev-mcp`.**
  Intel oneAPI `icx` contracts multiply-accumulate operations in unvectorized CPU
  extractor scalar loops when `-march=native` exposes FMA target capabilities,
  drifting SpEED scores from uncontracted reference builds by up to 7.9e-4 on
  1080p content and 3.38e-7 on the Netflix 576x324 48-frame pair. Removing
  `-Dc_args="-march=native"` restores bit-exact CPU reference parity with
  standard GCC reference builds, following the golden-gate isolation principles
  of [ADR-1317](docs/adr/1317-golden-gate-build-isolation.md).


- **`float_adm` refuses frames smaller than 17x17 instead of reading outside
  its buffers.** The float ADM extractor decomposes each frame into four
  wavelet levels. Below 17 pixels in width or height the coarsest level has a
  single sample, and the extractor read next to it: at 8 pixels or fewer the
  read lands before the start of a heap buffer (confirmed with
  AddressSanitizer), from 9 to 16 it picks up a sample of another level, so
  the scores were not meaningful (a random 8x8 pair scored `adm_scale3 =
  1.05`). `float_adm` and `float_adm_cuda` now fail at start with
  `float_adm requires width >= 17 and height >= 17 (got WxH)`, as the
  fixed-point `adm` extractor already does. Frames of 17x17 and larger are
  unaffected. The SYCL, HIP and Metal `float_adm` twins still accept smaller
  frames.


- **`--backend <gpu> --feature float_moment` runs the backend's twin.** The
  command computed `float_moment` on the CPU and warned that the backend had
  no twin, although `float_moment_cuda`, `float_moment_sycl`,
  `float_moment_hip` and `float_moment_metal` exist: the CPU extractor
  declared the pseudo-name `float_moment` instead of the four features it
  writes, so the lookup that pairs an extractor with its twin never matched.
  It now declares `float_moment_ref1st`, `float_moment_dis1st`,
  `float_moment_ref2nd` and `float_moment_dis2nd`. Naming the twin and the
  CPU extractor together (`--feature float_moment_cuda --feature
  float_moment`) no longer fails with `feature "float_moment_ref1st" cannot
  be overwritten`. Scores were correct before
  and are unchanged on the CPU; on an RTX 4090 the twin equals the CPU on
  every frame at 8, 10 and 12 bits and is within 7e-6 on the second moments
  at 16 bits. See [Float moment](docs/metrics/features.md).


- **The Gitleaks check scans only the commit it checked out.** It ran
  `git log --all` over a full-history checkout, so a finding on any branch in
  the repository, including a commit a force-push had already replaced,
  failed every other open pull request. Each run now scans the history of
  its own `HEAD`: on a pull request, master plus the PR's commits.


- **The `test-netflix-golden` target checks for pytest before execution.** When
  invoked in a fresh worktree where `.venv` only contains build-time dependencies,
  `make test-netflix-golden` previously stopped with `No module named pytest`.
  The target now checks for `pytest` availability up front and fails with an
  actionable error directing the developer to the documented install command in
  `docs/development/languages.md`.


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


- **`adm_hip` is bit-identical to the CPU `adm` extractor, and no longer
  returns garbage for the first frame of a second context.** The HIP twin
  rounded the ADM denominator once per thread where the CPU rounds once per
  row, derived a rounding shift on the device with an fp32 logarithm, and
  concluded each scale with host copies of the CPU's routines. On a gfx1036 a
  low-detail 576x324 frame was 4.0e-7 off in `integer_adm_scale3`, BBB
  3840x2160 up to 1.4e-7 in `integer_adm_scale0`, and a 962x13542 frame scored
  `integer_adm_scale0` 0.860 where the CPU scores 0.979. The twin now takes
  its CSF weights, border, shifts and score conclusion from the CPU's own
  routines and folds the denominator once per row: 21 fixture pairs from
  18x22 to 3840x2160 at 8 to 16 bits are identical at `--precision max`, the
  per-scale sums of `debug=true` included, with every option set tried.
  Separately, the twin cleared its accumulators ahead of each frame's upload,
  and on that device such a clear is lost in the first context of a process
  that needs larger planes than the contexts before it: a program scoring a
  256x144 clip and then a 3840x2160 clip through the library got `invalid ADM
  reduction` on the second. The clear now follows the upload. The parity gate compares the CPU and HIP `adm` cells
  with tolerance 0. Re-run any stored `adm_hip` output
  ([ADR-1423](docs/adr/1423-hip-adm-cpu-row-rounding.md),
  [HIP backend](docs/backends/hip/overview.md#adm_hip-returns-the-cpus-values-bit-for-bit-2026-10-01)).


- **`float_moment_hip` and `vif_hip` no longer return a wrong first frame in
  a later, larger context of one process.** Both cleared their device
  accumulators ahead of the frame's plane upload. On a gfx1036 such a clear
  has no effect in the first context of a process that needs larger planes
  than the contexts before it, so the frame's sums were added onto the sums
  the earlier context had left in recycled device memory: after a 640x360
  context, the first frame of a 3840x2160 context had `float_moment_ref1st`
  130.53 where the CPU has 127.00, and `vif_hip` scale 0 at 0.6748 where the
  CPU has 0.6934. Every HIP extractor now uploads, then clears, then launches
  its kernels (`float_psnr_hip` is reordered too; its scores were right).
  The `vmaf` tool creates one context per process and was not affected;
  programs that score several clips through the library were. A device test
  per extractor and a source check over every HIP file hold the order
  ([ADR-1427](docs/adr/1427-hip-clear-after-upload.md),
  [HIP backend](docs/backends/hip/overview.md#a-frame-clears-its-accumulators-after-its-upload-adr-1427)).


- **`float_moment_hip` is bit-identical to the CPU `float_moment` extractor
  at 16 bits.** The CPU forms each sample's square in `float` before adding
  it, which at 16 bits is the square rounded to 24 bits; the HIP twin added
  exact integer squares. Its second moments (`float_moment_ref2nd`,
  `float_moment_dis2nd`) were up to 1.0e-4 from the CPU's on 16-bit content
  with real low bits (0 of 77 such frames identical on a gfx1036), while 8-,
  10- and 12-bit input and the first moments were identical. The 16-bit
  kernel now adds the CPU's float square, and all four outputs are identical
  on 250 of 250 measured frames at `--precision max`. The parity gate compares
  the CPU and HIP `float_moment` cells with tolerance 0. One range stays
  within a derived bound instead: on a 16-bit frame of more than 2 097 152
  pixels whose sum of squares passes 2^53 the CPU's own sum rounds as it goes
  (2.7e-7 measured, 2.3e-5 at most at 3840x2160). No measurable cost. Stored
  16-bit `float_moment_hip` second moments change by up to 1.0e-4
  ([ADR-1447](docs/adr/1447-hip-float-moment-cpu-float-squares.md),
  [HIP backend](docs/backends/hip/overview.md#float_moment_hip-returns-the-cpus-moments-bit-for-bit-2026-10-02)).


- **`float_motion` on the HIP backend is bit-identical to the CPU, with every
  option.** The CPU extractor adds the absolute differences of a row into one
  `float`, the row sums into a second one, and divides in `float`; those
  running sums round at every step, so the score depends on the order of the
  additions. `float_motion_hip` added 16x16 blocks on the device and the
  blocks in `double` on the host, and was 3e-6 from the CPU on the Netflix
  576x324 pair, 1.4e-4 on 1080p checkerboards (above the 5e-5 cross-backend
  tolerance) and 2.2e-4 with `motion_add_scale1`. It now stores every absolute
  difference and adds each row in the CPU's order on the device, for the
  half-size term of `motion_add_scale1` and the chroma planes of
  `motion_add_uv` too, and `motion`, `motion2` and `motion3` equal
  `--backend cpu` at `--precision max` on a gfx1036: the Netflix pair at 8 and
  10 bits, both 1080p checkerboard pairs and BBB 3840x2160, with seven option
  sets (1617 of 1617 values; 237 before). The parity gate compares the CPU and
  HIP `float_motion` cells with tolerance 0. A 3840x2160 frame takes 19.8
  instead of 18.1 ms on that device, and 24.7 instead of 21.0 with
  `motion_add_scale1`. Re-run any stored HIP `float_motion` output
  ([ADR-1419](docs/adr/1419-hip-float-motion-cpu-float-sum.md),
  [HIP backend](docs/backends/hip/overview.md#float_motion_hip-options)).


- **`float_ms_ssim` on the HIP backend is bit-identical to the CPU.**
  `integer_ms_ssim_hip` accumulated its decimation and its Gaussian window
  sums as fp32 running sums, divided fp64 numerators by fp64 denominators and
  combined unrounded per-scale means, where the CPU extractor fuses each
  decimation tap, adds fp32 products in fp64, divides by fp32 denominators and
  combines fp32 means. On a gfx1036 no frame matched the CPU: the score was up
  to 3.0e-6 off and a per-scale mean up to 2.1e-5. The kernels now follow the
  CPU extractor operation for operation, as the CUDA twin does since ADR-1403,
  and every value of every frame is the CPU's at `--precision max`: the
  Netflix pair at 8 and 10 bits, both 1080p checkerboard pairs and BBB
  3840x2160, `enable_lcs`, `enable_db` and `clip_db` outputs included. The
  exact window sums cost time on that device: about 37 instead of 30 ms per
  1920x1080 frame and 169 instead of 158 ms per 3840x2160 frame. Re-run
  any stored HIP `float_ms_ssim` output
  ([HIP backend](docs/backends/hip/overview.md#integer_ms_ssim_hip)).


- **`float_psnr_hip` is bit-identical to the CPU `float_psnr` extractor at
  every bit depth.** The CPU adds the squared sample differences in `double`,
  which is exact. The HIP twin added each 16x16 block in single precision,
  which is exact at 8 bits and rounds at 10, 12 and 16 bits once the
  differences in a block are large. On real clips the two agreed; on
  full-range noise the twin was 6.3e-9 dB off at 10 bits, 2.5e-8 at 12 and
  1.8e-8 at 16, and 7.6e-8 on a bright 16-bit 1080p pair. The twin now adds
  the same squares as integers: 178 of 178 measured frames from 480x270 to
  3840x2160 at 8 to 16 bits are identical at `--precision max` on a gfx1036
  (167 before), with `uncapped=true` too, at the same time per frame. The
  parity gate compares the CPU and HIP `float_psnr` cells with tolerance 0.
  Stored `float_psnr_hip` scores of high-bit-depth clips with heavy
  distortion change by up to 7.6e-8 dB
  ([ADR-1440](docs/adr/1440-hip-float-psnr-exact-block-sums.md),
  [PSNR](docs/metrics/psnr.md#float_psnr)).


- **`float_ssim_hip` is bit-identical to the CPU `float_ssim` extractor.**
  The CPU adds the eleven products of a Gaussian window in `double` and
  rounds once per pass. The HIP twin added them in single precision, which
  rounds at every tap: on a gfx1036, 27 of 178 measured frames had the CPU's
  score and the others were up to 4.8e-7 away (5.4e-7 in the `enable_lcs`
  contrast and structure means). The twin now forms its window sums and its
  luminance, contrast and structure terms through the arithmetic
  `float_ms_ssim_hip` already shares with the CPU: all 178 frames from
  480x270 to 3840x2160 at 8 to 16 bits are identical at `--precision max`,
  with `enable_lcs`, `scale`, `enable_db` and `clip_db` too. The parity gate
  compares the CPU and HIP `float_ssim` cells with tolerance 0. It costs
  time: 2.0 ms instead of 1.7 ms per 1920x1080 frame and 5.2 ms instead of
  4.9 ms per 3840x2160 frame at the default scale, and a third more with
  `scale=1` (23.4 ms instead of 17.7 ms at 1080p). Stored `float_ssim_hip`
  scores change by up to 4.8e-7
  ([ADR-1441](docs/adr/1441-hip-float-ssim-cpu-window-sums.md),
  [HIP backend](docs/backends/hip/overview.md#float_ssim_hip-at-1080p-and-4k)).


- **`float_vif_hip` is bit-identical to the CPU `float_vif` extractor, and
  no longer faults on small frames.** The HIP twin filtered with a table of
  Gaussian taps the CPU stopped using, called the device `log2f()` where the
  CPU evaluates a polynomial, took the noise variance as a `float` where the
  CPU keeps a `double`, and added per wave and per block where the CPU adds
  row by row. On a gfx1036 10 of 712 scores (four scales, 178 frames from
  480x270 to 3840x2160 at 8 to 16 bits) were the CPU's; the others were up to
  3.8e-5 away on typical content and 1.06e-4 on bright 16-bit content, more
  than the twin's 5e-5 gate tolerance. The twin now runs the arithmetic of
  the CUDA twin from one shared header (`float_vif_gpu_common.h`) and all 712
  scores are identical at `--precision max`, with `debug=true` and the
  feature options too. It gains the CPU's `vif_scale1_min_val`,
  `vif_scale2_min_val` and `vif_scale3_min_val`. Frames smaller than 72
  pixels in either dimension, which ended with a GPU memory fault, now run.
  The parity gate compares the CPU and HIP `float_vif` cells with tolerance
  0. A frame takes 26.0 ms instead of 20.7 at 1920x1080 and 147 ms instead of
  86 at 3840x2160 on that device. Stored `float_vif_hip` scores change by up
  to 3.8e-5 ([ADR-1444](docs/adr/1444-hip-float-vif-cpu-arithmetic.md),
  [HIP backend](docs/backends/hip/overview.md#float_vif_hip-returns-the-cpus-scores-bit-for-bit-2026-10-02)).


- **`integer_ssim_hip` scores frames of up to 4096 pixels exactly as the CPU
  `ssim` does (ADR-1400).** The CPU adds one term per pixel in raster order,
  and on an identical frame the result is 1 or an ulp or two below it,
  depending on the frame: with `enable_db` an identical 1x1 frame of zeros
  reports 156.54 dB and a flat 3x3 frame of 51 reports 159.55 dB. The HIP twin
  reduced per block and reported `+inf` for every identical frame. For frames
  of at most 64x64 pixels the device now writes one term per pixel and the
  host adds them in the CPU's order, so the score equals the CPU's bit for
  bit at 8, 10, 12 and 16 bits, identical frames or not. Larger frames are
  unchanged. Verified on a gfx1036 by `test_hip_ssim_tiny_frames`.


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


- **`speed_chroma_hip` and `speed_temporal_hip` match the CPU with
  `speed_prescale_method=lanczos4`.** Like the CUDA and SYCL twins before
  them, they evaluated the lanczos4 kernel weights on the device in fp32,
  where the CPU scaler uses fp64 `sin()`, and SpEED amplifies the few-ulp
  differences on smooth content: on a gfx1036, `speed_chroma_hip` at
  `speed_prescale=0.5` was 8.8e-3 relative away from the CPU on a smooth
  1920x1080 field, and `speed_temporal_hip` 0.24 on a 1080p checkerboard pair
  at 2.0. The scale kernel now reads the weights from the table the host
  builds with the CPU scaler's own routine, and lanczos4 at 0.5 and 2.0 is
  bit-identical to the CPU extractor on that device (given a correctly rounded
  `log2f` on the CPU side, as for the other prescale methods). It is also
  faster: `speed_chroma_hip` with lanczos4 at 0.5 goes from 21.8 to 16.0 ms
  per 3840x2160 frame. Scores with the other three methods, and without
  prescale, are unchanged. This closes the lanczos4 prescale drift on all
  three device backends
  ([SpEED](docs/metrics/speed_qa.md#hip-device-resident-cpu-fp32-arithmetic)).


- **`integer_ssim_hip` is bit-identical to the CPU `ssim` extractor at every
  frame size.** The CPU adds the SSIM term of every pixel into one `double`,
  left to right and top to bottom. The HIP twin computed the same terms and
  added them in that order only for frames of at most 4096 pixels; above
  that it added them per 16x8 block, and on a gfx1036 the score of 1 of 178
  measured frames was the CPU's, the others up to 1.1e-11 away. The twin now
  stores every term and the host adds the plane in the
  CPU's order: 178 of 178 frames from 480x270 to 3840x2160 at 8 to 16 bits
  are identical at `--precision max`, with `enable_db` and `clip_db` too, and
  an identical frame reports the CPU's value at every size. The parity gate
  compares the CPU and HIP `ssim` cells with tolerance 0. Cost on the
  gfx1036: 30.0 ms instead of 28.2 ms per 1920x1080 frame and 98.1 ms instead
  of 94.3 ms per 3840x2160 frame, and 66 MB more device and pinned host
  memory at 3840x2160. Stored `integer_ssim_hip` scores change by up to
  1.1e-11 ([ADR-1438](docs/adr/1438-hip-ssim-cpu-frame-sum.md),
  [HIP backend](docs/backends/hip/overview.md#integer_ssim_hip)).


- **`ssimulacra2_hip` is bit-identical to the CPU `ssimulacra2` extractor.**
  The CPU evaluates six terms per pixel and channel in `double` and adds each
  into one `double`, pixel after pixel. The HIP twin evaluated the terms as
  pairs of floats and added them in a fixed tree: on a gfx1036 none of 178
  measured frames (480x270 to 3840x2160, 8 to 16 bits) equalled the CPU, and
  the score was up to 7.6e-11 away. The twin now evaluates the CPU's double
  expressions and forms the sums with the bits of the CPU's loops, from
  integer increments per binade as the CUDA twin does (`ordered_sum.h`). All
  178 frames are identical at `--precision max`, with every `yuv_matrix`. The
  parity gate compares the CPU and HIP `ssimulacra2` cells with tolerance 0
  instead of 5e-3. The twin is slower: 167 ms instead of 58 per 1920x1080
  frame and 662 ms instead of 234 per 3840x2160 frame on that device. Stored
  `ssimulacra2_hip` scores change by up to 7.6e-11
  ([ADR-1445](docs/adr/1445-hip-ssimulacra2-cpu-sum-order.md),
  [ssimulacra2](docs/metrics/ssimulacra2.md#hip-device-resident-tiled-row-pass)).


- **`vif_hip` is bit-identical to the CPU `vif` extractor.** The fixed-point
  VIF statistic takes every per-pixel logarithm from a 32768-entry table the
  CPU extractor fills with the host math library. The HIP twin evaluated
  `log2f()` on the device instead, which is one ulp from glibc's for about
  half of the arguments and rounded ties to even where the CPU rounds them
  away from zero: 77 entries were one lower, and on a gfx1036 only 49 of 440
  scores (four scales, 110 frames from 480x270 to 3840x2160) were the CPU's,
  the others up to 5.4e-7 away. The twin now uploads the CPU's table and
  looks every logarithm up; all 440 scores are identical at `--precision max`,
  and so are the numerator and denominator sums of `debug=true`, 12- and
  16-bit and 4:2:2 input, `vif_enhn_gain_limit=1.0` and `vif_skip_scale0`.
  The table has one definition, `vif_log2_table_generate()` in
  `integer_vif.h`. The parity gate compares the CPU and HIP `vif` cells with
  tolerance 0. Stored `vif_hip` scores change by up to 5.4e-7
  ([ADR-1435](docs/adr/1435-hip-vif-cpu-log2-table.md),
  [HIP backend](docs/backends/hip/overview.md#vif_hip-returns-the-cpus-scores-bit-for-bit-2026-10-01)).


- **`vmaf` no longer hangs after an out-of-memory on the device.**
  `vmaf_read_pictures()` kept the pair of pictures it was given when it failed
  before it reached an extractor (a non-increasing index, pictures that
  disagree with the stream, the picture pool, the CUDA ring buffer, the CUDA
  translation). Pictures from the CLI's pool then never came back, and
  `vmaf_close()` waited for them forever: with the device's memory taken by
  another process `vmaf --backend cuda` printed `problem reading pictures` and
  never exited, holding whatever lock the caller held. The call now owns both
  pictures on every return, as the failures after that point already did, so
  the CLI exits with `-ENOMEM` (status 244) at once. A caller that unref'd the
  pictures after an error, as `docs/api` used to say, must stop doing so
  ([ADR-1431](docs/adr/1431-read-pictures-owns-pictures-on-every-return.md),
  [API guide](docs/api/index.md#ownership-and-lifetime), Netflix/vmaf#1420,
  where the fork returns the error instead of asserting).


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


- **`scripts/dev/speed_gpu_parity.py` accepts a relative `--vmaf` path.** The
  script refused every relative path, including its own default
  `build/tools/vmaf`, with `allowlisted executable must be bare or absolute`
  and exit status 2, so the commands in the guides and in `docs/state.md` did
  not run as written. A relative path is now taken from the working directory;
  an absolute path and a bare name on `PATH` work as before
  ([SpEED](docs/metrics/speed_qa.md#checking-a-gpu-twin-against-the-cpu)).


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


- **`vmaf_read_json_model_collection` rejects sub-model name truncation with `-EINVAL`.**
  Port of the sub-model name truncation check from upstream Netflix/vmaf commit
  `15f1447c6` ([Netflix/vmaf#1428](https://github.com/Netflix/vmaf/pull/1428)).
  In both `core/src/read_json_model.cpp` and `core/src/read_json_model.c`,
  the return value of `snprintf` when formatting generated sub-model names
  `"%s_%04u"` was ignored via `(void)snprintf`. When a model collection
  reaches index 9999, `++i` increments to 10000 (5 digits), exceeding
  `cfg_name_sz` (`strlen(name) + 5 + 1`) and truncating the sub-model name.
  Both the C++23 parser and its C twin now validate `n < 0 || (size_t)n >= cfg_name_sz`,
  tear down any allocated model and collection objects without leaking, and
  return `-EINVAL`.


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


- **`ciede_sycl` follows the CPU `ciede` to 1.4e-11.** The CPU extractor
  computes CIEDE2000 in `double` and stores in `float`. The SYCL twin
  computed in `float` throughout, used the device's `float` math functions
  and a rewritten form of one branch, and was up to 1.14e-5 from the CPU. A
  SYCL kernel has no `double`, so the kernel now runs the CPU's statements
  with every `double` as a pair of `float` values and every math-library
  call as a function on such pairs, and the host adds the per-pixel values
  in the CPU's order
  ([ADR-1436](docs/adr/1436-sycl-ciede-cpu-arithmetic.md)). Measured on an
  Arc A380 at `--precision max`: the Netflix 576x324 pair identical to
  `--backend cpu` on 47 of 48 frames, its 10-, 12- and 16-bit versions and
  both 1080p checkerboard pairs on every frame, 200 frames of BBB 3840x2160
  within 1.4e-11. These are the CUDA twin's figures. It is not
  bit-identical: the C library's `powf` is not correctly rounded and 18 to
  64 of the 8.3 million pixels of a 3840x2160 frame round the other way. The parity
  gate compares the twin at `1e-9` instead of `5e-3`. A 3840x2160 frame
  takes 50.3 ms on an Arc A380 instead of 16.2 ms, and the twin holds 33 MB
  more on the device and on the host at that size. Stored `ciede_sycl`
  scores change by up to 1.14e-5.


- **Editing a SYCL header now rebuilds the kernels that include it.** The
  build compiled each SYCL source through a custom target that tracked only
  the source file. After a change to a header such as
  `core/src/feature/sycl/sycl_exact_fp.h`, `ninja` reported nothing to do and
  the library kept the kernels built from the old text; only a clean build
  picked the change up. The targets now record the headers each source
  includes (compiler depfiles, as the CUDA and HIP kernels have had since
  ADR-1320). Builds from a clean tree, such as CI and the release container,
  were not affected. Not yet on Windows, which builds from clean.


- **`float_adm_sycl` returns the CPU's scores bit for bit.** The SYCL float
  ADM twin was up to 1.7e-5 from the CPU extractor: it associated the angle
  test's threshold differently, used `float` where the CPU uses `double` for the
  enhancement gain and two constants, added the masking threshold and the
  frame sums in another order, and floored the frame sums at `1e-2` where the
  CPU uses `1e-10`. The kernels now run the CPU's arithmetic operation for
  operation; a SYCL kernel has no `double`, so the three `double` expressions
  are evaluated as exact pairs of `float` values, with the CPU's operations
  replayed in 64-bit integers next to a rounding boundary
  ([ADR-1434](docs/adr/1434-sycl-float-adm-cpu-arithmetic.md)). Measured on
  an Arc A380 at `--precision max`, every output of every frame equals the
  CPU extractor of the same build on the Netflix 576x324 pair at 8, 10, 12
  and 16 bits, both 1080p checkerboard pairs and 200 frames of BBB
  3840x2160, with `debug=true`. A 3840x2160 frame takes 12.3 ms instead of
  15.1 ms; the twin uses 48 MB more device memory there. On near-flat
  content scored with `adm_noise_weight=0` the twin reported `adm2 = 1`
  where the CPU reports 0; it now reports the CPU's value. The twin also
  takes the CPU options it rejected: `adm_skip_scale0`, `adm_skip_aim_scale`,
  `adm_f1s0..3` and `adm_f2s0..3`. `adm_p_norm` other than 1 or 3 stays
  within 1.8e-7 of the CPU. Stored `float_adm_sycl` scores change by up to
  1.7e-5.


- **`float_adm_sycl` works on Arc A-series GPUs under the xe driver.** Its two
  contrast-masking kernels indexed a small private array with the band number,
  which the Intel graphics compiler keeps in scratch memory, and kernels that
  use scratch memory return wrong values on these GPUs under xe (ADR-1395). On
  an Arc A380 `--backend sycl --feature float_adm` returned NaN and stopped
  with `problem reading pictures`. The kernels now pick the band by value.
  The twin is within 2.5e-6 of the CPU on the Netflix 576x324 pair, 3.5e-7 on
  1080p checkerboards and 1.3e-5 at 3840x2160, as on other devices. This was
  the last entry of the scratch ratchet list: no libvmaf SYCL kernel uses
  scratch memory any more (110 kernels audited on the A380), the list stays
  empty, and the start-up warning on an affected device now says that no
  extractor's scores are affected.


- **`float_moment_sycl` is bit-identical to the CPU `float_moment` extractor
  at 16 bits.** The CPU forms each sample's square in `float` before adding
  it, which at 16 bits is the square rounded to 24 bits; the SYCL twin added
  exact integer squares. Its second moments (`float_moment_ref2nd`,
  `float_moment_dis2nd`) were up to 1.0e-4 from the CPU's on 16-bit content
  with real low bits (0 of 13 such frames identical on an Arc A380), while
  8-, 10- and 12-bit input and the first moments were identical. The kernel
  now adds the CPU's float square, and all four outputs are identical on 288
  of 288 measured frames at `--precision max`. The parity gate compares the
  CPU and SYCL `float_moment` cells with tolerance 0. One range stays within
  a derived bound instead: on a 16-bit frame of more than 2 097 152 pixels
  whose sum of squares passes 2^53 the CPU's own sum rounds as it goes (2.7e-7
  measured at 2560x1440). No measurable cost. Stored 16-bit
  `float_moment_sycl` second moments change by up to 1.0e-4
  ([ADR-1449](docs/adr/1449-sycl-float-moment-cpu-float-squares.md),
  [SYCL backend](docs/backends/sycl/overview.md#float_moment_sycl-matches-the-cpu-float_moment-at-16-bits-2026-10-02)).


- **`float_motion_sycl` returns the CPU's scores bit for bit.** The SYCL twin
  summed the absolute differences of each 32x4 work-group on the device and
  the groups in `double` on the host, where the CPU `float_motion` extractor
  keeps one `float` running sum per row and one over the rows. Measured on an
  Arc A380 that left `motion` and `motion2` up to 1.36e-4 from the CPU on
  1920x1080 checkerboards (above the 5e-5 cross-backend tolerance), 2.4e-5 at
  3840x2160 and 3.1e-6 on the Netflix 576x324 pair. The twin now adds each
  row on the device in the CPU's order, one work-item per row, and the host
  adds the rows
  ([ADR-1411](docs/adr/1411-sycl-float-motion-cpu-float-sum.md), after
  ADR-1409 for CUDA). At `--precision max` every frame is identical on the
  Netflix pair, both 1080p checkerboard pairs and 200 frames of BBB
  3840x2160, also at 10, 12 and 16 bits and with `motion_fps_weight` and
  `motion_max_val` set. The second pass over the blurred planes costs 0.38 ms
  per 3840x2160 frame on the A380 (3.85 to 4.23 ms). The parity gate compares
  this twin with tolerance 0. Stored `float_motion_sycl` outputs change in
  their low digits by at most those differences. The HIP and Metal twins
  still agree with the CPU to four decimal places.


- **`float_motion_sycl` honours `motion_force_zero` and weights its debug
  score.** The twin declared `motion_force_zero` but ignored it and emitted
  real motion scores; it now emits zeros, like the CPU `float_motion`. Its
  debug `VMAF_feature_motion_score` now carries `motion_fps_weight`, as on the
  CPU, instead of the unweighted SAD. The SSIM page no longer documents an
  `enable_chroma` option and `_cb` / `_cr` outputs that the `ssim` extractor
  does not have; it lists `enable_db` and `clip_db`
  ([SSIM](docs/metrics/ssim.md#options)).


- **`float_ms_ssim_sycl` computes the CPU's arithmetic.** The SYCL twin
  added the decimate taps in two roundings where the CPU fuses them, kept the
  Gaussian window sums and the luminance, contrast and structure terms in
  fp32 where the CPU uses `double`, and combined unrounded per-scale means.
  Measured on an Arc A380 it matched the CPU on none of 104 frames (6.9e-8 on
  the Netflix 576x324 pair, up to 2.98e-6 on 1080p checkerboards, 1.23e-6 at
  3840x2160). It now follows the reference operation for operation, with the
  CPU's `double` values carried as exact pairs of floats and the frame sums
  in 64-bit fixed point
  ([ADR-1414](docs/adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md), after
  ADR-1403 for CUDA). Every per-scale mean of every measured frame equals
  the CPU's, `enable_lcs` and `enable_chroma` outputs included; the score
  equals a GCC build's on 253 of 254 frames, the other differing by 1.1e-16
  through the host `pow()` of an Intel-compiler build. The parity gate
  compares the twin with tolerance 0. A 3840x2160 frame takes 42.6 ms on the
  A380 against 31.4 before. Stored `float_ms_ssim_sycl` outputs change in
  their low digits by at most the differences above. The HIP and Metal twins
  keep the old arithmetic.


- **`float_psnr_sycl` is bit-identical to the CPU `float_psnr` extractor at
  every bit depth.** The CPU squares each sample difference in `float` and
  adds the squares in `double`, which does not round. The SYCL twin added
  each 16x16 work-group in single precision: exact at 8 bits, and at 10, 12
  and 16 bits only while the differences in a group are small. On an Arc A380
  it matched the CPU on every frame of real clips and was up to 7.4e-8 dB off
  on high-bit-depth input with large differences (0 of 19 such frames
  identical). The kernel now adds the squares as integers, and 288 of 288
  measured frames are identical at `--precision max`, with `uncapped=true`
  too. The parity gate compares the CPU and SYCL `float_psnr` cells with
  tolerance 0. A 3840x2160 frame takes 3.41 ms instead of 3.31. Stored
  `float_psnr_sycl` scores of such input change by up to 7.4e-8 dB
  ([ADR-1450](docs/adr/1450-sycl-float-psnr-exact-block-sums.md),
  [PSNR](docs/metrics/psnr.md#float_psnr)).


- **`float_ssim_sycl` combined formula residual eliminated.**
  Arithmetic alignment from PR #1645 (`core/src/feature/sycl/integer_ssim_sycl.cpp`,
  evaluating exact per-pixel $l \cdot c \cdot s$ in fp32 pairs, fixed-point work-group
  sums, and double host reduction) eliminated the residual against the CPU reference.
  Measured on an Intel Arc A380 under the Linux `xe` driver: max absolute difference
  against `--backend cpu` is 0.000e+00 across all 48 frames of Netflix 576x324 and
  all frames of BBB 3840x2160 at both auto scale and explicit `scale=1`.
  `test_sycl_twin_option_parity` passes 13/13 with exact match on flat identical frames.


- **`float_ssim_sycl` with `enable_db` no longer reports tens of dB below the
  CPU on near-identical frames.** The CPU rounds each frame's SSIM mean to fp32
  before converting it to dB, so a frame within half an fp32 step of 1 scores
  exactly 1 and reports `+inf` or the `clip_db` ceiling; the twin kept the
  double mean and reported a finite value (93.6 dB against the CPU's 121 dB on
  the first frames of a 4K pair). The twin now rounds the `float_ssim` and
  `float_ssim_l/c/s` means the same way; linear scores move by less than 6e-8.


- **`float_vif_sycl` returns the CPU's scores bit for bit.** The SYCL twin
  filtered with a table of Gaussian taps the CPU extractor stopped using
  (it derives them at start-up with `vif_get_filter()`), called the device
  `log2` where the CPU evaluates a polynomial, took `vif_sigma_nsq` as a
  `float` where the CPU keeps it in `double`, and reduced per sub-group and
  per block where the CPU keeps one `float` running sum per row and one over
  the rows. Measured on an Arc A380 that left no frame of the Netflix
  576x324 pair identical, up to 3.8e-5 away, and 7.0e-6 at 3840x2160. The
  twin now takes the CPU's taps, evaluates the CPU's per-pixel statistic
  without a 64-bit floating-point type (pairs of floats, and the CPU's
  `double` operations replayed in integers next to a rounding boundary) and
  adds in the CPU's order
  ([ADR-1422](docs/adr/1422-sycl-float-vif-cpu-arithmetic.md), after
  ADR-1412 for CUDA). At `--precision max` every output of every frame is
  identical on the Netflix pair at 8, 10, 12 and 16 bits, both 1080p
  checkerboard pairs and 200 frames of BBB 3840x2160, with `debug=true` and
  with non-default options. `float_vif_sycl` also accepts the CPU's
  `vif_scale1_min_val`, `vif_scale2_min_val` and `vif_scale3_min_val` now. A
  3840x2160 frame takes 23.95 ms on the A380, 20.54 ms before, and 100 MB
  more device memory. The parity gate compares this twin with tolerance 0.
  Stored `float_vif_sycl` outputs change by up to 3.8e-5. The HIP and Metal
  twins still agree with the CPU to four decimal places.


- **SYCL and HIP `psnr_hvs` return the CPU extractor's scores bit for bit**
  (`T-SYCL-PSNR-HVS-EXACT-SUM-2026-10-01`, `T-HIP-PSNR-HVS-EXACT-SUM-2026-10-01`,
  [ADR-1401](docs/adr/1401-psnr-hvs-sycl-hip-exact-twins.md)). Like the CUDA
  twin (ADR-1397), `psnr_hvs_sycl` and `psnr_hvs_hip` now store the 64 terms of
  every block in the CPU's arithmetic and the host adds them in the CPU's order.
  Before, they summed each block on the device and were up to 1.7e-2 dB from
  `--backend cpu` at 3840x2160, beyond the parity tolerance. `psnr_hvs`,
  `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr` are identical to the CPU at
  `--precision max` from 576x324 to 3840x2160 and at 8 to 12 bits, measured on
  an Arc A380 and a gfx1036, and the parity gate compares every `psnr_hvs` cell
  with tolerance 0. The SYCL kernel has no fp64: it takes the CPU's `double`
  masking threshold from a new integer square root of the exact product
  (`sqrt_prod_rn()` in `sycl_exact_fp.h`), and it stays free of scratch memory.
  The scores of both twins therefore change in their last digits (by up to
  1.7e-2 dB at 3840x2160). Both are slower for it: a 3840x2160 frame takes
  35.9 ms instead of 22.4 ms on an Arc A380 and about 38 ms instead of 18 ms on
  a gfx1036, and the term buffer needs 65 MB per 3840x2160 frame; tuning is
  tracked as `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. Compare a
  twin with the CPU extractor of the same `vmaf` binary: the dB value uses the
  host's `log10`, which differs by one unit in the last place between an `icx`
  and a gcc build. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#agreement-with-the-cpu-extractor).


- **SYCL and HIP `psnr_hvs` score 4:0:0 input, and the HIP twin takes
  `enable_chroma`** (`T-SYCL-HIP-PSNR-HVS-YUV400-REFUSED-2026-10-01`).
  `psnr_hvs_sycl` and `psnr_hvs_hip` refused 4:0:0 pictures at `init()`
  (`YUV400P unsupported`), where the CPU extractor and `psnr_hvs_cuda` score the
  luma plane and emit `psnr_hvs_y` and `psnr_hvs`. Both twins now do the same.
  `psnr_hvs_hip` also gains the CPU extractor's `enable_chroma` option (default
  `true`): with `false` it uploads, dispatches and scores luma only, like the
  CUDA and SYCL twins. Scores for 4:0:0 and for `enable_chroma=false` are
  identical to the CPU's at `--precision max`, measured on an Arc A380, a
  gfx1036 and an RTX 4090. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#sample-conversion).


- **SYCL: kernels stay out of scratch memory, and a wrong-result driver is
  reported.** On an Arc A380 under the Linux xe driver, a SYCL kernel that keeps
  a private array in memory or spills registers returns wrong values, with no
  error; 25 of the 109 SYCL kernels did. `vif_sycl`'s SIMD-32 kernels now use the
  256-entry register file and no longer spill: forced with the new
  `VMAF_SYCL_VIF_SUBGROUP_SIZE=32`, they scored every frame 0/0 on that device
  and now match the SIMD-16 kernels bit for bit. The first SYCL initialisation on
  each device runs two probe kernels and logs a warning naming the SYCL
  extractors that still use scratch memory when the probes come back wrong
  (`VMAF_SYCL_SCRATCH_SELFTEST=0` skips it); the device is used either way. The
  new `test_sycl_kernel_scratch` fails when a kernel outside
  `core/src/sycl/scratch_ratchet.txt` uses scratch memory. See
  [Scratch memory on Intel GPUs](docs/backends/sycl/overview.md#scratch-memory-on-intel-gpus-adr-1395)
  (ADR-1395, `T-SYCL-XE-SCRATCH-WRONG-RESULTS-2026-10-01`).


- **`motion_sycl` sizes chroma planes correctly for 4:2:2 and 4:4:4 input.**
  With `motion_add_uv=true`, `motion_configure_chroma()` previously assumed
  4:2:0 subsampling (`chroma_w = (w + 1) >> 1`, `chroma_h = (h + 1) >> 1`)
  for all input formats, causing `motion_stage_chroma()` on 4:2:2 and 4:4:4
  input to stage only a sub-rectangle of each chroma plane and normalize the
  SAD by an incorrect area. Chroma dimensions are now derived via
  `vmaf_chroma_extent()` from `picture_geometry.h` according to the pixel
  format, and `test_sycl_motion_add_uv_parity` verifies parity with the
  fixed-point oracle across 4:2:0, 4:2:2, and 4:4:4.


- **SYCL: `motion_sycl` and `motion_v2_sycl` kernels above 15 bpc are scratch-free on Intel Arc under xe (ADR-1395).**
  On the Linux `xe` driver on Intel Arc A380, scratch memory (private arrays and register spills) produces corrupted reads and writes. The 16-bit vertical filtering pipeline (`submit_sad<int64_t>`) spilled 768 B/thread at SIMD-32 due to the 128-register limit, causing `test_sycl_motion_tiny_frames` to fail on 16-bit frames. Implementing `MotionSadHbdKernel` derived from `VmafSyclKernelShape<32, 256>` (`sycl_compat.h`) requests the 256-entry register file, eliminating all spills and private memory (`spill_size: 0`, `private_size: 0`). On physical Arc A380 under `xe`, `test_sycl_motion_tiny_frames` passes (8, 10, and 16-bit across all 9 geometries, bit-exact vs scalar CPU), `test_sycl_motion3_parity`, `test_sycl_motion_add_uv_parity`, and `test_sycl_motion_v2_parity` pass, and 50 frames of 16-bit 4K BBB match the CPU reference bit-for-bit (`T-SYCL-MOTION-HBD-XE-SCRATCH-2026-10-01`).


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


- **SYCL**: Fixed identical/flat-frame handling in `float_ssim_sycl` and `integer_ssim_sycl` by implementing the CPU's exact arithmetic without identical-window shortcuts, grouping integer terms as `((w*a)*b)/den`, and preserving ADR-1370 fp32 frame-mean rounding.
- **SYCL**: Fixed a bug where `psnr_sycl` produced incorrectly scaled scores under `--subsample` by adding the missing `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag.
- **SYCL**: Fixed a bug where `motion_v2_sycl` diverged from the CPU by applying `motion_fps_weight` and the `motion_max_val` cap in `collect()` and emitting scores for one-frame inputs in `flush()`.


- **Shared SYCL frame buffers re-allocate when geometry changes.** When a single
  `VmafSyclState` was shared across consecutive `VmafContext` instances of different
  frame dimensions or bit depths, `vmaf_sycl_shared_frame_init()` kept the old
  buffer allocations and pitch, resulting in out-of-bounds reads and incorrect
  metric scores. Re-initialization now drains queues and reallocates the shared
  frame and chroma buffers to match the new geometry.


- **`speed_chroma_sycl` and `speed_temporal_sycl` match the CPU with
  `speed_prescale_method=lanczos4`.** Like the CUDA twins before them, they
  evaluated the lanczos4 kernel weights on the device in fp32, where the CPU
  scaler uses fp64 `sin()`, and SpEED amplifies the few-ulp differences on
  smooth content: on an Arc A380 no frame of a 1920x1080 gradient matched at
  `speed_prescale=2.0`. The scale kernel now reads the weights from the table
  the host builds with the CPU scaler's own routine, and nearest, bilinear,
  bicubic and lanczos4 prescale at 0.5 and 2.0 are bit-identical to the CPU
  extractor on that device. The kernels use no scratch memory, so the result
  also holds under the Linux xe driver, and the xe scratch warning no longer
  names the two SpEED twins. Scores with the other three methods, and without
  prescale, are unchanged. The HIP twins still evaluate the weights on the
  device ([SpEED](docs/metrics/speed_qa.md#sycl-device-resident-and-bit-identical-to-the-cpu)).


- **SpEED SYCL kernels are now scratch-free on Intel Arc GPUs (ADR-1395).**
  On the Intel Arc A380 under the Linux `xe` driver, kernel execution that used
  scratch memory (private memory arrays or register spills) caused silent data
  corruption, resulting in singular covariance matrices and zeroed SpEED scores.
  By replacing dynamically-indexed captured plane arrays in `RawPlanes` with
  scalar plane members resolved once per work-item (`RawBound` / `FloatBound`)
  and unrolling bicubic/lanczos weighting loops, all eight SpEED `launch_scale`
  and `launch_decimate` kernels now compile with zero private memory and zero
  register spill (`private_mem_size == 0`, `spill_memory_size == 0`).
  All SpEED parity tests pass, and `speed_gpu_parity.py` achieves bit-identical
  scores (`0.000e+00` max absolute difference) against the CPU reference on
  both 576x324 and 3840x2160 fixtures.


- **`integer_ssim_sycl` returns the CPU's `ssim` bit for bit, and works on
  16-bit input.** The CPU `ssim` extractor computes each pixel's term in
  `double` and adds all terms in one running sum. A SYCL kernel has no
  `double`, and the twin used `float` and added per block, which left the
  score up to 3.1e-7 from the CPU (2.0e-4 with `enable_db`) and made 16-bit
  frames fail with `invalid ratio`. The kernel now performs the CPU's
  `double` operations in 64-bit integers and the host adds the terms in the
  CPU's order ([ADR-1443](docs/adr/1443-sycl-ssim-cpu-arithmetic.md)).
  Measured on an Arc A380 at `--precision max`, every frame is identical on
  the Netflix 576x324 pair at 8, 10, 12 and 16 bits, both 1080p checkerboard
  pairs and 200 frames of BBB 3840x2160, with `enable_db` and `clip_db` too.
  A 3840x2160 frame takes 31.9 ms instead of 17.8 ms (0.78 instead of 0.45 ms
  at 576x324), and the twin holds 66 MB more pinned host memory at that size.
  The parity gate compares this twin with tolerance 0. Stored
  `integer_ssim_sycl` scores change by up to 3.1e-7.


- **`ssimulacra2_sycl` is bit-identical to the CPU `ssimulacra2` extractor.**
  The CPU evaluates six terms per pixel and channel in `double` and adds each
  into one `double`, pixel after pixel. A SYCL device has no `double`, so the
  twin evaluated the terms as pairs of floats and added them in a fixed tree:
  on an Arc A380 none of 266 measured frames (576x324 to 3840x2160, 8 to 16
  bits) equalled the CPU, and the score was up to 7.6e-11 away. The twin now
  computes each term's `double` in 64-bit integers, the CPU's operations one
  for one, and forms the sums with the bits of the CPU's loops, from integer
  increments per binade as the CUDA and HIP twins do (`ordered_sum.h`). All
  266 frames are identical at `--precision max`, with every `yuv_matrix`. The
  parity gate compares the CPU and SYCL `ssimulacra2` cells with tolerance 0
  instead of 5e-3, and the Arc A380's 5e-2 calibration for this feature is
  removed. The twin is slower: 195 ms instead of 84 per 3840x2160 frame and
  13.5 ms instead of 5.3 per 576x324 frame on that device. Stored
  `ssimulacra2_sycl` scores change by up to 7.6e-11
  ([ADR-1446](docs/adr/1446-sycl-ssimulacra2-cpu-bits.md),
  [ssimulacra2](docs/metrics/ssimulacra2.md#sycl-device-resident-one-readback-per-frame)).


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


- **The SYCL clang-tidy lane measures the SYCL sources again.** Since the
  SYCL build targets record their headers (PR #1764), the script that adds
  the SYCL compile commands to the lint database found 4 of 31 translation
  units: Ninja names a rule with a depfile differently and the script matched
  the old name only. `make tidy-ratchet LANE=sycl` and the changed-file SYCL
  lint job therefore saw none of the kernels. The script reads both rule
  forms, and it now stops with an error when a SYCL source is compiled by a
  command it cannot read, so the lane cannot go blind silently again.


- **`vif_sycl` rounds its per-scale sums where the CPU does, and emits the
  CPU's default outputs.** The CPU `vif` extractor stores each scale's
  numerator and denominator sum in a `float` and divides in single precision.
  The SYCL twin kept the sums in `double`, so every score of every frame was
  up to 3.5e-7 from the CPU. It now rounds at the same points: measured on an
  Arc A380 at `--precision max`, the denominator sums are identical on every
  frame, and the scores on 12 to 41 of the 48 Netflix 576x324 frames
  (depending on the scale) and on 140 to 196 of 200 BBB 3840x2160 frames,
  where none was before. The remaining frames are one or a few `float` steps
  off in a numerator (at most 3.6e-7 in a score), because the kernel computes
  the per-pixel gain in `float`. The twin's `debug` option now defaults to
  `false`, as on the CPU; it defaulted to `true` and added eleven debug
  outputs to every run. Request them with `--feature vif_sycl=debug=true`.
  Stored `vif_sycl` scores change by up to 3.5e-7.


- **`vif_sycl` returns the CPU's scores bit for bit.** The CPU `vif` extractor
  computes a pixel's gain in `double` and truncates two results to integers
  before its log2 table. A SYCL kernel has no `double`, and the twin used
  `float`, which put a share of those integers one off and left a scale's
  score up to 3.6e-7 from the CPU on some frames. The kernel now computes
  both integers exactly: one integer division decides them, and a pixel whose
  value lies within the `double` chain's own rounding error of an integer
  (one in 300 000) replays the CPU's operations in 64-bit integers
  ([ADR-1432](docs/adr/1432-sycl-integer-vif-exact-gain.md)). Measured on an
  Arc A380 at `--precision max`, every output of every frame is identical on
  the Netflix 576x324 pair at 8, 10, 12 and 16 bits, both 1080p checkerboard
  pairs and 200 frames of BBB 3840x2160, with `debug=true`, with
  `vif_enhn_gain_limit` of 1.0, 1.2 and 37.5, and for a clip scored against
  itself. A 3840x2160 frame takes 0.75 ms longer (21.46 to 22.21 ms). The
  parity gate compares this twin with tolerance 0. Stored `vif_sycl` scores
  change by up to 3.6e-7.


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


- **The CPU extractors of an Intel-compiler build match a GCC build where
  they use SIMD.** The two general x86 SIMD libraries were built without
  `-ffp-contract=off`, so `icx` (which every SYCL build uses) turned plain-C
  arithmetic in the tails of SIMD kernels into fused multiply-adds. In
  `ssim_avx512.c` that moved a score: on an AVX-512 host the CPU
  `float_ms_ssim` of an icx build differed from a GCC build and from its own
  scalar path by one fp32 unit in a per-scale mean, 7.7e-9 to 1.4e-8 in the
  score, on 4 of 104 frames (Netflix 576x324 pair, 1080p checkerboards, BBB
  3840x2160). `adm`, `vif` and `speed` files were contracted too, with no
  score difference measured. Every x86 SIMD library is now built with the
  strict floating-point flags
  ([ADR-1415](docs/adr/1415-x86-simd-libraries-strict-fp.md)). GCC builds
  are unchanged: the disassembly of all 28 objects is identical with and
  without the flag. `test_ssim_x86_simd` compares the AVX2 and AVX-512 SSIM
  kernels with the scalar reference bit for bit, and `test_integer_adm_simd`,
  which failed on icx builds since #1700 because its own translation unit
  carried no FP flag, passes again. An icx build still differs from a GCC
  build in `psnr`, `psnr_hvs`, `ciede` and `speed_chroma` (at most 7.1e-15,
  7.1e-15, 5.7e-12 and 1.2e-6) because it calls Intel's math library.

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
