<!-- markdownlint-disable MD060 -->
# ADR-0922: Aggressive coverage ratchet + per-PR coverage-delta gate

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: ci, coverage, gate, fork-local

## Context

The fork's Coverage Gate (`scripts/ci/coverage-check.sh`, governed by
[ADR-0110](0110-coverage-gate-fprofile-update-atomic.md),
[ADR-0114](0114-coverage-gate-per-file-overrides.md),
[ADR-0117](0117-coverage-gate-warning-noise-suppression.md), and
[ADR-0637](0637-ci-test-failures-omnibus.md)) historically tracked the
**measured** floor rather than the **aspirational** target.
[`docs/principles.md` §3](../principles.md) names 70 % overall and 85 %
critical, but the live floor in CI sat at 37 % overall and 85 % critical
after the 2026-05-19 merge burst (~2 200 LOC of new MCP/HIP/DNN/scaffold
C that the existing test suite did not yet reach).

Tracking-floor is honest about the present but creates two problems:

1. **No upward pressure.** A floor that follows measured coverage downward
   never asks anyone to invest in tests. Coverage stays where the cheapest
   path leaves it.
2. **No per-PR ratchet.** Even with the absolute floor in place, an
   individual PR can quietly drop overall coverage from 50 % to 38 % as
   long as it stays above 37 %. The next PR can drop from 38 % to 37.01 %.
   Coverage decays one PR at a time and the gate never fires.

The fork already enforces the absolute floor via `coverage-check.sh`. The
missing piece is a **per-PR delta gate** that rejects any PR which erodes
coverage on the files it touches (or overall) past a small tolerance,
regardless of whether the absolute floor is still met.

This ADR raises the absolute floors aggressively (37 % → 70 % overall,
85 % → 90 % critical, plus +5pp on every per-file override in
`PER_FILE_MIN`) and introduces a new per-PR coverage-delta gate
(`scripts/ci/coverage-delta-check.sh`) wired into the Coverage Gate job
on pull-request events. Note: the original PR proposed 60 % overall;
master's post-merge coverage uplift from #420 and #412 allowed the
floor to be set to 70 % on recovery.

## Decision

We will:

1. **Raise the absolute floors** in `scripts/ci/coverage-check.sh`:
   - `OVERALL_MIN`: 37 → 70 (original PR proposed 60; raised to 70 on
     recovery because master already measured above 70 after #420/#412)
   - `CRITICAL_MIN` (default per-file critical floor): 85 → 90
2. **Tighten every `PER_FILE_MIN` override by +5pp** (the
   per-file structural-ceiling exemptions established by ADR-0114):
   - `core/src/dnn/ort_backend.c`: 78 → 83
   - `core/src/dnn/dnn_api.c`: 78 → 83
   - `core/src/dnn/tiny_extractor_template.h`: 10 → 15

   No override is *lowered* relative to its prior value — the ratchet is
   one-way by design. Future per-file overrides may be raised further but
   not lowered without a new ADR superseding this one.
3. **Introduce `scripts/ci/coverage-delta-check.sh`**, a per-PR gate that
   compares head vs. merge-base gcovr summaries and fails if:
   - Overall coverage drops by more than `--max-overall-drop`
     (default **0.5pp**), OR
   - Any file present in both reports AND touched by the PR's diff drops
     by more than `--max-file-drop` (default **0.5pp**).

   New files have no base row to compare to and are covered by the
   absolute floors instead. Files not touched by the PR are not scored
   (the absolute gate already covers their floor).
4. **Wire the new gate into `tests-and-quality-gates.yml`** as two steps
   added to the existing `coverage` job:
   - A lean CPU-only-fast build at the PR's merge-base (no ORT, no
     Python suite) produces `/tmp/base-coverage.json`.
   - `coverage-delta-check.sh` consumes the base + head JSONs plus
     `git diff --name-only "$MERGE_BASE"..HEAD`.

   The two steps only run on `github.event_name == 'pull_request'` so
   `push` events to `master` continue to use the absolute gate alone.
5. **Grace period for in-flight PRs.** PRs opened **before
   2026-05-31** are exempt from the ratchet for **30 days** (until
   **2026-06-30**). The exemption is operational rather than enforced in
   code: reviewers may merge such PRs with the new gate failing if (a)
   the PR predates this ADR and (b) the failure is purely due to the
   new floor or the new delta gate. Past the grace window, every PR is
   subject to the new gates regardless of when it was opened.
6. **Exception process.** The new floors and the delta gate may be
   loosened only by a **new ADR that explicitly supersedes ADR-0922**
   and is referenced inline at the changed threshold in
   `coverage-check.sh` / `coverage-delta-check.sh`. Inline
   `# noqa`-style escape hatches are not permitted. Per-file overrides
   added to `PER_FILE_MIN` after this ADR must still cite the ADR that
   justifies each lower bar (the ADR-0114 pattern remains in force).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raise only the overall floor (37 → 70) without delta gate | Simplest change | Doesn't stop one-PR-at-a-time decay below 70.49 % | Solves half the problem; the delta gate is the structural fix |
