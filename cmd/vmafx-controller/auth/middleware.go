// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/middleware.go — JWT bearer-token middleware for
// the vmafx-controller HTTP and gRPC endpoints.
//
// Architecture
// ============
// This package implements a three-layer auth model:
//
//  1. Authentication — RS256 JWT verification against a JWKS endpoint (OIDC).
//     Supports generic OIDC providers: Auth0, Keycloak, Dex, and any
//     RFC 8414-compliant IdP.
//
//  2. Tenant isolation — every request carries a tenant_id derived from the
//     JWT's "tid" or "tenant_id" claim.  All controller operations (job submit,
//     get, cancel, stream) are scoped to that tenant.
//
//  3. RBAC — three roles per tenant extracted from the JWT "vmafx_roles" claim:
//       vmafx:reader  — GET-only (GetJob, StreamJobs, /v1/score readonly)
//       vmafx:writer  — submit / cancel jobs (SubmitJob, CancelJob, /v1/score)
//       vmafx:admin   — all of the above + node management (RegisterNode, PullWork)
//
// Token structure expected
// ========================
//
//	{
//	  "iss": "https://idp.example.com",
//	  "sub": "user123",
//	  "tid": "acme",           // tenant_id — required
//	  "vmafx_roles": ["vmafx:writer"],
//	  "exp": 1893456000,
//	  ...
//	}
//
// JWKS caching
// ============
// JWKS keys are fetched from the IdP's JWKS URI on first use and refreshed
// automatically when a token presents an unknown key ID (kid rotation).  The
// cache holds at most jwksCacheMax entries and is refreshed at most once every
// jwksCacheRefreshCooldown to prevent thundering-herd on rotation.
//
// ADR-0794: multi-tenant auth gateway.

package auth

import (
	"context"
	"crypto/rsa"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"math/big"
	"net/http"
	"strings"
	"sync"
	"time"
)

// ---------------------------------------------------------------------------
// Public constants / sentinel values
// ---------------------------------------------------------------------------

// Role constants that appear in the "vmafx_roles" JWT claim.
const (
	RoleReader = "vmafx:reader"
	RoleWriter = "vmafx:writer"
	RoleAdmin  = "vmafx:admin"
)

// contextKey is an unexported type for context keys in this package.
type contextKey int

