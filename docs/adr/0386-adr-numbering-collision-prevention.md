<!-- markdownlint-disable MD013 MD060 -->
# ADR-0386: ADR Number Collision Prevention — Hook + CI Gate + Helper Script

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `ci`, `docs`, `git`, `agents`

## Context

On 2026-05-10 (session "chore/adr-collision-and-mcp-backfill") seven ADR number
collisions occurred in a single evening.  Each collision required a follow-up
renumber PR, adding noise to the merge train and burning CI minutes:

| PR    | Claimed | Renamed to | Collided with                        |
|-------|---------|------------|--------------------------------------|
| #695  | 0374    | 0377       | disabled-build ADR                   |
| #702  | 0375    | 0378       | hip-batch3                           |
| #706  | 0377    | 0379       | hip-batch4                           |
| #734  | 0381    | 0382       | vulkan-vif-scale-precision           |
| #739  | 0382    | 0383       | y4m                                  |
| #742  | 0383    | 0384       | k150k                                |
| #744  | 0384    | 0385       | shfmt-fix (PR #743)                  |

Root cause: agents claim a number by running `ls docs/adr/` **on the branch at
creation time**.  By merge time, master has advanced and another agent has claimed
the same number.  This is a pure race condition: no single agent is at fault, and
no human review step currently catches it before merge.

Two distinct failure modes require two complementary defences:

1. **Local working-tree collision** (two files with the same prefix exist in the
   tree simultaneously, e.g. after an incomplete rename): caught by the pre-commit
   hook.
2. **Cross-branch race** (PR adds NNNN that was free when the branch was cut but
   has since merged on master): the local hook cannot see this; requires a CI gate
   with access to origin/master.

## Decision

We will implement a layered three-piece defence:

1. **`scripts/adr/next-free.sh`** — a helper that fetches origin/master and
   prints the next free 4-digit ADR number, accounting for both local working-tree
   files and origin/master.  All ADR-authoring procedures (template, README) direct
   authors to use this script; hand-picking a number is explicitly warned against.

2. **`scripts/ci/check-adr-numbering.sh`** + pre-commit hook entry — a local guard
   that runs on every commit that stages a `docs/adr/NNNN-*.md` file.  It checks:
   (a) no other file in the working tree shares the same NNNN, and
   (b) the `# ADR-NNNN:` heading inside the file matches the filename.

3. **`adr-collision-check` CI job** in `rule-enforcement.yml` — a blocking PR gate
   that fetches origin/master and verifies that every ADR file *added* in the PR
   has a prefix not already present on master.  This is the cross-branch race guard.

Together the three pieces cover both failure modes.  A local hook without a CI gate
would still miss the race; a CI gate alone would not catch in-tree duplicates during
interactive development.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pre-commit hook only | Zero CI cost; catches local duplicates | Cannot see origin/master; cross-branch race still undetected | The 7 collisions all involved race; hook alone would not have prevented them |
| CI gate only | Catches the race; no contributor tooling change | No local feedback; contributor must push to learn of collision | Slow feedback loop; contradicts "fail fast locally" principle |
| Hook + CI gate (chosen) | Defense in depth; fast local feedback + authoritative race guard | Slightly more implementation surface | Best trade-off; both failure modes are covered |
| Central number-registry file (`docs/adr/.last-number`) | Single source of truth; trivially scriptable | Race-prone at checkout time; merge conflicts on every ADR PR | Trades the existing problem for a different merge-conflict problem |
| GitHub bot / Actions app auto-assigns numbers | No local tooling needed; fully centralised | Requires an external app or complex Actions token flow | Engineering overhead disproportionate to the problem size |

## Consequences

- **Positive**: ADR number collisions are prevented at the earliest possible point
  (commit time locally, PR-open time in CI).  Renumber PRs should cease.
- **Positive**: The `next-free.sh` script is self-documenting and fast (<1 s on a
  warm clone); contributors don't need to memorise the current high-water mark.
- **Positive**: The heading-consistency check prevents a class of copy-paste error
  (author copies template, renames file, forgets to update heading).
- **Negative**: Every branch that authors an ADR must be online at commit time for
  `next-free.sh` to include origin/master data.  Offline development degrades
  gracefully (the fetch failure is soft; the CI gate is the hard backstop).
- **Neutral / follow-ups**: The CI gate is added to `rule-enforcement.yml` alongside
  the existing `adr-backfill-check` job.  The new job is **blocking** (no
  `continue-on-error: true`), consistent with other collision-prevention gates.
  The `docs/adr/README.md` and `docs/adr/0000-template.md` are updated to direct
  authors to `scripts/adr/next-free.sh`.

## References

- Motivating event: session "chore/adr-collision-and-mcp-backfill", 2026-05-10;
  7 collisions enumerated in Context above.
- Research digest: `docs/research/0386-adr-numbering-collision-2026-05-10.md`.
- Related ADRs: [ADR-0028](0028-adr-maintenance-rule.md) (ADR maintenance rule),
  [ADR-0106](0106-adr-backfill-policy.md) (ADR backfill policy),
  [ADR-0124](0124-automated-rule-enforcement.md) (automated rule enforcement).
- Source: user direction in session 2026-05-10 (`req`).
