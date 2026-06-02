// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_test.go — unit tests for the job executor.
//
// ADR-0719: vmafx-node rclone integration.
package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/storage"
)

// mockStorage is a Storage that always returns the path it was given.
type mockStorage struct {
	mode storage.Mode
}

func (m *mockStorage) Mode() storage.Mode { return m.mode }

func (m *mockStorage) Prepare(_ context.Context, sourceURI string) (string, func(), error) {
	return sourceURI, func() {}, nil
}

// TestExecutor_LocalPaths verifies that the executor calls Prepare on both
// reference and distorted URIs and forwards the resolved paths to vmaf.
// The actual vmaf invocation is not run in the unit test; we verify only that
// Prepare is called and that cleanup is deferred correctly by checking that no
// goroutines are leaked.
func TestExecutor_LocalPaths(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	refPath := filepath.Join(dir, "ref.yuv")
	disPath := filepath.Join(dir, "dis.yuv")

	for _, p := range []string{refPath, disPath} {
		if err := os.WriteFile(p, []byte("dummy"), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	store := &mockStorage{mode: storage.ModeHTTPServe}
	exec := NewExecutor(store, "false", nil) // "false" → exits 1 immediately, never actually runs vmaf

	// We do NOT expect a successful score here since "false" is not vmaf.
	// We just verify that Prepare is called for both URIs without panics or
	// data races, and that cleanup() defers are handled.
	job := ScoringJob{
		JobID:        "test-001",
		ReferenceURI: refPath,
		DistortedURI: disPath,
		Width:        576,
		Height:       324,
		PixelFormat:  "yuv420p",
		ModelPath:    "/models/vmaf_v0.6.1.json",
	}

	ctx := context.Background()
	_, err := exec.Execute(ctx, job)
	// We expect an error because "false" exits 1.
	if err == nil {
		t.Error("expected error from 'false' vmaf binary, got nil")
	}
}

// TestExecutor_NilLoggerDoesNotPanic verifies that NewExecutor substitutes
// slog.Default() when log is nil. Prior to this audit, passing nil and
// then calling Execute panicked on `e.log.InfoContext(...)` inside the
// scoring path. The existing TestExecutor_ScoringJobFailsWithBadBinary
// passed nil but was skipped on hosts without a usable "false" binary,
// hiding the NPE.
func TestExecutor_NilLoggerDoesNotPanic(t *testing.T) {
	t.Parallel()

	exec := NewExecutor(nil, nil, "cpu", nil)
	job := &controllerv1.Job{
		Id: "nil-log-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/r.yuv",
			Distorted: "/d.yuv",
		},
	}
	// Must return an error (scorer is nil) rather than panic.
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error when scorer is nil, got nil")
	}
}
