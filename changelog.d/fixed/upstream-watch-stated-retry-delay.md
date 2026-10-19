- CI: the upstream watchers wait out the delay a GitHub search rate limit states. In run 39123672700
  the FFmpeg HIP hwdec watcher failed on a `search/code` 429 asking to "try again in 84 s" after two
  backoff steps of 15 s and 30 s. `scripts/ci/upstream_watch_count.sh` now sleeps the stated delay
  (rounded up, plus 1 s) when it is longer than the backoff step, makes three attempts, refuses a
  stated delay above `UPSTREAM_WATCH_MAX_WAIT_S` (default 120 s) or a total wait above
  `UPSTREAM_WATCH_TOTAL_WAIT_S` (default 300 s), and still fails closed when the attempts run out
  (`T-UPSTREAM-WATCH-RETRY-DELAY-2026-10-18`).
