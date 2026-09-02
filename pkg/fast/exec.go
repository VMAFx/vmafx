// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package fast

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"time"
)

// defaultScoreTimeout bounds a single libvmaf scoring subprocess. A hung vmaf
// binary (blocked on a missing model file, a wedged GPU driver, a corrupt
// input pipe) would otherwise pin the whole TPE loop. Override via
// VMAFX_TUNE_SCORE_TIMEOUT, matching pkg/bisect. 0 or negative disables it.
const defaultScoreTimeout = 30 * time.Minute

// scoreTimeout returns the per-call libvmaf timeout, honouring
// VMAFX_TUNE_SCORE_TIMEOUT for operator overrides.
func scoreTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_SCORE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return defaultScoreTimeout
}

// commandRunner executes argv. Tests replace it to keep the unit boundary at
// the process edge; production uses execCommand.
var commandRunner = execCommand

// runCommand dispatches through the swappable runner.
func runCommand(ctx context.Context, argv []string) error {
	return commandRunner(ctx, argv)
}

// execCommand runs argv under a scoreTimeout deadline (unless the caller's
// context already carries an earlier one) and folds the combined output into
// the error on failure.
func execCommand(ctx context.Context, argv []string) error {
	if len(argv) == 0 {
		return fmt.Errorf("fast: empty command")
	}
	if ctx == nil {
		ctx = context.Background()
	}
	cancel := context.CancelFunc(func() {})
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		if to := scoreTimeout(); to > 0 {
			ctx, cancel = context.WithTimeout(ctx, to)
		}
	}
	defer cancel()

	// #nosec G204 -- argv[0] is the operator-configured libvmaf binary and
	// argv[1:] is built by BuildVMAFCommand from validated geometry plus this
	// package's own temp paths. ctx enforces scoreTimeout().
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	// A child that emits no output must still be reaped once the deadline
	// fires, so cap the post-cancel wait.
	cmd.WaitDelay = 2 * time.Second
	out, err := cmd.CombinedOutput()
	if err != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			return fmt.Errorf("fast: %s timed out (%w): %s",
				argv[0], ctxErr, tail(string(out), 300))
		}
		return fmt.Errorf("fast: %s failed: %w: %s", argv[0], err, tail(string(out), 300))
	}
	return nil
}

// tail returns the last n characters of s, for bounded error messages.
func tail(s string, n int) string {
	s = strings.TrimSpace(s)
	if len(s) <= n {
		return s
	}
	return "..." + s[len(s)-n:]
}
