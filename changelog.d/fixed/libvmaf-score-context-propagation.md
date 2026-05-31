- `libvmaf.Scorer.Score` and `libvmaf.ScoreDirect` now accept a
  `context.Context` as their first parameter, plumbed through every
  caller in `vmafx-server`, `vmafx-controller`, and `vmafx-node`. The
  subprocess path uses `exec.CommandContext` + `WaitDelay` so a client
  disconnect propagates SIGKILL to the `vmaf` binary within ~2 s; the
  cgo direct path checks `ctx.Done()` at frame boundaries and lets the
  deferred `vmaf_close` clean up. Fixes
  T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31: previously a dropped HTTP /
  gRPC request left the subprocess running to completion.
