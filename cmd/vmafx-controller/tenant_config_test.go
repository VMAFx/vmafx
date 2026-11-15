// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/tenant_config_test.go — the controller reads and
// enforces its tenant configuration (ADR-1519).
//
// TestMisconfiguredTenantStopsStartup builds the production graph with each
// kind of misconfiguration and requires the build to fail; the valid
// configuration next to them starts. TestTenantRegistryEnforcedOverTheWire
// boots the production graph with a tenant file of two identity providers
// and checks per-tenant OIDC, suspension, the role whitelist and a reload.

//go:build cgo

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"go.uber.org/fx"
	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/codes"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth/authtest"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// tenantDoc renders one VmafxTenant document trusting issuerURL; extra is
// appended to the spec (YAML flow mapping entries).
func tenantDoc(id, issuerURL, extra string) string {
	return fmt.Sprintf(`---
apiVersion: vmafx.dev/v1
kind: VmafxTenant
metadata: {name: %[1]s}
spec: {tenantId: %[1]s, oidc: {issuer: %[2]q, jwksEndpoint: %[2]q}%[3]s}
`, id, issuerURL, extra)
}

// writeTenantFile writes docs to a temp file and returns its path.
func writeTenantFile(t *testing.T, docs ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "tenants.yaml")
	if err := os.WriteFile(path, []byte(strings.Join(docs, "")), 0o600); err != nil {
		t.Fatalf("write tenant file: %v", err)
	}
	return path
}

// tenantEnv enables auth with a file tenant source at path.
func tenantEnv(t *testing.T, path string) {
	t.Helper()
	writeControllerEnv(t)
	t.Setenv("VMAFX_AUTH_DISABLED", "false")
	t.Setenv("VMAFX_AUTH_TENANTS_SOURCE", "file")
	t.Setenv("VMAFX_AUTH_TENANTS_FILE", path)
}

// startupCase sets up one configuration and says whether it may start.
type startupCase struct {
	name   string
	setup  func(t *testing.T, issuerURL string)
	starts bool
}

func fileCase(name string, starts bool, docs func(u string) []string) startupCase {
	return startupCase{name, func(t *testing.T, u string) { tenantEnv(t, writeTenantFile(t, docs(u)...)) }, starts}
}

func envCase(name string, env map[string]string) startupCase {
	return startupCase{name, func(t *testing.T, u string) {
		tenantEnv(t, writeTenantFile(t, tenantDoc("acme", u, "")))
		for k, v := range env {
			t.Setenv(k, v)
		}
	}, false}
}

var startupCases = []startupCase{
	fileCase("valid tenant", true, func(u string) []string { return []string{tenantDoc("acme", u, "")} }),
	fileCase("unknown allowed role", false, func(u string) []string {
		return []string{tenantDoc("acme", u, `, rbac: {allowedRoles: ["vmafx:root"]}`)}
	}),
	fileCase("default role not allowed", false, func(u string) []string {
		return []string{tenantDoc("acme", u, `, rbac: {defaultRole: "vmafx:admin"}`)}
	}),
	fileCase("misspelt field", false, func(u string) []string {
		return []string{tenantDoc("acme", u, `, rbac: {allowedRole: ["vmafx:reader"]}`)}
	}),
	fileCase("duplicate tenantId", false, func(u string) []string {
		return []string{tenantDoc("acme", u, ""), tenantDoc("acme", u, "")}
	}),
	fileCase("invalid tenantId among valid", false, func(u string) []string {
		return []string{tenantDoc("acme", u, ""), tenantDoc("Rival", u, "")}
	}),
	fileCase("plain-http JWKS off loopback", false, func(string) []string {
		return []string{tenantDoc("acme", "http://idp.example.com", "")}
	}),
	fileCase("not YAML", false, func(string) []string { return []string{"tenantId: [unclosed"} }),
	envCase("missing file", map[string]string{"VMAFX_AUTH_TENANTS_FILE": "/nonexistent/tenants.yaml"}),
	envCase("file source without file", map[string]string{"VMAFX_AUTH_TENANTS_FILE": ""}),
	envCase("file without source", map[string]string{"VMAFX_AUTH_TENANTS_SOURCE": ""}),
	envCase("unknown source", map[string]string{"VMAFX_AUTH_TENANTS_SOURCE": "consul"}),
	envCase("namespace with file source", map[string]string{"VMAFX_AUTH_TENANTS_NAMESPACE": "vmafx"}),
	envCase("refresh too short", map[string]string{"VMAFX_AUTH_TENANTS_REFRESH": "10ms"}),
	envCase("refresh not a duration", map[string]string{"VMAFX_AUTH_TENANTS_REFRESH": "often"}),
	envCase("with the global issuer", map[string]string{"VMAFX_AUTH_ISSUER": "https://idp.example.com"}),
	envCase("with the global JWKS", map[string]string{"VMAFX_JWKS_ENDPOINT": "https://idp.example.com/keys"}),
	envCase("with disabled auth", map[string]string{"VMAFX_AUTH_DISABLED": "true"}),
	envCase("kubernetes outside a cluster", map[string]string{
		"VMAFX_AUTH_TENANTS_SOURCE": "kubernetes", "VMAFX_AUTH_TENANTS_FILE": "", "KUBERNETES_SERVICE_HOST": "",
	}),
}

