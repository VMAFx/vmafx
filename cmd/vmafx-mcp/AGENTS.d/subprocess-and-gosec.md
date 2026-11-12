---
paths:
  - cmd/vmafx-mcp/impl.go
  - cmd/vmafx-mcp/impl_gosec_test.go
  - cmd/vmafx-mcp/impl_handlers_test.go
invariant: Validate paths/binaries for gosec G204/G304; >=1 error test per handler; exec.CommandContext.
---
<!-- markdownlint-disable MD013 MD029 -->
# Subprocess context propagation, gosec path gates and handler tests

## Rebase-sensitive invariants

7. **gosec G304 / G204 contract** (`os.ReadFile` / `os.Open` / `exec.Command*` in `impl.go`): path or command variable consumed MUST either (a) round-trip through `libvmaf.ValidatePath` (caller-supplied paths), (b) originate from `os.CreateTemp` / `os.MkdirTemp` (locally-generated temp paths), or (c) come from `libvmaf.FindBinary` / `findVmafTune` / `exec.LookPath` fixed binary name. Subprocess site or file read without gate = attacker-influenced path; add validation or annotate `// #nosec G204` / `// #nosec G304` with citation naming protecting helper. CI gate (`gosec -exclude-generated` in `go-ci.yml`) blocks merge. `cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal` pins `describeModel` allowlist; equivalent regressions belong next to it. See [ADR-0983](../../../docs/adr/0983-gosec-findings-fix-sweep.md).

8. **Handler test coverage** (`impl_handlers_test.go`): covers "binary not found" fast-fail branches, `findVmafTune` env-override, `parseArgs` nil/valid/invalid, ambiguous model-stem path in `describeModel`, and filesystem walks for `handleListModels` and `handleListExtractors`. When adding handler: add >=1 error-path test not requiring external binary (`t.Setenv("VMAF_BIN", "/nonexistent/...")`). Statement coverage target: 50 %+ on `cmd/vmafx-mcp` package.

9. **Context propagation invariant** (ADR-1085): subprocesses launched from tool handler MUST use `exec.CommandContext(ctx, ...)` where `ctx` comes from MCP dispatch layer. `exec.Command(...)` without context forbidden: leaves orphan processes when MCP client disconnects. Only exception `probeBackends` (uses fresh `context.WithTimeout` for module-level background cache fill, not per-request call). `runVmafScore`, `delegateToPythonEval`, and `runVmafScoreDirect` carry `ctx context.Context` parameter; new subprocess functions must follow same pattern.