const (
	ctxTenantID contextKey = iota
	ctxRoles
	ctxSubject
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Config holds the runtime configuration for the auth middleware.
// All fields are required unless noted.
type Config struct {
	// JWKSEndpoint is the URL of the IdP's JWKS endpoint, e.g.
	// "https://idp.example.com/.well-known/jwks.json".
	JWKSEndpoint string

	// Issuer is the expected "iss" claim value.  Tokens with a different
	// issuer are rejected with 401.
	Issuer string

	// Audience is the expected "aud" claim value (optional).
	// If empty, audience validation is skipped.
	Audience string

	// TenantClaim is the JWT claim field that carries the tenant identifier.
	// Defaults to "tid".
	TenantClaim string

	// RolesClaim is the JWT claim field that carries the list of roles.
	// Defaults to "vmafx_roles".
	RolesClaim string

	// Disabled bypasses all auth checks.  FOR TESTING ONLY.
	// In production this must be false.
	Disabled bool

	// Logger is the slog.Logger instance.  If nil, slog.Default() is used.
	Logger *slog.Logger
}

func (c *Config) tenantClaim() string {
	if c.TenantClaim != "" {
		return c.TenantClaim
	}
	return "tid"
}

func (c *Config) rolesClaim() string {
	if c.RolesClaim != "" {
		return c.RolesClaim
	}
	return "vmafx_roles"
}

// ---------------------------------------------------------------------------
// Claims
// ---------------------------------------------------------------------------

// Claims is the set of auth information extracted from a validated JWT.
type Claims struct {
	Subject  string
	TenantID string
	Roles    []string
}

// HasRole returns true if the caller holds at least one of the given roles.
func (c Claims) HasRole(roles ...string) bool {
	for _, want := range roles {
		for _, have := range c.Roles {
			if have == want {
				return true
			}
		}
	}
	return false
}

// TenantIDFromCtx extracts the tenant_id from a request context.
// Returns "" if not present (unauthenticated or auth disabled).
func TenantIDFromCtx(ctx context.Context) string {
	v, _ := ctx.Value(ctxTenantID).(string)
	return v
}

// ClaimsFromCtx extracts the full Claims from a request context.
func ClaimsFromCtx(ctx context.Context) (Claims, bool) {
	roles, _ := ctx.Value(ctxRoles).([]string)
	sub, _ := ctx.Value(ctxSubject).(string)
	tid := TenantIDFromCtx(ctx)
	if tid == "" {
		return Claims{}, false
	}
	return Claims{Subject: sub, TenantID: tid, Roles: roles}, true
}

// ---------------------------------------------------------------------------
// JWKS cache
// ---------------------------------------------------------------------------

const (
	jwksCacheMax             = 16
	jwksCacheRefreshCooldown = 30 * time.Second
	jwksFetchTimeout         = 10 * time.Second
)

// jwkKey holds a single RSA public key from the JWKS endpoint.
type jwkKey struct {
	kid string
	pub *rsa.PublicKey
}

// jwksCache caches RSA keys fetched from an IdP's JWKS endpoint.
type jwksCache struct {
	mu          sync.RWMutex
	endpoint    string
	keys        map[string]*rsa.PublicKey // kid → key
	lastRefresh time.Time
	client      *http.Client
	log         *slog.Logger
}

func newJWKSCache(endpoint string, log *slog.Logger) *jwksCache {
	return &jwksCache{
		endpoint: endpoint,
		keys:     make(map[string]*rsa.PublicKey),
		client:   &http.Client{Timeout: jwksFetchTimeout},
		log:      log,
	}
}

// Key returns the RSA public key for the given kid.  It fetches the JWKS
// endpoint if the kid is unknown or the cache is stale.
func (c *jwksCache) Key(kid string) (*rsa.PublicKey, error) {
	c.mu.RLock()
	k, ok := c.keys[kid]
	c.mu.RUnlock()
	if ok {
		return k, nil
	}
	// Unknown kid — refresh (rate-limited).
	return c.refresh(kid)
}

// refresh fetches the JWKS endpoint.  It is rate-limited by jwksCacheRefreshCooldown.
func (c *jwksCache) refresh(kid string) (*rsa.PublicKey, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	// Another goroutine may have refreshed while we waited for the write lock.
	if k, ok := c.keys[kid]; ok {
		return k, nil
	}

	// Cooldown guard.
	if time.Since(c.lastRefresh) < jwksCacheRefreshCooldown {
		return nil, fmt.Errorf("jwks: key %q not found and refresh is rate-limited", kid)
	}

	c.log.Info("jwks: refreshing key cache", "endpoint", c.endpoint, "reason_kid", kid)

	ctx, cancel := context.WithTimeout(context.Background(), jwksFetchTimeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.endpoint, nil)
	if err != nil {
		return nil, fmt.Errorf("jwks: build request: %w", err)
	}

	resp, err := c.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("jwks: fetch %s: %w", c.endpoint, err)
	}
	defer resp.Body.Close() //nolint:errcheck // read-only close

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("jwks: endpoint returned %d", resp.StatusCode)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20)) // 1 MiB limit
	if err != nil {
		return nil, fmt.Errorf("jwks: read body: %w", err)
	}

	keys, err := parseJWKS(body)
	if err != nil {
		return nil, fmt.Errorf("jwks: parse: %w", err)
	}

	// Build the cache from ALL parsed keys.  Round-3 R3-13: we must NOT
	// truncate by document position — real OIDC providers (Auth0, Azure AD,
	// Keycloak) publish more than jwksCacheMax keys during rotation-overlap
	// windows, and a positional slice cut could drop exactly the requested
	// kid, 401-ing valid tokens for the whole refresh-cooldown window.  The
	// 1 MiB body limit above already bounds how many keys can arrive.
	next := make(map[string]*rsa.PublicKey, len(keys))
	for _, k := range keys {
		next[k.kid] = k.pub
	}
	// Defensive memory bound: if the IdP advertises an unreasonable number of
	// keys, evict entries down to jwksCacheMax — but never evict the kid that
	// triggered this refresh, so the caller's valid token is always honoured.
	if len(next) > jwksCacheMax {
		for evictKid := range next {
			if len(next) <= jwksCacheMax {
				break
			}
			if evictKid == kid {
				continue
			}
			delete(next, evictKid)
		}
	}
	c.keys = next
	c.lastRefresh = time.Now()

	c.log.Info("jwks: key cache updated", "key_count", len(c.keys))

	k, ok := c.keys[kid]
	if !ok {
		return nil, fmt.Errorf("jwks: key %q not present in IdP's JWKS", kid)
	}
	return k, nil
}

