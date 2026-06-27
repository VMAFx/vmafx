// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/middleware_test.go — unit + integration tests for
// the JWT auth middleware.
//
// Test structure
// ==============
//  - TestJWKSParseAndVerify  : JWKS JSON parse + RSA key round-trip.
//  - TestVerifyJWT_Valid      : end-to-end sign + verify with a local test key.
//  - TestVerifyJWT_Expired    : expired token rejected.
//  - TestVerifyJWT_WrongIssuer: wrong issuer rejected.
//  - TestVerifyJWT_WrongAlg   : non-RS256 alg header rejected.
//  - TestHTTPMiddleware_*     : HTTP handler tests with a fake JWKS server.
//  - TestTenantIsolation      : tenant_id mismatch → 403.
//  - TestRBACRoles            : role enforcement.
//  - TestDisabledMiddleware   : bypass mode injects dev tenant.
//  - TestGRPCInterceptor      : gRPC unary interceptor with metadata.
//
// ADR-0794: multi-tenant auth gateway.

package auth_test

import (
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math/big"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/metadata"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
)

// ---------------------------------------------------------------------------
// Helpers — fake JWT issuer
// ---------------------------------------------------------------------------

// fakeIssuer wraps an RSA key pair and a test JWKS HTTP server.
type fakeIssuer struct {
	priv   *rsa.PrivateKey
	pub    *rsa.PublicKey
	server *httptest.Server
	kid    string
}

func newFakeIssuer(t *testing.T) *fakeIssuer {
	t.Helper()
	priv, pub, err := auth.GenerateTestKeyPair()
	if err != nil {
		t.Fatalf("generate key pair: %v", err)
	}

	fi := &fakeIssuer{priv: priv, pub: pub, kid: "test-key-1"}

	// Build the JWKS JSON for the public key.
	nBytes := pub.N.Bytes()
	eBytes := big.NewInt(int64(pub.E)).Bytes()
	nB64 := base64.RawURLEncoding.EncodeToString(nBytes)
	eB64 := base64.RawURLEncoding.EncodeToString(eBytes)

	jwks := fmt.Sprintf(`{"keys":[{"kty":"RSA","kid":%q,"n":%q,"e":%q}]}`,
		fi.kid, nB64, eB64)

	fi.server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(jwks))
	}))

	t.Cleanup(fi.server.Close)
	return fi
}

// MakeToken builds and signs a JWT.  opts lets callers override individual fields.
func (fi *fakeIssuer) MakeToken(t *testing.T, opts tokenOpts) string {
	t.Helper()

	if opts.issuer == "" {
		opts.issuer = fi.server.URL
	}
	if opts.exp == 0 {
		opts.exp = time.Now().Add(time.Hour).Unix()
	}
	if opts.tenantID == "" {
		opts.tenantID = "acme"
	}
	if opts.roles == nil {
		opts.roles = []string{auth.RoleWriter}
	}
	if opts.alg == "" {
		opts.alg = "RS256"
	}
	if opts.kid == "" {
		opts.kid = fi.kid
	}

	hdr := map[string]string{"alg": opts.alg, "typ": "JWT", "kid": opts.kid}
	hdrJSON, _ := json.Marshal(hdr)
	hdrB64 := base64.RawURLEncoding.EncodeToString(hdrJSON)

	payload := map[string]any{
		"iss":         opts.issuer,
		"sub":         "user123",
		"tid":         opts.tenantID,
		"vmafx_roles": opts.roles,
		"exp":         opts.exp,
	}
	if opts.audience != "" {
		payload["aud"] = opts.audience
	}
	if opts.nbf != 0 {
		payload["nbf"] = opts.nbf
	}
	payloadJSON, _ := json.Marshal(payload)
	payloadB64 := base64.RawURLEncoding.EncodeToString(payloadJSON)

	sigInput := hdrB64 + "." + payloadB64
	h := sha256.Sum256([]byte(sigInput))
	sig, err := rsa.SignPKCS1v15(rand.Reader, fi.priv, crypto.SHA256, h[:])
	if err != nil {
		t.Fatalf("sign JWT: %v", err)
	}
	sigB64 := base64.RawURLEncoding.EncodeToString(sig)

	return sigInput + "." + sigB64
}

type tokenOpts struct {
	issuer   string
	audience string
	tenantID string
	roles    []string
	exp      int64
	nbf      int64 // 0 means omit the claim
	alg      string
	kid      string
}

// ---------------------------------------------------------------------------
// Middleware factory helper
// ---------------------------------------------------------------------------

