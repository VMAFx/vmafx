- **fix(server,controller)**: `WaitForShutdown` no longer blocks the full
  `GracefulShutdownTimeout` on clean shutdown — drain window now exits early
  when `ctx.Done()` fires (ADR-1009).
- **fix(server,controller)**: `GracefulStop()` on both vmafx-server and
  vmafx-controller gRPC listeners now has a `GracefulShutdownTimeout` hard-stop
  fallback; a stuck streaming RPC can no longer prevent process exit after
  SIGTERM (ADR-1009).