// parseJWKS parses a JWKS JSON document and returns RSA public keys.
func parseJWKS(body []byte) ([]jwkKey, error) {
	var doc struct {
		Keys []struct {
			Kty string `json:"kty"`
			Kid string `json:"kid"`
			N   string `json:"n"`
			E   string `json:"e"`
		} `json:"keys"`
	}
	if err := json.Unmarshal(body, &doc); err != nil {
		return nil, err
	}

	var result []jwkKey
	for _, k := range doc.Keys {
		if k.Kty != "RSA" {
			continue
		}
		pub, err := rsaKeyFromComponents(k.N, k.E)
		if err != nil {
			return nil, fmt.Errorf("jwks: parse key %q: %w", k.Kid, err)
		}
		result = append(result, jwkKey{kid: k.Kid, pub: pub})
	}
	return result, nil
}

// rsaKeyFromComponents reconstructs an *rsa.PublicKey from JWKS n / e fields.
func rsaKeyFromComponents(nB64, eB64 string) (*rsa.PublicKey, error) {
	nBytes, err := base64.RawURLEncoding.DecodeString(nB64)
	if err != nil {
		return nil, fmt.Errorf("decode n: %w", err)
	}
	eBytes, err := base64.RawURLEncoding.DecodeString(eB64)
	if err != nil {
		return nil, fmt.Errorf("decode e: %w", err)
	}

	n := new(big.Int).SetBytes(nBytes)
	e := new(big.Int).SetBytes(eBytes)

	return &rsa.PublicKey{N: n, E: int(e.Int64())}, nil //nolint:gosec // safe: e is always a small public exponent (65537)
}

// ---------------------------------------------------------------------------
// JWT verification
// ---------------------------------------------------------------------------

// jwtHeader holds the decoded JWT header fields we care about.
type jwtHeader struct {
	Alg string `json:"alg"`
	Kid string `json:"kid"`
}

