<!-- markdownlint-disable MD060 -->
# Cross-backend GPU-parity gate

`scripts/ci/cross_backend_parity_gate.py` runs the same clip through the CPU
and one or more GPU backends (CUDA, SYCL, HIP, Metal), compares every
per-frame metric, and says whether each backend returns the CPU's numbers
within the allowed tolerance. It writes a JSON file and a Markdown summary.

In CI the gate runs in one lane only: the `SYCL Parity (Arc A380)` job in
[`.github/workflows/sycl-parity.yml`](../../.github/workflows/sycl-parity.yml),
which compares CPU and SYCL `float_ssim` on the self-hosted Arc runner. For
every other backend and feature you run it yourself, as
[Run it locally](#run-it-locally) shows. Vulkan and its hosted lavapipe lane
were removed by [ADR-0726](../adr/0726-drop-vulkan-backend.md).

## For users: what "exact" means

A GPU twin is *exact* when it returns the same bits as the CPU extractor for
the same input. The gate compares an exact cell with tolerance `0`, at
`--precision max` (`%.17g`) so the default `%.6f` rounding cannot hide a
last-bit difference. Every other cell passes when each per-frame value is
within an absolute tolerance, `5e-5` by default (four decimal places).

The [generated table of exact twins](cross-backend-exact-twins.md) lists every
(feature, backend) pair that is exact today, with the ADR and the measurement
behind it. Netflix golden assertions remain untouched; parity thresholds never
replace the CPU golden-data gate.

To check your own GPU build against the CPU:

1. Build `vmaf` with the backend, or use the dev container
   ([Run it locally](#run-it-locally)).
2. Run the gate on a clip pair and read `OK`, `FAIL`, `ERROR` or `SKIP` per
   cell ([Read the output](#read-the-output)):

    ```bash
    python3 scripts/ci/cross_backend_parity_gate.py \
        --vmaf-binary build/tools/vmaf \
        --reference testdata/ref_576x324_48f.yuv \
        --distorted testdata/dis_576x324_48f.yuv \
        --width 576 --height 324 \
        --backends cpu cuda --features float_ssim vif
    ```

## Backends

| Backend | Extractor suffix | Device flag passed to `vmaf` | Gate option | Default device |
| --- | --- | --- | --- | --- |
| `cpu` | none | none | | |
| `cuda` | `_cuda` | `--gpumask` | `--cuda-device` | `1` (first GPU) |
| `sycl` | `_sycl` | `--sycl_device` | `--sycl-device` | `0` |
| `hip` | `_hip` | `--hip_device` | `--hip-device` | `0` |
| `metal` | `_metal` | `--metal_device` | `--metal-device` | `0` |

The command line default is `--backends cpu cuda`. No CI job runs the HIP or
Metal cells; HIP is for a local run on an AMD host, and Metal runs from the
macOS tester bundle ([Metal](#metal)).

Extractors whose twin is not named `<feature>_<backend>` are listed in
`BACKEND_EXTRACTOR_ALIASES`:

- `float_ms_ssim` is `integer_ms_ssim_hip` on HIP;
- `ssim` is `integer_ssim_cuda`, `integer_ssim_sycl`, `integer_ssim_hip` and
  `integer_ssim_metal`;
- the Metal twins of the fixed-point extractors carry the CPU file's name
  (`integer_adm_metal`, `integer_motion_metal`, and so on).

## Command-line options

| Option | Default | Meaning |
| --- | --- | --- |
| `--vmaf-binary` | required | Path to the `vmaf` binary under test. |
| `--reference`, `--distorted` | required | Reference and distorted YUV files. |
| `--width`, `--height` | required | Frame size in pixels. |
| `--pixel-format` | `420` | Chroma subsampling of the YUV files. |
| `--bitdepth` | `8` | Bit depth of the YUV files. |
| `--features` | every feature in `FEATURE_METRICS` | Features (cells) to run. |
| `--backends` | `cpu cuda` | Backends to pair; `cpu` is the reference. |
| `--hold-exact` | none | Compare every cell of these backends exactly, although no fragment lists them ([Metal](#metal)). |
| `--fp16-features` | none | Features that use the FP16 contract, absolute tolerance `1e-2`. |
| `--cuda-device`, `--sycl-device`, `--hip-device`, `--metal-device` | see [Backends](#backends) | Device index per backend. |
| `--gpu-id` | none | Runtime GPU identifier (for example `cuda:8.6`, `sycl:0`); picks the most specific matching row in the calibration table. With no match, the feature's built-in tolerance stays authoritative. |
| `--calibration-table` | `scripts/ci/gpu_ulp_calibration.yaml` | Per-device calibration rows ([ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md)). |
| `--workdir` | `<tmp>/vmaf_parity_gate` | Scratch directory for per-run output. |
| `--json-out`, `--md-out` | none | Write the machine-readable and the Markdown summary. |

## Tolerance contract

The default is `5e-5` (places=4), the fork's GPU-versus-CPU contract from
ADR-0125, ADR-0138 and ADR-0140. Feature-specific relaxations live in
`FEATURE_TOLERANCE` inside
[`cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py).

| Cell | Tolerance | Source |
| --- | ---: | --- |
| Any feature, both sides `cpu` or a [listed exact twin](#exact-twins) | `0` (bit-identical, at `--precision max`) | The ADR in the twin's fragment under `scripts/ci/exact_twins.d/`. |
| `ciede`, every pair of CPU, CUDA, SYCL and HIP | `1e-9` at `--precision max` | ADR-1426, ADR-1436, ADR-1448; see [math-library twins](#twins-that-differ-only-in-their-math-library). |
| `vif`, `adm`, `psnr` (planes `psnr_y`, `psnr_cb`, `psnr_cr`), `float_moment`, `cambi`, `motion` family | `5e-5` | ADR-0125, ADR-0138, ADR-0140, ADR-0360. |
| `ssim` (fixed-point extractor) | `5e-5` | ADR-0564 (int64 moments, one double term per pixel). |
| `float_ssim`, `float_ssim_lcs`, `float_ms_ssim`, `float_ms_ssim_lcs`, `float_ms_ssim_chroma`, `float_psnr`, `float_motion`, `float_vif`, `float_adm` | `5e-5` | ADR-0188, ADR-0192, ADR-0215, ADR-1382. |
| `speed_chroma` (the three scores `speed_chroma_u`, `_v`, `_uv`), `speed_temporal` (the one score `speed_temporal`), for a twin that is not exact | `5e-5` | Places=4; the CUDA, HIP and SYCL twins are exact ([ADR-1477](../adr/1477-speed-upstream-double-math.md)). |
| `psnr_hvs` (a twin that is not exact) | `5e-4` at 576x324 and below, $5 \times 10^{-4} \sqrt{N / N_{576 \times 324}}$ above | ADR-0191 (DCT plus per-block float reduction); ADR-1361 (area scaling). |
| `ciede` (a twin that is not in the `1e-9` row) | `5e-3` | ADR-0187 (per-pixel pow, sqrt, sin, atan2). |
| `ssimulacra2` | `5e-3` | ADR-0192 (XYB cube root plus IIR blur). |

### Notes on single cells

- **`motion_debug`** is `motion` with `debug=true` and adds `integer_motion`.
  Both cells compare `VMAF_integer_feature_motion_sad_score`, the per-frame
  score that `motion2` and `motion3` derive from, next to `integer_motion2`
  and `integer_motion3`
  ([ADR-1418](../adr/1418-motion-parity-gate-metric-alignment.md)).
- **`motion_mffw` and `motion_v2_mffw`** are `motion` and `motion_v2` with
  `motion_five_frame_window=true:motion_moving_average=true`, the option set of
  the `vmaf_v1.0.16_hfr_*` models. They compare the SAD score, `motion2` and
  `motion3` under their option-suffixed names
  ([ADR-1491](../adr/1491-gpu-motion-five-frame-window.md)).
- **`float_motion`** compares `motion`, `motion2` and `motion3`; `motion3` since
  2026-10-03, when the SYCL twin started to emit it as the CUDA and HIP twins
  do (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`).
- **`float_ssim_lcs`** runs `float_ssim` with `enable_lcs=true` and compares
  `float_ssim_l`, `float_ssim_c` and `float_ssim_s` next to the score, at the
  same `5e-5` ([ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)).
- **`float_ms_ssim_chroma`** runs `float_ms_ssim` with `enable_chroma=true`
  and compares `float_ms_ssim_cb` and `float_ms_ssim_cr` next to the score;
  CUDA, SYCL and HIP are listed exact. Every chroma plane must be at least 176
  pixels on a side, so on a fixture with smaller chroma, such as the 576x324
  4:2:0 Netflix pair (288x162), the cell is reported `SKIP` with that reason
  and nothing is run (`FEATURE_MIN_CHROMA_DIM`). Run it on a 1080p 4:2:0, a
  4:2:2 or a 4:4:4 pair:

    ```bash
    python3 scripts/ci/cross_backend_parity_gate.py \
        --vmaf-binary build/tools/vmaf \
        --reference python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
        --distorted python/test/resource/yuv/checkerboard_1920_1080_10_3_10_0.yuv \
        --width 1920 --height 1080 --backends cpu cuda \
        --features float_ms_ssim_chroma
    ```

- **Every registered twin is a gate cell.** A twin registered in
  `core/src/feature/feature_extractor.cpp` that is no gate feature's extractor
  is guarded by its own unit test only; `speed_temporal` was the one such twin
  on CUDA, SYCL and HIP.
  `core/test/test_parity_gate_covers_registered_twins.py` fails when a new twin
  of a gated backend has no gate feature, and when the tables of this gate and
  of `cross_backend_vif_diff.py` differ.

### Area-scaled `psnr_hvs`

!!! note "Currently unused by the gate"
    The CPU `psnr_hvs` adds every coefficient error of a plane into one
    `float`, so its rounding error, and the achievable CPU/GPU agreement,
    grows with the number of 8x8 blocks.

Both `cross_backend_parity_gate.py`
and `cross_backend_vif_diff.py` take the `psnr_hvs` tolerance from the
table or calibration row as the contract at 576x324 and multiply it by
sqrt(N / N_576x324) for larger fixtures, where N is the luma plane's term
count (64 per block, a block every 7 pixels). Frames of 576x324 or smaller
keep `5e-4`; 1920x1080 gets `1.67e-3` and 3840x2160 `3.34e-3`. The label in
the output shows the factor, for example `default+area x6.69`
([ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md) derives
it from the float-accumulation bound).

This is the contract of a twin that sums each block on the device. The
CUDA, SYCL and HIP twins no longer do (they are exact), and the Metal twin
does but has no listed `psnr_hvs` fragment, so no `psnr_hvs` cell of the
gate uses it today. It stays for a caller that names no backends and for a
twin added later.

## Exact twins

A GPU twin that returns the CPU extractor's bits for a feature is listed by
adding one file, `scripts/ci/exact_twins.d/<feature>.<backend>` (for example
`psnr_hvs.cuda`), and nothing else shared
([ADR-1428](../adr/1428-exact-twins-fragments.md)). The file holds two
`key: value` lines:

- `adr:` one or more `ADR-NNNN` that exist under `docs/adr/`, the ADR that
  establishes exactness;
- `evidence:` one line: fixtures and result.

`cross_backend_calibration.py` builds `EXACT_TWINS` from the directory at
import and rejects an unknown key, a missing key, an empty file, an unknown
feature or backend, and a missing or empty directory. The
[generated table of exact twins](cross-backend-exact-twins.md) is rendered from
the same files by `make docs-fragments-write`.

A listed CUDA, SYCL or HIP twin also needs a recorded row on the
[depth and layout matrix](exact-twin-matrix.md): every twin at 8, 10, 12 and 16
bits and in 4:2:0, 4:2:2 and 4:4:4, `==` against the CPU on generated fixtures.
`scripts/ci/exact_twin_matrix.py --record` writes the row, and
`test_exact_twin_matrix_contract` fails while a listed twin has none.

A cell whose two sides are the CPU extractor or a listed twin is compared with
tolerance `0`, at every frame size and ahead of any calibration row, and both
sides run with `--precision max` so that a last-bit difference is not rounded
away by the default `%.6f` output. The label in the output is `exact:ADR-1397`
for every listed twin. An explicit `--fp16-features <feature>` still selects the
FP16 contract. A cell with a backend that is not listed keeps the tolerance of
the [tolerance contract](#tolerance-contract).

### The rule for listing

Listing needs a measurement that shows bit-identity (`--precision max`, the
Netflix 576x324 pairs, the 1080p checkerboard pairs and `testdata/bbb` 4K) and
an ADR that records it. A listed twin that drifts is fixed; it is never given a
tolerance and never taken off the list to make a gate pass.

Identical on those fixtures is necessary, not sufficient
([ADR-1437](../adr/1437-hip-exact-twins-declared.md)). A twin is listed when it
also reaches the CPU's value by construction: integer sums on the device, the
CPU's own helpers on the host, or the CPU's arithmetic type for type.

Natural 8-bit content does not tell the two apart, and the repository's 10-,
12- and 16-bit Netflix clips are the 8-bit clip shifted left. Before listing a
twin, also run it on full-range noise at 8, 10, 12 and 16 bits: `float_psnr_hip`
and
`float_moment_hip` matched every real clip measured and differ there.

### What the comparison assumes

The equality holds between runs of one `vmaf` binary, which is how the gate runs
a cell. The dB value goes through the host's `log10`.

A binary built with
oneAPI `icx` before [ADR-1495](../adr/1495-icx-system-libm.md) used Intel's
`libimf` and a gcc build glibc, and the two rounded differently by one unit in
the last place on a few frames (3 of the 48 Netflix frames), for the CPU
extractor and the twins alike. An icx build now links glibc's `libm`
(`test_icx_system_libm` checks it), so a SYCL build's twins also equal a gcc
build's CPU extractor; a binary built with another C library can still differ
from either.

An exact cell runs one binary on both sides, so it also needs the CPU extractor
of that binary to be the reference arithmetic; for an icx build on an AVX-512
host that needs the x86 SIMD libraries built without FP contraction (#1706).

## Twins that differ only in their math library

`ciede_cuda` evaluates `ciede.c`'s expressions in its types and the host adds
the per-pixel values in the CPU's order
([ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md)). It is not
bit-identical: the CPU calls glibc's `pow`, `atan2`, `sin`, `cos`, `exp` and
`powf`, the device CUDA's, and a few pixels per million round to the
neighbouring float.

`LIBM_TWINS` in `scripts/ci/cross_backend_calibration.py` gives such a cell its
own tolerance (`ciede`: `cuda` at `1e-9`, label `libm:ADR-1426`) and runs both
sides at `--precision max`. The bound is for frames of 576x324 and larger; one
such pixel weighs more in a smaller frame.

The three twins that carry this bound:

| Twin | How it differs from the CPU | ADR |
| --- | --- | --- |
| `ciede_cuda` | CUDA's math library against glibc's. | [ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md) |
| `ciede_sycl` | A SYCL kernel has no fp64 type, so it runs the same statements on fp32 pairs, which decide all but about one pixel in a million the way fp64 does. | [ADR-1436](../adr/1436-sycl-ciede-cpu-arithmetic.md) |
| `ciede_hip` | Runs the SYCL twin's pair statements from the same header. | [ADR-1448](../adr/1448-hip-ciede-cpu-arithmetic.md) |

Measured against a GCC build on 180 frames (Netflix 576x324 at 8 to 16 bits and
as 10-bit 4:2:2, Sparks, both 1080p checkerboards, BBB 1920x1080 and
3840x2160), since [ADR-1467](../adr/1467-ciede-squares-as-products.md) writes
the squares of `ciede.c` as products:

| Twin | Frames identical | Largest difference | Frames identical before ADR-1467 | Largest before |
| --- | ---: | ---: | ---: | ---: |
| CUDA | 127 | 5.2e-12 | 113 | 2.0e-11 |
| SYCL | 124 | 5.2e-12 | 111 | 2.0e-11 |
| HIP | 127 | 5.2e-12 | 113 | 2.0e-11 |

The Netflix pair is identical on every frame. What is left is glibc's
`powf(x, 7)`. The bound stays `1e-9`: it is the size of one differing pixel on
the smallest gated frame, and there are fewer such pixels, not smaller ones.

## Metal

The `metal` backend runs where an Apple device is: the macOS tester bundle
carries the gate and runs it on its four fixtures on the tester's Mac
([ADR-1496](../adr/1496-metal-gate-in-tester-bundle.md)). The SpEED features
have no Metal twin and are not run there.

No fragment lists a Metal twin yet, so the bundle runs the gate with
`--hold-exact metal`: every Metal cell is compared with tolerance `0` at
`--precision max`, or at the `LIBM_TWINS` bound for ciede (`1e-9`), and the
label in the output is `held-exact:ADR-1496`. That run is the measurement a
`scripts/ci/exact_twins.d/<feature>.metal` fragment cites; the option never
replaces the fragment.

The bundle leaves `float_ssim` and `float_ssim_lcs` out of the 1080p fixtures,
where the Metal twin's automatic scale (4) is one it does not implement
(`T-METAL-FLOAT-SSIM-SCALE-GT1-2026-09-29`); the report lists them under
`left_out`.

The gate must stay importable with the Python standard library alone and read
nothing beyond `scripts/ci/cross_backend_calibration.py`,
`scripts/lib/safe_subprocess.py`, `scripts/ci/exact_twins.d/` and the ADR files
the fragments cite: those are the files the bundle copies (`GATE_FILES` in
`tools/rc1-tester/image/prepare_build.py`).

## Run it locally

Use the dev-MCP container, which carries the backend toolchains and the
repository fixture mounts.

1. Build and start the container:

    ```bash
    docker compose --project-directory "$(git rev-parse --show-toplevel)" \
      -f dev/docker-compose.yml build dev-mcp
    docker compose -f dev/docker-compose.yml up -d
    ```

2. Run the gate inside it:

    ```bash
    docker exec vmaf-dev-mcp bash -lc '
      cd /workspace &&
      python3 scripts/ci/cross_backend_parity_gate.py \
        --vmaf-binary /usr/local/bin/vmaf \
        --reference testdata/ref_576x324_48f.yuv \
        --distorted testdata/dis_576x324_48f.yuv \
        --width 576 --height 324 \
        --backends cpu cuda \
        --features float_ssim vif \
        --json-out /tmp/parity.json \
        --md-out /tmp/parity.md
    '
    ```

On an AMD host, the same runner compares the HIP twins, including the
`float_ssim` `enable_lcs` cell:

```bash
python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build-hip/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 \
    --backends cpu hip --hip-device 0 \
    --features float_ssim float_ssim_lcs psnr motion_v2 vif \
    --json-out /tmp/parity-hip.json --md-out /tmp/parity-hip.md
```

Pin each concurrent run to different hardware; do not multiplex one device
across parallel parity jobs.

## Whole models on one backend

The gate compares one extractor at a time. Whether a model runs wholly on a
device is a separate question: the option gate
([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md),
[ADR-1316](../adr/1316-gpu-option-value-capability-fallback.md)) sends an
extractor to the CPU when its twin cannot honour a model option, and the run
still succeeds. `core/test/test_gpu_v1_models_no_fallback.py` checks the model
set end to end. For every built-in `vmaf_v1.0.16*` model and the default model,
it scores these fixtures with `--backend cpu` and with the device backend at
`--precision max`:

- the 576x324 `src01_hrc00` / `src01_hrc01` pair at 8, 10 and 12 bits 4:2:0
  and 10 bits 4:2:2;
- the first 16 frames of the 3840x2160 pair in `testdata/bbb`.

It fails when the device run's `feature_backends` lists an extractor on any
other backend, or when any per-frame, pooled or aggregate value differs.
Each run also scores `--feature float_adm=adm_csf_mode=1`, a known fallback,
and fails if the check does not report it.

Meson registers one test per backend the build enables:
`test_cuda_v1_models_no_fallback`, `test_sycl_v1_models_no_fallback` and
`test_hip_v1_models_no_fallback` (suites `slow`, `gpu` and the backend name).
Each one skips with the reason when it finds no device or a missing fixture.
`testdata/bbb` and `python/test/resource/yuv` are not in the repository.
When `~/.cache/vmafx-locks` exists (or `VMAFX_LOCK_DIR`), each device run
takes that backend's lock file. Run a SYCL build's test with the oneAPI
environment loaded:

```bash
python3 scripts/ci/run_meson_test.py -- -C build test_cuda_v1_models_no_fallback
```

## Read the output

The JSON artifact contains one record per cell with `status`, `tolerance_abs`,
`tolerance_source`, `n_frames`, `per_metric_max_abs_diff`,
`per_metric_mismatches`, and a free-text `note` for errors. Its top-level
`schema_version` versions the format. The Markdown artifact contains the same
cell summary plus a failure-detail section.

A cell's status is one of:

| Status | Meaning |
|---|---|
| `OK` | Every per-frame metric is within tolerance. |
| `FAIL` | At least one per-frame mismatch exceeds `tolerance_abs`. |
| `ERROR` | Execution failed before diffing, the frame counts differ, or one backend does not emit a metric the cell compares (the note names the backend and the metrics). |
| `SKIP` | The fixture cannot exercise the cell (the chroma of a `float_ms_ssim_chroma` fixture is below 176 pixels); the note says why. A skipped cell does not fail the run and is not a pass. |

## Add a feature or backend

1. Add the feature-to-metric mapping to `FEATURE_METRICS` in both parity
   scripts (`cross_backend_parity_gate.py` and `cross_backend_vif_diff.py`),
   with the metrics the CPU extractor emits by default. Metric names must match
   the keys emitted by the selected `vmaf` feature extractor.
2. Add a `FEATURE_TOLERANCE` entry only when the feature differs from the
   default `5e-5`, and cite the measurement or ADR that owns the relaxation.
3. Declare a twin exact, once it is measured bit-identical, by adding
   `scripts/ci/exact_twins.d/<feature>.<backend>` (`adr:` and `evidence:`),
   then run `make docs-fragments-write`. Do not edit the calibration module,
   the tests or this page for it.
4. Add or update unit tests in `scripts/ci/test_cross_backend_parity_gate.py`
   and update the tolerance table above where the feature's tolerance changed.
   The exact-twin tests hold for any set of fragments and need no edit.
5. For a new backend, extend the suffix and device-selection maps, add
   executable coverage, and update the consuming workflow explicitly. Adding
   support to the script alone does not create CI coverage.

## Relationship to other gates

```figure
test-gates
```

| Gate | Role |
|---|---|
| **Netflix golden** ([ADR-0024](../adr/0024-netflix-golden-preserved.md)) | CPU numerical correctness; required and untouchable. |
| **SYCL Parity (Arc A380)** | Conditional required lane; CPU and SYCL `float_ssim` on real Arc hardware. It runs for eligible non-draft in-repository pull requests, pushes to `master` and manual dispatches when `SYCL_ARC_RUNNER_ENABLED=true` and the runner is online; when the lane is disabled, the required-check aggregator explicitly accepts its skip. |
| Backend Meson parity tests | Backend-specific correctness, including large-fixture variants where registered. |
| `test_<backend>_v1_models_no_fallback` | Every `vmaf_v1.0.16*` model and the default model run wholly on the device and return the CPU run's bits ([Whole models on one backend](#whole-models-on-one-backend)). |
| This matrix runner outside CI | Broader CPU, CUDA, SYCL and HIP feature sweeps and calibration evidence. |
| macOS tester bundle ([tester page](../usage/tester-image.md)) | Runs this gate's Metal cells, held exact, on an outside tester's Mac ([ADR-1496](../adr/1496-metal-gate-in-tester-bundle.md)). |
| Per-backend snapshots (`testdata/scores_cpu_*.json`) | Snapshot-based regression checks, not pairwise parity. |

## Sources

- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md): original matrix design.
- [ADR-0726](../adr/0726-drop-vulkan-backend.md): removal of Vulkan and its
  hosted matrix lanes.
- [ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md): current Arc runner
  and lane-switch contract.
- [ADR-1428](../adr/1428-exact-twins-fragments.md): exact-twin fragments.

## History

- **ADR-1477**: `speed_chroma` and `speed_temporal` were libm twins (`5e-6`
  and `4e-5`; ADR-1430, ADR-1452, ADR-1460) because the fork's `speed.c`
  called `log2f`, each twin rounded `log2` on the device, and glibc's `log2f`
  moved a few scores by one to five steps of the fp32 result. `speed.c`
  evaluates Netflix's fp64 `log2` again, and the twins form the entropies and
  the score on the host with those statements and the same C library, so the
  six cells are [exact](#exact-twins): `speed_chroma.{cuda,hip,sycl}` and
  `speed_temporal.{cuda,hip,sycl}` under `scripts/ci/exact_twins.d/`. An
  AdaptiveCpp build of the SYCL twin is outside that
  ([ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md)).
- **ADR-1467**: the first ciede measurements predate the change that writes
  the squares of `ciede.c` as products. On an RTX 4090: 1.4e-11 on 200 frames
  of BBB 3840x2160, 6.9e-13 on the Netflix 576x324 pair; on an Arc A380 the
  same 1.4e-11 and 6.9e-13; on a gfx1036 1.4e-11 on 48 frames of BBB
  3840x2160 and 6.9e-13 on the Netflix pair, where of 437 million pixels
  compared one by one 2 206 differ through glibc's `powf` and 8 through the
  last bits of a pair.
- **ADR-1496**: the gate gained the `metal` backend, `--metal-device` and
  `--hold-exact`. `T-GATE-NO-METAL-BACKEND-2026-10-02` in
  [`state.md`](../state.md) tracks the remaining work: a hardware report that
  shows every Metal cell `OK`.
