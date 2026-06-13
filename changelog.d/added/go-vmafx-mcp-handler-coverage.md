## cmd/vmafx-mcp: expand handler test coverage from 35 % to 53 %

Added `impl_handlers_test.go` with table-driven tests covering the error and
no-binary-required paths of all 15 tool handlers. New tests cover:
`handleListModels` (real model-dir walk), `handleListBackends` (binary-absent
fallback), `handleVmafVersion` (binary-absent early return), `handleRunBenchmark`
(binary-absent exit-code path), `handleEvalModelOnSplit` / `handleCompareModels` /
`handleDescribeWorstFrames` (ValidatePath rejection), `handleRunCompare` /
`handleRunLadder` / `handleRunTunePerShot` (vmaf-tune binary absent),
`findVmafTune` (env override + fallback), `parseArgs` (nil / valid / invalid
JSON), `probeBackends` (cache hit + help-output parsing), `describeModel`
(ambiguous stem + not-found), `describeModelFile` (ONNX and JSON with
model_dict), `handleListExtractors` (real feature-dir walk), and the
`addRawTool` closure error-to-IsError conversion. Coverage: 35.3 % → 53.2 %.
