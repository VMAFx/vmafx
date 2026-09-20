- Bound repository automation subprocesses to explicit executables, arguments,
  output memory, deadlines, and process-group cleanup on timeout, overflow, or
  caller cancellation instead of relying on static-analysis waivers at each
  call site; canonicalized the helper's Python package identity so direct
  scripts, fail-soft consumers, and type checks exercise the same API; isolated
  the blocking mypy delta check from environment-dependent third-party stubs.
