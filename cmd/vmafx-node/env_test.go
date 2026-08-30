// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/env_test.go — VMAFX_ env-contract regression guards.
//
// golusoris' env transform splits EVERY underscore on the delimiter, so the
// grpc.Module's underscore-bearing leaf keys (grpc.cert_file, grpc.key_file,
// grpc.max_recv_size, grpc.max_send_size) only bind if nodeEnvOptions declares
// them as CompoundKeys. Without that, VMAFX_GRPC_MAX_RECV_SIZE silently maps to
// grpc.max.recv.size and the operator override is a no-op. These tests pin both
// the declared contract and the end-to-end env→config binding.

//go:build cgo

package main

import (
	"testing"

	"github.com/golusoris/golusoris/config"
)

// TestNodeEnvOptionsContract pins the VMAFX_ prefix, delimiter, and the exact
// CompoundKey set (the four underscore-bearing grpc leaves).
func TestNodeEnvOptionsContract(t *testing.T) {
	t.Parallel()
	opts := nodeEnvOptions(true)
	if opts.EnvPrefix != "VMAFX_" {
		t.Errorf("EnvPrefix = %q, want VMAFX_", opts.EnvPrefix)
	}
	if opts.Delimiter != "." {
		t.Errorf("Delimiter = %q, want .", opts.Delimiter)
	}
	if !opts.Watch {
		t.Error("nodeEnvOptions(true).Watch = false, want true")
	}
	if nodeEnvOptions(false).Watch {
		t.Error("nodeEnvOptions(false).Watch = true, want false")
	}
	want := map[string]bool{
		"grpc.cert_file":     true,
		"grpc.key_file":      true,
		"grpc.max_recv_size": true,
		"grpc.max_send_size": true,
	}
	got := make(map[string]bool, len(opts.CompoundKeys))
	for _, k := range opts.CompoundKeys {
		got[k] = true
	}
	for k := range want {
		if !got[k] {
			t.Errorf("missing CompoundKey %q (its VMAFX_GRPC_* env var would not bind)", k)
		}
	}
	for k := range got {
		if !want[k] {
			t.Errorf("unexpected CompoundKey %q", k)
		}
	}
}

// TestNodeEnvOptionsBindGrpcKeys is the end-to-end regression guard: the
// VMAFX_GRPC_* env vars must resolve onto the dotted grpc.* leaf keys through a
// real config.Config built with nodeEnvOptions().
func TestNodeEnvOptionsBindGrpcKeys(t *testing.T) {
	t.Setenv("VMAFX_GRPC_MAX_RECV_SIZE", "8388608")
	t.Setenv("VMAFX_GRPC_KEY_FILE", "/etc/tls/tls.key")

	cfg, err := config.New(nodeEnvOptions(false))
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}
	if got := cfg.Int("grpc.max_recv_size"); got != 8388608 {
		t.Errorf("grpc.max_recv_size = %d, want 8388608 (compound key not binding)", got)
	}
	if got := cfg.String("grpc.key_file"); got != "/etc/tls/tls.key" {
		t.Errorf("grpc.key_file = %q, want /etc/tls/tls.key", got)
	}
}
