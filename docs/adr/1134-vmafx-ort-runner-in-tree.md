<!-- markdownlint-disable MD013 MD060 -->
# ADR-1134: Build `vmafx-ort-runner` in-tree as a cgo shim over libvmaf's DNN session API

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: Lusoris
- **Tags**: `go`, `ai`, `onnx`, `build`, `ci`, `container`, `vmafx-tune`, `fork-local`

## Context

`vmafx-ort-runner` is the subprocess that `pkg/ai.Registry.Infer` execs for
every ONNX inference on the Go side: `vmafx-tune predict / sidecar / auto
--model`, `pkg/tune/predictor`, and `pkg/fast.ORTProxy`. ADR-0713 (2026-05-28)
chose that subprocess design deliberately — "Stage 1 uses a subprocess to run
ORT sessions; this avoids CGO coupling on libtensorrt at the Go layer" — so the
pure-Go `vmafx-tune` never links ONNX Runtime. The binary itself was never
delivered. On 2026-09-02 the name appeared in **22 files** and was produced by
nothing: no `cmd/vmafx-ort-runner`, no Makefile rule, no `dev/Containerfile`
step, no CI job, no `docker/` stage. Every `--model` invocation therefore hit
`ErrORTRunnerNotFound` and degraded to the analytical curve, while the tree
described the runner variously as "bundled in the container image", "an
external binary with no source in this repo", and "not built from this
repository".

The reference list, which is the evidence for the decision below:

| Role | Files |
| --- | --- |
| Consumer (execs it) | `pkg/ai/infer.go` (`exec.LookPath("vmafx-ort-runner")`) |
| Callers through `pkg/ai` | `cmd/vmafx-tune/cmd/ortsession.go`, `cmd/vmafx-tune/cmd/predict.go`, `pkg/fast/fast.go`, `pkg/fast/proxy.go` |
| Documented blockers that name it | `cmd/vmafx-tune/cmd/saliencysession.go`, `cmd/vmafx-tune/cmd/recommend_saliency.go`, `cmd/vmafx-tune/AGENTS.md`, `pkg/codecadapter/AGENTS.md` |
| Tests | `pkg/ai/infer_test.go` (PATH="" → not found), `pkg/fast/proxy_test.go`, `pkg/predictor/session_fallback_test.go` |
| User docs / research | `docs/usage/vmafx-tune-go.md`, `docs/research/vmafx-tune-go-fast-2026-08-30.md`, `docs/research/gosec-findings-fix-sweep-2026-06-01.md`, `docs/adr/0713-vmafx-node-impl.md`, `docs/state.md` |
| Changelog (historical, rendered) | `CHANGELOG.md`, `changelog.d/added/vmafx-tune-go-corpus-sidecar.md`, `changelog.d/added/vmafx-tune-go-fast-subcommand.md`, `changelog.d/added/vmafx-tune-go-ml-driven-subcommands.md`, `changelog.d/fixed/gorust-probe-timeout-rust-picture-double-free.md` |

Three facts settle whether the runner is a phantom to delete or a binary to
build. (1) Fourteen `model/predictor_*.onnx` per-shot predictors ship in-tree,
each a single-input `[1, 14]` float32 graph whose only Go consumer is this
seam. (2) The Python `vmaf-tune` runs those models in-process with
`onnxruntime`, and the Go port's parity contract (ADR-0705, ADR-0730)
documents `--model` as ported "with the same fallback as Python". (3) The fork
already has an in-process ONNX Runtime binding in Go — `pkg/libvmaf/dnn.go`
wraps libvmaf's `vmaf_dnn_session_open` / `vmaf_dnn_session_run`
(`libvmaf/dnn.h`) through cgo and is used by `vmafx-mcp`'s model-evaluation
tools — so the runner needs no new dependency at all. Deleting the reference
would orphan the models, break the parity contract, and remove a documented
CLI surface; the missing piece is roughly 150 lines of glue.

