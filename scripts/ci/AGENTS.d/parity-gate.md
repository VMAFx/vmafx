---
paths:
  - scripts/ci/cross_backend_*.py
  - scripts/ci/gpu_ulp_calibration.yaml
  - scripts/ci/test_calibration.py
  - scripts/ci/test_cross_backend_*.py
  - scripts/ci/exact_twins.d/*
invariant: `tolerance_for()` falls back to the default; a cell compares every metric; the gate stays stdlib-only (macOS bundle).
---
<!-- markdownlint-disable MD013 MD060 -->
# Cross-backend parity gate: calibration table and lanes

## Calibration table contract (ADR-0234)

`gpu_ulp_calibration.yaml` = single source of truth for
per-GPU-generation tolerance overrides on cross-backend parity
gate. Lookup contract:

1. Caller passes `--gpu-id <runtime_id>` to the gate. Current executable
   backend identifiers are `cuda:M.m` and `sycl:0xVVVV:DRIVER`. The table
   retains historical Vulkan rows for old reports, but ADR-0726 removed
   Vulkan from the gate's backend choices.
2. Loader picks most-specific glob match (longest non-
   wildcard prefix wins; trailing `*` supported).
3. If row has `features:` override for cell, that wins.
   Else gate falls back to built-in
   `FEATURE_TOLERANCE` default (preserving backward compatibility
   for every caller pre-dating ADR-0234).
4. If `--gpu-id` omitted, no calibration consulted at all
   (legacy behaviour exact).

**Invariant**: `tolerance_for(feature, gpu_id, default)` returns
`default` whenever any resolution step above falls through.
Enforced by `test_calibration.py`. Future PR
"optimising" lookup must keep all four fallback paths intact, or
existing CI lanes not passing `--gpu-id` will silently change
behaviour.

**Area-scaled features (ADR-1361).** The resolved tolerance (table
default or calibration row) = contract at 576x324. For features in
`AREA_SCALED_FEATURES` (`psnr_hvs`: CPU sums a whole plane in one
float) both gates multiply it by `area_tolerance_factor(feature, w, h)`
= √(N / N₅₇₆ₓ₃₂₄) above the reference term count, 1 at or below it.
Never loosens or tightens small fixtures. FP16 contract stays
absolute. Both gates compare metrics through `metric_delta()`: JSON
`null` (non-finite score) on both sides = agreement, on one side =
mismatch. Keep both helpers in `cross_backend_calibration.py`, the
module both gates import; tests in `test_cross_backend_parity_gate.py`.

**Missing metric = cell ERROR (ADR-1418).** Cell compares every metric
in `FEATURE_METRICS[feature]`; never a common subset. Metric absent
from any frame of either run -> `missing_metrics()` names it, cell
`ERROR` (gate fails), matrix continues; `cross_backend_vif_diff.py`
prints `FAIL: missing metrics` and exits 1. `motion` = default runs
(`VMAF_integer_feature_motion_sad_score`, `integer_motion2`,
`integer_motion3`); `motion_debug` = `debug=true` both sides, adds
`integer_motion`. SAD score = what CPU `extract()` appends every frame,
source of motion2 / motion3; in both cells since
`T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02` (twin without it = cell
ERROR). `motion_mffw` / `motion_v2_mffw` = `motion` / `motion_v2` with
`motion_five_frame_window=true:motion_moving_average=true` (HFR models'
option set, ADR-1491): SAD + motion2 + motion3 under `_mffw_mma` names,
exact on `cuda`, `sycl`, `hip`. A twin that loses the window either errors
(`-ENOTSUP` / unknown option) -> cell ERROR, or scores three-frame -> cell
FAIL; never drop the cells. Same tuples in `cross_backend_vif_diff.py`:
change both. Twin
emits a different default set -> fix the twin's option default
(`test_sycl_twin_option_parity.c`), never shrink the cell.

**Every registered twin = gate cell (ADR-1460).** Twin registered in
`core/src/feature/feature_extractor.cpp` for `cuda` / `sycl` / `hip` must be
extractor of some `FEATURE_METRICS` key (via `FEATURE_ALIASES` +
`BACKEND_EXTRACTOR_ALIASES`). New twin -> add feature to `FEATURE_METRICS`
in BOTH `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py`
(same tables, held equal), metrics = everything CPU extractor emits by
default (`psnr` = three planes), tolerance in `FEATURE_TOLERANCE`, row in
`docs/development/cross-backend-gate.md`. Backend with registered twins but
no gate backend = listed in `UNGATED_BACKENDS` with state row (empty since
`metal` became a gate backend, ADR-1496).
Guard: `core/test/test_parity_gate_covers_registered_twins.py`.
`speed_temporal` = libm twin on `cuda`, `hip`, `sycl` at `4e-5` (five
float steps below 128; twins round `log2` correctly, CPU = host `log2f`).
`speed_chroma` = libm twin on `cuda`, `hip`, `sycl` at `5e-6`. SYCL twin ==
icx-build CPU on all measured (Intel `log2f` rounds correctly), != glibc CPU
on 15 of 918: NEVER declare it exact from an icx run; equality = property of
host libm, not of twin.

**Metal backend + `--hold-exact` (ADR-1496).** `metal` = gate backend
(`--metal_device`; `integer_*_metal` names via `BACKEND_EXTRACTOR_ALIASES`, same
tuples in both gate scripts). Runs only on an Apple device = macOS tester
bundle (`tools/rc1-tester/src/vmaf_rc1_tester/hw_gate.py`): `--backends cpu
metal --hold-exact metal` on four fixtures. `--hold-exact B` = cells with B
compared at 0 + `--precision max` (ciede: max `LIBM_TWINS` bound), label
`held-exact:ADR-1496`; other side must be `cpu`, held, or listed for the
feature. Measurement before a fragment; NEVER a fragment substitute, never in
a CI lane. Bundle copies the gate: stdlib imports only, files =
`GATE_FILES` + `exact_twins.d` + cited ADRs (`prepare_build.py stage_gate`);
new import or data file -> add there or the bundle's gate run errors.
Gate features run on Metal = `gate.features` in
`tools/rc1-tester/image/metal-rows.json` (held equal to the features with a
registered Metal twin by `core/test/test_metal_report_rows_contract.py`).

## When adding a new lane

1. New `--feature` value → add to `FEATURE_METRICS` in *both*
   gate scripts (single source of truth lives in parity gate;
   per-feature script mirrors it). Add it to a workflow only when that lane
   owns executable hardware coverage; the current consumer is
   `sycl-parity.yml`.
2. New backend → extend `BACKEND_SUFFIX`, `BACKEND_DEVICE_FLAG`,
   `BACKEND_DEFAULT_DEVICE`, tests, and user documentation. Script support
   alone is not CI coverage.
3. New GPU arch → add row to `gpu_ulp_calibration.yaml`. Mark it
   `status: placeholder` until real-hardware corpus exists;
   placeholder row operationally no-op (empty `features:`
   block).

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `cross_backend_vif_diff.py` | `tests-and-quality-gates.yml` — every `*-cross-backend-diff` step | The `--feature`, `--backend`, `--places` flag names; the `FEATURE_METRICS` dict (workflow steps reference feature names verbatim). |
| `cross_backend_parity_gate.py` | `sycl-parity.yml` — `Run cross-backend parity gate (calibrated features)` | The `--gpu-id`, `--calibration-table`, `--backends`, `--features`, `--fp16-features`, `--json-out`, and `--md-out` flag names. The current CI consumer is CPU↔SYCL `float_ssim` on Arc A380. `SYCL Parity (Arc A380)` is conditionally required through the lane switch; the removed Vulkan matrix job must not be documented as current coverage. |
| `cross_backend_calibration.py` | (loader, not invoked directly by workflow) | Imported by the two gate scripts via `sys.path.insert(0, …)`; lives next to them on purpose. Don't move it without updating the import sites. |
| `gpu_ulp_calibration.yaml` | (data, not invoked directly by workflow) | The default path is hard-coded as `Path(__file__).parent / "gpu_ulp_calibration.yaml"` in `cross_backend_calibration.DEFAULT_CALIBRATION_PATH`. Renaming this file is a breaking change for the gate scripts and any caller that didn't pass `--calibration-table` explicitly. |
| `test_calibration.py` | `tests-and-quality-gates.yml` — pytest collection (`pytest-tests` lane) | Discovered automatically by pytest; the test module name is part of the gate's contract. |
