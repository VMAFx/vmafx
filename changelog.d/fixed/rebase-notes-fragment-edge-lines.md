- Docs tooling: `scripts/docs/concat-rebase-notes.sh --lint` now requires a
  rebase-notes fragment to start with its `## <title> (<date>)` heading on
  line 1 and to end without a blank line. A fragment with a blank first line
  passed the old check, rendered a double blank line into
  `docs/rebase-notes.md` and made the hooks refuse every render commit, so
  master owed its render past the 24-hour bound.
