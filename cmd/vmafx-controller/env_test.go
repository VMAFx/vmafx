// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/env_test.go — VMAFX_ environment reaches the keys.
//
// golusoris' env transform turns every underscore of a VMAFX_ variable into
// the key delimiter, except in the CompoundKeys of controllerEnvOptions. The
// list is generated (config_keys.gen.go, ADR-2350 D13). Before it was, the
// controller's hand list lacked the grpc.* keys its gRPC server reads, so
// VMAFX_GRPC_MAX_RECV_SIZE reached grpc.max.recv.size and changed nothing.
// Positive: every grpc.* and store.* variable reaches its key. Negative: a
// variable whose key is not declared is split, so the positive case is not
// vacuous. Boundary: a value at the int32 limit of the gRPC message size.

//go:build cgo

package main

import (
	"testing"

	"github.com/golusoris/golusoris/core/config"
)

func controllerTestConfig(t *testing.T) *config.Config {
	t.Helper()
	opts := controllerEnvOptions()
	opts.Watch = false
	cfg, err := config.New(opts)
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}
	return cfg
}

func TestControllerEnvReachesGrpcKeys(t *testing.T) {
	t.Setenv("VMAFX_GRPC_MAX_RECV_SIZE", "8388608")
	t.Setenv("VMAFX_GRPC_MAX_SEND_SIZE", "2147483647")
	t.Setenv("VMAFX_GRPC_CERT_FILE", "/etc/tls/tls.crt")
	t.Setenv("VMAFX_GRPC_KEY_FILE", "/etc/tls/tls.key")
	cfg := controllerTestConfig(t)
	if got := cfg.Int("grpc.max_recv_size"); got != 8388608 {
		t.Errorf("grpc.max_recv_size = %d, want 8388608", got)
	}
	if got := cfg.Int("grpc.max_send_size"); got != 2147483647 {
		t.Errorf("grpc.max_send_size = %d, want 2147483647", got)
	}
	if got := cfg.String("grpc.cert_file"); got != "/etc/tls/tls.crt" {
		t.Errorf("grpc.cert_file = %q, want /etc/tls/tls.crt", got)
	}
	if got := cfg.String("grpc.key_file"); got != "/etc/tls/tls.key" {
		t.Errorf("grpc.key_file = %q, want /etc/tls/tls.key", got)
	}
}

func TestControllerEnvReachesStoreKeys(t *testing.T) {
	want := map[string]string{
		"VMAFX_STORE_LEASE_TTL":      "store.lease_ttl",
		"VMAFX_STORE_SESSION_TTL":    "store.session_ttl",
		"VMAFX_STORE_SWEEP_INTERVAL": "store.sweep_interval",
		"VMAFX_STORE_BACKOFF_BASE":   "store.backoff_base",
		"VMAFX_STORE_BACKOFF_MAX":    "store.backoff_max",
	}
	for env := range want {
		t.Setenv(env, "17s")
	}
	cfg := controllerTestConfig(t)
	for env, key := range want {
		if got := cfg.String(key); got != "17s" {
			t.Errorf("%s: %s = %q, want 17s", env, key, got)
		}
	}
}

func TestControllerEnvSplitsUndeclaredKeys(t *testing.T) {
	t.Setenv("VMAFX_GRPC_NOT_DECLARED", "1")
	cfg := controllerTestConfig(t)
	if got := cfg.String("grpc.not_declared"); got != "" {
		t.Errorf("grpc.not_declared = %q, want unset (the transform splits it)", got)
	}
	if got := cfg.String("grpc.not.declared"); got != "1" {
		t.Errorf("grpc.not.declared = %q, want 1", got)
	}
}
