### fix(adr): remote-aware ADR number allocator — eliminate parallel-worktree collisions

`scripts/adr/next-free.sh --claim` now prevents collisions across isolated
worktrees and in-flight remote branches:

- **`.git/adr-claims/<NUMBER>` side-pointer**: written atomically during
  `--claim`, visible to all worktrees sharing the common `.git/` directory
  (standard `git worktree add` layout). Sibling agents see the claim
  immediately, before any push. `--release` removes the side-pointer.
- **Remote-branch scan**: `git ls-remote --heads origin` is called in the main
  shell (not a subshell) so the offline flag propagates correctly; each
  non-master branch SHA is then resolved via `git ls-tree` and its `docs/adr/`
  entries added to the taken-number set.
- **Offline fallback**: if `git ls-remote` fails, a `WARNING` is printed to
  stderr and the allocator proceeds with local + master + `.git/adr-claims/`
  only — no fatal error.
- **Lock key migrated** to the common `.git/` directory so parallel agents in
  different worktrees of the same repo contend on the same POSIX mkdir lock.
- **CI gate extended**: `adr-collision-check` in `rule-enforcement.yml` now
  has a second phase that fetches each open PR's HEAD via `gh pr list` and
  checks for number overlaps, preventing the collision class where two PRs
  claim the same number before either merges.
- **Acceptance tests** under `scripts/adr/tests/test-next-free-remote-aware.sh`
  cover: overlapping remote branches → `max+1`; offline fallback + WARNING;
  sibling worktree claim blocks number; performance smoke < 5 s on 30 branches.

Fixes the 2026-05-19 collision chain (ADR-0607 × 2, ADR-0608 × 2).
See [ADR-0628](../../docs/adr/0628-adr-allocator-remote-aware.md).
