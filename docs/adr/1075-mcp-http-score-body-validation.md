<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1075: MCP HTTP transport `POST /v1/score` body-validation edge cases

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `mcp`, `security`, `correctness`, `http`, `fork-local`

## Context

Two correctness bugs were found in `_handle_score` in
`mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py`:

**Bug A — chunked oversize body mis-reported as 400 instead of 413.**
`aiohttp.web.HTTPRequestEntityTooLarge` is a subclass of both `Exception`
and `aiohttp.web.Response`.  When a chunked (no `Content-Length`) request
body exceeds the `client_max_size` limit set on the `Application`, aiohttp
raises `HTTPRequestEntityTooLarge` inside `request.json()`.  The bare
`except Exception` at that call site caught the exception and re-wrapped
it as a 400 "invalid JSON" response rather than propagating the 413.
Callers observing 400 cannot distinguish a syntax error from an oversize
body; the HTTP spec mandates 413 for the latter.

**Bug B — JSON `null` body causes uncaught `TypeError` → uncontrolled 500.**
`request.json()` succeeds for any valid JSON value, including the JSON
literal `null` (Python `None`).  The next statement performs membership
tests `f not in body` on the returned value.  When `body` is `None` this
raises `TypeError: argument of type 'NoneType' is not a container or
iterable`, which propagated as an unhandled exception through the aiohttp
framework — producing a plain-text 500 with no `request_id` field and the
wrong `Content-Type` header.  The same issue affected JSON integers,
booleans, and floating-point values as the body.

Both bugs were identified through systematic edge-case coverage testing
(ADR-0108 scope: malformed JSON-RPC, oversize payload, transport-specific
errors).

## Decision

Fix `_handle_score` to handle both cases correctly:

1. Add `except aiohttp.web.HTTPRequestEntityTooLarge: raise` **before** the
   generic `except Exception` block so that chunked oversize bodies propagate
   as 413 (aiohttp converts the exception to the correct wire response) rather
   than being swallowed as 400.

2. After `request.json()` returns, check `isinstance(body, dict)` and return
   a structured 400 response (with `request_id`) when the body is not a JSON
   object.

Update `docs/mcp/http-transport.md` to document the 413 and 401 responses in
the error-response table.

Add nine regression tests in
`mcp-server/vmaf-mcp/tests/test_mcp_http_edge_cases_adr1075.py` covering:
both bugs, GET on `/v1/score` (→ 405), array/integer/string/null JSON bodies,
empty body, and concurrent request ID uniqueness.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wrap entire handler in `try/except` | Single catch point | Masks other genuine 500 errors; violates narrow-exception principle | Too broad |
| Validate `Content-Type: application/json` header before parsing | Fails fast for non-JSON requests | Does not protect against valid JSON of wrong type; clients may omit Content-Type | Incomplete |
| Let aiohttp default 500 handler emit the error | Zero code change | No `request_id` in the response body; plain-text 500 breaks clients that expect JSON | Unacceptable for an API |

## Consequences

- **Positive**: chunked oversize bodies now return correct 413; non-dict JSON
  bodies return structured 400 with `request_id`; both are testable via the
  new regression suite.
- **Negative**: none; both changes are narrowly scoped to the affected code path.
- **Neutral / follow-ups**: `docs/mcp/http-transport.md` now documents 413 and
  401 in the error-response table for `/v1/score`.

## References

- `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` — `_handle_score` function.
- `mcp-server/vmaf-mcp/tests/test_mcp_http_edge_cases_adr1075.py` — regression tests.
- [ADR-0967](0967-mcp-http-transport-security-hardening.md) — original security hardening
  (auth + body limit + bind default).
- [ADR-0701](0701-vmafx-server-http-transport.md) — HTTP transport foundation.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverables rule.
