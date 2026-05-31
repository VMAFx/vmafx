### controller: implement StreamJobs snapshot + add reaper stop signal (ADR-0962)

**B.3 — `StreamJobs` no-op fix**: `controllerServer.StreamJobs` previously
logged a line and returned `nil` without sending any messages, causing every
client to silently observe an empty stream regardless of queue state.  The
handler now calls `queue.ListAll` to obtain a point-in-time snapshot, applies
the optional proto `StatusFilter`, and streams each matching job before
closing.  `queue.Queue` gains a `ListAll(ctx, statuses)` method backed by a
parameterised SQLite query.

**B.4 — reaper goroutine stop signal**: `nodes.NewRegistry` spawned a
background reaper with no cancellation path (`reaper(_ ...context.Context)`
never received a context from its call site).  The goroutine looped on
`ticker.C` forever, leaking one goroutine per `NewRegistry` call in tests.
`NewRegistry` now accepts `ctx context.Context` and the reaper exits via
`select { case <-ctx.Done() }`.  `Registry.Close()` cancels the internal
context; `main.go` defers it after the shutdown context is set up.
