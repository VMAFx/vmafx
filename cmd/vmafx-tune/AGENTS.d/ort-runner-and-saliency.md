---
paths:
  - pkg/predictor/*.go
  - cmd/vmafx-ort-runner/*.go
  - cmd/vmafx-tune/cmd/predict*.go
  - cmd/vmafx-tune/cmd/recommend*.go
  - cmd/vmafx-tune/cmd/saliency*.go
invariant: vmafx-ort-runner built in-tree; predict + recommend share saliencySessionFactory.
---
<!-- markdownlint-disable MD013 MD029 -->
# `vmafx-ort-runner` in-tree contract and saliency session factory

## Rebase-sensitive invariants

27. **`vmafx-ort-runner` is repository artefact, not environment
    assumption** (`cmd/vmafx-ort-runner`, ADR-1134): `pkg/predictor.ORTSession`
    (one ORT-session adapter since ADR-1137; `pkg/tune/predictor` = its
    transitional alias) execs it through `pkg/ai.Registry.Infer`; dev
    container plus Go CI job build it from `./cmd/...`, smoke-run
    it against `model/predictor_libx264.onnx`. When `predict --model`
    degrades to analytical curve, log line carries runner's
    stderr — `exit status 3` means linked libvmaf has no ONNX Runtime,
    `not found on PATH` means it is not installed. Do not reintroduce
    comments or docs describing runner as external or "bundled by
    image": that claim hid missing binary for three months.
    Runner still takes one flat `[1, N]` vector (invariant 20 stands);
    named inputs and stdin transport = protocol extensions of
    in-tree runner, not new dependencies.
28. **Saliency moments reach `predict` via shared session factory
    (PR for #1272 gaps).** `predict --use-saliency` opens session via
    `saliencySessionFactory` (same path `recommend --saliency` uses), writes
    per-shot population moments into `ShotFeatures.SaliencyMean` /
    `SaliencyVar`, landing at `predictor.FeatureVector` indices 5-6.
    Nonexistent `--saliency-model` = usage error (exit 2, naming file);
    unavailable runtime degrades to `(0, 0)` with log line. Never bypass
    factory with direct `newSaliencySession`; any change to
    feature-vector layout updates `predictor.FeatureVector` and
    `saliency_honesty_test.go` in same commit. Retired Python CLI
    redirects in `compare`/`root`/`main` gone; do not reintroduce
    "use vmaf-tune (Python)" hints for subcommands Go binary implements.
