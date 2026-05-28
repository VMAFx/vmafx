// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package server_test

import (
	"context"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	"github.com/VMAFx/vmafx/cmd/vmafx-node/server"
)

// TestNew_InvalidConfig verifies that New rejects configurations with missing
// required fields.
func TestNew_InvalidConfig(t *testing.T) {
	t.Parallel()

	inv := probe.EmptyInventory()

	cases := []struct {
		name string
		cfg  server.Config
	}{
		{
			name: "empty addr",
			cfg:  server.Config{Addr: "", FFmpegBin: "ffmpeg", Encoders: inv},
		},
		{
			name: "empty ffmpeg bin",
			cfg:  server.Config{Addr: ":50052", FFmpegBin: "", Encoders: inv},
		},
		{
			name: "nil encoders",
			cfg:  server.Config{Addr: ":50052", FFmpegBin: "ffmpeg", Encoders: nil},
		},
	}

	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := server.New(tc.cfg)
			if err == nil {
				t.Errorf("New(%q): expected error, got nil", tc.name)
			}
		})
	}
}

// TestServe_ShutdownOnContextCancel verifies that Serve returns when the
// context is cancelled.
func TestServe_ShutdownOnContextCancel(t *testing.T) {
	t.Parallel()

	inv := probe.EmptyInventory()
	srv, err := server.New(server.Config{
		Addr:      ":0", // OS-assigned port
		FFmpegBin: "ffmpeg",
		Encoders:  inv,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	// Cancel immediately to verify shutdown path.
	cancel()

	if err := srv.Serve(ctx); err != nil {
		t.Fatalf("Serve: unexpected error: %v", err)
	}
}
