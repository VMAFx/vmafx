# Cross-backend GPU-parity gate

The cross-backend gate compares supported GPU extractors with the CPU
reference on identical frames. The current matrix supports `cpu`, `cuda`, and
`sycl`; it emits machine-readable JSON and a Markdown summary for every
requested `(feature, backend-pair)` cell.

The required hardware lane is **`SYCL Parity (Arc A380)`** in
[`sycl-parity.yml`](../../.github/workflows/sycl-parity.yml). It runs the
matrix gate as `cpu sycl` when the repository's Arc runner is enabled and
available. CUDA parity is exercised by backend tests and can be run through
the same matrix command on a CUDA-capable development host or container.

## What it checks

- **Per-feature absolute tolerance.** The default is `5e-5`. Relaxations live
  in `FEATURE_TOLERANCE` inside
  [`cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py)
  and identify the decision that established each contract.

  | Feature | Tolerance | Contract source |
  | --- | ---: | --- |
  | `vif`, `motion`, `motion_v2`, `adm`, `psnr`, `float_moment` | `5e-5` | ADR-0125 / ADR-0138 / ADR-0140 |
  | `float_ssim`, `float_ms_ssim`, `float_psnr`, `float_motion`, `float_vif`, `float_adm`, `cambi` | `5e-5` | ADR-0188 / ADR-0192 / ADR-0360 |
  | `ciede` | `5e-3` | ADR-0187 |
  | `psnr_hvs` | `5e-4` | ADR-0191 |
  | `ssimulacra2` | `5e-3` | ADR-0192 |

- **Supported backend pairs.** `--backends` accepts `cpu`, `cuda`, and `sycl`.
  Supplying all three creates the CPU↔CUDA, CPU↔SYCL, and CUDA↔SYCL cells.

- **Architecture calibration.** `--gpu-id` selects a row from
  `scripts/ci/gpu_ulp_calibration.yaml`. A calibrated per-feature value wins;
  an absent row or feature falls back visibly to the table above. Each output
  cell records the selected tolerance source.

- **FP16 opt-in.** `--fp16-features` applies the `1e-2` absolute FP16
  contract only to the named features.

## Run it locally

Use the development container for backend work. Rebuild it first when its
image predates changes under `core/`, `dev/`, `ai/`, or `mcp-server/`:

```bash
docker compose --project-directory "$(git rev-parse --show-toplevel)" \
  -f dev/docker-compose.yml build dev-mcp
docker compose --project-directory "$(git rev-parse --show-toplevel)" \
  -f dev/docker-compose.yml up -d
```

Run one backend per physical device. This example checks the Arc-backed SYCL
extractor against CPU:

```bash
docker exec vmaf-dev-mcp python3 /workspace/scripts/ci/cross_backend_parity_gate.py \
  --vmaf-binary /usr/local/bin/vmaf \
  --reference /workspace/testdata/ref_576x324_48f.yuv \
  --distorted /workspace/testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 \
  --backends cpu sycl \
  --features float_ssim \
  --gpu-id sycl:0x8086:0x56a5 \
  --json-out /tmp/parity.json \
  --md-out /tmp/parity.md
```

For CUDA, use `--backends cpu cuda`, a CUDA calibration ID, and a container
with the NVIDIA device exposed. Do not schedule CUDA and SYCL work onto the
same physical device concurrently.

## Read the output

The JSON report has one record per cell with `status`, `tolerance_abs`,
`tolerance_source`, `n_frames`, `per_metric_max_abs_diff`,
`per_metric_mismatches`, and an error `note`. The Markdown report renders the
same records as a reviewable table.

| Status | Meaning |
| --- | --- |
| `OK` | Every metric on every frame is within tolerance |
| `FAIL` | At least one metric exceeds the selected tolerance |
| `ERROR` | A run failed before comparison or returned incompatible output |

An `ERROR` is a gate failure. Missing binaries, unavailable devices, command
failures, malformed output, and frame-count mismatches are never treated as
successful parity.

## Add a feature

1. Add its emitted metric names to `FEATURE_METRICS` in
   `scripts/ci/cross_backend_parity_gate.py`.
2. Add `FEATURE_TOLERANCE` only when the default `5e-5` contract is wrong, and
   cite the measurement-backed decision beside it.
3. Add or update backend parity tests for the extractor.
4. Update the tolerance table above and the relevant metric/backend guide.
5. Run the unit tests and a real device comparison; do not adjust Netflix
   golden assertions to accommodate backend drift.

## Related gates

| Gate | Role |
| --- | --- |
| [Netflix golden](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify) | CPU numerical ground truth |
| `SYCL Parity (Arc A380)` | Required CPU↔SYCL hardware comparison when the runner is enabled |
| Backend unit parity tests | Kernel-level CPU↔GPU comparisons, including large fixtures |
| `testdata/scores_cpu_*.json` snapshots | Regression snapshots; not a substitute for pairwise comparison |

The matrix design originates in [ADR-0214](../adr/0214-gpu-parity-ci-gate.md),
and calibrated tolerance selection is defined by
[ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md).
