## Daily Netflix/vmaf unported commit watcher (2026-10-19)

- `.github/workflows/upstream-watcher.yml`: added `netflix-unported` job running
  daily at 08:00 UTC. It polls upstream Netflix/vmaf `master` against the
  recorded parity head in `docs/development/known-upstream-bugs.md`, filtering
  out commits recorded in `scripts/upstream-watcher/netflix-dispositions.tsv`.
- `scripts/upstream-watcher/check_netflix_unported.py`: CLI driver that resolves
  the parity pin via `scripts/ci/upstream_parity_pin.py`, lists unported
  non-merge commits in chronological order, strips trailing PR suffixes, and
  returns JSON or plain text.
- `scripts/upstream-watcher/netflix-dispositions.tsv`: tracked ledger recording
  the disposition (`ported`, `in-fork`, or `not-applicable`) and reference
  for upstream commits.
