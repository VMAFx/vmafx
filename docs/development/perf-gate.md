# Perf Gate Operator Guide

The wall-clock perf regression gate (ADR-0907) benchmarks VMAF at multiple
resolutions and compares the results against a committed baseline.  This page
explains how to read CI output, refresh the baseline, and promote the gate from
advisory to blocking.

## What the gate does

1. `scripts/perf/bench-multi-resolution.sh` runs the `vmaf` binary on several
   resolution-backend-metric combinations and writes timing results to
   `testdata/perf_multi_resolution.current.json` (uploaded as a CI artifact).
2. `scripts/perf/check-regression.py` joins the current results against the
   committed baseline (`testdata/perf_multi_resolution.json`) and reports any
   cell whose median wall time exceeds the baseline by more than the configured
   tolerance.
3. The artifact `perf-regression-current-run` is uploaded on every run so you
   can download it to inspect or promote to a new baseline.

## Current mode: advisory

The CI step runs with `--advisory --skip-if-no-baseline` (see ADR-1005).
This means:

- The step always exits 0, so the Perf job does not fail.
- The full regression report is still printed in the step log.
- `--skip-if-no-baseline` causes an early clean exit if the committed baseline
  contains no `ok` cells for the `cpu` backend — useful for the first run
  after an empty seed baseline is committed.

The gate is in advisory mode because the committed baseline was recorded on a
developer workstation (AMD Ryzen 9 9950X3D) that is 5–15x faster than the
GitHub Actions `ubuntu-latest` 2-core runners.  A hard 5% tolerance against
that baseline will always flag regressions unrelated to code changes.

## Refreshing the baseline

To generate a CI-runner-calibrated baseline:

1. Trigger a manual workflow run (`workflow_dispatch`) or wait for a normal CI
   run that passes the build step.
2. Download the `perf-regression-current-run` artifact from the
   `perf-regression` job.
3. Verify the downloaded JSON has `ok_cells > 0` and the `hardware.cpu_model`
   field matches a GitHub Actions runner (e.g. contains "QEMU" or "Intel(R)
   Xeon(R)").
4. Copy the artifact to `testdata/perf_multi_resolution.json` in a new branch.
5. Open a PR, get it reviewed, and merge.
6. Remove `--advisory` from the `check-regression.py` invocation in
   `.github/workflows/tests-and-quality-gates.yml` to promote the gate to
   blocking.  Update ADR-1005 status to Superseded and open a new ADR if the
   tolerance needs adjustment given GitHub Actions runner variance.

Alternatively, to record a baseline on the dev machine for local pre-push
checks only:

```bash
VMAF_BIN=core/build/tools/vmaf \
  scripts/perf/bench-multi-resolution.sh \
  --backends cpu \
  --runs 5 \
  --output testdata/perf_multi_resolution.json
```

Do not commit a dev-machine baseline as the CI baseline; the timing numbers
are not comparable across hardware.

## Running the gate locally

```bash
# Build first
meson setup core/build -Denable_cuda=false -Denable_sycl=false --buildtype=release
ninja -C core/build

# Benchmark
VMAF_BIN=core/build/tools/vmaf \
  scripts/perf/bench-multi-resolution.sh \
  --backends cpu \
  --runs 3 \
  --output /tmp/perf_current.json

# Compare against the committed baseline
python3 scripts/perf/check-regression.py \
  --baseline testdata/perf_multi_resolution.json \
  --current /tmp/perf_current.json \
  --tolerance-pct 5.0 \
  --backend cpu
```

Omit `--advisory` for a hard-fail run (useful for local pre-push checks on
the same hardware as the baseline).

## Extending the baseline to GPU backends

The current baseline and CI gate cover CPU only.  To add GPU cells:

1. Run `bench-multi-resolution.sh --backends cpu,cuda` on the dev machine.
2. Check that CUDA cells have `status: ok` in the output JSON.
3. Commit the JSON as the new baseline.
4. In the CI workflow, add a second invocation of `check-regression.py` with
   `--backend cuda` on a self-hosted GPU runner.  Wrap in `continue-on-error:
   true` until the GPU runner pool is stable.

## Relevant files

| File | Purpose |
| --- | --- |
| `testdata/perf_multi_resolution.json` | Committed baseline (refreshed manually) |
| `scripts/perf/bench-multi-resolution.sh` | Benchmark harness |
| `scripts/perf/check-regression.py` | Comparison and reporting script |
| `scripts/perf/test_check_regression.py` | Unit tests for the comparison script |
| `.github/workflows/tests-and-quality-gates.yml` | `perf-regression` CI job |

## See also

- ADR-0907 — Wall-clock perf regression gate (original decision)
- ADR-1005 — Advisory mode and baseline refresh documentation (this guide)
