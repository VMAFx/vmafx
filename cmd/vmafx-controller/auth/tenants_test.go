// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/tenants_test.go — the tenant registry (ADR-1519).
//
// Covers:
//   - spec validation and CRD defaults: every invalid field refused with the
//     tenant and source named; Load is all-or-nothing, Reload drops only the
//     broken tenants (and every copy of a duplicated tenantId);
//   - resolution: each tenant's own issuer, JWKS, audience and claim names;
//     a token one tenant's provider minted for another tenant refused; an
//     unknown issuer refused; an ambiguous token refused;
//   - suspension (enabled=false) refused with PermissionDenied / 403;
//   - allowedRoles strips roles, defaultRole fills a token without roles;
//   - a stale tenant set refuses every token (Unavailable / 503);
//   - the middleware refuses settings a tenant registry would ignore.

package auth_test

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth/authtest"
)

const (
	readMethod  = "/test.Service/Read"
	adminMethod = "/test.Service/Admin"
	nodeMethod  = "/test.Service/Node"
)

// tierPolicy grants readMethod to every user role, writeMethod to writers and
// admins, adminMethod to admins and nodeMethod to the node role only.
func tierPolicy() auth.MethodRoles {
	return auth.MethodRoles{
		readMethod:  {auth.RoleReader, auth.RoleWriter, auth.RoleAdmin},
		writeMethod: {auth.RoleWriter, auth.RoleAdmin},
		adminMethod: {auth.RoleAdmin},
		nodeMethod:  {auth.RoleNode},
	}
}

//go:fix inline
func boolPtr(b bool) *bool { return new(b) }

// tenantSpec returns a valid spec of id trusting iss.
func tenantSpec(id string, iss *authtest.Issuer) auth.NamedTenantSpec {
	return auth.NamedTenantSpec{Source: "test/" + id, Spec: auth.TenantSpec{
		TenantID: id,
		OIDC:     auth.TenantOIDC{Issuer: iss.URL(), JWKSEndpoint: iss.URL()},
	}}
}

// newRegistry returns a registry loaded with specs.
func newRegistry(t *testing.T, specs ...auth.NamedTenantSpec) *auth.TenantRegistry {
	t.Helper()
	reg, err := auth.NewTenantRegistry(time.Minute, nil)
	if err != nil {
		t.Fatalf("NewTenantRegistry: %v", err)
	}
	if err := reg.Load(specs); err != nil {
		t.Fatalf("Load: %v", err)
	}
	return reg
}

// tenantMiddleware returns a Middleware over reg with tierPolicy.
func tenantMiddleware(t *testing.T, reg *auth.TenantRegistry) *auth.Middleware {
	t.Helper()
	mw, err := auth.New(auth.Config{Tenants: reg, MethodRoles: tierPolicy()})
	if err != nil {
		t.Fatalf("auth.New: %v", err)
	}
	return mw
}

// admitted calls method with token and returns the tenant the handler saw.
func admitted(mw *auth.Middleware, token, method string) (string, error) {
	tenant := ""
	_, err := mw.GRPCUnaryInterceptor()(bearerCtx(token), nil, testInfo(method),
		func(ctx context.Context, _ any) (any, error) {
			tenant = auth.TenantIDFromCtx(ctx)
			return nil, nil
		})
	return tenant, err
}