// verifyJWT verifies an RS256 JWT against the JWKS cache and returns the raw
// claims map (the full payload).
func verifyJWT(token string, cache *jwksCache, issuer, audience string) (map[string]json.RawMessage, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, fmt.Errorf("jwt: malformed token (expected 3 parts, got %d)", len(parts))
	}

	headerJSON, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return nil, fmt.Errorf("jwt: decode header: %w", err)
	}
	payloadJSON, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, fmt.Errorf("jwt: decode payload: %w", err)
	}

	var hdr jwtHeader
	if err = json.Unmarshal(headerJSON, &hdr); err != nil {
		return nil, fmt.Errorf("jwt: parse header: %w", err)
	}
	if hdr.Alg != "RS256" {
		return nil, fmt.Errorf("jwt: unsupported algorithm %q (only RS256 accepted)", hdr.Alg)
	}

	pubKey, err := cache.Key(hdr.Kid)
	if err != nil {
		return nil, fmt.Errorf("jwt: fetch public key: %w", err)
	}

	// Verify signature.
	sig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return nil, fmt.Errorf("jwt: decode signature: %w", err)
	}
	if err = verifyRS256([]byte(parts[0]+"."+parts[1]), sig, pubKey); err != nil {
		return nil, fmt.Errorf("jwt: signature invalid: %w", err)
	}

	// Parse and validate standard claims.
	var payload struct {
		Iss string          `json:"iss"`
		Sub string          `json:"sub"`
		Aud json.RawMessage `json:"aud"`
		Exp int64           `json:"exp"`
		Nbf int64           `json:"nbf"`
	}
	if err = json.Unmarshal(payloadJSON, &payload); err != nil {
		return nil, fmt.Errorf("jwt: parse payload: %w", err)
	}

	if payload.Iss != issuer {
		return nil, fmt.Errorf("jwt: issuer mismatch (got %q, want %q)", payload.Iss, issuer)
	}
	now := time.Now().Unix()
	if now > payload.Exp {
		return nil, fmt.Errorf("jwt: token expired at %d", payload.Exp)
	}
	if payload.Nbf != 0 && now < payload.Nbf {
		return nil, fmt.Errorf("jwt: token not yet valid (nbf=%d, now=%d)", payload.Nbf, now)
	}
	if audience != "" {
		if err = checkAudience(payload.Aud, audience); err != nil {
			return nil, err
		}
	}

	// Return full claims map for claim extraction.
	var claims map[string]json.RawMessage
	if err = json.Unmarshal(payloadJSON, &claims); err != nil {
		return nil, fmt.Errorf("jwt: parse claims map: %w", err)
	}
	return claims, nil
}

// checkAudience validates that the "aud" claim contains the expected audience.
// The "aud" claim may be a JSON string or a JSON array of strings.
func checkAudience(aud json.RawMessage, want string) error {
	if len(aud) == 0 {
		return fmt.Errorf("jwt: audience claim missing")
	}

	// Try string first.
	var single string
	if err := json.Unmarshal(aud, &single); err == nil {
		if single == want {
			return nil
		}
		return fmt.Errorf("jwt: audience mismatch (got %q, want %q)", single, want)
	}

	// Try array.
	var multi []string
	if err := json.Unmarshal(aud, &multi); err != nil {
		return fmt.Errorf("jwt: parse audience claim: %w", err)
	}
	for _, a := range multi {
		if a == want {
			return nil
		}
	}
	return fmt.Errorf("jwt: audience %q not in token audience list", want)
}

// extractStringClaim extracts a single string value from a claims map.
func extractStringClaim(claims map[string]json.RawMessage, key string) (string, bool) {
	raw, ok := claims[key]
	if !ok {
		return "", false
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return "", false
	}
	return s, true
}

// extractStringSliceClaim extracts a []string from a claims map field.
// The field may be a JSON string or a JSON array of strings.
func extractStringSliceClaim(claims map[string]json.RawMessage, key string) []string {
	raw, ok := claims[key]
	if !ok {
		return nil
	}

	var single string
	if err := json.Unmarshal(raw, &single); err == nil {
		return []string{single}
	}

	var multi []string
	if err := json.Unmarshal(raw, &multi); err != nil {
		return nil
	}
	return multi
}

// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------

// Middleware holds the parsed configuration and key cache.
type Middleware struct {
	cfg   Config
	cache *jwksCache
	log   *slog.Logger
}

// New creates a Middleware from the given Config.  It validates the config and
// creates the JWKS cache but does NOT fetch keys eagerly — the first request
// triggers a cache warm-up.
func New(cfg Config) (*Middleware, error) {
	if !cfg.Disabled {
		if cfg.JWKSEndpoint == "" {
			return nil, fmt.Errorf("auth: JWKSEndpoint is required")
		}
		if cfg.Issuer == "" {
			return nil, fmt.Errorf("auth: Issuer is required")
		}
	}

	log := cfg.Logger
	if log == nil {
		log = slog.Default()
	}

	return &Middleware{
		cfg:   cfg,
		cache: newJWKSCache(cfg.JWKSEndpoint, log),
		log:   log,
	}, nil
}

