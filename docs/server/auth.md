# vmafx-controller: Multi-Tenant Auth Gateway

> ADR: [ADR-0794](../adr/0794-controller-multi-tenant-auth-gateway.md)

The vmafx-controller supports multi-tenant deployments through a built-in
JWT auth gateway. Every gRPC and HTTP request (except liveness/readiness
probes) must carry a valid RS256 bearer token from a configured OIDC
provider. Requests are scoped to the tenant identified by the token, and
access to operations is governed by embedded role claims.

## Table of contents

- [Quick start](#quick-start)
- [Token structure](#token-structure)
- [OIDC provider configuration](#oidc-provider-configuration)
  - [Auth0](#auth0)
  - [Keycloak](#keycloak)
  - [Dex](#dex)
- [Roles and RBAC](#roles-and-rbac)
- [Tenant isolation](#tenant-isolation)
- [Helm configuration](#helm-configuration)
- [VmafxTenant CRD](#vmafxtenant-crd)
- [Disabling auth](#disabling-auth)
- [CLI flags and environment variables](#cli-flags-and-environment-variables)
- [Key rotation](#key-rotation)
- [Threat model summary](#threat-model-summary)

---

## Quick start

```bash
# Start the controller with auth enabled (Auth0 example):
vmafx-controller \
  --jwks-endpoint  https://YOUR_DOMAIN.auth0.com/.well-known/jwks.json \
  --auth-issuer    https://YOUR_DOMAIN.auth0.com/ \
  --auth-audience  https://vmafx.example.com/api
```

Call the API with a bearer token:

```bash
TOKEN=$(curl -s -X POST \
  https://YOUR_DOMAIN.auth0.com/oauth/token \
  -d grant_type=client_credentials \
  -d client_id=YOUR_CLIENT_ID \
  -d client_secret=YOUR_CLIENT_SECRET \
  -d audience=https://vmafx.example.com/api \
  | jq -r .access_token)

curl -H "Authorization: Bearer $TOKEN" \
  http://localhost:8080/v1/score \
  -d '{"reference":"/data/ref.yuv","distorted":"/data/dist.yuv"}'
```

---

## Token structure

The controller extracts the following claims from the JWT payload:

| Claim | Required | Default field | Description |
| --- | --- | --- | --- |
| `iss` | Yes | — | Must match `--auth-issuer`. |
| `exp` | Yes | — | Token expiry; checked on every request. |
| `aud` | No | — | Checked if `--auth-audience` is set. |
| `sub` | No | — | Subject (logged for audit). |
| `tid` | Yes* | `--auth-tenant-claim` | Tenant identifier. |
| `vmafx_roles` | No | `--auth-roles-claim` | List of role strings. |

*`tid` is required unless `--auth-disabled` is set.

Example payload:

```json
{
  "iss": "https://idp.example.com/",
  "sub": "user|abc123",
  "aud": "https://vmafx.example.com/api",
  "exp": 1893456000,
  "tid": "acme",
  "vmafx_roles": ["vmafx:writer"]
}
```

---

## OIDC provider configuration

The controller only needs the IdP's JWKS endpoint and issuer URL. It does
not perform OIDC discovery automatically — provide the endpoint directly.

### Auth0

```bash
--jwks-endpoint  https://YOUR_DOMAIN.auth0.com/.well-known/jwks.json
--auth-issuer    https://YOUR_DOMAIN.auth0.com/
--auth-audience  https://vmafx.example.com/api
--auth-tenant-claim org_id       # Auth0 organisation ID claim
```

In Auth0, add the `org_id` claim to your token and create a custom
`vmafx_roles` action in the Auth0 Login flow.

### Keycloak

```bash
--jwks-endpoint  https://keycloak.example.com/realms/vmafx/protocol/openid-connect/certs
--auth-issuer    https://keycloak.example.com/realms/vmafx
--auth-audience  vmafx-api
--auth-tenant-claim tid          # add as a custom mapper in Keycloak
--auth-roles-claim vmafx_roles   # add as a custom mapper in Keycloak
```

### Dex

```bash
--jwks-endpoint  https://dex.example.com/keys
--auth-issuer    https://dex.example.com
--auth-tenant-claim tid
```

---

## Roles and RBAC

Three roles are recognised. Include one or more in the `vmafx_roles` claim:

| Role | Permitted operations |
| --- | --- |
| `vmafx:reader` | `GetJob`, `StreamJobs`, health endpoints |
| `vmafx:writer` | All of reader + `SubmitJob`, `CancelJob`, `POST /v1/score` |
| `vmafx:admin` | All of writer + `RegisterNode`, `Heartbeat`, `PullWork`, `ReportResult` |

If the token carries no `vmafx_roles` claim (or the claim is empty) the
request is rejected with `403 Forbidden` for any operation that requires a
role.

---

## Tenant isolation

Every job is tagged with the `tenant_id` extracted from the submitter's
token at submission time. The controller enforces:

- `GetJob` / `CancelJob` — returns `PERMISSION_DENIED` if the caller's
  `tenant_id` does not match the job's stored tenant.
- `SubmitJob` — stamps the new job with the caller's `tenant_id`.
- `StreamJobs` — Phase 4b.2 will add tenant-scoped filtering.

Tenant IDs are opaque strings; the controller does not interpret them beyond
equality comparison.

---

## Helm configuration

```yaml
auth:
  enabled: true
  jwksEndpoint: https://idp.example.com/.well-known/jwks.json
  issuer: https://idp.example.com/
  audience: vmafx-api          # optional
  tenantClaim: tid             # default
  rolesClaim: vmafx_roles      # default

  tenants:
    - tenantId: acme
      oidc:
        issuer: https://acme.auth0.com/
        jwksEndpoint: https://acme.auth0.com/.well-known/jwks.json
        audience: vmafx-api
      rbac:
        defaultRole: vmafx:reader
        allowedRoles: [vmafx:reader, vmafx:writer]
```

The `auth.tenants` list creates `VmafxTenant` CRs in the same namespace.

---

## VmafxTenant CRD

Each tenant can be configured as a Kubernetes custom resource:

```yaml
apiVersion: vmafx.dev/v1
kind: VmafxTenant
metadata:
  name: acme
spec:
  tenantId: acme
  enabled: true
  oidc:
    issuer: https://acme.auth0.com/
    jwksEndpoint: https://acme.auth0.com/.well-known/jwks.json
    audience: vmafx-api
    tenantClaim: org_id
    rolesClaim: vmafx_roles
  rbac:
    defaultRole: vmafx:reader
    allowedRoles: [vmafx:reader, vmafx:writer]
```

`kubectl apply` VmafxTenant CRs directly for operator-managed multi-tenant
clusters. The CRD is installed by the Helm chart's `crds/` directory.

---

## Disabling auth

For internal deployments or integration-test pipelines:

```bash
vmafx-controller --auth-disabled
# or
VMAFX_AUTH_DISABLED=true vmafx-controller
```

When disabled, all requests are processed as tenant `dev` with role
`vmafx:admin`. Never use this in production.

---

## CLI flags and environment variables

| Flag | Env var | Default | Description |
| --- | --- | --- | --- |
| `--auth-disabled` | `VMAFX_AUTH_DISABLED` | `false` | Bypass all auth checks. |
| `--jwks-endpoint` | `VMAFX_JWKS_ENDPOINT` | — | JWKS endpoint URL. |
| `--auth-issuer` | `VMAFX_AUTH_ISSUER` | — | Expected `iss` claim value. |
| `--auth-audience` | `VMAFX_AUTH_AUDIENCE` | — | Expected `aud` claim value. |
| `--auth-tenant-claim` | `VMAFX_AUTH_TENANT_CLAIM` | `tid` | Tenant claim field name. |
| `--auth-roles-claim` | `VMAFX_AUTH_ROLES_CLAIM` | `vmafx_roles` | Roles claim field name. |

---

## Key rotation

When the controller receives a token whose `kid` (key ID) is not in the
local JWKS cache, it fetches the JWKS endpoint once. To prevent thundering-
herd on rotation, re-fetches are rate-limited to one per 30 seconds.

If the new key is not present in the endpoint's response within the cooldown
window, requests with the new `kid` are rejected with `401` until the cache
refreshes successfully.

---

## Threat model summary

| Threat | Mitigation |
| --- | --- |
| Algorithm confusion (`alg=none`, `alg=HS256`) | Only RS256 is accepted; any other `alg` header is rejected before key lookup. |
| Token replay | `exp` checked on every request. |
| Cross-tenant data access | `tenant_id` ownership enforced on every read/write/cancel. |
| JWKS endpoint spoofing | Endpoint configured by operator via trusted Helm/env values. |
| Privilege escalation | `allowedRoles` whitelist in VmafxTenant strips unexpected roles. |
| Revocation | Use short-lived tokens (≤1 hour); revocation list support is a follow-up. |
