# ADR-1021: Constant-time session-token comparison + JWT nbf-claim validation

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `security`, `auth`

## Context

A security audit (r5-crypto) identified four findings across the auth surfaces of
the vmafx-controller and MCP HTTP transport:

1. `cmd/vmafx-controller/nodes/registry.go` — `Heartbeat` (line 133) and
   `ValidateSession` (line 162) compared 32-char hex session tokens with plain
   `!=` / `==`.  String equality in Go is not constant-time; the comparison
   returns as soon as a differing byte is found.  An attacker with sub-millisecond
   clock access to the endpoint can enumerate tokens one character at a time
   (timing oracle), reducing a 256-bit brute-force space to at most 64 sequential
   guesses of 16 hex values.

2. `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` — the `Authorization:
   Bearer` header was compared against the configured token using Python's `!=`
   operator, which is also non-constant-time for strings of identical prefix.

3. `cmd/vmafx-controller/auth/middleware.go` — `verifyJWT` validated the `exp`
   claim but did not check the `nbf` (not-before) claim.  A token issued with a
   future `nbf` was accepted immediately, allowing pre-issued tokens to be used
   before their intended validity window.

All three issues violate SEI CERT C / Go and Python secure-coding guidance
(timing-safe comparisons for secrets; full standard-claim validation for JWTs).

## Decision

- In Go, replace all session-token `==` / `!=` comparisons with
  `crypto/subtle.ConstantTimeCompare` — the standard library primitive for
  constant-time byte-slice comparison.
- In Python, replace the `!=` Bearer-token comparison with
  `hmac.compare_digest` from the standard library.
- In `verifyJWT`, after parsing the payload struct, add an `nbf` field and
  reject tokens where `nbf != 0 && now < nbf`.

No new dependencies are introduced; all three fixes use standard-library
primitives available in the minimum supported language versions.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `==` / `!=`, rely on network latency jitter | Zero code change | Latency jitter on a local network or localhost loopback is insufficient to mask per-byte timing; not a viable mitigation | Rejected |
| Switch to opaque tokens (not hex-encoded) | Shorter tokens, harder to partially guess | Does not eliminate the timing channel for the comparison itself; orthogonal improvement | Out of scope for this fix |
| Third-party constant-time library | Potentially more ergonomic API | Adds a dependency; standard library covers the need | Unnecessary |

## Consequences

- **Positive**: session-token comparisons are now safe against timing-oracle
  attacks.  JWT `nbf` is enforced, closing a window for pre-issued tokens.
- **Negative**: `subtle.ConstantTimeCompare` always compares all bytes (even
  after a mismatch), adding at most O(32) byte comparisons on the hot path —
  negligible for a heartbeat RPC.
- **Neutral / follow-ups**: existing tests are extended with
  `TestValidateSession_ConstantTime` (nodes) and `TestVerifyToken_NbfRejected` /
  `TestVerifyToken_NbfInPastAccepted` (auth) to guard against regression.

## References

- r5-crypto audit findings HIGH × 2 + MEDIUM × 2.
- Go `crypto/subtle` package: <https://pkg.go.dev/crypto/subtle>
- Python `hmac.compare_digest`: <https://docs.python.org/3/library/hmac.html#hmac.compare_digest>
- RFC 7519 §4.1.5 — "nbf" (Not Before) Claim.
- SEI CERT MSC61-J (timing-safe comparisons).
