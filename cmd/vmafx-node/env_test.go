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
	"os"
	"path/filepath"
	"testing"

	"github.com/golusoris/golusoris/config"
	grpcmod "github.com/golusoris/golusoris/grpc"
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

// TestWithNodeGRPCDefault pins the historical standalone-node port while also
// proving that an operator-supplied address is never replaced by the decorator.
func TestWithNodeGRPCDefault(t *testing.T) {
	t.Run("missing uses node default", func(t *testing.T) {
		t.Setenv("VMAFX_GRPC_LISTEN", "")
		raw, err := config.New(nodeEnvOptions(false))
		if err != nil {
			t.Fatalf("config.New: %v", err)
		}

		got := withNodeGRPCDefault(grpcmod.DefaultConfig(), raw)
		if got.Listen != defaultNodeGRPCListen {
			t.Errorf("Listen = %q, want %q", got.Listen, defaultNodeGRPCListen)
		}
	})

	t.Run("explicit override is preserved", func(t *testing.T) {
		const override = ":9090"
		t.Setenv("VMAFX_GRPC_LISTEN", override)
		raw, err := config.New(nodeEnvOptions(false))
		if err != nil {
			t.Fatalf("config.New: %v", err)
		}

		got := withNodeGRPCDefault(grpcmod.Config{
			Listen:      override,
			MaxRecvSize: 8 << 20,
			MaxSendSize: 16 << 20,
		}, raw)
		if got.Listen != override {
			t.Errorf("Listen = %q, want explicit override %q", got.Listen, override)
		}
		if got.MaxRecvSize != 8<<20 || got.MaxSendSize != 16<<20 {
			t.Errorf("decorator changed message-size limits: %+v", got)
		}
	})

	t.Run("file override is preserved", func(t *testing.T) {
		const override = ":9090"
		old, existed := os.LookupEnv("VMAFX_GRPC_LISTEN")
		if err := os.Unsetenv("VMAFX_GRPC_LISTEN"); err != nil {
			t.Fatalf("unset VMAFX_GRPC_LISTEN: %v", err)
		}
		t.Cleanup(func() {
			if existed {
				_ = os.Setenv("VMAFX_GRPC_LISTEN", old)
			} else {
				_ = os.Unsetenv("VMAFX_GRPC_LISTEN")
			}
		})

		path := filepath.Join(t.TempDir(), "node.yaml")
		if err := os.WriteFile(path, []byte("grpc:\n  listen: \":9090\"\n"), 0o600); err != nil {
			t.Fatalf("write config fixture: %v", err)
		}
		opts := nodeEnvOptions(false)
		opts.Files = []string{path}
		raw, err := config.New(opts)
		if err != nil {
			t.Fatalf("config.New: %v", err)
		}
		framework := grpcmod.DefaultConfig()
		if err := raw.Unmarshal("grpc", &framework); err != nil {
			t.Fatalf("unmarshal grpc config: %v", err)
		}

		got := withNodeGRPCDefault(framework, raw)
		if got.Listen != override {
			t.Errorf("Listen = %q, want file override %q", got.Listen, override)
		}
	})
}