| Raise floors incrementally (37 → 45 → 55 → 70 over months) | Less disruption to in-flight PRs | Delays the upward pressure for weeks; humans forget to ratchet | The 30-day grace window achieves the same softness without sustained planning overhead |
| Lower the delta tolerance to 0.1pp | Tighter ratchet | Too noisy — gcov small-loop hit-count variance can move per-file percentages by ~0.2pp without any source change | 0.5pp is comfortably above measured variance and still catches real regressions |
| Apply delta gate to every file (not just touched) | Catches regressions caused by unrelated test changes | Penalises PRs for shared-test-suite drift outside their diff | Touched-file scoping keeps the gate actionable; overall delta already catches whole-tree drift |
| Skip the per-file override ratchet (only raise headline numbers) | Doesn't move the ADR-0114 baseline | Lets the exemptions decay relative to the headline | +5pp keeps the exemption gap constant in absolute terms |
| Make the delta gate advisory (continue-on-error) | Soft rollout | Soft gates become permanent — that's how 37 % survived for 12 days | Required from day 1, but with the 30-day grace window for in-flight PRs |

## Consequences

**Positive:**

- The Coverage Gate now matches the aspirational target in
  `docs/principles.md §3` (overall 70 % matches the documented 70 %
  goal; 90 % critical exceeds the 85 % goal).
- Per-PR ratchet prevents one-PR-at-a-time coverage decay. A PR that
  drops `foo.c` from 80 % → 75 % now fails its own job, not the next
  PR's job months later.
- Per-file override ratchet keeps the ADR-0114 exemptions honest: the
  +5pp tightening is small enough to be reachable today (current
  measurements sit above the new floors per the latest CI run) but
  large enough that drift is visible.
- The delta gate's failure messages name the exact files and deltas
  involved and point the contributor at the supersede-ADR process,
  so failures are actionable rather than mysterious.

**Negative:**

- Coverage Gate wall-clock time increases on PRs: the base-coverage
  build adds roughly one full CPU-only `meson setup + ninja + meson test`
  cycle on top of the existing head build. Measured locally at ~4 minutes
  added on `ubuntu-latest`. The Coverage Gate is non-blocking for `push`
  events (job runs unchanged), so this only affects PR CI latency.
- The 30-day grace period requires reviewer discipline to apply
  correctly. There is no automated marker for "PR predates the ratchet";
  reviewers cross-check the PR's open-date against 2026-05-31 manually.
  After 2026-06-30 the rule self-disables and the policy is uniform.
- New files added in a PR are not scored by the delta gate (they have
  no base row). The absolute critical floor still applies to files
  under `core/src/dnn/`, `core/src/opt.c`, and
  `core/src/read_json_model.c`. Non-critical new files are protected
  only by the 70 % overall floor — a PR could ship a wholly-new
  `core/src/foo.c` at 30 % coverage without tripping the delta gate.
  This is the same gap the existing absolute floors have; a future ADR
  could add a per-new-file absolute floor if drift shows up.

**Neutral / follow-ups:**

- `docs/principles.md §3` numbers (70 % / 85 %) are now matched or
  exceeded by this ratchet. The critical floor raised to 90 %; the
  overall floor now matches the documented 70 % target, with ADR-0922
  cited.
- The next ratchet (70 → 80 %) should land via a follow-up ADR once two
  consecutive CI weeks show overall sitting above 75 %.
- `coverage-delta-check.sh` has a smoke-test suite in this PR's
  reproducer section; consider promoting it to a tracked test under
  `scripts/ci/tests/` once we add the directory structure for
  CI-script tests.

## References

- [ADR-0110](0110-coverage-gate-fprofile-update-atomic.md) — atomic gcov
  counters; foundation of the current gate.
- [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) — `lcov → gcovr`
  migration that made per-file numbers honest.
- [ADR-0114](0114-coverage-gate-per-file-overrides.md) — the
  `PER_FILE_MIN` map this ADR tightens.
- [ADR-0117](0117-coverage-gate-warning-noise-suppression.md) — stderr
  filter for `gcovr` suspicious-hits noise; preserved unchanged.
- [ADR-0637](0637-ci-test-failures-omnibus.md) — recorded the 40 % → 37 %
  drop and committed to ratcheting upward as targeted tests landed; this
  ADR executes that ratchet.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — changelog fragment
  pattern used by this PR's `changelog.d/changed/` entry.
- Source: `req` (paraphrased): user directed an aggressive ratchet of the
  coverage thresholds plus a per-PR coverage-delta gate, with a grace
  period for in-flight PRs and an exception process gated on a follow-up
  ADR.
