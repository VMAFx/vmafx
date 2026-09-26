# AGENTS.md — internal/app/scoringservice

Shared application wiring for `vmafx-server` and `vmafx-controller`.

## Rebase-sensitive invariants

1. Prometheus registry construction, libvmaf scorer construction/lifecycle,
   legacy `/healthz` and `/readyz` behavior, and JSON response writing live
   here once. Binary packages may keep thin adapters for fx signatures and
   private state, but must not re-grow implementations.
2. Probe response bodies are compatibility bytes: no trailing newline, exact
   status strings, JSON content type, GET-only behavior, and request counters.
3. `WriteJSON` keeps `SetEscapeHTML(false)` and the encoder's trailing newline.
   Encoding/socket failures must be logged; never discard the returned error.
4. `ProvideScorer` must be constructed before network servers so fx's reverse
   stop order drains gRPC before calling `Scorer.Close`.

## Test requirements

```bash
CGO_ENABLED=0 go test ./internal/app/scoringservice/
CGO_LDFLAGS="-L$PWD/core/build-cpu/src -lvmaf -lm" \
  LD_LIBRARY_PATH=$PWD/core/build-cpu/src \
  go test ./cmd/vmafx-server/ ./cmd/vmafx-controller/
```
