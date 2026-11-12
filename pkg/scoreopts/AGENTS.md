<!-- markdownlint-disable MD013 -->
# AGENTS.md — pkg/scoreopts

Generated scoring option schemas and `vmaf` CLI argv builder (ADR-1852, ADR-2044). Parent: [../../AGENTS.md](../../AGENTS.md).

## Rebase-sensitive invariants

1. `options.gen.json` = generated from `core/api/vmafx.toml` via `python3 scripts/codegen/vmafx-api.py --write` (byte-identical to `mcp-server/vmaf-mcp/src/vmaf_mcp/options.gen.json`). Never hand-edit `options.gen.json` or reintroduce hand-written flag lists.
2. Go↔Python MCP scoring schema/argv parity, `%.6f` legacy precision default, 64x64 backend probe, and `backend_used` receipt rules: read [scoring-and-probe-parity](../../cmd/vmafx-mcp/AGENTS.d/scoring-and-probe-parity.md).
