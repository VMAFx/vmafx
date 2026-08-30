- `vmaf_mcp_stop()` no longer SIGSEGVs on its third (or later)
  invocation. The prior implementation called
  `atomic_exchange(running, 2)` unconditionally on each of the
  three transport state atomics; on a server that had never
  started a given transport the first call mutated `0 -> 2`
  silently, and the second call then re-entered the join branch
  with `prev == 2` and invoked `pthread_join()` on a
  default-initialised `pthread_t` (UB; observed as SIGSEGV on
  glibc 2.40). The same defect tripped any caller that explicitly
  invoked `vmaf_mcp_stop()` once and then let `vmaf_mcp_close()`
  invoke it a second time — by the third entry, the join branch
  re-ran on an already-joined thread handle. Replaced each
  exchange with `atomic_compare_exchange_strong(expected=1,
  desired=2)` so the join branch fires exactly once per started
  transport, regardless of how many times `vmaf_mcp_stop()` /
  `vmaf_mcp_close()` is invoked. Regression test:
  `core/test/test_mcp_stop_idempotent.c` (two sub-tests: triple
  `stop()` with and without an active stdio transport). Flagged
  by PR #460 (`test(core-mcp): coverage push for transport +
  dispatcher error paths`) audit, follow-up item #5.
