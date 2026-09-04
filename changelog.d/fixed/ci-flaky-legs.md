- Fixed flaky CI leg failure in `test_mcp_smoke` (job "MCP Smoke (Embedded C + Python Server)")
  which timed out after 60s on master run 33916590280. On Linux, closing a listening AF_UNIX
  socket without `shutdown()` does not unblock `accept(2)` in the background worker thread,
  causing `pthread_join()` in `stop_uds()` to hang waiting for accept to return. `stop_uds()`
  now issues `shutdown(listen_fd, SHUT_RDWR)` before `close()`, mirroring the SSE listener
  shutdown pattern. Measured passing wall-clock distribution is ~0.03s.
- Hardened macOS Homebrew package installation across CI workflows (`build.yml` and
  `libvmaf-build-matrix.yml`) against intermittent download failures (such as `llvm` bottle
  fetches) by wrapping `brew install` in a 3-attempt retry loop with exponential backoff and
  `brew fetch --retry`, while disabling unnecessary auto-updates and cleanup via
  `HOMEBREW_NO_AUTO_UPDATE=1` and `HOMEBREW_NO_INSTALL_CLEANUP=1`.
