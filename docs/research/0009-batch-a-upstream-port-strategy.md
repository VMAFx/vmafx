# Research-0009: Batch-A upstream small-fix sweep — porting strategy for open PRs

- **Status**: Active
- **Workstream**: ADR-0131, ADR-0132, ADR-0134, ADR-0135
- **Last updated**: 2026-04-20

## Question

Four Netflix upstream PRs addressing backlog items T0-1 / T4-4 / T4-5 /
T4-6 are all **open** — none of them have merged. Should the fork port
their contents now, wait for upstream merge, or cherry-pick SHAs
later? If porting, from which commit — the PR tip, `master`, or the
head branch of the open PR? And what coherent "batch shape" makes
sense for a single fork-local PR given that the four items touch four
different subsystems (CUDA, feature collector, build, public API)?

## Sources

- Netflix PR [#1382](https://github.com/Netflix/vmaf/pull/1382) —
  `cuMemFreeAsync` → `cuMemFree` in `vmaf_cuda_picture_free`
  (tracks [Netflix#1381](https://github.com/Netflix/vmaf/issues/1381)
  assertion-0 crash)
- Netflix PR [#1406](https://github.com/Netflix/vmaf/pull/1406) —
  feature-collector mount/unmount model-list bugfix
- Netflix PR [#1451](https://github.com/Netflix/vmaf/pull/1451) —
  meson `declare_dependency` for subproject consumption
- Netflix PR [#1424](https://github.com/Netflix/vmaf/pull/1424) —
  expose builtin model versions via an iterator
- Fork backlog audit:
  [`.workingdir2/analysis/upstream-backlog-audit.md`](../../.workingdir2/analysis/upstream-backlog-audit.md)
- `gh pr view <n> --json mergeCommit,state` — confirmed `state:OPEN`
  and `mergeCommit:null` for all four on 2026-04-20.

## Findings

### All four PRs are unmerged as of 2026-04-20

Netflix/vmaf's #1382 has been open since 2024-07-15 (~21 months), #1406
since 2024-09, #1424 since 2024-12, #1451 since 2025-07. None have a
merge signal (no "Approved" review, no merge-queue entry, no
`Squash & merge` commit). Waiting is not bounded.

### Per-PR defect analysis

Reading the diffs carefully rather than trusting the PR titles
surfaced latent defects in two of the four:

- **#1382** ships clean; one-line change that's structurally correct.
- **#1406** ships clean; mount/unmount bodies are correct once the
  traversal bug is fixed, but the *test* extension duplicates ~60 LoC
  across two functions and would trip the fork's clang-tidy
  `readability-function-size` rule (JPL-P10 rule 4). Resolved by a
  shared `load_three_test_models` / `destroy_three_test_models` helper.
- **#1451** ships clean; only stylistic drift (trailing comma) to
  reconcile with fork conventions.
- **#1424** ships with three latent defects the fork's quality gates
  would catch:
  1. NULL-pointer arithmetic undefined behaviour — two `if` branches
     not joined by `else` cause `NULL + 1` on the first-call path.
     Detected-by: UBSan, clang's `-Wnull-pointer-arithmetic`.
  2. Off-by-one at end of iteration — `idx < CNT` admits the last
     real index, `prev + 1` then hands back the `{0}` sentinel.
     Detected-by: any test that actually checks total iteration
     count (upstream's test only compared pointers each step).
  3. const-qualifier mismatches in the test — `char *version`
     passed where `const char **` expected. Blocks on `-Werror`.
     C11 §6.5.16.1 forbids the conversion.

The fork ports substance while correcting these during the port, and
ADR-0135 documents each correction with the C11 section references.

### Open-PR tip is the right port base

Cherry-picking from an open PR's head branch (not `master`) means
the fork picks up any iterative fixes the upstream author has pushed
since opening the PR. All four PRs have only one commit each
(confirmed via `gh api repos/Netflix/vmaf/pulls/<n>/commits` — single-
commit PRs from opening day), so there's no difference between
"PR tip" and "the single PR commit" in practice today. Still, the
habit of porting from the PR tip is more robust against multi-commit
PRs in future iterations of this rule.

### Batch shape

The four items touch four different subsystems:

- T0-1: `core/src/cuda/picture_cuda.c` (CUDA backend)
- T4-4: `core/src/feature/feature_collector.c` + test
- T4-5: `core/src/meson.build` (build)
- T4-6: `libvmaf/{include,src,test}` of model surface (public API)

Four independent diffs with minimal cross-interaction. Bundling them
into one PR is coherent because: all four are upstream-port fixes,
all four are safety / correctness / ergonomics items under
Tier-0-or-4 (per the backlog's tier system), and each commit carries
its own ADR so reviewers can evaluate commit-by-commit. Splitting
into four PRs would multiply review overhead without adding review
quality — each diff is <50 LoC (except T4-6's test, slightly larger).

### Initial scope included a larger fifth item

The original audit grouped [Netflix#1430][pr1430] (thread-local RNG
cleanup) into this batch as T4-3. On first-read inspection the diff
is +600/-11 with a new subsystem (`thread_locale.c`), which does not
belong in a "small-fix sweep" narrative. Held out to its own PR;
backlog now tracks it as `T4-3-standalone`.

[pr1430]: https://github.com/Netflix/vmaf/pull/1430

## Alternatives explored

- **Cherry-pick upstream SHAs via `/port-upstream-commit`** — rejected
  because `mergeCommit` is null (PRs unmerged). Cherry-pick by PR-
  head SHA works but leaves the fork diverging invisibly; explicit
  port-with-ADR is more auditable.
- **Wait for Netflix to merge** — rejected because the oldest PR is
  21 months old with no merge signal; the T0-1 correctness issue
  (Netflix#1381 assert-0) lives in the fork in the meantime.
- **Port each PR verbatim, defer test refactor / defect fixes to a
  separate PR** — rejected. Landing a known-buggy port and fixing
  it in a follow-up doubles the git noise and lets UBSan-failing
  code sit on master between the two merges. Better to fix during
  the port and document the deviation in the ADR.

## Open questions

- When (or if) Netflix merges any of these four PRs, the
  `/sync-upstream` will see conflicts in the files carrying the
  fork-local corrections. Resolution is always "keep fork version"
  — [`rebase-notes.md`](../rebase-notes.md) tracks this expectation.
- [Netflix#1430](https://github.com/Netflix/vmaf/pull/1430) thread-
  local RNG port is unresolved; open as `T4-3-standalone` in the
  backlog. Size classification was wrong in the original audit —
  update audit-tier heuristic to require a line-count sanity check.

## Related

- ADRs: [ADR-0131](../adr/0131-port-netflix-1382-cumemfree.md),
  [ADR-0132](../adr/0132-port-netflix-1406-feature-collector-model-list.md),
  [ADR-0134](../adr/0134-port-netflix-1451-meson-declare-dependency.md),
  [ADR-0135](../adr/0135-port-netflix-1424-expose-builtin-model-versions.md)
- PRs: VMAFx/vmafx#TBD (this Batch-A PR)
- Upstream issues: Netflix/vmaf#1381
- Upstream PRs: Netflix/vmaf#1382, #1406, #1451, #1424
