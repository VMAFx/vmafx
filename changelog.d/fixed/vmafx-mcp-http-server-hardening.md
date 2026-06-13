- `vmafx-mcp --transport=http` no longer exposes a Slowloris
  (CWE-400) vector. The `&http.Server{...}` literal was missing
  `ReadHeaderTimeout` / `ReadTimeout` / `WriteTimeout` /
  `IdleTimeout`, so a malicious or stuck client could hold a
  TCP connection open indefinitely while dripping header bytes
  and exhaust the server's goroutine / file-descriptor budget.
  Added the four timeouts mirroring the hardened pattern in
  `cmd/vmafx-server/http_server.go` (10s read-header, 30s read,
  120s write, 60s idle). Graceful shutdown was also unbounded
  (`srv.Shutdown(context.Background())`) and could hang the
  process forever if an in-flight request stalled; it now uses
  `observability.GracefulShutdownTimeout` (30s) as an upper bound.
