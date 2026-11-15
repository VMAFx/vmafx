---
paths:
  - cmd/vmafx-mcp/tools.go
  - cmd/vmafx-mcp/tool_contract_test.go
  - cmd/vmafx-mcp/tool_schema_test.go
  - mcp-server/vmaf-mcp/tool-contract.json
invariant: Match Python tool-contract.json; register via registerTools helpers; fail closed on bad schema.
---
<!-- markdownlint-disable MD013 MD029 -->
# Tool contract parity and fail-closed registration (`tools.go`)

## Rebase-sensitive invariants

1. **Tool name + schema parity with Python** (`tools.go`): every tool of Python `_list_tools()` served with same property names, JSON types, required set. Python list = `mcp-server/vmaf-mcp/tool-contract.json`, written by `python3 -m vmaf_mcp.tool_contract --write`, kept current by `mcp-server/vmaf-mcp/tests/test_tool_contract.py`; never hand-edit it, never copy the list into Go. Served tool outside contract = must be in `goOnlyTools` (invariant 17). Enforced by `tool_contract_test.go::TestToolListMatchesPython`, `TestToolSchemasMatchPython`; `TestToolContractComparisonRefusesDrift` plants missing / extra / required / type drift. `.github/ci-impact.json` routes contract changes to Go checks.

19. **Every tool reaches server through `registerTools` grouped helpers** (`tools.go`). `registerTools` list of `registerXTools(srv)` calls; tool registrations live in helpers in original order. New tool must go into one (or new helper called by `registerTools`). A `register*` function nothing calls registers nothing. Invariant #1 parity test catches dropped Python or declared Go-only tool. Schema literals marshalled by `toolRegistrar.add`, not tool literal; schema failing marshal cannot reach registered tool: `add` retains error, registers nothing, skips later registrations; `registerTools` returns error, `buildServer` fails. Never substitute default schema for failed marshal: permissive `{"type":"object"}` accepts every argument map, tool stays reachable with declared contract silently disabled, parity tests miss because they compare registered tools only. `cmd/vmafx-mcp/tool_schema_test.go` pins outcome.
