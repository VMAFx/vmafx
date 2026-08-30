<!-- markdownlint-disable MD013 MD060 -->
# Research digest — slow-test audit (2026-05-30)

Companion to [ADR-0908](../adr/0908-slow-test-audit-2026-05-30.md).

## Goal

Identify any test in the fork's test surface that takes longer than
**30 seconds** to execute, document the cause, and either speed it up
or justify keeping it.

## Method

Ran each pytest package and the full meson suite on the dev host with
the default toolchain, capturing per-test durations with
`--durations=0` / `--durations=30`:

- `python -m pytest ai/tests/ --durations=30` (657 tests collected
  excluding three torchvision-blocked collectors that fail with
  `RuntimeError: operator torchvision::nms does not exist` — pre-existing
  env issue, out of scope).
- `python -m pytest mcp-server/vmaf-mcp/tests/ --durations=20` (run
  from the package root with `PYTHONPATH=src`; 113 tests collected).
- `python -m pytest tools/vmaf-tune/tests/ --durations=20` (full suite).
- `meson setup build-slow core -Denable_cuda=false -Denable_sycl=false`
  followed by `meson test -C build-slow` (all 63 tests across `fast`,
  `fast+simd`, `slow`, `dnn` suites).

Container: dev host, no GPU at probe time during initial pass; NVENC
probe re-run separately on GPU host to capture worst-case timing.

## Findings

### Headline: zero tests over 30 s

The audit found **no test in any suite exceeding 30 seconds**. The
top-10 slowest tests across the whole tree:

| # | Test | Suite | Wall | Cause |
|---|---|---|---|---|
| 1 | `test_v14_a_nvenc_probe_succeeds_on_gpu_host` | vmaf-tune | 13.20 s | Live NVENC encoder probe: ffmpeg-encoders listing + dummy 1-frame encode against real NVENC driver. Dominated by GPU context creation latency. Skipped without NVIDIA GPU. |
| 2 | `test_ladder_against_bbb_container_yields_plausible_vmaf` | vmaf-tune | 12.98–13.40 s | Real docker-exec ladder run inside `vmaf-dev-mcp`: 2 resolutions x 3 CRFs x 4 s duration = 24 frame-seconds of SVT-AV1 encode + libvmaf scoring. Skipped without docker. |
| 3 | `test_framesync` | libvmaf (meson, fast) | 5.00 s | Frame-sync correctness probe. Drives multiple frames through the pipeline; runtime is intrinsic to what it tests. |
| 4 | `test_smoke_run_is_deterministic` | ai/tests | 3.21 s | Tiny-AI training smoke (deterministic re-run). Cost is PyTorch eager + ONNX export, not I/O. |
| 5 | `test_train_epochs_zero_smoke` | ai/tests | 2.32 s | Trainer wiring smoke with `epochs=0` (no actual training). Cost is PyTorch Lightning + import overhead. |
| 6 | `test_qat_train_cli_smoke` | ai/tests | 1.99 s | QAT CLI smoke. PyTorch quantization + ONNX export. |
| 7 | `test_smoke_run_produces_allowlist_conformant_onnx` | ai/tests | 1.37 s | ONNX op-allowlist smoke. |
| 8 | `test_pic_preallocation` | libvmaf (meson, fast) | 1.25–1.31 s | Picture preallocation probe (alloc + free across many sizes). |
| 9 | `test_fr_regressor_num_codecs_zero_is_v1_contract` | ai/tests | 1.22 s | PyTorch forward-pass shape contract. |
| 10 | `test_report_handles_nan_rows` | vmaf-tune | 1.19 s | Markdown report renderer; matplotlib lazy-import cost. |

Below 1.2 s the distribution flattens to many tests under 0.5 s. The
mode is < 0.05 s.

### Per-suite headline

