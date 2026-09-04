<!-- markdownlint-disable MD013 MD060 -->
# ADR-1166: Harvest stale upstream Netflix/vmaf reports, verify each against the fork, fix what still bites

- **Status**: Accepted
- **Date**: 2026-09-03
- **Deciders**: Lusoris
- **Tags**: process, upstream, bug, build, windows, api, docs

## Context

Netflix/vmaf carries a long tail of open, unworked bug reports — some years old,
several with a patch attached that nobody merged. The fork diverged from
upstream a long time ago (ADR-0700 moved `libvmaf/` to `core/`, several C files
became C++, and CUDA / SYCL / HIP / Metal backends were added), so an upstream
report is never automatically applicable and never automatically stale: some
defects were fixed here years ago, some were never present, some are present
*verbatim*, and a few are **wider here than upstream** because a fork-added
backend or SIMD path copied the defective shape.

Nothing in the fork's process converted that queue into either fixes or
recorded knowledge. Every session that looked at an upstream issue re-did the
same "does this still affect us?" investigation from scratch, and the answer —
including the negative answers, which are the expensive ones to re-derive —
evaporated with the session.

The forces:

- Upstream reports are free signal about real defects in code we still ship.
- They are also noisy: a report's own diagnosis is frequently wrong about the
  fork (and sometimes about upstream), so it cannot be trusted without local
  verification.
- Porting an upstream patch verbatim is often wrong here: paths moved, the fork
  guards inputs upstream does not, and at least one upstream proposal
  (Netflix/vmaf#1422) is retracted by a later upstream PR (Netflix/vmaf#1551).
- The fork's own hard rules (CLAUDE.md §12) demand an ADR, docs, changelog,
  `docs/state.md` row and a regression test per behavioural change, which makes
  a per-issue drip of micro-PRs expensive; the user has explicitly asked for
  bundles instead of 200-item queues.

## Decision

We harvest upstream reports in periodic batches. Each candidate is **verified
against the fork's own tree** — read the real file at the real path, run the
real reproducer — and assigned one of four verdicts: ALREADY-FIXED,
NOT-APPLICABLE, AFFECTS-FORK, or NEEDS-HARDWARE. Everything that still bites and
is safe to batch is fixed in one bundled PR with a regression test per fix;
everything else gets a `docs/state.md` row citing the upstream reference and the
evidence. The complete triage table — including the negative verdicts — is
written to a numbered research digest under `docs/research/`, because that table
is the durable output of the work: it is what stops the next session
re-investigating a closed question.

Where the fork's fix deliberately differs from what upstream proposes, the
divergence and its reason go into `docs/rebase-notes.md`, so the next
`/sync-upstream` knows why the two trees disagree.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Harvest, verify, fix the confirmed subset, record the rest** (chosen) | Real defects get fixed; negative verdicts are recorded once and reused; divergences are documented for the next sync | Verification is the expensive part — each candidate needs the file read and, where possible, a reproducer | — |
| Ignore upstream reports entirely | Zero cost | Leaves live memory-safety defects in shipped code. This batch alone found a reachable heap out-of-bounds read on the plain CPU path (Netflix/vmaf#1582), a silently-wrong-score path on older Windows hardware (Netflix/vmaf#1551), and a static-link break for every downstream FFmpeg build (Netflix/vmaf#1178) | Rejected: the defects are real and reachable |
| Port every upstream patch blindly | Cheap per item; keeps the diff close to upstream | Half of them do not apply (paths moved, already fixed here), and porting Netflix/vmaf#1422's `__lzcnt` form would have *introduced* the silent-wrong-score defect upstream's own #1551 retracts. Upstream's #1178 patch is not even valid meson (`else compiler.get_id() == 'clang':`) and its clang→`-lc++` mapping is wrong on Linux | Rejected: the fork must re-derive, not transcribe |
| File findings back upstream only | Cheapest; helps everyone | Does not fix our shipped binary, and the harvested queue is evidence that upstream reports can sit for years. This session is also a read-only visitor to a repository we do not own | Rejected as the *only* action; the digest records what would be worth reporting |
| One PR per upstream issue | Small, easy reviews | Nine issues × (ADR + docs + changelog + state row + rebase note) is a merge-train stall, and the fixes overlap (three issues touch the same mirror helper) | Rejected: bundle per round, per the user's standing direction |

## Consequences

- **Positive**: the confirmed subset is fixed with a regression test each; the
  triage table means a future session answers "does upstream #N affect us?" by
  reading one file. Three of the fixes close memory-safety defects that were
  reachable from the public C API with supported inputs.
- **Negative**: two of the fixes are user-visible behaviour changes —
  `float_vif` now rejects frames below 16 px (it used to accept 9 px and read
  out of bounds at scale 3), and `float_motion` with `motion_add_uv` now
  validates the chroma planes. Both convert previously-undefined behaviour into
  a documented `-EINVAL`, but a caller feeding sub-16px frames will see a new
  error. The public `VmafFeatureDictionary` ownership contract is also now
  written the same way in all three headers, which means one of the two
  previously-contradictory readings is now explicitly wrong.
- **Neutral / follow-ups**: the digest lists the confirmed-but-not-batched
  items (Netflix/vmaf#1564, #930, #1568, #1109, #766, #818, #1305, #1494) with
  their evidence; each has a `docs/state.md` row and needs its own PR, because
  each either moves scores, changes CLI grammar, or needs hardware we do not
  have here. The Metal `motion_v2` mirror off-by-one found while triaging
  Netflix/vmaf#1580 is one of those: fixing it moves Metal scores and needs an
  Apple GPU for the parity run.

## References

- Netflix/vmaf#1580, #1242, #743, #1582, #1581, #1573, #1551, #1422, #1178 —
  the batch fixed here.
- Netflix/vmaf#1564, #930, #1568, #1109, #766, #818, #1305, #1494 — confirmed,
  deferred, one `docs/state.md` row each.
- [docs/research/1166-upstream-issue-harvest-2026-09-03.md](../research/1166-upstream-issue-harvest-2026-09-03.md)
  — the full triage table with per-candidate evidence.
- [ADR-0700](0700-vmafx-repo-layout.md) — the `libvmaf/` → `core/` rename
  that makes upstream paths non-obvious.
- [ADR-0806](0806-feature-dictionary-ownership.md) — the earlier, internally
  inconsistent codification of the dictionary ownership contract this ADR
  supersedes in substance (see the header rewrite).
- [ADR-1138](1138-c-translation-units-keep-null.md) — the `NULL`-in-C
  carve-out the new test files reuse.
- [ADR-1142](1142-whole-codebase-standards.md) — the clang-tidy ratchet the
  touched files are measured against.
- Source: `req` (paraphrased) — fix the upstream-reported defects that were
  confirmed to still affect the fork, in one bundled draft PR, and record the
  rest.
