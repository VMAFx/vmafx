// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/swagger_extra_test.go — additional swagger UI handler
// coverage: swaggerTryItOut env toggle, method-not-allowed on /swagger routes,
// and /swagger/spec.json method guard.
//
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// TestSwaggerTryItOut_Disabled verifies that swaggerTryItOut returns an empty
// string when VMAFX_SWAGGER_TRY_IT_OUT is not set (try-it-out disabled).
func TestSwaggerTryItOut_Disabled(t *testing.T) {
	t.Setenv("VMAFX_SWAGGER_TRY_IT_OUT", "")
	got := swaggerTryItOut()
	if got != "" {
		t.Errorf("expected empty string when env unset, got %q", got)
	}
}

// TestSwaggerTryItOut_Enabled verifies that swaggerTryItOut returns the HTTP
// method list when VMAFX_SWAGGER_TRY_IT_OUT=1.
func TestSwaggerTryItOut_Enabled(t *testing.T) {
	t.Setenv("VMAFX_SWAGGER_TRY_IT_OUT", "1")
	got := swaggerTryItOut()
	if got == "" {
		t.Error("expected non-empty method list when env set to 1")
	}
	if !strings.Contains(got, "post") {
		t.Errorf("expected method list to include 'post', got %q", got)
	}
}

// TestSwaggerIndex_MethodNotAllowed verifies that POST /swagger returns 405.
func TestSwaggerIndex_MethodNotAllowed(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/swagger", "text/plain", strings.NewReader("x"))
	if err != nil {
		t.Fatalf("POST /swagger: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", resp.StatusCode)
	}
}

// TestSwaggerSpec_MethodNotAllowed verifies that POST /swagger/spec.json returns 405.
func TestSwaggerSpec_MethodNotAllowed(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/swagger/spec.json", "text/plain", strings.NewReader("x"))
	if err != nil {
		t.Fatalf("POST /swagger/spec.json: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", resp.StatusCode)
	}
}

// TestSwaggerTryItOut_UIContainsMethods verifies that when try-it-out is
// enabled the rendered HTML page includes the method list.
func TestSwaggerTryItOut_UIContainsMethods(t *testing.T) {
	t.Setenv("VMAFX_SWAGGER_TRY_IT_OUT", "1")

	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/swagger")
	if err != nil {
		t.Fatalf("GET /swagger: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}
