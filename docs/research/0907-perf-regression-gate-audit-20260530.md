<!-- markdownlint-disable MD013 MD056 MD060 -->
# Research 0907 — Perf regression gate audit (2026-05-30)

## Question

Does the VMAFx fork have a wall-clock performance regression gate? If
not, what's the minimum viable shape?

## Method

Inspected every `bench*` / `*perf*` script under `scripts/perf`,
`testdata/`, `tools/`, every workflow under `.github/workflows/`, and
every committed baseline JSON under `testdata/`. Cross-referenced
against the existing ADR corpus (search for "perf gate", "regression
gate", "wall-clock").

## Findings

### Benchmark scripts that exist

| Script | Purpose | CI? |
|---|---|---|
| `testdata/bench_all.sh` (ADR-0513) | Per-backend VMAF *score* parity gate (max_diff < 0.01) on 3 fixtures | Nightly artefact only; gate logic is correctness-oriented, not wall-clock |
| `testdata/bench_perf.py` (ADR-0429) | FFmpeg lavfi wall-clock harness | **Explicitly "operator-facing, not a CI gate"** per ADR-0429 |
| `testdata/benchmark_netflix.py` | Regenerates `netflix_benchmark_results.json` snapshot | Snapshot classified as **noise** per ADR-0001 |
| `testdata/bench_quick.py` | Quick smoke | Not in CI |
| `scripts/perf/bench-multi-resolution.sh` (ADR-0752) | Multi-resolution wall-clock baseline producer | **No CI consumer** — ADR-0752 says "future perf PRs must re-run the script and include a diff in the PR description" (manual) |
| `tools/vmaf-tune/src/vmaftune/benchmark.py` | vmaf-tune corpus benchmark | Internal to vmaf-tune |

### CI invocations of bench scripts

| Workflow | Step | Status |
|---|---|---|
| `nightly.yml` | `bash testdata/bench_all.sh || true` then upload artefact | No gate — `|| true` swallows any failure; artefact-only |
| `tests-and-quality-gates.yml` (job `cross-backend`) | `./testdata/bench_all.sh --backend=cpu --snapshot-only --tolerance-ulp=2` | **Job is `if: false` — disabled.** And even if enabled, `bench_all.sh` does not parse any of those flags. Silent no-op on both counts. |

### Baseline files

| File | Versioned? | Used by gate? |
|---|---|---|
| `testdata/perf_multi_resolution.json` | Yes (schema_version=1, hardware-tagged, 50 cells) | No — operator-only per ADR-0752 |
| `testdata/perf_benchmark_results.json` | Yes | No |
| `testdata/netflix_benchmark_results.json` | Noise per ADR-0001 | No |

### Conclusion

**There is no wall-clock perf regression gate.** ADR-0752 built the
versioned baseline but stopped short of wiring it into CI. The
`cross-backend` job in `tests-and-quality-gates.yml` referenced a
gate-shaped invocation but is disabled and the invocation itself was
broken (unparsed flags).

## Proposed gate

Per [ADR-0907](../adr/0907-perf-regression-gate-wall-clock.md):

1. `scripts/perf/check-regression.py` — stdlib-only gate that joins a
   fresh run against `testdata/perf_multi_resolution.json` by
   `(resolution, backend, metric)` and exits 1 when any cell
   regresses by > `--tolerance-pct` (default 5%).
2. New CI job `perf-regression` in `tests-and-quality-gates.yml`,
   CPU-only on `ubuntu-latest`, `continue-on-error: true` for one
   release cycle so cross-runner variance data informs the tolerance.
3. Smoke tests at `scripts/perf/test_check_regression.py` (9 tests,
   stdlib + pytest; all pass locally).

## Reproducer

```bash
# Run the gate locally against the committed baseline as a self-test:
python3 scripts/perf/check-regression.py \
  --baseline testdata/perf_multi_resolution.json \
  --current  testdata/perf_multi_resolution.json \
  --backend  cpu
# Exit 0; reports any status=skip cells.

# Run the smoke tests:
python3 -m pytest scripts/perf/test_check_regression.py -v
# 9 passed.
```

## References

- [ADR-0001](../adr/0001-stash-benchmark-noise-file.md)
- [ADR-0429](../adr/0429-testdata-bench-perf-portability.md)
- [ADR-0513](../adr/0513-per-shot-scene-threshold-and-1-shot-chart.md)
- [ADR-0752](../adr/0752-perf-bench-multi-resolution.md)
- [ADR-0164](../adr/0164-ssimulacra2-snapshot-gate.md) — precedent snapshot-based regression gate.
- [ADR-0907](../adr/0907-perf-regression-gate-wall-clock.md)
