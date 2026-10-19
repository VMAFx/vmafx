# vmafx-controller: Multi-Tenant Auth Gateway

> ADRs: [ADR-0794](../adr/0794-controller-multi-tenant-auth-gateway.md)
> (gateway), [ADR-1518](../adr/1518-controller-grpc-authorization.md) (roles),
> [ADR-1522](../adr/1522-controller-tenant-scoped-reads.md) (tenant scoping),
> [ADR-1519](../adr/1519-controller-tenant-registry.md) (tenant registry)

The vmafx-controller supports multi-tenant deployments through a built-in
JWT auth gateway. You configure it with `VMAFX_*` environment variables (the
controller has no CLI flags beyond `--version`, since ADR-1119) and send a
bearer token on every request.

The controller verifies tokens in one of two ways:

- **One identity provider** for every tenant: the controller trusts
  `VMAFX_JWKS_ENDPOINT` and `VMAFX_AUTH_ISSUER` and takes the tenant from
  each token's tenant claim. Any tenant name the provider issues is accepted.
  The [quick start](#quick-start) uses this mode.
- **A tenant registry**: the controller reads `VmafxTenant` resources (from
  the Kubernetes API or a file) and accepts only those tenants, each with its
  own identity provider, suspension switch and role whitelist. See [Tenant
  registry](#tenant-registry).

Every gRPC and HTTP request, except the liveness, readiness and metrics
endpoints, must carry a valid RS256 bearer token from a configured OIDC
provider. Requests are scoped to the tenant identified by the token, and every
gRPC call and the HTTP `POST /v1/score` endpoint require a role from the token;
see [Roles and RBAC](#roles-and-rbac).

## Table of contents

- [Quick start](#quick-start)
- [Token structure](#token-structure)
- [OIDC provider configuration](#oidc-provider-configuration)
  - [Auth0](#auth0)
  - [Keycloak](#keycloak)
  - [Dex](#dex)
- [Roles and RBAC](#roles-and-rbac)
- [Tenant isolation](#tenant-isolation)
- [Tenant registry](#tenant-registry)
- [Helm configuration](#helm-configuration)
- [VmafxTenant CRD](#vmafxtenant-crd)
- [Disabling auth](#disabling-auth)
- [Environment variables](#environment-variables)
- [Key rotation](#key-rotation)
- [Threat model summary](#threat-model-summary)

---

## Quick start

1. Start the controller with auth enabled (Auth0 example).

    ```bash
    VMAFX_JWKS_ENDPOINT=https://YOUR_DOMAIN.auth0.com/.well-known/jwks.json \
    VMAFX_AUTH_ISSUER=https://YOUR_DOMAIN.auth0.com/ \
    VMAFX_AUTH_AUDIENCE=https://vmafx.example.com/api \
      vmafx-controller
    ```

2. Fetch a token from the identity provider.

    ```bash
    TOKEN=$(curl -s -X POST \
      https://YOUR_DOMAIN.auth0.com/oauth/token \
      -d grant_type=client_credentials \
      -d client_id=YOUR_CLIENT_ID \
      -d client_secret=YOUR_CLIENT_SECRET \
      -d audience=https://vmafx.example.com/api \
      | jq -r .access_token)
    ```

3. Call the API with the bearer token.

    ```bash
    curl -H "Authorization: Bearer $TOKEN" \
      http://localhost:8080/v1/score \
      -d '{"reference":"/data/ref.yuv","distorted":"/data/dist.yuv"}'
    ```

---

## Token structure

The controller extracts the following claims from the JWT payload:

| Claim | Required | Default field | Description |
| --- | --- | --- | --- |
| `iss` | Yes | — | Must match `VMAFX_AUTH_ISSUER`. |
| `exp` | Yes | — | Token expiry; checked on every request. |
| `aud` | No | — | Checked if `VMAFX_AUTH_AUDIENCE` is set. |
| `sub` | No | — | Subject (logged for audit). |
| `tid` | Yes* | `VMAFX_AUTH_TENANT_CLAIM` | Tenant identifier. |
| `vmafx_roles` | No | `VMAFX_AUTH_ROLES_CLAIM` | List of role strings. |

*`tid` is required unless `VMAFX_AUTH_DISABLED=true` is set.

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
not perform OIDC discovery automatically; provide the endpoint directly.

Rules for the signing keys in the JWKS:

- Keys must be RSA keys of at least 2048 bits with an odd public exponent
  (normally 65537).
- A key below 2048 bits is skipped and logged as
  `jwks: skipping RSA key below the minimum size` with its `kid`. Tokens
  signed with that key are rejected with `401`; other keys in the same JWKS
  keep working.
- A key with a malformed exponent makes the JWKS refresh fail.

### Auth0

```bash
VMAFX_JWKS_ENDPOINT=https://YOUR_DOMAIN.auth0.com/.well-known/jwks.json
VMAFX_AUTH_ISSUER=https://YOUR_DOMAIN.auth0.com/
VMAFX_AUTH_AUDIENCE=https://vmafx.example.com/api
VMAFX_AUTH_TENANT_CLAIM=org_id       # Auth0 organisation ID claim
```

In Auth0, add the `org_id` claim to your token and create a custom
`vmafx_roles` action in the Auth0 Login flow.

### Keycloak

```bash
VMAFX_JWKS_ENDPOINT=https://keycloak.example.com/realms/vmafx/protocol/openid-connect/certs
VMAFX_AUTH_ISSUER=https://keycloak.example.com/realms/vmafx
VMAFX_AUTH_AUDIENCE=vmafx-api
VMAFX_AUTH_TENANT_CLAIM=tid          # add as a custom mapper in Keycloak
VMAFX_AUTH_ROLES_CLAIM=vmafx_roles   # add as a custom mapper in Keycloak
```

### Dex

```bash
VMAFX_JWKS_ENDPOINT=https://dex.example.com/keys
VMAFX_AUTH_ISSUER=https://dex.example.com
VMAFX_AUTH_TENANT_CLAIM=tid
```

---

## Roles and RBAC

Four roles are recognised. Include one or more in the `vmafx_roles` claim (a
JSON array, or a single string):

| Role | May call |
| --- | --- |
| `vmafx:reader` | `GetJob`, `StreamJobs`, `VmafxScoring.Health` |
| `vmafx:writer` | Everything a reader may, plus `SubmitJob`, `CancelJob`, `VmafxScoring.Score`, `VmafxScoring.ScoreStream` and HTTP `POST /v1/score` |
| `vmafx:admin` | Everything a writer may |
| `vmafx:node` | The node API only: `RegisterNode`, `Heartbeat`, `PullWork`, `ReportResult` |

`vmafx:node` is the only role that reaches the node API, and it reaches
nothing else: a node's token cannot read, submit or cancel jobs or score, and
no user token can register a node or take jobs from the queue
([ADR-1563](../adr/1563-controller-node-role.md)). Give a node a token that
holds `vmafx:node` alone.

The controller enforces this table on every call
([ADR-1518](../adr/1518-controller-grpc-authorization.md)):

- A gRPC call whose token holds none of the method's roles fails with
  `PERMISSION_DENIED` and the message `role required: <roles>` before the
  handler runs. A token without a `vmafx_roles` claim, with an empty one, or
  with only unknown strings (for example `vmafx:root`) holds no role and is
  refused everywhere.
- HTTP `POST /v1/score` answers `403 Forbidden` to a token without
  `vmafx:writer` or `vmafx:admin`.
- A gRPC method the controller does not list in its role table is refused for
  every caller, including the synthetic admin of [disabled
  mode](#disabling-auth). The table lives in
  `cmd/vmafx-controller/grpc_roles.go`, and a test fails when a served method
  is missing from it.

Roles are not inherited implicitly: the table above lists, for each call,
every role that may make it. A writer can read because reader calls also list
`vmafx:writer`.

Clients therefore need a token with the right role:

- `vmafx-mcp` sends `VMAFX_CONTROLLER_TOKEN` with every controller call; its
  `submit_job` and `cancel_job` tools need `vmafx:writer`, `get_job` and
  `list_jobs` need `vmafx:reader`.
- A compute node needs `vmafx:node` (before ADR-1563 it needed `vmafx:admin`,
  which no longer reaches the node API).

---

## Tenant isolation

Every job is tagged with the `tenant_id` of the submitter's token, and every
call reads and writes only the jobs of its own token's tenant
([ADR-1522](../adr/1522-controller-tenant-scoped-reads.md)):

- `SubmitJob` stamps the new job with the caller's tenant.
- `GetJob` and `CancelJob` answer `PERMISSION_DENIED` (`resource belongs to
  another tenant`) for another tenant's job, without naming that tenant.
- `StreamJobs` streams only the caller's tenant's jobs; the tenant is part of
  the database query, so no other tenant's job is read.
- A node session belongs to the tenant of the token that called
  `RegisterNode`. `Heartbeat` answers `ok=false`, and `PullWork` and
  `ReportResult` answer `PERMISSION_DENIED`, when called with that session and
  a token of another tenant. `PullWork` gives a node only its tenant's jobs.
- `ReportResult` accepts a result, final or partial, for a job assigned to the
  reporting node, or for a running job of the same tenant whose node has no
  live session any more (a node that registered again after a controller
  restart or an eviction reports what it finished before). A report for a job
  of another tenant, a pending job, a job of a live node or an unknown job
  answers `PERMISSION_DENIED` (`job "<id>" is not assigned to node "<id>"`)
  and changes nothing. Repeating a final report of a finished job succeeds
  without changing it.

A deployment that serves several tenants from one pool of nodes therefore
needs a node registration per tenant; nodes shared across tenants are not
supported. Jobs stored before the auth gateway carry the empty tenant, which
no token can hold, so no caller can read them.

Tenant IDs are opaque strings compared exactly (case and whitespace count).

## Scoring roots

A caller may score only inputs that lie under its tenant's scoring roots
([ADR-1577](../adr/1577-scoring-paths-per-tenant.md)). Without that rule a
writer of one tenant could make the controller or a node read another
tenant's media, or any file the process can open, and read the scores as an
oracle. A tenant without roots scores nothing: every input is refused (deny
by default).

A root is one of:

| Kind | Example root | Admits |
| --- | --- | --- |
| Absolute directory | `/media/acme` | `/media/acme/ref.y4m`, `file:///media/acme/x/dis.y4m` |
| http(s) prefix | `https://media.example.com/acme/` | `https://media.example.com/acme/a.y4m` (scheme and host compared case-insensitively) |
| rclone remote prefix | `s3:media/acme`, `s3://media/acme`, `:local:/srv/acme` | `s3:media/acme/k/ref.y4m`, `s3://media/acme/ref.y4m` |

An input is refused when it is relative, contains a `..` element (also
percent-encoded in a URL), or lies outside every root (`/media/acme-old` is
not under `/media/acme`). The refusal is `PERMISSION_DENIED` over gRPC and
`403` over HTTP, with the message `scoring input "<input>" is outside the
tenant's scoring roots`.

Where the check runs:

- `VmafxScoring.Score` and `POST /v1/score` read the files on the controller:
  each input is resolved there, symlinks followed, and must still lie under a
  root; the controller then scores the resolved path, so a link changed
  after the check is not followed. A link inside `/media/acme` that points at
  `/media/rival/secret.y4m` is refused.
- `SubmitJob`'s inputs are read by a node: the controller checks them as
  written, and `PullWork` hands the job's tenant roots to the node
  (`Job.scoring_roots`), which resolves the inputs on its own file system
  before it prepares or scores them, and fails the job otherwise. A node
  refuses a job that arrives without roots.

Where the roots come from:

- With a [tenant registry](#tenant-registry): `spec.scoring.roots` of each
  `VmafxTenant` (at most 32). An invalid root makes the tenant invalid, like
  any other setting.
- With one identity provider or with auth disabled: `VMAFX_SCORING_ROOTS`, a
  comma-separated list for every caller; `{tenant}` in an entry becomes the
  caller's tenant ID (`/media/{tenant},s3:media/{tenant}`). A tenant ID that
  contains `/`, `\`, `:` or is `.` or `..` is refused rather than
  substituted. `VMAFX_SCORING_ROOTS` together with a tenant registry stops
  the controller at startup.

---

## Tenant registry

With `VMAFX_AUTH_TENANTS_SOURCE` set, the controller accepts a token only when
it belongs to exactly one configured, enabled tenant:

1. The token's `iss` claim selects the tenants whose `oidc.issuer` it names.
   No such tenant: `UNAUTHENTICATED` / `401`.
2. Each of those tenants checks the signature against its own
   `oidc.jwksEndpoint`, the expiry, its own issuer and (when set) its own
   `oidc.audience`, then reads its own `oidc.tenantClaim`. The tenant whose
   claim value equals its `tenantId` is the match. A token one tenant's
   provider issued with another tenant's ID matches nothing and is refused;
   a token matching two tenants is refused too.
3. A tenant with `enabled: false` is refused with `PERMISSION_DENIED` / `403`.
4. The caller's roles are the vmafx roles of the token's `oidc.rolesClaim`
   that are in `rbac.allowedRoles`; roles outside the list are dropped. A
   token that names no vmafx role at all gets `rbac.defaultRole`. A token
   whose only vmafx roles are all dropped keeps no role and is refused
   everywhere.

Defaults follow the CRD: `enabled: true`, `tenantClaim: tid`,
`rolesClaim: vmafx_roles`, `defaultRole: vmafx:reader`,
`allowedRoles: [vmafx:reader, vmafx:writer]`. So by default no token of a
registry tenant can act as a node: list `vmafx:node` in `allowedRoles` for a
tenant that runs its own nodes. `defaultRole: vmafx:node` is refused (the CRD
does not offer it and the controller rejects it from a file source): a token
without a role claim never acts as a node.

Set `oidc.audience`: without it, a token the tenant's provider issues for any
other application, with the tenant's claim, is accepted. The controller logs
a warning at startup for each tenant without one. When several tenants share
one provider, the signature of a token is checked once per JWKS endpoint, not
once per tenant.

Refused tokens get a fixed `UNAUTHENTICATED` message (`invalid or missing
token`); the reason (unknown issuer, bad signature, expired, no matching
tenant) is only logged, so a caller cannot probe which issuers and tenants
are configured.

### Sources

- `VMAFX_AUTH_TENANTS_SOURCE=kubernetes` lists the `VmafxTenant` resources of
  `VMAFX_AUTH_TENANTS_NAMESPACE` (default: the controller pod's namespace)
  with the pod's service account, which needs `get`, `list` and `watch` on
  `vmafxtenants` there. The Helm chart grants it (see [Helm
  configuration](#helm-configuration)).
- `VMAFX_AUTH_TENANTS_SOURCE=file` reads `VMAFX_AUTH_TENANTS_FILE`: YAML or
  JSON documents, each a `VmafxTenant` (as in the [CRD
  example](#vmafxtenant-crd)) or a list of them (`kind: List` or
  `VmafxTenantList`, as `kubectl get vmafxtenants -o yaml` writes). Use it to
  run a tenant registry outside Kubernetes.

The controller re-reads the source every `VMAFX_AUTH_TENANTS_REFRESH`
(default `30s`, between `1s` and `1h`), so suspending a tenant or changing its
roles takes effect within one interval, without a restart.

### What stops the controller, and what it refuses at run time

At startup the controller does not start, and logs why, when:

- the source cannot be read, or the file is not YAML or JSON;
- a document is not `apiVersion: vmafx.dev/v1`, `kind: VmafxTenant` (or a
  list of them), or its spec has a field the CRD does not define (for example
  `allowedRole` instead of `allowedRoles`);
- a tenant is invalid: `tenantId` not matching the CRD pattern, an issuer
  that is not an absolute `http(s)` URL, a JWKS endpoint that is not `https`
  (plain `http` is accepted for `localhost` and loopback addresses only), an
  unknown role, an empty `allowedRoles`, or a `defaultRole` that is not in
  `allowedRoles`;
- two tenants have the same `tenantId`;
- a tenant setting does not fit the source (`VMAFX_AUTH_TENANTS_FILE` without
  `VMAFX_AUTH_TENANTS_SOURCE=file`, an unknown source, a refresh interval out
  of range), or the tenant source is combined with `VMAFX_AUTH_DISABLED=true`
  or with `VMAFX_JWKS_ENDPOINT`, `VMAFX_AUTH_ISSUER`, `VMAFX_AUTH_AUDIENCE`,
  `VMAFX_AUTH_TENANT_CLAIM` or `VMAFX_AUTH_ROLES_CLAIM` (each tenant carries
  its own; the global values would be ignored).

Because startup is strict, a `VmafxTenant` that the API server accepts but the
controller refuses (for example `defaultRole: vmafx:admin` with the default
`allowedRoles`) stops the controller from starting after its next restart,
for every tenant. Check a new tenant in the controller log (refreshes report
it, see below) before restarting.

On a refresh, an invalid tenant is dropped and logged
(`tenant refused; its tokens are rejected until it is fixed`) while the other
tenants are reloaded. A refresh that cannot read the source keeps the last
tenant set; once that set is older than ten refresh intervals (five minutes
by default) every request is refused with `UNAVAILABLE` / `503` (`tenant
configuration is stale`), because the controller can no longer tell whether a
tenant was suspended.

---

## Helm configuration

The auth gateway is part of `vmafx-controller`, which the chart deploys as
its own workload with `controller.enabled`
([ADR-1589](../adr/1589-helm-controller-workload.md),
[Kubernetes guide](../development/k8s-deployment.md#controller)). `auth.*`
configures that workload only; `auth.enabled` and `controller.enabled` go
together, and the render fails for one without the other. The server
workload (`vmafx-server`) has no auth gateway and gets no auth settings; an
`image.repository` naming a `vmafx-controller` image is refused.

One identity provider:

```yaml
controller:
  enabled: true
auth:
  enabled: true
  jwksEndpoint: https://idp.example.com/.well-known/jwks.json
  issuer: https://idp.example.com/
  audience: vmafx-api          # optional
  tenantClaim: tid             # default
  rolesClaim: vmafx_roles      # default
  scoringRoots:                # VMAFX_SCORING_ROOTS; none refuses every input
    - /media/{tenant}
```

With a tenant registry, give each entry of `auth.tenants` its
`scoring.roots`; the chart refuses `auth.scoringRoots` next to a registry and
without `auth.enabled`.

These values become the `VMAFX_*` variables of the [environment
table](#environment-variables).

A tenant registry:

```yaml
controller:
  enabled: true
auth:
  enabled: true
  issuer: https://idp.example.com/               # default for entries without one
  jwksEndpoint: https://idp.example.com/keys     # default for entries without one
  tenants:
    - tenantId: acme
      oidc:
        issuer: https://acme.auth0.com/
        jwksEndpoint: https://acme.auth0.com/.well-known/jwks.json
        audience: vmafx-api
      rbac:
        defaultRole: vmafx:reader
        allowedRoles: [vmafx:reader, vmafx:writer]
    - tenantId: lab
      enabled: false                             # suspended
```

A non-empty `auth.tenants` (or `auth.tenantSource: kubernetes`, for
`VmafxTenant` resources managed outside the chart) switches the controller to
the tenant registry. The chart then:

- creates one `VmafxTenant` per entry in the release namespace, filling an
  entry's missing `oidc` fields from the global `auth.issuer`,
  `auth.jwksEndpoint`, `auth.audience`, `auth.tenantClaim` and
  `auth.rolesClaim` (an entry with neither its own nor a global issuer or JWKS
  endpoint fails the render);
- passes `VMAFX_AUTH_TENANTS_SOURCE=kubernetes` and
  `VMAFX_AUTH_TENANTS_NAMESPACE=<release namespace>`, and none of the global
  provider variables;
- grants the controller's own service account (`<serviceAccount
  name>-controller`) `get`, `list` and `watch` on `vmafxtenants` in the
  release namespace (Role and RoleBinding `<release>-tenant-reader`);
- with `networkPolicy.enabled`, allows the controller's egress to the API
  server (`networkPolicy.allow.serverToApiserver`, policy
  `<release>-allow-controller-to-apiserver`, ports 443 and 6443). The rule
  allows those ports to any address, as the operator's rule does, because the
  API server's Service IP cannot be selected. The controller's JWKS fetches
  have their own rule while auth is on
  (`networkPolicy.allow.controllerToIdentityProvider`, port 443). Narrow both
  to your control plane's and identity providers' CIDRs where you know them.

The Role is bound to the controller's own service account and to no other
account ([ADR-1592](../adr/1592-helm-split-service-accounts.md)): the
chart creates `<name>-controller` (from `serviceAccount.name`, default the
chart's full name, as the operator's `<name>-operator`) whenever
`controller.enabled`, and only the controller pods use it. The server, job
and node pods keep the chart's shared account, which holds no RBAC, and the
operator's `ClusterRole` grants nothing on `vmafxtenants`. So only the
controller can read the namespace's `VmafxTenant` resources (identity
provider URLs, audiences and role lists).

The render also fails for `auth.tenants` or `auth.tenantSource` without
`auth.enabled`, for `auth.disabled` combined with a tenant registry, for any
`env.VMAFX_AUTH_*` / `env.VMAFX_JWKS_*` or `controller.env.VMAFX_AUTH_*` /
`VMAFX_JWKS_*` / `VMAFX_SCORING_ROOTS` entry (set those through `auth.*`),
and for an entry with an empty `rbac.allowedRoles` (the CRD would default it
to reader and writer).

Nodes and the operator present tokens of their own to the controller:
`node.controllerToken` and `operator.controllerToken` name a Secret whose key
is mounted as `VMAFX_CONTROLLER_TOKEN_FILE`
([Kubernetes guide](../development/k8s-deployment.md#controller)).

---

## VmafxTenant CRD

Each tenant is a Kubernetes custom resource; the controller reads them as
described in [Tenant registry](#tenant-registry):

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

The Helm `auth.tenants` list and a `kubectl apply`-ed `VmafxTenant` are two
ways to create the same resource, and the controller reads both. The CRD is
installed by the chart's `crds/` directory. The vmafx-operator does not
reconcile `VmafxTenant`; the controller reads the resources directly.

---

## Disabling auth

For internal deployments or integration-test pipelines:

```bash
VMAFX_AUTH_DISABLED=true vmafx-controller
```

When disabled, all requests are processed as tenant `dev` with the roles
`vmafx:admin` and `vmafx:node`, so a local node can register. Never use this in
production.

---

## Environment variables

The controller has no CLI flags beyond `--version`; all configuration is
environment-only (ADR-1119).

The auth variables (`VMAFX_AUTH_*`, `VMAFX_JWKS_ENDPOINT`, `VMAFX_SCORING_ROOTS`)
are rows of the controller's generated
[environment table](controller.md#configuration), with their keys, defaults
and the chart values that set them; the sections above explain how they
combine.

---

## Key rotation

When the controller receives a token whose `kid` (key ID) is not in the
local JWKS cache, it fetches the JWKS endpoint once. To prevent thundering-
herd on rotation, fetches (successful or not) are rate-limited to one per 30
seconds per endpoint.

If the new key is not present in the endpoint's response within the cooldown
window, requests with the new `kid` are rejected with `401` until the cache
refreshes successfully.

Fetched keys are used for 15 minutes; the first token after that refetches
the JWKS, so a key the identity provider withdraws stops verifying tokens
within 15 minutes even if nobody presents a new `kid`. When the refetch fails,
the cached keys keep working for up to 24 hours after their last successful
fetch; past that, tokens are refused until the endpoint answers again. A JWKS
fetch that starts over `https` does not follow a redirect to plain `http`
(except to a loopback host).

---

## Threat model summary

| Threat | Mitigation |
| --- | --- |
| Algorithm confusion (`alg=none`, `alg=HS256`) | Only RS256 is accepted; any other `alg` header is rejected before key lookup. |
| Token replay | `exp` checked on every request. |
| Cross-tenant data access | Every job read and write is scoped to the token's tenant (`GetJob`, `CancelJob`, `StreamJobs`, `SubmitJob`); node sessions belong to one tenant, nodes pull only that tenant's jobs and report only jobs assigned to them. Refusals do not name the owning tenant. |
| JWKS endpoint spoofing | Endpoint configured by operator via trusted Helm/env values. |
| Privilege escalation | Every gRPC method and HTTP `POST /v1/score` require a role from the token; a gRPC method without a role entry is refused. With a tenant registry, roles outside a tenant's `allowedRoles` are dropped. |
| One tenant's provider acting for another | With a tenant registry, a token is verified with the provider of the tenant it names, and that tenant's claim must carry its own ID. |
| Suspension not applied | `enabled: false` refuses the tenant within one refresh interval; a tenant set the controller cannot refresh for ten intervals refuses everyone. |
| Revocation | Use short-lived tokens (≤1 hour); revocation list support is a follow-up. |
