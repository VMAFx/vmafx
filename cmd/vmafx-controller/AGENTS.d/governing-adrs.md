---
paths:
  - cmd/vmafx-controller/**/*.go
invariant: Follow ADR-0711, ADR-1119, ADR-1518/1519/1522/1563, ADR-2350 controller contracts.
---
<!-- markdownlint-disable MD013 MD060 -->
# Governing ADRs (`cmd/vmafx-controller`)

Per-package invariants for subtree.

## Governing ADRs

| ADR | Title | Scope |
|-----|-------|-------|
| [ADR-0711](../../../docs/adr/0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 | Go service: gRPC + HTTP, queue, node registry, FIFO scheduler |
| [ADR-0961](../../../docs/adr/0961-queue-pullwork-rollback-on-get-failure.md) | PullWork rollback on post-update Get failure | queue package correctness |
| [ADR-0962](../../../docs/adr/0962-controller-streamjobs-and-reaper-stop.md) | StreamJobs snapshot + reaper stop signal | controller / queue / nodes correctness |
| [ADR-1119](../../../docs/adr/1119-golusoris-go-framework-adoption.md) | golusoris fx framework adoption | composition root, env contract, lifecycle ordering, auth injection |
| [ADR-2350](../../../docs/adr/2350-cloud-native-platform.md) | cloud-native platform (replaces ADR-1119 queue decision) | Postgres store, leased node claims, River, stateless replicas, generated proto |
| [ADR-1518](../../../docs/adr/1518-controller-grpc-authorization.md) | gRPC authorisation: per-method role table, deny by default | auth interceptors, `grpc_roles.go` |
| [ADR-1522](../../../docs/adr/1522-controller-tenant-scoped-reads.md) | tenant-scoped job reads, tenant-bound node sessions | queue, nodes, scheduler, gRPC handlers |
| [ADR-1519](../../../docs/adr/1519-controller-tenant-registry.md) | tenant registry from VmafxTenant resources or file | `auth/tenants.go`, `tenants/`, `tenant_config.go` |
| [ADR-1563](../../../docs/adr/1563-controller-node-role.md) | `vmafx:node` only role on node API, nothing else | `grpc_roles.go`, `auth/policy.go`, `auth/tenants.go` |
