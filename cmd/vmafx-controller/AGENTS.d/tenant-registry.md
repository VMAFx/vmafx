---
paths:
  - cmd/vmafx-controller/auth/middleware*.go
  - cmd/vmafx-controller/auth/tenants*.go
  - cmd/vmafx-controller/tenants/*.go
  - cmd/vmafx-controller/tenant_config*.go
invariant: Single verifyBearer path; strict startup Load, lenient Reload; generated VmafxTenantSpec.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Tenant registry and JWKS verification (ADR-1519)

## Invariants

### tenant registry (ADR-1519)

1. **One verification path** (`auth/middleware.go::verifyBearer`): HTTP and
   gRPC both call `verifyBearer`; registry mode -> `TenantRegistry.Resolve`,
   else global provider. Never add transport-specific token checks.
2. **Resolve contract** (`auth/tenants.go`): issuer picks candidates; each
   verifies with own JWKS/issuer/audience; match only when own `tenantClaim`
   value == own `tenantId`; exactly one match. Suspended -> PermissionDenied
   (403); stale set (age > `StaleFactor` x refresh) -> Unavailable (503).
   One snapshot (`atomic.Pointer`) per request; never reload mid-decision.
3. **Startup strict, refresh lenient**: `Load` all-or-nothing (controller
   start fails); `Reload` drops only invalid tenants and every copy of
   duplicate `tenantId`. Undecodable resource = entry with `Err`, refused by
   registry, never load failure. Mode exclusivity in `Config.validateMode`:
   registry excludes disabled and all five global provider settings.
4. **JWKS cache** (`auth/middleware.go`): keys used `jwksKeyMaxAge` (15 min)
   then refetched; on fetch failure kept until `jwksKeyHardMaxAge` (24 h);
   every fetch attempt sets cooldown; https->http redirect refused
   (`refuseDowngradeRedirect`). Registry: one signature check per cache
   (`matchTenant` + `verifiedOnce`), caches pruned in `publish`. gRPC
   Unauthenticated message fixed (`grpcAuthError`); reasons only logged.
5. **Sources** (`tenants/`): `FileSource` (YAML/JSON docs, lists) and
   `KubernetesSource` (dynamic client, namespace-scoped list, timeout).
   Strict spec decode (`DisallowUnknownFields`). `Refresher` Start/Close like
   `nodes.Registry`. Spec type = generated `vmafxv1.VmafxTenantSpec`
   (`api/vmafx/v1`, ADR-2350 D13); `auth/tenants.go` aliases `TenantSpec`,
   `TenantOIDC`, `TenantRBAC`, `TenantScoring` to it. New field ->
   `api/vmafx-platform.toml`, regenerate; never re-declare struct here. Guard:
   `auth/tenants_generated_type_test.go`.
6. **Guards**: `auth/tenants_internal_test.go` (once-per-cache, key age,
   redirect, prune), `auth/tenants_test.go` (mutation-checked: tenant-claim match,
   suspension, allowedRoles, staleness, defaultRole-in-allowed),
   `tenants/source_test.go`, `tenant_config_test.go`
   (`TestMisconfiguredTenantStopsStartup`, over-wire enforcement + reload).
