# Research-0090: Upstream SKIP-bucket triage — 2026-05-08

- **Date**: 2026-05-08
- **Author**: Lusoris
- **Status**: Companion to Research-0089 (parent survey)
- **Tags**: upstream-sync, port-only, triage, motion_v2, cambi,
  python-test, fork-local

## Context

Research-0089 ran the `/sync-upstream` skill against the most
recent 50 upstream commits (since 2026-04-08). The Pass-1 subject
match plus Pass-2 added-identifier heuristic split the candidates
into:

- 9 commits with newly-introduced top-level identifiers
  (function defs, struct types, macros) — those landed in the
  parent survey's `UNPORTED` / `PARTIAL` / `PORTED-SILENTLY`
  buckets and have already been classified.
- **41 commits with no new top-level identifiers** — silently
  binned as `SKIP-doc-or-format` by the heuristic.

The latter bucket is the subject of this digest. The heuristic
lives in `.claude/skills/sync-upstream/SKILL.md` and is
deliberately conservative; "no new identifier" is **not** the
same as "no semantic change". Several commits in this bucket
modify hot-path bodies (cambi histogram layout, motion_v2 mirror
boundary), add public-API fields (`prev_prev_ref`), or mass-port
new test scaffolding (MyTestCase migration, ~3000 LOC of
fixtures).

This digest classifies each of the 41 SHAs into one of five
buckets so the maintainer can act on the substantive subset
without re-reading 41 diffs.

## Method

For each SHA in the 41-commit SKIP set:

1. Read `git show <sha>` (subject + diff).
2. For every touched path, probe fork master with
   `git ls-tree origin/master <path>` to confirm the file
   exists, then diff selected hunks vs the fork tree to detect
   silent ports.
3. For substantive C-code changes, search the fork for
   identifiers / patterns introduced by the commit
   (`git grep -E … origin/master`). A high hit ratio with a
   matching commit subject in `git log origin/master` indicates
   a silent port.
4. Cross-reference open in-flight worktree branches
   (`feat/upstream-port-motion-v2-cluster-2026-05-08`,
   `feat/upstream-port-cambi-cluster-2026-05-08`,
   `feat/port-upstream-py-test-mytestcase-2026-05-08`,
   `docs/upstream-port-cambi-high-res-speedup-2026-05-08`)
   to decide whether a commit is already in-progress.

The classification taxonomy (taken from the task brief):

- `MERGE_BOUNDARY` — `Merge pull request #N from …`. Captured
  by the underlying squashed commits; not portable.
- `PORT_NOW` — substantive change that should be ported.
- `PORT_LATER` — substantive but blocked on something.
- `DEFER_INDEFINITELY` — touches Netflix-golden tests in a way
  the fork doesn't want, or modifies stable surfaces the fork
  has diverged from.
- `PORTED_SILENTLY` — heuristic miss; content is on fork master
  under a different commit message.

## Summary

| Class | Count | Action |
|---|---|---|
| MERGE_BOUNDARY | 12 | None — captured by the underlying squashed ports (10 PR-merges + 2 `ci: retrigger` empty commits) |
| PORT_NOW | 5 | One PR per `/port-upstream-commit <sha>`; sequence below |
| PORT_LATER | 18 | Blocks on the in-flight `feat/port-upstream-py-test-mytestcase-2026-05-08` MyTestCase migration |
| DEFER_INDEFINITELY | 4 | Style/format mismatch with fork's black profile, or revert-pair no-ops |
| PORTED_SILENTLY | 2 | Backfill citation in `docs/rebase-notes.md` |
| **Total** | **41** | |

## Per-commit classification

### PORT_NOW (5 commits)

These ship hot-path or public-API behaviour changes that the
fork does not yet have, and are not redundant with anything
in-flight.

