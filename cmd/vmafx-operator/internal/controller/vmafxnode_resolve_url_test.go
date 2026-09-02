// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_resolve_url_test.go —
// Standalone (non-envtest) unit tests for VmafxNodeReconciler.resolveControllerHTTPURL.
//
// resolveControllerHTTPURL is a pure helper with three code paths (struct
// field override, environment variable, default in-cluster DNS) and was
// previously at 0% test coverage.  These tests run without KUBEBUILDER_ASSETS
// and gate the three branches before they can regress.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops.
// ADR-1069: VmafxNode LastHeartbeat ownership — node agent only.

package controller

import (
	"os"
	"testing"
)

// TestResolveControllerHTTPURL_FieldOverride tests the ControllerHTTPAddr struct field.
// The field value must be returned verbatim with "/healthz" appended.
func TestResolveControllerHTTPURL_FieldOverride(t *testing.T) {
	t.Parallel()
	r := &VmafxNodeReconciler{ControllerHTTPAddr: "http://my-controller:8080"}
	got := r.resolveControllerHTTPURL("default")
	want := "http://my-controller:8080/healthz"
	if got != want {
		t.Errorf("resolveControllerHTTPURL: got %q, want %q", got, want)
	}
}

// TestResolveControllerHTTPURL_FieldOverride_TakesPrec ensures the struct field
// wins when both the field and the environment variable are set.
func TestResolveControllerHTTPURL_FieldOverride_TakesPrec(t *testing.T) {
	// Not parallel — sets env var.
	t.Setenv("VMAFX_CONTROLLER_HTTP_ADDR", "http://env-controller:8080")
	r := &VmafxNodeReconciler{ControllerHTTPAddr: "http://field-controller:8080"}
	got := r.resolveControllerHTTPURL("default")
	want := "http://field-controller:8080/healthz"
	if got != want {
		t.Errorf("resolveControllerHTTPURL: expected field to beat env; got %q, want %q", got, want)
	}
}

// TestResolveControllerHTTPURL_EnvOverride tests the VMAFX_CONTROLLER_HTTP_ADDR path.
func TestResolveControllerHTTPURL_EnvOverride(t *testing.T) {
	// Not parallel — sets env var.
	const key = "VMAFX_CONTROLLER_HTTP_ADDR"
	t.Setenv(key, "http://env-controller:8080")
	r := &VmafxNodeReconciler{}
	got := r.resolveControllerHTTPURL("default")
	want := "http://env-controller:8080/healthz"
	if got != want {
		t.Errorf("resolveControllerHTTPURL: got %q, want %q", got, want)
	}
	if err := os.Unsetenv(key); err != nil {
		t.Fatalf("Unsetenv: %v", err)
	}
}

// TestResolveControllerHTTPURL_DefaultFormat tests the in-cluster DNS default.
// Namespace is embedded as the second DNS label.
func TestResolveControllerHTTPURL_DefaultFormat(t *testing.T) {
	t.Parallel()
	if v := os.Getenv("VMAFX_CONTROLLER_HTTP_ADDR"); v != "" {
		t.Skipf("VMAFX_CONTROLLER_HTTP_ADDR is set to %q; skipping default format test", v)
	}
	r := &VmafxNodeReconciler{}
	got := r.resolveControllerHTTPURL("scoring")
	want := "http://vmafx-controller.scoring.svc.cluster.local:8080/healthz"
	if got != want {
		t.Errorf("resolveControllerHTTPURL: got %q, want %q", got, want)
	}
}

// TestResolveControllerHTTPURL_DefaultNamespaceInURL verifies that the namespace
// argument is correctly interpolated into the in-cluster URL.
func TestResolveControllerHTTPURL_DefaultNamespaceInURL(t *testing.T) {
	t.Parallel()
	if v := os.Getenv("VMAFX_CONTROLLER_HTTP_ADDR"); v != "" {
		t.Skipf("VMAFX_CONTROLLER_HTTP_ADDR is set to %q; skipping namespace test", v)
	}
	r := &VmafxNodeReconciler{}
	for _, ns := range []string{"default", "prod", "staging", "my-ns"} {
		ns := ns
		t.Run(ns, func(t *testing.T) {
			t.Parallel()
			got := r.resolveControllerHTTPURL(ns)
			wantSuffix := ".svc.cluster.local:8080/healthz"
			wantInfix := "vmafx-controller." + ns
			if len(got) == 0 || got[:len("http://")] != "http://" {
				t.Errorf("URL does not start with http://: %q", got)
			}
			if contains := func(s, sub string) bool {
				return len(s) >= len(sub) && (s == sub || len(s) > 0 && containsStr(s, sub))
			}; !contains(got, wantInfix) {
				t.Errorf("URL %q does not contain namespace label %q", got, wantInfix)
			}
			if !containsStr(got, wantSuffix) {
				t.Errorf("URL %q does not end with %q", got, wantSuffix)
			}
		})
	}
}

// containsStr is a minimal strings.Contains replacement to avoid importing strings.
func containsStr(s, sub string) bool {
	if len(sub) == 0 {
		return true
	}
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}
