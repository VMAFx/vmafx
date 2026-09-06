# AGENTS.md — cmd/vmafx-ort-runner

The ONNX Runtime subprocess behind `pkg/ai.Registry.Infer`
([ADR-0713](../../docs/adr/0713-vmafx-node-impl.md) Stage 1,
[ADR-1134](../../docs/adr/1134-vmafx-ort-runner-in-tree.md)): a cgo shim over
`pkg/libvmaf.DNNSession` that binds one JSON array to a graph's single input
as a `[1, N]` float32 row and prints the flattened output tensor as one JSON
array line. User docs: [docs/usage/vmafx-ort-runner.md](../../docs/usage/vmafx-ort-runner.md).

## Rebase-sensitive invariants

1. **The wire format is `pkg/ai`'s, byte for byte** (`main.go::run`,
   `pkg/ai/infer.go::Registry.Infer`): argv is
   `--model <path> --inputs '<JSON array>'`, stdout is one JSON array of
   numbers plus a newline, nothing else ever goes to stdout.
   `main_test.go::TestRun_ProtocolRoundTrip` pins this side and
   `pkg/ai/infer_runner_test.go::TestInfer_FakeRunnerProtocol` pins the
   other; change them together or `--model` silently falls back to the
   analytical curve again.

2. **Exit codes are a contract**: 0 success, 1 open/run failure, 2 usage or
   protocol error, 3 libvmaf built without ONNX Runtime. `pkg/ai` quotes the
   status plus stderr in its error, `pkg/ai/infer_runner_test.go` looks for
   `exit status 3`, and operators are told in the usage page that 3 means
   "rebuild with `-Denable_dnn=enabled`". Do not fold 3 into 1.

3. **Positional binding by default** (`--input-name` empty →
   `DNNSession.Predict(ctx, "", …)` → NULL `VmafDnnInput.name`). Every shipped
   `model/predictor_*.onnx` names its input `input`, but the runner must not
   know that: `pkg/ai` never sends a name. Keep the `pkg/libvmaf` empty-name
   rule (its AGENTS.md invariant 11) intact.

4. **Real inference is asserted three times**, and all three must survive a
   rebase: `main_test.go::TestRun_PredictorModel` (skips without an ORT
   libvmaf), the `go-ci.yml` smoke step (which installs ORT so the test does
   not skip), and the `dev/Containerfile` `dev-mcp` stage `RUN` after
   `COPY --from=go-build`. The reference value `66.13961791992188` is
   onnxruntime 1.29.0's CPU-EP answer for the deliberately unrealistic row
   `[51,1,1,1,1,0,0,0,0,0,1,1,16,16]`; realistic rows saturate at `100.0` and
   cannot distinguish a correct pass from a mis-bound tensor.

5. **No framework, and no OpenTelemetry init.** The runner is stdlib `flag`
   only — no cobra, no golusoris/fx (ADR-1119 §5, ADR-1134). It is spawned
   once per inference and has no config, logger or lifecycle to inject;
   wrapping it would add startup cost to every predictor call for nothing.
   That exemption extends to OTel (ADR-0782 rollout, `cmd/AGENTS.md` #5): an
   exporter here would add a config load and an export flush to every
   predictor call, and argv carries no trace context to parent the span
   anyway. The `vmafx.onnx.inference` span is emitted by the caller,
   `pkg/ai.Registry.Infer` (`pkg/ai/infer_otel_test.go`); do not add an
   `otel.New` / `InitOTel` call to `main.go`.

6. **Session per call, closed on every path** (`main.go::infer`): `defer
   sess.Close()` runs on the error path too; `TestRun_InferenceFailureIsExit1`
   checks the fake session was closed.
