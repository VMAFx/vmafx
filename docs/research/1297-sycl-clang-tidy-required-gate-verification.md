<!-- markdownlint-disable MD013 MD024 MD060 -->
# Research-1297: SYCL Clang-Tidy Required Gate Verification and Promotion

## Scope

This research digest documents the verification and promotion of pre-RC1 state item
`T-SYCL-CLANG-TIDY-DISABLED` to a required, non-advisory CI merge gate. It records the
audit of live master GitHub Actions runs, the verification of check name stability and
zero-exit behavior under real SYCL changes, the remediation of header pattern coverage
gaps, and the fail-closed contract testing infrastructure.

## Context and Reactivation Criteria

ADR-0623 (`docs/adr/0623-scaffold-audit-p2-half-finished.md`) re-enabled the
`clang-tidy-sycl` CI job using `scripts/ci/gen-sycl-compile-commands.py` (which synthesises
compilation database entries for Meson's `CUSTOM_COMMAND` SYCL translation units) and the
`scripts/ci/clang-tidy-sycl.sh` icpx-aware wrapper. The job was deliberately configured with
`continue-on-error: true` (advisory status) pending at least one green master run confirming
that the toolchain synthesis and header include resolution held without false positives.

ADR-1297 (`docs/adr/1297-ci-gate-every-reporting-check.md`) established the repository-wide
merge gate policy: every check reporting on pull requests must be a required context in
`required-aggregator.yml`. Under this policy, `Tidy SYCL (advisory)` dropped its advisory suffix,
had `continue-on-error: true` removed, and was added to the aggregator's required check array.

However, state item `T-SYCL-CLANG-TIDY-DISABLED` remained open in `docs/state.md`, changed-file
detection in `lint-and-format.yml` omitted `.h` files, and no dedicated contract test guarded
against silent relaxation or workflow decoupling.

## Live Master GitHub Actions Audit

We audited the most recent workflow runs on `origin/master` (up to commit `ef97f72c8`):

| Run ID | Commit | Event | Conclusion | Details |
| --- | --- | --- | --- | --- |
| 35914295017 | `ef97f72c8` | push | `success` | Clean skip (no SYCL files touched) |
| 35904645958 | `b856cbfd7` | push | `success` | Clean skip (no SYCL files touched) |
| 35898349474 | `b1f2dc7e3` | push | `success` | Clean skip (no SYCL files touched) |
| 35868533022 | `6062f6b3d` | push | `success` | Clean skip (no SYCL files touched) |
| 35861745563 | `6475fa9ea` | push | `success` | Clean skip (no SYCL files touched) |
| 35857035653 | `c40b8a05c` | push | `success` | **Touched SYCL**: PR #1520 modified `core/src/feature/sycl/integer_ms_ssim_sycl.cpp`. Job executed `scripts/ci/clang-tidy-sycl.sh` via GNU parallel on the changed TU with exit code 0 |
| 35839197106 | `9713b1945` | push | `success` | Clean skip |
| 35833239963 | `81045231c` | push | `success` | Clean skip |

Findings:

1. **Zero Failures**: `Tidy SYCL` has been consistently green across all recent master runs.
2. **Real Execution Verified**: Run 35857035653 touched `integer_ms_ssim_sycl.cpp`. The job detected the changed file, installed oneAPI compiler components, synthesised the compilation database, ran `clang-tidy-sycl.sh`, and succeeded cleanly.
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
2. No `continue-on-error` or `if: false` constructs exist in the job.
3. All SYCL source and header file patterns are tracked in the detection logic.
4. Execution step uses `scripts/ci/clang-tidy-sycl.sh` and exits 1 if `/tmp/tidy-sycl.fail` is created.
5. `required-aggregator.yml` registers `'Tidy SYCL'` in `const required = [...]`.
6. Real Node.js execution of the aggregator script proves:
   - A `failure` conclusion on `Tidy SYCL` blocks the aggregator (`Tidy SYCL: failure`).
   - A `success` conclusion passes cleanly.
   - An unreported (skipped) run passes cleanly under ADR-0313 absent-means-path-skip semantics.
7. Red mutation tests confirm that injecting `continue-on-error: true`, renaming to `(advisory)`,
   omitting the marker comment, or dropping header patterns raises assertion failures.

## Integration Surfaces

The contract is wired into:

- CI: `.github/workflows/rule-enforcement.yml` under `deep-dive-checklist`.
- Local hooks: `.pre-commit-config.yaml` (`test-sycl-tidy-workflow-contract`).
- Documentation: `scripts/ci/AGENTS.md` (Workflow coupling table & SYCL custom-command section).
- State ledger: `docs/state.md` (`T-SYCL-CLANG-TIDY-DISABLED` closed and moved to `## Recently closed`).
- Rebase notes: `docs/rebase-notes.md`.
- Changelog: `changelog.d/fixed/sycl-clang-tidy-required-gate.md` and synced `CHANGELOG.md`.
