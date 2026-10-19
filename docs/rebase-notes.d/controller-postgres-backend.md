## vmafx-controller job backends (2026-10-07)

`rc4/api-wp17-controller`, [ADR-2350](adr/2350-cloud-native-platform.md). The
gRPC handlers talk to `cmd/vmafx-controller/backend` (`Backend`), never to
`queue`, `nodes` or `scheduler` directly; the SQLite queue sits behind
`backend.Legacy` and the PostgreSQL store behind `backend.Postgres`. The
store's generated `pgdb/` takes either side on a conflict and is regenerated
with `python3 scripts/codegen/sqlc_generate.py --write`. A sync that touches
`grpc_server.go` keeps the handlers on the interface and the tenant contract
(another tenant's job is `PERMISSION_DENIED`, an unknown one `NOT_FOUND`) on
both backends: `replicas_test.go` and `grpc_tenant_test.go` guard it. no
upstream file.
