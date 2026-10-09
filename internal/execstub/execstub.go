// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package execstub writes the executable stubs Go tests run in place of a real
// tool (the vmaf CLI, a runner, a helper binary).
//
// A stub written with os.WriteFile while another goroutine of the same test
// binary starts a process can fail to run with "text file busy" (ETXTBSY,
// https://go.dev/issue/22315): the forked child holds a copy of the stub's
// write descriptor until it reaches its own exec, and Linux refuses to execute
// a file that is open for writing. Writing to a temporary name and renaming
// does not help, because the child's descriptor refers to the same inode.
//
// Write holds syscall.ForkLock for reading while it creates, writes and
// closes the file. Every fork of this process takes that lock for writing,
// so no child can be forked while the stub's descriptor is open, and no child
// forked earlier has it.
package execstub

import (
	"fmt"
	"os"
	"syscall"
	"testing"
)

// Write creates path with content, executable by its owner (0o700), and
// fails the test when it cannot.
func Write(t testing.TB, path string, content []byte) {
	t.Helper()
	if err := write(path, content); err != nil {
		t.Fatalf("execstub: %v", err)
	}
}

func write(path string, content []byte) error {
	syscall.ForkLock.RLock()
	defer syscall.ForkLock.RUnlock()
	// The stub is executed by the test, so it must be executable.
	if err := os.WriteFile(path, content, 0o700); err != nil { // #nosec G306 -- an executable stub, owner only
		return fmt.Errorf("write %s: %w", path, err)
	}
	return nil
}
