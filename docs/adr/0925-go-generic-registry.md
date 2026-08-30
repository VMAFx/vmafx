<!-- markdownlint-disable MD060 -->
# ADR-0925: Generic in-memory registry for vmafx-controller subsystems

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `go`, `controller`, `refactoring`, `observability`

## Context

`cmd/vmafx-controller/nodes/registry.go` and `pkg/observability/observability.go`
both carried boilerplate that Go generics (available since 1.18, the fork
targets 1.25 per `go.mod`) can collapse:

1. The node registry hand-rolled the same `sync.RWMutex` + `map[string]*Node`
   napshot-copy + predicate-eviction pattern that any keyed in-memory
   store needs.  The reaper goroutine, the `Get` / `All` / `Count` /
   `Heartbeat` helpers, and the shallow-copy guards were all generic in
   shape; only the `SessionToken` validation and heartbeat-deadline
   semantics were node-specific.
2. `pkg/observability.SetControllerSources` accepted two single-method
   narrow interfaces (`jobQueueSource` and `nodeRegistrySource`) to wire
   Prometheus `GaugeFunc` instruments.  `nodeRegistrySource` was a literal
   `Count() int` — exactly the shape any registry-style subsystem
   exposes.

The job queue (`cmd/vmafx-controller/queue/queue.go`) at first glance
looks like another `Add/Get/List/Delete` consumer, but its backing store
is SQLite (`modernc.org/sqlite`) with FIFO + transactional pull-and-claim
semantics that the generic in-memory store cannot serve without forcing
a second storage paradigm through one interface.

## Decision

Introduce a generic `pkg/registry.Store[K comparable, V any]` that
encapsulates the keyed-map + RWMutex + snapshot-copy + predicate-eviction
pattern, plus a `registry.Counter` constraint (`Count() int`) for
observability wiring.  Refactor `nodes.Registry` to compose
`*Store[string, Node]`; refactor `observability.SetControllerSources` to
accept `registry.Counter` for the node-count gauge.  Leave the SQLite job
queue as-is — its semantics are not a generic-Store match.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Force `queue.Queue` + `nodes.Registry` behind one `Registry[T Identifiable]` interface | Symmetric API; fewer top-level types | Queue is SQLite-backed (FIFO + transactional pull-and-claim); registry is in-memory. A shared interface would either expose the lowest common denominator (losing queue capabilities) or carry mostly-empty methods on the registry side. | Over-abstraction; semantics differ too much. |
| Keep the duplication; "it works" | Zero churn | Two separate hand-rolled mutex/map patterns to maintain; two near-identical narrow interfaces in `pkg/observability`. | Misses the modernization win the audit (#15) flagged. |
| Move the generic store under `cmd/vmafx-controller/internal/registry/` | Tighter scope; not promised as a public package | The `Counter` constraint needs to be importable from `pkg/observability/` without a back-edge into `cmd/vmafx-controller/`. | `pkg/` placement keeps the import DAG clean. |
| Use a third-party generic-cache library (e.g. `puzpuzpuz/xsync`) | Battle-tested concurrency | Adds a runtime dep for ~200 LOC of std-lib code; no production caching / eviction needs beyond the heartbeat reaper. | Not worth a dependency. |

## Consequences

- **Positive**:
  - `nodes/registry.go` shrinks from ~190 LOC (hand-rolled mutex + map +
    snapshot copies) to ~145 LOC of domain-specific logic
    (heartbeat / session / capability handling); the generic plumbing
    lives in one tested package (`pkg/registry/`, ~200 LOC + tests).
  - `pkg/observability.SetControllerSources` collapses one of its two
    narrow interfaces into the reusable `registry.Counter`.  Future
    controller subsystems exposing `Count()` wire into the same gauge
    mechanism without a new narrow interface.
  - Race-free snapshot semantics are enforced by the `Cloner[V]` callback
    in one place rather than ad-hoc `cp := *n` lines across consumers.
- **Negative**:
  - One new top-level package (`pkg/registry/`) to maintain.
  - Mild cognitive load: contributors need to know that mutating callbacks
    (`Update`, `EvictWhere`, `Read`) run under the Store's lock and must
    not re-enter the Store (deadlock).  Documented inline.
- **Neutral / follow-ups**:
  - `queue.PendingCount` / `queue.RunningCount` retain a dedicated narrow
    interface (`jobQueueSource` in `pkg/observability`) because the
    terminal-status partitioning is queue-specific.  If the queue is ever
    re-implemented atop an in-memory store, the narrow interface can fold
    into `registry.Counter` then.
  - No public C-API or CLI surface affected; no rebase impact
    (fork-only Go files).

## References

- Source: VMAFX modernization audit item #15 ("collapse
  `cmd/vmafx-controller/queue/` + `nodes/` boilerplate with generics"),
  `req`.
- Related: ADR-0711 (vmafx-controller Phase 4b.1 scope expansion),
  ADR-0703 (vmafx-server Go gRPC + HTTP service origin).
- Touched files:
  - `pkg/registry/registry.go` (new)
  - `pkg/registry/registry_test.go` (new)
  - `cmd/vmafx-controller/nodes/registry.go` (refactor)
  - `pkg/observability/observability.go` (narrow-interface collapse)