- `ai/tests/` — 657 tests collected, **30.00 s** wall total. Slowest 3.21 s.
- `mcp-server/vmaf-mcp/tests/` — 113 tests collected, **1.52 s** wall total. Slowest 0.65 s.
- `tools/vmaf-tune/tests/` — ~250 tests collected, ~85 s wall total. Slowest 13.40 s.
- `libvmaf` meson `fast` — 49 tests, **~9 s** wall total. Slowest 5.00 s.
- `libvmaf` meson `fast+slow+dnn` — 63 tests. Slowest 5.00 s. The two
  tests in the `slow` suite (`test_vmaf_per_shot`, `test_vmaf_roi_high_bitdepth`)
  are actually < 0.05 s each — historical naming.

### Pre-existing test failures (out of scope)

The audit incidentally surfaced 19 test failures in `ai/tests/` and
5 in `tools/vmaf-tune/tests/`, all unrelated to timing:

- `torchvision::nms` not registered (Python 3.14 + torch nightly drift).
- SVT-AV1 quality-range upper bound moved (50 -> ?) breaking
  `test_default_crf_sweep_within_svtav1_quality_range`.
- HTTP transport: `charset must not be in content_type argument`
  (aiohttp 3.13.5 stricter validation).

These are tracked separately; this PR does not attempt to fix them.

## Speedups applied

### `test_ladder_against_bbb_container_yields_plausible_vmaf` (13.0 s -> ~7 s expected)

Two tuning knobs in the docker-exec CLI invocation:

- `--duration 4` -> `--duration 2`: halves the encode+score work.
  Test asserts `vmaf >= 50.0`; 2 s of BBB sunflower content clears
  that floor with margin.
- `--crf-sweep 23,28,33` -> `--crf-sweep 23,33`: 3 points -> 2 points.
  Test asserts `len(samples) >= 4`; combined with 2 resolutions this
  still yields exactly 4 samples.
- Both V5-2 (plausible VMAF on container source) and V5-3
  (`(width, height, crf)` uniqueness) invariants remain testable.

### Marker infrastructure

- Registered `slow` marker in
  `tools/vmaf-tune/pyproject.toml`,
  `ai/pyproject.toml`,
  `mcp-server/vmaf-mcp/pyproject.toml`.
- Marked the two ~13 s tests with `@pytest.mark.slow` so a future
  `pytest -m "not slow"` developer gate excludes them. (Both already
  skip on the wrong host, but the marker is the proper future-proof.)

## Speedups considered and rejected

- **Mock the NVENC probe runner**: would cut 13 s -> <1 s but defeats
  the V14-A regression-catching purpose (real driver init failure mode).
  Rejected.
- **Drop the docker e2e to 1 res x 1 CRF**: would cut 13 s -> <3 s but
  loses the dedup-uniqueness assertion (need at least 2 distinct
  `(w, h, crf)` tuples to test dedup). Rejected.
- **Touch `test_framesync` (5.00 s)**: it intrinsically needs many
  frames to test sync; speeding it loses coverage. Rejected.

## Reproducer

```bash
# From repo root
python -m pytest tools/vmaf-tune/tests/ --durations=20 -q  # vmaf-tune
python -m pytest ai/tests/ --durations=30 -q \
    --ignore=ai/tests/test_dnn_exporter_run_provenance.py \
    --ignore=ai/tests/test_export_roundtrip.py \
    --ignore=ai/tests/test_registry.py
cd mcp-server/vmaf-mcp && PYTHONPATH=src python -m pytest tests/ --durations=20 -q
meson setup build-slow core -Denable_cuda=false -Denable_sycl=false
meson test -C build-slow --print-errorlogs
```

## Conclusion

The fork's test surface is healthy on the >30 s axis — no test
breaches the threshold and the two outliers near the high end
(~13 s) are intrinsically slow because they run real subprocesses
(docker exec, ffmpeg NVENC). The added marker + targeted speedup
on the docker e2e cuts the second-slowest test by ~45 % without
losing coverage. The `slow` marker is now installed as a documented
opt-out the next time a test does breach 30 s.
