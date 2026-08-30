<!-- markdownlint-disable MD060 -->
# Cross-backend GPU-parity gate

The GPU-parity gate is the fork's single matrix gate for verifying
that every GPU backend agrees with the CPU scalar reference (and
with each other) on every feature with a GPU twin. It generalises
the older single-feature gate
(`scripts/ci/cross_backend_vif_diff.py`) to a one-shot run that
covers every `(feature, backend-pair)` cell.

The gate is enforced on every PR as the CI job
**`vulkan-parity-matrix-gate`** in
[`.github/workflows/tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).

## What it gates

- **Per-feature absolute tolerance.** Default `5e-5` (places=4 —
  the fork's GPU-vs-CPU contract from ADR-0125 / ADR-0138 /
  ADR-0140). Feature-specific relaxations live in the
  `FEATURE_TOLERANCE` table inside
  [`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py)
  and each carries an ADR pointer:

  | Feature | Tolerance | Contract source |
  |---|---:|---|
  | `vif`, `motion`, `motion_v2`, `adm`, `psnr`, `float_moment` | `5e-5` | ADR-0125 / ADR-0138 / ADR-0140 |
  | `float_ssim`, `float_ms_ssim`, `float_psnr`, `float_motion`, `float_vif`, `float_adm` | `5e-5` | ADR-0188 / ADR-0192 |
  | `ciede` | `5e-3` | ADR-0187 (per-pixel pow/sqrt/sin/atan2) |
  | `psnr_hvs` | `5e-4` | ADR-0191 (DCT + per-block float reduction) |
  | `ssimulacra2` | `5e-3` | ADR-0192 (XYB cube root + IIR blur) |

  Vulkan `motion` is routed through the canonical `integer_motion_vulkan`
  extractor in both parity scripts. The legacy `motion_vulkan` extractor
  remains addressable by explicit name for compatibility, but it is not the
  lavapipe gate path after ADR-0662.

- **Backend pairs.** The CI lane runs `cpu ↔ vulkan` only — Mesa
  lavapipe runs on stock GitHub-hosted Ubuntu and needs no GPU
  hardware. CUDA / SYCL / hardware-Vulkan are advisory until a
  self-hosted runner is wired in (see
  [`docs/development/self-hosted-runner.md`](self-hosted-runner.md));
  the script already supports them via `--backends cpu cuda sycl
  vulkan`.

- **FP16 features.** A `--fp16-features` flag forces the
  `1e-2` absolute tolerance the FP16 contract calls for. Used by
  the future ONNX Runtime tiny-AI lane (T7-39).

## How to run it locally

```bash
# Build CPU + Vulkan once
cd libvmaf && meson setup build \
    -Denable_cuda=false -Denable_sycl=false \
    -Denable_vulkan=enabled -Denable_float=true \
    --buildtype=release && ninja -C build

# Run the matrix gate on the fork's stock 576×324 fixture
cd ..
python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary core/build/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 \
    --backends cpu vulkan \
    --json-out /tmp/parity.json --md-out /tmp/parity.md
```

Default fixture in CI is the Netflix `src01_hrc00_576x324.yuv ↔
src01_hrc01_576x324.yuv` pair fetched from the
`Netflix/vmaf_resource` mirror with caching — same fixture as
the legacy lane.

## How to read failure output

The gate writes two artefacts:

- **JSON** (`--json-out`) — one record per cell with
  `status`, `tolerance_abs`, `n_frames`, `per_metric_max_abs_diff`,
  `per_metric_mismatches`, and a free-text `note` field for
  ERROR-class failures (binary not built, frame-count mismatch,
  Vulkan ICD init failure, etc.). Schema is versioned in the
  top-level `schema_version` field.
- **Markdown** (`--md-out`) — single status table plus a
  per-failure detail block. Suitable for direct PR comment.

A cell's status is one of:

| Status | Meaning |
|---|---|
| `OK` | Every per-frame metric within tolerance |
| `FAIL` | At least one per-frame mismatch beyond `tolerance_abs` |
| `ERROR` | Run failed before diffing (binary missing, fixture missing, ICD init failed, frame count mismatch) |

## How to add a new feature to the gate

1. Add the feature → metric-name list to `FEATURE_METRICS` in
   `scripts/ci/cross_backend_parity_gate.py`. The metric names
   must match the keys the `vmaf` JSON output emits when the
   feature is selected via `--feature <name>`.
2. If the feature is FP32 with no relaxations, no further change
   is needed — it picks up the default `5e-5` (places=4)
   automatically.
3. If the feature carries a relaxed contract, add an entry to
   `FEATURE_TOLERANCE` with an inline comment citing the ADR
   that justifies the relaxation. Per CLAUDE.md §12 r1
   ("Netflix golden assertions are untouchable") any tightening
   later requires a measurement-driven follow-up ADR.
4. Add a row to the table in this document.

## Relationship to other gates

| Gate | Role |
|---|---|
| **Netflix golden** ([§8](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify)) | CPU numerical correctness; required status check, untouchable |
| **GPU-parity matrix gate (this doc, T6-8)** | CPU↔GPU agreement on every feature; required status check |
| `vulkan-vif-arc-nightly` | Hardware-Vulkan drift versus lavapipe (advisory, self-hosted) |
| Per-backend snapshot diffs (`testdata/scores_cpu_*.json`) | Per-backend regression catch (snapshot-based, not pairwise) |

## Source

- ADR: [ADR-0214](../adr/0214-gpu-parity-ci-gate.md).
- Backlog row: T6-8.
- Wave-1 roadmap §7.
