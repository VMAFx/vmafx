## Node container decode (2026-10-24)

`cmd/vmafx-node/executor_inputs.go::openScored` decodes non-Y4M inputs and windowed inputs through `pkg/y4mdecode` after the scoring-root check on the source path. A rebase keeps the check before the decode and the pipe (no temp file). `ScoringParams` fields 5 and 6 (`window_start`, `window_duration`) are taken; regenerate `gen/go/controller` from the proto, never hand-merge it. No upstream Netflix file is touched.
