// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

//go:build unix

package executor

import (
	"context"
	"os/exec"
	"testing"
)

// TestExecRunnerReportsSignalAsNegativeSignum pins the CPython
// subprocess.returncode convention.
//
// Go's os.ProcessState.ExitCode() collapses every signal death to -1, so a
// tool that segfaults would land -1 in tune_results.jsonl's
// score_exit_status while a Python run of the same plan recorded -11. The
// executor recovers the signal number so the two logs stay comparable — this
// is not cosmetic, it is the difference between "crashed" and "exited 1" for
// anyone triaging a results log.
func TestExecRunnerReportsSignalAsNegativeSignum(t *testing.T) {
	t.Parallel()

	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skipf("no POSIX shell available: %v", err)
	}

	tests := []struct {
		name   string
		script string
		want   int
	}{
		{"clean exit", "exit 0", 0},
		{"ordinary failure", "exit 3", 3},
		{"SIGSEGV", "kill -SEGV $$; sleep 5", -11},
		{"SIGKILL", "kill -KILL $$; sleep 5", -9},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ExecRunner(context.Background(), []string{sh, "-c", tc.script})
			if err != nil {
				t.Fatalf("ExecRunner: %v", err)
			}
			if got.ExitCode != tc.want {
				t.Errorf("ExitCode = %d, want %d", got.ExitCode, tc.want)
			}
		})
	}
}

// TestExecRunnerCapturesBothStreams pins the CommandResult contract the
// encoder-version probe depends on: `ffmpeg -version` prints its configure
// summary on stdout, while encoders print their banners on stderr.
func TestExecRunnerCapturesBothStreams(t *testing.T) {
	t.Parallel()

	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skipf("no POSIX shell available: %v", err)
	}
	got, err := ExecRunner(context.Background(),
		[]string{sh, "-c", "echo to-stdout; echo to-stderr >&2"})
	if err != nil {
		t.Fatalf("ExecRunner: %v", err)
	}
	if got.Stdout != "to-stdout\n" {
		t.Errorf("Stdout = %q, want %q", got.Stdout, "to-stdout\n")
	}
	if got.Stderr != "to-stderr\n" {
		t.Errorf("Stderr = %q, want %q", got.Stderr, "to-stderr\n")
	}
}

// TestExecRunnerRejectsEmptyArgv covers the guard.
func TestExecRunnerRejectsEmptyArgv(t *testing.T) {
	t.Parallel()

	if _, err := ExecRunner(context.Background(), nil); err == nil {
		t.Error("expected an error for an empty argv")
	}
}
