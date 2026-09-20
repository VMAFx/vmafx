<!-- markdownlint-disable MD060 -->
# Cross-backend GPU-parity gate

`scripts/ci/cross_backend_parity_gate.py` compares per-frame metrics across
selected CPU, CUDA, and SYCL backends. It can run every selected feature over
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
  | `vif`, `motion`, `motion_v2`, `adm`, `psnr`, `float_moment`, `cambi` | `5e-5` | ADR-0125 / ADR-0138 / ADR-0140 / ADR-0360 |
  | `float_ssim`, `float_ms_ssim`, `float_ms_ssim_lcs`, `float_psnr`, `float_motion`, `float_vif`, `float_adm` | `5e-5` | ADR-0188 / ADR-0192 / ADR-0215 |
  | `ciede` | `5e-3` | ADR-0187 (per-pixel pow/sqrt/sin/atan2) |
  | `psnr_hvs` | `5e-4` | ADR-0191 (DCT plus per-block float reduction) |
  | `ssimulacra2` | `5e-3` | ADR-0192 (XYB cube root plus IIR blur) |

- **Backend pairs.** The script accepts `cpu`, `cuda`, and `sycl`; its command
  line default is `cpu cuda`. HIP and Metal have backend-specific tests but are
  not wired into this matrix runner.

- **Per-device calibration.** `--gpu-id` selects the most-specific matching
  row in `scripts/ci/gpu_ulp_calibration.yaml`. If no row or feature override
  matches, the feature's built-in tolerance remains authoritative.

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
| `ERROR` | Execution failed before diffing, or the frame counts differ. |

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
