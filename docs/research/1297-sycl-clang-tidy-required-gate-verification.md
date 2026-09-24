<!-- markdownlint-disable MD013 MD024 MD060 -->
# Research-1297: SYCL Clang-Tidy Required Gate Verification and Hardening

- **Status**: Active
- **Workstream**: ADR-1297, ADR-0623
- **Last updated**: 2026-09-24

## Scope

This research digest documents post-promotion verification and hardening of pre-RC1 state
item `T-SYCL-CLANG-TIDY-DISABLED`. Commit `6475fa9ea` had already made `Tidy SYCL` a
required, non-advisory CI merge gate under ADR-1297. This follow-up records the audit of
live master GitHub Actions runs, reconciles the stale state row, remediates header-pattern
coverage gaps, makes non-reporting fail closed, and adds mutation-tested contract
infrastructure on one shared aggregator harness.

## Context and Reactivation Criteria

ADR-0623 (`docs/adr/0623-scaffold-audit-p2-half-finished.md`) re-enabled the
`clang-tidy-sycl` CI job using `scripts/ci/gen-sycl-compile-commands.py` (which synthesises
compilation database entries for Meson's `CUSTOM_COMMAND` SYCL translation units) and the
`scripts/ci/clang-tidy-sycl.sh` icpx-aware wrapper. The job was deliberately configured with
`continue-on-error: true` (advisory status) pending at least one green master run confirming
that the toolchain synthesis and header include resolution held without false positives.

ADR-1297 (`docs/adr/1297-ci-gate-every-reporting-check.md`) established the repository-wide
merge gate policy: every check reporting on pull requests must be a required context in
`required-aggregator.yml`. Commit `6475fa9ea` implemented that policy for this lane:
`Tidy SYCL (advisory)` dropped its suffix, lost `continue-on-error: true`, and joined the
aggregator's `required` array.

However, state item `T-SYCL-CLANG-TIDY-DISABLED` remained open in `docs/state.md`, changed-file
detection in `lint-and-format.yml` omitted `.h` files, and no dedicated contract test guarded
against silent relaxation or workflow decoupling.

## Live Master GitHub Actions Audit

On 2026-09-24 we re-read the live run and job pages for this bounded sequence of master
pushes. The run page supplies the exact head and conclusion; the `Tidy SYCL` job page
supplies each step conclusion. The squash-commit subject supplies the merged PR number.

| Run ID | Exact head | PR | Event | `Tidy SYCL` result |
| --- | --- | --- | --- | --- |
| [35914295017](https://github.com/VMAFx/vmafx/actions/runs/35914295017) | `5b1bb866f08455b48f34d05401b30b99cfa588b5` | #1529 | push | Job `success`; wrapper step `skipped` (no matching SYCL path) |
| [35904645958](https://github.com/VMAFx/vmafx/actions/runs/35904645958) | `737430956f2141a81dd9ac4ce0bb52b21b37f21f` | #1528 | push | Job `success`; wrapper step `skipped` (no matching SYCL path) |
| [35898349474](https://github.com/VMAFx/vmafx/actions/runs/35898349474) | `b1f22066d46e38305ce589897ff3b3b028025d0f` | #1527 | push | Job `success`; install, compile-database, and wrapper steps all `success` for the changed SYCL SpEED twins |
| [35868533022](https://github.com/VMAFx/vmafx/actions/runs/35868533022) | `961662ba302624f78dec8e8e31966ba78b09d553` | #1524 | push | Job `success`; wrapper step `skipped` (no matching SYCL path) |
| [35861745563](https://github.com/VMAFx/vmafx/actions/runs/35861745563) | `7f1005915fe16c2f329ae0cf80dd5e590b1e828d` | #1516 | push | Job `success`; wrapper step `skipped` (no matching SYCL path) |
| [35857035653](https://github.com/VMAFx/vmafx/actions/runs/35857035653) | `2718263cb34a900799bbb9c1f295cff18b63b173` | #1523 | push | Job `success`; install, compile-database, and wrapper steps all `success` for `integer_ms_ssim_sycl.cpp` |
| [35839197106](https://github.com/VMAFx/vmafx/actions/runs/35839197106) | `301c2b5f04a6af1cfaba0fa4d430ecb6ad5c700f` | #1522 | push | Job `success`; wrapper step `skipped` (no matching SYCL path) |
| [35833239963](https://github.com/VMAFx/vmafx/actions/runs/35833239963) | `a6e3faad489d147124784ede58eccc093d36b718` | #1430 | push | Job `success`; wrapper step `skipped` (no matching SYCL path) |

Findings:

1. **Zero Failures in the bounded sample**: all eight `Tidy SYCL` jobs reported `success`.
2. **Two real executions verified**: runs 35857035653 and 35898349474 installed the toolchain, generated the compilation database, ran `clang-tidy-sycl.sh`, and succeeded. The earlier draft incorrectly labelled 35898349474 as a clean skip.
3. **Name Stability**: The check name is stably reported as `Tidy SYCL` across all runs, matching the declaration in `required-aggregator.yml`.

## File Pattern Gap Analysis and Remediation

In `.github/workflows/lint-and-format.yml`, the changed-file detection step for `clang-tidy-sycl`
previously matched:

```sh
-- 'core/src/sycl/*.cpp' 'core/src/sycl/*.hpp' \
   'core/src/feature/sycl/*.cpp' \
   'core/src/feature/sycl/*.hpp' \
   'core/test/test_sycl*.c' 'core/test/test_sycl*.cpp' \
   'core/test/test_integer_cambi_sycl.c'
```

However, the codebase contains SYCL header files with `.h` extensions:

- `core/src/sycl/picture_sycl.h`
- `core/src/feature/sycl/sycl_compat.h`

Changes modifying only these `.h` headers would have resulted in `files=""` and skipped the
job body entirely.

**Remediation**: Expanded file patterns across pull-request, push, and dispatch triggers in
`.github/workflows/lint-and-format.yml` to include `'core/src/sycl/*.h'` and
`'core/src/feature/sycl/*.h'`. Note that `core/include/libvmaf/libvmaf_sycl.h` is a public C API
header and is already analyzed by the CPU `clang-tidy` job.

## Contract Test Infrastructure

To ensure that the required, non-advisory gate is permanent and protected against regression,
we implemented `scripts/ci/test_sycl_tidy_workflow_contract.py`.

The contract test asserts:

1. `lint-and-format.yml` declares job `clang-tidy-sycl` with exact `name: Tidy SYCL` and `# required-aggregator`.
2. No `continue-on-error` or normalized constant-false job guard exists in the job.
3. Every pull-request, push-fallback, normal-push, and dispatch selection independently tracks all SYCL source and header patterns.
4. Execution step uses `scripts/ci/clang-tidy-sycl.sh` and exits 1 if `/tmp/tidy-sycl.fail` is created.
5. `required-aggregator.yml` registers `'Tidy SYCL'` in both `required` and `strictMustReport`; the job has no path filter, so absence is never a legitimate path skip.
6. Real Node.js execution of the aggregator script proves:
   - A `failure` conclusion on `Tidy SYCL` blocks the aggregator (`Tidy SYCL: failure`).
   - A `success` conclusion passes cleanly.
   - A `skipped` or unreported run blocks under the strict-required contract.
7. Red mutation tests confirm that injecting `continue-on-error: true`, renaming to `(advisory)`,
   omitting the marker comment, using normalized constant-false job guards, dropping either
   header pattern from any one event branch, or removing `Tidy SYCL` from either aggregator
   array raises an assertion failure.
8. `scripts/ci/required_aggregator_harness.py` owns the Node driver shared by the Go and SYCL
   contract suites, eliminating the HISS-19 duplication that allowed their simulations to drift.

## Integration Surfaces

The contract is wired into:

- CI: `.github/workflows/rule-enforcement.yml` under `deep-dive-checklist`.
- Local hooks: `.pre-commit-config.yaml` (`test-sycl-tidy-workflow-contract`).
- Shared execution seam: `scripts/ci/required_aggregator_harness.py`, reused by the Go and
  SYCL workflow contracts.
- Strict-set replay contract: `scripts/ci/tests/test_hiss_replay_contract.py`, which pins
  `Tidy SYCL` as a fail-closed ADR-1297 context.
- Documentation: `scripts/ci/AGENTS.md` (Workflow coupling table & SYCL custom-command section).
- State ledger: `docs/state.md` (`T-SYCL-CLANG-TIDY-DISABLED` closed and moved to `## Recently closed`).
- Rebase notes: `docs/rebase-notes.md`.
- Changelog: `changelog.d/fixed/sycl-clang-tidy-required-gate.md` and synced `CHANGELOG.md`.
