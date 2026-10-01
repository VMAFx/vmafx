<!-- markdownlint-disable MD060 -->
# Cross-backend GPU-parity gate

`scripts/ci/cross_backend_parity_gate.py` compares per-frame metrics across
selected CPU, CUDA, SYCL, and HIP backends. It can run every selected feature over
every selected backend pair and emits machine-readable JSON plus a Markdown
summary.

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
  | `vif`, `motion`, `motion_debug`, `motion_v2`, `adm`, `psnr`, `float_moment`, `cambi` | `5e-5` | ADR-0125 / ADR-0138 / ADR-0140 / ADR-0360; `motion_debug` is `motion` with `debug=true` and adds `integer_motion` ([ADR-1418](../adr/1418-motion-parity-gate-metric-alignment.md)) |
  | `float_ssim`, `float_ssim_lcs`, `float_ms_ssim`, `float_ms_ssim_lcs`, `float_psnr`, `float_motion`, `float_vif`, `float_adm` | `5e-5` | ADR-0188 / ADR-0192 / ADR-0215 / ADR-1382 |
  | `adm` (CPU ↔ CUDA) | `0` (bit-identical, compared at `--precision max`) | ADR-1416 (the twin runs the CPU's host routines and folds the denominator per row); the `5e-5` row stays for the other twins |
  | `ciede` | `5e-3` | ADR-0187 (per-pixel pow/sqrt/sin/atan2) |
  | `psnr_hvs` (every pair of CPU, CUDA, SYCL and HIP) | `0` (bit-identical, compared at `--precision max`) | ADR-1397, ADR-1401 (the twins reproduce the CPU's running float sum) |
  | `psnr_hvs` (a twin that is not listed as exact) | `5e-4` at 576x324 and below, `5e-4 × √(N / N₅₇₆ₓ₃₂₄)` above | ADR-0191 (DCT plus per-block float reduction); ADR-1361 (area scaling) |
  | `float_motion` (CPU ↔ CUDA, CPU ↔ SYCL, CUDA ↔ SYCL) | `0` (bit-identical, compared at `--precision max`) | ADR-1409, ADR-1411 (the twins add their SAD in the CPU's order); the row above stays for HIP and Metal |
  | `float_vif` (CPU ↔ CUDA) | `0` (bit-identical, compared at `--precision max`) | ADR-1412 (the twin computes the CPU's arithmetic and adds in the CPU's order); the `5e-5` row stays for the other twins |
  | `ssimulacra2` | `5e-3` | ADR-0192 (XYB cube root plus IIR blur) |
  | `float_ms_ssim`, `float_ms_ssim_lcs` (CPU ↔ SYCL) | `0` (bit-identical, compared at `--precision max`) | ADR-1414 (the twin computes the CPU's arithmetic); the `5e-5` row above stays for the other twins |

- **Backend pairs.** The script accepts `cpu`, `cuda`, `sycl`, and `hip`; its
  command line default is `cpu cuda`. `--hip-device` picks the HIP device by
  index (`--hip_device` on the `vmaf` command line). No CI job runs the HIP
  cells; they are for a local run on an AMD host. Metal has backend-specific
  tests but is not wired into this matrix runner. Extractors whose twin is not
  named `<feature>_<backend>` are listed in `BACKEND_EXTRACTOR_ALIASES`
  (`float_ms_ssim` is `integer_ms_ssim_hip` on HIP).

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

- **Exact twins.** `adm_cuda` takes its CSF weights, rounding shifts and
  score conclusion from the CPU's own routines and folds the denominator once
  per row ([ADR-1416](../adr/1416-cuda-adm-cpu-row-rounding.md)).
  `psnr_hvs_cuda`, `psnr_hvs_sycl` and
  `psnr_hvs_hip` store every term the CPU sums and the host adds them in the
  CPU's order, so their scores are the CPU's bit for bit
  ([ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md) for CUDA,
  [ADR-1401](../adr/1401-psnr-hvs-sycl-hip-exact-twins.md) for SYCL and HIP).
  `float_motion_cuda` adds the absolute differences of each row on the device
  in the CPU's order and the rows on the host, with the same result
  ([ADR-1409](../adr/1409-float-motion-twins-cpu-float-sum.md)), and
  `float_motion_sycl` does so too
  ([ADR-1411](../adr/1411-sycl-float-motion-cpu-float-sum.md), measured on an
  Arc A380: 0 on the Netflix pair, both 1080p checkerboard pairs and 200
  frames of BBB 3840x2160).
  `float_vif_cuda` filters with the taps the CPU extractor computes, evaluates
  the CPU's per-pixel statistic in the CPU's types and adds the terms of each
  row on the device and the rows on the host, in the CPU's order
  ([ADR-1412](../adr/1412-cuda-float-vif-cpu-arithmetic.md)).
  `EXACT_TWINS` in `scripts/ci/cross_backend_calibration.py` lists such twins.
  A cell whose two sides are the CPU extractor or a listed twin is compared
  with tolerance `0`, at every frame size and ahead of any calibration row,
  and both sides run with `--precision max` so that a last-bit difference
  is not rounded away by the default `%.6f` output. The label in the output is
  `exact:ADR-1397` for every listed twin. Measured on an RTX 4090 (CUDA, for
  all three features), an Arc A380 (SYCL, for `psnr_hvs` and `float_motion`)
  and a gfx1036 (HIP, for `psnr_hvs`): 0 on all 200 frames of the BBB
  3840x2160 fixture and on the Netflix 576x324 pair. An explicit
  `--fp16-features psnr_hvs` still selects the FP16 contract. Adding a twin to
  the table needs a measurement that shows bit-identity and an ADR that
  records it.

  The equality holds between runs of one `vmaf` binary, which is how the gate
  runs a cell. The dB value goes through the host's `log10`: a binary built
  with oneAPI `icx` uses Intel's `libimf`, a gcc build uses glibc, and the
  two round differently by one unit in the last place on a few frames (3 of
  the 48 Netflix frames), for the CPU extractor and the twins alike. Do not
  compare a twin from one build with the CPU extractor of another.

  `float_ms_ssim_sycl` is listed for `float_ms_ssim` and `float_ms_ssim_lcs`
  ([ADR-1414](../adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md)): its kernels
  follow the CPU reference type for type and its per-scale means are the
  CPU's on every frame measured on an Arc A380. An exact cell runs one
  binary on both sides, so it also needs the CPU extractor of that binary to
  be the reference arithmetic; for an icx build on an AVX-512 host that
  needs the x86 SIMD libraries built without FP contraction (#1706).

- **FP16 features.** Names passed through `--fp16-features` use the `1e-2`
  FP16 absolute-tolerance contract.

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
3. Add or update unit tests in
   `scripts/ci/test_cross_backend_parity_gate.py` and update the tolerance
   table above.
4. For a new backend, extend the suffix and device-selection maps, add
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
| This matrix runner outside CI | Broader CPU/CUDA/SYCL feature sweeps and calibration evidence. |
| Per-backend snapshots (`testdata/scores_cpu_*.json`) | Snapshot-based regression checks, not pairwise parity. |

## Sources

- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md): original matrix design.
- [ADR-0726](../adr/0726-drop-vulkan-backend.md): removal of Vulkan and its
  hosted matrix lanes.
- [ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md): current Arc runner
  and lane-switch contract.