Constraints: `vmafx-tune` and `vmafx-operator` must stay pure Go (dev
container comment, ADR-1119 §5); `go build ./...` on a developer host must not
acquire a new C dependency; the runner must be produced and smoke-tested by
the same builds that ship the other Go binaries (CLAUDE.md §12 rule 15 —
container is canonical); every touched file is lint-clean (ADR-0141).

## Decision

We will build `cmd/vmafx-ort-runner` in this repository as a thin cgo shim
over `pkg/libvmaf.DNNSession`. The runner keeps the wire format `pkg/ai`
already speaks — `--model <path> --inputs '<JSON array>'` in, one JSON array
of numbers on stdout out — binds the array positionally to the graph's single
input as a `[1, N]` float32 row (`--input-name` selects by-name binding), and
exits 0 on success, 1 on any open/run failure, 2 on a usage or protocol
error, and 3 when libvmaf was built without ONNX Runtime. It is produced by
`go build ./cmd/...`, by a new `make go-ort-runner` target, by the dev
container's `go-build` stage (which now asserts seven binaries and smoke-runs
the runner against `model/predictor_libx264.onnx` in the `dev-mcp` stage),
and by the Go CI job, which installs the ONNX Runtime 1.29.0 tarball, builds
libvmaf with `-Denable_dnn=enabled`, puts the runner on `PATH` for
`go test ./...` and smoke-tests it against the shipped predictor.
`pkg/libvmaf.DNNSession.Predict` gains positional binding for an empty input
name (NULL `VmafDnnInput.name`), and `pkg/ai.Registry.Infer` reports the
runner's stderr alongside its exit status. The runner uses stdlib `flag` and
no golusoris/fx wiring: it is a one-shot subprocess spawned per inference with
no configuration, no logger and no lifecycle to manage.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(chosen)** Go `cmd/` over `pkg/libvmaf.DNNSession` | No new dependency; inherits libvmaf's size cap, op allowlist and EP selection (ADR-0211); same cgo/link requirement as four existing binaries; ~150 lines | Runner is only as capable as `dnn.h`'s single-input `[rows, cols]` binding; needs libvmaf built with DNN to be useful | — |
| Treat as phantom: delete every reference and the dependent code paths | Smallest tree | Orphans 14 shipped predictor models; removes the documented `--model` surface on `predict`/`sidecar`/`auto`; breaks Python parity; contradicts ADR-0713's design | The evidence says needed, not phantom |
| Go `cmd/` with direct cgo against `onnxruntime_c_api.h` | Full ORT C API (named inputs, arbitrary ranks) | Second ORT binding in the tree beside `core/src/dnn`; adds `libonnxruntime` headers as a build dependency of `go build ./...` (or a build tag that silently skips the runner — the phantom again); bypasses libvmaf's model hardening | Duplicates what libvmaf already owns |
| Go `cmd/` via `purego` dlopen of `libonnxruntime.so` (no cgo) | Keeps CGO_ENABLED=0 | New dependency; hand-indexed `OrtApi` function-pointer table pinned to an ORT struct layout; no compile-time type checking | Fragile for no gain — the runner already lives beside four cgo binaries |
| C tool under `core/tools/` linked against libvmaf | Built by meson with `enable_dnn`; no Go involvement | A second implementation of the protocol (JSON parse/emit in C under the Power-of-10 profile) with no consumer outside Go; `go test` cannot build it | Wrong language for a Go-only seam |
| Python console script (`ai/` package, `onnxruntime` already a dependency) | Twenty lines; identical numerics to the Python reference | Runs against the Python sunset (ADR-0703/0704: "the Python cannot be removed from the image before the Go replacements are in it"); production Go images ship no Python | Strategic direction |
| Replace the subprocess with in-process `pkg/libvmaf` calls inside `vmafx-tune` | One fewer process | Makes `vmafx-tune` a cgo build — ADR-level change to the binary's build contract (`saliencysession.go`, dev container comment) | Out of scope; the runner keeps that option open as Stage 2 |
| Wrap the runner in golusoris/fx like the six service binaries (ADR-1119) | Uniform bootstrap | Millisecond-scale startup cost paid once per inference; nothing to inject (no config, no logger, no listeners) | ADR-1119 §5 keeps domain code framework-agnostic; a subprocess protocol shim is closer to `pkg/` than to a service |

