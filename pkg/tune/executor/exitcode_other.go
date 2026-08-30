// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

//go:build !unix

package executor

import "os"

// exitCodeOf reports the process's exit status.
//
// The Unix build recovers the signal number so a signal death is reported as
// -signum, matching CPython's subprocess.returncode. Windows has no signals,
// so ExitCode() is already the whole story there.
func exitCodeOf(state *os.ProcessState) int {
	if state == nil {
		return 1
	}
	return state.ExitCode()
}
