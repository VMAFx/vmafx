<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3295: The render lands as its own render-only pull request

- **Status**: Accepted
- **Date**: 2026-10-13
- **Deciders**: lusoris
- **Tags**: ci, docs, release, process

## Context

Under [ADR-2197](2197-render-generated-docs-at-landing.md) a pull request
carries fragments only. `CHANGELOG.md`, the ADR index (`docs/adr/README.md`,
`docs/adr/by-tag/`, `docs/adr/titles.md`, `docs/research/titles.md`) and
`docs/rebase-notes.md` are written by `make docs-render`. The merge train ran
that render at the tip of each landing batch and pushed the result straight to
`master`, and the master push ran `make docs-render-check`, so every master tip
had to be rendered.

On 2026-10-13 the bypass of master ruleset 22587111 moved to pull-request mode
with zero required reviews. From then on a direct push to `master` is refused
(`GH013`), and the train's render push failed. The fleet rule says a pull
request merges only when the forge reports it mergeable without a bypass, so
pushing the render with the bypass is not an option. Master kept landing
fragments without their render, and the Docs job of the master push went red
on every tip.

## Decision

1. **Render-only pull request.** The render lands as its own pull request
   after a landing batch. `scripts/ci/deliverables-check.sh` accepts a pull
   request whose diff touches only the rendered paths (`_order.txt` excluded,
   it stays refused) when `make docs-render-check` passes on the pull
   request's head commit, checked out in a disposable detached worktree. Such
   a pull request needs none of the ADR-0108 deliverables. A rendered file next
   to any other file, a stale render and a hand-edited render are refused as
   before.
2. **One per batch, re-rendered, never hand-merged.** There is one render pull
   request per batch. When master moves under it, its branch is rebased and
   the render is regenerated at the new base; a conflict in a rendered file is
   never resolved by hand. It merges when the forge reports it CLEAN, like
   every other pull request.
3. **Master push check.** The Docs job of the master push runs
   `scripts/ci/check_render_on_master.py` instead of `make docs-render-check`.
   A current render at HEAD passes. Otherwise the last first-parent commit
   that touched a rendered file must pass `make docs-render-check` on its own
   tree (a hand edit or a stale render is red at any age), and the render owed
   since the first landing after it passes with a warning for up to 24 hours
   and is red after that.

This decision amends ADR-2197: the render is no longer pushed with a landing
batch, and a master tip may wait up to 24 hours for its render. The rest of
ADR-2197 (fragments only in an ordinary pull request, landing order of the
index, `_order.txt` frozen) is unchanged.

## Measured

- `scripts/ci/tests/test_deliverables_render_only.py`: 9 gate cases (3
  positive, 6 planted negatives: a rendered file next to a script, next to a
  fragment, a hand-edited `CHANGELOG.md`, a stale render, `_order.txt` alone,
  a mixed diff with a broken `Makefile`) and 3 wiring cases. Against the gate
  before this change 8 cases failed; with it all pass.
- `scripts/ci/tests/test_check_render_on_master.py`: 8 cases and 1 wiring
  case, all passing. Mutations: with the exactness step of the last render
  removed, the two hand-edit cases fail; with the age bound disabled, the
  past-bound case fails.
- The gate and the check leave no worktree behind (one case each).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the train's direct push with the operator's bypass | No extra pull request; every tip rendered | A bypass merge is what the fleet rule forbids; the ruleset now refuses it | Breaks the merge-only-clean rule |
| Render inside every pull request again | Every tip rendered | Every pull request collides on the same generated files, which is why ADR-0221 and ADR-2197 exist | Brings back the collisions |
| Render-only pull request, master push still requires an exact render at every tip | Simple check | Master is red between a landing and its render pull request, every time | Red without a defect trains people to ignore the job |
| **Render-only pull request, last render exact, owed render bounded at 24 h** (chosen) | No bypass; a hand edit or stale render is still red; the window is bounded | Master may carry an owed render for up to 24 hours; one extra pull request per batch | — |

## Consequences

### Positive

- Every change to `master`, the render included, goes through a pull request
  that the forge reports CLEAN.
- A render that is not the render of its fragments is red on the pull request
  and on master.

### Negative

- `CHANGELOG.md`, the ADR index and the rebase notes on master may lag the
  fragments by up to 24 hours.
- One more pull request per landing batch, which goes BEHIND whenever master
  moves and is re-rendered at each rebase.

### Neutral / follow-ups

- The merge train's `render_generated()` still pushes to `master`; it changes
  to open or refresh the render pull request instead.
- The release cut keeps running `make docs-render` on its own branch.

## References

- [ADR-2197](2197-render-generated-docs-at-landing.md), amended here.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md), the fragment pattern.
- [ADR-0108](0108-deep-dive-deliverables-rule.md), the deliverables a render-only pull request does not need.
- Controller answer `vmafx-render-after-pr-bypass`, 2026-10-13 (paraphrased): allow a render-only pull request that the deliverables gate accepts when its head passes `make docs-render-check`, merged when CLEAN, one per batch, regenerated at its own rebase, with planted negatives shown red first.
- Controller answer `vmafx-ruleset-review-count`, 2026-10-13 (paraphrased): the master ruleset's bypass is in pull-request mode with zero reviews.