## Consequences

- **Positive**: `vmafx-tune predict/sidecar/auto --model` performs real ONNX
  inference inside the dev container and in Go CI; the value is bit-identical
  to `onnxruntime`'s CPU provider for the shipped predictor. The 22 references
  now describe an artefact that exists. Fallback diagnostics name the actual
  cause (`not found on PATH` vs `exit status 3: … built without DNN support`).
- **Positive**: the Go CI job now links a libvmaf that carries ONNX Runtime,
  so the real-ORT branches of `pkg/libvmaf/dnn_test.go` and
  `cmd/vmafx-mcp`'s native evaluation tests execute instead of skipping.
- **Negative**: the Go CI job downloads the ORT tarball (~60 MB) on every
  run and its libvmaf build gains the DNN module; roughly a minute of wall
  time.
- **Negative**: one session open per inference call. Adequate for the
  per-shot predictor's handful of calls per clip; a batching protocol is a
  follow-up if a caller ever needs thousands.
- **Neutral / follow-ups**: the runner serves single-input graphs only, so
  `fr_regressor_v2` (two ports) and saliency (argv-sized tensors) stay
  blocked exactly as before — but the documented unblocks (a named-input
  protocol plus `Registry.InferNamed`; a stdin transport) are now protocol
  extensions of in-tree code. The production `docker/Dockerfile.node` and
  `Dockerfile.go-server` images build libvmaf without DNN and do not ship the
  runner; no code path in those binaries execs it. `renovate.json` tracks the
  `ORT_VERSION` pin in `go-ci.yml` alongside `dev/Containerfile`.

## Supply-chain impact

- **New dependencies**: none in `go.mod`. `cmd/vmafx-ort-runner` imports only
  the standard library and `pkg/libvmaf`.
- **Build-time fetches**: `.github/workflows/go-ci.yml` now fetches
  `onnxruntime-linux-x64-1.29.0.tgz` from `github.com/microsoft/onnxruntime`
  releases (MIT), pinned by version and tracked by renovate — the same
  artefact and pin `dev/Containerfile` already installs. No digest pin, matching
  the existing Containerfile and DNN-job practice.
- **Sigstore-signable**: the runner is a Go binary built by the same stage as
  the other six; it inherits whatever provenance that stage produces.
- **CVE surface delta**: none new — the runner links the ONNX Runtime the
  image already ships and exposes no listener.

## References

- [ADR-0713](0713-vmafx-node-impl.md) — designed the Stage 1 subprocess bridge this ADR delivers.
- [ADR-0211](0211-model-registry-sigstore.md) — the model size cap and operator allowlist the runner inherits through libvmaf.
- [ADR-1119](1119-golusoris-go-framework-adoption.md) §5 — framework-agnostic domain code; why the runner has no fx wiring.
- [ADR-0705](0705-vmafx-tune-go-stage1.md), [ADR-0730](0730-vmafx-tune-go-stage2.md) — the `vmafx-tune` parity contract that `--model` belongs to.
- [docs/research/vmafx-tune-go-fast-2026-08-30.md](../research/vmafx-tune-go-fast-2026-08-30.md) §4.1 — the option table that marked "extend the runner" as preferred but blocked on its missing source.
- User docs: [docs/usage/vmafx-ort-runner.md](../usage/vmafx-ort-runner.md).
- Source: per user direction (task brief, 2026-09-02) — resolve `vmafx-ort-runner`, referenced by 22 files but built by nothing, either by wiring it into the build, Makefile, dev container and CI with a smoke test, or by removing every reference; write the decision down with the reference list as evidence.
