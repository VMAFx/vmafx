<!-- markdownlint-disable MD013 MD024 MD060 -->
# Coverage Gate

The Coverage Gate fails a pull request when line coverage of the CPU build falls below a floor. Two required checks run it: `Coverage Gate` (hosted CPU build) and `Coverage GPU` (self-hosted GPU build). Both are defined in [`.github/workflows/tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml) and listed in the `Required Checks Aggregator` ([ADR-1297](../adr/1297-ci-gate-every-reporting-check.md)). The floors come from [`docs/principles.md`](../principles.md) §3; the ratchet rules are [ADR-0922](../adr/0922-coverage-ratchet-aggressive.md).

## When you need this page

A PR shows `Coverage Gate` or `Coverage GPU` red, or you want to raise or lower a floor. To reproduce a failure, run the commands in [Debugging a failing gate](#debugging-a-failing-gate).

## What the gate checks

[`scripts/ci/coverage-check.sh`](../../scripts/ci/coverage-check.sh) takes a gcovr JSON summary and two percentages:

```text
scripts/ci/coverage-check.sh <gcovr-summary.json> <overall_min%> <critical_min%>
```

1. Overall line coverage, the unweighted average over every file in the summary, must reach the overall minimum. Exit code 1 on failure.
2. Every security-critical file must reach the critical minimum, unless it has an entry in the `PER_FILE_MIN` map of the script. Security-critical files match `*core/src/dnn/*`, `*core/src/opt.cpp` and `*core/src/read_json_model.cpp`. A critical file with no executed lines is listed but not enforced.

The script defaults are 70 % overall and 90 % critical. The workflow passes its own values:

| Job | Invocation in the workflow | Overall | Critical |
| --- | --- | --- | --- |
| `Coverage Gate` (`coverage`) | `coverage-check.sh core/build-coverage/coverage.json 37 85` | 37 % | 85 % |
| `Coverage GPU` (`coverage-gpu`) | `coverage-check.sh core/build-coverage-gpu/coverage.json 70 85` | 70 % | 85 % |

The CPU job's 37 % overall floor tracks the measured value after the 2026-05-19 merge burst of new scaffold code, as the comment above the step records (ADR-0637). The per-file values in `PER_FILE_MIN` apply in both jobs.

## The Python suite runs in four shards

Before gcovr collects coverage, the `Coverage Gate` job runs the Python suite under `python/test/` against the instrumented `libvmaf.so` and `vmaf` CLI. The suite takes about 33 minutes on a hosted runner, and praetor's test-timeout audit refuses a test step above 30 minutes, so it runs as four steps, `Run Python test suite, shard 1/4` to `4/4`, each under `timeout 30m`.

[`scripts/ci/run_pytest_shard.py`](../../scripts/ci/run_pytest_shard.py) selects a shard: it runs pytest in its own process with one more plugin that, after `-m`, `-k`, `--deselect` and `--ignore` have filtered the collection, keeps every fourth item starting at item I and reports the others as deselected. The four shards together run every selected test once. gcov counters add up across processes, so the shards need no coverage merge. Each shard step continues on error; the step `Assert every Python coverage shard passed` then fails the job and names each shard whose outcome was not `success`. To run one shard locally:

```text
python3 scripts/ci/run_pytest_shard.py --shard 2/4 -- python/test/ -v --timeout=600
```

An empty shard exits 5 and a bad `--shard` exits 2, so a shard count larger than the suite fails instead of passing on nothing.

The `coverage` job skips itself in-job when `scripts/ci/plan-ci-impact.py` ([ADR-1140](../adr/1140-ci-impact-planner.md)) reports no impacted source, so the check always reports and `skipped` is never its outcome.

### Per-file overrides

Some files have a structural ceiling below the critical floor. They live in the `PER_FILE_MIN` map of `coverage-check.sh`, and each entry cites the ADR that justifies it.

| File | Floor | Justification |
| --- | --- | --- |
| `core/src/dnn/ort_backend.c` | 83 % | [ADR-0114](../adr/0114-coverage-gate-per-file-overrides.md): the CUDA execution-provider success arm is unreachable on a CPU-only ONNX Runtime. |
| `core/src/dnn/dnn_api.c` | 83 % | ADR-0114: same ceiling as `ort_backend.c`. |
| `core/src/dnn/tiny_extractor_template.h` | 75 % | [ADR-0881](../adr/0881-coverage-overrides-audit-2026-05-30.md): template helpers are only instantiated by callers. |

Overrides only ratchet upward.

### The per-PR delta script

[`scripts/ci/coverage-delta-check.sh`](../../scripts/ci/coverage-delta-check.sh) compares a base and a head gcovr summary and fails when overall coverage, or a touched file's coverage, drops by more than a tolerance (0.5 percentage points by default). Exit code 1 means the overall drop, 2 a per-file drop, 3 a usage error. ADR-0922 introduced it, but no step of `tests-and-quality-gates.yml` calls it today, so it is a local tool, not a required check. Run it by hand:

```bash
scripts/ci/coverage-delta-check.sh \
  --base-json /path/to/base-coverage.json \
  --head-json core/build-coverage/coverage.json \
  --changed-files "$(git diff --name-only origin/master...HEAD)"
```

The options are `--base-json`, `--head-json`, `--changed-files`, `--max-overall-drop` and `--max-file-drop`; read the header of the script for the exact input format.

## Debugging a failing gate

The `coverage-cpu` and `coverage-gpu` artifacts of the failed run hold `coverage.json`, `coverage.txt` and `coverage.xml`. `coverage.txt` lists per-file numbers. To check a downloaded summary:

```bash
scripts/ci/coverage-check.sh coverage.json 37 85
```

To rebuild what the `Coverage Gate` job builds (needs `gcovr`):

```bash
cd core
meson setup build-coverage --buildtype=debug \
  -Db_coverage=true -Denable_cuda=false -Denable_sycl=false \
  -Denable_float=true -Denable_avx512=true -Denable_dnn=enabled \
  -Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic
ninja -C build-coverage
python3 ../scripts/ci/run_meson_test.py -- -C build-coverage --print-errorlogs --num-processes 1
gcovr --root .. \
    --filter 'src/.*' \
    --exclude '.*/test/.*' --exclude '.*/tests/.*' --exclude '.*/subprojects/.*' \
    --gcov-ignore-parse-errors=negative_hits.warn \
    --gcov-ignore-parse-errors=suspicious_hits.warn \
    --json-summary build-coverage/coverage.json \
    --txt build-coverage/coverage.txt \
    build-coverage
../scripts/ci/coverage-check.sh build-coverage/coverage.json 37 85
```

`-fprofile-update=atomic` and `--num-processes 1` are both needed so test binaries do not corrupt each other's counters in the shared `libvmaf.so` ([ADR-0110](../adr/0110-coverage-gate-fprofile-update-atomic.md)). The CI job also runs a Python suite before collecting coverage; a local run without it measures lower on the DNN files.

`make coverage-check` runs the same recipe locally: it builds the instrumented tree in `build-coverage/`, runs the meson suite serially, writes the gcovr summary `build-coverage/coverage.json` and passes it to `coverage-check.sh` with the local floors `COVERAGE_MIN_OVERALL` (37) and `COVERAGE_MIN_CRITICAL` (85) from the Makefile. `make coverage-html` renders the gcovr HTML report. `coverage-check.sh` exits 2 on an input that is not a gcovr JSON summary, an lcov `.info` file included. Like the CI job without its Python suite, a local run measures the DNN files lower.

## Raising a floor

Raising needs no ADR. Change the numbers in the `Enforce coverage thresholds` step of the workflow (or the value in `PER_FILE_MIN`), confirm that the latest master run is above the new floor, and leave a comment with the date and the measurement.

## Lowering a floor

Lowering any floor needs a new ADR that supersedes ADR-0922 (and ADR-0114 for a per-file override):

1. Allocate it with `scripts/adr/next-free.sh --claim <slug>`. State why the lower floor is unavoidable and what mitigates the loss of coverage.
2. Cite the ADR inline at the changed number, for example `# ADR-NNNN — supersedes ADR-0922, lowers floor to X%`. A bare number change is rejected in review.
3. Use `PER_FILE_MIN` for a per-file exception. No other suppression mechanism exists.

## Related ADRs

| ADR | Topic |
| --- | --- |
| [ADR-0110](../adr/0110-coverage-gate-fprofile-update-atomic.md) | Atomic gcov counters; foundation of the gate. |
| [ADR-0111](../adr/0111-coverage-gate-gcovr-with-ort.md) | Move from lcov to gcovr, so per-file numbers are honest. |
| [ADR-0114](../adr/0114-coverage-gate-per-file-overrides.md) | `PER_FILE_MIN` map and the structural-ceiling rationale. |
| [ADR-0117](../adr/0117-coverage-gate-warning-noise-suppression.md) | gcovr suspicious-hits warning filter. |
| [ADR-0637](../adr/0637-ci-test-failures-omnibus.md) | Floor tracks the measured value, ratcheted upward as tests land. |
| [ADR-0881](../adr/0881-coverage-overrides-audit-2026-05-30.md) | `tiny_extractor_template.h` floor rationale. |
| [ADR-0922](../adr/0922-coverage-ratchet-aggressive.md) | Ratchet rules and the delta script. |
