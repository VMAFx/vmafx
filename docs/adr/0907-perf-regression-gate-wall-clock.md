<!-- markdownlint-disable MD013 MD060 -->
# ADR-0907: Wall-clock perf regression gate over the multi-resolution baseline

- **Status**: Proposed
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: ci, performance, regression-gate, fork-local, testing

## Context

The fork ships multiple benchmark harnesses but no automated wall-clock
regression gate:

- `testdata/bench_all.sh` (ADR-0513) compares per-backend VMAF *scores* on
  three canonical fixtures. Its in-script `compare()` checks numerical
  correctness (max_diff < 0.01), not wall time.
- `testdata/bench_perf.py` (ADR-0429) is explicitly "operator-facing, not a
  CI gate".
- `testdata/benchmark_netflix.py` regenerates the
  `netflix_benchmark_results.json` snapshot but ADR-0001 classifies that
  snapshot as "noise" — not a tracked baseline.
- `scripts/perf/bench-multi-resolution.sh` (ADR-0752) writes a versioned
  baseline at `testdata/perf_multi_resolution.json` (schema_version=1,
  hardware-tagged, 50 cells). Its README and ADR say "future perf PRs must
  re-run the script and include a diff in the PR description" — **manual
  inspection, no enforcement.**
- `.github/workflows/tests-and-quality-gates.yml` invokes
  `./testdata/bench_all.sh --backend=cpu --snapshot-only --tolerance-ulp=2`
  but `bench_all.sh` does not parse any of those flags; the run is
  effectively a no-op gate.
- `.github/workflows/nightly.yml` runs `bash testdata/bench_all.sh || true`
  and uploads `netflix_benchmark_results.json` as an artefact, never
  asserting anything.

Net effect: a wall-clock regression in any CPU/SYCL/CUDA path can land on
master without any signal. The closest existing gate is ADR-0164's
SSIMULACRA 2 numerical snapshot gate, which is correctness-only.

## Decision

We will ship a minimal wall-clock perf regression gate over the
ADR-0752 multi-resolution baseline:

1. **Gate script** at `scripts/perf/check-regression.py` — a single
   Python file (no new third-party deps; stdlib only) that loads the
   committed `testdata/perf_multi_resolution.json` baseline and a
   freshly-produced run JSON, joins them by `(resolution, backend,
   metric)`, and exits non-zero when any cell's `median_ms` exceeds the
   baseline by more than `--tolerance-pct` (default 5%). Cells with
   `status != "ok"` in either side are reported and skipped, not
   failed, so missing GPU lanes do not block CPU-only CI.
2. **Wire it into `tests-and-quality-gates.yml`** — replace the broken
   `bench_all.sh --backend=cpu --snapshot-only --tolerance-ulp=2`
   invocation (the flags are silently ignored today) with a CPU-only
   run of `bench-multi-resolution.sh` against the baseline. The job
   stays optional (`continue-on-error: true`) for one release cycle so
   we can collect cross-runner variance data before flipping it into
   the required-check aggregator.
3. **5% wall-clock tolerance** as the default. Single-cell runs on
   GitHub-hosted Ubuntu show typical CPU-time variance of ~2-3% across
   identical commits; 5% catches real regressions while absorbing
   measurement noise. The tolerance is a CLI flag so a future ADR can
   tighten it once we have a self-hosted bench runner with lower
   variance.
4. **Baseline lives in `testdata/perf_multi_resolution.json`** —
   already committed, already versioned. Intentional perf improvements
   regenerate it via `scripts/perf/bench-multi-resolution.sh` with the
   regen documented in the commit message (mirrors the `/regen-snapshots`
   discipline for SSIMULACRA 2).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Gate script + CPU-only CI step (this ADR)** | Stdlib-only; reuses the existing baseline + harness; CPU-only keeps the gate runnable on stock GitHub Ubuntu; tolerance flag tunable as variance data accrues | Catches CPU regressions only at start; GPU lanes deferred to a follow-up self-hosted job | **Chosen.** Smallest viable gate; closes the "any wall-clock regression can land silently" gap immediately |
| Status-quo (manual diff in PR descriptions per ADR-0752) | No infra work | Relies on perfect human discipline; ADR-0752 has been live since 2026-05-29 and no PR has actually pasted a diff | Drops on the floor in practice |
| Fix `bench_all.sh` to honour the `--snapshot-only --tolerance-ulp=N` flags it is already invoked with | Closes the broken-CI-invocation foot-gun directly | `tolerance-ulp` is a *numerical* tolerance, not a wall-clock one; would gate on score parity that the SSIMULACRA 2 + Netflix golden gates already cover; doesn't address the wall-clock gap | Wrong tool — would conflate two orthogonal gates |
| Full GPU matrix gate (CUDA + SYCL + HIP in CI) | Catches GPU regressions too | Requires self-hosted GPU runners; cross-runner variance is much higher than CPU; would block on infra not yet in place | Out of scope for the first iteration; follow-up ADR |
| External tool (e.g. `pyperformance`, `bencher.dev`) | Off-the-shelf statistical machinery | New dependency; SaaS integration; doesn't read our JSON shape; pinning + supply-chain review cost dwarfs the value at this scale | Overkill; ~120 LOC of stdlib Python covers our shape |

## Consequences

### Positive

- A wall-clock regression > 5% on any tracked CPU cell now turns the
  CI step red (after the one-cycle `continue-on-error` warm-up).
- The broken `bench_all.sh --snapshot-only` invocation in
  `tests-and-quality-gates.yml` is replaced with a real gate.
- The baseline file (`testdata/perf_multi_resolution.json`) now has a
  consumer in CI, not just operator-facing inspection.

### Negative

- One CI step adds ~3-5 min of wall time per PR run (CPU-only, five
  resolutions × five metrics, 3 runs each at median ~74 ms per cell).
- Operators who intentionally regress perf for a correctness fix must
  regenerate the baseline in the same PR (same discipline as
  ADR-0752 already implies).

### Neutral / follow-ups

- Promote the step from `continue-on-error: true` to required-check
  after one release cycle of variance data.
- A follow-up ADR will add a self-hosted GPU lane (CUDA + SYCL) once
  the runner pool stabilises. The script is backend-agnostic by design.
- The gate intentionally **does not** read `netflix_benchmark_results.json`
  — ADR-0001 already classified that snapshot as noise.

## References

- [ADR-0001](0001-stash-benchmark-noise-file.md) — netflix_benchmark_results.json is noise, not a baseline.
- [ADR-0429](0429-testdata-bench-perf-portability.md) — bench_perf.py is operator-facing, not a CI gate.
- [ADR-0513](0513-per-shot-scene-threshold-and-1-shot-chart.md) — bench_all.sh skip-on-missing-backend behaviour.
- [ADR-0752](0752-perf-bench-multi-resolution.md) — multi-resolution baseline this gate consumes.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — deep-dive deliverables for this PR.
- [ADR-0164](0164-ssimulacra2-snapshot-gate.md) — precedent for a snapshot-based regression gate.
- Source: `req` — user direction to "audit existing benchmark infrastructure — is there a performance regression gate? If not, propose one" with a "±5% wall-clock regression gate" target.
