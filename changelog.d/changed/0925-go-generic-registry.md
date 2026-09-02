- Introduce `pkg/registry.Store[K, V]` — generic in-memory keyed store with
  RWMutex concurrency, snapshot-copy reads, predicate-bulk eviction, and an
  `Update` / `Read` callback API for write-locked composition.  Replaces the
  hand-rolled mutex + map + snapshot plumbing in
  `cmd/vmafx-controller/nodes/Registry` (ADR-0925); the SQLite-backed
  `cmd/vmafx-controller/queue/Queue` intentionally stays as-is (different
  storage paradigm — FIFO + transactional pull-and-claim).  Also folds
  `pkg/observability.SetControllerSources`'s former `nodeRegistrySource`
  narrow interface into the new `registry.Counter` constraint
  (`Count() int`); the queue-specific `jobQueueSource` narrow interface
  stays because its `PendingCount` / `RunningCount` partition by terminal
  status, not by raw cardinality.