func TestTenantSpecValidation(t *testing.T) {
	iss := authtest.NewIssuer(t)
	mutate := map[string]func(*auth.TenantSpec){
		"upper-case tenantId":      func(s *auth.TenantSpec) { s.TenantID = "Acme" },
		"one-character tenantId":   func(s *auth.TenantSpec) { s.TenantID = "a" },
		"leading dash":             func(s *auth.TenantSpec) { s.TenantID = "-acme" },
		"issuer not a URL":         func(s *auth.TenantSpec) { s.OIDC.Issuer = "acme" },
		"issuer ftp":               func(s *auth.TenantSpec) { s.OIDC.Issuer = "ftp://idp.example.com" },
		"jwks missing":             func(s *auth.TenantSpec) { s.OIDC.JWKSEndpoint = "" },
		"jwks plain http off-host": func(s *auth.TenantSpec) { s.OIDC.JWKSEndpoint = "http://idp.example.com/keys" },
		"allowedRoles empty":       func(s *auth.TenantSpec) { s.RBAC = &auth.TenantRBAC{AllowedRoles: []string{}} },
		"unknown allowed role":     func(s *auth.TenantSpec) { s.RBAC = &auth.TenantRBAC{AllowedRoles: []string{"vmafx:root"}} },
		"unknown default role":     func(s *auth.TenantSpec) { s.RBAC = &auth.TenantRBAC{DefaultRole: "admin"} },
		"default role not allowed": func(s *auth.TenantSpec) { s.RBAC = &auth.TenantRBAC{DefaultRole: auth.RoleAdmin} },
		"default outside explicit": func(s *auth.TenantSpec) { s.RBAC = &auth.TenantRBAC{AllowedRoles: []string{auth.RoleWriter}} },
		"admin default not in admin": func(s *auth.TenantSpec) {
			s.RBAC = &auth.TenantRBAC{DefaultRole: auth.RoleWriter, AllowedRoles: []string{auth.RoleAdmin}}
		},
		"node as default role": func(s *auth.TenantSpec) {
			s.RBAC = &auth.TenantRBAC{DefaultRole: auth.RoleNode, AllowedRoles: []string{auth.RoleNode, auth.RoleReader}}
		},
	}
	for name, fn := range mutate {
		ns := tenantSpec("acme", iss)
		fn(&ns.Spec)
		reg, _ := auth.NewTenantRegistry(time.Minute, nil)
		err := reg.Load([]auth.NamedTenantSpec{ns})
		if err == nil || !strings.Contains(err.Error(), "test/acme") {
			t.Errorf("%s: Load error = %v, want a refusal naming the source", name, err)
		}
	}
	good := tenantSpec("acme", iss)
	good.Spec.OIDC.JWKSEndpoint = "https://idp.example.com/.well-known/jwks.json"
	if err := newRegistry(t).Load([]auth.NamedTenantSpec{good}); err != nil {
		t.Errorf("https JWKS endpoint refused: %v", err)
	}
}

func TestLoadIsAllOrNothingAndReloadDropsOnlyBrokenTenants(t *testing.T) {
	iss := authtest.NewIssuer(t)
	reg := newRegistry(t, tenantSpec("acme", iss))
	bad := tenantSpec("broken", iss)
	bad.Spec.RBAC = &auth.TenantRBAC{AllowedRoles: []string{"vmafx:root"}}
	dupA, dupB := tenantSpec("twin", iss), tenantSpec("twin", iss)
	dupB.Source = "test/twin-copy"
	specs := []auth.NamedTenantSpec{tenantSpec("rival", iss), bad, dupA, dupB}

	if err := reg.Load(specs); err == nil {
		t.Fatal("Load accepted a set with an invalid and a duplicated tenant")
	}
	if reg.Count() != 1 {
		t.Fatalf("after a refused Load the registry holds %d tenants, want the previous 1", reg.Count())
	}
	errs := reg.Reload(specs)
	if len(errs) != 3 || reg.Count() != 1 {
		t.Fatalf("Reload: %d errors, %d tenants; want 3 errors (broken, twin x2) and only rival", len(errs), reg.Count())
	}
	mw := tenantMiddleware(t, reg)
	for id, wantOK := range map[string]bool{"rival": true, "acme": false, "twin": false, "broken": false} {
		tok := iss.Token(t, map[string]any{"tid": id})
		_, err := admitted(mw, tok, readMethod)
		if (err == nil) != wantOK {
			t.Errorf("tenant %s after Reload: err = %v, want admitted=%v", id, err, wantOK)
		}
	}
}

func TestTokensResolveOnlyThroughTheirTenantsProvider(t *testing.T) {
	issA, issB := authtest.NewIssuer(t), authtest.NewIssuer(t)
	mw := tenantMiddleware(t, newRegistry(t, tenantSpec("acme", issA), tenantSpec("rival", issB)))

	if got, err := admitted(mw, issA.Token(t, map[string]any{"tid": "acme"}), readMethod); err != nil || got != "acme" {
		t.Fatalf("acme's token: tenant %q err %v", got, err)
	}
	if got, err := admitted(mw, issB.Token(t, map[string]any{"tid": "rival"}), readMethod); err != nil || got != "rival" {
		t.Fatalf("rival's token: tenant %q err %v", got, err)
	}
	refused := map[string]string{
		"acme's provider minting for rival": issA.Token(t, map[string]any{"tid": "rival"}),
		"rival's key under acme's issuer":   issB.Token(t, map[string]any{"iss": issA.URL(), "tid": "acme"}),
		"unknown issuer":                    issA.Token(t, map[string]any{"iss": "https://evil.example.com", "tid": "acme"}),
		"unconfigured tenant":               issA.Token(t, map[string]any{"tid": "nobody"}),
		"no tenant claim":                   issA.Token(t, map[string]any{}),
		"expired":                           issA.Token(t, map[string]any{"tid": "acme", "exp": time.Now().Add(-time.Minute).Unix()}),
	}
	for name, tok := range refused {
		_, err := admitted(mw, tok, readMethod)
		if status.Code(err) != codes.Unauthenticated {
			t.Errorf("%s: err = %v, want Unauthenticated", name, err)
		}
		if err != nil && (strings.Contains(err.Error(), "evil.example.com") || strings.Contains(err.Error(), issA.URL())) {
			t.Errorf("%s: the refusal names an issuer: %v", name, err)
		}
	}
}

