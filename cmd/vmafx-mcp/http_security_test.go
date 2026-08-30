// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// okHandler is a trivial downstream handler that records whether it ran and
// returns 200.
func okHandler(ran *bool) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		*ran = true
		w.WriteHeader(http.StatusOK)
	})
}

func TestSecurityMiddlewareRefusesWhenNoTokenAndNoOptOut(t *testing.T) {
	t.Setenv("VMAFX_MCP_HTTP_TOKEN", "")
	t.Setenv("VMAFX_MCP_HTTP_NO_AUTH", "")

	ran := false
	h := securityMiddleware(okHandler(&ran))
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/", nil))

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("want 401 when no token and no opt-out, got %d", rec.Code)
	}
	if ran {
		t.Fatal("downstream handler ran despite refuse-all auth gate")
	}
}

func TestSecurityMiddlewareNoAuthMode(t *testing.T) {
	t.Setenv("VMAFX_MCP_HTTP_TOKEN", "")
	t.Setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")

	ran := false
	h := securityMiddleware(okHandler(&ran))
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("want 200 with NO_AUTH=1, got %d", rec.Code)
	}
	if !ran {
		t.Fatal("downstream handler did not run with auth disabled")
	}
}

func TestSecurityMiddlewareBearerToken(t *testing.T) {
	t.Setenv("VMAFX_MCP_HTTP_TOKEN", "s3cr3t")
	t.Setenv("VMAFX_MCP_HTTP_NO_AUTH", "")

	cases := []struct {
		name   string
		header string
		want   int
	}{
		{"valid token", "Bearer s3cr3t", http.StatusOK},
		{"wrong token", "Bearer nope", http.StatusUnauthorized},
		{"missing header", "", http.StatusUnauthorized},
		{"no bearer prefix", "s3cr3t", http.StatusUnauthorized},
		{"empty bearer", "Bearer ", http.StatusUnauthorized},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			ran := false
			h := securityMiddleware(okHandler(&ran))
			req := httptest.NewRequest(http.MethodPost, "/", nil)
			if tc.header != "" {
				req.Header.Set("Authorization", tc.header)
			}
			rec := httptest.NewRecorder()
			h.ServeHTTP(rec, req)
			if rec.Code != tc.want {
				t.Fatalf("status: want %d, got %d", tc.want, rec.Code)
			}
			if (rec.Code == http.StatusOK) != ran {
				t.Fatalf("handler-ran mismatch: ran=%v code=%d", ran, rec.Code)
			}
		})
	}
}

func TestSecurityMiddlewareBodyLimitContentLength(t *testing.T) {
	t.Setenv("VMAFX_MCP_HTTP_TOKEN", "")
	t.Setenv("VMAFX_MCP_HTTP_NO_AUTH", "1") // isolate the body-size gate

	ran := false
	h := securityMiddleware(okHandler(&ran))
	req := httptest.NewRequest(http.MethodPost, "/", strings.NewReader("x"))
	req.ContentLength = maxRequestBodyBytes + 1
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("want 413 for oversized Content-Length, got %d", rec.Code)
	}
	if ran {
		t.Fatal("downstream handler ran despite oversized body")
	}
}

func TestApplyBindHost(t *testing.T) {
	cases := []struct {
		name     string
		bindEnv  string
		addr     string
		wantAddr string
	}{
		{"no-host defaults loopback", "", ":3000", "127.0.0.1:3000"},
		{"no-host honours bind env", "0.0.0.0", ":3000", "0.0.0.0:3000"},
		{"explicit host wins over env", "0.0.0.0", "127.0.0.1:3000", "127.0.0.1:3000"},
		{"explicit all-interfaces preserved", "", "0.0.0.0:8080", "0.0.0.0:8080"},
		{"non host:port unchanged", "", "garbage", "garbage"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("VMAFX_MCP_HTTP_BIND", tc.bindEnv)
			if got := applyBindHost(tc.addr); got != tc.wantAddr {
				t.Fatalf("applyBindHost(%q): want %q, got %q", tc.addr, tc.wantAddr, got)
			}
		})
	}
}
