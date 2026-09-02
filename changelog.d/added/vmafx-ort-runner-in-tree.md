- **feat(go):** `cmd/vmafx-ort-runner` — the ONNX Runtime subprocess that
  `pkg/ai.Registry.Infer` (and therefore `vmafx-tune predict / sidecar / auto
  --model`) execs is now built from this repository as a cgo shim over
  libvmaf's DNN session API. It is produced by `go build ./cmd/...`,
  `make go-ort-runner`, the dev container (`/usr/local/bin/vmafx-ort-runner`,
  with an image-build smoke against `model/predictor_libx264.onnx`) and the
  Go CI job, which now builds libvmaf with ONNX Runtime and exercises the real
  subprocess path. The argv/stdout JSON protocol and exit codes 0/1/2/3 are
  documented in `docs/usage/vmafx-ort-runner.md`.
  `pkg/libvmaf.DNNSession.Predict` accepts an empty input name for positional
  binding, and `pkg/ai` errors now carry the runner's stderr, so a fallback to
  the analytical curve says whether the runner is missing from `PATH` or its
  libvmaf lacks ONNX Runtime. Previously the binary existed only in comments,
  docs and 22 references, and every `--model` invocation silently fell back.
  (ADR-1134)
