### Fixed

- **security**: Replace plain `==`/`!=` session-token comparisons in
  `cmd/vmafx-controller/nodes/registry.go` (`Heartbeat`, `ValidateSession`) with
  `crypto/subtle.ConstantTimeCompare` to eliminate timing-oracle side-channels
  (r5-crypto HIGH).
- **security**: Replace `!=` Bearer-token comparison in
  `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` with `hmac.compare_digest`
  (r5-crypto MEDIUM).
- **security**: Validate JWT `nbf` (not-before) claim in
  `cmd/vmafx-controller/auth/middleware.go`; tokens with a future `nbf` are now
  rejected with "token not yet valid" (r5-crypto MEDIUM).

ADR: [ADR-1021](docs/adr/1021-session-token-const-time-compare.md)
