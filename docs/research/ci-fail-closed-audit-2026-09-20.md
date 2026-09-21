# CI fail-closed audit (2026-09-20)

## Scope

The whole-tree standards policy requires failures to remain visible regardless
of whether the failing code originated in the fork, Netflix/vmaf, or a vendored
tree. Research-2027 already identified several workflow escape hatches. This
audit rechecked the executable paths rather than accepting their comments as
proof.

## Findings

| Surface | Previous behavior | Failure hidden |
| --- | --- | --- |
| Python tox coverage | `ignore_outcome = true` plus three coverage `-i` flags | Missing data and report errors could not fail tox. |
| CPU coverage pytest | Command ended with `\|\| true` | Any test failure or timeout still produced a green step. |
| Nightly Netflix benchmark | Command ended with `\|\| true` | Benchmark regressions and harness failures were indistinguishable from success. |
| Advisory Semgrep registry scan | `continue-on-error` plus `\|\| true` | The command reported success, so even the advisory step lost its failure outcome. |
| Sanitizer test discovery | A three-process pipeline disabled `pipefail` and ended in `\|\| true` | Meson or JSON parsing failure became an empty filtered list instead of preserving the producer error. |
| Cross-backend workflow job | Job-level `if: false` | The dead CPU-only placeholder could never report evidence despite its GPU-oriented name. |
| GPU coverage job | A stale merge restored `continue-on-error` after its recorded 2026-06-02 promotion | GPU test and threshold failures no longer blocked the train. |

## Resolution

Tox now orders coverage after `py314` and lets combination and report failures
propagate. Test and benchmark commands return their real status. The CPU
coverage pytest step may continue only long enough to gather and upload its
diagnostics; a final step checks the raw step outcome and fails the job. The
Semgrep registry scan remains advisory under the existing policy, but its step
now records a failed outcome. Sanitizer discovery filters names inside the JSON
consumer while Bash `pipefail` remains active. The permanently disabled and
misnamed cross-backend placeholder was removed; hardware parity remains owned
by the active backend-specific lanes. GPU coverage is again a required named
context, matching its existing promotion record and the elapsed stability
window.

`scripts/ci/test_fail_closed_ci.py` protects each behavior and is called by the
required rules workflow and local pre-commit/pre-push hooks.

## Alternatives considered

| Option | Result |
| --- | --- |
| Keep permissive exits and inspect artifacts manually | Rejected: green no longer means the command succeeded. |
| Stop immediately on the CPU pytest failure | Rejected: it would discard useful coverage diagnostics. |
| Preserve diagnostics, then reassert the original outcome | Selected: artifacts remain available without changing the job result. |

## Verification

```text
python3 scripts/ci/test_fail_closed_ci.py
pre-commit run fail-closed-ci-contract --all-files
actionlint .github/workflows/tests-and-quality-gates.yml \
  .github/workflows/nightly.yml .github/workflows/security-scans.yml \
  .github/workflows/sanitizers.yml .github/workflows/rule-enforcement.yml
```
