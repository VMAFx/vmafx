// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/runner.go — the subprocess seam.
//
// Every ffmpeg / ffprobe / vmaf / vmaf-perShot invocation in this package goes
// through a Runner. Production callers use ExecRunner; tests inject a stub, the
// same way the Python modules parameterise `runner=subprocess.run`.

package corpus

import (
	"context"
	"os/exec"
	"strings"
)

// RunResult mirrors the subset of subprocess.CompletedProcess the Python
// drivers read.
type RunResult struct {
	Stdout     string
	Stderr     string
	ReturnCode int
}

// Runner executes one argv and returns its captured output.
//
// A Runner must never return an error: a failed spawn is reported as a
// non-zero ReturnCode, matching subprocess.run(check=False) semantics, so the
// corpus row records exit_status != 0 rather than aborting the sweep.
type Runner func(ctx context.Context, argv []string) RunResult

// ExecRunner runs argv as a real subprocess with stdout / stderr captured.
func ExecRunner(ctx context.Context, argv []string) RunResult {
	if len(argv) == 0 {
		return RunResult{ReturnCode: 1, Stderr: "empty argv"}
	}
	// #nosec G204 -- argv[0] is an operator-configured binary path
	// (--ffmpeg-bin / --vmaf-bin / --ffprobe-bin) and argv[1:] is built by
	// the adapters and drivers in this package from validated CLI flags.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	var stdout, stderr strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	rc := 0
	switch {
	case err == nil:
		rc = cmd.ProcessState.ExitCode()
	default:
		var exitErr *exec.ExitError
		if ok := asExitError(err, &exitErr); ok {
			rc = exitErr.ExitCode()
		} else {
			rc = 1
			if stderr.Len() == 0 {
				stderr.WriteString(err.Error())
			}
		}
	}
	if rc < 0 {
		// Killed by a signal (or context cancellation): report a
		// non-zero status the row writer can record.
		rc = 1
	}
	return RunResult{Stdout: stdout.String(), Stderr: stderr.String(), ReturnCode: rc}
}

// asExitError is errors.As specialised to *exec.ExitError, kept as a helper so
// ExecRunner reads as a flat switch.
func asExitError(err error, target **exec.ExitError) bool {
	for err != nil {
		if ee, ok := err.(*exec.ExitError); ok { //nolint:errorlint // unwrap loop below handles wrapping
			*target = ee
			return true
		}
		u, ok := err.(interface{ Unwrap() error })
		if !ok {
			return false
		}
		err = u.Unwrap()
	}
	return false
}

// runnerOrExec returns r, defaulting to ExecRunner when nil.
func runnerOrExec(r Runner) Runner {
	if r == nil {
		return ExecRunner
	}
	return r
}
