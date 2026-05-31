<!-- markdownlint-disable MD013 MD060 -->
# Research-0061 — Docs-only PR fast-track for CI matrix

## Problem

Docs-only / research-only PRs (no code, only `docs/`, `changelog.d/`,
`*.md`) currently wait the full ~25-minute CI matrix because the 23
required status checks include heavy build / sanitizer / CodeQL jobs
that are completely orthogonal to docs content. Recent merge train
PRs that hit this:

- #353 — NVIDIA-Vulkan ciede places=4 docs (ADR-0273)
- #354 — vmaf-tune capability audit research
- #389 (vmaf-tune mp4→yuv decode) — code, but small
- #390 (CUDA `--std c++20`) — code, but small

For docs-only PRs the cycle is dominant overhead: ~25 min wait per
PR × 5+ docs PRs/week ≈ 2 hours/week of pure CI latency.

## Why straightforward fixes don't work

| approach | failure mode |
|----------|--------------|
| Add `paths-ignore: ['docs/**']` to heavy workflows | Required checks then never report → branch protection blocks merge indefinitely. Per GitHub: "If a required status check isn't reported, the pull request is blocked." |
| Job-level `if:` that skips on docs-only | GitHub reports the job as `skipped`, not `success`. Branch protection treats `skipped` as missing for required checks (verified empirically on this repo, recent PRs). |
| Branch-protection rule sets per file pattern | Not currently a feature; rule sets are repo-wide. |

## Working pattern: shim + always-runs

The standard pattern across large open-source repos:

1. **Detector job** at the top of every heavy workflow:

   ```yaml
   detect-changes:
     runs-on: ubuntu-latest
     outputs:
       code: ${{ steps.filter.outputs.code }}
     steps:
       - uses: dorny/paths-filter@<sha-pin>
         id: filter
         with:
           filters: |
             code:
               - '!docs/**'
               - '!changelog.d/**'
               - '!**/*.md'
               - '!**/AGENTS.md'
   ```

2. **Heavy job** consumes the output: `if: needs.detect-changes.outputs.code == 'true'`.
3. **Always-success shim** for the same check name when docs-only:

   ```yaml
   build-shim:
     needs: detect-changes
     if: needs.detect-changes.outputs.code != 'true'
     name: Build — Ubuntu gcc (CPU)   # MUST match the required-check context name
     runs-on: ubuntu-latest
     steps: [{run: 'echo "docs-only diff: skipping heavy build"'}]
   ```

The shim takes ~10s to spin up, runs the no-op echo, and reports
`success` under the same context name as the heavy job. Branch
protection sees the required check as passed.

## Scope

Required-check inventory (23, from `gh api repos/VMAFx/vmafx/branches/master/protection`):

| category | # checks | candidate for skip-on-docs |
|----------|---------:|---------------------------|
| Build matrix (Ubuntu gcc/clang, macOS, MinGW, MSVC) | 6 | yes |
| Sanitizers (ASan / UBSan / TSan) | 3 | yes |
| CodeQL (Actions / C/C++ / Python) | 3 | yes (for docs-only — no code to scan) |
| Pre-Commit (formatters) | 1 | NO — markdown lint runs here |
| Python Lint | 1 | NO — checks .py docstrings? actually yes if no .py touched |
| Semgrep | 1 | yes (for docs-only) |
| Netflix CPU Golden | 1 | yes |
| Tiny AI suite | 1 | yes |
| Clang-Tidy | 1 | yes |
| Cppcheck | 1 | yes |
| Assertion Density | 1 | yes |
| Dependency Review | 1 | yes |
| Gitleaks | 1 | NO — must scan docs for leaked tokens |
| ShellCheck + shfmt | 1 | yes (for non-shell docs) |

Estimated 18 of 23 required checks are skippable on docs-only PRs.

## Proposed roll-out

1. **Phase 0** (this digest) — agree on the pattern + scope.
2. **Phase 1** — pilot with `libvmaf-build-matrix.yml` (1 workflow,
   6 required check names). Validate: shim PR for a docs-only test
   merges in <2 min instead of 25 min. **Rollback path**: revert one
   workflow.
3. **Phase 2** — extend to sanitizers + Netflix golden + Tiny AI +
   the rest of the skippable list.
4. **Phase 3** — measure: median CI time on docs-only PRs over the
   following week. Target: < 5 min wall.

## Risks

- **Drift between shim's `name:` and the required-check context.**
  GitHub's check-context naming is exact-match. A typo in the shim's
  `name:` field reports a *different* status name, leaving the
  required check missing. Mitigation: a CI-of-CI test that diffs the
  branch-protection required-check list against the workflow `name:`
  fields.
- **Shim claims success on a code-touching PR with a bug in the
  detector.** If `dorny/paths-filter`'s `code:` predicate misses a
  newly-added directory (e.g. a new top-level `tools_v2/`), the
  heavy job is skipped for code that should have been built. Test
  coverage: include a `'!docs/**'` + `'!changelog.d/**'` + `'!*.md'`
  exclude-set rather than allow-listing specific code paths.
- **Required-check name renames.** Any rename (e.g. when matrix
  entries shuffle) needs both the heavy job's `name:` and the
  shim's `name:` updated together, or the shim reports the wrong
  context.

## Decision pending

The work is mechanical but spans every heavy workflow. Recommend
picking up after the current merge train (PRs #354–#382) drains so
we don't churn workflows mid-train.

## References

- [GitHub Actions: paths-filter](https://github.com/dorny/paths-filter) — load-bearing dependency
- [GitHub branch protection docs](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches#require-status-checks-before-merging)
- Required-check list for `VMAFx/vmafx:master`: 23 entries (snapshot 2026-05-04)
