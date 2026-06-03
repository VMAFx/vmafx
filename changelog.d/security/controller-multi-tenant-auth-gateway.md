## feat(controller/auth): multi-tenant JWT auth gateway (ADR-0794)

The vmafx-controller HTTP and gRPC endpoints are now protected by a
built-in JWT authentication and authorisation gateway.

### What changed

- **New package** `cmd/vmafx-controller/auth/` — RS256 JWT verification,
  JWKS key caching with automatic refresh on key rotation, tenant isolation
  via a configurable JWT claim (`tid` by default), and three-tier RBAC
  (`vmafx:reader` / `vmafx:writer` / `vmafx:admin`).
- **HTTP middleware** wraps all `/v1/*` endpoints; `/healthz`, `/readyz`,
  and `/metrics` are exempted.
- **gRPC interceptors** enforce the same auth model on `VmafxScoring` and
  `VmafxController` services via `authorization` metadata.
- **Tenant isolation** — `SubmitJob` stamps jobs with the caller's
  `tenant_id`; `GetJob` and `CancelJob` enforce ownership.
- **SQLite schema** — `jobs` table gains `tenant_id TEXT` column + index.
- **VmafxTenant CRD** (`vmafx.dev/v1`) for per-tenant OIDC + RBAC config.
- **Helm** — new `auth:` values block; `auth.tenants[]` renders
  `VmafxTenant` CRs; auth env vars injected into the Deployment.
- **Disabled mode** — `--auth-disabled` / `VMAFX_AUTH_DISABLED=true` for
  internal/dev deployments (injects synthetic `dev:admin` identity).

### Upgrading

Set `auth.enabled: false` (the default) to preserve existing behaviour.
Enable auth by providing `auth.jwksEndpoint` and `auth.issuer` in
`values.yaml` or via `--jwks-endpoint` / `--auth-issuer` CLI flags.

See `docs/server/auth.md` for the full configuration guide.
