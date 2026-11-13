<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 -->
# AGENTS.md — cmd/vmafx-tune

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

Parent: [../../AGENTS.md](../../AGENTS.md).

Go port of vmaf-tune rate-quality tuning CLI. Installed as `vmafx-tune-go`
during migration; see Stage roadmap in
[ADR-0705](../../docs/adr/0705-vmafx-tune-go-stage1.md).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `/pkg/bisect/*.go`, `cmd/compare*.go`, `cmd/ladder*.go` | [bisect-and-scoring](AGENTS.d/bisect-and-scoring.md) | Midpoint rounds higher (lo+hi+1)/2; keep ScoreFunc seam; Y4MScorer vs YUVScoreFunc distinct. |
| `main*.go`, `cmd/root*.go`, `cmd/golusoris.go`, `cmd/otel_test.go`, `cmd/required_flags*.go`, `cmd/exitcode*.go` | [cli-root-and-fx](AGENTS.d/cli-root-and-fx.md) | Install as vmafx-tune-go; one-shot withGolusoris + fx.NopLogger; command span; usage errors exit 2. |
| `/pkg/encoder/*.go`, `/pkg/hwdevice/*.go`, `/pkg/ffencode/*.go`, `/pkg/codecadapter/*.go` | [encoder-and-hwdevice](AGENTS.d/encoder-and-hwdevice.md) | Absolute vs Quality CRF windows; pre-input InputArgs vs post-input ExtraArgs; QSV/AMF argv parity. |
| `/pkg/bisect/*.go`, `/pkg/encoder/*.go`, `/pkg/storage/*.go`, `/cmd/vmafx-controller/queue/*.go` | [errors-join-cleanup](AGENTS.d/errors-join-cleanup.md) | Combine primary + cleanup errors via errors.Join; ignore os.ErrNotExist; slog key = error. |
| `cmd/fast*.go`, `cmd/root.go`, `/pkg/fast/*.go` | [fast-and-proxy](AGENTS.d/fast-and-proxy.md) | Fast exit codes 0/2/3; sorted RecommendResult + pythonFloatRepr; 1-port ORT guard; sequential TPE. |
| `/pkg/predictor/*.go`, `/cmd/vmafx-ort-runner/*.go`, `main*.go`, `cmd/root*.go`, `cmd/compare*.go`, `cmd/predict*.go`, `cmd/recommend*.go`, `cmd/saliency*.go` | [ort-runner-and-saliency](AGENTS.d/ort-runner-and-saliency.md) | vmafx-ort-runner built in-tree; predict + recommend share saliencySessionFactory. |
| `/pkg/pershot/*.go`, `cmd/pershot*.go` | [pershot-plan](AGENTS.d/pershot-plan.md) | Alphabetical wire fields + pyFloat for byte-identical plan JSON; unported flags fail fast. |
| `/pkg/report/*.go`, `cmd/report*.go`, `/pkg/pyjson/**` | [report-and-json](AGENTS.d/report-and-json.md) | Additive JSON schema; NaN -> null; Row vs WireRow split; report key probe; pyjson for parity. |
| `cmd/sidecar*.go`, `cmd/testdata/sidecar/**` | [sidecar-parity](AGENTS.d/sidecar-parity.md) | Validation errors exit 2, state I/O exit 1; byte-identical Python fixtures + universal newlines. |
