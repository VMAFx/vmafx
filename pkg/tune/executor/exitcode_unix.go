// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

//go:build unix

package executor

import (
	"os"
	"syscall"
)

// exitCodeOf reports the process's status the way CPython's
// subprocess.returncode does: the exit status for a normal exit, and
// -signum when the process was killed by a signal.
//
// This matters because the value lands in tune_results.jsonl's
// score_exit_status / encode_exit_status, which downstream tooling reads. Go's
// os.ProcessState.ExitCode() collapses every signal death to a flat -1, so a
// vmaf binary that segfaults would record -1 here and -11 in a Python run of
// the same plan. Recovering the signal number keeps the two logs comparable.
func exitCodeOf(state *os.ProcessState) int {
	if state == nil {
		return 1
	}
	if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() {
		return -int(status.Signal())
	}
	return state.ExitCode()
}