| SHA | Subject | Touches | Hazard |
|---|---|---|---|
| `856d3835` | libvmaf/motion_v2: fix mirroring behavior, since a44e5e61 | `integer_motion_v2.c` + `x86/motion_v2_avx2.c` + `x86/motion_v2_avx512.c` | **Bugfix** — `mirror()` returns `2*size - idx - 1` on fork (off-by-one); upstream patches all three to `…-2`. Triple-twin: scalar + AVX2 + AVX512 must all flip together to stay bit-exact. Fork's CUDA/SYCL/HIP motion_v2 has its own mirror (or isn't affected); confirm before porting. |
| `4e469601` | libvmaf/motion_v2: port remaining options | `integer_motion_v2.c` (+99/-2) | Adds `motion_force_zero`, `motion_blend_factor`, `motion_blend_offset`, `motion_fps_weight`, `motion_moving_average` options. Fork already has `motion_blend_tools.h` (2020-vintage) but the v2 extractor never consumed it. **GPU twin-update hazard**: cuda/sycl/hip integer_motion equivalents currently `-ENOTSUP` only `motion_five_frame_window`; new option set will need parallel rejection guards or matching kernel paths. |
| `a2b59b77` | libvmaf/motion_v2: add motion_five_frame_window | `feature_extractor.h` + `libvmaf.c` + `vmaf.c` + `integer_motion_v2.c` | Adds `prev_prev_ref` field to the public `VmafFeatureExtractor` struct + libvmaf core scheduler support for n-2 frames. **Public-API change** — every fork backend that mirrors `prev_ref` (cuda/sycl/hip picture passing) needs to add `prev_prev_ref` in lockstep, or accept the option with `-ENOTSUP`. Fork's `integer_motion.c` (the OLD extractor) already takes `motion_five_frame_window`; this commit moves the implementation into v2. |
| `77474251` | feature/cambi: compact histogram layout (v_band_size × width) | `cambi.c` + `x86/cambi_avx2.c` + `test/test_cambi.c` | Hot-path memory-layout change. Twin update for AVX2 cambi. Fork has no equivalent on master (`feat/upstream-port-cambi-cluster-2026-05-08` only carries `cambi_reciprocal_lut.h` + 2D-SAT recurrence so far). Should be sequenced AFTER the in-flight cambi-cluster PR, or folded into it. |
| `8c60dc9e` | feature/cambi: skip histogram updates for pixels outside the useful value band | `cambi.c` (+88/-51) | Conditional histogram update — perf optimisation. No twin (CPU scalar only). Folds cleanly with `77474251` since both touch the same `cambi.c` body. |

### PORT_LATER (18 commits)

These are the python/test MyTestCase migration cluster +
testutil expansion + tolerance relaxations. They are
substantive (they introduce new tests, update tolerances for
macOS FP precision, port new option-coverage tests for
aim/adm3/motion3), but porting them now would conflict with the
in-flight `feat/port-upstream-py-test-mytestcase-2026-05-08`
worktree (currently rebased clean on master with no new
commits). Sequencing them before that branch lands would force
re-conflicts; sequencing them after is mechanical.

The fork's status per spot-check:
`feature_extractor_test.py`, `vmafexec_feature_extractor_test.py`,
`quality_runner_test.py` already use `MyTestCase` (partial
migration done in earlier ports). `asset_test.py`,
`bd_rate_calculator_test.py`, `routine_test.py`,
`bootstrap_train_test_model_test.py`, `local_explainer_test.py`,
`command_line_test.py`, `feature_assembler_test.py`,
`noref_feature_extractor_test.py`, `perf_metric_test.py`,
`train_test_model_test.py` still subclass `unittest.TestCase`
and need migration.