func newTestMiddleware(t *testing.T, fi *fakeIssuer, extra ...func(*auth.Config)) *auth.Middleware {
	t.Helper()
	cfg := auth.Config{
		JWKSEndpoint: fi.server.URL,
		Issuer:       fi.server.URL,
	}
	for _, fn := range extra {
		fn(&cfg)
	}
	mw, err := auth.New(cfg)
	if err != nil {
		t.Fatalf("auth.New: %v", err)
	}
	return mw
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

func TestVerifyJWT_Valid(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	token := fi.MakeToken(t, tokenOpts{})
	req := httptest.NewRequest(http.MethodGet, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	var gotClaims auth.Claims
	handler := mw.HTTPHandler(http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		c, ok := auth.ClaimsFromCtx(r.Context())
		if !ok {
			t.Error("claims not in context")
			return
		}
		gotClaims = c
	}))

	rr := httptest.NewRecorder()
	handler.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d; body=%s", rr.Code, rr.Body)
	}
	if gotClaims.TenantID != "acme" {
		t.Errorf("tenant_id: got %q, want %q", gotClaims.TenantID, "acme")
	}
	if !gotClaims.HasRole(auth.RoleWriter) {
		t.Errorf("expected vmafx:writer role")
	}
}

func TestVerifyJWT_Expired(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	token := fi.MakeToken(t, tokenOpts{exp: time.Now().Add(-time.Hour).Unix()})
	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rr.Code)
	}
}

func TestVerifyJWT_WrongIssuer(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	token := fi.MakeToken(t, tokenOpts{issuer: "https://evil.example.com"})
	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rr.Code)
	}
}

func TestVerifyJWT_WrongAlg(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	// Build a token with alg=HS256 — must be rejected before key lookup.
	token := fi.MakeToken(t, tokenOpts{alg: "HS256"})
	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rr.Code)
	}
}

// jwkJSON renders a single RSA public key as a JWKS entry object.
func jwkJSON(t *testing.T, kid string, pub *rsa.PublicKey) string {
	t.Helper()
	nB64 := base64.RawURLEncoding.EncodeToString(pub.N.Bytes())
	eB64 := base64.RawURLEncoding.EncodeToString(big.NewInt(int64(pub.E)).Bytes())
	return fmt.Sprintf(`{"kty":"RSA","kid":%q,"n":%q,"e":%q}`, kid, nB64, eB64)
}

// TestVerifyJWT_KidInJWKSTail is the round-3 R3-13 regression. A JWKS that
// publishes more than jwksCacheMax (16) keys must NOT drop a key by document
// position: a token whose kid lands past the 16th entry has to authenticate.
// Before the fix, refresh() did `keys = keys[:jwksCacheMax]`, evicting the
// trailing signing key, so the token 401'd until the 30s cooldown elapsed.
func TestVerifyJWT_KidInJWKSTail(t *testing.T) {
	// The real signing key whose kid will sit at the END of the JWKS list.
	priv, pub, err := auth.GenerateTestKeyPair()
	if err != nil {
		t.Fatalf("generate key pair: %v", err)
	}
	const realKid = "real-signing-key"

	// Build a JWKS with 20 decoy keys first, then the real key last — so the
	// real kid is at index 20 (>= jwksCacheMax=16) in document order.
	entries := make([]string, 0, 21)
	for i := range 20 {
		_, decoyPub, derr := auth.GenerateTestKeyPair()
		if derr != nil {
			t.Fatalf("generate decoy key %d: %v", i, derr)
		}
		entries = append(entries, jwkJSON(t, fmt.Sprintf("decoy-%d", i), decoyPub))
	}
	entries = append(entries, jwkJSON(t, realKid, pub))
	jwks := `{"keys":[` + strings.Join(entries, ",") + `]}`

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(jwks))
	}))
	t.Cleanup(srv.Close)

	fi := &fakeIssuer{priv: priv, pub: pub, server: srv, kid: realKid}
	mw := newTestMiddleware(t, fi)

	token := fi.MakeToken(t, tokenOpts{kid: realKid})
	req := httptest.NewRequest(http.MethodGet, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("token with kid in JWKS tail must authenticate: got %d, body=%s",
			rr.Code, rr.Body)
	}
}

func TestVerifyJWT_MissingTenantClaim(t *testing.T) {
	fi := newFakeIssuer(t)
	// Use a non-default tenant claim that the token does NOT have.
	mw := newTestMiddleware(t, fi, func(c *auth.Config) {
		c.TenantClaim = "org_id"
	})

	// Token uses "tid" (default), but middleware expects "org_id".
	token := fi.MakeToken(t, tokenOpts{})
	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d; body=%s", rr.Code, rr.Body)
	}
}

func TestHTTPMiddleware_ProbeExemption(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	// /healthz must pass without any Authorization header.
	for _, path := range []string{"/healthz", "/readyz", "/metrics"} {
		req := httptest.NewRequest(http.MethodGet, path, nil)
		rr := httptest.NewRecorder()
		mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusOK)
		})).ServeHTTP(rr, req)

		if rr.Code != http.StatusOK {
			t.Errorf("%s: expected 200, got %d", path, rr.Code)
		}
	}
}

func TestTenantIsolation(t *testing.T) {
	fi := newFakeIssuer(t)
	// Token belongs to tenant "acme"; handler checks against "rival-corp".
	token := fi.MakeToken(t, tokenOpts{tenantID: "acme"})
	mw := newTestMiddleware(t, fi)

	req := httptest.NewRequest(http.MethodGet, "/v1/job/123", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	handler := mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Simulate a handler that checks tenant ownership.
		err := auth.AssertHTTPTenantOwns(r.Context(), "rival-corp")
		if err != nil {
			http.Error(w, err.Error(), http.StatusForbidden)
			return
		}
		w.WriteHeader(http.StatusOK)
	}))
	handler.ServeHTTP(rr, req)

	if rr.Code != http.StatusForbidden {
		t.Fatalf("expected 403, got %d", rr.Code)
	}
}

