- CI: upstream watcher workflows fail closed on GitHub search API rate limits. In run 38142503890,
  `gh api search/code` rate-limit errors were swallowed by `|| echo 0`, printing JSON that broke
  comparison logic and nearly created a false upstream-merged tracking issue. All count queries now
  call `scripts/ci/upstream_watch_count.sh` with exponential backoff (15s, 30s, 60s) and fail closed
  on non-integer output, and workflows ensure the `upstream-merged` label exists before issue operations
  (`T-UPSTREAM-WATCHERS-RATE-LIMIT-2026-10-15`).
