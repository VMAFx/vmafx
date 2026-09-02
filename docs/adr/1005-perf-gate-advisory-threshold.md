<!-- markdownlint-disable MD013 MD060 -->
# ADR-1005: Perf Gate Advisory Mode and Baseline Refresh Documentation

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `ci`, `perf`

## Context

The wall-clock perf regression gate introduced in ADR-0907 has been failing
chronically in CI. Diagnosis showed two root causes:

1. The committed baseline (`testdata/perf_multi_resolution.json`) was recorded
   on an AMD Ryzen 9 9950X3D workstation. GitHub Actions ubuntu-latest runners
   use 2-core virtual machines that are typically 5–15x slower. Any
   `--tolerance-pct 5` comparison against that baseline will always flag
   regressions — not because the code regressed, but because the hardware
   is different.

2. The `perf-regression` job depends on `netflix-golden` (to avoid wasting runner
   time when the build is broken). When the build fails, the Perf job is skipped;
   when the build succeeds, the timing comparison against a dev-machine baseline
   will fail. The gate has therefore never produced a meaningful result.

`continue-on-error: true` means the Perf gate does not block merges, but it
does add noise to CI and makes the "advisory" intent opaque.

## Decision

We will add an `--advisory` flag to `scripts/perf/check-regression.py` that
prints the full regression report but always exits 0. The CI workflow's
"Check regression" step will pass `--advisory` until a CI-runner-calibrated
baseline is committed. This makes the advisory-only intent explicit in the
workflow, eliminates the chronic non-zero exit from the comparison step, and
keeps the instrumentation running so that data accumulates toward a future
CI-calibrated baseline.

We will also add a `--skip-if-no-baseline` flag that makes the script exit 0
with an informational message when the baseline file is empty or has no `ok`
cells for the requested backend. This lets a future operator commit an empty
seed baseline without triggering false positives during the first run cycle.

Documentation for baseline refresh is added to
`docs/development/perf-gate.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Remove the tolerance check entirely | Silences noise immediately | Destroys the instrumentation; no data for a future calibrated baseline | Discards ADR-0907 investment |
| Record a new baseline on a GitHub Actions runner | Gate becomes meaningful immediately | Requires a manual CI-triggered run + PR merge cycle; runner hardware varies | Valid follow-up; not a blocker today |
| Raise `--tolerance-pct` to 2000% | Keeps current invocation; avoids exit 1 | Semantically wrong; hides the advisory intent; makes future calibration harder | Rejected |
| Remove `needs: netflix-golden` dependency | Perf job runs even when build is broken | Wastes runner minutes on broken builds | Relationship is correct; keep |

## Consequences

- **Positive**: Perf CI step no longer exits non-zero, removing chronic noise.
  The full regression report is still printed so reviewers can see timing trends
  even before a CI-calibrated baseline exists.
- **Negative**: The gate is advisory-only until a CI-runner-calibrated baseline
  is committed. A true regression will not block a PR during this period.
- **Neutral / follow-ups**: Once master CI is green and the perf bench has
  accumulated 3+ runs on GitHub Actions runners, regenerate the baseline using
  the artifact uploaded by the `perf-regression` job and commit it. At that
  point remove `--advisory` from the workflow step to promote the gate to
  blocking (requires a follow-up ADR superseding ADR-0907 to adjust the
  tolerance given GitHub Actions variance).

## References

- ADR-0907 — Wall-clock perf regression gate (original gate introduction).
- `testdata/perf_multi_resolution.json` — committed baseline (dev-machine timings).
- `scripts/perf/check-regression.py` — gate script modified by this ADR.
- `.github/workflows/tests-and-quality-gates.yml` — `perf-regression` job.
- `docs/development/perf-gate.md` — new operator guide added by this ADR.
- req: "Perf regression gate (CPU wall-clock, ADR-0907) has been failing chronically — there's likely no baseline registered. … Lower the alert threshold to advisory if baseline is empty/stale to unblock CI."
