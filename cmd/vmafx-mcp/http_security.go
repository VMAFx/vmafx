// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// http_security.go ports the Python MCP HTTP transport hardening (ADR-0967)
// to the Go streamable-HTTP transport: bearer-token auth, a request-body size
// limit, and a loopback-only bind default. The Go and Python servers expose
// the same HTTP surface to MCP clients, so the security posture must match —
// otherwise the Go server is an unauthenticated, all-interfaces, unbounded-body
// hole where the Python server is locked down.
//
// Environment contract (identical names to the Python transport so a single
// deployment config drives both, ADR-0967):
//
//	VMAFX_MCP_HTTP_TOKEN    Bearer token. When set (and NO_AUTH is unset),
//	                        every request must carry
//	                        `Authorization: Bearer <token>` matching it via a
//	                        constant-time compare.
//	VMAFX_MCP_HTTP_NO_AUTH  Set to "1" to disable auth entirely (explicit
//	                        operator opt-out). Any other value keeps auth on.
//	VMAFX_MCP_HTTP_BIND     Bind host. Defaults to 127.0.0.1 (loopback-only).
//	                        Set to 0.0.0.0 to listen on all interfaces. Only
//	                        applied when the configured listen address
//	                        (mcp.http.addr) has no explicit host (e.g. ":3000").
//
// Auth-default semantics mirror the Python middleware exactly: when neither
// VMAFX_MCP_HTTP_TOKEN nor VMAFX_MCP_HTTP_NO_AUTH is set, the server refuses
// ALL traffic with 401 — a missing token means the operator has not set up
// auth, and rejecting is safer than silently accepting.
//
// These vars are read directly via os.Getenv (NOT through koanf): they share
// the secret-handling contract of VMAF_BIN / VMAFX_MCP_DIRECT and must not be
// logged or surfaced through the config dump.

package main

import (
	"crypto/subtle"
	"encoding/json"
	"net"
	"net/http"
	"os"
	"strings"
)

// maxRequestBodyBytes bounds the request body the HTTP transport accepts.
// Matches the Python transport's MAX_REQUEST_BODY_BYTES (4 MiB) so both servers
// reject the same oversized payloads.
const maxRequestBodyBytes int64 = 4 * 1024 * 1024 // 4 MiB

// resolveAuthToken returns the expected bearer token, or "" when unset/empty.
func resolveAuthToken() string {
	return strings.TrimSpace(os.Getenv("VMAFX_MCP_HTTP_TOKEN"))
}

// noAuthMode reports whether VMAFX_MCP_HTTP_NO_AUTH=1 disables auth.
func noAuthMode() bool {
	return strings.TrimSpace(os.Getenv("VMAFX_MCP_HTTP_NO_AUTH")) == "1"
}

// resolveBindHost returns the bind host, defaulting to 127.0.0.1 (loopback)
// per ADR-0967. Operators set VMAFX_MCP_HTTP_BIND=0.0.0.0 to listen on all
// interfaces.
func resolveBindHost() string {
	if h := strings.TrimSpace(os.Getenv("VMAFX_MCP_HTTP_BIND")); h != "" {
		return h
	}
	return "127.0.0.1"
}

// applyBindHost reconciles the configured listen address (mcp.http.addr, e.g.
// ":3000" or "0.0.0.0:3000") with the VMAFX_MCP_HTTP_BIND host default. When
// the configured address has no explicit host (host part empty, the ":3000"
// form, which net.Listen treats as all-interfaces), the loopback-only default
// from resolveBindHost() is substituted so the Go transport is not exposed on
// every interface by default. When the operator already pinned a host in
// mcp.http.addr, it wins (explicit config beats the env default).
func applyBindHost(addr string) string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		// Not a host:port string (e.g. a bare port or malformed value); leave
		// it to net.Listen to interpret / error on, unchanged.
		return addr
	}
	if host == "" {
		return net.JoinHostPort(resolveBindHost(), port)
	}
	return addr
}

// writeJSONError writes a JSON {"error": msg} body with the given status.
func writeJSONError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	// Best-effort: the connection may already be gone; nothing to recover.
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// securityMiddleware wraps an http.Handler with the ADR-0967 body-size and
// bearer-auth gates, mirroring the Python _make_security_middleware exactly:
//
//   - Body size: a Content-Length header exceeding maxRequestBodyBytes is
//     rejected with 413 before the handler runs; the body itself is wrapped in
//     http.MaxBytesReader so chunked / unknown-length bodies are capped too
//     (the inner handler sees a read error once the cap is hit).
//   - Auth: when VMAFX_MCP_HTTP_NO_AUTH=1, auth is skipped. Otherwise a missing
//     VMAFX_MCP_HTTP_TOKEN means refuse-all (401). When a token is configured,
//     the Authorization: Bearer <token> header must match it via a
//     constant-time compare.
//
// Health/metrics routes are NOT exempt, matching the Python transport.
func securityMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// --- Body-size pre-flight via Content-Length header -------------------
		if r.ContentLength > maxRequestBodyBytes {
			writeJSONError(w, http.StatusRequestEntityTooLarge,
				"Request body too large: Content-Length exceeds limit")
			return
		}
		// Cap chunked / unknown-length bodies too. MaxBytesReader makes a read
		// past the limit fail inside the handler with a 413-shaped error.
		if r.Body != nil {
			r.Body = http.MaxBytesReader(w, r.Body, maxRequestBodyBytes)
		}

		// --- Auth gate --------------------------------------------------------
		if !noAuthMode() {
			expected := resolveAuthToken()
			if expected == "" {
				// No token configured and no explicit opt-out: refuse all
				// traffic (safer than silently accepting). Mirrors Python.
				writeJSONError(w, http.StatusUnauthorized,
					"Unauthorized: server requires VMAFX_MCP_HTTP_TOKEN or VMAFX_MCP_HTTP_NO_AUTH=1")
				return
			}
			const prefix = "Bearer "
			authHeader := r.Header.Get("Authorization")
			presented, ok := strings.CutPrefix(authHeader, prefix)
			// Constant-time compare guards against token enumeration via
			// wall-clock timing side-channels (matches hmac.compare_digest).
			if !ok || subtle.ConstantTimeCompare([]byte(presented), []byte(expected)) != 1 {
				writeJSONError(w, http.StatusUnauthorized,
					"Unauthorized: invalid or missing Bearer token")
				return
			}
		}

		next.ServeHTTP(w, r)
	})
}
