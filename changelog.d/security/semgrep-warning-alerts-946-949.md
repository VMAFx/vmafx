- Resolve Semgrep Python alerts 946–949 at source: memoization decorators now use
  pure SHA-256 keys with cold invalidation, cross-process locking, merge-on-write,
  and unique `mkstemp` atomic replacements. The reviewed base wrote JSON directly
  to its destination; it never used PID-suffixed temporary files.
- Make the unauthenticated sidecar Unix socket owner-only (`0o600`) for the
  shipped same-UID contract, and protect its pathname with a lifetime claim,
  no-follow/type checks, active/stale probing, device/inode validation, and
  identity-checked cleanup (ADR-1309).
- Exercise all 26 decorator regressions through Nox and the hosted Linux, macOS,
  and Windows build matrix (including the real `msvcrt` backend), with the hosted
  pytest dependency exact-pinned to 9.1.1; extend the
  POSIX socket suite to 19 adversarial lifecycle and permission cases. Local
  Semgrep text and SARIF scans report zero findings for all four alerts; hosted
  closure remains pending the post-merge Code Scanning run.
- Correct the sidecar operator guide to the executable state: there is no current
  chart wiring, automatic feedback producer, node checkpoint consumer, CUDA
  setting, HTTP trainer status service, or Prometheus counter registration.
