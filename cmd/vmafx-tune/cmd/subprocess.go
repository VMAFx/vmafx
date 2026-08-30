// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"os/exec"
	"strings"
	"time"
)

// subprocessTimeout bounds a helper subprocess (an ffmpeg decode, an ffprobe,
// an `ffmpeg -filters` capability probe) when the caller's context carries no
// deadline. The encode and score drivers have their own, longer budgets.
const subprocessTimeout = 30 * time.Minute

// runCommand executes argv and returns its stdout, stderr and exit status.
//
// A non-zero exit is NOT an error: it is reported through exitStatus so the
// callers can degrade (a failed decode becomes a NaN score, a failed probe
// becomes zero features) instead of aborting a whole sweep. Only a failure to
// spawn the process at all returns an error.
func runCommand(ctx context.Context, argv []string) (stdout, stderr string, exitStatus int, err error) {
	if len(argv) == 0 {
		return "", "", 1, errors.New("runCommand: empty argv")
	}
	cancel := func() {}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		ctx, cancel = context.WithTimeout(ctx, subprocessTimeout)
	}
	defer cancel()

	// #nosec G204 -- argv[0] is an operator-configured binary path from a CLI
	// flag and argv[1:] is assembled by the typed command builders in
	// pkg/scorecli, pkg/predictor and pkg/pershot. ctx enforces the timeout.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.WaitDelay = 2 * time.Second
	var outBuf, errBuf strings.Builder
	cmd.Stdout = &outBuf
	cmd.Stderr = &errBuf

	runErr := cmd.Run()
	if runErr != nil {
		var exitErr *exec.ExitError
		if errors.As(runErr, &exitErr) {
			return outBuf.String(), errBuf.String(), exitErr.ExitCode(), nil
		}
		return outBuf.String(), errBuf.String(), 1, runErr
	}
	return outBuf.String(), errBuf.String(), 0, nil
}