func TestMisconfiguredTenantStopsStartup(t *testing.T) {
	iss := authtest.NewIssuer(t)
	for _, tc := range startupCases {
		t.Run(tc.name, func(t *testing.T) {
			tc.setup(t, iss.URL())
			err := fx.New(productionGraph(), fx.NopLogger).Err()
			if tc.starts && err != nil {
				t.Fatalf("valid configuration refused: %v", err)
			}
			if !tc.starts && err == nil {
				t.Fatal("misconfiguration accepted; the controller would start")
			}
			t.Logf("startup error: %v", err)
		})
	}
}

// wireSubmit issues SubmitJob with token and returns the status code.
func wireSubmit(t *testing.T, cc *googlegrpc.ClientConn, token string) codes.Code {
	t.Helper()
	_, err := controllerClient(cc).SubmitJob(tokenCtx(t, token), &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"},
	})
	return codeOf(err)
}

// wireGet issues GetJob of an unknown job with token: NotFound when the call
// is admitted, PermissionDenied or Unauthenticated when it is refused.
func wireGet(t *testing.T, cc *googlegrpc.ClientConn, token string) codes.Code {
	t.Helper()
	_, err := controllerClient(cc).GetJob(tokenCtx(t, token), &controllerv1.GetJobRequest{JobId: "none"})
	return codeOf(err)
}

// startTenantController boots the production graph over the tenant file at
// path with a one-second refresh.
func startTenantController(t *testing.T, path string) *googlegrpc.ClientConn {
	t.Helper()
	tenantEnv(t, path)
	t.Setenv("VMAFX_AUTH_TENANTS_REFRESH", "1s")
	app, addr := startController(t, "VMAFX_GRPC_LISTEN")
	t.Cleanup(app.RequireStop)
	return dialInsecure(t, addr)
}

func TestTenantRegistryEnforcedOverTheWire(t *testing.T) {
	issA, issB := authtest.NewIssuer(t), authtest.NewIssuer(t)
	// acme may score every local path (ADR-1577), so the submit check below
	// is decided by roles and suspension alone.
	acmeRoots := `, scoring: {roots: ["/"]}`
	docs := []string{tenantDoc("acme", issA.URL(), acmeRoots), tenantDoc("rival", issB.URL(), ", enabled: false")}
	path := writeTenantFile(t, docs...)
	cc := startTenantController(t, path)

	writer := issA.Token(t, map[string]any{"tid": "acme", "vmafx_roles": []string{auth.RoleWriter}})
	checks := []struct {
		name  string
		got   codes.Code
		want  codes.Code
		using string
	}{
		{"acme writer submits", wireSubmit(t, cc, writer), codes.OK, "SubmitJob"},
		{"acme's provider minting for rival", wireGet(t, cc, issA.Token(t, map[string]any{"tid": "rival", "vmafx_roles": []string{auth.RoleAdmin}})), codes.Unauthenticated, "GetJob"},
		{"suspended rival", wireGet(t, cc, issB.Token(t, map[string]any{"tid": "rival", "vmafx_roles": []string{auth.RoleAdmin}})), codes.PermissionDenied, "GetJob"},
		{"acme admin stripped by allowedRoles", wireGet(t, cc, issA.Token(t, map[string]any{"tid": "acme", "vmafx_roles": []string{auth.RoleAdmin}})), codes.PermissionDenied, "GetJob"},
		{"acme without roles reads (default reader)", wireGet(t, cc, issA.Token(t, map[string]any{"tid": "acme"})), codes.NotFound, "GetJob of an unknown id"},
		{"acme without roles cannot submit", wireSubmit(t, cc, issA.Token(t, map[string]any{"tid": "acme"})), codes.PermissionDenied, "SubmitJob"},
	}
	for _, c := range checks {
		if c.got != c.want {
			t.Errorf("%s (%s): %v, want %v", c.name, c.using, c.got, c.want)
		}
	}

	// Suspend acme in the file; the next refresh must refuse its writer.
	if err := os.WriteFile(path, []byte(tenantDoc("acme", issA.URL(), acmeRoots+", enabled: false")), 0o600); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(10 * time.Second)
	for wireSubmit(t, cc, writer) != codes.PermissionDenied {
		if time.Now().After(deadline) {
			t.Fatal("acme still admitted 10 s after it was suspended in the tenant file")
		}
		time.Sleep(100 * time.Millisecond)
	}
}