func TestPerTenantClaimNamesAudienceAndSharedIssuer(t *testing.T) {
	iss := authtest.NewIssuer(t)
	org := tenantSpec("org-one", iss)
	org.Spec.OIDC.TenantClaim, org.Spec.OIDC.RolesClaim, org.Spec.OIDC.Audience = "org_id", "roles", "vmafx-api"
	plain := tenantSpec("plain", iss)
	reg := newRegistry(t, org, plain)
	mw := tenantMiddleware(t, reg)

	ok := iss.Token(t, map[string]any{"org_id": "org-one", "roles": []string{auth.RoleWriter}, "aud": "vmafx-api"})
	if got, err := admitted(mw, ok, writeMethod); err != nil || got != "org-one" {
		t.Errorf("org-one writer: tenant %q err %v", got, err)
	}
	if got, err := admitted(mw, iss.Token(t, map[string]any{"tid": "plain"}), readMethod); err != nil || got != "plain" {
		t.Errorf("plain on the shared issuer: tenant %q err %v", got, err)
	}
	wrongAud := iss.Token(t, map[string]any{"org_id": "org-one", "aud": "other"})
	if _, err := admitted(mw, wrongAud, readMethod); status.Code(err) != codes.Unauthenticated {
		t.Errorf("org-one token for another audience: %v, want Unauthenticated", err)
	}
	ambiguous := iss.Token(t, map[string]any{"org_id": "org-one", "tid": "plain", "aud": "vmafx-api"})
	if _, err := admitted(mw, ambiguous, readMethod); status.Code(err) != codes.Unauthenticated {
		t.Errorf("token naming two tenants: %v, want Unauthenticated", err)
	}
	if _, err := reg.Resolve(ambiguous); err == nil || !strings.Contains(err.Error(), "matches tenants") {
		t.Errorf("token naming two tenants: Resolve error %v, want the ambiguity named", err)
	}
}

func TestSuspendedTenantRefused(t *testing.T) {
	iss := authtest.NewIssuer(t)
	off := tenantSpec("acme", iss)
	off.Spec.Enabled = new(false)
	mw := tenantMiddleware(t, newRegistry(t, off))
	tok := iss.Token(t, map[string]any{"tid": "acme", "vmafx_roles": []string{auth.RoleAdmin}})

	if _, err := admitted(mw, tok, readMethod); status.Code(err) != codes.PermissionDenied {
		t.Errorf("gRPC: err = %v, want PermissionDenied", err)
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/score", nil)
	req.Header.Set("Authorization", "Bearer "+tok)
	rr := httptest.NewRecorder()
	mw.HTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})).ServeHTTP(rr, req)
	if rr.Code != http.StatusForbidden {
		t.Errorf("HTTP: status %d, want 403", rr.Code)
	}
}

// roleCase is one roles claim and the tier methods it must reach.
type roleCase struct {
	name   string
	roles  any // nil omits the claim
	reach  []string
	refuse []string
}

func TestAllowedRolesAndDefaultRole(t *testing.T) {
	iss := authtest.NewIssuer(t)
	mw := tenantMiddleware(t, newRegistry(t, tenantSpec("acme", iss))) // CRD defaults: reader; reader+writer
	cases := []roleCase{
		{"no roles claim gets the default reader", nil, []string{readMethod}, []string{writeMethod, adminMethod}},
		{"empty claim gets the default reader", []string{}, []string{readMethod}, []string{writeMethod}},
		{"non-vmafx strings get the default reader", []string{"admin"}, []string{readMethod}, []string{writeMethod}},
		{"admin alone is stripped to nothing", []string{auth.RoleAdmin}, nil, []string{readMethod, writeMethod, adminMethod}},
		{"admin stripped, writer kept", []string{auth.RoleAdmin, auth.RoleWriter}, []string{readMethod, writeMethod}, []string{adminMethod}},
	}
	checkRoleCases(t, mw, iss, cases)
}

