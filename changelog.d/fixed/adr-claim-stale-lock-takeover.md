- ADR tooling: `scripts/adr/next-free.sh --claim` no longer fails every later
  claim for good when a claim is killed while it holds the allocator lock. A
  lock whose holder process is gone is taken over with a warning, and a lock
  directory without an owner file counts as free; a running holder's lock is
  still respected. The
  wait for the lock is 60 s instead of 10 s (`VMAF_ADR_LOCK_WAIT_S`), and the
  timeout message names the holder
  ([adr-workflow](docs/development/adr-workflow.md#why-use-claim)).
