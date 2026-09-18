# AGENTS.md — cmd/vmafx-ort-runner

ONNX Runtime subprocess behind `pkg/ai.Registry.Infer`
([ADR-0713](../../docs/adr/0713-vmafx-node-impl.md) Stage 1,
[ADR-1134](../../docs/adr/1134-vmafx-ort-runner-in-tree.md)): cgo shim over
`pkg/libvmaf.DNNSession`. Binds 1 JSON array to graph single input as `[1, N]`
float32 row; prints flattened output tensor as 1 JSON array line.
User docs:
[docs/usage/vmafx-ort-runner.md](../../docs/usage/vmafx-ort-runner.md).

## Rebase-sensitive invariants

1. **Wire format `pkg/ai` byte for byte** (`main.go::run`,
   `pkg/ai/infer.go::Registry.Infer`): argv =
   `--model <path> --inputs '<JSON array>'`. Stdout = 1 JSON array of numbers
   plus newline; nothing else ever to stdout.
   `main_test.go::TestRun_ProtocolRoundTrip` pins this side;
   `pkg/ai/infer_runner_test.go::TestInfer_FakeRunnerProtocol` pins other.
   Change together or `--model` silently falls back to analytical curve again.

2. **Exit codes = contract**: 0 success, 1 open/run failure, 2 usage or
   protocol error, 3 libvmaf built without ONNX Runtime. `pkg/ai` quotes
   status plus stderr in error; `pkg/ai/infer_runner_test.go` looks for
   `exit status 3`. Usage page tells operators 3 =
   "rebuild with `-Denable_dnn=enabled`". Do not fold 3 into 1.

3. **Positional binding by default** (`--input-name` empty ->
   `DNNSession.Predict(ctx, "", …)` -> NULL `VmafDnnInput.name`). Every shipped
   `model/predictor_*.onnx` names input `input`, but runner must not know that:
   `pkg/ai` never sends name. Keep `pkg/libvmaf` empty-name rule
   (its AGENTS.md invariant 11) intact.

4. **Real inference asserted x3**; all 3 must survive rebase:
   `main_test.go::TestRun_PredictorModel` (skips without ORT libvmaf),
   `go-ci.yml` smoke step (installs ORT so test does not skip),
   and `dev/Containerfile` `dev-mcp` stage `RUN` after `COPY --from=go-build`.
   Reference value `66.13961791992188` = onnxruntime 1.29.0 CPU-EP answer for
   deliberately unrealistic row `[51,1,1,1,1,0,0,0,0,0,1,1,16,16]`; realistic
   rows saturate at `100.0` and cannot distinguish correct pass from mis-bound
   tensor.

5. **No framework, no OpenTelemetry init.** Runner is stdlib `flag` only — no
   cobra, no golusoris/fx (ADR-1119 §5, ADR-1134). Spawned once per inference;
   no config, logger, lifecycle to inject; wrapping adds startup cost to every
   predictor call for nothing. Exemption extends to OTel (ADR-0782 rollout,
   `cmd/AGENTS.md` #5): exporter adds config load + export flush to every
   predictor call; argv carries no trace context to parent span anyway.
   `vmafx.onnx.inference` span emitted by caller `pkg/ai.Registry.Infer`
   (`pkg/ai/infer_otel_test.go`); do not add `otel.New` / `InitOTel` call to
   `main.go`.

6. **Session per call, closed on every path** (`main.go::infer`):
   `defer sess.Close()`; ` runs on the error path too; `
   `TestRun_InferenceFailureIsExit1` checks fake session closed.
