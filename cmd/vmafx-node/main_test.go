// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main_test.go — unit tests for vmafx-node config helpers.
//
// The pre-fx helpers (logLevel / envOrDefault / ffmpegPath over raw os.Getenv)
// were removed in the golusoris migration (ADR-1119): logging level is now read
// by golusoris log.Module from the log.level config key, and listen / ffmpeg /
// model settings come from the golusoris config tree. These tests cover the
// thin config accessors that replaced them.
//
// ADR-0713: vmafx-node Go worker binary.
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"testing"

	"github.com/golusoris/golusoris/config"
)

// newTestConfig builds a golusoris config with the VMAFX_ prefix and "."
// delimiter — matching the production fx.Replace(config.Options{...}) — seeded
// from the given env map. Watch is disabled so tests do not start watchers.
func newTestConfig(t *testing.T, env map[string]string) *config.Config {
	t.Helper()
	for k, v := range env {
		t.Setenv(k, v)
	}
	cfg, err := config.New(config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: false})
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}
	return cfg
}

// TestFFmpegBinary verifies the ffmpeg binary resolves from VMAFX_FFMPEG_BIN
// (koanf key ffmpeg.bin) and falls back to "ffmpeg" when unset.
func TestFFmpegBinary(t *testing.T) {
	t.Run("unset → ffmpeg", func(t *testing.T) {
		cfg := newTestConfig(t, nil)
		if got := ffmpegBinary(cfg); got != "ffmpeg" {
			t.Errorf("ffmpegBinary = %q, want \"ffmpeg\"", got)
		}
	})

	t.Run("VMAFX_FFMPEG_BIN → override", func(t *testing.T) {
		cfg := newTestConfig(t, map[string]string{"VMAFX_FFMPEG_BIN": "/usr/local/bin/ffmpeg-custom"})
		if got := ffmpegBinary(cfg); got != "/usr/local/bin/ffmpeg-custom" {
			t.Errorf("ffmpegBinary = %q, want the override", got)
		}
	})
}

// TestProvideExecutorBackendDefault verifies the executor backend reads
// VMAFX_BACKEND (koanf key "backend") and defaults to "cpu" when unset.
func TestProvideExecutorBackendDefault(t *testing.T) {
	t.Run("unset → cpu", func(t *testing.T) {
		cfg := newTestConfig(t, nil)
		exec := provideExecutor(nil, cfg, nil)
		if exec == nil {
			t.Fatal("provideExecutor returned nil")
		}
		if exec.backend != "cpu" {
			t.Errorf("backend = %q, want \"cpu\"", exec.backend)
		}
	})

	t.Run("VMAFX_BACKEND → override", func(t *testing.T) {
		cfg := newTestConfig(t, map[string]string{"VMAFX_BACKEND": "cuda"})
		exec := provideExecutor(nil, cfg, nil)
		if exec.backend != "cuda" {
			t.Errorf("backend = %q, want \"cuda\"", exec.backend)
		}
	})
}