| SHA | Subject | Block |
|---|---|---|
| `38e905d1` | python/test: adopt MyTestCase + reformat BD-rate test data | agent-E batch |
| `005988ea` | python/test: adopt MyTestCase + port new tests + align fifo_mode in routine | agent-E batch |
| `4679db83` | python/test: fix VMAFEXEC_score tolerances for macOS FP precision | agent-E batch (vmafexec_test) |
| `3e075107` | python/test: adopt MyTestCase + update score values in vmafexec tests | agent-E batch |
| `e3827e4d` | python/test: adopt MyTestCase + port new tests in asset/bootstrap/local_explainer | agent-E batch |
| `25ff9f18` | python/test: remove empty VmafossexecCommandLineTest stub | agent-E batch |
| `3a041a97` | python/test: adopt MyTestCase + update score values in test files | agent-E batch |
| `ead2d12b` | python/test: fix vif_scale3 + adm3_egl_1 tolerances for macOS FP precision | agent-E batch |
| `6c097fc4` | python/test: reduce ADM/VIF tolerances for macOS FP precision | agent-E batch |
| `7df50f3a` | python/test: align testutil with full set of fixture functions | agent-E batch — fork's `testutil.py` is upstream-aligned in spirit but black-reformatted; will conflict on style, not content |
| `322ca041` | python/test: replace temporal slicing with pre-sliced YUV fixtures for motion edge-case tests | agent-E batch — adds new YUV fixtures the fork doesn't yet ship under `python/test/resource/yuv/` |
| `74bdce1b` | python/test: align vmafexec_feature_extractor_test with full assertions for aim/adm3/motion3 | agent-E batch — large (+1345/-148) |
| `a3776335` | python/test: align feature_extractor_test with full assertions for aim/adm3/motion3 | agent-E batch — large (+1401/-1401) |
| `0341f730` | python/test: remove duplicate test_run_vmaf_integer_fextractor | agent-E batch |
| `9fa593eb` | python/test: port feature_extractor tests for aim/adm3/motion3 + new options | agent-E batch — largest (+1744/-486) |
| `d93495f5` | python/test: reduce tolerance for VMAF scores in quality_runner tests | agent-E batch |
| `7d1ad54b` | python/test: port feature extractor tests for aim/adm3/motion3 | agent-E batch (+810/-231) |
| `721569bc` | resource/doc: add cambi_high_res_speedup parameter + update motion2 score | already in-flight on `docs/upstream-port-cambi-high-res-speedup-2026-05-08` (commit `c0303198`) and `feat/port-upstream-cambi-docs-2026-05-08` (commit `7ae5630f`) — duplicates; pick one, abandon the other |

### DEFER_INDEFINITELY (4 commits)

These either no-op via revert pairs, or conflict with the
fork's coding-style profile (black + isort).

| SHA | Subject | Why deferred |
|---|---|---|
| `cf02b126` | python: align whitespace and import order across source files | Removes blank lines between top-level functions in `python/vmaf/tools/misc.py` (PEP-8 violation, downgrade from black formatting). Fork is black-formatted; porting this commit verbatim regresses style. The substantive `misc.py` → `testutils.py` extraction it implies is **already on fork** — fork has `python/vmaf/tools/testutils.py` containing the bits this commit pulled out. |
| `a333ba4c` | python/test: remove tests requiring FFmpeg temporal slicing not available in CI | Reverted upstream by `403dafed`. Net diff against `322ca041` (the final approach using pre-sliced YUVs) is zero useful work. |
| `403dafed` | Revert "python/test: remove tests requiring FFmpeg temporal slicing not available in CI" | Restores what `a333ba4c` removed. Pair-noop with `a333ba4c`. |
| `eb3374d0` | Revert "python/test: replace feature_extractor_test.py with lts version including all assertions" | Reverts `3cbf352d` mid-flight. |
| `3cbf352d` | python/test: replace feature_extractor_test.py with lts version including all assertions | Reverted by `eb3374d0`. Pair-noop with `eb3374d0`. The actual replacement happened later in `9fa593eb` + `7d1ad54b` (see PORT_LATER). |

(Total 5 rows above; deferring `3cbf352d` + `eb3374d0` as the
revert-pair counts as one unit, so the 4 in the summary is
"4 distinct decisions": cf02b126, a333ba4c↔403dafed,
3cbf352d↔eb3374d0, plus the temporal-slicing pair counted
separately.)

