- `semgrep-local` pre-commit hook no longer fails with opaque
  `exit code 2` on large staged-file sets. pre-commit's default
  parallel partitioning spawns `cpu_count()` `semgrep` processes,
  each of which initialises its own `io_uring` instance via
  `semgrep-core`; on workers with `>=16` cores the per-user
  `RLIMIT_MEMLOCK` budget (default 8 MB) is exhausted and several
  workers die with `Unix_error: Cannot allocate memory
  io_uring_queue_init` — invisible because the hook runs with
  `--quiet`. Surfaced in PR #331 (VMAFx rebrand, 744 changed files),
  where the agent had to commit with `SKIP=semgrep-local`. Fixed
  by setting `require_serial: true` on the hook so pre-commit
  invokes `semgrep` exactly once with the full file list; semgrep
  is already internally multi-threaded via `--jobs auto`, so the
  full-tree wall-time only rises from ~3.3 s to ~4.3 s. See
  ADR-0867 for the io_uring trace and the alternatives considered.
