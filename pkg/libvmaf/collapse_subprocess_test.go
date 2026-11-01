// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/collapse_subprocess_test.go — the helpers the file-pair and pipe
// entry points share after the collapse (#2434), and their failure text.

//go:build cgo

package libvmaf

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestCLIFailure covers the failure text the file-pair and the pipe entry points
// of the subprocess path share.
func TestCLIFailure(t *testing.T) {
	t.Run("cliFailure_genuine_failure", func(t *testing.T) {
		stderr := bytes.NewBufferString("boom")
		err := cliFailure(context.Background(), errors.New("exit status 1"), stderr)
		if !strings.Contains(err.Error(), "vmaf binary failed") || !strings.Contains(err.Error(), "boom") {
			t.Fatalf("cliFailure = %v", err)
		}
		if errors.Is(err, context.Canceled) {
			t.Fatalf("a genuine failure must not report a cancellation: %v", err)
		}
	})
	t.Run("cliFailure_cancelled", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		err := cliFailure(ctx, errors.New("signal: killed"), &bytes.Buffer{})
		if !errors.Is(err, context.Canceled) || !strings.Contains(err.Error(), "subprocess cancelled") {
			t.Fatalf("cliFailure = %v", err)
		}
	})
}

// TestSubprocessSharedHelpers covers what the file-pair and the pipe entry
// points of the subprocess path share besides the failure text: the command and
// the run set-up.
func TestSubprocessSharedHelpers(t *testing.T) {
	t.Run("prepareRun_resolves_and_releases", func(t *testing.T) {
		dir := t.TempDir()
		writeModel(t, dir, "m")
		s := &Scorer{binaryPath: "unused", modelDir: dir}
		run, err := s.prepareRun(nil, "Run", "m") //nolint:staticcheck // a nil context is documented as Background
		if err != nil {
			t.Fatalf("prepareRun: %v", err)
		}
		if run.modelPath != filepath.Join(dir, "m.json") {
			t.Fatalf("modelPath = %q", run.modelPath)
		}
		if _, err := os.Stat(run.out); err != nil {
			t.Fatalf("report file not created: %v", err)
		}
		run.release()
		if _, err := os.Stat(run.out); !errors.Is(err, os.ErrNotExist) {
			t.Fatalf("report file survives release: %v", err)
		}
		if run.ctx.Err() == nil {
			t.Fatal("release must cancel the run context")
		}
	})
	t.Run("prepareRun_model_not_found", func(t *testing.T) {
		s := &Scorer{binaryPath: "unused", modelDir: t.TempDir()}
		if _, err := s.prepareRun(context.Background(), "Run", "absent"); err == nil {
			t.Fatal("prepareRun must fail for an unknown model")
		}
	})
	t.Run("prepareRun_cancelled_names_the_operation", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		s := &Scorer{binaryPath: "unused"}
		_, err := s.prepareRun(ctx, "Score", "")
		if !errors.Is(err, context.Canceled) || !strings.Contains(err.Error(), "before Score") {
			t.Fatalf("prepareRun = %v", err)
		}
	})
	t.Run("newCLICommand_bounds_the_wait", func(t *testing.T) {
		s := &Scorer{binaryPath: "/bin/true"}
		cmd, stderr := s.newCLICommand(context.Background(), []string{"x"})
		if cmd.WaitDelay <= 0 || cmd.Stderr == nil || stderr == nil || cmd.Args[0] != "/bin/true" {
			t.Fatalf("newCLICommand = %+v", cmd)
		}
	})
}
