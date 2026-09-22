- **New required CI gate `Silent-Revert Guard`** — a merge can remove work
  from `master` with nothing in its review diff looking like a removal of
  somebody else's commit. `scripts/ci/check-silent-revert.py` measures the
  real merge result (`git merge-tree --write-tree`, not the branch tree in
  isolation) and fails when it removes target work the branch never set out
  to touch: a file rewound to an older blob it held on the target's history,
  a target commit undone hunk-for-hunk, or lines dropped or resurrected by a
  conflict resolution rather than by any non-merge commit of the branch. It
  runs on every non-draft pull request and locally as
  `make silent-revert-check` (`BASE_REF=` / `HEAD_REF=` to pick the pair).
  A deliberate revert is declared where a reviewer sees it — a `revert:`
  Conventional-Commit title, or `reverts: #N` /
  `intentional revert: <reason>` in the PR body — and there is no in-tree
  suppression. The gate fails closed: an unresolvable ref, unrelated
  histories, a merge that does not resolve cleanly, or a git older than 2.38
  exit non-zero rather than report "clean". See
  [`docs/development/silent-revert-gate.md`](docs/development/silent-revert-gate.md)
  and ADR-1284.
