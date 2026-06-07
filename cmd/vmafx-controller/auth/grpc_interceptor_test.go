// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/grpc_interceptor_test.go — coverage for the gRPC
// interceptors and helpers not reached by middleware_test.go.
//
// Covers:
//   - GRPCUnaryInterceptor in Disabled mode (bypass dev-tenant injection).
//   - GRPCStreamInterceptor happy path (valid token, context enriched).
//   - GRPCStreamInterceptor missing metadata → Unauthenticated.
//   - GRPCStreamInterceptor missing Bearer prefix → Unauthenticated.
//   - GRPCStreamInterceptor in Disabled mode (dev-tenant injected).
//   - RequireGRPCRole — allowed when role matches.
//   - RequireGRPCRole — denied when role absent.
//   - RequireGRPCRole — denied when no claims in context.
//   - AssertTenantOwns — owner match → nil.
//   - AssertTenantOwns — owner mismatch → PermissionDenied.
//   - AssertTenantOwns — empty tenant_id in context → Unauthenticated.
//   - AssertHTTPTenantOwns — owner mismatch → error.
//   - AssertHTTPTenantOwns — empty tenant_id → error.
//   - MarshalPublicKeyPEM round-trip.
//   - checkAudience with array audience (via middleware JWT path).
//
// ADR-0794: multi-tenant auth gateway.

package auth_test

import (
	"context"
	"crypto"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
)

// ---------------------------------------------------------------------------
// GRPCUnaryInterceptor — Disabled mode
// ---------------------------------------------------------------------------

// TestGRPCUnaryInterceptor_Disabled verifies that the unary interceptor in
// Disabled mode injects the synthetic "dev" tenant without checking tokens.
func TestGRPCUnaryInterceptor_Disabled(t *testing.T) {
	mw, err := auth.New(auth.Config{Disabled: true})
	if err != nil {
		t.Fatalf("auth.New: %v", err)
	}

	interceptor := mw.GRPCUnaryInterceptor()
	var gotTenantID string

	// No metadata at all — Disabled mode must not care.
	_, grpcErr := interceptor(context.Background(), nil, nil,
		func(ctx context.Context, _ any) (any, error) {
			gotTenantID = auth.TenantIDFromCtx(ctx)
			return nil, nil
		})
	if grpcErr != nil {
		t.Fatalf("Disabled mode interceptor error: %v", grpcErr)
	}
	if gotTenantID != "dev" {
		t.Errorf("Disabled mode: tenant_id = %q, want %q", gotTenantID, "dev")
	}
}

// ---------------------------------------------------------------------------
// GRPCStreamInterceptor
// ---------------------------------------------------------------------------

// fakeServerStream is a minimal grpc.ServerStream stub for use in stream
// interceptor tests.
type fakeServerStream struct {
	grpc.ServerStream
	ctx context.Context
}

func (f *fakeServerStream) Context() context.Context { return f.ctx }

// TestGRPCStreamInterceptor_Valid verifies that the stream interceptor enriches
// the context with the claims from a valid Bearer token.
func TestGRPCStreamInterceptor_Valid(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	token := fi.MakeToken(t, tokenOpts{})
	md := metadata.Pairs("authorization", "Bearer "+token)
	ctx := metadata.NewIncomingContext(context.Background(), md)
	stream := &fakeServerStream{ctx: ctx}

	interceptor := mw.GRPCStreamInterceptor()
	var gotTenantID string

	err := interceptor(nil, stream, &grpc.StreamServerInfo{FullMethod: "/test.Service/TestMethod"},
		func(_ any, ss grpc.ServerStream) error {
			gotTenantID = auth.TenantIDFromCtx(ss.Context())
			return nil
		})
	if err != nil {
		t.Fatalf("GRPCStreamInterceptor error: %v", err)
	}
	if gotTenantID != "acme" {
		t.Errorf("tenant_id: got %q, want %q", gotTenantID, "acme")
	}
}