// HTTPHandler wraps the given http.Handler and enforces JWT authentication.
// Unauthenticated or unauthorized requests receive a JSON 401 / 403 response.
// The /healthz and /readyz probes are explicitly exempted.
//
// On success, the request Context is enriched with TenantID, roles, and subject.
func (m *Middleware) HTTPHandler(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Liveness / readiness probes must not require auth.
		if r.URL.Path == "/healthz" || r.URL.Path == "/readyz" || r.URL.Path == "/metrics" {
			next.ServeHTTP(w, r)
			return
		}

		if m.cfg.Disabled {
			// Inject a synthetic "bypass" tenant for dev/test environments.
			ctx := withClaims(r.Context(), Claims{
				Subject:  "dev",
				TenantID: "dev",
				Roles:    []string{RoleAdmin},
			})
			next.ServeHTTP(w, r.WithContext(ctx))
			return
		}

		claims, err := m.extractAndVerify(r.Header.Get("Authorization"))
		if err != nil {
			m.log.Warn("auth: rejected request", "path", r.URL.Path, "error", err)
			writeJSONError(w, http.StatusUnauthorized, "invalid or missing token")
			return
		}

		next.ServeHTTP(w, r.WithContext(withClaims(r.Context(), claims)))
	})
}

// RequireRole returns an http.Handler wrapper that checks for at least one of
// the given roles.  It must be called after HTTPHandler has placed Claims in
// the context.
func (m *Middleware) RequireRole(roles ...string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			c, ok := ClaimsFromCtx(r.Context())
			if !ok {
				writeJSONError(w, http.StatusUnauthorized, "unauthenticated")
				return
			}
			if !c.HasRole(roles...) {
				writeJSONError(w, http.StatusForbidden,
					fmt.Sprintf("role required: %s", strings.Join(roles, " | ")))
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// extractAndVerify parses the Authorization header, verifies the JWT, and
// returns Claims.
func (m *Middleware) extractAndVerify(authHeader string) (Claims, error) {
	if authHeader == "" {
		return Claims{}, fmt.Errorf("missing Authorization header")
	}

	const prefix = "Bearer "
	if !strings.HasPrefix(authHeader, prefix) {
		return Claims{}, fmt.Errorf("Authorization header must be Bearer token")
	}
	token := strings.TrimPrefix(authHeader, prefix)

	rawClaims, err := verifyJWT(token, m.cache, m.cfg.Issuer, m.cfg.Audience)
	if err != nil {
		return Claims{}, err
	}

	// Extract tenant_id.
	tenantID, ok := extractStringClaim(rawClaims, m.cfg.tenantClaim())
	if !ok || tenantID == "" {
		return Claims{}, fmt.Errorf("jwt: claim %q is required and was not found", m.cfg.tenantClaim())
	}

	sub, _ := extractStringClaim(rawClaims, "sub")
	roles := extractStringSliceClaim(rawClaims, m.cfg.rolesClaim())

	return Claims{Subject: sub, TenantID: tenantID, Roles: roles}, nil
}

// withClaims stores Claims in a context.
func withClaims(ctx context.Context, c Claims) context.Context {
	ctx = context.WithValue(ctx, ctxTenantID, c.TenantID)
	ctx = context.WithValue(ctx, ctxRoles, c.Roles)
	ctx = context.WithValue(ctx, ctxSubject, c.Subject)
	return ctx
}

// ContextWithClaims returns a context enriched with the supplied Claims.
// Intended for use in unit tests that call gRPC handler methods directly
// (bypassing the auth interceptor) and need a valid tenant context.
func ContextWithClaims(ctx context.Context, c Claims) context.Context {
	return withClaims(ctx, c)
}

// writeJSONError writes a JSON error body.
func writeJSONError(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("WWW-Authenticate", `Bearer realm="vmafx"`)
	w.WriteHeader(code)
	body := fmt.Sprintf(`{"error":%q}`, msg)
	_, _ = w.Write([]byte(body))
}
