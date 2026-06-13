<!-- markdownlint-disable MD013 MD060 -->
# ADR-0967: MCP HTTP transport security — add auth + body limit + safer bind default (Round 26 audit A.1)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: security, mcp, http, auth, hardening, fork-local

## Context

The MCP HTTP transport (`mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py`, introduced
in ADR-0701) shipped three gaps that were flagged as finding A.1 in the Round 26 security
audit:

1. **Unbounded request bodies.** `_handle_score` called `await request.json()` with no
   upper bound on the request body. A client could stream an arbitrarily large POST and
   exhaust server memory or trigger an OOM kill.

2. **No authentication.** Any client on the network that could reach the TCP port received
   a valid response. There was no `Authorization` header enforcement, no token, no opt-in
   gate.

3. **Default bind `0.0.0.0`.** The server bound all interfaces unconditionally.  In a
   container or VM without a network firewall, this exposed the MCP endpoint to the entire
   host network — including any bridge networks shared with other containers.

**Breaking-change note (bind default):** Pre-ADR-0967 the default bind host was
`0.0.0.0`.  After ADR-0967 it is `127.0.0.1`.  Existing Helm deployments, Docker Compose
files, and bare `docker run` invocations that rely on pod-network or container-network
reachability **must** set `VMAFX_MCP_HTTP_BIND=0.0.0.0` to restore the old behaviour.
The Helm values file (`deploy/helm/vmafx/values.yaml`) documents this operator choice.

## Decision

We add three complementary security layers to the HTTP transport, all configurable via
environment variables (12-factor §III) and implemented without new runtime dependencies:

1. **Body size limit (4 MiB).**  Two complementary mechanisms enforce it:
   - A `Content-Length` pre-flight in the security middleware rejects requests whose
     declared size exceeds `MAX_REQUEST_BODY_BYTES` (4 MiB) immediately with HTTP 413,
     before any body bytes are read.
   - `client_max_size=MAX_REQUEST_BODY_BYTES` on the aiohttp `Application` enforces the
     limit for chunked / streaming bodies — aiohttp raises `HTTPRequestEntityTooLarge`
     (413) inside `request.read()` / `request.json()`.

2. **Bearer token authentication.**  A new `@web.middleware` function checks
   `Authorization: Bearer <token>` on every request:
   - Token is read from `VMAFX_MCP_HTTP_TOKEN`.  If the env var is unset and
     `VMAFX_MCP_HTTP_NO_AUTH` is also unset, the server starts but rejects every request
     with HTTP 401.  This "fail-closed" default means an operator who forgets to configure
     auth sees a clear 401 rather than an accidentally-open service.
   - Explicit opt-out: `VMAFX_MCP_HTTP_NO_AUTH=1` disables authentication (for deployments
     that sit behind a network-level gateway).  A startup warning is logged.

3. **Loopback-only bind default.**  `_resolve_bind_host()` returns `127.0.0.1` when
   `VMAFX_MCP_HTTP_BIND` is unset.  Operators who need pod-network or host-network
   reachability set `VMAFX_MCP_HTTP_BIND=0.0.0.0` explicitly.

4. **Optional TLS.**  If `VMAFX_MCP_HTTP_TLS_CERT` and `VMAFX_MCP_HTTP_TLS_KEY` are both
   set, an `ssl.SSLContext` is built and passed to `aiohttp.web.TCPSite`.  If either is
   absent, a startup warning is logged (HTTP-only mode).

The Unix-domain-socket transport (default stdio / UDS) is unaffected by all of the above.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| No auth (status quo) | Zero operator friction | Any reachable client has full API access; violates OWASP API Security Top 10 (API1:2023 Broken Object Level Authorization / API2:2023 Broken Authentication) | Rejected — unacceptable for a production service |
| Path-allowlist auth (restrict `/v1/score` only) | Simple to implement | Health probes and `/metrics` still unauthenticated; attacker can still enumerate endpoints; partial solutions invite operator confusion | Rejected — auth must be uniform; a health probe leaks "is this host running vmafx" to unauthenticated callers |
| Bearer token (chosen) | Zero new deps; widely understood by HTTP clients; easy to rotate; composable with network-layer TLS or mTLS | Shared secret; token rotation requires restart or reload; not suitable for multi-tenant key-per-client scenarios | Accepted — best fit for single-operator deployment model; multi-tenant can be layered via a gateway |
| mTLS only | Strongest identity guarantee; eliminates shared-secret risks | Requires PKI and certificate provisioning; not available in edge/dev deployments; significantly higher operator burden | Deferred — can be added as a follow-on via `ssl.SSLContext.verify_mode = ssl.CERT_REQUIRED`; the TLS scaffolding in this ADR already loads the SSLContext, so mTLS is a one-flag change |

## Consequences

- **Positive**: HTTP transport is now fail-closed on auth; body-exhaustion DoS is
  bounded at 4 MiB; default bind is safe for local dev and container deployments
  without an explicit firewall rule.
- **Negative**: **Breaking bind change** — existing deployments that relied on
  `0.0.0.0` default must add `VMAFX_MCP_HTTP_BIND=0.0.0.0`.  Operators who have not
  configured a token see 401 for all traffic (by design — better than silent
  open access).
- **Neutral / follow-ups**: TLS is scaffolded but optional; mTLS requires no
  additional scaffolding — add `ssl_context.verify_mode = ssl.CERT_REQUIRED` +
  `ssl_context.load_verify_locations(ca)` if desired.  `VMAFX_MCP_HTTP_NO_AUTH=1`
  must be documented in `deploy/helm/vmafx/values.yaml` for operators using
  network-layer auth gateways.

## References

- [ADR-0701](0701-vmafx-cloud-native-redesign.md) — HTTP transport foundation (introduces the transport this ADR hardens).
- OWASP API Security Top 10 — API2:2023 Broken Authentication, API4:2023 Unrestricted Resource Consumption.
- Source: Round 26 security audit finding A.1 (user direction, 2026-05-31).
