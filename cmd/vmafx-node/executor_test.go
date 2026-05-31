// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_test.go — unit tests for the job executor covering
// the binary-delegation path (ScoringJob via "false" binary).
//
// ADR-0719: vmafx-node rclone integration.
package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// TestExecutor_ScoringJobFailsWithBadBinary verifies that Execute returns a
// non-nil error when the underlying vmaf binary exits non-zero.
// We construct a Scorer pointing at "false" (a POSIX built-in that always
// exits 1) so the unit test never requires a real vmaf installation.
func TestExecutor_ScoringJobFailsWithBadBinary(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	refPath := filepath.Join(dir, "ref.yuv")
	disPath := filepath.Join(dir, "dis.yuv")

	for _, p := range []string{refPath, disPath} {
		if err := os.WriteFile(p, []byte("dummy"), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	// Build a Scorer that delegates to "false" — always exits 1.
	scorer, err := libvmaf.New("false", "")
	if err != nil {
		// "false" must exist on the PATH in CI; skip rather than fail.
		t.Skipf("could not locate 'false' binary: %v", err)
	}

	exec := NewExecutor(scorer, nil, "cpu", nil)

	job := &controllerv1.Job{
		Id: "test-001",
		Scoring: &controllerv1.ScoringParams{
			Reference: refPath,
			Distorted: disPath,
			Model:     "vmaf_v0.6.1",
		},
	}

	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
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
