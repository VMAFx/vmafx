// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package vmaftest resolves the vmaf CLI that a Go test runs: the build under
// test, never a host install.
//
// A test that scored with whatever "vmaf" the host had on PATH or under
// /usr/local/bin passed against a stale binary (a different release, another
// default model) while the tree under test was never exercised. Binary
// resolves exactly two sources, in this order: the VMAF_BIN environment
// variable, then the CPU build the Makefile and go-ci.yml link the Go tests
// against (core/build-cpu/tools/vmaf). Neither present is a test failure that
// says how to build the binary; there is no third source.
package vmaftest

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// EnvVar names the explicit override.
const EnvVar = "VMAF_BIN"

// BuildRelPath is the in-tree CPU build's CLI, relative to the repository root.
var BuildRelPath = filepath.Join("core", "build-cpu", "tools", "vmaf")

// Binary returns the vmaf CLI under test and fails the test when there is none.
func Binary(t testing.TB) string {
	t.Helper()
	root, err := repoRoot()
	if err != nil {
		t.Fatalf("vmaftest: %v", err)
	}
	bin, err := Resolve(os.Getenv(EnvVar), root)
	if err != nil {
		t.Fatalf("vmaftest: %v", err)
	}
	return bin
}

// Resolve returns env when it is set, otherwise root's in-tree CPU build, and
// an error when the chosen file is not an executable regular file. It reads
// no other location.
func Resolve(env, root string) (string, error) {
	path, source := env, EnvVar
	if path == "" {
		path, source = filepath.Join(root, BuildRelPath), "the in-tree CPU build"
	}
	// #nosec G703 -- path is VMAF_BIN, the test operator's explicit choice of
	// the binary under test, or the fixed in-tree build path. Naming any file
	// is the purpose of the variable: there is no directory to confine it to,
	// and the value crosses no trust boundary. The call only checks that the
	// file is an executable regular file before a test runs it.
	info, err := os.Stat(path)
	if err != nil {
		return "", fmt.Errorf(
			"vmaf CLI under test not found at %s (%s): build core/build-cpu "+
				"(meson setup core/build-cpu core && ninja -C core/build-cpu) or set %s: %w",
			path, source, EnvVar, err)
	}
	if info.IsDir() || info.Mode().Perm()&0o111 == 0 {
		return "", fmt.Errorf("vmaf CLI under test at %s (%s) is not an executable file", path, source)
	}
	return path, nil
}

// repoRoot walks up from the working directory to the directory holding
// CLAUDE.md. The walk is bounded by the number of path separators.
func repoRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("cannot determine the working directory: %w", err)
	}
	for range len(dir) + 1 {
		if _, statErr := os.Stat(filepath.Join(dir, "CLAUDE.md")); statErr == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return "", errors.New("repository root not found (no CLAUDE.md above the working directory)")
}
