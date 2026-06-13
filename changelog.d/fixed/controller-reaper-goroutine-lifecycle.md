- vmafx-controller: fixed a goroutine leak in
  `cmd/vmafx-controller/nodes.Registry` — the heartbeat reaper was started by
  `NewRegistry()` but had no shutdown path, so the goroutine ran for the
  lifetime of the binary even when the registry was no longer needed. Added an
  idempotent `Close() error` method that signals the reaper via a `done`
  channel and waits for it through a `sync.WaitGroup`; wired
  `defer nodeRegistry.Close()` into `cmd/vmafx-controller/main.go` so the
  reaper is torn down on shutdown. New unit tests bound `Close()` to ≤ 2 s and
  exercise concurrent close paths under `-race`.
