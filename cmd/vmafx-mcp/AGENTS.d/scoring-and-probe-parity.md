---
paths:
  - cmd/vmafx-mcp/tools.go
  - cmd/vmafx-mcp/impl.go
  - cmd/vmafx-mcp/impl_direct*.go
  - cmd/vmafx-mcp/parity_argv_test.go
  - cmd/vmafx-mcp/score_extras_test.go
  - pkg/scoreopts/*.go
  - mcp-server/vmaf-mcp/src/vmaf_mcp/server.py
invariant: Generated options.gen.json schema/argv; legacy %.6f default; 64x64 probe; CLI backend_used receipt.
---
<!-- markdownlint-disable MD013 MD029 -->
# Scoring schema/argv parity, precision default and backend probe

## Rebase-sensitive invariants

10. **Go↔Python byte-identical scoring surface** (ADR-1117 / ADR-2044 / #1240): Go server (`tools.go` `toolRegistrar.addGenerated` + `pkg/scoreopts` `options.gen.json`, `impl.go` `parseScoreExtras` / `buildVmafArgv`) and Python server (`server.py` `score_options.tool_schema` + `vmaf_mcp/options.gen.json`, `_extras_from_args` / `_build_vmaf_argv`) derive scoring input schemas and flags from `core/api/vmafx.toml` (`scripts/codegen/vmafx-api.py --write`, invariant #21) and MUST expose identical `vmaf_score` / `vmaf_score_encoded` schema (property names, types, enums, defaults, required-ness, device selectors `--cpumask`, `--gpumask`, `--sycl_device`, `--hip_device`, `--metal_device`, `output_fmt`, `subsample`, tiny-AI flags) AND build identical `vmaf` CLI argv. Client gets same result from either server — 1 exception in invariant #15. Changing scoring param updates `core/api/vmafx.toml`, regenerates both `options.gen.json` copies, and keeps canonical argv ORDER identical (`--subsample` only when `>1`, emitted before extras). `TestGoAndPythonArgvParity` (Go) and `test_parity_argv.py` (Python) enforce byte-identical CLI invocation; `score_extras_test.go` and `tests/test_score_extras_adr1117.py` pin schema and bounds validation against `core/tools/cli_parse.cpp`. Allowed roots must include `python/test/resource/yuv` in both servers so worktree symlinks resolve.

12. **probe_backend parity invariant** (ADR-0608 follow-up): Go `handleProbeBackend` (`impl.go`) and Python `_probe_backend` (`server.py`) MUST use same synthetic probe frame and `runtime_healthy` predicate. Frame = **64x64** 4:2:0 8-bit mid-grey (`probeYUVWidth`/`probeYUVHeight` ↔ `_PROBE_YUV_WIDTH`/`_PROBE_YUV_HEIGHT`): must NOT shrink below 36px per dimension (CUDA ADM kernel returns null score under minimum; naive `runtime_healthy=true` misreports healthy backend). `runtime_healthy` = true iff subprocess exits 0 AND pooled `vmaf.mean` score non-null (Go additionally rejects non-finite floats); on null/non-finite score both servers set `runtime_healthy=false` with error string `"vmaf returned exit 0 but score was null"`. `TestProbeYUVDimensions` / `TestScoreIsHealthy` (Go) and `tests/test_probe_backend_pr850.py` (Python) pin both sides.

14. **Score-precision default parity** (ADR-0119 / ADR-1117): every MCP scoring path — Go stdio/subprocess (`impl.go`), Go direct-cgo fallback (`impl_direct.go`), Python stdio (`server.py`), Python HTTP `/v1/score` (`http_transport.py`) — MUST default `precision` arg to `legacy` (`%.6f`, documented C-CLI default). Do not reintroduce `"17"` default on any path: client must get same numeric format regardless of server / transport / dispatch path.

20. **Auto-dispatch backend identity comes from CLI receipt, never metric counts.** `decodeVmafOutput` accepts concrete top-level `backend_used` value (`cpu`, `cuda`, `sycl`, `hip`, or `metal`) when request used `auto`; missing, generic `gpu`, `auto`, non-string, and unknown receipts become `unknown`. Metric-key counts are run observations and move as extractors change. Keep non-object JSON rejection before response annotation. Explicit subprocess requests echo requested backend; direct-cgo path remains separate, keeps `cpu (direct cgo)`.

21. **Scoring schemas + argv from generated options** (RC4 WP8, ADR-2044).
    `vmaf_score`, `vmaf_score_encoded`, `describe_worst_frames` registered via
    `toolRegistrar.addGenerated`: schema = `pkg/scoreopts` `options.gen.json`
    (generated from `core/api/vmafx.toml`, byte-identical to Python copy
    `mcp-server/vmaf-mcp/src/vmaf_mcp/options.gen.json`). Validation =
    `doc.FromMCP` (bounds, enums, reserved); flags = `cliFlag()` /
    `doc.ExtraArgs` / `scoreExtras.modelSpec`. Hand-written only: tiny_device
    vs dnn_ep fold, csv/sub/output_fmt agreement, no_reference needs
    tiny_model, backend -> `--no_<sibling>`. Do not bring back
    `scoringExtraProperties` or hand flag lists. Omitted `model` =
    `version=` + `library_defaults[VMAF_DEFAULT_MODEL_VERSION]`, never a
    literal. New scoring argument = definition entry + regenerate + both
    servers unchanged unless it needs a hand rule.
