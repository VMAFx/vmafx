// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package cmd

import (
	"errors"
	"fmt"
	"os/exec"
	"testing"
)

// TestExecuteDoesNotAdoptChildExitStatus pins the fix for a defect introduced
// when the two groups' exitCodeError definitions were unified: Execute matched
// the bare exitCoder interface, and *exec.ExitError satisfies it through its
// embedded *os.ProcessState. A wrapped ffmpeg/vmaf failure therefore became the
// CLI's own exit status, colliding with the documented usage (2) and
// out-of-distribution (3) codes.
func TestExecuteDoesNotAdoptChildExitStatus(t *testing.T) {
	t.Parallel()

	runErr := exec.Command("sh", "-c", "exit 42").Run()
	if runErr == nil {
		t.Fatal("expected the child to fail")
	}
	wrapped := fmt.Errorf("ffmpeg failed: %w", runErr)

	// The interface still matches — that is precisely why Execute must not use
	// it as the discriminator.
	var coder exitCoder
	if !errors.As(wrapped, &coder) {
		t.Skip("*exec.ExitError no longer satisfies exitCoder; the trap is gone")
	}

	var codePtr *exitCodeError
	if errors.As(wrapped, &codePtr) {
		t.Errorf("a wrapped *exec.ExitError matched *exitCodeError; the CLI would exit %d", codePtr.ExitCode())
	}
	var codeVal exitCodeError
	if errors.As(wrapped, &codeVal) {
		t.Errorf("a wrapped *exec.ExitError matched exitCodeError; the CLI would exit %d", codeVal.ExitCode())
	}

	// exitCodeOf is the final fallback and must report the generic failure.
	if got := exitCodeOf(wrapped); got != 1 {
		t.Errorf("exitCodeOf(wrapped child error) = %d, want 1", got)
	}
}
