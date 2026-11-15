---
paths:
  - cmd/vmafx-mcp/impl_grpc*.go
  - cmd/vmafx-mcp/tools.go
invariant: 5 control-plane gRPC tools Go-only in goOnlyTools; env-only targets; validateRemotePath guard.
---
<!-- markdownlint-disable MD013 MD029 -->
# Go-only control-plane gRPC bridge tools (ADR-1184)

## Rebase-sensitive invariants

17. **gRPC bridge deliberately Go-only** (ADR-1184). 5 control-plane tools (`submit_job`, `get_job`, `cancel_job`, `list_jobs`, `vmaf_score_remote`) have NO Python twin: Python server ships no gRPC stack and Phase-4b architecture names Go binary controller MCP client. Legal under invariant #1: listed in `goOnlyTools`; parity test fails when one is dropped or an undeclared tool appears. Do not delete tools; do not add `grpcio` to `mcp-server/vmaf-mcp` without superseding ADR-1184. Connection targets and credentials environment-only (`VMAFX_CONTROLLER_ADDR`, `VMAFX_SERVER_ADDR`, `VMAFX_CONTROLLER_TOKEN`, `VMAFX_GRPC_TIMEOUT`) — tool argument naming host makes MCP server SSRF pivot. `impl_grpc_test.go::TestGRPCTargetsComeFromEnv` pins this; `validateRemotePath` (shape-only: absolute, no `..`, no control characters) = ONLY guard on remote path arguments (`libvmaf.ValidatePath` cannot apply to file on worker node).
