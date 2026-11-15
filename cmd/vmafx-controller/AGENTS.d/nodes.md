---
paths:
  - cmd/vmafx-controller/nodes/*.go
  - cmd/vmafx-controller/main.go
invariant: StartDetached/Close reaper lifecycle; eviction requeues RUNNING jobs; tenant-bound sessions.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Node registry lifecycle and tenant sessions (`nodes/`)

## Invariants

### nodes package

1. **`nodes.NewRegistry` Start/Close lifecycle (ADR-1119)** (`nodes/registry.go`):
   `NewRegistry(log *slog.Logger)` takes no context, spawns no reaper. Reaper
   started by `StartDetached()` (fx `OnStart` in `provideNodeRegistry`),
   stopped by `Close()` (fx `OnStop`). Never `r.Start(ctx)` with the fx
   OnStart ctx: fx lets it expire after the start timeout (15 s) -> reaper
   stopped, silent nodes never evicted (`TestNodeRegistryReaperOutlivesStartContext`).
   Tests call `Close()` in `t.Cleanup`. `Close()` safe without `Start()`.
   Replaced eager `NewRegistry(ctx)` (ADR-0962).
2. **Eviction requeues** (`requeueEvictedNode` in `main.go`): registry
   eviction hook -> `queue.RequeueNode` returns the evicted node's RUNNING
   jobs to PENDING (FIFO front). Hook runs outside the registry lock; keep it
   (controller.proto promises the requeue).
3. **Sessions belong to one tenant (ADR-1522)**: `Register(name, tenantID,
   cap)` stores `Node.TenantID`; `ValidateSession` and `Heartbeat` compare
   token (constant time) and tenant (exact) in `sessionMatches`. Every session
   check takes caller tenant; no tenant-less variant.