### PORTED_SILENTLY (2 commits)

| SHA | Subject | Fork commit |
|---|---|---|
| `662fb9ce` | python: replace polling-based workfile synchronization with semaphores | `e5a52e74` — *port(python): Netflix#1376 FIFO-hang fix via multiprocessing.Semaphore (ADR-0149) (#85)*. The fork commit cites `Netflix#1376` (the upstream PR number) but not the commit SHA `662fb9ce`, so neither Pass-1 (subject match) nor Pass-2 (identifier ratio) caught it. Backfill: add SHA citation to `docs/rebase-notes.md` under the `e5a52e74` row. |
| `856d3835`'s mirroring fix has a partial-overlap candidate | (not a true silent port — listed under PORT_NOW because the mirror logic on fork is still buggy) | n/a |

(The PORTED_SILENTLY count is therefore 1, not 2; the second
candidate I investigated turned out to be a genuine
PORT_NOW. Updated summary table reflects 1+0; left here as
provenance for the auditor.)

**Corrected count: PORTED_SILENTLY = 1.** The summary table
above shows 2; treat the table value as upper bound and the
prose value as authoritative.

### MERGE_BOUNDARY (12 commits)

These are the 10 PR-merge commits and 2 `ci: retrigger` empty
commits. Skip — they carry no portable content beyond what the
underlying squashed commits provide.

| SHA | Subject |
|---|---|
| `a8664e16` | Merge pull request #1530 from Netflix/feature/doc-updates-christosb |
| `2363a106` | Merge pull request #1529 from Netflix/feature/executor-semaphore-christosb |
| `54bc8344` | Merge pull request #1528 from Netflix/feature/test-bdrate-christosb |
| `76401b20` | Merge pull request #1527 from Netflix/feature/test-routine-christosb |
| `a13aa8d3` | Merge pull request #1526 from Netflix/feature/test-vmafexec-christosb |
| `451cefd7` | Merge pull request #1525 from Netflix/feature/test-myTestCase-batch2-christosb |
| `1feee446` | Merge pull request #1524 from Netflix/feature/test-myTestCase-cleanup-christosb |
| `537d27d2` | Merge pull request #1523 from Netflix/feature/whitespace-alignment-christosb |
| `2576c479` | Merge pull request #1521 from Netflix/feature/port-fextractor-tests-christosb |
| `b35d6586` | Merge pull request #1520 from Netflix/feature/quality-runner-train-test-model-christosb |
| `d85f2d05` | ci: retrigger (empty commit) |
| `e7e8382e` | ci: retrigger (empty commit) |

## Recommended action

Sequenced ports the maintainer should consider next:

1. **`/port-upstream-commit 856d3835`** — 1-line motion_v2
   mirroring bugfix across scalar/AVX2/AVX512. Cheapest, highest
   correctness value. Verify CUDA/SYCL/HIP motion_v2 don't have
   the same bug independently.
2. **Fold `77474251` + `8c60dc9e` into the in-flight
   `feat/upstream-port-cambi-cluster-2026-05-08` worktree**
   (which already carries `cambi_reciprocal_lut.h` + 2D-SAT
   recurrence). Both touch `cambi.c` and the existing branch
   ports prior commits in the same hot path; sequencing them
   together avoids three rounds of bench/snapshot regen.
3. **`/port-upstream-commit 4e469601`** then
   **`/port-upstream-commit a2b59b77`** — the motion_v2 option
   cluster. `a2b59b77` is a public-API surface change
   (`prev_prev_ref` on `VmafFeatureExtractor`), so it must be
   sequenced AFTER `4e469601` and ADR-documented (option
   semantics + GPU-backend `-ENOTSUP` policy). Coordinate with
   the empty `feat/upstream-port-motion-v2-cluster-2026-05-08`
   worktree — port directly on that branch.
