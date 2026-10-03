<!-- markdownlint-disable MD060 -->
# Cross-backend GPU-parity gate

`scripts/ci/cross_backend_parity_gate.py` compares per-frame metrics across
selected CPU, CUDA, SYCL, HIP and Metal backends. It can run every selected
feature over every selected backend pair and emits machine-readable JSON plus
a Markdown summary.

This script is not currently an all-backend, every-PR CI matrix. Vulkan and
its hosted lavapipe lane were removed by
[ADR-0726](../adr/0726-drop-vulkan-backend.md). The current CI consumer is the
`SYCL Parity (Arc A380)` job in
[`.github/workflows/sycl-parity.yml`](../../.github/workflows/sycl-parity.yml),
which compares CPU and SYCL `float_ssim` on the self-hosted Arc runner. That
job runs for eligible non-draft in-repository pull requests, pushes to
`master`, and manual dispatches when `SYCL_ARC_RUNNER_ENABLED=true` and the
runner is online. When the lane is disabled, the required-check aggregator
explicitly accepts its skip.

## What it checks

- **Per-feature absolute tolerance.** The default is `5e-5` (places=4), the
  fork's GPU-vs-CPU contract from ADR-0125, ADR-0138, and ADR-0140.
  Feature-specific relaxations live in `FEATURE_TOLERANCE` inside
  [`cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py):

  | Feature | Tolerance | Contract source |
  |---|---:|---|
  | `vif`, `motion`, `motion_debug`, `motion_mffw`, `motion_v2`, `motion_v2_mffw`, `adm`, `psnr` (all three planes: `psnr_y`, `psnr_cb`, `psnr_cr`), `float_moment`, `cambi` | `5e-5` | ADR-0125 / ADR-0138 / ADR-0140 / ADR-0360; `motion_debug` is `motion` with `debug=true` and adds `integer_motion`; both cells compare `VMAF_integer_feature_motion_sad_score`, the per-frame score `motion2` / `motion3` are derived from, next to `integer_motion2` and `integer_motion3` ([ADR-1418](../adr/1418-motion-parity-gate-metric-alignment.md)); `motion_mffw` and `motion_v2_mffw` are `motion` and `motion_v2` with `motion_five_frame_window=true:motion_moving_average=true`, the option set of the `vmaf_v1.0.16_hfr_*` models, and compare the SAD score, `motion2` and `motion3` under their option-suffixed names ([ADR-1491](../adr/1491-gpu-motion-five-frame-window.md)) |
  | `ssim` (the fixed-point extractor; its twins are `integer_ssim_<backend>`) | `5e-5` | ADR-0564 (int64 moments, one double term per pixel) |
  | any feature, cell whose two sides are `cpu` or [listed exact twins](cross-backend-exact-twins.md) | `0` (bit-identical, compared at `--precision max`) | the ADR in the twin's fragment under `scripts/ci/exact_twins.d/`; the rows above and below stay for every other backend ([Exact twins](#exact-twins)) |
  | `float_ssim`, `float_ssim_lcs`, `float_ms_ssim`, `float_ms_ssim_lcs`, `float_psnr`, `float_motion`, `float_vif`, `float_adm` | `5e-5` | ADR-0188 / ADR-0192 / ADR-0215 / ADR-1382 |
  | `ciede` | `5e-3` | ADR-0187 (per-pixel pow/sqrt/sin/atan2) |
  | `ciede` (every pair of CPU, CUDA, SYCL and HIP) | `1e-9`, compared at `--precision max` | ADR-1426, ADR-1436, ADR-1448 (the twins run the CPU's arithmetic and the CPU's sum; what differs is the math library and, on SYCL and HIP, the last bits of an fp32 pair, `LIBM_TWINS`); the `5e-3` row stays for the other twins |
  | `speed_chroma` (the three scores `speed_chroma_u`, `_v`, `_uv`) | `5e-5` | places=4 for a twin that is not listed as exact; the CUDA, HIP and SYCL twins are ([ADR-1477](../adr/1477-speed-upstream-double-math.md)) |
  | `speed_temporal` (the one score `speed_temporal`) | `5e-5` | places=4 for a twin that is not listed as exact; the CUDA, HIP and SYCL twins are (ADR-1477) |
  | `psnr_hvs` (a twin that is not listed as exact) | `5e-4` at 576x324 and below, `5e-4 × √(N / N₅₇₆ₓ₃₂₄)` above | ADR-0191 (DCT plus per-block float reduction); ADR-1361 (area scaling) |
  | `ssimulacra2` | `5e-3` | ADR-0192 (XYB cube root plus IIR blur) |

- **Twins that differ only in their math library.** `ciede_cuda` evaluates
  `ciede.c`'s expressions in its types and the host adds the per-pixel values
  in the CPU's order ([ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md)).
  It is not bit-identical: the CPU calls glibc's `pow`, `atan2`, `sin`, `cos`,
  `exp` and `powf`, the device CUDA's, and a few pixels per million round to
  the neighbouring float. `LIBM_TWINS` in
  `scripts/ci/cross_backend_calibration.py` gives such a cell its own
  tolerance (`ciede`: `cuda` at `1e-9`, label `libm:ADR-1426`) and runs both
  sides at `--precision max`. Measured on an RTX 4090: 1.4e-11 on 200 frames
  of BBB 3840x2160, 6.9e-13 on the Netflix 576x324 pair. The bound is for
  frames of 576x324 and larger; one such pixel weighs more in a smaller
  frame. `ciede_sycl` is listed at the same `1e-9`
  ([ADR-1436](../adr/1436-sycl-ciede-cpu-arithmetic.md)): a SYCL kernel has
  no fp64 type, so it runs the same statements on fp32 pairs, which decide
  all but about one pixel in a million the way fp64 does. Measured on an Arc
  A380: the same 1.4e-11 and 6.9e-13. `ciede_hip` runs the SYCL twin's pair
  statements from the same header and is listed at `1e-9` as well
  ([ADR-1448](../adr/1448-hip-ciede-cpu-arithmetic.md)). Measured on a
  gfx1036: 1.4e-11 on 48 frames of BBB 3840x2160 and 6.9e-13 on the Netflix
  pair; of 437 million pixels compared one by one, 2 206 differ through
  glibc's `powf` and 8 through the last bits of a pair.
  Those figures predate
  [ADR-1467](../adr/1467-ciede-squares-as-products.md), which writes the
  squares of `ciede.c` as products, the form the twins compute. Since then,
  against a GCC build on 180 frames (Netflix 576x324 at 8 to 16 bits and as
  10-bit 4:2:2, Sparks, both 1080p checkerboards, BBB 1920x1080 and
  3840x2160): CUDA 127 frames identical and at most 5.2e-12, SYCL 124 and
  5.2e-12, HIP 127 and 5.2e-12 (113, 111 and 113 frames and 2.0e-11 before);
  the Netflix pair is identical on every frame. What is left is glibc's
  `powf(x, 7)`. The bound stays `1e-9`: it is the size of one differing pixel
  on the smallest gated frame, and there are fewer such pixels, not smaller
  ones.
  `speed_chroma` and `speed_temporal` were in this table until
  [ADR-1477](../adr/1477-speed-upstream-double-math.md) (`5e-6` and `4e-5`,
  ADR-1430, ADR-1452, ADR-1460): the fork's `speed.c` called `log2f`, each
  twin rounded `log2` on the device, and glibc's `log2f` moved a few scores
  by one to five steps of the fp32 result. `speed.c` evaluates Netflix's fp64
  `log2` again, and the twins form the entropies and the score on the host
  with those statements and the same C library, so the six cells are
  [exact](#exact-twins): `speed_chroma.{cuda,hip,sycl}` and
  `speed_temporal.{cuda,hip,sycl}` under `scripts/ci/exact_twins.d/`. An
  AdaptiveCpp build of the SYCL twin is outside that
  ([ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md)).

- **Every registered twin is a gate cell.** A twin that is registered in
  `core/src/feature/feature_extractor.cpp` and is no gate feature's extractor
  is guarded by its own unit test only. `speed_temporal` was the one such
  twin on CUDA, SYCL and HIP.
  `core/test/test_parity_gate_covers_registered_twins.py` fails when a new
  twin of a gated backend has no gate feature, and when the tables of this
  gate and of `cross_backend_vif_diff.py` differ. To add a twin, add its
  feature to `FEATURE_METRICS` in both scripts with the metrics the CPU
  extractor emits by default, and a tolerance. The gate has no `metal`
  backend, so the Metal twins are outside it
  (`T-GATE-NO-METAL-BACKEND-2026-10-02` in [`state.md`](../state.md)).

- **Backend pairs.** The script accepts `cpu`, `cuda`, `sycl`, and `hip`; its
  command line default is `cpu cuda`. `--hip-device` picks the HIP device by
  index (`--hip_device` on the `vmaf` command line). No CI job runs the HIP
  cells; they are for a local run on an AMD host. Metal has backend-specific
  tests but is not wired into this matrix runner. Extractors whose twin is not
  named `<feature>_<backend>` are listed in `BACKEND_EXTRACTOR_ALIASES`
  (`float_ms_ssim` is `integer_ms_ssim_hip` on HIP; `ssim` is
  `integer_ssim_cuda`, `integer_ssim_sycl` and `integer_ssim_hip`).

- **`float_ssim_lcs`.** Runs `float_ssim` with `enable_lcs=true` and compares
  `float_ssim_l`, `float_ssim_c` and `float_ssim_s` next to the score, at the
  same `5e-5` ([ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)).

- **Per-device calibration.** `--gpu-id` selects the most-specific matching
  row in `scripts/ci/gpu_ulp_calibration.yaml`. If no row or feature override
  matches, the feature's built-in tolerance remains authoritative.

- **Area-scaled `psnr_hvs`.** The CPU `psnr_hvs` adds every coefficient error
  of a plane into one `float`, so its rounding error, and the achievable
  CPU/GPU agreement, grows with the number of 8x8 blocks. Both
  `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py` take the
  `psnr_hvs` tolerance from the table or calibration row as the contract at
  576x324 and multiply it by √(N / N₅₇₆ₓ₃₂₄) for larger fixtures, where N is
  the luma plane's term count (64 per block, a block every 7 pixels). Frames
  of 576x324 or smaller keep `5e-4`; 1920x1080 gets `1.67e-3` and 3840x2160
  `3.34e-3`. The label in the output shows the factor, for example
  `default+area x6.69`. [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)
  derives the factor from the float-accumulation bound.
  This is the contract of a twin that sums each block on the device. None of
  the gate's backends has such a twin any more (the Metal twin does, and
  Metal is not a gate backend), so no `psnr_hvs` cell of the matrix uses it;
  it stays for a caller that names no backends and for a twin added later.

- **Exact twins.** A cell whose two sides are the CPU extractor or a listed
  exact twin is compared with tolerance `0`; see [Exact twins](#exact-twins).

- **FP16 features.** Names passed through `--fp16-features` use the `1e-2`
  FP16 absolute-tolerance contract.

## Exact twins

A GPU twin that returns the CPU
extractor's bits for a feature is listed by adding one file,
`scripts/ci/exact_twins.d/<feature>.<backend>` (for example `psnr_hvs.cuda`),
and nothing else shared ([ADR-1428](../adr/1428-exact-twins-fragments.md)).
The file holds two `key: value` lines: `adr:` (one or more `ADR-NNNN` that
exist under `docs/adr/`, the ADR that establishes exactness) and `evidence:`
(one line: fixtures and result). `cross_backend_calibration.py` builds
`EXACT_TWINS` from the directory at import and rejects an unknown key, a
missing key, an empty file, an unknown feature or backend, and a missing or
empty directory. The [generated table of exact twins](cross-backend-exact-twins.md)
is rendered from the same files by `make docs-fragments-write`.

A cell whose two sides are the CPU extractor or a listed twin is compared
with tolerance `0`, at every frame size and ahead of any calibration row,
and both sides run with `--precision max` so that a last-bit difference
is not rounded away by the default `%.6f` output. The label in the output is
`exact:ADR-1397` for every listed twin. An explicit
`--fp16-features <feature>` still selects the FP16 contract. A cell with a
backend that is not listed keeps the tolerance of the rows above.

The rule: listing needs a measurement that shows bit-identity (`--precision
max`, the Netflix 576x324 pairs, the 1080p checkerboard pairs and
`testdata/bbb` 4K) and an ADR that records it. A listed twin that drifts is
fixed; it is never given a tolerance and never taken off the list to make a
gate pass.

Identical on those fixtures is necessary, not sufficient
([ADR-1437](../adr/1437-hip-exact-twins-declared.md)). A twin is listed when
it also reaches the CPU's value by construction: integer sums on the device,
the CPU's own helpers on the host, or the CPU's arithmetic type for type.
Natural 8-bit content does not tell the two apart, and the repository's 10-,
12- and 16-bit Netflix clips are the 8-bit clip shifted left. Before listing
a twin, also run it on full-range noise at 8, 10, 12 and 16 bits:
`float_psnr_hip` and `float_moment_hip` matched every real clip measured and
differ there.

The equality holds between runs of one `vmaf` binary, which is how the gate
runs a cell. The dB value goes through the host's `log10`: a binary built
with oneAPI `icx` uses Intel's `libimf`, a gcc build uses glibc, and the
two round differently by one unit in the last place on a few frames (3 of
the 48 Netflix frames), for the CPU extractor and the twins alike. Do not
compare a twin from one build with the CPU extractor of another.

An exact cell runs one binary on both sides, so it also needs the CPU
extractor of that binary to be the reference arithmetic; for an icx build on
an AVX-512 host that needs the x86 SIMD libraries built without FP
contraction (#1706).

## Metal

The `metal` backend (`--metal-device`, default 0) runs where an Apple device
is: the macOS tester bundle carries the gate and runs it on its four fixtures
on the tester's Mac
([ADR-1496](../adr/1496-metal-gate-in-tester-bundle.md)). The Metal twins of
the fixed-point extractors carry the CPU file's name (`integer_adm_metal`,
`integer_motion_metal`, ...), which `BACKEND_EXTRACTOR_ALIASES` maps; the
SpEED features have no Metal twin and are not run there.

No fragment lists a Metal twin yet, so the bundle runs the gate with
`--hold-exact metal`: every Metal cell is compared with tolerance `0` at
`--precision max`, or at the `LIBM_TWINS` bound for ciede (`1e-9`), and the
label in the output is `held-exact:ADR-1496`. That run is the measurement a
`scripts/ci/exact_twins.d/<feature>.metal` fragment cites; the option never
replaces the fragment. The bundle leaves `float_ssim` and `float_ssim_lcs` out
of the 1080p fixtures, where the Metal twin's automatic scale (4) is one it
does not implement (`T-METAL-FLOAT-SSIM-SCALE-GT1-2026-09-29`); the report
lists them under `left_out`.

The gate must stay importable with the Python standard library alone and
read nothing beyond `scripts/ci/cross_backend_calibration.py`,
`scripts/lib/safe_subprocess.py`, `scripts/ci/exact_twins.d/` and the ADR files
the fragments cite: those are the files the bundle copies
(`GATE_FILES` in `tools/rc1-tester/image/prepare_build.py`).

## Run it locally

Use the dev-MCP container, which carries the backend toolchains and the
repository fixture mounts:

```bash
docker compose --project-directory "$(git rev-parse --show-toplevel)" \
  -f dev/docker-compose.yml build dev-mcp
docker compose -f dev/docker-compose.yml up -d

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
`float_ssim` enable_lcs cell:

```bash
python3 scripts/ci/cross_backend_parity_gate.py   --vmaf-binary build-hip/tools/vmaf   --reference testdata/ref_576x324_48f.yuv   --distorted testdata/dis_576x324_48f.yuv   --width 576 --height 324   --backends cpu hip --hip-device 0   --features float_ssim float_ssim_lcs psnr motion_v2 vif   --json-out /tmp/parity-hip.json --md-out /tmp/parity-hip.md
```

Pin each concurrent run to different hardware; do not multiplex one device
across parallel parity jobs.

## Read the output

The JSON artifact contains one record per cell with `status`,
`tolerance_abs`, `tolerance_source`, `n_frames`,
`per_metric_max_abs_diff`, `per_metric_mismatches`, and a free-text `note` for
errors. Its top-level `schema_version` versions the format. The Markdown
artifact contains the same cell summary plus a failure-detail section.

A cell's status is one of:

| Status | Meaning |
|---|---|
| `OK` | Every per-frame metric is within tolerance. |
| `FAIL` | At least one per-frame mismatch exceeds `tolerance_abs`. |
| `ERROR` | Execution failed before diffing, the frame counts differ, or one backend does not emit a metric the cell compares (the note names the backend and the metrics). |

## Add a feature or backend

1. Add the feature-to-metric mapping to `FEATURE_METRICS` in both parity
   scripts. Metric names must match the keys emitted by the selected `vmaf`
   feature extractor.
2. Add a `FEATURE_TOLERANCE` entry only when the feature differs from the
   default `5e-5`, and cite the measurement or ADR that owns the relaxation.
3. Declare a twin exact, once it is measured bit-identical, by adding
   `scripts/ci/exact_twins.d/<feature>.<backend>` (`adr:` and `evidence:`),
   then run `make docs-fragments-write`. Do not edit the calibration module,
   the tests or this page for it.
4. Add or update unit tests in
   `scripts/ci/test_cross_backend_parity_gate.py` and update the tolerance
   table above where the feature's tolerance changed. The exact-twin tests hold
   for any set of fragments and need no edit.
5. For a new backend, extend the suffix and device-selection maps, add
   executable coverage, and update the consuming workflow explicitly. Adding
   support to the script alone does not create CI coverage.

Netflix golden assertions remain untouched; parity thresholds never replace
the CPU golden-data gate.

## Relationship to other gates

| Gate | Role |
|---|---|
| **Netflix golden** ([§8](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify)) | CPU numerical correctness; required and untouchable. |
| **SYCL Parity (Arc A380)** | Conditional required lane; CPU↔SYCL `float_ssim` on real Arc hardware. |
| Backend Meson parity tests | Backend-specific correctness, including large-fixture variants where registered. |
| This matrix runner outside CI | Broader CPU/CUDA/SYCL/HIP feature sweeps and calibration evidence. |
| macOS tester bundle ([tester page](../usage/tester-image.md)) | Runs this gate's Metal cells, held exact, on an outside tester's Mac ([ADR-1496](../adr/1496-metal-gate-in-tester-bundle.md)). |
| Per-backend snapshots (`testdata/scores_cpu_*.json`) | Snapshot-based regression checks, not pairwise parity. |

## Sources

- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md): original matrix design.
- [ADR-0726](../adr/0726-drop-vulkan-backend.md): removal of Vulkan and its
  hosted matrix lanes.
- [ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md): current Arc runner
  and lane-switch contract.