// TestNodeRoleOnlyWhereAllowed: vmafx:node reaches the node method only for a
// tenant that lists it, reaches nothing else, and a tenant with the CRD
// defaults strips it (ADR-1563).
func TestNodeRoleOnlyWhereAllowed(t *testing.T) {
	iss := authtest.NewIssuer(t)
	nodes := tenantSpec("acme", iss)
	nodes.Spec.RBAC = &auth.TenantRBAC{AllowedRoles: []string{auth.RoleReader, auth.RoleWriter, auth.RoleNode}}
	mw := tenantMiddleware(t, newRegistry(t, nodes))
	all := []string{readMethod, writeMethod, adminMethod}
	checkRoleCases(t, mw, iss, []roleCase{
		{"node alone reaches only the node method", []string{auth.RoleNode}, []string{nodeMethod}, all},
		{"reader and node", []string{auth.RoleReader, auth.RoleNode}, []string{readMethod, nodeMethod}, []string{writeMethod, adminMethod}},
		{"writer never reaches the node method", []string{auth.RoleWriter}, []string{readMethod, writeMethod}, []string{nodeMethod}},
	})
	defaults := tenantMiddleware(t, newRegistry(t, tenantSpec("acme", iss))) // allowedRoles: reader, writer
	checkRoleCases(t, defaults, iss, []roleCase{
		{"node stripped by the CRD defaults", []string{auth.RoleNode}, nil, append(all, nodeMethod)},
	})
}

// checkRoleCases runs each case's token against the methods it must reach
// and the ones it must be refused.
func checkRoleCases(t *testing.T, mw *auth.Middleware, iss *authtest.Issuer, cases []roleCase) {
	t.Helper()
	for _, tc := range cases {
		claims := map[string]any{"tid": "acme"}
		if tc.roles != nil {
			claims["vmafx_roles"] = tc.roles
		}
		tok := iss.Token(t, claims)
		for _, m := range tc.reach {
			if _, err := admitted(mw, tok, m); err != nil {
				t.Errorf("%s: %s refused: %v", tc.name, m, err)
			}
		}
		for _, m := range tc.refuse {
			if _, err := admitted(mw, tok, m); status.Code(err) != codes.PermissionDenied {
				t.Errorf("%s: %s err = %v, want PermissionDenied", tc.name, m, err)
			}
		}
	}
}

func TestStaleTenantSetRefusesEveryToken(t *testing.T) {
	iss := authtest.NewIssuer(t)
	reg := newRegistry(t, tenantSpec("acme", iss))
	mw := tenantMiddleware(t, reg)
	tok := iss.Token(t, map[string]any{"tid": "acme"})
	if _, err := admitted(mw, tok, readMethod); err != nil {
		t.Fatalf("fresh set: %v", err)
	}
	reg.SetClockForTest(func() time.Time { return time.Now().Add(2 * time.Minute) })
	if _, err := admitted(mw, tok, readMethod); status.Code(err) != codes.Unavailable {
		t.Errorf("set older than the bound: err = %v, want Unavailable", err)
	}
	reg.MarkRefreshFailed(nil)
	if reg.Reload([]auth.NamedTenantSpec{tenantSpec("acme", iss)}) != nil {
		t.Fatal("Reload of a valid set returned errors")
	}
	if _, err := admitted(mw, tok, readMethod); err != nil {
		t.Errorf("after a successful reload: %v", err)
	}
	empty, _ := auth.NewTenantRegistry(time.Minute, nil)
	if _, err := admitted(tenantMiddleware(t, empty), tok, readMethod); status.Code(err) != codes.Unavailable {
		t.Errorf("registry never loaded: err = %v, want Unavailable", err)
	}
}

func TestTenantRegistryExcludesOtherModes(t *testing.T) {
	reg, _ := auth.NewTenantRegistry(time.Minute, nil)
	bad := map[string]auth.Config{
		"with disabled":     {Tenants: reg, Disabled: true},
		"with JWKS":         {Tenants: reg, JWKSEndpoint: "https://idp.example.com/keys"},
		"with issuer":       {Tenants: reg, Issuer: "https://idp.example.com"},
		"with audience":     {Tenants: reg, Audience: "api"},
		"with tenant claim": {Tenants: reg, TenantClaim: "org_id"},
		"with roles claim":  {Tenants: reg, RolesClaim: "roles"},
	}
	for name, cfg := range bad {
		if _, err := auth.New(cfg); err == nil {
			t.Errorf("auth.New accepted a tenant registry %s", name)
		}
	}
	if _, err := auth.NewTenantRegistry(0, nil); err == nil {
		t.Error("NewTenantRegistry accepted a zero staleness bound")
	}
}

// testInfo returns the unary server info of method.
func testInfo(method string) *grpc.UnaryServerInfo {
	return &grpc.UnaryServerInfo{FullMethod: method}
}
