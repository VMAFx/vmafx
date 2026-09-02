<!-- markdownlint-disable MD060 -->
# `vmafx-ort-runner` — ONNX Runtime subprocess for the Go tools

`vmafx-ort-runner` runs one ONNX Runtime forward pass through libvmaf's
standalone DNN session API and prints the output tensor as JSON. It is the
subprocess half of the Go ONNX seam: `vmafx-tune`'s `predict`, `sidecar` and
`auto` subcommands stay pure Go and exec this runner for their
`--model predictor_<codec>.onnx` inference instead of linking ONNX Runtime
themselves ([ADR-0713](../adr/0713-vmafx-node-impl.md) Stage 1,
[ADR-1134](../adr/1134-vmafx-ort-runner-in-tree.md)).

You normally never invoke it by hand — `pkg/ai.Registry.Infer` does, by looking
it up on `PATH`. This page exists so you can tell whether the runner is
installed and working, and so the wire format is written down for anything
else that wants to speak it.

## Synopsis

```bash
vmafx-ort-runner --model <path.onnx> --inputs '<JSON array of numbers>' [--input-name <name>]
```

| Flag | Required | Meaning |
|---|---|---|
| `--model` | yes | Path to the `.onnx` file. The caller resolves it (`pkg/ai` passes an absolute path from `VMAFX_MODEL_DIR`). |
| `--inputs` | yes | One JSON array of numbers. Bound to the graph input as a float32 row vector of shape `[1, N]`, `N` = array length. |
| `--input-name` | no | Bind to this graph input **name** instead of positionally (the first input). A wrong name fails with exit 1. |

## Output and exit codes

stdout carries exactly one line: the flattened output tensor as a JSON array
of numbers (`[66.13961791992188]`). Nothing else is ever written to stdout;
diagnostics — including ONNX Runtime's own provider log lines — go to stderr.

| Exit | Meaning | What to do |
|---|---|---|
| 0 | Success; stdout holds the result. | — |
| 1 | The model could not be opened or run: missing file, over libvmaf's 50 MB size cap, an operator outside the allowlist, a graph with more than one input or output, a wrong `--input-name`, or an ONNX Runtime failure. | Read stderr. Multi-input graphs (`fr_regressor_v2`) are outside the runner's protocol — see [Limits](#limits). |
| 2 | Usage error: a flag is missing, or `--inputs` is not a non-empty JSON array of float32-representable numbers. | Fix the invocation. |
| 3 | libvmaf was built without ONNX Runtime. | Rebuild libvmaf with `-Denable_dnn=enabled` against an installed onnxruntime — see [Build](#build). |

`pkg/ai.Registry.Infer` surfaces the exit status **and** the runner's stderr in
its error, so `vmafx-tune predict --model …` logs `vmafx-ort-runner failed:
exit status 3: … libvmaf was built without DNN support …` before it falls back
to the analytical curve, and `vmafx-ort-runner not found on PATH` when the
binary is simply not installed.

## Example

```bash
vmafx-ort-runner --model model/predictor_libx264.onnx \
  --inputs '[51,1,1,1,1,0,0,0,0,0,1,1,16,16]'
# [66.13961791992188]
```

That row is the smoke test the Go CI job and the dev container image build
run. It is deliberately unrealistic: the shipped predictors saturate at `100.0`
for ordinary shots, and a saturated output cannot tell a correct forward pass
from a mis-bound tensor. The value is onnxruntime 1.29.0's CPU execution
provider answer for the same row, bit-identical through libvmaf.

## Build

The runner is a cgo binary over `pkg/libvmaf`, so it needs libvmaf's headers
and `libvmaf.so` — the same requirement as `vmafx-server`, `vmafx-mcp`,
`vmafx-controller` and `vmafx-node`. It is produced by:

- `go build ./cmd/...`, or `make go-ort-runner`, which writes
  `./vmafx-ort-runner` at the repository root;
- the dev container's `go-build` stage — `/usr/local/bin/vmafx-ort-runner` is
  on `PATH` inside `vmaf-dev-mcp`, and the image build fails unless the runner
  reproduces the reference value above;
- the Go CI job (`.github/workflows/go-ci.yml`), which builds it, puts it on
  `PATH` for `go test ./...` and smoke-tests it against the shipped predictor.

For a **working** runner the libvmaf it links must itself link ONNX Runtime:

```bash
meson setup core/build-cpu core -Denable_dnn=enabled   # needs libonnxruntime.pc
ninja -C core/build-cpu
go build -o vmafx-ort-runner ./cmd/vmafx-ort-runner
LD_LIBRARY_PATH=core/build-cpu/src ./vmafx-ort-runner \
  --model model/predictor_libx264.onnx --inputs '[51,1,1,1,1,0,0,0,0,0,1,1,16,16]'
```

Against a libvmaf built with `-Denable_dnn=disabled` (or `auto` on a host
without onnxruntime) the binary still builds and runs, but every invocation
exits 3. Nothing under `docker/` ships the runner: the production node and
server images build libvmaf without DNN, and no code path in those binaries
execs it today.

## Limits

- **One input, one output, `[1, N]` float32.** The runner binds the array as a
  single row; a graph with several inputs (`fr_regressor_v2`'s `features` +
  `codec` ports) or an input rank other than 2 fails with exit 1. This is the
  same limit `pkg/ai.Registry.Infer` has; see
  [vmafx-tune-go.md](vmafx-tune-go.md#production-mode-blocker-onnx-named-inputs).
- **argv-sized tensors.** Inputs travel in `--inputs`, so they are bounded by
  the platform's `ARG_MAX` — fine for the 14-float per-shot predictors, not
  for a 3×H×W saliency frame. A stdin transport is the documented follow-up in
  [vmafx-tune-go.md](vmafx-tune-go.md#saliency-onnx-inference).
- **Execution provider is libvmaf's `AUTO` chain.** On a CPU-only ONNX Runtime
  the chain's CUDA probe logs a `Failed to load library
  libonnxruntime_providers_cuda.so` line on stderr before it falls back to the
  CPU provider; that line is harmless and never reaches stdout.
- **One process per inference.** Every call opens and closes a session. The
  per-shot predictor path makes a handful of calls per clip, which is what
  this was sized for.