// TestGRPCStreamInterceptor_MissingMetadata verifies that the stream interceptor
// rejects a call with no incoming metadata.
func TestGRPCStreamInterceptor_MissingMetadata(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	stream := &fakeServerStream{ctx: context.Background()}
	interceptor := mw.GRPCStreamInterceptor()

	err := interceptor(nil, stream, &grpc.StreamServerInfo{FullMethod: "/test.Service/TestMethod"},
		func(_ any, _ grpc.ServerStream) error { return nil })
	if err == nil {
		t.Fatal("expected Unauthenticated error, got nil")
	}
	st, _ := status.FromError(err)
	if st.Code() != codes.Unauthenticated {
		t.Errorf("expected Unauthenticated, got %v", st.Code())
	}
}

// TestGRPCStreamInterceptor_MissingBearerPrefix verifies that a metadata
// "authorization" value without the Bearer prefix is rejected.
func TestGRPCStreamInterceptor_MissingBearerPrefix(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi)

	md := metadata.Pairs("authorization", "Basic dXNlcjpwYXNz")
	ctx := metadata.NewIncomingContext(context.Background(), md)
	stream := &fakeServerStream{ctx: ctx}
	interceptor := mw.GRPCStreamInterceptor()

	err := interceptor(nil, stream, &grpc.StreamServerInfo{FullMethod: "/test.Service/TestMethod"},
		func(_ any, _ grpc.ServerStream) error { return nil })
	if err == nil {
		t.Fatal("expected Unauthenticated error, got nil")
	}
	st, _ := status.FromError(err)
	if st.Code() != codes.Unauthenticated {
		t.Errorf("expected Unauthenticated, got %v", st.Code())
	}
}

// TestGRPCStreamInterceptor_Disabled verifies that Disabled mode injects the
// dev tenant into the stream context without requiring any token.
func TestGRPCStreamInterceptor_Disabled(t *testing.T) {
	mw, err := auth.New(auth.Config{Disabled: true})
	if err != nil {
		t.Fatalf("auth.New: %v", err)
	}

	stream := &fakeServerStream{ctx: context.Background()}
	interceptor := mw.GRPCStreamInterceptor()
	var gotTenantID string

	grpcErr := interceptor(nil, stream, &grpc.StreamServerInfo{FullMethod: "/test.Service/TestMethod"},
		func(_ any, ss grpc.ServerStream) error {
			gotTenantID = auth.TenantIDFromCtx(ss.Context())
			return nil
		})
	if grpcErr != nil {
		t.Fatalf("Disabled stream interceptor error: %v", grpcErr)
	}
	if gotTenantID != "dev" {
		t.Errorf("Disabled mode: tenant_id = %q, want %q", gotTenantID, "dev")
	}
}

// ---------------------------------------------------------------------------
// RequireGRPCRole
// ---------------------------------------------------------------------------

// TestRequireGRPCRole_Allowed verifies that a caller who holds the required
// role passes through to the handler.
func TestRequireGRPCRole_Allowed(t *testing.T) {
	ctx := auth.ContextWithClaims(context.Background(), auth.Claims{
		Subject:  "user1",
		TenantID: "acme",
		Roles:    []string{auth.RoleWriter},
	})

	interceptor := auth.RequireGRPCRole(auth.RoleWriter, auth.RoleAdmin)
	var called bool
	_, err := interceptor(ctx, nil, nil,
		func(_ context.Context, _ any) (any, error) {
			called = true
			return nil, nil
		})
	if err != nil {
		t.Fatalf("RequireGRPCRole: unexpected error: %v", err)
	}
	if !called {
		t.Error("handler was not called")
	}
}

