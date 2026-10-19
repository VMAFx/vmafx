- **A stale private-state lock no longer stops every commit hook.** `scripts/githooks/state-sync.sh`
  records its owner (process id and boot id) in its lock. A lock whose owner ran in an earlier boot or
  no longer runs, or an ownerless lock older than two minutes, is taken over with a log line; before,
  a lock left by an interrupted sync failed every post-commit state sync until it was removed by hand.
