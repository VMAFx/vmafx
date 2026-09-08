- **`scripts/dev/resolve-state-md-conflict.py`** — resolves a `docs/state.md`
  rebase conflict the way ADR-0165 requires, instead of by hand. `state.md` is
  deliberately excluded from the `merge=union` list in `.gitattributes` because
  its rows move between the "Open bugs" and "Recently closed" sections, so a
  keep-both resolution duplicates the row and leaves a closed bug reading as
  open. The script takes master's side whole and appends only the rows master
  does not have, deduplicating by bug id so a row master reworded is not
  re-added in its stale form. `scripts/ci/check-state-md-rows.sh` now names it
  in the failure message, and `docs/development/ci.md` documents the rule it
  encodes and the case it cannot decide for you.
