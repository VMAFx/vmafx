<!-- markdownlint-disable MD013 -->
# Research-2480: Stale-lock recovery for the ADR number allocator

- **Status**: Active
- **Workstream**: issue #2784 (no ADR: bug fix in fork-added tooling)
- **Last updated**: 2026-10-15

## Question

`scripts/adr/next-free.sh --claim` serialises claims with a lock directory under `/tmp`. When a
claim dies while it holds the lock, every later claim waits for the lock and then fails. Which lock
protocol recovers from a dead holder without ever giving two live claims the lock at the same time?

## Sources

- Issue #2784: the `Tooling Tests` failure in `scripts/adr/tests/test-next-free-concurrent.sh`.
  Round 1 gave 12 distinct numbers. In round 2 all 12 claims failed about 11 s after the start,
  which matches the allocator's fixed 10 s wait.
- `set -o noclobber` in the Bash manual: a `>` redirection to an existing file fails. Bash then
  opens the file with `O_CREAT | O_EXCL`, so only one writer can create it.
- `scripts/adr/tests/test-next-free-concurrent.sh 12 200` was run as a soak on two pinned CPUs,
  with six busy loops competing for them.

## Findings

- The old allocator removed its lock only from an exit trap, and it set that trap after `mkdir`.
  So a claim killed between `mkdir` and the trap, or killed with `SIGKILL`, left the lock behind for
  good. The CI log shows no stderr from the failed claims, so we cannot tell which of these two
  happened in CI.
- The owner file is the lock. A claim creates it under `noclobber` inside the lock directory.
  Creating the file is the atomic step, so a lock directory without an owner file holds no lock.
- A takeover removes the owner file of a process that has exited. Takeovers run under a second lock,
  `<lock>.takeover`, and that lock is itself removed after one minute. Before it deletes anything,
  the takeover runs `ps -p` again and reads the owner line again. If the line changed, a live claim
  took the lock in the meantime, and the takeover leaves the file alone.
- The final design passed the soak in 200 of 200 rounds. `test-next-free-stale-lock.sh` has three
  cases (dead holder, lock directory without an owner file, live holder): all three pass on the
  fix, and the two takeover cases fail on master's allocator.
- A planted negative, a liveness check that always says the holder is dead, makes the live-holder
  case fail.

## Alternatives explored

| Design | Soak result | Why rejected |
| :--- | :--- | :--- |
| `mkdir` lock, takeover of an ownerless directory after a 5 s grace | failed (soak 2) | A claim that had just created the directory, and had not yet written its owner file, lost the lock to a takeover. Two claims then held the lock at once. |
| Takeover that moves the lock aside, checks it, and restores it if the holder is alive | failed (soak 3) | Between the move and the restore, a live holder's owner file could be lost. |
| Owner file created with `O_EXCL`, takeovers serialised under a mutex and re-checked | passed (soak 4, 200/200) | Chosen. |

## Open questions

- We do not know which holder left the lock in the CI run of #2784. With this change, the
  concurrent test prints the stderr of every claim that fails, so the next failure will show it.

## Related

- Issues: #2784
- Files: `scripts/adr/next-free.sh`, `scripts/adr/tests/test-next-free-stale-lock.sh`,
  `scripts/adr/tests/test-next-free-concurrent.sh`
