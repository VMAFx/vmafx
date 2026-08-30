- `cmd/vmafx-controller/scheduler/` + `cmd/vmafx-node/` bug audit
  round 1 (2026-05-31): four reachable defects found and fixed
  across the Phase 4b distributed scheduling core. (1)
  `FeedbackClient.NewFeedbackClient` documented a `Close()` method
  that never existed — the only way to stop the background drainer
  was to cancel the constructor context. Added an idempotent
  `Close()` that cancels an internal sub-context and synchronously
  waits for the goroutine to exit via a `done` channel; multi-Close
  and post-ctx-cancel Close are both safe. (2) `FeedbackClient`
  accepted a nil logger but the drainer's `log.Warn`/`log.Debug`
  calls dereferenced it on the first dial-failure or queue-drop
  path. Constructor now substitutes `slog.Default()` when log is
  nil. (3) `Executor.NewExecutor` accepted a nil logger and the
  scoring path's `e.log.InfoContext(...)` would panic on the first
  job — masked by the existing `TestExecutor_ScoringJobFailsWithBadBinary`
  skipping when `false` is unreachable via `libvmaf.New`'s PATH
  lookup. Constructor now substitutes `slog.Default()`. (4)
  `classifyJob`'s "AI heuristic" comment said *"no model is set"*
  but the code required the opposite (model set, distorted empty)
  — corrected the comment to describe the actual behaviour, which
  the existing `TestExecutor_AIJobUnsupportedStage1` already
  pinned. Test-only: `TestAssignJobBackToQueueOnNodeDisconnect`
  leaked two registry reaper goroutines per run by skipping
  `r1.Close()` / `r2.Close()`; added explicit `defer Close()` calls
  matching the `newFixture` precedent. Four new regression tests
  in `cmd/vmafx-node/online_feedback_test.go` cover the Close()
  contract and nil-logger guard; one new test in
  `executor_test.go` locks in the nil-logger guard for `Executor`.
