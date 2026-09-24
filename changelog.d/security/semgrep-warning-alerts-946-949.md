- Resolve Semgrep Python alerts 946–949 at source: memoization decorators now use
  pure SHA-256 keys with cold invalidation, cross-process locking, merge-on-write,
  and unique `mkstemp` atomic replacements. The reviewed base wrote JSON directly
  to its destination; it never used PID-suffixed temporary files.
- Make the unauthenticated sidecar Unix socket owner-only (`0o600`) for the
  shipped same-UID contract, and protect its pathname with a lifetime claim,
  no-follow/type checks, bounded non-blocking active/stale probing,
  device/inode validation, and identity-checked cleanup (ADR-1309). A live
  listener with a full accept queue now fails closed instead of blocking startup.
- Exercise all 26 decorator regressions through Nox and the hosted Linux, macOS,
  and Windows build matrix (including the real `msvcrt` backend), with the hosted
  pytest dependency exact-pinned to 9.1.1; extend the
  POSIX socket suite to 20 adversarial lifecycle and permission cases. Local
  Semgrep text and SARIF scans report zero findings for all four alerts; hosted
  closure remains pending the post-merge Code Scanning run.
- Correct the sidecar operator guide to the executable state: there is no current
  chart wiring, automatic feedback producer, node checkpoint consumer, CUDA
  setting, HTTP trainer status service, or Prometheus counter registration;
  require standalone quick-start examples to configure a writable checkpoint
  directory alongside the socket.
- Make the newly required sidecar suite warning-clean on PyTorch 2.14: preserve
  batch-size-one training without MSE broadcasting, reject prediction/target
  count mismatches, serialize each reserve-to-commit training lifecycle, consume
  only the pending samples used by a replay-mixed batch, sample replay without
  replacement when history suffices and with replacement only when it does not,
  and count only successfully trained new rows toward checkpoint eligibility.
  Retry the oldest failed window without discarding concurrent samples and bound
  pending work with explicit backpressure. Admission-aware ACKs prevent a
  retained failed-step sample from being duplicated by caller retry; the Go
  client requeues an explicitly retryable capacity rejection without counting a
  delivery. Export a genuinely dynamic ONNX batch axis and fail socket lifecycle
  tests on server-thread exceptions.