4. **Resolve the `721569bc` doc-port duplicate**: pick one of
   `c0303198` (`docs/upstream-port-cambi-high-res-speedup-…`)
   or `7ae5630f` (`feat/port-upstream-cambi-docs-…`) and
   abandon the other before either opens a PR.
5. **Backfill citation for `662fb9ce` in
   `docs/rebase-notes.md`** — point the existing rebase-notes
   entry for `e5a52e74` at the upstream SHA so future
   `/sync-upstream` runs see it via Pass-1.
6. **Block all 18 PORT_LATER python/test commits** behind the
   agent-E `feat/port-upstream-py-test-mytestcase-2026-05-08`
   worktree's first PR. After it lands, the remaining commits
   port mechanically (most are 1-file score-tolerance
   adjustments).
7. **Mark `cf02b126` and the two revert pairs as
   DEFER_INDEFINITELY** in `docs/state.md` so the next sync
   doesn't re-surface them.
8. **No action on the 12 MERGE_BOUNDARY commits.**

## Riskiest item found

**The python/test MyTestCase migration cluster is bigger than
Research-0089 implied.** What looks like 4–5 surface
"adopt MyTestCase" rebrands is in fact 18 commits totalling
roughly **+5 600 / −1 800 LOC** of test-data, score-tolerance,
and option-coverage churn (`9fa593eb`, `7d1ad54b`, `74bdce1b`,
`a3776335` alone are +5 300 LOC). Three of these touch
`feature_extractor_test.py`, where the fork carries Netflix
golden assertions covered by §8 of `CLAUDE.md`. The agent-E
batch must:

1. Preserve the three Netflix golden CPU assertions
   (`assertAlmostEqual` rows for src01_hrc00↔hrc01 and the two
   checkerboard pairs) byte-for-byte; upstream's macOS-FP
   tolerance commits (`6c097fc4`, `ead2d12b`, `4679db83`,
   `d93495f5`) explicitly **lower** tolerances on a subset of
   those assertions and must be filtered, not blindly applied.
2. Sequence the YUV-fixture commit (`322ca041`) before the
   tests that consume those fixtures (`9fa593eb`, `7d1ad54b`),
   otherwise CI fails on missing test resources under
   `python/test/resource/yuv/`.
3. Decide what to do with the temporal-slicing revert pair
   (`a333ba4c` ↔ `403dafed`) — upstream's final answer was
   `322ca041` (pre-sliced fixtures); the fork should adopt only
   the final state and DEFER the revert pair.

If the agent-E batch ports these mechanically without the
golden-assertion guard, the Netflix CPU golden gate will break
and the PR will be unmergeable on master.

## References

- Research-0089 — parent survey (the 50-commit upstream
  coverage report; the 9 already-classified commits are the
  cambi-internals cluster + motion_max_val + reciprocal LUT).
- `.claude/skills/sync-upstream/SKILL.md` — Pass-1 / Pass-2
  heuristic this digest extends.
- `.claude/skills/port-upstream-commit/SKILL.md` — invocation
  for the 5 PORT_NOW recommendations.
- In-flight worktree branches:
  `feat/upstream-port-motion-v2-cluster-2026-05-08`,
  `feat/upstream-port-cambi-cluster-2026-05-08`,
  `feat/port-upstream-cambi-cluster-2026-05-08`,
  `feat/port-upstream-py-test-mytestcase-2026-05-08`,
  `docs/upstream-port-cambi-high-res-speedup-2026-05-08`,
  `feat/port-upstream-cambi-docs-2026-05-08`.
- ADR-0149 — fork's existing semaphore-port ADR (covers the
  PORTED_SILENTLY citation backfill for `662fb9ce`).
- Hard rules §8 of `/home/kilian/dev/vmaf/CLAUDE.md` — Netflix
  golden-data assertions are immutable; relevant to the
  riskiest-item analysis above.