// TestRequireGRPCRole_Denied verifies that a caller who lacks the required role
// receives PermissionDenied.
func TestRequireGRPCRole_Denied(t *testing.T) {
	ctx := auth.ContextWithClaims(context.Background(), auth.Claims{
		Subject:  "user1",
		TenantID: "acme",
		Roles:    []string{auth.RoleReader}, // reader only
	})

	interceptor := auth.RequireGRPCRole(auth.RoleWriter, auth.RoleAdmin)
	_, err := interceptor(ctx, nil, nil,
		func(_ context.Context, _ any) (any, error) { return nil, nil })
	if err == nil {
		t.Fatal("expected PermissionDenied, got nil")
	}
	st, _ := status.FromError(err)
	if st.Code() != codes.PermissionDenied {
		t.Errorf("expected PermissionDenied, got %v", st.Code())
	}
}

// TestRequireGRPCRole_NoClaims verifies that a context with no claims (e.g.
// the auth interceptor was bypassed) receives Unauthenticated.
func TestRequireGRPCRole_NoClaims(t *testing.T) {
	interceptor := auth.RequireGRPCRole(auth.RoleWriter)
	_, err := interceptor(context.Background(), nil, nil,
		func(_ context.Context, _ any) (any, error) { return nil, nil })
	if err == nil {
		t.Fatal("expected Unauthenticated, got nil")
	}
	st, _ := status.FromError(err)
	if st.Code() != codes.Unauthenticated {
		t.Errorf("expected Unauthenticated, got %v", st.Code())
	}
}

// ---------------------------------------------------------------------------
// AssertTenantOwns (gRPC)
// ---------------------------------------------------------------------------

// TestAssertTenantOwns_Match verifies that AssertTenantOwns returns nil when
// the caller's tenant_id matches the resource tenant.
func TestAssertTenantOwns_Match(t *testing.T) {
	ctx := auth.ContextWithClaims(context.Background(), auth.Claims{
		TenantID: "acme",
		Roles:    []string{auth.RoleWriter},
	})
	if err := auth.AssertTenantOwns(ctx, "acme"); err != nil {
		t.Errorf("expected nil for matching tenant, got: %v", err)
	}
}

// TestAssertTenantOwns_Mismatch verifies PermissionDenied when tenants differ.
func TestAssertTenantOwns_Mismatch(t *testing.T) {
	ctx := auth.ContextWithClaims(context.Background(), auth.Claims{
		TenantID: "acme",
		Roles:    []string{auth.RoleWriter},
	})
	err := auth.AssertTenantOwns(ctx, "rival-corp")
	if err == nil {
		t.Fatal("expected PermissionDenied, got nil")
	}
	st, _ := status.FromError(err)
	if st.Code() != codes.PermissionDenied {
		t.Errorf("expected PermissionDenied, got %v", st.Code())
	}
}

// TestAssertTenantOwns_EmptyTenantID verifies Unauthenticated when the context
// has no tenant_id (unauthenticated call or missing claim).
func TestAssertTenantOwns_EmptyTenantID(t *testing.T) {
	err := auth.AssertTenantOwns(context.Background(), "acme")
	if err == nil {
		t.Fatal("expected Unauthenticated, got nil")
	}
	st, _ := status.FromError(err)
	if st.Code() != codes.Unauthenticated {
		t.Errorf("expected Unauthenticated, got %v", st.Code())
	}
}

// ---------------------------------------------------------------------------
// AssertHTTPTenantOwns
// ---------------------------------------------------------------------------

// TestAssertHTTPTenantOwns_Mismatch verifies that AssertHTTPTenantOwns returns
// a non-nil error when the caller's tenant differs from the resource tenant.
func TestAssertHTTPTenantOwns_Mismatch(t *testing.T) {
	ctx := auth.ContextWithClaims(context.Background(), auth.Claims{
		TenantID: "acme",
		Roles:    []string{auth.RoleWriter},
	})
	err := auth.AssertHTTPTenantOwns(ctx, "rival-corp")
	if err == nil {
		t.Fatal("expected error for tenant mismatch, got nil")
	}
	if !strings.Contains(err.Error(), "rival-corp") {
		t.Errorf("error should mention resource tenant, got: %v", err)
	}
}

