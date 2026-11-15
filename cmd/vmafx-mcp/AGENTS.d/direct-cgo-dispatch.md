---
paths:
  - cmd/vmafx-mcp/impl.go
  - cmd/vmafx-mcp/impl_direct*.go
  - pkg/libvmaf/*.go
invariant: Dispatch via directPathEnabled (exact "1"); one stderr marker; additive keys; 4 model arg forms.
---
<!-- markdownlint-disable MD013 MD029 -->
# Direct cgo scoring path and subprocess fallback (ADR-0931)

## Rebase-sensitive invariants

2. **Direct/subprocess dispatcher** (`impl.go`, `impl_direct.go`): tool handlers with both paths MUST dispatch via:
   `if directPathEnabled() { runFooDirect(...) } else { runFoo(...) }`.
   Direct variant responsible for fallback to subprocess variant on unhandled cases (GPU backends, `.onnx` models, unresolvable model versions in Phase 1). Keeps env gate safe globally. See ADR-0931 §"Fallback flag" and `docs/architecture/mcp-cgo-direct-migration.md`.

3. **`VMAFX_MCP_DIRECT` strict** (`impl_direct.go::directPathEnabled`): only exact string `"1"` enables direct path. `"true"`, `"yes"`, `"on"` treated as off. Phase 3 may relax; until then strict check asserted by `impl_direct_test.go::TestDirectPathEnabled`.

4. **Marker stream** (`pkg/libvmaf.LogDirectPathSelected`): "VMAFX_MCP_DIRECT=1 ... direct cgo scoring path" marker writes stderr exactly once per process. Operators rely on marker confirming path took effect. Routing through slog or stdout breaks convention.

5. **Response shape additions additive only**: direct path adds `backend_used = "cpu (direct cgo)"` and `frame_count` to subprocess JSON shape. Phase 2/3 populates `frames` and per-feature pooled scores. Existing keys (`pooled_metrics.vmaf.mean`, `backend_requested`, `mismatched_model_warning`) MUST remain identical to subprocess output.

6. **Model arg compatibility** (`impl_direct.go::resolveModelArgToPath`): accepts 4 MCP-level model forms (`version=NAME`, `path=ABS`, bare stem, abs/rel path). New forms require coordinated update to Python server resolver (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).
