---
paths:
  - cmd/vmafx-controller/grpc_*.go
  - cmd/vmafx-controller/auth/policy*.go
  - cmd/vmafx-controller/auth/grpc_interceptor*.go
  - cmd/vmafx-controller/auth/authtest/*.go
invariant: Deny-by-default controllerMethodRoles; node API = vmafx:node only; callerTenant read once.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# gRPC server and per-RPC authorisation (ADR-1518, ADR-1563)

## Invariants

### grpc server

1. **`protoStatusToQueue` / `queueStatusToProto` sync (ADR-0962)** (`grpc_server.go`):
   inverse helpers. New `Job.Status` requires updating both plus `queue.Status*`.
2. **`grpc_server_test.go` mock stream (ADR-0962)**: `mockStreamJobsServer`
   implements `grpc.ServerStream` (6 methods inline).
3. **Per-RPC role policy (ADR-1518)** (`grpc_roles.go`, `auth/policy.go`):
   `controllerMethodRoles()` names roles for every served method; auth
   interceptors authenticate then authorise in one function (`admitGRPC`).
   Unlisted method = refused for every caller, disabled-mode admin included.
   New RPC -> add entry in same PR; `TestEveryServedRPCHasARolePolicy` fails
   otherwise. Never chain separate role interceptor; never move role checks
   into handlers. `TestGRPCRolesEnforcedPerRPC` holds independent
   expectation table: change only together with ADR-0794/ADR-1518 role table.
   Node API = `auth.RoleNode` (`vmafx:node`) only, in no other entry; admin
   never on node API (ADR-1563). `devClaims` holds admin + node (disabled-mode
   nodes register). `tenantRoles` refuses `defaultRole: vmafx:node`; CRD
   offers node in `allowedRoles` only. Never re-add admin to node API.
4. **`auth/authtest` test-only**: RS256 issuer + JWKS server for tests.
   Import from `_test.go` files only.
5. **Tenant from context, once (ADR-1522)**: handlers read tenant via
   `callerTenant(ctx)` once, hand value down (queue query, registry, scheduler).
   Empty tenant -> `Unauthenticated`. Ownership refusals name no tenant
   (`AssertTenantOwns`).
