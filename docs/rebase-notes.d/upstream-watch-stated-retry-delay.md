## Upstream watchers honour a stated retry delay (2026-10-18)

- `scripts/ci/upstream_watch_count.sh` parses "try again in N s" from a failed `gh api` reply and
  sleeps that delay, bounded per retry (`UPSTREAM_WATCH_MAX_WAIT_S`) and in total
  (`UPSTREAM_WATCH_TOTAL_WAIT_S`); three attempts, then it fails closed.
- `scripts/ci/tests/test_upstream_watch_count.py` covers the stated delay, three 429s, and both bounds.
- No rebase impact beyond these two `scripts/ci/` files.
