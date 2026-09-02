- **Test coverage**: extended Go unit tests for `cmd/vmafx-controller`,
  `cmd/vmafx-controller/nodes`, `cmd/vmafx-server`, and `cmd/vmafx-mcp`.
  No behavior change; closes Section F coverage gaps from the master-tip
  workflow audit.
  - `cmd/vmafx-controller`: 18.6 % → 32.4 % (+13.8 pp). `handleHealthz`,
    `handleReadyz`, `handleScore`, `writeJSON`, `envOr`, and `version` are
    now 100 %; `runHTTP` graceful-shutdown path covered.
  - `cmd/vmafx-controller/nodes`: 80.7 % → 82.5 % (+1.8 pp). Adds
    concurrent Register/Heartbeat/Get under `-race`, defensive-copy
    assertion on `All()`, distinct-IDs-for-same-name contract pin, and
    `LastHeartbeat` advancement check (reaper eviction predicate).
  - `cmd/vmafx-server`: 27.5 % → 47.9 % (+20.4 pp). Same shape as the
    controller HTTP server (405 / 400 / 500 paths + bounded-timeout
    shutdown, mirroring the PR #300 invariant).
  - **`.gitignore` fix (drive-by)**: anchored the Go binary-ignore rules
    (`vmafx-server`, `vmafx-mcp`, `vmafx-tune`) with a leading slash so
    they only match the compiled binaries at the repo root, not any path
    component sharing the name. Without this, new untracked files like
    `cmd/vmafx-server/main_extra_test.go` were silently ignored by `git
    add`. Also added entries for `vmafx-controller`, `vmafx-node`, and
    `vmafx-operator`.
  - `cmd/vmafx-mcp`: 3.5 % → 24.6 % (+21.1 pp). Covers all arg helpers,
    every pure helper (`classifySourceResolution`,
    `resolutionMismatchWarning`, `inferBackendFromPayload`,
    `inferBackendFromSym`, `stripModelExt`, `toFFmpegPixfmt`,
    `pickWorstFrames`, `floatFromAny`, `roundF`, `truncate`), and
    representative handler error paths (`handleProbeBackend` missing
    backend, `handleDescribeModel` missing name, `handleVmafScore`
    invalid path / zero dimensions, `handleCompareModels` empty list).
    Pins the `errorResult().IsError == true` invariant.