func TestRBACRoles_ReaderDeniedWrite(t *testing.T) {
	fi := newFakeIssuer(t)
	token := fi.MakeToken(t, tokenOpts{roles: []string{auth.RoleReader}})
	mw := newTestMiddleware(t, fi)

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	requireWriter := mw.RequireRole(auth.RoleWriter, auth.RoleAdmin)
	rr := httptest.NewRecorder()
	mw.HTTPHandler(requireWriter(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))).ServeHTTP(rr, req)

	if rr.Code != http.StatusForbidden {
		t.Fatalf("expected 403, got %d", rr.Code)
	}
}

func TestRBACRoles_AdminAllowed(t *testing.T) {
	fi := newFakeIssuer(t)
	token := fi.MakeToken(t, tokenOpts{roles: []string{auth.RoleAdmin}})
	mw := newTestMiddleware(t, fi)

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	requireWriter := mw.RequireRole(auth.RoleWriter, auth.RoleAdmin)
	rr := httptest.NewRecorder()
	mw.HTTPHandler(requireWriter(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))).ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d; body=%s", rr.Code, rr.Body)
	}
}

func TestDisabledMiddleware(t *testing.T) {
	mw, err := auth.New(auth.Config{Disabled: true})
	if err != nil {
		t.Fatalf("auth.New disabled: %v", err)
	}

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	// No Authorization header.

	var gotTenantID string
	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		gotTenantID = auth.TenantIDFromCtx(r.Context())
	})).ServeHTTP(rr, req)

	if gotTenantID != "dev" {
		t.Errorf("disabled mode: expected tenant 'dev', got %q", gotTenantID)
	}
}

func TestGRPCInterceptor_Valid(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	token := fi.MakeToken(t, tokenOpts{})
	md := metadata.Pairs("authorization", "Bearer "+token)
	ctx := metadata.NewIncomingContext(context.Background(), md)

	interceptor := mw.GRPCUnaryInterceptor()
	var gotTenantID string

	_, err := interceptor(ctx, nil, nil,
		func(ctx context.Context, _ any) (any, error) {
			gotTenantID = auth.TenantIDFromCtx(ctx)
			return nil, nil
		})
	if err != nil {
		t.Fatalf("interceptor error: %v", err)
	}
	if gotTenantID != "acme" {
		t.Errorf("gRPC tenant_id: got %q, want %q", gotTenantID, "acme")
	}
}

func TestGRPCInterceptor_MissingMetadata(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	interceptor := mw.GRPCUnaryInterceptor()
	_, err := interceptor(context.Background(), nil, nil,
		func(_ context.Context, _ any) (any, error) {
			return nil, nil
		})

	if err == nil {
		t.Fatal("expected error for missing metadata, got nil")
	}
	if !strings.Contains(err.Error(), "Unauthenticated") {
		t.Errorf("expected Unauthenticated error, got: %v", err)
	}
}

func TestAudienceValidation(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi, func(c *auth.Config) {
		c.Audience = "vmafx-api"
	})

	// Token without audience — should fail.
	tokenNoAud := fi.MakeToken(t, tokenOpts{})
	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+tokenNoAud)
	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)
	if rr.Code != http.StatusUnauthorized {
		t.Errorf("no-aud token: expected 401, got %d", rr.Code)
	}

	// Token with correct audience — should pass.
	tokenWithAud := fi.MakeToken(t, tokenOpts{audience: "vmafx-api"})
	req2 := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req2.Header.Set("Authorization", "Bearer "+tokenWithAud)
	rr2 := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr2, req2)
	if rr2.Code != http.StatusOK {
		t.Errorf("correct-aud token: expected 200, got %d; body=%s", rr2.Code, rr2.Body)
	}
}

// TestVerifyToken_NbfRejected verifies that a token whose "nbf" (not-before)
// claim is set to a future time is rejected with 401 even if the signature
// and all other claims are valid.
func TestVerifyToken_NbfRejected(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	// nbf = now + 1 hour — token is not yet valid.
	futureNbf := time.Now().Add(time.Hour).Unix()
	token := fi.MakeToken(t, tokenOpts{nbf: futureNbf})

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 for future-nbf token, got %d; body=%s", rr.Code, rr.Body)
	}
}

// TestVerifyToken_NbfInPastAccepted verifies that a token whose "nbf" claim
// is in the past is accepted normally (past nbf is not a rejection reason).
func TestVerifyToken_NbfInPastAccepted(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	// nbf = now - 1 hour — token is already valid.
	pastNbf := time.Now().Add(-time.Hour).Unix()
	token := fi.MakeToken(t, tokenOpts{nbf: pastNbf})

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200 for past-nbf token, got %d; body=%s", rr.Code, rr.Body)
	}
}
