---
paths:
  - internal/app/scoringservice/*.go
  - cmd/vmafx-server/*.go
  - cmd/vmafx-controller/*.go
invariant: Server and controller share scoring-service plumbing in internal/app/scoringservice.
---
<!-- markdownlint-disable MD013 MD029 -->
# Shared scoring-service plumbing (`internal/app/scoringservice`)

## Rebase-sensitive invariants

7. **Server/controller scoring-service plumbing has one owner.** Prometheus
   registry construction, libvmaf scorer lifecycle, legacy health/readiness
   probes, and JSON response writing live in
   `internal/app/scoringservice`. Binary-local methods are adapters only; do
   not restore parallel implementations. The shared tests pin exact probe
   bytes and fail-loud response-write logging.