// TestAssertHTTPTenantOwns_EmptyTenantID verifies error when context carries
// no tenant_id.
func TestAssertHTTPTenantOwns_EmptyTenantID(t *testing.T) {
	err := auth.AssertHTTPTenantOwns(context.Background(), "acme")
	if err == nil {
		t.Fatal("expected error for missing tenant_id, got nil")
	}
}

// ---------------------------------------------------------------------------
// MarshalPublicKeyPEM
// ---------------------------------------------------------------------------

// TestMarshalPublicKeyPEM_RoundTrip verifies that MarshalPublicKeyPEM produces
// a valid PEM block for a freshly generated test key pair.
func TestMarshalPublicKeyPEM_RoundTrip(t *testing.T) {
	_, pub, err := auth.GenerateTestKeyPair()
	if err != nil {
		t.Fatalf("GenerateTestKeyPair: %v", err)
	}
	pemBytes, err := auth.MarshalPublicKeyPEM(pub)
	if err != nil {
		t.Fatalf("MarshalPublicKeyPEM: %v", err)
	}
	if len(pemBytes) == 0 {
		t.Error("MarshalPublicKeyPEM returned empty PEM block")
	}
	if !strings.Contains(string(pemBytes), "BEGIN PUBLIC KEY") {
		t.Errorf("PEM block does not contain expected header; got: %s", pemBytes)
	}
}

// ---------------------------------------------------------------------------
// checkAudience with array audience (via middleware JWT path)
// ---------------------------------------------------------------------------

// makeArrayAudToken builds a JWT whose "aud" claim is a JSON array.
// This exercises the multi-value branch in checkAudience which is skipped by
// MakeToken (which only emits a scalar string for the "aud" field).
func makeArrayAudToken(t *testing.T, fi *fakeIssuer, audiences []string, issuer string) string {
	t.Helper()

	hdr := map[string]string{"alg": "RS256", "typ": "JWT", "kid": fi.kid}
	hdrJSON, _ := json.Marshal(hdr)
	hdrB64 := base64.RawURLEncoding.EncodeToString(hdrJSON)

	payload := map[string]any{
		"iss":         issuer,
		"sub":         "user123",
		"tid":         "acme",
		"vmafx_roles": []string{auth.RoleWriter},
		"exp":         time.Now().Add(time.Hour).Unix(),
		"aud":         audiences, // array form
	}
	payloadJSON, _ := json.Marshal(payload)
	payloadB64 := base64.RawURLEncoding.EncodeToString(payloadJSON)

	sigInput := hdrB64 + "." + payloadB64
	h := sha256.Sum256([]byte(sigInput))
	sig, err := fi.priv.Sign(rand.Reader, h[:], crypto.SHA256)
	if err != nil {
		t.Fatalf("sign token: %v", err)
	}
	return sigInput + "." + base64.RawURLEncoding.EncodeToString(sig)
}

// TestAudienceValidation_ArrayAudience verifies that a token whose "aud" claim
// is a JSON array passes validation when the expected audience is in the array.
func TestAudienceValidation_ArrayAudience(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi, func(c *auth.Config) {
		c.Audience = "vmafx-api"
	})

	// Build a token with aud as an array that includes the expected value.
	token := makeArrayAudToken(t, fi, []string{"other-service", "vmafx-api"}, fi.server.URL)

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Errorf("array-aud token: expected 200, got %d; body=%s", rr.Code, rr.Body)
	}
}

// TestAudienceValidation_ArrayAudience_NotPresent verifies that a token whose
// "aud" array does not contain the expected audience is rejected.
func TestAudienceValidation_ArrayAudience_NotPresent(t *testing.T) {
	fi := newFakeIssuer(t)
	mw := newTestMiddleware(t, fi, func(c *auth.Config) {
		c.Audience = "vmafx-api"
	})

	// Build a token with aud as an array that does NOT include the expected value.
	token := makeArrayAudToken(t, fi, []string{"other-service", "another-service"}, fi.server.URL)

	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Errorf("array-aud not present: expected 401, got %d; body=%s", rr.Code, rr.Body)
	}
}
