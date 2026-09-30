<!-- markdownlint-disable MD013 MD060 -->
# ADR-1389: Run CodeQL (Actions) unconditionally on every pull request for universal SAST coverage

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: ci, security, codeql, scorecard, sast, fork-local

## Context

OpenSSF Scorecard alert #6 (`SASTID`) reported:
> "score is 9: SAST tool detected but not run on all commits: 28 commits out of 30 are checked with a SAST tool"

Scorecard's `sastToolInCheckRuns` evaluates the last 30 commits on default branch (`master`) by looking up each commit's associated pull request head SHA and querying check runs. It checks whether a SAST tool (matching GitHub apps such as `github-advanced-security`, `github-code-scanning`, or `sonarcloud`) completed with success or neutral status on each PR.

Under [ADR-1140](1140-ci-impact-planner.md) (diff-aware CI plan impact) and [ADR-1222](1222-code-scanning-alert-triage-and-scope.md), CodeQL's heavy C/C++ analysis (`codeql-cpp`, which compiles C/C++ with Meson/Ninja taking 12+ minutes) and Python analysis (`codeql-python`) only run when their respective file categories (`c_core`, `python_lint`) are impacted.

Previously, `codeql-actions` was also conditioned on `steps.impact.outputs.actions == 'true'` via `scripts/ci/plan-ci-impact.py`. Consequently, PRs that touched neither C/C++ nor Python nor GitHub Actions workflows (such as documentation-only changes, Markdown edits, or repository-metadata PRs) had CodeQL analysis skipped entirely. When `codeql-action/analyze` was skipped on a PR, no CodeQL SARIF was uploaded and no CodeQL check run was registered by GitHub Code Scanning on that PR's head commit.

Because Scorecard's SAST check evaluates whether static analysis ran across merged PR commits, skipping CodeQL on docs-only and non-code PRs caused Scorecard's SAST score to drop from 10 to 9, flagging that not all commits were scanned.

## Decision

1. **Run `CodeQL (Actions)` unconditionally on every pull request (non-draft) and push to `master`.** The `plan-ci-impact.py` skip is removed from `codeql-actions` in `.github/workflows/security-scans.yml`.
2. **Add `CodeQL (Actions)` to the required checks in `.github/workflows/required-aggregator.yml`.** Every PR must have passing CodeQL workflow analysis to merge.
3. **Keep `codeql-cpp` and `codeql-python` diff-aware under ADR-1140.** The expensive Meson build and extraction for C/C++ (~12–15 min) remains gated on `c_core` changes, and Python scanning remains gated on `python_lint`.
4. **Why `CodeQL (Actions)` is optimal for universal PR SAST:**
   - **Ultra-fast execution**: scanning GitHub Actions workflow definitions takes only ~15–20 seconds, adding negligible overhead to CI turnaround.
   - **Universal SAST compliance**: every pull request receives a completed CodeQL analysis check run registered by `github-advanced-security`, guaranteeing 100% commit coverage for OpenSSF Scorecard.
   - **Continuous workflow security**: workflow token permissions, script injection risks, and action pinning are verified across all pull requests, preventing supply-chain regressions.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Run `codeql-actions` unconditionally on every PR (chosen) | Ultra-fast (~15–20s); satisfies Scorecard SAST on 100% of commits; ensures workflow security; keeps heavy C/C++ scan diff-aware | Consumes ~20s of GitHub runner time on docs PRs | Best balance of speed, security, and standards compliance |
| Run all CodeQL jobs (`cpp`, `python`, `actions`) unconditionally | Scans all languages on every PR | Extreme CI latency: 12–15 minutes for Meson C/C++ build and scan on docs or typo PRs; wastes compute | Violates ADR-1140 and CI efficiency invariant HISS-18 |
| Rely solely on Semgrep (`semgrep-local`) for SAST | Already runs on every PR | Scorecard's unpaginated check runs query can miss later checks if check runs exceed 30 on a PR; does not provide Actions AST injection coverage | Fails to guarantee universal detection and misses workflow security defects |
| Dismiss or ignore Scorecard alert #6 | Zero CI changes | Leaves a permanent security tab alert; fails OpenSSF Scorecard supply-chain bar | Violates project goal of zero open security alerts |

## Consequences

- **Positive**: 100% of PR commits receive a verified CodeQL SAST check run; Scorecard SAST score reaches 10/10 (30/30 commits checked); workflow security regressions are caught immediately on every PR; heavy compiled analysis remains diff-aware.
- **Negative**: Pull requests touching only docs or non-code files run one additional ~20s hosted runner job.
- **Neutral / follow-ups**: Enforced by `required-aggregator.yml` and monitored via OpenSSF Scorecard weekly runs.

## References

- OpenSSF Scorecard alert #6 (`SASTID`): "score is 9: SAST tool detected but not run on all commits: 28 commits out of 30 are checked with a SAST tool".
- [ADR-1140](1140-ci-impact-planner.md) (diff-aware CI plan impact).
- [ADR-1222](1222-code-scanning-alert-triage-and-scope.md) (in-code suppressions do not close code-scanning alerts; scope the scan instead).
- [ADR-1247](1247-scorecard-exact-head-gates.md) (scorecard exact-head gates).
- OpenSSF Scorecard SAST check implementation: `github.com/ossf/scorecard/v5/checks/evaluation/sast.go` and `github.com/ossf/scorecard/v5/checks/raw/sast.go`.
