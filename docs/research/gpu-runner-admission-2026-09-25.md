<!-- markdownlint-disable MD013 MD060 -->
# Research: fail-closed self-hosted GPU runner admission

**Date:** 2026-09-25
**Scope:** `T-GPU-RUNNER-LABEL-MISMATCH-2026-09-05` at collector
`a7f77d66997a1a718e7f32156ceb11b3af7786dc`
**Decision:** [ADR-1319](../adr/1319-fail-closed-self-hosted-gpu-admission.md)

## Sources

1. Repository source at the collector commit:
   `.github/workflows/tests-and-quality-gates.yml`,
   `.github/workflows/sycl-parity.yml`,
   `.github/workflows/required-aggregator.yml`,
   `scripts/ci/check-runner-available.sh`, and ADR-1177.
2. Read-only live GitHub API on 2026-09-25:
   `GET repos/VMAFx/vmafx/actions/runners`,
   `GET orgs/VMAFx/actions/runners`,
   `GET repos/VMAFx/vmafx/actions/variables`, and
   `GET repos/VMAFx/vmafx/actions/secrets`.
3. Read-only local supervisor evidence:
   `systemctl --user status/list-unit-files`, process search, runner-container
   listing, and inspection of the documented environment/state paths.
4. GitHub's current
   [workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idif),
   [runner monitoring guide](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/monitor-and-troubleshoot),
   and [self-hosted runner REST API](https://docs.github.com/en/rest/actions/self-hosted-runners).

## Reproducer before the change

```bash
git grep -n 'runs-on: \[self-hosted, linux, gpu-full\]' -- \
  .github/workflows/tests-and-quality-gates.yml
gh api repos/VMAFx/vmafx/actions/runners
gh api orgs/VMAFx/actions/runners
gh api repos/VMAFx/vmafx/actions/variables
systemctl --user status vmafx-sycl-arc-runner.service --no-pager
python3 -B scripts/ci/test_self_hosted_runner_workflow_contract.py
```

Observed live inventory:

- repository runners: `total_count=0`;
- organisation runners: `total_count=0`;
- repository variables: `total_count=0`;
- repository secrets exposed by name only: `RELEASE_BOT_TOKEN` and the
  existing read-only `SYCL_RUNNER_PROBE_TOKEN`;
- local runner user unit: not found; no documented environment file or state
  directory; no matching runner process or container.

The first red contract run found two `gpu-full` targets, no hosted admission
job, no `GPU_COVERAGE_ENABLED` handling in the aggregator, the obsolete SYCL
check still required, and no way for the real aggregator harness to exercise
lane-variable outcomes.

## Hypotheses and checks

| Hypothesis | Cheapest falsification | Result |
|---|---|---|
| The `gpu-full` jobs lack live admission and can wait forever after a stale enable | Inspect each job's `needs`, `if` and `runs-on`; query runners/variables | Confirmed: both jobs depended only on `GPU_COVERAGE_ENABLED`; there was no runner and no variable |
| The older SYCL job remained after ADR-1177 established a dedicated owner | Compare both workflows and required names | Confirmed: both ran `float_ssim` parity, but on incompatible labels |
| Aggregator semantics hide an enabled-but-unavailable `gpu-full` lane | Execute the embedded JavaScript with synthetic absent/skipped results and a true switch | Confirmed after adding an environment seam: generic absence/skip acceptance had no GPU-lane exception |
| Relabelling the Arc runner could satisfy both lanes | Compare `runs-on` contract and ADR-1177 device isolation | Falsified: Arc exposes only Intel; `Coverage GPU` builds CUDA and SYCL, so the label would lie |
| Fixing these jobs would establish tiny-AI cross-device parity | Search job commands for ONNX multi-provider score comparison | Falsified: no job runs one tiny model on two execution providers and compares its outputs |

## Findings

GitHub documents that a job-level `if` can prevent a job from running, and
that `runs-on` label arrays require a runner matching all labels. A hosted job
can therefore query the runner inventory first and expose `available=true`
only for an online complete label match. The self-hosted job consumes that
output in its job-level `if`, so no hardware queue entry is created when the
probe rejects admission.

Checking only the custom label is insufficient. A runner can carry
`sycl-arc` but omit `x64`, or carry `gpu-full` while omitting a default label;
the probe would pass and the dependent job could still be unroutable. The
probe must compare the entire `runs-on` set case-insensitively because GitHub's
default labels commonly appear as `Linux` and `X64` in API responses.

The two hardware lanes remain independent:

- `sycl-arc`: isolated Intel Arc parity owned by `sycl-parity.yml`;
- `gpu-full`: dormant multi-capability CUDA + SYCL coverage recipe owned by
  `tests-and-quality-gates.yml`.

No hardware result was produced during this investigation. A green repository
contract proves safe scheduling and fail-closed aggregation; it does not prove
GPU correctness, provision a runner, validate tiny-AI parity, or satisfy either
hardware blocker.

## Validation contract

`scripts/ci/test_self_hosted_runner_workflow_contract.py` now executes the
real aggregator JavaScript for both hardware lanes across enabled/disabled and
success/absent/skipped/neutral/failure combinations, pins one `gpu-full`
owner, pins the dedicated SYCL owner, and requires hosted admission before
dispatch.
`scripts/ci/tests/test-runner-available.sh` separately pins complete-label,
case-insensitive, online/offline and API-failure behavior.
