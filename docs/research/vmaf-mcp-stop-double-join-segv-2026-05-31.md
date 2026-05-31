# Research: `vmaf_mcp_stop()` double-join SIGSEGV

- **Status**: Active
- **Workstream**: ADR-0209 (embedded MCP scaffold lifecycle)
- **Last updated**: 2026-05-31

## Question

`vmaf_mcp_stop()` was reported (PR #460 audit follow-up #5) to
SIGSEGV on its third invocation against the same server handle.
What is the root cause, and what is the minimum-blast-radius fix
that preserves the existing 3-state `*_running` semantics
(0 = never-started, 1 = started, 2 = stopped)?

## Sources

- `core/src/mcp/mcp.c` — `vmaf_mcp_stop()` body (pre-fix at
  `2581c7ba0`).
- PR #460 description, `Known follow-ups` section item 5
  (`gh pr view 460`).
- ADR-0209 (embedded MCP scaffold) — public lifecycle contract.
- glibc 2.40 `pthread_join(3)` man page: "Joining with a thread
  that has previously been joined results in undefined behaviour."

## Findings

The defect is a state-machine confusion between the *transition
trigger* and the *guard predicate*.

Pre-fix control flow on each transport atomic:

```text
int prev = atomic_exchange(&server->stdio_running, 2);
assert(prev == 0 || prev == 1 || prev == 2);
if (prev == 1 || prev == 2) {
    (void)pthread_join(server->stdio_thread, NULL);
    atomic_store(&server->stdio_running, 0);
}
```

The `atomic_exchange` unconditionally writes `2` regardless of
the previous value, and the guard fires for both `1` and `2`.
The store-back to `0` at the end of the join branch only happens
when the join branch is taken.

This produces three distinct failure modes:

1. **Never-started transport, two stop() calls.** First call:
   prev=0, skip join, but state is now `2`. Second call: prev=2,
   enter join branch, `pthread_join()` on a default-initialised
   `pthread_t` (UB, EFAULT or SIGSEGV depending on libc).
2. **Started transport, three stop() calls.** First call: prev=1,
   join, store 0. Second call: prev=0, skip join, state now `2`.
   Third call: prev=2, enter join branch, `pthread_join()` on the
   already-joined thread handle (UB; SIGSEGV on glibc 2.40).
3. **Explicit stop() then close().** `vmaf_mcp_close()` calls
   `vmaf_mcp_stop()` internally. If the user calls stop() once
   explicitly and then close() — a perfectly reasonable pattern —
   the second internal stop() trips failure mode 1 above.

The minimum-blast-radius fix replaces each
`atomic_exchange(running, 2)` + dual-value guard with a single
`atomic_compare_exchange_strong(expected=1, desired=2)`. The CAS
only succeeds on the 1->2 transition; the 0 and 2 cases are
silent no-ops. The trailing `atomic_store(running, 0)` is
removed because the new terminal state is `2` (consistent with
the existing `start_*` paths, which use
`compare_exchange_strong(expected=0, desired=1)` and therefore
already reject restart attempts on a `2`-state transport with
`-EBUSY`).

Touched paths: `core/src/mcp/mcp.c::vmaf_mcp_stop` (three
analogous patches, one per transport). Regression test:
`core/test/test_mcp_stop_idempotent.c`, with two sub-tests —
`stop_thrice_without_start` (failure mode 1) and
`stop_thrice_with_stdio` (failure mode 2). Both pass with the
fix; both deterministically crash without it.

## Alternatives explored

- **Guard on `prev == 1` only, keep the unconditional exchange.**
  Cheaper diff (one-character change per transport), but leaves
  `running` in state `2` after a no-op stop on a never-started
  transport — which would still be wrong if any other code path
  ever reads `*_running` to decide between "never-started" and
  "stopped". The CAS variant keeps the state authoritative
  (0 stays 0; 1 transitions to 2 exactly once) and matches the
  start-path's CAS pattern symmetrically.
- **Add a `stopped` boolean to the server struct and short-circuit
  the whole function on second entry.** Solves the SIGSEGV but
  silently changes the existing 3-state contract by adding a
  fourth flag with no documentation. Loses the per-transport
  granularity (stop() currently operates on each transport
  independently — a SSE-only server's stop() must not touch the
  uninitialised stdio thread handle, which is exactly what the
  per-transport CAS preserves). Rejected.
- **Make `pthread_join()` second-call-safe by zeroing the
  `pthread_t` after the first join.** Doesn't work — `pthread_t`
  is an opaque type per POSIX; portably zero-comparing it isn't
  defined, and glibc-specific tricks (`memset` to 0 then compare)
  would still UB on the second join. Rejected.

## Open questions

- Should `vmaf_mcp_stop()` after a successful `vmaf_mcp_close()`
  be safe? Today the function dereferences `server` immediately
  so a use-after-free would SIGSEGV; the public contract says
  the caller must not use the handle after close. The fix does
  not change that contract. If a future audit wants `stop()`
  to tolerate close-after-stop ordering, that needs an additional
  per-handle epoch counter — out of scope for this PR.

## Related

- ADRs: [ADR-0209](../adr/0209-embedded-mcp-scaffold.md)
- PRs: #460 (flagged the defect in the audit follow-ups section)
- Issues: none open
