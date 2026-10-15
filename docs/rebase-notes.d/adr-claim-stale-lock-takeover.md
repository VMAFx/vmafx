## ADR claim lock: stale-lock takeover and longer wait (2026-10-15)

In `scripts/adr/next-free.sh` the lock is the owner file, created with
`O_EXCL` (`noclobber`) inside the lock directory; the directory alone holds
nothing, so a claim killed between `mkdir` and the owner write leaves a free
lock. An owner file whose process is gone is removed with a warning, under a
second directory (`<lock>.takeover`) that serialises takeovers, after the
process and the owner line are checked again; a running holder's file is never
removed. The exit trap is set before the lock is taken, and the wait bound is
`VMAF_ADR_LOCK_WAIT_S` (default 60). A sync or cleanup must not bring back
`mkdir`-only ownership, a grace-period takeover of a directory without an owner
file (it raced a claim that had just created the directory), the fixed 10 s
wait, a takeover outside the takeover lock or without the re-checks, or a
takeover of a lock whose holder still runs.
`scripts/adr/tests/test-next-free-stale-lock.sh` and
`scripts/adr/tests/test-next-free-concurrent.sh` guard it.
