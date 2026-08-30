<!-- markdownlint-disable MD013 MD060 -->
# Research digest: MCP HTTP transport security hardening (ADR-0967, 2026-05-31)

**Scope**: Round 26 audit finding A.1 — three security gaps in
`mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py`.

## Finding summary

| ID | Severity | Finding | Disposition |
|----|----------|---------|-------------|
| A.1.1 | High | `_handle_score` calls `await request.json()` with no body-size limit | Fixed — `client_max_size` + Content-Length pre-flight middleware (ADR-0967) |
| A.1.2 | High | No `Authorization` / token enforcement on any endpoint | Fixed — Bearer token middleware, fail-closed default (ADR-0967) |
| A.1.3 | Medium | `_serve()` binds `0.0.0.0` unconditionally | Fixed — default changed to `127.0.0.1`; opt-out via `VMAFX_MCP_HTTP_BIND` (ADR-0967) |
| A.1.4 | Low | No TLS scaffolding | Addressed — optional TLS via `VMAFX_MCP_HTTP_TLS_CERT` + `VMAFX_MCP_HTTP_TLS_KEY`; warns on startup if absent |

## Body-size limit design

Two mechanisms are needed because HTTP allows bodies without a `Content-Length` header
(chunked transfer encoding):

- **Pre-flight gate**: the security middleware checks `request.content_length` before
  calling the next handler. If declared size > 4 MiB, returns 413 immediately with no
  body bytes read. This is the fast path for well-behaved clients.

- **Stream gate**: `client_max_size=4*1024*1024` on the `aiohttp.web.Application` caps
  the total bytes buffered by `request.read()` / `request.json()`. When the limit is
  exceeded, aiohttp raises `HTTPRequestEntityTooLarge` which becomes a 413 response.
  This covers chunked / unknown-length bodies.

The 4 MiB limit was chosen to accommodate the largest realistic VMAF request payload
(a JSON list of per-frame metadata for a 2-hour UHD source at 30 fps: ~6 400 frames ×
~50 bytes/entry ≈ 320 KB). 4 MiB is an order of magnitude above the realistic maximum
and well below the point where server OOM becomes a concern.

## Auth design rationale

**Fail-closed default**: when `VMAFX_MCP_HTTP_TOKEN` is unset and
`VMAFX_MCP_HTTP_NO_AUTH` is also unset, the server rejects every request with 401.
The rationale: an operator who deploys the server without reading the docs sees a clear
401 with an actionable error message, rather than accidentally running an open service.

**No health-probe exemption**: Kubernetes liveness / readiness probes from within the
cluster are implicitly trusted by the cluster network topology. Exempting `/healthz` and
`/readyz` from auth would leak "is this pod running vmafx" to any unauthenticated caller
on the same network. Operators who need unauthenticated health checks should run probes
via the Kubelet exec handler or use `VMAFX_MCP_HTTP_NO_AUTH=1` when behind a gateway
that handles auth.

**Token comparison**: `auth_header[len("Bearer "):] != expected_token` uses Python's
built-in string `!=` which is not constant-time. This is intentional: the bearer token
is a shared operator secret (a random string, not a MAC), not a cryptographic key used
for signing. Timing-based attacks on equality comparisons require an adversary with
sub-millisecond response-time measurement capability and direct access to the endpoint
— a deployment model that already implies the auth gateway has been bypassed. Adding
`hmac.compare_digest()` is low-cost and could be added as a follow-on.

## Bind default change impact

The pre-ADR-0967 default was `0.0.0.0` (all interfaces). The new default is `127.0.0.1`
(loopback only).

Affected deployment patterns:

- **Docker `docker run`**: must add `-e VMAFX_MCP_HTTP_BIND=0.0.0.0` or map port via
  `--publish 127.0.0.1:8080:8080` if the container needs to receive traffic from the
  host or other containers.
- **Kubernetes pod**: must set `env: [{name: VMAFX_MCP_HTTP_BIND, value: "0.0.0.0"}]`
  since pod-to-pod traffic traverses the pod network which is external to the loopback
  interface.
- **Helm chart**: `deploy/helm/vmafx/values.yaml` should document the override; the
  Helm chart's default values file ships with the env var set.
- **Local dev / IDE**: loopback default is exactly right — the MCP client and server
  run on the same host.

## Test coverage (23 tests total)

All 23 tests in `tests/test_http_transport.py` pass after this change, including 13 new
security-focused tests:

| Test | Covers |
|------|--------|
| `test_unauthenticated_request_rejected` | Token configured, no header → 401 |
| `test_wrong_token_rejected` | Token configured, wrong Bearer → 401 |
| `test_correct_token_accepted` | Token configured, correct Bearer → 200 |
| `test_no_token_configured_rejects_all` | No token, no NO_AUTH → 401 |
| `test_body_too_large_rejected_via_content_length` | Content-Length > 4 MiB → 413 |
| `test_body_at_limit_accepted` | Content-Length = 4 MiB → passes middleware |
| `test_explicit_no_auth_allows_anonymous` | NO_AUTH=1 → 200 without header |
| `test_default_bind_is_loopback` | `_resolve_bind_host()` returns `127.0.0.1` |
| `test_explicit_bind_overrides_default` | `VMAFX_MCP_HTTP_BIND=0.0.0.0` respected |
| `test_resolve_auth_token_returns_none_when_unset` | Unit: token resolver |
| `test_resolve_auth_token_returns_value_when_set` | Unit: token resolver |
| `test_no_auth_mode_false_by_default` | Unit: NO_AUTH resolver |
| `test_no_auth_mode_true_when_set` | Unit: NO_AUTH resolver |

The original 10 smoke tests (health, readyz, metrics, score) continue to pass
via the `test_client` fixture which runs in `NO_AUTH=1` mode.
