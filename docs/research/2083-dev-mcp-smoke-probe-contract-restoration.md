# Research-2083: dev-MCP smoke-probe contract restoration

- **Status**: Active
- **Workstream**: BUG-048 A13; no ADR (bug fix)
- **Last updated**: 2026-09-24

## Question

Could `dev/scripts/smoke-probe-loop.sh` still prove that each requested VMAFx
backend and the production MCP service returned a real score, or had later CLI,
backend, and MCP migrations invalidated the probe while leaving it apparently
operational?

## Sources

- `core/tools/cli_parse.cpp` for the current raw-input and exclusive-backend
  grammar.
- `core/tools/vmaf.cpp::amend_json_with_backend_used` for the backend receipt.
- `cmd/vmafx-mcp/main.go`, `tools.go`, and `impl.go` for the production stdio
  transport, current tool names, schemas, and response shape.
- `cmd/vmafx-mcp/server_test.go` and the MCP transport tests for the initialize
  sequence and 8-bit fixture call.
- `48dcc7ddb` (PR #991), the original probe correction, and the A13 row in
  `.workingdir/evidence/silent-reverts-2026-09-18.md` for the regression
  history.
- [ADR-0726](../adr/0726-drop-vulkan-backend.md) for the active backend set.

## Findings

The ledger's three obsolete flags were real, but they were not the complete
failure:

1. The current selector is `--backend cpu|cuda|sycl|hip`; the dedicated
   `--cuda`, `--sycl`, and `--hip` switches do not exist.
2. Raw input requires all four geometry fields. The probe supplied
   `yuv420p`, while the CLI accepts `420`, and it omitted `--bitdepth 8`.
3. `--no_prediction` suppresses the model prediction the probe is meant to
   measure. The old `--no_prediction_flags` spelling was invalid too.
4. The CLI writes pooled metrics to its selected output file and deliberately
   does not print a `VMAF score:` line on redirected stderr. The probe sent the
   JSON to `/dev/null` and then grepped process output, so even a successful run
   had no score source.
5. A score alone does not prove exclusive dispatch. Current JSON carries
   `backend_used`, which must equal the requested backend to reject fallback or
   wrong-device evidence.
6. The production service is the Go `vmafx-mcp` binary. The Python
   `vmaf-mcp-server` entry point and the `list_features` / `compute_vmaf`
   operations were retired; their replacements are `list_extractors` and
   `vmaf_score`, with different argument and result schemas.
7. MCP stdio is a session protocol. A bare `tools/call` before `initialize`
   is rejected by the Go SDK. Each probe now performs the initialize response
   sequence before calling a tool.
8. A producer-to-server-to-parser shell pipeline closed the producer side as
   soon as it wrote the request. The Go SDK treats stdin EOF as a session
   disconnect, so it could tear down the session before the asynchronous tool
   response was written. The probe now owns the subprocess lifecycle, keeps
   stdin open until response ID 2 arrives, and closes it only afterward.
9. Shell quote replacement did not escape tabs or every JSON control byte, so
   a backend diagnostic could corrupt the complete probe file.
10. Tab is IFS whitespace in Bash. When a failed helper emitted an empty first
   field, `read` discarded it and shifted the duration and error into the score
   and duration fields. A non-whitespace delimiter plus an explicit JSON
   `null` score preserves field identity.
11. The operator guide still advertised the removed Vulkan backend, the old
    MCP binary/tool names, and a fixture score from a different pair.

The output keys `mcp_results.list_features` and
`mcp_results.compute_vmaf` are kept as compatibility keys. Only the operations
behind them change to the current Go MCP surface.

## Alternatives explored

| Alternative | Result |
| --- | --- |
| Restore aliases for the removed flags, Python server, and old MCP tools | Rejected: it would grow duplicate compatibility surfaces solely to keep a broken internal probe alive. |
| Keep scraping human CLI output | Rejected: non-TTY output intentionally omits the line, and text does not carry an exact backend receipt. |
| Send only `tools/call` to the Go server | Rejected: it violates the MCP session contract and is rejected before the tool handler. |
| Pipe a finite request producer into `vmafx-mcp` | Rejected: producer EOF races the Go SDK's asynchronous response and yielded an empty result in the exact-source image. |
| Rename the probe JSON keys to match the new MCP tools | Rejected for this bug fix: existing probe consumers may rely on those schema keys; the guide now distinguishes stable keys from operation names. |
| Rewrite the complete loop in Python | Deferred as unnecessary: the shell supervisor is small once JSON encoding/parsing is delegated to Python and its field boundary is made unambiguous. |

No new ADR is needed. This restores already-decided CLI, MCP, and backend
contracts and does not introduce an architectural choice.

## Verification

`dev/scripts/test-smoke-probe-loop.sh` is a hermetic end-to-end contract test.
Against the pre-fix script it failed while parsing the emitted probe as JSON.
It now exercises three complete `--once` runs:

- every backend and both MCP operations succeed;
- HIP fails with quotes, a backslash, and a tab in its diagnostic while the
  enclosing probe remains valid JSON and its score remains `null`; and
- SYCL exits successfully but reports `backend_used=cpu`, which the probe
  rejects instead of publishing as SYCL evidence.

The script also asserts the exact `420` / 8-bit CLI arguments, current tool
names, two initialize handshakes, response-before-EOF behaviour, and absence
of retired or prediction-suppressing flags. `bash -n`, `shfmt`, and
`shellcheck` cover both scripts. A current dev-container run supplies native
CLI/MCP acceptance before delivery.

## Open questions

None for the contract restoration. Individual GPU backends may correctly
report a hardware/runtime error on a host without that device; the probe's job
is to preserve that error as valid evidence, not to turn hardware absence into
success.

## Related

- BUG-048 A13
- [ADR-0726](../adr/0726-drop-vulkan-backend.md)
- [dev-MCP operator guide](../development/dev-mcp.md)
