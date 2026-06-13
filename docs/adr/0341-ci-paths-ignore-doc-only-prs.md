<!-- markdownlint-disable MD036 MD060 -->
# ADR-0341: `paths-ignore` filter on heavy CI workflows for doc-only PRs

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude
- **Tags**: `ci`, `build`, `policy`, `fork-local`

## Context

[Research-0089 §3.2](../research/0089-ci-cost-optimization-audit-2026-05-09.md)
audited the fork's CI surface and found two heavy workflows that fire the
full build / test matrix on every pull request, including PRs whose diff
touches only documentation:

- [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml)
  — 18 build cells across Linux gcc/clang, macOS, ARM, Windows MinGW64,
  Windows MSVC + CUDA, Windows MSVC + oneAPI SYCL, Vulkan, HIP, DNN,
  i686 — p50 wall-clock 12.3 min, ~143 runner-min per push.
- [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
  — Netflix CPU golden, sanitizers (ASan/UBSan/TSan), coverage, Tiny AI
  (DNN suite + ai/ pytests), MCP smoke, Vulkan parity matrix gate, assertion
  density — p50 wall-clock 13.6 min.

Together they account for ~80% of per-PR runner-spend. Of the last 50 PRs
sampled, ~25% had a doc-only or research-only diff (e.g. PR #525 — single
file `docs/research/0089-ci-cost-optimization-audit-2026-05-09.md`); these
PRs gain nothing from re-running 18 builds + 10 test jobs that exercise
zero of the inputs they validate.

The naive fix — `paths-ignore: ['docs/**']` — was previously rejected in
[Research-0061](../research/0061-docs-only-ci-fast-track.md) because branch
protection at the time enforced 23 named required checks, and a
workflow-not-reported was treated as "missing", deadlocking doc-only PRs.

[ADR-0313](0313-ci-required-checks-aggregator.md) replaced that posture
with a single `Required Checks Aggregator` workflow that explicitly tolerates
the missing-check case as path-filter-skipped/acceptable. The aggregator
script ([.github/workflows/required-aggregator.yml](../../.github/workflows/required-aggregator.yml))
contains the load-bearing branch:

```javascript
if (!run) {
  core.info(`OK (not reported, treated as path-filter-skip): ${name}`);
  continue;
}
```

That commit unblocks the original Research-0061 strategy: a `paths-ignore`
filter at the trigger level is now safe, because branch protection only
asks the aggregator, and the aggregator accepts not-reported as success.

## Decision

Add a conservative `paths-ignore` deny-list to the `pull_request:` trigger
of both `libvmaf-build-matrix.yml` and `tests-and-quality-gates.yml`.
Skip when **and only when** the diff is entirely documentation:

```yaml
paths-ignore:
  - 'docs/**'
  - '**/*.md'
  - 'changelog.d/**'
  - 'CHANGELOG.md'
  - '.workingdir2/**'
```

Mirrors the `paths:` allow-list pattern introduced in
[ADR-0317](0317-ci-doc-only-pr-flake-fix.md) on `docker-image.yml` and
`ffmpeg-integration.yml`, but uses a deny-list instead so the default for
any new file class is "run the matrix" — the failure mode of an unknown
path is conservative.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **`paths-ignore` deny-list** (chosen) | Conservative default — unknown paths run CI. Mirrors GitHub-native trigger filtering. Aggregator already tolerates skips per ADR-0313. | Trigger-level skip is binary per workflow — no per-job granularity. | — |
| **`paths:` allow-list** | Same effect; matches ADR-0317's pattern verbatim. | Allow-list is less safe: a new top-level directory (e.g. `tools_v2/`) would silently skip CI until the allow-list grows. Research-0061 §Risks called this out explicitly. | Failure mode goes the wrong way. |
| **`dorny/paths-filter` job-level detector + always-success shim** | Per-job granularity; doc-only PRs can still get a green checkmark from each individual workflow. | ~120 LOC of YAML per workflow + a third-party action; superseded by the simpler aggregator (ADR-0313). | Aggregator already does the same job at the policy layer. |
| **Drop builds from required list entirely** | Maximally permissive. | Loses pre-merge build coverage on every PR. | Regression vs current posture. |

## Consequences

**Positive**

- Doc-only / research-only PRs (e.g. #525) merge with only the
  always-runs-everywhere lanes (`lint-and-format`, `security-scans`,
  aggregator, rule-enforcement) — typically <5 min wall-clock vs the
  current ~14–16 min.
- Estimated savings: ~14 runner-min per avg PR (Research-0089 §3.2),
  scaling with doc-PR rate.
- Frees runner capacity during merge trains so code PRs land faster.

**Negative**

- One additional cognitive step when reviewing CI on a doc PR: the
  build matrix and quality gates do not appear under "Checks" because
  they were never triggered. The aggregator's check name still
  appears (always runs) and reports green; reviewers verify via the
  aggregator's per-check log line `OK (not reported, treated as
  path-filter-skip)`.
- Mismatch between the two workflows' deny-lists must stay in sync.
  Both use the identical doc-only set, by design.

**Neutral / follow-ups**

- Other heavy workflows (`docker-image.yml`, `ffmpeg-integration.yml`)
  already have path filters from ADR-0317; `lint-and-format.yml`,
  `security-scans.yml`, `required-aggregator.yml` always run regardless
  of paths and are out of scope here.
- If the deny-list ever expands beyond pure-doc paths (e.g. a request
  to also exclude `python/**` for libvmaf-build-matrix), it ships as a
  separate ADR — the failure mode of a too-broad deny-list is silent
  loss of build coverage.

## References

- [Research-0089 §3.2](../research/0089-ci-cost-optimization-audit-2026-05-09.md)
  — CI cost-optimization audit; this ADR implements the second-ranked
  finding ("Add `paths-ignore` filter").
- [Research-0061](../research/0061-docs-only-ci-fast-track.md) — earlier
  feasibility analysis that rejected `paths-ignore` under the pre-ADR-0313
  branch-protection posture.
- [ADR-0313](0313-ci-required-checks-aggregator.md) — required-aggregator
  workflow, load-bearing dependency: tolerates missing checks.
- [ADR-0317](0317-ci-doc-only-pr-flake-fix.md) — first path-filter rollout
  on `docker-image.yml` / `ffmpeg-integration.yml`; this ADR extends the
  pattern to the two heaviest required-check-emitting workflows.
- [ADR-0037](0037-master-branch-protection.md) — original 23-required-check
  posture (now superseded by ADR-0313's single-aggregator).
- PR #525 — recent doc-only PR (single file
  `docs/research/0089-ci-cost-optimization-audit-2026-05-09.md`) that
  would have qualified for the skip.
- Source: paraphrased — operator direction on 2026-05-09 to land the
  Research-0089 §3.2 finding as a draft PR while leaving the §3.1
  ccache-persistence finding for a separate change.
