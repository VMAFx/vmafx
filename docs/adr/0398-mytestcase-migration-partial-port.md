<!-- markdownlint-disable MD060 -->
# ADR-0398: MyTestCase upstream migration — partial port (golden-pinned files deferred)

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: testing, upstream-sync, python

## Context

Netflix/vmaf landed a 21-commit cluster between `7d1ad54b..2363a106` (early
May 2026) that adopts a new `MyTestCase` base class throughout `python/test/`,
adds new test methods for the `aim` / `adm3` / `motion3` features exposed by
`VmafFeatureExtractor` v0.2.21 (upstream PR #1519, ADR-0219 on the fork side),
widens `assertAlmostEqual` tolerances for macOS FP precision, replaces ad-hoc
FFmpeg temporal slicing with pre-sliced YUV fixtures, and reformats the
BD-rate calculator test data into snake_case + one-per-line style. The user
direction (2026-05-08) was: "full migration, but with our fork rules of
course".

The fork's CLAUDE.md §1 / §8 names five "Netflix golden-data" files —
`quality_runner_test.py`, `vmafexec_test.py`, `vmafexec_feature_extractor_test.py`,
`feature_extractor_test.py`, `result_test.py` — whose `assertAlmostEqual` score
literals are inviolable per [ADR-0024](0024-netflix-golden-preserved.md). The
fork has independently mutated several of those literals over time
(loosened-precision motion-mirror bug fixes, vif_sigma_nsq port, etc.), so
the in-tree file state has diverged from upstream's pre-cluster state by
hundreds of values, hundreds of lines, and several test method names.
Cherry-picking the cluster commit-by-commit produces structural conflicts
that cannot be resolved without either (a) silently overwriting fork golden
values with upstream's, or (b) leaving the fork's tests structurally broken
(orphan `results` references, duplicate method definitions, dropped setup
code).

## Decision

We port the **structurally-clean subset** of the cluster — testutil fixture
expansions, the BD-rate snake_case rename, `MyTestCase` adoption in
asset/bootstrap/local_explainer tests, and the project-wide whitespace +
import-order alignment — and **defer** the golden-pinned-file commits to a
follow-up port. The deferred commits' end state is byte-identical preservation
of all 660 fork (`key`, `value`) assertion pairs across the five
golden-pinned files; the upstream additions (aim/adm3/motion3 test methods,
macOS-FP tolerance widenings on lines that don't exist in the fork yet) are
deferred until the fork's C-side feature surface and test method layout are
reconciled with upstream's.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Cherry-pick the entire cluster with `-X theirs` | One commit, minimal manual work | Silently overwrites fork golden values (verified: 4 SSIM/PSNR keys lost on commit 1 alone) | Violates ADR-0024 / CLAUDE §1 |
| Per-commit cherry-pick with auto-resolver that preserves fork values | Maintains commit attribution per-commit | Resolver kept_head case drops upstream's setup lines, leaving orphan assertions; produces structurally-broken files (verified: F821 cascades on routine_test, asset_test, feature_extractor_test) | Ships broken tests |
| Squash to one big commit and apply cumulative diff with manual conflict resolution | Single coherent migration | Hours of manual work × high error risk; fork-value preservation requires diff-by-diff inspection of 660 literals | Time/risk-prohibitive in single session |
| Partial port (chosen): clean subset now, golden-file commits deferred | Preserves ADR-0024 invariants verbatim; ships testutil/asset/bootstrap/local_explainer/bd_rate gains; documents the deferral path explicitly | Upstream sync surface remains partially divergent on the five golden-pinned files; future port-runner inherits the work | Best risk-adjusted progress; the deferral is documented in `docs/rebase-notes.md` and below so a future agent can pick it up cleanly |

## Consequences

- **Positive**: 4 upstream commits land with provenance preserved
  (`(cherry picked from commit ...)` trailers); fork's golden-pinned
  assertion multisets verified byte-identical via
  `/tmp/mytestcase-port/verify_golden.py`; the BD-rate, asset, bootstrap,
  local_explainer, and testutil files now match upstream's `MyTestCase` base
  class and snake_case conventions, eliminating five Conventional-Commits
  conflict surfaces in the next sync.
- **Negative**: the five golden-pinned files retain the fork's pre-cluster
  layout. They will continue to conflict against upstream on every future
  sync until the deferred commits are ported (or upstream forks again).
  The new aim/adm3/motion3 test methods are absent from the fork even though
  the fork's C-side has the underlying features — coverage gap.
- **Neutral / follow-ups**: when a future session retries the deferred
  port, the prerequisite work is (a) ensure the fork's
  `python/test/feature_extractor_test.py` test method order matches
  upstream's post-cluster layout (psnr → ssim → ms_ssim → ansnr, not the
  current psnr → ansnr → ssim → ms_ssim), and (b) hand-merge the
  aim/adm3/motion3 additive blocks. The `/tmp/mytestcase-port/verify_golden.py`
  helper (and its baseline JSON `/tmp/mytestcase-port/baseline-pairs.json`)
  remain valid as the gate.

## References

- Source: `req` (user 2026-05-08: "full migration, but with our fork rules of
  course"; "stay in your lane: python/test/ and python/vmaf/ only")
- Related ADRs: [ADR-0024](0024-netflix-golden-preserved.md) (golden-data
  inviolability), [ADR-0108](0108-deep-dive-deliverables-rule.md)
  (deep-dive deliverables), [ADR-0219](0219-motion-v2.md) (fork's
  motion-v2 / aim port)
- Upstream commits ported (4): `7df50f3a`, `38e905d1`, `e3827e4d`,
  `cf02b126` (plus `25ff9f18` as a no-op since the fork already removed
  the `VmafossexecCommandLineTest` stub)
- Upstream commits deferred (golden-pinned-file structural conflicts):
  `7d1ad54b`, `9fa593eb`, `0341f730`, `3cbf352d`+`eb3374d0`
  (revert-pair, no-op), `a3776335`, `74bdce1b`, `a333ba4c`+`403dafed`
  (revert-pair, no-op), `322ca041`, `6c097fc4`, `ead2d12b`, `4679db83`,
  `005988ea`, `3a041a97`, `3e075107`
- Verification script: `/tmp/mytestcase-port/verify_golden.py` (multiset
  check on `(key, value)` pairs across the five golden-pinned files;
  PASS for all 660 baseline pairs post-port)
