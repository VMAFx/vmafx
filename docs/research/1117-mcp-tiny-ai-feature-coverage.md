# Research digest: MCP tiny-AI / feature / CTC coverage (2026-06-14)

## Scope

This digest records the investigation behind ADR-1117: extending the MCP
`vmaf_score` and `vmaf_score_encoded` tools to reach the fork's Tiny-AI / DNN
scoring surface, feature selection, and the Common-Test-Conditions presets,
while preserving the Go ↔ Python byte-compatibility invariant.

## The gap

The RC-readiness audit (`.workingdir2/rc/RC_SCOPE_LOCKED.md`, "Workstream C —
MCP coverage gap list") quantified the gaps. Both MCP servers shell out to the
`vmaf` CLI but only forwarded `model` / `backend` / `precision` / `subsample`.
The audit ranked the Tiny-AI / DNN surface as **RC-PRIORITY 1** because it is
the fork's largest distinguishing feature set and was **0 %** reachable over
MCP — eleven flags entirely absent. Feature selection (`--feature`) and the
CTC presets (`--aom_ctc` / `--nflx_ctc`) were PRIORITY 2, and a tail of
score-completeness flags (`--threads`, `--frame_cnt`, `--frame_skip_*`,
`--no_prediction`) PRIORITY 4.

The audit also corrected one earlier false lead: there is **no `--pool`
flag** on the CLI (pooling is hardcoded to MEAN), so the "missing pooling
selector" hinted in earlier notes is not a real gap and was not implemented.

## Source-of-truth verification

Every parameter was verified against `core/tools/cli_parse.c` (the parser) and
its `usage()` help text, rather than from memory:

- Tiny-AI flags: the `tiny-*` family is declared at `cli_parse.c:151–180`,
  with both `-` and `_` spellings; `--dnn-ep` is an explicit alias for
  `--tiny-device` (both write `settings->tiny_device`). Accepted
  `--tiny-device` values enumerated in the help block at `cli_parse.c:250–253`
  (`auto|cpu|cuda|openvino|openvino-npu|openvino-cpu|openvino-gpu|coreml|
  coreml-ane|coreml-gpu|coreml-cpu|rocm`). `--tiny-resize` accepts
  `bilinear|nearest|bicubic|disabled` (`cli_parse.c:889`); `--tiny-crf` is
  clamped to `[0,63]` (`cli_parse.c:877`).
- `--no-reference` requires a tiny model and forces `--no_prediction`
  (`cli_parse.c:991–1002`): "the only scorer that can produce a value without
  a reference is a no-reference tiny ONNX model". This drove the NR-mode gate
  and the optional-`ref` handling in both servers.
- CTC versions: `parse_aom_ctc` accepts `v1.0…v7.0` (`proposed` deprecated;
  `cli_parse.c:606–646`); `parse_nflx_ctc` accepts `v1.0` only
  (`cli_parse.c:675–682`).
- Output formats `--csv` / `--sub` set `output_fmt` (`cli_parse.c:731–734`).
  Because both MCP handlers parse the JSON output file (`-o <tmp> --json`),
  exposing `--csv`/`--sub` would break that parse — so they were deliberately
  **excluded** from the score tools.

## Byte-compatibility approach

The invariant (`cmd/vmafx-mcp/AGENTS.md` §1) is that the Go and Python tool
schemas must match exactly. Rather than maintaining two hand-written schemas
per tool, a single shared schema generator was added to each server
(`scoringExtraProperties()` in Go, `_scoring_extra_properties()` in Python) and
spliced into both `vmaf_score` and `vmaf_score_encoded`. The argv construction
is likewise centralised (`scoreExtras.appendArgs` / `ScoreExtras.to_argv`) with
a fixed flag order so the two servers emit identical command lines.

Verification method: both servers' live `vmaf_score` + `vmaf_score_encoded`
input schemas were dumped to JSON and compared as canonical (sorted-key) JSON.
The diff surfaced exactly one pre-existing inconsistency — the
`reference_encoded` description used a Unicode ellipsis (U+2026) in Python vs
ASCII `...` in Go — which was reconciled to ASCII. After that, both tool
schemas are byte-identical.

## Decisions and trade-offs

The full decision matrix (optional params on existing tools vs new dedicated
tools vs a raw `extra_args` pass-through) lives in ADR-1117's
"Alternatives considered" section. The chosen approach keeps the operation
model 1:1 with the CLI, leaves existing callers untouched, and confines the
byte-compat surface the parity tests must police to a single shared generator.

## Outcome

All eleven tiny-AI flags plus feature selection, the CTC presets, and the
frame-range / worker controls are now reachable over MCP through both servers.
NR mode works end-to-end. Go (`go build`/`vet`/`test`) and Python
(`py_compile` + pytest + `ruff`) gates are green, and the schema diff confirms
byte-identity.
