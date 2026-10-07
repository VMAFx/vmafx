<!-- markdownlint-disable MD013 -->
# AGENTS.md — internal/app/scoringservice

Shared application wiring for `vmafx-server` and `vmafx-controller`.

## Rebase-sensitive invariants

1. Prometheus registry construction, libvmaf scorer construction/lifecycle, legacy `/healthz` and `/readyz` behavior, JSON response writing live here once. Binary packages may keep thin adapters for fx signatures, private state; must not re-grow implementations.
2. Probe response bodies: compatibility bytes (no trailing newline, exact status strings, JSON content type, GET-only behavior, request counters).
3. `WriteJSON` keeps `SetEscapeHTML(false)` and encoder trailing newline. Encoding/socket failures logged; never discard returned error.
4. `ProvideScorer` constructed before network servers: fx reverse stop order drains gRPC before calling `Scorer.Close`.
5. ScoreStream session metrics = `StreamMetrics` (`stream_metrics.go`), shared by `vmafx-server` and `vmafx-node` handlers: `Begin` at handler entry, `defer End(retErr)` on named return, `Frame` per pushed frame pair. Outcome: nil completed; cancelled/expired ctx or `codes.Canceled`/`DeadlineExceeded` cancelled; else failed. nil `*StreamMetrics` records nothing (tests). Never count sessions in handler directly. Contract guards: `cmd/vmafx-server/metrics_contract_test.go`, `cmd/vmafx-node/metrics_test.go`.

## Test requirements

```bash
CGO_ENABLED=0 go test ./internal/app/scoringservice/
CGO_LDFLAGS="-L$PWD/core/build-cpu/src -lvmaf -lm" \
  LD_LIBRARY_PATH=$PWD/core/build-cpu/src \
  go test ./cmd/vmafx-server/ ./cmd/vmafx-controller/
```
