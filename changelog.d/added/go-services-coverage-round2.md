## Added

- **Go test coverage round 2 for Phase 4b services**: focused unit tests
  for the four lowest-coverage Go packages that previous coverage
  pushes (PR #330 cmd/, PR #347 pkg/, PR #362 nilness audit) did not
  reach. Per-package coverage delta (race + cover, vs. master baseline):

  - `cmd/vmafx-tune/cmd`            0.0%  → 70.5% (+70.5 pp)
  - `cmd/vmafx-controller` (gRPC)  18.6% → 49.0% (+30.4 pp)
  - `cmd/vmafx-controller/queue`   77.2% → 82.4% (+5.2 pp)
  - `pkg/encoder`                  31.2% → 35.5% (+4.3 pp)

  New surfaces covered: in-process unit tests for `parseResolution`,
  `failRow`, `rowFromBisect`, `sortRows`, `emitSweepJSON`,
  `emitSweepMarkdown`, `emitLadderJSON`, `emitLadderMarkdown`,
  `writeOutput`, `stubSubcommand`, `newCompareCmd`, `newLadderCmd`,
  `runCompare` / `runLadder` validation paths in vmafx-tune-go; full
  gRPC handler tests (`SubmitJob`, `GetJob`, `CancelJob`, `StreamJobs`,
  `RegisterNode`, `Heartbeat`, `PullWork`, `ReportResult`,
  `queueJobToProto`, `queueStatusToProto`, `protoCapToNodes`, scoring
  `Health`) for vmafx-controller; `RunningCount`, `Close` idempotency,
  cancel-of-running, FIFO ordering, and cancelled-FIFO-entry skip for
  the SQLite queue; `extractEncoderVersion`, `ffmpegBin`, `outputDir`
  defaulting for the encoder package. No production code touched.
