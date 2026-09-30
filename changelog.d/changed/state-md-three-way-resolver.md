- `scripts/dev/resolve-state-md-conflict.py` now resolves a conflicted
  `docs/state.md` by a three-way merge of the merge base and both sides, keyed
  by bug id, instead of letting master's side win. A branch that closes, edits
  or deletes a row keeps that change, and a later commit that rewrites a row an
  earlier commit added keeps the rewrite. Disposition rows merge their id lists
  as sets, and repeated rows with one label are folded into one. When both sides
  changed the same row differently the tool writes nothing and names it; rerun
  with `--take NAME=ours|theirs`. It runs `scripts/ci/check-state-md-rows.sh` on
  its result and always writes LF line endings. Its test suite now runs real
  `git rebase` conflicts in CI (ADR-1383).
