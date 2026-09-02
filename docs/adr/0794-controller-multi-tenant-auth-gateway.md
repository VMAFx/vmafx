# ADR-0794: Multi-Tenant Auth Gateway for vmafx-controller

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `security`, `controller`, `auth`, `multi-tenant`, `oidc`, `grpc`

## Context

The vmafx-controller exposes a gRPC service (`:50051`) and an HTTP service
(`:8080`) with no authentication or authorisation. In single-operator
on-premises deployments this was acceptable, but the cloud-native Phase 4b
roadmap targets multi-tenant SaaS deployments where different organisations
submit scoring jobs to a shared controller cluster.

Without auth the controller has no concept of job ownership, so one tenant
can read or cancel another tenant's jobs, and there is no mechanism to
restrict dangerous operations (node registration, result reporting) to
trusted callers.

The decision was requested: add JWT bearer-token auth with RS256/JWKS,
tenant isolation, and RBAC to the controller, wired through the existing
HTTP and gRPC stacks, with a Helm values block and CRD for tenant
configuration.

Constraints:

- Must support generic OIDC providers (Auth0, Keycloak, Dex).
- Must not require a sidecar proxy (no Envoy / Istio dependency).
- Must be bypassable for internal/dev deployments (`--auth-disabled`).
- Must preserve the existing unauthenticated probe paths (`/healthz`,
  `/readyz`, `/metrics`).
- Key rotation must be handled transparently (JWKS refresh on unknown kid).

## Decision

We will implement a self-contained `cmd/vmafx-controller/auth` Go package
that provides:

1. **JWT verification** — RS256 only; tokens with any other algorithm header
   are rejected before key lookup (algorithm confusion attack prevention).
2. **JWKS key cache** — fetches keys from the IdP's JWKS endpoint; refreshes
   on unknown `kid` with a 30-second rate-limit cooldown to prevent
   thundering-herd on key rotation.
3. **Tenant isolation** — every verified token must carry a configurable
   `tid` claim (default). The claim value is stored in the request context
   and all job operations (SubmitJob, GetJob, CancelJob) scope their
   database queries and ownership checks to that tenant.
4. **RBAC** — three roles extracted from the `vmafx_roles` JWT claim:
   `vmafx:reader` (read-only), `vmafx:writer` (submit/cancel),
   `vmafx:admin` (all operations including node management).
5. **HTTP middleware** — `Middleware.HTTPHandler` wraps the HTTP mux with
   Bearer extraction and claim injection; `RequireRole` wraps individual
   handlers for role enforcement.
6. **gRPC interceptors** — `GRPCUnaryInterceptor` and `GRPCStreamInterceptor`
   extract tokens from gRPC metadata key `authorization` and inject claims
   into the context identically to the HTTP path.
7. **VmafxTenant CRD** — a Kubernetes custom resource for per-tenant OIDC
   and RBAC configuration; rendered as CRs by the Helm chart.
8. **Schema migration** — the SQLite jobs table gains a `tenant_id TEXT`
   column indexed for tenant-scoped queries; existing rows default to `''`.
9. **Disabled mode** — `--auth-disabled` / `VMAFX_AUTH_DISABLED=true` injects
   a synthetic `dev` tenant with `vmafx:admin` role, enabling unmodified
   existing deployments and integration tests.

The auth package has no external runtime dependencies beyond the Go standard
library and `google.golang.org/grpc` (already in go.mod). It does not pull
in a JWT library — RS256 verification is implemented directly over
`crypto/rsa` and `crypto/sha256`, keeping the TCB minimal.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Envoy sidecar proxy (ext_authz) | No code in the controller; fully decoupled; industry pattern | Adds Envoy + control-plane dependency; increases operational complexity; not available in bare-metal/laptop dev setups | Too heavy for the fork's deployment range |
| OPA (Open Policy Agent) sidecar | Rich policy language; audit log | Same sidecar complexity as Envoy; OPA learning curve | Over-engineered for three roles |
| Third-party JWT library (golang-jwt/jwt) | Less code; standard claims helpers | Adds external dependency; algorithm-agnostic defaults require careful configuration to avoid alg=none or HS256 attacks | Dependency hygiene + attack surface reduction |
| mTLS only (no JWT) | Strong identity; no token expiry | Cannot encode tenant_id or roles without a custom CA per tenant; no standard OIDC integration | Tenant claim binding requires JWT or equivalent token |
| No auth, network-policy only | Zero code | Requires Kubernetes NetworkPolicy + no internet exposure; rules out SaaS/shared-cluster deployments | Does not meet the multi-tenant SaaS requirement |

## Consequences

- **Positive**:
  - Any OIDC-compliant IdP works out of the box (Auth0, Keycloak, Dex,
    Google, Azure AD, Okta).
  - Tenant isolation is enforced at the application layer, independent of
    network topology.
  - RBAC is token-embedded, requiring no database round-trip per request.
  - Key rotation is transparent to operators.
  - Disabled mode preserves full backward compatibility.
- **Negative**:
  - RS256 key verification adds ~0.3 ms per request (single SHA-256 + modexp
    on 2048-bit key); negligible vs. scoring latency.
  - JWKS cache refresh adds a network round-trip on first request after key
    rotation; the 30-second cooldown bounds the blast radius.
  - The SQLite `tenant_id` column is added non-destructively but existing
    rows have `tenant_id = ''`; operators upgrading live databases should be
    aware that pre-auth jobs are owned by the empty-string tenant.
- **Neutral / follow-ups**:
  - Phase 4b.2 (StreamJobs push model) must apply the same tenant filter to
    the stream subscription.
  - A VmafxTenant operator reconciler (reading the CRD and updating the
    controller's runtime config) is a follow-up item; the CRD is inert
    until that reconciler is implemented.
  - Token revocation is not supported (stateless JWT model); short-lived
    tokens (≤1 hour) and JWKS rotation are the primary mitigations.
  - Audit logging (who did what, for which tenant) is a follow-up.

## Threat model

| Threat | Mitigation |
| --- | --- |
| Algorithm confusion (alg=none, alg=HS256) | Reject any token whose header `alg` ≠ `RS256` before key lookup |
| Token replay | JWT `exp` checked on every request; clock skew tolerance is zero |
| Cross-tenant data access | `AssertTenantOwns` enforced on every read/write/cancel handler |
| JWKS endpoint spoofing | Operator configures endpoint via trusted Helm values / env vars |
| Key rotation DoS | 30-second refresh cooldown prevents hammering the IdP on rotation |
| Privilege escalation via roles claim | `allowedRoles` whitelist in VmafxTenant strips unexpected roles |
| Unauthenticated probe scraping | `/healthz`, `/readyz`, `/metrics` are exempted (no sensitive data) |

## References

- req: "Build a multi-tenant auth gateway for vmafx-controller. Currently
  the controller exposes gRPC + HTTP without auth."
- [ADR-0711](0711-vmafx-controller-phase4b1.md): vmafx-controller Phase
  4b.1 scope expansion.
- [ADR-0703](0703-vmafx-server-grpc-http.md): vmafx-server Go gRPC + HTTP service.
- RFC 7517: JSON Web Key (JWK).
- RFC 7519: JSON Web Token (JWT).
- RFC 8414: OAuth 2.0 Authorization Server Metadata (OIDC discovery).
