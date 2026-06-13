// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main_test.go — unit tests for vmafx-node main-package helpers.
//
// ADR-0713: vmafx-node Go worker binary.

package main

import (
	"log/slog"
	"os"
	"testing"
)

// TestLogLevel exercises all branches of the logLevel() helper.
// Note: t.Setenv is incompatible with t.Parallel in subtests.
func TestLogLevel(t *testing.T) {
	tests := []struct {
		env  string
		want slog.Level
	}{
		{"DEBUG", slog.LevelDebug},
		{"debug", slog.LevelDebug},
		{"WARN", slog.LevelWarn},
		{"warn", slog.LevelWarn},
		{"WARNING", slog.LevelWarn},
		{"warning", slog.LevelWarn},
		{"ERROR", slog.LevelError},
		{"error", slog.LevelError},
		{"INFO", slog.LevelInfo},
		{"", slog.LevelInfo},
		{"TRACE", slog.LevelInfo}, // unknown → default INFO
	}
	for _, tt := range tests {
		tt := tt
		t.Run("env="+tt.env, func(t *testing.T) {
			t.Setenv("VMAFX_LOG_LEVEL", tt.env)
			if got := logLevel(); got != tt.want {
				t.Errorf("logLevel(%q) = %v, want %v", tt.env, got, tt.want)
			}
		})
	}
}

// TestEnvOrDefault exercises envOrDefault's fallback and override paths.
// Note: t.Setenv is incompatible with t.Parallel in subtests.
func TestEnvOrDefault(t *testing.T) {
	const key = "VMAFX_NODE_TEST_ENVORDEFAULT"

	t.Run("unset → fallback", func(t *testing.T) {
		if err := os.Unsetenv(key); err != nil {
			t.Fatalf("Unsetenv: %v", err)
		}
		if got := envOrDefault(key, "default"); got != "default" {
			t.Errorf("got %q, want %q", got, "default")
		}
	})

	t.Run("set → value", func(t *testing.T) {
		t.Setenv(key, "override")
		if got := envOrDefault(key, "default"); got != "override" {
			t.Errorf("got %q, want %q", got, "override")
		}
	})
}

// TestFFmpegPath verifies that ffmpegPath() returns the env value when set or
// falls back to "ffmpeg".
// Note: t.Setenv is incompatible with t.Parallel in subtests.
func TestFFmpegPath(t *testing.T) {
	const key = "VMAFX_FFMPEG_BIN"

	t.Run("unset → ffmpeg", func(t *testing.T) {
		if err := os.Unsetenv(key); err != nil {
			t.Fatalf("Unsetenv: %v", err)
		}
		if got := ffmpegPath(); got != "ffmpeg" {
			t.Errorf("got %q, want \"ffmpeg\"", got)
		}
	})

	t.Run("set → override", func(t *testing.T) {
		t.Setenv(key, "/usr/local/bin/ffmpeg-custom")
		if got := ffmpegPath(); got != "/usr/local/bin/ffmpeg-custom" {
			t.Errorf("got %q, want %q", got, "/usr/local/bin/ffmpeg-custom")
		}
	})
}
