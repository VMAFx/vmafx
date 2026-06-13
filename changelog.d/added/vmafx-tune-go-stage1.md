### vmafx-tune-go Stage 1 — Go port of vmaf-tune compare subcommand

Adds `cmd/vmafx-tune/` — a Go port of the Python `vmaf-tune` rate-quality
tuning CLI, installed as `vmafx-tune-go` alongside the Python binary.

Stage 1 ships one fully-functional subcommand:

- **`compare`** — Rate-quality sweep: for each `(codec, VMAF target)` pair,
  bisects the CRF axis to find the highest CRF whose measured VMAF still meets
  the target, then reports bitrate and score. Supports libx264 and libx265.
  Outputs schema-v1 (single target) or schema-v2 (multi-target sweep) JSON
  compatible with the Python `report.py` renderer.

New Go packages (no C CGo dependencies in Stage 1):

- `pkg/encoder/` — `Encoder` interface, `LibX264Encoder`, `LibX265Encoder`.
- `pkg/bisect/` — Stateless VMAF-target CRF bisect, score function injectable.
- `pkg/report/` — JSON (RFC 8259 strict, NaN → null) and Markdown renderers.

All other subcommands (`tune-per-shot`, `ladder`, `fast`, `corpus`, `report`,
`benchmark`, `auto`, `sidecar`) are stubs that redirect to `vmaf-tune`.
The Python `tools/vmaf-tune/` is unchanged.

See [docs/usage/vmafx-tune-go.md](docs/usage/vmafx-tune-go.md) and
[ADR-0705](docs/adr/0705-vmafx-tune-go-stage1.md).
