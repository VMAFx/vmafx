<!-- markdownlint-disable MD013 MD036 -->
# ADR-0313 — CI required-checks aggregator (unblock doc/Python-only PRs)

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-05-05 |
| Authors | Kilian, Claude |
| Supersedes | — |
| Superseded by | — |

## Context

The fork's `master` branch protection (per ADR-0037) requires 23 named status
checks to report a passing conclusion before a PR can merge. The list is
heavy on C/C++ build matrix entries:

- `Build — Ubuntu gcc (CPU) + DNN`, `Build — Ubuntu clang (CPU) + DNN`
- `Build — Windows MinGW64 (CPU)`, `Build — Windows MSVC + CUDA (build only)`,
  `Build — Windows MSVC + oneAPI SYCL (build only)`
- `CodeQL`, `CodeQL (Actions)`, `CodeQL (C/C++)`, `CodeQL (Python)`
- `Clang-Tidy (Changed C/C++ Files)`, `Cppcheck (Whole Project)`
- `Pre-Commit (Formatters + Basic Checks)`, `Python Lint (Ruff + Black + isort + mypy)`
- `Semgrep (CWE Top 25 + CERT-C + Custom)`, `Gitleaks (Secret Scan)`,
  `ShellCheck + shfmt (All *.sh)`, `Dependency Review (PR Diff)`

Each build/lint workflow carries a path filter so it only fires when files
in its scope changed. That's the right policy: a doc-only PR shouldn't burn
30 minutes of Windows MSVC matrix time. **But** the fork's branch protection
counts a path-filter short-circuit (workflow runs and self-skips, conclusion
`skipped`) **and** a never-ran-at-all (workflow not triggered, no check_run
ever appears) as **not satisfying** the required-check constraint.

Concrete failure modes observed on PR #400 (research/encoder-knob-sweep
Pareto analysis — diff is `docs/`, `ai/scripts/`, `ai/tests/`,
`changelog.d/` only):

- 10 of 23 required checks reported `success` (the always-run lint /
  golden-data / sanitizer / DNN suite).
- 13 of 23 either reported `skipped` (path-filter short-circuit, conclusion
  set) or never appeared at all (workflow `paths:` filter rejected the
  trigger entirely).
- GitHub's mergeability gate marks the PR `BLOCKED` because the 13
  not-`success` checks aren't satisfying the named required-check list.
- `gh pr merge 400 --squash` returns `the base branch policy prohibits the
  merge`. Only `--admin` succeeds.

The same blocker hits every doc/Python-only PR. PR #399 cleared because
its diff included `scripts/ci/ensemble_prod_gate.py` — that one path
hit the C-build path filter and triggered the matrix. Pure docs/AI PRs
(#400, #403, #404, #405, #406, #407) are structurally stuck.

User direction (popup, 2026-05-05): "Add a required-jobs-aggregator job
(CI policy fix)" — option 2 of 3 (vs. admin-merge per PR or hold the train).

## Decision

Add **one new workflow** `Required Checks Aggregator` that:

1. Runs on every non-draft PR and every push to master, **with no path
   filter**, so it's always present on every commit's check-runs list.
2. Polls up to 8 minutes for the 23 named required checks to register.
3. Treats `success`, `skipped`, and `neutral` as passing for each named
   check.
4. Treats a check that never appears at all as **passing** (path-filter
   rejection of the workflow trigger is the documented "I don't apply
   here" semantics).
5. Fails only if a named check reported `failure`, `cancelled`, `timed_out`,
   or `action_required`.

The aggregator becomes the **single** branch-protection required check.
The 23 individual checks remain wired but stop being required-by-name —
the aggregator is the one that GitHub's protection consults.

Branch protection update is a **manual operator step** (the workflow PR
itself can't change `repos/.../branches/master/protection` because that
endpoint requires admin auth and is shared state). The PR body documents
the exact `gh api PUT` call the operator runs after merge.

## Alternatives considered

| Option | Pros | Cons | Rejected because |
| --- | --- | --- | --- |
| **Aggregator workflow** (chosen) | One required check; permanent fix; no per-PR overrides; path filters stay sane | Requires manual branch-protection edit at adoption | — |
| **Admin-merge each blocked PR** (`gh pr merge --admin`) | Zero infrastructure work; works today | Bypasses protection on every doc/Python-only PR; trains the operator to reach for `--admin` reflexively (memory: "no skip-shortcuts") | Erodes the protection-as-policy posture; recurring per-PR friction |
| **Loosen path filters** (e.g., let C-builds run on every PR including doc-only) | Single-mechanism fix; no aggregator code | Burns ~30 min runner-time × 5 build matrix entries × every doc-only PR; OSS Action minutes are not free | Cost > benefit; goes the wrong way on "fast feedback" |
| **Drop builds from required list** (rely on push-to-master CI catching breaks) | Maximally permissive; matches some upstream policies | Loses pre-merge build coverage; a broken merge to master would only surface after-the-fact via the push CI | Regression vs current posture |

## Implementation

- `.github/workflows/required-aggregator.yml` — the workflow.
- Branch-protection update (manual, by repo admin):

  ```bash
  # Replace the 23-check required list with the single aggregator.
  gh api -X PUT "repos/VMAFx/vmafx/branches/master/protection/required_status_checks" \
    -f strict=true \
    -F 'contexts=["Required Checks Aggregator"]'
  ```

- Documentation:
  - This ADR (Proposed → flips to Accepted once branch protection is updated
    and the aggregator passes on a doc-only PR).
  - `docs/development/release.md` § "master branch protection" updated to
    reflect the new single-required-check posture.
  - `docs/rebase-notes.md` §0313.

The 23 individual workflows continue to run; they're the substance of CI.
The aggregator is a pure policy-layer add — zero impact on existing build /
test / lint behaviour.

## Consequences

**Positive**

- PRs #400, #403, #404, #405, #406, #407 (and any future doc/Python-only
  PR) merge through the normal `gh pr merge --squash` path.
- Operator no longer reaches for `--admin` for routine doc/Python merges.
- `--admin` stays available for genuine emergencies (a stuck check, a
  bug in the aggregator itself).

**Negative**

- One additional workflow run per PR (~30s for the aggregator job itself,
  plus its 8-min poll budget when sibling workflows are slow). Negligible
  vs the build matrix.
- If the aggregator itself has a bug, **every** PR is blocked. Mitigation:
  the workflow is small (~120 LOC YAML), reviewed pre-merge; rollback is
  a one-line revert + branch-protection edit.

**Neutral**

- Path filters on the 23 build/lint workflows are unchanged — they keep
  short-circuiting on irrelevant diffs.

## References

- [ADR-0037](0037-master-branch-protection.md) — original 23-check
  required list.
- [ADR-0125](0125-ms-ssim-decimate-simd.md) and similar — examples of
  C/C++ workflows that path-filter on `libvmaf/`.
- User direction (popup, 2026-05-05): "Add a required-jobs-aggregator
  job (CI policy fix)".
- PR #400 mergeability investigation, 2026-05-05: 10/23 required checks
  succeeded, 13/23 either skipped or never reported; `gh pr merge` returned
  "the base branch policy prohibits the merge".
